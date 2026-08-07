/**
 * @file main.c
 * @brief vfd_controller entry point (ESP32-S3).
 *
 * Boot sequence:
 *  1. NVS init
 *  2. sys_logger init (captures all logs + free heap)
 *  3. RS-485 bus init, create both VFD drivers
 *  4. control_engine init (configures drives into comm + torque mode)
 *  5. esp-now LR link init (APSTA wifi + telemetry @10 Hz)
 *  6. wifi_manager / web_server / udp_broadcast / upload_task / ota_task
 *  7. Start control engine task
 *
 * Priority 1 = VFD safety control + esp-now link. WiFi/web/upload/OTA are
 * background services that must never interrupt cutting.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "sys_logger.h"
#include "board.h"
#include "modbus_rtu.h"
#include "vfd.h"
#include "control_engine.h"
#include "espnow_link.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "vfd_controller boot (22kW diamond wire cutter)");

    /* 1. NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. Logger */
    sys_logger_init();

    /* 3. RS-485 bus + VFD drivers */
    modbus_bus_config_t bus_cfg = {
        .uart_num      = BOARD_RS485_UART,
        .tx_io         = BOARD_RS485_TX,
        .rx_io         = BOARD_RS485_RX,
        .de_io         = BOARD_RS485_DE,
        .baud          = BOARD_RS485_BAUD_DEFAULT,
        .data_bits     = 8,
        .parity        = 'N',
        .stop_bits     = 1,
        .rx_timeout_ms = 100,
    };
    modbus_bus_t *bus = modbus_bus_init(&bus_cfg);
    if (!bus) {
        ESP_LOGE(TAG, "RS-485 bus init failed");
        return;
    }

    vfd_dev_t *main_vfd   = vfd_zuked720_create(bus);   /* 22 kW */
    vfd_dev_t *travel_vfd = vfd_sf100_create(bus);      /* travel */
    if (!main_vfd || !travel_vfd) {
        ESP_LOGE(TAG, "VFD driver alloc failed");
        return;
    }

    /* 4. Control engine */
    control_engine_cfg_t eng_cfg = {
        .main_vfd   = main_vfd,
        .travel_vfd = travel_vfd,
    };
    ESP_ERROR_CHECK(control_engine_init(&eng_cfg));

    /* 5. ESP-NOW long-range link (brings up APSTA wifi). */
    ESP_ERROR_CHECK(espnow_link_init());

    /* 6. Network services (web dashboard, UDP broadcast, upload, OTA). */
    extern void wifi_manager_start(void);
    extern void web_server_start(void);
    extern void udp_broadcast_start(void);
    extern void upload_task_start(void);
    extern void ota_task_start(void);
    wifi_manager_start();
    web_server_start();
    udp_broadcast_start();
    upload_task_start();
    ota_task_start();

    /* 7. Start the safety-critical control engine. */
    ESP_ERROR_CHECK(control_engine_start());

    ESP_LOGI(TAG, "boot complete");
}
