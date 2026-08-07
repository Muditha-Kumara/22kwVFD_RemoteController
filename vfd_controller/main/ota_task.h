#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ota_task.h
 * @brief Startup OTA update check.
 *
 * At boot (and periodically), fetches settings.ota_url which must return a
 * JSON manifest:
 *   { "magic": <VFD_PROTO_MAGIC>, "version": <fw_version>,
 *     "url": "<bin url>", "size": <bytes>, "crc32": <crc> }
 *
 * Rules:
 *  - Refuse manifest whose magic != VFD_PROTO_MAGIC (wrong device/protocol).
 *  - Update only if manifest version > running version.
 *  - Never update while cutting.
 *  - Validate image CRC before marking the new partition valid.
 */

/** Start the OTA checker task. */
void ota_task_start(void);

#ifdef __cplusplus
}
#endif
