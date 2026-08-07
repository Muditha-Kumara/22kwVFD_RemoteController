#include "upload_task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

#include "settings.h"
#include "wifi_manager.h"
#include "control_engine.h"
#include "data_log.h"

#define TAG "upload"

#define BATCH_LINES   100
#define BATCH_BUF     (BATCH_LINES * 80 + 512)

static bool cutting_active(void)
{
    vfd_machine_state_t st = control_engine_get_state();
    return st == VFD_STATE_STARTING ||
           st == VFD_STATE_CUTTING ||
           st == VFD_STATE_FRESH_CUT ||
           st == VFD_STATE_REVERSING ||
           st == VFD_STATE_STOPPING;
}

static esp_err_t http_event(esp_http_client_event_t *e)
{
    (void)e;
    return ESP_OK;
}

/* POST one batch. Returns true on HTTP 2xx. */
static bool post_batch(const char *body)
{
    const app_settings_t *cfg = settings_get();

    esp_http_client_config_t hc = {
        .url = cfg->upload_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = http_event,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return false;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, strlen(body));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK && status >= 200 && status < 300) {
        return true;
    }
    ESP_LOGW(TAG, "upload failed: %s (status %d)", esp_err_to_name(err), status);
    return false;
}

static void upload_task(void *arg)
{
    static char lines_buf[BATCH_LINES * 80];
    static char body[BATCH_BUF];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        /* Only when connected, idle, and there is data. */
        if (!wifi_manager_sta_connected()) {
            continue;
        }
        if (cutting_active()) {
            continue;
        }
        if (data_log_pending() <= 0) {
            continue;
        }

        int n = data_log_read(lines_buf, sizeof(lines_buf), BATCH_LINES);
        if (n <= 0) {
            continue;
        }

        /* Build JSON: {"device":"controller","rows":[...]} */
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "device", "vfd_controller");
        cJSON *rows = cJSON_AddArrayToObject(j, "rows");
        const char *p = lines_buf;
        for (int i = 0; i < n; i++) {
            cJSON_AddItemToArray(rows, cJSON_CreateString(p));
            p += strlen(p) + 1;
        }
        char *s = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        if (!s) {
            continue;
        }
        strncpy(body, s, sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';
        free(s);

        if (post_batch(body)) {
            data_log_consume(n);
            ESP_LOGI(TAG, "uploaded %d lines", n);
        }
    }
}

void upload_task_start(void)
{
    if (xTaskCreate(upload_task, "upload", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
