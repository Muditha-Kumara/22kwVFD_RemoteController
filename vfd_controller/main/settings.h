#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file settings.h
 * @brief Persistent device settings stored in NVS.
 *        All user-configurable values live here.
 */

typedef struct {
    /* WiFi STA */
    char     wifi_ssid[33];
    char     wifi_pass[65];

    /* Single WiFi channel for AP + STA + ESP-NOW (1..13). */
    uint8_t  wifi_channel;

    /* Data endpoints */
    char     udp_ip[16];        /* live UDP broadcast target   */
    uint16_t udp_port;
    char     upload_url[128];   /* HTTP POST for stored logs   */
    char     ota_url[128];      /* OTA manifest endpoint       */

    /* Modbus */
    uint32_t rs485_baud;
    uint8_t  vfd_addr_main;
    uint8_t  vfd_addr_travel;

    /* Battery (remote) */
    uint16_t batt_full_mv;
    uint16_t batt_empty_mv;
} app_settings_t;

/** Load settings from NVS (fills defaults for missing keys). */
esp_err_t settings_load(app_settings_t *out);

/** Save settings to NVS. */
esp_err_t settings_save(const app_settings_t *s);

/** Get pointer to the cached global settings (loaded at boot). */
const app_settings_t *settings_get(void);

/** Reload cached settings from NVS. */
esp_err_t settings_reload(void);

#ifdef __cplusplus
}
#endif
