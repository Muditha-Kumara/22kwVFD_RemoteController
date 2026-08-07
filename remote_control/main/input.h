#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file input.h
 * @brief Encoders, buttons and battery ADC for the handheld remote.
 *
 * Encoders use the PCNT peripheral (quadrature x4). Buttons are GPIO with
 * software debounce + long-press detection (E-STOP = fresh-cut long press).
 */

typedef enum {
    BTN_TENSION = 0,   /* tighten wire / travel-only test */
    BTN_START,         /* safe start cutting              */
    BTN_FRESH,         /* fresh-cut start                 */
    BTN_ENC_MAIN,      /* main encoder push               */
    BTN_ENC_TRAVEL,    /* travel encoder push             */
    BTN_COUNT,
} button_id_t;

typedef enum {
    BTN_EV_PRESS = 0,
    BTN_EV_LONG_PRESS,   /* held > 1.5 s */
    BTN_EV_RELEASE,
} button_event_t;

typedef struct {
    button_id_t    id;
    button_event_t event;
} button_msg_t;

/** Initialize encoders, buttons, battery ADC. Starts input task. */
esp_err_t input_init(void);

/** Read accumulated encoder counts and reset them (delta). */
int32_t input_encoder_delta(bool travel);

/** Read battery voltage in mV (averaged). */
uint16_t input_battery_mv(void);

/** Battery percentage 0..100 from settings thresholds. */
uint8_t input_battery_pct(void);

/** Subscribe to button events (delivered from input task). */
typedef void (*button_cb_t)(const button_msg_t *msg);
void input_on_button(button_cb_t cb);

#ifdef __cplusplus
}
#endif
