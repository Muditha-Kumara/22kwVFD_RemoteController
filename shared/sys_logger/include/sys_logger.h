#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file sys_logger.h
 * @brief Unified logger for both devices.
 *
 * Features:
 *  - Hooks esp_log (esp_log_set_vprintf) so ALL esp_log output is captured
 *    into a RAM ring buffer (in addition to UART).
 *  - Samples free heap / minimum-ever heap once per second as log records.
 *  - Provides a batch-read API so an uploader (HTTP / ESP-NOW piggyback)
 *    can drain buffered logs when the link is free.
 *  - Ring buffer is overwrite-oldest: newest logs always kept.
 *
 * RAM budget: default ring 16 KB (configurable).
 */

/** Initialize the logger. Installs the esp_log vprintf hook and starts the
 *  heap-monitor timer. Call once after NVS init. Safe to call before wifi. */
esp_err_t sys_logger_init(void);

/**
 * @brief Drain up to max_lines buffered log lines into `out`.
 *        Each line is null-terminated. Returns number of lines copied.
 *        Lines are REMOVED from the buffer once read (upload-and-forget).
 * @param out caller buffer of out_cap bytes
 * @param max_lines stop after this many lines even if space remains
 */
int sys_logger_drain(char *out, size_t out_cap, int max_lines);

/** Number of log lines currently buffered (approximate). */
int sys_logger_pending_count(void);

/** Snapshot of memory stats (also logged once per second internally). */
typedef struct {
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint32_t free_spiram;   /* 0 if no PSRAM */
} sys_mem_stat_t;

void sys_logger_get_mem_stat(sys_mem_stat_t *st);

/** Tag used by the logger's own records. */
#define SYS_LOG_TAG "syslog"

#ifdef __cplusplus
}
#endif
