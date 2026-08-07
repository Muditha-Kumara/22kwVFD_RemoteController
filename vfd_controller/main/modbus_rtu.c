#include "modbus_rtu.h"

#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "vfd_protocol.h"   /* vfd_crc16 */

#define TAG "modbus"

#define MB_MAX_FRAME   64
#define MB_GAP_MS      2     /* >= 3.5 char times @9600 (4ms); 2ms + tx wait is safe */

struct modbus_bus {
    int      uart_num;
    int      de_io;
    uint32_t rx_timeout_ms;
    SemaphoreHandle_t lock;
    uint8_t  last_exception;
};

/* ------------------------------------------------------------------ */
static void de_set(modbus_bus_t *bus, bool tx_enable)
{
    if (bus->de_io >= 0) {
        gpio_set_level((gpio_num_t)bus->de_io, tx_enable ? 1 : 0);
    }
}

modbus_bus_t *modbus_bus_init(const modbus_bus_config_t *cfg)
{
    if (!cfg) {
        return NULL;
    }

    modbus_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus) {
        return NULL;
    }
    bus->uart_num     = cfg->uart_num;
    bus->de_io        = cfg->de_io;
    bus->rx_timeout_ms = cfg->rx_timeout_ms ? cfg->rx_timeout_ms : 200;
    bus->lock = xSemaphoreCreateMutex();
    if (!bus->lock) {
        free(bus);
        return NULL;
    }

    uart_config_t ucfg = {
        .baud_rate  = (int)cfg->baud,
        .data_bits  = (cfg->data_bits == 8) ? UART_DATA_8_BITS : UART_DATA_7_BITS,
        .stop_bits  = (cfg->stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    switch (cfg->parity) {
        case 'E': ucfg.parity = UART_PARITY_EVEN; break;
        case 'O': ucfg.parity = UART_PARITY_ODD;  break;
        default:  ucfg.parity = UART_PARITY_DISABLE; break;
    }

    ESP_ERROR_CHECK(uart_driver_install(cfg->uart_num,
                                        MB_MAX_FRAME * 2, MB_MAX_FRAME * 2,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_num, &ucfg));
    ESP_ERROR_CHECK(uart_set_pin(cfg->uart_num, cfg->tx_io, cfg->rx_io,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    if (cfg->de_io >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->de_io,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io));
    }
    de_set(bus, false);   /* receive mode */

    ESP_LOGI(TAG, "bus ready uart%d baud=%lu", cfg->uart_num, (unsigned long)cfg->baud);
    return bus;
}

void modbus_bus_deinit(modbus_bus_t *bus)
{
    if (!bus) {
        return;
    }
    uart_driver_delete(bus->uart_num);
    vSemaphoreDelete(bus->lock);
    free(bus);
}

uint8_t modbus_last_exception(modbus_bus_t *bus)
{
    return bus ? bus->last_exception : 0;
}

/* ------------------------------------------------------------------ */
/* Low-level transceive: send frame, wait for expected response length */
/* ------------------------------------------------------------------ */
static esp_err_t mb_transceive(modbus_bus_t *bus,
                               const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_expect,
                               size_t *rx_len_out)
{
    uart_flush_input(bus->uart_num);

    de_set(bus, true);
    int w = uart_write_bytes(bus->uart_num, (const char *)tx, tx_len);
    esp_err_t err = ESP_OK;
    if (w != (int)tx_len) {
        err = ESP_FAIL;
    } else {
        /* wait for shift register to empty before flipping DE */
        if (uart_wait_tx_done(bus->uart_num, pdMS_TO_TICKS(20)) != ESP_OK) {
            err = ESP_FAIL;
        }
    }
    de_set(bus, false);
    if (err != ESP_OK) {
        return err;
    }

    /* Read response. Poll until we have rx_expect bytes or timeout. */
    size_t got = 0;
    int64_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(bus->rx_timeout_ms);
    while (got < rx_expect) {
        int n = uart_read_bytes(bus->uart_num, rx + got, rx_expect - got,
                                pdMS_TO_TICKS(10));
        if (n > 0) {
            got += (size_t)n;
        }
        if (xTaskGetTickCount() > deadline) {
            break;
        }
    }
    if (rx_len_out) {
        *rx_len_out = got;
    }
    if (got < rx_expect) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* Validate CRC + address + function of a response frame. */
static esp_err_t mb_check_response(modbus_bus_t *bus, uint8_t slave,
                                   const uint8_t *rx, size_t rx_len,
                                   uint8_t expect_func)
{
    bus->last_exception = 0;
    if (rx_len < 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint16_t crc_calc = vfd_crc16(rx, rx_len - 2);
    uint16_t crc_recv = (uint16_t)(rx[rx_len - 2] | (rx[rx_len - 1] << 8));
    if (crc_calc != crc_recv) {
        return ESP_ERR_INVALID_CRC;
    }
    if (rx[0] != slave) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (rx[1] == (expect_func | 0x80)) {
        /* Modbus exception */
        bus->last_exception = (rx_len >= 3) ? rx[2] : 0;
        ESP_LOGW(TAG, "slave %d exception %d (func %02X)",
                 slave, bus->last_exception, expect_func);
        return ESP_ERR_INVALID_STATE;
    }
    if (rx[1] != expect_func) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
esp_err_t modbus_write_reg(modbus_bus_t *bus, uint8_t slave,
                           uint16_t reg, uint16_t value)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(bus->lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t tx[8];
    tx[0] = slave;
    tx[1] = 0x06;
    tx[2] = (uint8_t)(reg >> 8);
    tx[3] = (uint8_t)(reg & 0xFF);
    tx[4] = (uint8_t)(value >> 8);
    tx[5] = (uint8_t)(value & 0xFF);
    uint16_t crc = vfd_crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFF);       /* CRC low byte first */
    tx[7] = (uint8_t)(crc >> 8);

    uint8_t rx[8];
    size_t rx_len = 0;
    esp_err_t err = mb_transceive(bus, tx, 8, rx, 8, &rx_len);
    if (err == ESP_OK) {
        err = mb_check_response(bus, slave, rx, rx_len, 0x06);
        if (err == ESP_OK) {
            /* echo: reg + value must match */
            if (rx[2] != tx[2] || rx[3] != tx[3] ||
                rx[4] != tx[4] || rx[5] != tx[5]) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(MB_GAP_MS));
    xSemaphoreGive(bus->lock);
    return err;
}

esp_err_t modbus_read_regs(modbus_bus_t *bus, uint8_t slave,
                           uint16_t reg, uint16_t n, uint16_t *out)
{
    if (!bus || !out || n == 0 || n > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(bus->lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t tx[8];
    tx[0] = slave;
    tx[1] = 0x03;
    tx[2] = (uint8_t)(reg >> 8);
    tx[3] = (uint8_t)(reg & 0xFF);
    tx[4] = (uint8_t)(n >> 8);
    tx[5] = (uint8_t)(n & 0xFF);
    uint16_t crc = vfd_crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFF);
    tx[7] = (uint8_t)(crc >> 8);

    const size_t rx_expect = 5 + (size_t)n * 2;
    uint8_t rx[MB_MAX_FRAME];
    size_t rx_len = 0;
    esp_err_t err = mb_transceive(bus, tx, 8, rx, rx_expect, &rx_len);
    if (err == ESP_OK) {
        err = mb_check_response(bus, slave, rx, rx_len, 0x03);
        if (err == ESP_OK && rx[2] == (uint8_t)(n * 2)) {
            for (uint16_t i = 0; i < n; i++) {
                out[i] = (uint16_t)((rx[3 + i * 2] << 8) | rx[4 + i * 2]);
            }
        } else if (err == ESP_OK) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(MB_GAP_MS));
    xSemaphoreGive(bus->lock);
    return err;
}
