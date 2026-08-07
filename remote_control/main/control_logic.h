#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file control_logic.h
 * @brief Operator-side logic for the handheld remote.
 *
 * Responsibilities:
 *  - Map encoder deltas to torque setpoints, send to controller @10 Hz.
 *  - Map buttons to machine commands:
 *      BTN_TENSION      -> VFD_CMD_TENSION_TEST (travel-only tighten)
 *      BTN_START        -> VFD_CMD_START_CUT (safe ramp)
 *      BTN_FRESH        -> VFD_CMD_FRESH_CUT
 *      BTN_FRESH long   -> VFD_CMD_ESTOP
 *      BTN_START long   -> VFD_CMD_STOP
 *  - Send heartbeat @1 Hz with battery level.
 *  - Watch link age: warn on display when telemetry stale (>500 ms).
 */

/** Start the operator logic task. */
esp_err_t control_logic_start(void);

#ifdef __cplusplus
}
#endif
