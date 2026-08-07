#include "settings.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#define SETTINGS_NS   "settings"

static app_settings_t s_settings;
static bool s_loaded;

static void settings_defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strcpy(s->wifi_ssid, "");
    strcpy(s->wifi_pass, "");
    strcpy(s->udp_ip, "192.168.1.101");
    s->udp_port = 5005;
    strcpy(s->upload_url, "http://192.168.1.101:8080/upload");
    strcpy(s->ota_url, "http://192.168.1.101:8080/manifest.json");
    s->rs485_baud     = 9600;
    s->vfd_addr_main   = 1;
    s->vfd_addr_travel = 2;
    s->batt_full_mv    = 4200;
    s->batt_empty_mv   = 3300;
}

/* Helper macros to reduce boilerplate. */
#define LOAD_STR(key, field) do { \
    size_t l = sizeof(s->field); \
    if (nvs_get_str(h, key, s->field, &l) != ESP_OK) { /* keep default */ } \
} while (0)

#define LOAD_U32(key, field) do { \
    uint32_t v; \
    if (nvs_get_u32(h, key, &v) == ESP_OK) s->field = v; \
} while (0)

#define LOAD_U16(key, field) do { \
    uint16_t v; \
    if (nvs_get_u16(h, key, &v) == ESP_OK) s->field = v; \
} while (0)

#define LOAD_U8(key, field) do { \
    uint8_t v; \
    if (nvs_get_u8(h, key, &v) == ESP_OK) s->field = v; \
} while (0)

esp_err_t settings_load(app_settings_t *out)
{
    app_settings_t *s = &s_settings;
    settings_defaults(s);

    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        LOAD_STR("wifi_ssid", wifi_ssid);
        LOAD_STR("wifi_pass", wifi_pass);
        LOAD_STR("udp_ip", udp_ip);
        LOAD_U16("udp_port", udp_port);
        LOAD_STR("upload_url", upload_url);
        LOAD_STR("ota_url", ota_url);
        LOAD_U32("rs485_baud", rs485_baud);
        LOAD_U8("vfd_addr_m", vfd_addr_main);
        LOAD_U8("vfd_addr_t", vfd_addr_travel);
        LOAD_U16("batt_full", batt_full_mv);
        LOAD_U16("batt_empty", batt_empty_mv);
        nvs_close(h);
    }

    s_loaded = true;
    if (out) {
        *out = s_settings;
    }
    return ESP_OK;
}

esp_err_t settings_save(const app_settings_t *s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "wifi_ssid", s->wifi_ssid);
    nvs_set_str(h, "wifi_pass", s->wifi_pass);
    nvs_set_str(h, "udp_ip", s->udp_ip);
    nvs_set_u16(h, "udp_port", s->udp_port);
    nvs_set_str(h, "upload_url", s->upload_url);
    nvs_set_str(h, "ota_url", s->ota_url);
    nvs_set_u32(h, "rs485_baud", s->rs485_baud);
    nvs_set_u8(h, "vfd_addr_m", s->vfd_addr_main);
    nvs_set_u8(h, "vfd_addr_t", s->vfd_addr_travel);
    nvs_set_u16(h, "batt_full", s->batt_full_mv);
    nvs_set_u16(h, "batt_empty", s->batt_empty_mv);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_settings = *s;
    }
    return err;
}

const app_settings_t *settings_get(void)
{
    if (!s_loaded) {
        settings_load(NULL);
    }
    return &s_settings;
}

esp_err_t settings_reload(void)
{
    return settings_load(NULL);
}
