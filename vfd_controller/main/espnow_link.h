#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "vfd_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espnow_link.h
 * @brief ESP-NOW long-range link (controller side).
 *
 * - WiFi init in APSTA mode, channel fixed, LR protocol enabled on both IFs.
 * - ESP-NOW with per-peer LR rate (WIFI_PHY_MODE_LR).
 * - Sends telemetry @10 Hz, receives commands, feeds control_engine.
 */

/** Initialize wifi (APSTA) + esp-now. Blocks until ready. */
esp_err_t espnow_link_init(void);

/** Set the remote's MAC (learned from first received packet or from NVS). */
esp_err_t espnow_link_set_peer(const uint8_t mac[6]);

/** Send a packet payload (builds header internally). Returns ESP_OK. */
esp_err_t espnow_link_send(vfd_packet_type_t type,
                           const void *payload, size_t len);

/** Latest heartbeat from remote (battery). */
void espnow_link_get_remote_hb(vfd_heartbeat_t *out, bool *valid);

/** RX success ratio 0..100 for link quality display. */
uint8_t espnow_link_quality(void);

#ifdef __cplusplus
}
#endif
