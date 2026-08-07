# ESP-NOW Link Protocol (vfd_protocol)

Single source of truth: `shared/vfd_protocol/include/vfd_protocol.h`.
This document mirrors it for humans. All structs are packed, fixed-width,
little-endian (ESP32 native). Max ESP-NOW payload 250 bytes.

## Constants

| Name | Value | Meaning |
|---|---|---|
| `VFD_PROTO_MAGIC` | `0x224B5746` | Protocol magic; bump on breaking change. OTA gate. |
| `VFD_PROTO_VERSION` | `0x0001` | Protocol version. |
| `VFD_TELEM_HZ` | 10 | Telemetry cadence. |
| `VFD_LINK_TIMEOUT_MS` | 500 | Safe-stop if no valid packet for this long. |

## Devices

| ID | Device |
|---|---|
| `0x01` | `VFD_DEV_CONTROLLER` (machine, ESP32-S3) |
| `0x02` | `VFD_DEV_REMOTE` (handheld, ESP32) |

## Packet header (every packet)

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;     // VFD_PROTO_MAGIC
    uint16_t version;   // VFD_PROTO_VERSION
    uint8_t  type;      // vfd_packet_type_t
    uint8_t  src;       // sender device
    uint8_t  dst;       // receiver device
    uint8_t  seq;       // rolling sequence (loss detection)
    uint8_t  len;       // payload bytes
    uint16_t crc16;     // CRC16 over header(excl crc)+payload
} vfd_pkt_hdr_t;
```

CRC16: Modbus poly `0xA001`, init `0xFFFF`, low byte first on the wire
(same as RS-485 frames). Build with `vfd_pkt_build()`, validate with
`vfd_pkt_parse()`.

## Packet types

| Type | Code | Direction | Payload |
|---|---|---|---|
| `TELEMETRY` | `0x10` | controller → remote | `vfd_telemetry_t` @10 Hz |
| `EVENT` | `0x11` | controller → remote | discrete event/alarm |
| `CMD` | `0x20` | remote → controller | `vfd_cmd_t` |
| `TORQUE_SET` | `0x21` | remote → controller | `vfd_torque_set_t` |
| `HEARTBEAT` | `0x22` | remote → controller | `vfd_heartbeat_t` @1 Hz |
| `OTA_HDR` | `0x30` | bidirectional | OTA handshake |
| `ACK` | `0x40` | bidirectional | generic ack |

## Telemetry (`vfd_telemetry_t`)

Fields (units in parentheses):

- machine: `machine_state`, `alarm`, `fault_code_main`, `fault_code_travel`
- main VFD: `main_torque_pct` (0.01%), `main_torque_set`, `main_freq_hz`
  (0.01 Hz), `main_current_a` (0.1 A), `main_bus_v` (1 V)
- travel VFD: `travel_torque_pct`, `travel_torque_set`, `travel_freq_hz`,
  `travel_current_a`
- cutting/link: `wire_tension_raw` (torque-derived proxy), `cut_depth_mm`
  (0.1 mm), `link_quality` (0–100), `uptime_ms`

## Machine states

`IDLE(0) CONNECTING(1) READY(2) STARTING(3) CUTTING(4) FRESH_CUT(5)
REVERSING(6) STOPPING(7) ESTOP(8) FAULT(9)`

## Commands (`vfd_cmd_t`)

`cmd` (u8) + `arg0` (u8) + `arg1` (u16) + `arg2` (u32).

| Code | Command |
|---|---|
| 0 | NONE |
| 1 | START_CUT (safe soft start) |
| 2 | FRESH_CUT |
| 3 | STOP (controlled) |
| 4 | ESTOP |
| 5 | TENSION_TEST (travel-only tighten) |
| 6 | FAULT_RESET |
| 7 | PAUSE |
| 8 | RESUME |

## Torque setpoint (`vfd_torque_set_t`)

`main_torque_set` (i16, 0.01%), `travel_torque_set` (i16), `flags`
(bit0 main enabled, bit1 travel enabled). Sent on encoder change,
throttled to ≤10 Hz.

## Heartbeat (`vfd_heartbeat_t`)

`battery_mv` (u16), `battery_pct` (u8), `flags` (bit0 charging). @1 Hz.

## OTA manifest (HTTP JSON, not ESP-NOW)

```json
{ "magic": 575362886, "version": 2, "url": "http://.../fw.bin",
  "size": 1048576, "crc32": 305419896 }
```

Device accepts the update only if `magic == VFD_PROTO_MAGIC` and
`version > APP_FW_VERSION`, then validates the downloaded image CRC32
before committing the OTA partition.
