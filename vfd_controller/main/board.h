#pragma once

/**
 * @file board.h
 * @brief Hardware pin map for the vfd_controller (ESP32-S3).
 *        All board-specific constants live here only.
 */

/* ---------------- RS-485 bus (shared by both VFDs) ---------------- */
#define BOARD_RS485_UART        1          /* UART_NUM_1 */
#define BOARD_RS485_TX          17
#define BOARD_RS485_RX          18
#define BOARD_RS485_DE          21         /* transceiver direction ctrl */
#define BOARD_RS485_BUF_SIZE    512

/* ---------------- Status LED ---------------- */
#define BOARD_LED_STATUS        48         /* onboard WS2812 or plain LED */
#define BOARD_LED_ACTIVE_LOW    0

/* ---------------- External SPI NOR flash (future log storage) ------- */
#define BOARD_FLASH_SPI_HOST    2
#define BOARD_FLASH_CS          10
#define BOARD_FLASH_SCK         12
#define BOARD_FLASH_MOSI        11
#define BOARD_FLASH_MISO        13

/* ---------------- Defaults (overridable in NVS) ---------------- */
#define BOARD_RS485_BAUD_DEFAULT   9600
#define BOARD_VFD_ADDR_MAIN        1      /* ZUKED D720 (22 kW) */
#define BOARD_VFD_ADDR_TRAVEL      2      /* SF100 (travel motor) */
