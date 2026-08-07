#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file web_server.h
 * @brief HTTP dashboard served on both Soft-AP and STA interfaces.
 *
 * Endpoints:
 *  GET  /              -> dashboard HTML (embedded)
 *  GET  /api/status    -> live telemetry JSON
 *  GET  /api/settings  -> current settings JSON
 *  POST /api/settings  -> save settings JSON
 *  POST /api/cmd       -> machine command {"cmd":N,"arg":N}
 *  GET  /api/logs      -> drained sys_logger lines (JSON array)
 *  WS   /ws            -> live telemetry ~5 Hz (websocket)
 */

/** Start the HTTP server. Call after wifi is up (works on AP too). */
void web_server_start(void);

#ifdef __cplusplus
}
#endif
