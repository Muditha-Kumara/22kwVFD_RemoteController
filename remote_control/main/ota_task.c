#include "ota_task.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "cJSON.h"

#include "settings.h"
#include "display.h"
#include "board.h"
#include "vfd_protocol.h"

#define TAG "ota_r"

/** Firmware version. Bump for every release. */
#define APP_FW_VERSION          1

#define OTA_CONNECT_TIMEOUT_MS  15000
#define CHUNK                   1024

#define WIFI_DONE_BIT   BIT0
#define WIFI_FAIL_BIT   BIT1

static EventGroupHandle_t s_eg;

static void wifi_event(void *arg, esp_event_base_t base,
                       int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_eg, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_eg, WIFI_DONE_BIT);
    }
}

static char *http_get(const char *url, int *out_len)
{
    esp_http_client_config_t hc = { .url = url, .timeout_ms = 15000 };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return NULL;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    int total = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (total <= 0 || status != 200) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }
    char *buf = malloc(total + 1);
    if (!buf) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }
    int got = esp_http_client_read(c, buf, total);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (got != total) {
        free(buf);
        return NULL;
    }
    buf[total] = '\0';
    if (out_len) {
        *out_len = total;
    }
    return buf;
}

static esp_err_t flash_image(const char *url, uint32_t expect_size,
                             uint32_t expect_crc)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_http_client_config_t hc = { .url = url, .timeout_ms = 30000 };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }
    int total = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200 || total <= 0 ||
        (expect_size && (uint32_t)total != expect_size)) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(update,
                        expect_size ? expect_size : OTA_WITH_SEQUENTIAL_WRITES,
                        &ota);
    if (err != ESP_OK) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return err;
    }

    static uint8_t chunk[CHUNK];
    uint32_t crc = 0;
    uint32_t written = 0;
    while (written < (uint32_t)total) {
        int want = (total - written) > CHUNK ? CHUNK : (total - written);
        int r = esp_http_client_read(c, (char *)chunk, want);
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        crc = esp_rom_crc32_le(crc, chunk, r);
        err = esp_ota_write(ota, chunk, r);
        if (err != ESP_OK) {
            break;
        }
        written += (uint32_t)r;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || written != (uint32_t)total) {
        esp_ota_abort(ota);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    if (expect_crc && crc != expect_crc) {
        esp_ota_abort(ota);
        return ESP_ERR_INVALID_CRC;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        return err;
    }
    return esp_ota_set_boot_partition(update);
}

static void ota_task(void *arg)
{
    const remote_settings_t *cfg = settings_get();

    /* Validate current boot first. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }

    if (cfg->wifi_ssid[0] == '\0' || cfg->ota_url[0] == '\0') {
        ESP_LOGI(TAG, "no wifi/ota configured; skipping OTA check");
        vTaskDelete(NULL);
        return;
    }

    display_status("OTA CHECK...");

    /* Connect to wifi (wifi was initialized by espnow_link in STA mode). */
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event, NULL));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_eg, WIFI_DONE_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(OTA_CONNECT_TIMEOUT_MS));
    if (!(bits & WIFI_DONE_BIT)) {
        ESP_LOGW(TAG, "wifi connect failed; skipping OTA");
        display_status("OTA: NO WIFI");
        esp_wifi_disconnect();
        vTaskDelete(NULL);
        return;
    }

    int len = 0;
    char *manifest = http_get(cfg->ota_url, &len);
    if (manifest) {
        cJSON *j = cJSON_Parse(manifest);
        free(manifest);
        if (j) {
            uint32_t magic = (uint32_t)cJSON_GetNumberValue(
                cJSON_GetObjectItem(j, "magic"));
            uint32_t ver = (uint32_t)cJSON_GetNumberValue(
                cJSON_GetObjectItem(j, "version"));
            uint32_t size = (uint32_t)cJSON_GetNumberValue(
                cJSON_GetObjectItem(j, "size"));
            uint32_t crc = (uint32_t)cJSON_GetNumberValue(
                cJSON_GetObjectItem(j, "crc32"));
            cJSON *url_j = cJSON_GetObjectItem(j, "url");
            const char *url = cJSON_IsString(url_j) ? url_j->valuestring : NULL;

            if (magic != VFD_PROTO_MAGIC) {
                display_status("OTA: BAD MAGIC");
            } else if (ver <= APP_FW_VERSION) {
                display_status("OTA: UP TO DATE");
            } else if (url && flash_image(url, size, crc) == ESP_OK) {
                display_status("OTA: REBOOTING");
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            } else {
                display_status("OTA: FAILED");
            }
            cJSON_Delete(j);
        }
    } else {
        display_status("OTA: NO SERVER");
    }

    /* Done: disconnect wifi, restore ESP-NOW channel, LR continues. */
    esp_wifi_disconnect();
    esp_wifi_set_channel(BOARD_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "OTA check done");
    vTaskDelete(NULL);
}

void ota_task_start(void)
{
    if (xTaskCreate(ota_task, "ota_r", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
