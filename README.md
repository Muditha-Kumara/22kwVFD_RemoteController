# 22 kW Diamond Wire Cutter — Remote VFD Control System

Advanced control system for a 22 kW construction-reinforcement diamond
wire cutting machine. Two VFDs run in **torque control mode** over a
shared RS-485 (Modbus RTU) bus, driven by an ESP32-S3 controller, and
operated from a battery-powered handheld ESP32 remote over
**ESP-NOW Long Range**.

```
┌─────────────┐  ESP-NOW LR (10 Hz, ch 6)   ┌──────────────────┐
│ remote_     │ ───────────────────────────▶│ vfd_controller   │
│ control     │ ◀── telemetry @10 Hz ────── │ (ESP32-S3)       │
│ (ESP32)     │                             │                  │
│ TFT+2 enc   │                             │  RS-485 UART1    │
│ 3 buttons   │                             │   ┌──────┴─────┐ │
│ battery ADC │                             │ addr 1      addr 2│
└─────────────┘                             │ ZUKED D720   SF100│
                                            │ (22 kW main) (travel)│
                                            └──────────────────┘
```

## Repository layout

| Path | Purpose |
|---|---|
| `vfd_controller/` | Machine-side firmware (ESP32-S3, ESP-IDF 6.0). Controls both VFDs, hosts web dashboard, logs data, UDP broadcast, upload, OTA. |
| `remote_control/` | Handheld operator remote (ESP32). TFT dashboard, 2 torque encoders, 3 buttons, battery monitor, ESP-NOW LR link, startup OTA. |
| `shared/vfd_protocol/` | Single source of truth for the ESP-NOW packet protocol (structs, CRC16, magic). Used by both projects via `EXTRA_COMPONENT_DIRS`. |
| `shared/sys_logger/` | Lightweight system logger component. |
| `docs/` | `ARCHITECTURE.md`, `PROTOCOL.md`, distilled VFD Modbus maps. |
| `VFD_Manual/` | Original manufacturer PDFs (reference only — do not re-parse; use `docs/vfd_*_modbus.md`). |

## Control concept (no extra sensors)

Both VFDs operate in **torque control mode**. The cutting wire tension is
*inferred from motor torque*:

- Cutting resistance rises → main motor torque rises.
- The controller reads torque via Modbus and commands the travel VFD to
  slow down when torque is high (and vice versa), keeping wire tension
  in band without any tension sensor.
- The same principle applies symmetrically to the main drive.

### Fresh-cut stuck-wire recovery
If start torque exceeds a threshold during a fresh cut, the machine:
1. reverses the main motor,
2. vibrates the wire in both directions to break the stuck interface,
3. then slowly ramps back up to normal cutting speed.

### Safety
- Telemetry/command cadence is **10 Hz**; loss of link for **> 0.5 s**
  triggers a controlled safe stop.
- E-STOP: long-press the FRESH button on the remote.
- All settings, limits and the paired peer MAC persist in NVS.

## Building

Requires **ESP-IDF v6.0** (`source $IDF_PATH/export.sh` first).

```bash
# Machine controller (ESP32-S3)
cd vfd_controller && idf.py set-target esp32s3 && idf.py build && idf.py flash

# Handheld remote (ESP32)
cd remote_control && idf.py set-target esp32 && idf.py build && idf.py flash
```

Both projects pull `shared/` automatically via `EXTRA_COMPONENT_DIRS`
and managed components (`lvgl/lvgl`, `espressif/cjson`).

## Operating the remote

| Input | Action |
|---|---|
| MAIN encoder | Adjust main motor torque setpoint |
| TRAVEL encoder | Adjust travel motor torque setpoint |
| Encoder push | Zero that motor's torque setpoint |
| TENSION button | Travel-only torque to tighten/test the wire |
| START button | Safe soft-start cutting (slow ramp) |
| START long-press | Controlled stop |
| FRESH button | Dedicated fresh-cut start |
| FRESH long-press | **E-STOP** |

The TFT shows machine state, both torque bars/setpoints, frequency,
current, wire tension, link quality and battery level.

## Web dashboard (controller)

The controller hosts a dashboard on both STA and AP:

- `/` — live dashboard (WebSocket push @5 Hz)
- `/api/status` — JSON telemetry
- `/api/settings` — GET/POST persistent settings
- `/api/cmd` — machine commands
- `/api/logs` — recent log lines

Settings (wifi, UDP target, upload URL, OTA URL, RS-485 baud, VFD
addresses, battery calibration) are stored in NVS.

## Data pipeline (controller)

1. **UDP broadcast** — live telemetry @10 Hz to the configured IP when
   wifi is connected.
2. **Local log** — rolling CSV on FAT partition (`/logstore`), ~1 MB
   rotation.
3. **Upload** — when connected and idle, buffered lines are POSTed in
   batches to the configured URL.
4. **ESP-NOW** — all telemetry is also broadcast over ESP-NOW LR.

## OTA

Both devices check a manifest at startup (magic number + version gate +
CRC32 validation) from the configured URL and update `ota_1` if a newer
compatible firmware exists. Manifest JSON:

```json
{ "magic": 575362886, "version": 2, "url": "http://.../fw.bin",
  "size": 1048576, "crc32": 305419896 }
```

`magic` must equal `VFD_PROTO_MAGIC` (0x224B5746).

## Development notes

See `AGENTS.md` for AI-assisted development conventions and
`docs/ARCHITECTURE.md` for the module map. Build with `idf.py build`
after every change; fix compile errors immediately.
