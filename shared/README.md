# Shared Components

Reusable ESP-IDF components included by both `vfd_controller` and
`remote_control` via `EXTRA_COMPONENT_DIRS`.

| Component | Purpose |
|---|---|
| `vfd_protocol` | ESP-NOW packet structs, pack/unpack, CRC16, magic/version constants. Single source of truth for the wireless link. |
| `sys_logger` | Unified logger: esp_log ring buffer, free-heap monitor, upload queue. Captures logs + free RAM every second for upload. |

## Rules
- Shared structs live ONLY in `vfd_protocol`. Never duplicate packet definitions.
- All structs `__attribute__((packed))`, fixed-width integers, explicit CRC.
- No FreeRTOS or driver dependencies here — keep components portable.
