# AGENTS.md — AI Development Conventions

Guidelines for developing this project with AI assistance using minimum
token usage.

## Environment

- ESP-IDF **v6.0.1** at `/home/muditha/.espressif/v6.0.1/esp-idf`.
  Always `source /home/muditha/.espressif/v6.0.1/esp-idf/export.sh`
  before any `idf.py` command.
- `vfd_controller` target: `esp32s3` (8 MB flash, octal PSRAM).
- `remote_control` target: `esp32` (4 MB flash).
- Build: `cd <project> && idf.py build`. If the build dir is corrupt,
  `rm -rf build sdkconfig` and rebuild (sdkconfig regenerates from
  `sdkconfig.defaults`).

## Rules

1. **One module per iteration; compile immediately** after each file
   group. Never accumulate many uncompiled files.
2. **Never re-read the VFD PDFs.** Use the distilled maps in
   `docs/vfd_zuked720_modbus.md` and `docs/vfd_sf100_modbus.md`.
3. **All wire-format structs live only in `shared/vfd_protocol`.**
   Never duplicate packet structs in a project.
4. Keep filter output small: pipe builds through
   `grep -E "error:|FAILED|warning:" | head -30`.
5. Read files in large meaningful chunks, not many small reads.
6. After fixing an error, rebuild the same project to confirm before
   moving on.

## IDF 6.0 API facts (verified by compile)

- NVS: `NVS_READONLY` / `NVS_READWRITE` (not `NVS_MODE_*`).
- `esp_netif_create_default_wifi_sta()/ap()` return `esp_netif_t *`;
  check `!= NULL`, not `ESP_ERROR_CHECK`.
- No `json` component → managed dependency `espressif/cjson`.
- `esp_now` is part of `esp_wifi`; do not list it in REQUIRES.
- UART needs `esp_driver_uart`, GPIO needs `esp_driver_gpio`, PCNT
  needs `esp_driver_pcnt`, SPI needs `esp_driver_spi`, ADC needs
  `esp_adc` in component REQUIRES.
- WebSocket server needs `CONFIG_HTTPD_WS_SUPPORT=y`.
- ST7789: built into `esp_lcd` (`esp_lcd_panel_st7789.h`). There is no
  managed `esp_lcd_st7789` component. LVGL via managed `lvgl/lvgl`.
- ESP-NOW send callback:
  `void cb(const esp_now_send_info_t *, esp_now_send_status_t)`;
  success is `ESP_NOW_SEND_SUCCESS`.
- ESP-NOW LR: `esp_wifi_set_protocol(ifx, ... | WIFI_PROTOCOL_LR)` +
  per-peer `esp_now_set_peer_rate_config()` with
  `phymode=WIFI_PHY_MODE_LR, rate=WIFI_PHY_RATE_LORA_250K`.
- Classic ESP32 ADC calibration: line fitting
  (`adc_cali_create_scheme_line_fitting`); curve fitting is S3/C3+.
- LVGL v9: `lv_display_create`, `lv_display_set_buffers`,
  `lv_display_set_flush_cb`, `LV_DISPLAY_RENDER_MODE_PARTIAL`;
  kconfig `CONFIG_LV_USE_BUTTON`.
- Task WDT kconfig: `CONFIG_ESP_TASK_WDT_EN` / `CONFIG_ESP_TASK_WDT_PANIC`
  (not `CONFIG_FREERTOS_*_TASK_WDT`).
- `uart_driver_install`: `rx_buffer_size` must be **greater than** the HW
  FIFO length (128 on ESP32-S3), else `ESP_FAIL`. Use ≥ 256.

## Hardware map

See `vfd_controller/main/board.h` and `remote_control/main/board.h` for
authoritative pin assignments. RS-485: UART1, single DE pin, main VFD
Modbus addr 1 (ZUKED D720), travel addr 2 (SF100), 9600 baud.

## Known hardware-verification flags

- SF100 has no direct output-torque register; commanded torque is used
  as a proxy. Verify on hardware.
- SF100 register 0x2001 torque scale assumed signed 16-bit 0.01% units.
  Verify on hardware.
