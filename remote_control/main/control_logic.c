#include "control_logic.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "espnow_link.h"
#include "input.h"
#include "display.h"
#include "settings.h"
#include "vfd_protocol.h"

#define TAG "ctrl_logic"

#define TORQUE_MAX_X100   15000   /* 150.00 % ceiling */
#define TORQUE_MIN_X100   0

static struct {
    int16_t main_set_x100;
    int16_t travel_set_x100;
    int64_t last_warn_us;
} s_logic;

/* ------------------------------------------------------------------ */
static int16_t clamp_torque(int32_t v)
{
    if (v < TORQUE_MIN_X100) return TORQUE_MIN_X100;
    if (v > TORQUE_MAX_X100) return TORQUE_MAX_X100;
    return (int16_t)v;
}

static void send_torque_set(void)
{
    vfd_torque_set_t ts = {
        .main_torque_set   = s_logic.main_set_x100,
        .travel_torque_set = s_logic.travel_set_x100,
        .flags = 0x03,   /* both enabled */
    };
    espnow_link_send(VFD_PKT_TORQUE_SET, &ts, sizeof(ts));
    display_set_torque_set(s_logic.main_set_x100, s_logic.travel_set_x100);
}

static void send_cmd(vfd_cmd_code_t cmd)
{
    vfd_cmd_t c = { .cmd = (uint8_t)cmd, .arg0 = 0, .arg1 = 0, .arg2 = 0 };
    esp_err_t err = espnow_link_send(VFD_PKT_CMD, &c, sizeof(c));
    ESP_LOGI(TAG, "cmd %d -> %s", cmd,
             err == ESP_OK ? "sent" : "NO PEER");
    if (err != ESP_OK) {
        display_status("NOT PAIRED!");
    }
}

static void send_heartbeat(void)
{
    vfd_heartbeat_t hb = {
        .battery_mv  = input_battery_mv(),
        .battery_pct = input_battery_pct(),
        .flags       = 0,
    };
    espnow_link_send(VFD_PKT_HEARTBEAT, &hb, sizeof(hb));
    display_set_battery(hb.battery_mv, hb.battery_pct);
}

/* ------------------------------------------------------------------ */
static void on_button(const button_msg_t *m)
{
    switch (m->id) {
    case BTN_TENSION:
        if (m->event == BTN_EV_PRESS) {
            display_status("TENSION TEST...");
            send_cmd(VFD_CMD_TENSION_TEST);
        }
        break;
    case BTN_START:
        if (m->event == BTN_EV_PRESS) {
            display_status("STARTING...");
            send_cmd(VFD_CMD_START_CUT);
        } else if (m->event == BTN_EV_LONG_PRESS) {
            display_status("STOPPING...");
            send_cmd(VFD_CMD_STOP);
        }
        break;
    case BTN_FRESH:
        if (m->event == BTN_EV_PRESS) {
            display_status("FRESH CUT...");
            send_cmd(VFD_CMD_FRESH_CUT);
        } else if (m->event == BTN_EV_LONG_PRESS) {
            /* dedicated E-STOP on long press */
            display_status("!! E-STOP !!");
            send_cmd(VFD_CMD_ESTOP);
        }
        break;
    case BTN_ENC_MAIN:
        if (m->event == BTN_EV_PRESS) {
            s_logic.main_set_x100 = 0;   /* press = zero torque */
            send_torque_set();
        }
        break;
    case BTN_ENC_TRAVEL:
        if (m->event == BTN_EV_PRESS) {
            s_logic.travel_set_x100 = 0;
            send_torque_set();
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
static void on_telemetry(const vfd_telemetry_t *t)
{
    display_update(t);
}

static void logic_task(void *arg)
{
    int64_t last_torque_us = 0;
    int64_t last_hb_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();

        /* Encoders -> torque setpoints (send at up to 10 Hz). */
        int32_t dm = input_encoder_delta(false);
        int32_t dt = input_encoder_delta(true);
        if (dm != 0 || dt != 0) {
            int16_t step = settings_get()->torque_step_x100;
            s_logic.main_set_x100 =
                clamp_torque(s_logic.main_set_x100 + dm * step);
            s_logic.travel_set_x100 =
                clamp_torque(s_logic.travel_set_x100 + dt * step);
            if (now - last_torque_us >= 100 * 1000) {
                last_torque_us = now;
                send_torque_set();
            }
        }

        /* Heartbeat + battery @1 Hz. */
        if (now - last_hb_us >= 1000 * 1000) {
            last_hb_us = now;
            send_heartbeat();
        }

        /* Link-loss warning. */
        uint32_t age = espnow_link_age_ms();
        if (age > VFD_LINK_TIMEOUT_MS &&
            now - s_logic.last_warn_us > 1000 * 1000) {
            s_logic.last_warn_us = now;
            display_status("LINK LOST!");
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ------------------------------------------------------------------ */
esp_err_t control_logic_start(void)
{
    memset(&s_logic, 0, sizeof(s_logic));
    input_on_button(on_button);
    espnow_link_on_telemetry(on_telemetry);

    if (xTaskCreate(logic_task, "ctrl_logic", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "control logic started");
    return ESP_OK;
}
