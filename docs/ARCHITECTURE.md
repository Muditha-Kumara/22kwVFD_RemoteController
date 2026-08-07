# Architecture

## System overview

Two independent ESP-IDF 6.0 projects plus shared components:

```
22kwVFD_RemoteController/
├── shared/
│   ├── vfd_protocol/     packet structs, CRC16, magic (both projects)
│   └── sys_logger/       leveled logger component
├── vfd_controller/       ESP32-S3 — machine side
└── remote_control/       ESP32    — handheld operator side
```

Both projects include `../shared` through `EXTRA_COMPONENT_DIRS` in
their top-level `CMakeLists.txt`. **All wire-format structs live only in
`shared/vfd_protocol`** — never duplicate them in a project.

## vfd_controller (ESP32-S3)

Module map (`main/`):

| Module | Responsibility |
|---|---|
| `main.c` | Boot orchestration: NVS → logger → RS-485 → VFD drivers → control engine → network services. |
| `board.h` | Pin map: RS-485 UART1 (TX=17, RX=18, DE=21), 9600 baud, main addr 1, travel addr 2. |
| `settings.c` | NVS-backed `app_settings_t` (wifi, udp/upload/ota URLs, RS-485, battery calibration). |
| `modbus_rtu.c` | Modbus RTU master: FC 0x03 read / 0x06 write, mutex, DE control, CRC16. Single shared bus. |
| `vfd.h` | Abstract VFD interface (`vfd_dev_t`, `vfd_ops_t`, inline wrappers). |
| `vfd_zuked720.c` | ZUKED D720 (22 kW main) driver — torque control mode. |
| `vfd_sf100.c` | SF100 (travel) driver — torque control mode. ⚠ torque readback uses commanded torque as proxy (verify on hardware). |
| `control_engine.c` | Cutting state machine (IDLE→…→CUTTING/FAULT), tension-from-torque regulation, fresh-cut stuck-wire recovery, safe stop on link loss. |
| `espnow_link.c` | Controller-side ESP-NOW LR: telemetry out @10 Hz, commands/torque/heartbeat in, peer MAC in NVS. |
| `wifi_manager.c` | STA+AP coexistence; reconnect deferred while cutting is active. |
| `web_server.c` | Dashboard + REST + WebSocket (`CONFIG_HTTPD_WS_SUPPORT=y`). |
| `data_log.c` | Rolling CSV on FAT `/logstore`, rotate @1 MB, buffered flush ~1 s. |
| `udp_broadcast.c` | Telemetry UDP @10 Hz to configured target when STA connected. |
| `upload_task.c` | Batched HTTP POST of buffered log lines when connected+idle. |
| `ota_task.c` | Startup OTA: manifest magic/version gate + CRC32 validation. |

State machine (`vfd_machine_state_t`):
`IDLE → CONNECTING → READY → STARTING → CUTTING ⇄ (FRESH_CUT → REVERSING → STARTING) → STOPPING → IDLE`, with `ESTOP` and `FAULT` reachable from anywhere.

## remote_control (ESP32)

| Module | Responsibility |
|---|---|
| `main.c` | Boot: NVS → settings → ESP-NOW link → display → input → control logic → OTA check. |
| `board.h` | ST7789 SPI TFT (240×240), 2× rotary encoders (PCNT), 3 buttons, battery ADC (GPIO0, ÷2 divider). |
| `espnow_link.c` | Remote-side ESP-NOW LR: telemetry in @10 Hz, auto-learns controller MAC, commands/torque/heartbeat out. |
| `input.c` | PCNT quadrature ×4 encoders, debounced buttons with long-press, battery ADC with line-fitting calibration. |
| `display.c` | ST7789 (built-in esp_lcd driver) + LVGL v9 dashboard; all LVGL calls under `lv_mux`. |
| `control_logic.c` | Encoders → torque setpoints (≤10 Hz), buttons → commands, heartbeat+battery @1 Hz, link-loss warning. |
| `settings.c` | NVS settings (wifi for OTA, ota_url, battery calibration, torque step). |
| `ota_task.c` | Startup wifi OTA check, then releases wifi (restores ESP-NOW channel). |

## Communication timing

- Telemetry cadence: **10 Hz** (`VFD_TELEM_PERIOD_MS = 100`).
- Link timeout: **500 ms** (`VFD_LINK_TIMEOUT_MS`) → controller safe-stops.
- Remote heartbeat: 1 Hz (carries battery level).
- Torque setpoints: sent on encoder change, throttled to ≤10 Hz.

## Key IDF 6.0 gotchas (learned the hard way)

- `NVS_READONLY` / `NVS_READWRITE` (not `NVS_MODE_*`).
- `esp_netif_create_default_wifi_sta()` returns a **pointer** — check `!= NULL`.
- `json` component removed → managed `espressif/cjson`.
- `esp_now` is part of `esp_wifi` — never list as a component in REQUIRES.
- UART/GPIO drivers need `esp_driver_uart` / `esp_driver_gpio` in REQUIRES.
- WebSocket needs `CONFIG_HTTPD_WS_SUPPORT=y`.
- ST7789 is built into `esp_lcd` (`esp_lcd_panel_st7789.h`) — no managed component exists.
- ESP-NOW send callback: `(const esp_now_send_info_t *, esp_now_send_status_t)`.
- Classic ESP32 ADC calibration: `adc_cali_line_fitting_config_t` + `adc_cali_create_scheme_line_fitting` (curve fitting is S3/C3+ only).
- LVGL v9 kconfig: `CONFIG_LV_USE_BUTTON` (not `LV_USE_BTN`).
