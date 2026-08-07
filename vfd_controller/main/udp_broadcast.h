#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file udp_broadcast.h
 * @brief Broadcast every telemetry sample as a UDP datagram to the IP/port
 *        configured in settings, whenever the STA wifi is connected.
 *
 * Packet format: the raw vfd_pkt (header + vfd_telemetry_t payload) built by
 * vfd_protocol, so receivers use the same parser as the esp-now stream.
 */

/** Start the broadcast task (hooks into control_engine telemetry via espnow
 *  link's 10 Hz tick — implemented as its own lightweight task). */
void udp_broadcast_start(void);

#ifdef __cplusplus
}
#endif
