#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file modbus_rtu.h
 * @brief Minimal Modbus-RTU master over a single shared RS-485 UART.
 *
 * One transceiver (DE pin) shared by both VFDs; each slave has a distinct
 * address. All access is serialized through a mutex so concurrent tasks
 * never corrupt the bus.
 *
 * Frame timing: RTU inter-frame gap >= 3.5 char times. We enforce a small
 * fixed delay after each transaction which is safe at 9600 baud.
 */

/** Opaque bus handle. */
typedef struct modbus_bus modbus_bus_t;

typedef struct {
    int      uart_num;   /* e.g. UART_NUM_1 */
    int      tx_io;
    int      rx_io;
    int      de_io;      /* direction-enable GPIO, -1 if unused */
    uint32_t baud;
    uint8_t  data_bits;  /* 8 */
    char     parity;     /* 'N','E','O' */
    uint8_t  stop_bits;  /* 1 or 2 */
    uint32_t rx_timeout_ms;   /* per-transaction read timeout */
} modbus_bus_config_t;

/**
 * @brief Initialize the RS-485 UART + DE GPIO and create the bus.
 */
modbus_bus_t *modbus_bus_init(const modbus_bus_config_t *cfg);

/**
 * @brief Write a single holding register (function 0x06).
 * @return ESP_OK on valid echo response, else error.
 */
esp_err_t modbus_write_reg(modbus_bus_t *bus, uint8_t slave,
                           uint16_t reg, uint16_t value);

/**
 * @brief Read one or more holding registers (function 0x03).
 * @param out buffer for n words
 * @return ESP_OK, or ESP_ERR_TIMEOUT / ESP_ERR_INVALID_CRC / modbus exception.
 */
esp_err_t modbus_read_regs(modbus_bus_t *bus, uint8_t slave,
                           uint16_t reg, uint16_t n, uint16_t *out);

/**
 * @brief Return the last Modbus exception code (1..4) or 0 if none.
 */
uint8_t modbus_last_exception(modbus_bus_t *bus);

/** Free the bus (rarely used). */
void modbus_bus_deinit(modbus_bus_t *bus);

#ifdef __cplusplus
}
#endif
