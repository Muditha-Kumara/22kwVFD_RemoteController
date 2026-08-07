#pragma once

#include "vfd_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file data_log.h
 * @brief Persist every telemetry sample to the 'logstore' FAT partition.
 *
 * Samples are appended to a rolling CSV file. The upload_task later reads
 * and ships them to the server when the network is free.
 */

/** Mount the FAT partition and open the log. */
void data_log_init(void);

/** Append one telemetry sample. Safe to call at 10 Hz. */
void data_log_write(const vfd_telemetry_t *t);

/**
 * Read up to max_lines lines into buf (NUL-separated). Returns count.
 * Used by upload_task. Does not delete the lines.
 */
int data_log_read(char *buf, int buf_len, int max_lines);

/**
 * Remove the oldest `lines` lines after a successful upload.
 */
void data_log_consume(int lines);

/** Number of pending (un-uploaded) lines. */
int data_log_pending(void);

#ifdef __cplusplus
}
#endif
