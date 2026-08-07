#include "settings.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#define SETTINGS_NS "rsettings"

static remote_settings_t s_settings;
static bool s_loaded;

static void defaults(remote_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->wifi_channel     = 13;
    s->batt_full_mv     = 4200;
    s->batt_empty_mv    = 3300;
    s->torque_step_x100 = 100;   /* 1.00 % per encoder detent */
}

esp_err_t settings_load(void)
{
    defaults(&s_settings);

    nvs_handle_t h;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l;
        l = sizeof(s_settings.wifi_ssid);
        nvs_get_str(h, "wifi_ssid", s_settings.wifi_ssid, &l);
        l = sizeof(s_settings.wifi_pass);
        nvs_get_str(h, "wifi_pass", s_settings.wifi_pass, &l);
        l = sizeof(s_settings.ota_url);
        nvs_get_str(h, "ota_url", s_settings.ota_url, &l);
        nvs_get_u8(h, "chan", &s_settings.wifi_channel);
        nvs_get_u16(h, "batt_full", &s_settings.batt_full_mv);
        nvs_get_u16(h, "batt_empty", &s_settings.batt_empty_mv);
        nvs_get_i16(h, "tstep", &s_settings.torque_step_x100);
        nvs_close(h);
    }
    /* Sanitize channel. */
    if (s_settings.wifi_channel < 1 || s_settings.wifi_channel > 13) {
        s_settings.wifi_channel = 13;
    }
    s_loaded = true;
    return ESP_OK;
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "wifi_ssid", s_settings.wifi_ssid);
    nvs_set_str(h, "wifi_pass", s_settings.wifi_pass);
    nvs_set_str(h, "ota_url", s_settings.ota_url);
    nvs_set_u8(h, "chan", s_settings.wifi_channel);
    nvs_set_u16(h, "batt_full", s_settings.batt_full_mv);
    nvs_set_u16(h, "batt_empty", s_settings.batt_empty_mv);
    nvs_set_i16(h, "tstep", s_settings.torque_step_x100);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

const remote_settings_t *settings_get(void)
{
    if (!s_loaded) {
        settings_load();
    }
    return &s_settings;
}

esp_err_t settings_set_wifi_channel(uint8_t channel)
{
    if (channel < 1 || channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    s_settings.wifi_channel = channel;
    return settings_save();
}

uint16_t settings_batt_full_mv(void)
{
    return settings_get()->batt_full_mv;
}

uint16_t settings_batt_empty_mv(void)
{
    return settings_get()->batt_empty_mv;
}
