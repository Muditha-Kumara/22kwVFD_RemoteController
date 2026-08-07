#pragma once

/**
 * @file board.h
 * @brief Hardware pin map for the remote_control handheld (ESP32).
 *        All board-specific constants live here only.
 *
 * Peripherals:
 *  - TFT display (ST7789, SPI, 240x240)
 *  - 2 rotary encoders (quadrature + push) — main & travel torque
 *  - 3 buttons: tension-test, start-cut, fresh-cut (+ E-STOP on long press)
 *  - Battery voltage divider on ADC
 */

/* ---------------- TFT display (ST7789, SPI2) ---------------- */
#define BOARD_TFT_SPI_HOST      2
#define BOARD_TFT_SCK           18
#define BOARD_TFT_MOSI          23
#define BOARD_TFT_MISO          -1
#define BOARD_TFT_CS            5
#define BOARD_TFT_DC            16
#define BOARD_TFT_RST           17
#define BOARD_TFT_BL            4          /* backlight */
#define BOARD_TFT_H_RES         240
#define BOARD_TFT_V_RES         240
#define BOARD_TFT_PIXEL_CLK_HZ  (40 * 1000 * 1000)

/* ---------------- Rotary encoder A (main motor torque) -------- */
#define BOARD_ENC_MAIN_A        34         /* input-only pins */
#define BOARD_ENC_MAIN_B        35
#define BOARD_ENC_MAIN_BTN      32

/* ---------------- Rotary encoder B (travel motor torque) ------ */
#define BOARD_ENC_TRAVEL_A      36
#define BOARD_ENC_TRAVEL_B      39
#define BOARD_ENC_TRAVEL_BTN    33

/* ---------------- Buttons ------------------------------------- */
#define BOARD_BTN_TENSION       25         /* tighten wire / test travel */
#define BOARD_BTN_START         26         /* safe start cutting         */
#define BOARD_BTN_FRESH         27         /* fresh-cut start            */

/* ---------------- Battery ------------------------------------- */
#define BOARD_BATT_ADC          0          /* ADC1_CH0 */
#define BOARD_BATT_DIV_NUM      2          /* divider ratio: Vbatt/2 */

/* ---------------- Defaults ------------------------------------ */
#define BOARD_ESPNOW_CHANNEL    6          /* must match controller */
