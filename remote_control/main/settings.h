#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file settings.h
 * @brief Persistent settings for the handheld remote (NVS).
 */

typedef struct {
    char     wifi_ssid[33];    /* for startup OTA check        */
    char     wifi_pass[65];
    char     ota_url[128];     /* OTA manifest endpoint        */
    uint16_t batt_full_mv;     /* battery calibration          */
    uint16_t batt_empty_mv;
    int16_t  torque_step_x100; /* encoder step (0.01 % units)  */
} remote_settings_t;

esp_err_t settings_load(void);
esp_err_t settings_save(void);
const remote_settings_t *settings_get(void);

/* convenience accessors */
uint16_t settings_batt_full_mv(void);
uint16_t settings_batt_empty_mv(void);

#ifdef __cplusplus
}
#endif
