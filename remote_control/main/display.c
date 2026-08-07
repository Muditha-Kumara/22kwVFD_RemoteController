#include "display.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "lvgl.h"

#include "board.h"

#define TAG "display"

/* ------------------------------------------------------------------ */
static struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
    SemaphoreHandle_t         lv_mux;
    lv_display_t             *lv_disp;

    /* widgets */
    lv_obj_t *lbl_state;
    lv_obj_t *bar_main, *bar_travel;
    lv_obj_t *lbl_main_t, *lbl_travel_t;
    lv_obj_t *lbl_main_f, *lbl_travel_f;
    lv_obj_t *lbl_main_set, *lbl_travel_set;
    lv_obj_t *lbl_tension;
    lv_obj_t *lbl_link, *lbl_alarm, *lbl_batt;
    lv_obj_t *lbl_status;
} s_d;

/* ------------------------------------------------------------------ */
/* LVGL flush + tick                                                   */
/* ------------------------------------------------------------------ */
static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    size_t len = (size_t)(area->x2 - area->x1 + 1) *
                 (area->y2 - area->y1 + 1) * sizeof(uint16_t);
    esp_lcd_panel_io_tx_color(s_d.io, -1, px, len);
    lv_display_flush_ready(disp);
}

static void lv_tick_task(void *arg)
{
    for (;;) {
        lv_tick_inc(5);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void lv_timer_task(void *arg)
{
    for (;;) {
        if (xSemaphoreTake(s_d.lv_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_d.lv_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(33));   /* ~30 fps */
    }
}

/* ------------------------------------------------------------------ */
/* Dashboard layout                                                    */
/* ------------------------------------------------------------------ */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, int x, int y,
                          const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_bar(lv_obj_t *parent, int x, int y, int w, int h,
                        lv_color_t color)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_bar_set_range(b, 0, 15000);   /* 0..150.00 % x100 */
    lv_obj_set_style_bg_color(b, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
    return b;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    lv_color_t white = lv_color_hex(0xFFFFFF);
    lv_color_t cyan  = lv_color_hex(0x00E5FF);
    lv_color_t green = lv_color_hex(0x00FF88);
    lv_color_t amber = lv_color_hex(0xFFB300);

    /* Header: state */
    s_d.lbl_state = mk_label(scr, "IDLE", 8, 4,
                             &lv_font_montserrat_28, white);

    /* Battery + link top-right */
    s_d.lbl_batt = mk_label(scr, "--%", 180, 4, &lv_font_montserrat_14, green);
    s_d.lbl_link = mk_label(scr, "LNK --", 180, 22, &lv_font_montserrat_14, cyan);

    /* Main motor block */
    mk_label(scr, "MAIN 22kW", 8, 40, &lv_font_montserrat_14, amber);
    s_d.bar_main = mk_bar(scr, 8, 60, 224, 16, amber);
    s_d.lbl_main_t   = mk_label(scr, "0.0%", 8, 80, &lv_font_montserrat_20, white);
    s_d.lbl_main_set = mk_label(scr, "set 0.0%", 110, 80, &lv_font_montserrat_14, amber);
    s_d.lbl_main_f   = mk_label(scr, "0.0 Hz  0.0 A", 8, 104, &lv_font_montserrat_14, white);

    /* Travel motor block */
    mk_label(scr, "TRAVEL", 8, 126, &lv_font_montserrat_14, cyan);
    s_d.bar_travel = mk_bar(scr, 8, 146, 224, 16, cyan);
    s_d.lbl_travel_t   = mk_label(scr, "0.0%", 8, 166, &lv_font_montserrat_20, white);
    s_d.lbl_travel_set = mk_label(scr, "set 0.0%", 110, 166, &lv_font_montserrat_14, cyan);
    s_d.lbl_travel_f   = mk_label(scr, "0.0 Hz  0.0 A", 8, 190, &lv_font_montserrat_14, white);

    /* Footer: tension, alarm, status */
    s_d.lbl_tension = mk_label(scr, "TENSION --", 8, 214, &lv_font_montserrat_14, white);
    s_d.lbl_alarm   = mk_label(scr, "", 110, 214, &lv_font_montserrat_14, lv_color_hex(0xFF3333));
    s_d.lbl_status  = mk_label(scr, "", 8, 230, &lv_font_montserrat_14, green);
}

/* ------------------------------------------------------------------ */
static const char *state_name(uint8_t st)
{
    static const char *names[] = {
        "IDLE", "CONNECTING", "READY", "STARTING", "CUTTING",
        "FRESH CUT", "REVERSING", "STOPPING", "E-STOP", "FAULT",
    };
    return (st < sizeof(names) / sizeof(names[0])) ? names[st] : "?";
}

static const char *alarm_name(uint8_t a)
{
    static const char *names[] = {
        "", "COMM LOSS", "MAIN FAULT", "TRAVEL FAULT", "OVER TORQUE",
        "STUCK WIRE", "E-STOP", "WIFI OK", "WIFI LOST", "OTA READY",
    };
    return (a < sizeof(names) / sizeof(names[0])) ? names[a] : "ALARM";
}

void display_update(const vfd_telemetry_t *t)
{
    if (!t || xSemaphoreTake(s_d.lv_mux, pdMS_TO_TICKS(30)) != pdTRUE) {
        return;
    }
    char buf[32];

    lv_label_set_text(s_d.lbl_state, state_name(t->machine_state));
    lv_obj_set_style_text_color(s_d.lbl_state,
        (t->machine_state == VFD_STATE_FAULT || t->machine_state == VFD_STATE_ESTOP)
            ? lv_color_hex(0xFF3333) : lv_color_hex(0xFFFFFF), 0);

    /* main */
    int mt = t->main_torque_pct < 0 ? -t->main_torque_pct : t->main_torque_pct;
    lv_bar_set_value(s_d.bar_main, mt > 15000 ? 15000 : mt, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d.%02d%%",
             t->main_torque_pct / 100,
             t->main_torque_pct < 0 ? -(t->main_torque_pct % 100)
                                    : (t->main_torque_pct % 100));
    lv_label_set_text(s_d.lbl_main_t, buf);
    snprintf(buf, sizeof(buf), "%d.%01d Hz  %d.%01d A",
             t->main_freq_hz / 100, (t->main_freq_hz % 100) / 10,
             t->main_current_a / 10, t->main_current_a % 10);
    lv_label_set_text(s_d.lbl_main_f, buf);

    /* travel */
    int tt = t->travel_torque_pct < 0 ? -t->travel_torque_pct : t->travel_torque_pct;
    lv_bar_set_value(s_d.bar_travel, tt > 15000 ? 15000 : tt, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d.%02d%%",
             t->travel_torque_pct / 100,
             t->travel_torque_pct < 0 ? -(t->travel_torque_pct % 100)
                                      : (t->travel_torque_pct % 100));
    lv_label_set_text(s_d.lbl_travel_t, buf);
    snprintf(buf, sizeof(buf), "%d.%01d Hz  %d.%01d A",
             t->travel_freq_hz / 100, (t->travel_freq_hz % 100) / 10,
             t->travel_current_a / 10, t->travel_current_a % 10);
    lv_label_set_text(s_d.lbl_travel_f, buf);

    /* tension / link / alarm */
    snprintf(buf, sizeof(buf), "TENSION %d.%02d",
             t->wire_tension_raw / 100, t->wire_tension_raw % 100);
    lv_label_set_text(s_d.lbl_tension, buf);
    snprintf(buf, sizeof(buf), "LNK %d%%", t->link_quality);
    lv_label_set_text(s_d.lbl_link, buf);
    lv_label_set_text(s_d.lbl_alarm, alarm_name(t->alarm));

    xSemaphoreGive(s_d.lv_mux);
}

void display_status(const char *msg)
{
    if (xSemaphoreTake(s_d.lv_mux, pdMS_TO_TICKS(30)) == pdTRUE) {
        lv_label_set_text(s_d.lbl_status, msg ? msg : "");
        xSemaphoreGive(s_d.lv_mux);
    }
}

void display_set_torque_set(int16_t main_x100, int16_t travel_x100)
{
    char buf[24];
    if (xSemaphoreTake(s_d.lv_mux, pdMS_TO_TICKS(30)) == pdTRUE) {
        snprintf(buf, sizeof(buf), "set %d.%02d%%",
                 main_x100 / 100, main_x100 % 100 < 0 ? -(main_x100 % 100) : main_x100 % 100);
        lv_label_set_text(s_d.lbl_main_set, buf);
        snprintf(buf, sizeof(buf), "set %d.%02d%%",
                 travel_x100 / 100, travel_x100 % 100 < 0 ? -(travel_x100 % 100) : travel_x100 % 100);
        lv_label_set_text(s_d.lbl_travel_set, buf);
        xSemaphoreGive(s_d.lv_mux);
    }
}

void display_set_battery(uint16_t mv, uint8_t pct)
{
    char buf[16];
    if (xSemaphoreTake(s_d.lv_mux, pdMS_TO_TICKS(30)) == pdTRUE) {
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(s_d.lbl_batt, buf);
        lv_obj_set_style_text_color(s_d.lbl_batt,
            pct < 20 ? lv_color_hex(0xFF3333) : lv_color_hex(0x00FF88), 0);
        xSemaphoreGive(s_d.lv_mux);
    }
}

/* ------------------------------------------------------------------ */
esp_err_t display_init(void)
{
    memset(&s_d, 0, sizeof(s_d));
    s_d.lv_mux = xSemaphoreCreateMutex();

    /* Backlight on */
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BOARD_TFT_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(BOARD_TFT_BL, 1);

    /* SPI bus */
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_TFT_MOSI,
        .miso_io_num = BOARD_TFT_MISO,
        .sclk_io_num = BOARD_TFT_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_TFT_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_TFT_SPI_HOST, &bus,
                                       SPI_DMA_CH_AUTO));

    /* Panel IO */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_TFT_DC,
        .cs_gpio_num = BOARD_TFT_CS,
        .pclk_hz = BOARD_TFT_PIXEL_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_TFT_SPI_HOST, &io_cfg, &s_d.io));

    /* ST7789 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_d.io, &panel_cfg, &s_d.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_d.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_d.panel));
    /* ST7789 on many modules is inverted by default */
    esp_lcd_panel_invert_color(s_d.panel, true);
    esp_lcd_panel_disp_on_off(s_d.panel, true);

    /* LVGL */
    lv_init();

    static uint8_t *buf1;
    buf1 = heap_caps_malloc(BOARD_TFT_H_RES * 40 * sizeof(uint16_t),
                            MALLOC_CAP_DMA);
    s_d.lv_disp = lv_display_create(BOARD_TFT_H_RES, BOARD_TFT_V_RES);
    lv_display_set_buffers(s_d.lv_disp, buf1, NULL,
                           BOARD_TFT_H_RES * 40 * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_d.lv_disp, lv_flush_cb);

    build_ui();

    xTaskCreate(lv_tick_task, "lv_tick", 2048, NULL, 5, NULL);
    xTaskCreate(lv_timer_task, "lv_timer", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}
