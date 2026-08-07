#include "ota_task.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_rom_crc.h"
#include "cJSON.h"

#include "settings.h"
#include "wifi_manager.h"
#include "control_engine.h"
#include "vfd_protocol.h"

#define TAG "ota"

/** Firmware version. Bump for every release; OTA updates only forward. */
#define APP_FW_VERSION          1

#define BOOT_CHECK_WAIT_S       30      /* grace before marking OTA good   */
#define OTA_CHECK_PERIOD_MS     (60 * 60 * 1000)   /* re-check hourly      */
#define CHUNK                   1024

static bool cutting_active(void)
{
    vfd_machine_state_t st = control_engine_get_state();
    return st == VFD_STATE_STARTING ||
           st == VFD_STATE_CUTTING ||
           st == VFD_STATE_FRESH_CUT ||
           st == VFD_STATE_REVERSING ||
           st == VFD_STATE_STOPPING;
}

/* GET a URL into a malloc'd NUL-terminated buffer. Caller frees. */
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

/* Stream the firmware image to the target partition, verifying CRC32. */
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
        ESP_LOGE(TAG, "bad image response: status=%d len=%d", status, total);
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
        ESP_LOGE(TAG, "CRC mismatch: got %08lx want %08lx",
                 (unsigned long)crc, (unsigned long)expect_crc);
        esp_ota_abort(ota);
        return ESP_ERR_INVALID_CRC;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ota_set_boot_partition(update);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA staged: %lu bytes, crc %08lx",
                 (unsigned long)written, (unsigned long)crc);
    }
    return err;
}

static void ota_task(void *arg)
{
    /* Let the system settle + validate current boot before any update. */
    vTaskDelay(pdMS_TO_TICKS(BOOT_CHECK_WAIT_S * 1000));

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "first boot after OTA: marked valid");
        }
    }

    for (;;) {
        if (wifi_manager_sta_connected() && !cutting_active()) {
            const app_settings_t *cfg = settings_get();
            if (cfg->ota_url[0]) {
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
                        const char *url = cJSON_IsString(url_j)
                                              ? url_j->valuestring : NULL;

                        if (magic != VFD_PROTO_MAGIC) {
                            ESP_LOGW(TAG, "manifest magic %08lx rejected",
                                     (unsigned long)magic);
                        } else if (ver <= APP_FW_VERSION) {
                            ESP_LOGI(TAG, "up to date (v%lu)",
                                     (unsigned long)APP_FW_VERSION);
                        } else if (!url) {
                            ESP_LOGW(TAG, "manifest has no url");
                        } else if (cutting_active()) {
                            ESP_LOGI(TAG, "deferring OTA: cutting active");
                        } else {
                            ESP_LOGI(TAG, "OTA v%lu -> v%lu",
                                     (unsigned long)APP_FW_VERSION,
                                     (unsigned long)ver);
                            if (flash_image(url, size, crc) == ESP_OK) {
                                ESP_LOGI(TAG, "rebooting into new firmware");
                                vTaskDelay(pdMS_TO_TICKS(1000));
                                esp_restart();
                            }
                        }
                        cJSON_Delete(j);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_MS));
    }
}

void ota_task_start(void)
{
    if (xTaskCreate(ota_task, "ota", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
