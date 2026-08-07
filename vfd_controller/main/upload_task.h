#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file upload_task.h
 * @brief Background uploader.
 *
 * When the STA wifi is connected and no cutting is in progress, the task
 * POSTs batches of stored telemetry (from data_log) to settings.upload_url
 * as a JSON array of CSV rows, then consumes the uploaded lines.
 *
 * It never uploads while cutting (bandwidth + CPU must stay with control).
 */

/** Start the uploader task. */
void upload_task_start(void);

#ifdef __cplusplus
}
#endif
