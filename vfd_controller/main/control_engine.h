#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "vfd.h"
#include "vfd_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file control_engine.h
 * @brief The cutting control state machine.
 *
 * Responsibilities:
 *  - Own the machine state (vfd_machine_state_t) and all transitions.
 *  - Poll both VFDs at ~10 Hz and publish telemetry.
 *  - Implement safe start (soft ramp), stop, e-stop.
 *  - Implement the fresh-cut stuck-wire routine: if start torque exceeds a
 *    limit, reverse + vibrate the wire both directions to free it, then
 *    slowly ramp back up.
 *  - Enforce safety: comm loss > VFD_LINK_TIMEOUT_MS => safe stop.
 *
 * Torque-control philosophy:
 *  Both drives run in torque-control mode. The travel motor applies a set
 *  torque; when cutting resistance (wire tension) rises, the motor naturally
 *  slows — no extra sensor needed. The engine only sets torque setpoints and
 *  supervises limits.
 */

typedef struct {
    vfd_dev_t *main_vfd;      /* ZUKED D720, 22 kW cutting motor */
    vfd_dev_t *travel_vfd;    /* SF100, travel motor             */
} control_engine_cfg_t;

/** Tunable limits (persisted in NVS, editable via web UI). */
typedef struct {
    int16_t  main_torque_max_x100;   /* hard ceiling main torque      */
    int16_t  travel_torque_max_x100; /* hard ceiling travel torque    */
    int16_t  start_torque_limit_x100;/* fresh-cut stuck detection     */
    uint16_t ramp_time_ms;           /* soft-start ramp duration      */
    uint16_t stuck_reverse_ms;       /* reverse time per vibration    */
    uint16_t stuck_fwd_ms;           /* forward time per vibration    */
    uint8_t  stuck_vibrate_cycles;   /* number of back/forth cycles   */
    uint16_t stuck_torque_ok_x100;   /* resume when torque below this */
} control_limits_t;

/** Initialize engine, probe + configure both VFDs. */
esp_err_t control_engine_init(const control_engine_cfg_t *cfg);

/** Start the engine task (polls VFDs, runs state machine). */
esp_err_t control_engine_start(void);

/* ---- Commands (called from espnow/web handlers) ---- */
esp_err_t control_engine_cmd(vfd_cmd_code_t cmd, uint32_t arg);

/** Set torque setpoints from remote encoders (0.01 % units). */
void control_engine_set_torque(int16_t main_x100, int16_t travel_x100);

/** Notify the engine that a valid remote packet arrived (keeps link alive). */
void control_engine_link_alive(void);

/* ---- Telemetry / status ---- */
void control_engine_get_telemetry(vfd_telemetry_t *out);
vfd_machine_state_t control_engine_get_state(void);

/** Load/save tunable limits to NVS. */
void control_engine_get_limits(control_limits_t *out);
esp_err_t control_engine_set_limits(const control_limits_t *lim);

#ifdef __cplusplus
}
#endif
