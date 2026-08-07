#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "vfd_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espnow_link.h
 * @brief ESP-NOW long-range link (remote control side).
 *
 * - WiFi STA-only init, fixed channel, LR protocol enabled.
 * - Receives telemetry @10 Hz from controller, feeds UI.
 * - Sends commands / torque setpoints / heartbeat to controller.
 * - Tracks link quality; UI must warn when link degrades.
 */

/** Initialize wifi (STA) + esp-now. */
esp_err_t espnow_link_init(void);

/** Set the controller's MAC (learned from first RX or from NVS). */
esp_err_t espnow_link_set_peer(const uint8_t mac[6]);

/** Send a packet payload to controller. Returns ESP_OK. */
esp_err_t espnow_link_send(vfd_packet_type_t type,
                           const void *payload, size_t len);

/** Latest telemetry from controller (copied under lock). */
void espnow_link_get_telemetry(vfd_telemetry_t *out, bool *valid);

/** Milliseconds since last valid telemetry from controller. */
uint32_t espnow_link_age_ms(void);

/** RX success ratio 0..100. */
uint8_t espnow_link_quality(void);

/** Register a callback invoked on each valid telemetry packet. */
typedef void (*espnow_telemetry_cb_t)(const vfd_telemetry_t *t);
void espnow_link_on_telemetry(espnow_telemetry_cb_t cb);

#ifdef __cplusplus
}
#endif
