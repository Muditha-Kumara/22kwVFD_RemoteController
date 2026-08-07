#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ota_task.h
 * @brief Startup OTA check for the remote (same manifest protocol as the
 *        controller: magic number + version gating + CRC validation).
 *
 * The remote connects to the configured wifi at startup, checks
 * settings.ota_url, updates if a newer compatible firmware exists, then
 * disconnects wifi and runs purely on ESP-NOW LR.
 */

/** Start the OTA check task (runs once at startup, then sleeps). */
void ota_task_start(void);

#ifdef __cplusplus
}
#endif
