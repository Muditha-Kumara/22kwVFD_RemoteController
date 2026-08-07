#include "wifi_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "settings.h"
#include "control_engine.h"

#define TAG "wifi_mgr"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5
#define RETRY_DELAY_MS      5000

static EventGroupHandle_t s_eg;
static bool s_connected;
static char s_ip_str[16];
static int  s_retry_count;

bool wifi_manager_sta_connected(void)
{
    return s_connected;
}

void wifi_manager_get_ip(char out[16])
{
    strncpy(out, s_ip_str, 15);
    out[15] = '\0';
}

/* Is a cutting operation in progress? (must not disturb wifi) */
static bool cutting_active(void)
{
    vfd_machine_state_t st = control_engine_get_state();
    return st == VFD_STATE_STARTING ||
           st == VFD_STATE_CUTTING ||
           st == VFD_STATE_FRESH_CUT ||
           st == VFD_STATE_REVERSING ||
           st == VFD_STATE_STOPPING;
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip_str[0] = '\0';
        xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "STA disconnected");
        /* Reconnect only if not cutting. The retry loop task handles it. */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip_str, sizeof(s_ip_str));
        s_connected = true;
        s_retry_count = 0;
        xEventGroupSetBits(s_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
    }
}

static void wifi_task(void *arg)
{
    const app_settings_t *cfg = settings_get();

    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGI(TAG, "no STA ssid configured; AP-only mode");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, cfg->wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, cfg->wifi_pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    /* Initial connect attempt. */
    esp_wifi_connect();

    /* Supervision loop: reconnect when disconnected and not cutting. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));

        if (s_connected) {
            continue;
        }
        if (cutting_active()) {
            ESP_LOGI(TAG, "cutting active; deferring wifi reconnect");
            continue;
        }
        if (s_retry_count >= MAX_RETRY) {
            /* Back off longer; keep trying periodically. */
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS * 4));
            s_retry_count = 0;
        }
        s_retry_count++;
        ESP_LOGI(TAG, "reconnect attempt %d", s_retry_count);
        esp_wifi_connect();
    }
}

void wifi_manager_start(void)
{
    s_eg = xEventGroupCreate();
    if (xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
