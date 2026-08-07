#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file wifi_manager.h
 * @brief STA connection policy for the controller.
 *
 * Policy (per requirements):
 *  - Try to connect at startup.
 *  - Reconnect only when NO cutting process is ongoing
 *    (state not STARTING/CUTTING/FRESH_CUT/REVERSING/STOPPING).
 *  - If connected during cutting, stay connected (UDP broadcast continues).
 *  - Never disconnect or reconfigure wifi mid-cut.
 *
 * The Soft-AP (dashboard) is always up regardless of STA state.
 */

/** Start the manager task. Uses settings_get() for SSID/pass. */
void wifi_manager_start(void);

/** True when STA has an IP address. */
bool wifi_manager_sta_connected(void);

/** Get current STA IP as string ("" if not connected). */
void wifi_manager_get_ip(char out[16]);

#ifdef __cplusplus
}
#endif
