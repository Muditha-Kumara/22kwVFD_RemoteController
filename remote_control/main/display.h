#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "vfd_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file display.h
 * @brief ST7789 TFT + LVGL UI for the handheld remote.
 *
 * Shows: machine state, main/travel torque bars, freq, current, wire
 * tension, link quality, battery, alarms. All updates go through
 * display_update() which is safe to call from the telemetry callback.
 */

/** Initialize SPI, panel, LVGL and build the dashboard. */
esp_err_t display_init(void);

/** Refresh the dashboard with the latest telemetry. */
void display_update(const vfd_telemetry_t *t);

/** Show a transient status message (e.g. "LINK LOST", "OTA..."). */
void display_status(const char *msg);

/** Set the torque setpoint indicators (from encoders). */
void display_set_torque_set(int16_t main_x100, int16_t travel_x100);

/** Battery indicator update. */
void display_set_battery(uint16_t mv, uint8_t pct);

#ifdef __cplusplus
}
#endif
