#include "input.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"

#define TAG "input"

#define DEBOUNCE_MS       30
#define LONG_PRESS_MS     1500
#define INPUT_TASK_PERIOD 10   /* ms */

/* ------------------------------------------------------------------ */
static struct {
    pcnt_unit_handle_t enc_main;
    pcnt_unit_handle_t enc_travel;
    int32_t count_main;
    int32_t count_travel;

    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t         adc_cali;
    uint32_t batt_mv_sum;
    uint32_t batt_mv_n;

    button_cb_t cb;

    struct {
        bool     level;
        bool     pressed;
        int64_t  press_time;
        bool     long_fired;
    } btn[BTN_COUNT];
} s_in;

static const int s_btn_gpio[BTN_COUNT] = {
    [BTN_TENSION]    = BOARD_BTN_TENSION,
    [BTN_START]      = BOARD_BTN_START,
    [BTN_FRESH]      = BOARD_BTN_FRESH,
    [BTN_ENC_MAIN]   = BOARD_ENC_MAIN_BTN,
    [BTN_ENC_TRAVEL] = BOARD_ENC_TRAVEL_BTN,
};

/* ------------------------------------------------------------------ */
static esp_err_t encoder_init(bool travel)
{
    pcnt_unit_config_t uc = {
        .high_limit = 32767,
        .low_limit  = -32768,
    };
    pcnt_unit_handle_t *unit = travel ? &s_in.enc_travel : &s_in.enc_main;
    ESP_ERROR_CHECK(pcnt_new_unit(&uc, unit));

    pcnt_chan_config_t ca = {
        .edge_gpio_num  = travel ? BOARD_ENC_TRAVEL_A : BOARD_ENC_MAIN_A,
        .level_gpio_num = travel ? BOARD_ENC_TRAVEL_B : BOARD_ENC_MAIN_B,
    };
    pcnt_channel_handle_t ch_a, ch_b;
    ESP_ERROR_CHECK(pcnt_new_channel(*unit, &ca, &ch_a));
    /* quadrature x4: count on both edges of A, direction from B level */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(
        ch_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(
        ch_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    pcnt_chan_config_t cb_cfg = {
        .edge_gpio_num  = travel ? BOARD_ENC_TRAVEL_B : BOARD_ENC_MAIN_B,
        .level_gpio_num = travel ? BOARD_ENC_TRAVEL_A : BOARD_ENC_MAIN_A,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(*unit, &cb_cfg, &ch_b));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(
        ch_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(
        ch_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(*unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(*unit));
    ESP_ERROR_CHECK(pcnt_unit_start(*unit));
    return ESP_OK;
}

int32_t input_encoder_delta(bool travel)
{
    pcnt_unit_handle_t u = travel ? s_in.enc_travel : s_in.enc_main;
    int cnt = 0;
    pcnt_unit_get_count(u, &cnt);
    pcnt_unit_clear_count(u);
    return cnt;
}

/* ------------------------------------------------------------------ */
uint16_t input_battery_mv(void)
{
    if (s_in.batt_mv_n == 0) {
        return 0;
    }
    return (uint16_t)(s_in.batt_mv_sum / s_in.batt_mv_n);
}

uint8_t input_battery_pct(void)
{
    extern uint16_t settings_batt_full_mv(void);
    extern uint16_t settings_batt_empty_mv(void);
    uint16_t mv = input_battery_mv();
    uint16_t full = settings_batt_full_mv();
    uint16_t empty = settings_batt_empty_mv();
    if (mv >= full) return 100;
    if (mv <= empty) return 0;
    return (uint8_t)(((uint32_t)(mv - empty) * 100) / (full - empty));
}

/* ------------------------------------------------------------------ */
void input_on_button(button_cb_t cb)
{
    s_in.cb = cb;
}

static void emit(button_id_t id, button_event_t ev)
{
    if (s_in.cb) {
        button_msg_t m = { .id = id, .event = ev };
        s_in.cb(&m);
    }
}

static void poll_buttons(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < BTN_COUNT; i++) {
        bool lvl = gpio_get_level(s_btn_gpio[i]) == 0;   /* active low */
        if (lvl != s_in.btn[i].level) {
            s_in.btn[i].level = lvl;
            if (lvl) {
                s_in.btn[i].pressed = true;
                s_in.btn[i].press_time = now;
                s_in.btn[i].long_fired = false;
            } else if (s_in.btn[i].pressed) {
                s_in.btn[i].pressed = false;
                if (!s_in.btn[i].long_fired &&
                    (now - s_in.btn[i].press_time) >= DEBOUNCE_MS) {
                    emit((button_id_t)i, BTN_EV_PRESS);
                }
                emit((button_id_t)i, BTN_EV_RELEASE);
            }
        }
        if (s_in.btn[i].pressed && !s_in.btn[i].long_fired &&
            (now - s_in.btn[i].press_time) >= LONG_PRESS_MS) {
            s_in.btn[i].long_fired = true;
            emit((button_id_t)i, BTN_EV_LONG_PRESS);
        }
    }
}

static void poll_battery(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_in.adc, ADC_CHANNEL_0, &raw) == ESP_OK) {
        int mv = 0;
        if (s_in.adc_cali &&
            adc_cali_raw_to_voltage(s_in.adc_cali, raw, &mv) == ESP_OK) {
            mv *= BOARD_BATT_DIV_NUM;
        } else {
            mv = (raw * 3300 * BOARD_BATT_DIV_NUM) / 4095;
        }
        /* rolling average of 16 samples */
        if (s_in.batt_mv_n >= 16) {
            s_in.batt_mv_sum -= s_in.batt_mv_sum / s_in.batt_mv_n;
            s_in.batt_mv_n--;
        }
        s_in.batt_mv_sum += (uint32_t)mv;
        s_in.batt_mv_n++;
    }
}

static void input_task(void *arg)
{
    for (;;) {
        poll_buttons();
        poll_battery();
        vTaskDelay(pdMS_TO_TICKS(INPUT_TASK_PERIOD));
    }
}

/* ------------------------------------------------------------------ */
esp_err_t input_init(void)
{
    memset(&s_in, 0, sizeof(s_in));

    /* Encoders */
    ESP_ERROR_CHECK(encoder_init(false));
    ESP_ERROR_CHECK(encoder_init(true));

    /* Buttons: input with pull-up, active low */
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < BTN_COUNT; i++) {
        io.pin_bit_mask = 1ULL << s_btn_gpio[i];
        gpio_config(&io);
    }

    /* Battery ADC */
    adc_oneshot_unit_init_cfg_t ac = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ac, &s_in.adc));
    adc_oneshot_chan_cfg_t cc = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_in.adc, ADC_CHANNEL_0, &cc));

    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_in.adc_cali) != ESP_OK) {
        s_in.adc_cali = NULL;   /* fallback to raw scaling */
    }

    if (xTaskCreate(input_task, "input", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "input ready");
    return ESP_OK;
}
