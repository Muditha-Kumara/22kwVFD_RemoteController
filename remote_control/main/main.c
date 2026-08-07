/**
 * @file main.c
 * @brief remote_control entry point (handheld ESP32).
 *
 * Boot sequence:
 *  1. NVS init
 *  2. settings load
 *  3. ESP-NOW LR link init (also brings up STA wifi for OTA)
 *  4. display init (ST7789 + LVGL dashboard)
 *  5. input init (encoders, buttons, battery ADC)
 *  6. control logic task (encoders->torque, buttons->cmds, heartbeat)
 *  7. OTA check task (startup, over wifi, then releases wifi)
 *
 * The remote is battery powered and talks to the controller only over
 * ESP-NOW long range at 10 Hz. WiFi is used solely for the startup OTA
 * check, then released.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "settings.h"
#include "espnow_link.h"
#include "display.h"
#include "input.h"
#include "control_logic.h"
#include "ota_task.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "remote_control boot (handheld)");

    /* 1. NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. Settings */
    settings_load();

    /* 3. ESP-NOW LR link (STA wifi up for OTA). */
    ESP_ERROR_CHECK(espnow_link_init());

    /* 4. Display. */
    ESP_ERROR_CHECK(display_init());
    display_status("BOOTING...");

    /* 5. Input. */
    ESP_ERROR_CHECK(input_init());

    /* 6. Operator logic. */
    ESP_ERROR_CHECK(control_logic_start());

    /* 7. Startup OTA check (uses wifi, then releases it). */
    ota_task_start();

    display_status("READY");
    ESP_LOGI(TAG, "boot complete");
}
