#include "control_engine.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "engine"

#define ENGINE_TASK_STACK   4096
#define ENGINE_TASK_PRIO    8          /* high: safety-critical polling */
#define ENGINE_POLL_MS      50         /* 20 Hz internal poll */

#define NVS_NAMESPACE       "engine"
#define NVS_KEY_LIMITS      "limits"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static struct {
    vfd_dev_t *main_vfd;
    vfd_dev_t *travel_vfd;

    vfd_machine_state_t state;
    vfd_event_code_t    alarm;

    /* torque setpoints (0.01 %) */
    int16_t main_torque_set;
    int16_t travel_torque_set;

    /* link watchdog */
    int64_t last_link_us;
    bool    link_established;

    /* telemetry snapshot */
    vfd_status_t main_st;
    vfd_status_t travel_st;

    SemaphoreHandle_t lock;
    QueueHandle_t     cmd_q;
    TaskHandle_t      task;
    bool              inited;
} s_eng;

static control_limits_t s_lim;

/* ------------------------------------------------------------------ */
static void set_state(vfd_machine_state_t ns)
{
    if (s_eng.state != ns) {
        ESP_LOGI(TAG, "state %d -> %d", s_eng.state, ns);
        s_eng.state = ns;
    }
}

static void raise_alarm(vfd_event_code_t ev)
{
    s_eng.alarm = ev;
    ESP_LOGW(TAG, "alarm %d", ev);
}

/* ------------------------------------------------------------------ */
/* Default limits                                                      */
/* ------------------------------------------------------------------ */
static void limits_default(control_limits_t *l)
{
    l->main_torque_max_x100   = 15000;  /* 150.00 % */
    l->travel_torque_max_x100 = 15000;
    l->start_torque_limit_x100 = 12000; /* stuck-wire detect @120 % */
    l->ramp_time_ms           = 3000;
    l->stuck_reverse_ms       = 400;
    l->stuck_fwd_ms           = 400;
    l->stuck_vibrate_cycles   = 3;
    l->stuck_torque_ok_x100   = 6000;   /* resume below 60 % */
}

static void limits_load(void)
{
    limits_default(&s_lim);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_lim);
        if (nvs_get_blob(h, NVS_KEY_LIMITS, &s_lim, &sz) != ESP_OK) {
            limits_default(&s_lim);
        }
        nvs_close(h);
    }
}

esp_err_t control_engine_set_limits(const control_limits_t *lim)
{
    if (!lim) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_eng.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_lim = *lim;
    nvs_handle_t h;
    esp_err_t err = ESP_OK;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_LIMITS, &s_lim, sizeof(s_lim));
        if (err == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_eng.lock);
    return err;
}

void control_engine_get_limits(control_limits_t *out)
{
    if (out) {
        *out = s_lim;
    }
}

/* ------------------------------------------------------------------ */
/* Public command + torque + link                                      */
/* ------------------------------------------------------------------ */
esp_err_t control_engine_cmd(vfd_cmd_code_t cmd, uint32_t arg)
{
    if (!s_eng.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t msg = ((uint32_t)cmd << 24) | (arg & 0x00FFFFFF);
    if (xQueueSend(s_eng.cmd_q, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void control_engine_set_torque(int16_t main_x100, int16_t travel_x100)
{
    if (xSemaphoreTake(s_eng.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_eng.main_torque_set   = main_x100;
        s_eng.travel_torque_set = travel_x100;
        xSemaphoreGive(s_eng.lock);
    }
}

void control_engine_link_alive(void)
{
    s_eng.last_link_us = esp_timer_get_time();
    s_eng.link_established = true;
}

void control_engine_get_telemetry(vfd_telemetry_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (xSemaphoreTake(s_eng.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    out->machine_state     = (uint8_t)s_eng.state;
    out->alarm             = (uint8_t)s_eng.alarm;
    out->fault_code_main   = s_eng.main_st.fault_code;
    out->fault_code_travel = s_eng.travel_st.fault_code;

    out->main_torque_pct   = s_eng.main_st.torque_pct_x100;
    out->main_torque_set   = s_eng.main_torque_set;
    out->main_freq_hz      = s_eng.main_st.freq_hz_x100;
    out->main_current_a    = s_eng.main_st.current_a_x10;
    out->main_bus_v        = s_eng.main_st.bus_v;

    out->travel_torque_pct = s_eng.travel_st.torque_pct_x100;
    out->travel_torque_set = s_eng.travel_torque_set;
    out->travel_freq_hz    = s_eng.travel_st.freq_hz_x100;
    out->travel_current_a  = s_eng.travel_st.current_a_x10;

    /* Wire tension proxy = travel motor output torque. */
    out->wire_tension_raw  = (uint16_t)s_eng.travel_st.torque_pct_x100;
    out->uptime_ms         = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(s_eng.lock);
}

vfd_machine_state_t control_engine_get_state(void)
{
    return s_eng.state;
}

/* ------------------------------------------------------------------ */
/* Safety helpers                                                      */
/* ------------------------------------------------------------------ */
static void safe_stop_all(void)
{
    ESP_LOGW(TAG, "SAFE STOP");
    if (s_eng.travel_vfd) vfd_stop(s_eng.travel_vfd);
    if (s_eng.main_vfd)   vfd_stop(s_eng.main_vfd);
}

static void estop_all(void)
{
    ESP_LOGE(TAG, "E-STOP");
    if (s_eng.travel_vfd) vfd_coast_stop(s_eng.travel_vfd);
    if (s_eng.main_vfd)   vfd_coast_stop(s_eng.main_vfd);
}

/* Check link watchdog; returns true if link is lost. */
static bool link_lost(void)
{
    if (!s_eng.link_established) {
        return false;   /* never had a remote -> don't fault on startup */
    }
    int64_t now = esp_timer_get_time();
    return (now - s_eng.last_link_us) > (VFD_LINK_TIMEOUT_MS * 1000LL);
}

/* ------------------------------------------------------------------ */
/* Fresh-cut stuck-wire routine                                        */
/* ------------------------------------------------------------------ */
static esp_err_t stuck_wire_routine(void)
{
    ESP_LOGW(TAG, "stuck wire: reversing + vibrating");
    raise_alarm(VFD_EVT_STUCK_WIRE);
    set_state(VFD_STATE_REVERSING);

    vfd_dev_t *m = s_eng.main_vfd;
    int16_t vib_torque = s_lim.stuck_torque_ok_x100;  /* modest torque */

    for (int c = 0; c < s_lim.stuck_vibrate_cycles; c++) {
        /* Reverse to loosen the fresh-cut sharp interface. */
        vfd_set_torque(m, -vib_torque);
        vfd_run(m, VFD_DIR_REVERSE);
        vTaskDelay(pdMS_TO_TICKS(s_lim.stuck_reverse_ms));
        vfd_stop(m);
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Forward pulse. */
        vfd_set_torque(m, vib_torque);
        vfd_run(m, VFD_DIR_FORWARD);
        vTaskDelay(pdMS_TO_TICKS(s_lim.stuck_fwd_ms));
        vfd_stop(m);
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Re-probe torque to see if friction dropped. */
        vfd_status_t st;
        if (vfd_read_status(m, &st) == ESP_OK) {
            if (st.torque_pct_x100 < s_lim.stuck_torque_ok_x100) {
                ESP_LOGI(TAG, "wire freed after %d cycles", c + 1);
                return ESP_OK;
            }
        }
    }
    ESP_LOGW(TAG, "wire still stuck after vibration");
    return ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* State machine handlers                                              */
/* ------------------------------------------------------------------ */
static void handle_cmd(uint32_t msg)
{
    vfd_cmd_code_t cmd = (vfd_cmd_code_t)(msg >> 24);
    uint32_t arg = msg & 0x00FFFFFF;

    switch (cmd) {
    case VFD_CMD_START_CUT:
        if (s_eng.state == VFD_STATE_READY || s_eng.state == VFD_STATE_IDLE) {
            set_state(VFD_STATE_STARTING);
        }
        break;
    case VFD_CMD_FRESH_CUT:
        if (s_eng.state == VFD_STATE_READY || s_eng.state == VFD_STATE_IDLE) {
            set_state(VFD_STATE_FRESH_CUT);
        }
        break;
    case VFD_CMD_STOP:
        if (s_eng.state != VFD_STATE_IDLE && s_eng.state != VFD_STATE_FAULT) {
            set_state(VFD_STATE_STOPPING);
        }
        break;
    case VFD_CMD_ESTOP:
        estop_all();
        set_state(VFD_STATE_ESTOP);
        raise_alarm(VFD_EVT_ESTOP);
        break;
    case VFD_CMD_TENSION_TEST:
        /* Travel-only torque to tighten the cable for test cutting. */
        if (s_eng.state == VFD_STATE_READY || s_eng.state == VFD_STATE_IDLE) {
            ESP_LOGI(TAG, "tension test");
            vfd_set_torque(s_eng.travel_vfd, s_eng.travel_torque_set);
            vfd_run(s_eng.travel_vfd, VFD_DIR_FORWARD);
        }
        break;
    case VFD_CMD_FAULT_RESET:
        if (s_eng.main_vfd)   vfd_fault_reset(s_eng.main_vfd);
        if (s_eng.travel_vfd) vfd_fault_reset(s_eng.travel_vfd);
        s_eng.alarm = VFD_EVT_NONE;
        set_state(VFD_STATE_READY);
        break;
    case VFD_CMD_PAUSE:
    case VFD_CMD_RESUME:
    case VFD_CMD_NONE:
    default:
        break;
    }
    (void)arg;
}

/* Ramp-up: gradually apply torque over ramp_time_ms. */
static void soft_start(void)
{
    ESP_LOGI(TAG, "soft start");
    int16_t target_main, target_travel;
    if (xSemaphoreTake(s_eng.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        target_main   = s_eng.main_torque_set;
        target_travel = s_eng.travel_torque_set;
        xSemaphoreGive(s_eng.lock);
    } else {
        target_main = target_travel = 0;
    }

    const int steps = 10;
    for (int i = 1; i <= steps; i++) {
        int16_t mt = (int16_t)((int32_t)target_main * i / steps);
        int16_t tt = (int16_t)((int32_t)target_travel * i / steps);
        vfd_set_torque(s_eng.main_vfd, mt);
        vfd_set_torque(s_eng.travel_vfd, tt);
        vTaskDelay(pdMS_TO_TICKS(s_lim.ramp_time_ms / steps));
    }
    vfd_run(s_eng.main_vfd, VFD_DIR_FORWARD);
    vfd_run(s_eng.travel_vfd, VFD_DIR_FORWARD);
    set_state(VFD_STATE_CUTTING);
}

/* Fresh cut: start, but watch start torque; if too high, run stuck routine. */
static void fresh_cut_start(void)
{
    ESP_LOGI(TAG, "fresh cut start");
    vfd_set_torque(s_eng.main_vfd, s_eng.main_torque_set);
    vfd_run(s_eng.main_vfd, VFD_DIR_FORWARD);
    vTaskDelay(pdMS_TO_TICKS(500));   /* let torque build */

    vfd_status_t st;
    if (vfd_read_status(s_eng.main_vfd, &st) == ESP_OK) {
        if (st.torque_pct_x100 > s_lim.start_torque_limit_x100) {
            vfd_stop(s_eng.main_vfd);
            if (stuck_wire_routine() == ESP_OK) {
                /* friction reduced -> slowly ramp to normal cutting */
                set_state(VFD_STATE_STARTING);
                soft_start();
            } else {
                safe_stop_all();
                set_state(VFD_STATE_FAULT);
                raise_alarm(VFD_EVT_STUCK_WIRE);
            }
            return;
        }
    }
    /* Normal fresh cut -> ramp both motors. */
    set_state(VFD_STATE_STARTING);
    soft_start();
}

/* ------------------------------------------------------------------ */
/* Engine task                                                         */
/* ------------------------------------------------------------------ */
static void engine_task(void *arg)
{
    ESP_LOGI(TAG, "engine task started");
    set_state(VFD_STATE_READY);

    for (;;) {
        /* Drain pending commands. */
        uint32_t msg;
        while (xQueueReceive(s_eng.cmd_q, &msg, 0) == pdTRUE) {
            handle_cmd(msg);
        }

        /* Link watchdog: safe stop on comm loss. */
        if (link_lost()) {
            if (s_eng.state == VFD_STATE_CUTTING ||
                s_eng.state == VFD_STATE_STARTING ||
                s_eng.state == VFD_STATE_FRESH_CUT) {
                safe_stop_all();
                set_state(VFD_STATE_STOPPING);
                raise_alarm(VFD_EVT_COMM_LOSS);
            }
            s_eng.link_established = false;  /* avoid re-triggering */
        }

        /* Poll both drives. */
        if (s_eng.main_vfd) {
            vfd_read_status(s_eng.main_vfd, &s_eng.main_st);
        }
        if (s_eng.travel_vfd) {
            vfd_read_status(s_eng.travel_vfd, &s_eng.travel_st);
        }

        /* Latch drive faults. */
        if (s_eng.main_st.fault && s_eng.state != VFD_STATE_FAULT) {
            safe_stop_all();
            set_state(VFD_STATE_FAULT);
            raise_alarm(VFD_EVT_VFD_FAULT_MAIN);
        }
        if (s_eng.travel_st.fault && s_eng.state != VFD_STATE_FAULT) {
            safe_stop_all();
            set_state(VFD_STATE_FAULT);
            raise_alarm(VFD_EVT_VFD_FAULT_TRAVEL);
        }

        /* State-specific continuous actions. */
        switch (s_eng.state) {
        case VFD_STATE_STARTING:
            /* soft_start() runs synchronously on command; nothing here */
            break;
        case VFD_STATE_CUTTING:
            /* Torque mode: drives self-regulate speed vs tension.
             * We continuously refresh torque setpoints in case encoders
             * changed them. */
            vfd_set_torque(s_eng.main_vfd, s_eng.main_torque_set);
            vfd_set_torque(s_eng.travel_vfd, s_eng.travel_torque_set);
            break;
        case VFD_STATE_STOPPING:
            if (!s_eng.main_st.running && !s_eng.travel_st.running) {
                set_state(VFD_STATE_READY);
            }
            break;
        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(ENGINE_POLL_MS));
    }
}

/* ------------------------------------------------------------------ */
esp_err_t control_engine_init(const control_engine_cfg_t *cfg)
{
    if (!cfg || !cfg->main_vfd || !cfg->travel_vfd) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_eng, 0, sizeof(s_eng));
    s_eng.main_vfd   = cfg->main_vfd;
    s_eng.travel_vfd = cfg->travel_vfd;
    s_eng.state      = VFD_STATE_IDLE;
    s_eng.lock       = xSemaphoreCreateMutex();
    s_eng.cmd_q      = xQueueCreate(8, sizeof(uint32_t));
    if (!s_eng.lock || !s_eng.cmd_q) {
        return ESP_ERR_NO_MEM;
    }

    limits_load();

    /* Configure both drives into comm + torque-control mode. */
    vfd_cfg_t mc = {
        .slave_addr       = 1,
        .torque_limit_pct = (uint16_t)s_lim.main_torque_max_x100,
        .max_freq_hz_x100 = 5000,   /* 50.00 Hz safety ceiling */
    };
    vfd_cfg_t tc = {
        .slave_addr       = 2,
        .torque_limit_pct = (uint16_t)s_lim.travel_torque_max_x100,
        .max_freq_hz_x100 = 5000,
    };
    esp_err_t err = vfd_configure(s_eng.main_vfd, &mc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "main VFD configure failed");
    }
    err = vfd_configure(s_eng.travel_vfd, &tc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "travel VFD configure failed");
    }

    s_eng.inited = true;
    ESP_LOGI(TAG, "engine initialized");
    return ESP_OK;
}

esp_err_t control_engine_start(void)
{
    if (!s_eng.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreatePinnedToCore(engine_task, "engine",
                                ENGINE_TASK_STACK, NULL,
                                ENGINE_TASK_PRIO, &s_eng.task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
