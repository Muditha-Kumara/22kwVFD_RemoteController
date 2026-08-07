# VFD Controller Server

Python server for the 22kW VFD Remote Controller system. Receives telemetry via UDP, serves OTA updates, accepts log uploads, and provides a real-time web dashboard.

## Features

- **UDP Telemetry Receiver** — Receives binary telemetry packets from the VFD controller at 10 Hz
- **Real-time Web Dashboard** — Live monitoring of machine state, motor torques, cutting metrics
- **HTTP Upload Endpoint** — Accepts batched log data from the controller
- **OTA Manifest Server** — Serves firmware manifests and binary updates
- **Telemetry History** — Stores last hour of telemetry for charting

## Requirements

- Python 3.7+
- No external dependencies (uses only standard library)

## Quick Start

```bash
cd server
python3 vfd_server.py
```

The server will start with default settings:
- UDP port: 5005 (telemetry receiver)
- HTTP port: 8080 (dashboard + API)
- Data directory: `./data/`

Access the dashboard at: **http://192.168.1.101:8080/**

## Configuration

### Command-line Options

```bash
python3 vfd_server.py --help

Options:
  --udp-port PORT     UDP port for telemetry (default: 5005)
  --http-port PORT    HTTP port for dashboard/API (default: 8080)
  --data-dir PATH     Data directory for uploads/firmware (default: ./data)
```

### Example: Custom Ports

```bash
python3 vfd_server.py --udp-port 6000 --http-port 9000
```

## API Endpoints

### Dashboard
- `GET /` — Real-time web dashboard

### Status API
- `GET /api/status` — Current telemetry as JSON
- `GET /api/history?seconds=60` — Telemetry history (last N seconds)

### Upload Endpoint
- `POST /upload` — Accept log data from controller

Request format:
```json
{
  "device": "vfd_controller",
  "rows": [
    "1234,4,0,5000,5000,5000,100,380,2000,2000,2500,50,1500,100,95",
    "..."
  ]
}
```

Response:
```json
{
  "status": "ok",
  "rows_received": 100,
  "filename": "vfd_controller_20260807_143022.csv"
}
```

### OTA Endpoints
- `GET /manifest.json` — OTA manifest for vfd_controller
- `GET /ota/{device}/manifest.json` — OTA manifest for specific device
- `GET /fw/{device}/{filename}.bin` — Firmware binary

## OTA Setup

To enable OTA updates, create the firmware directory structure:

```
data/
└── firmware/
    ├── vfd_controller/
    │   ├── manifest.json
    │   └── firmware.bin
    └── remote_control/
        ├── manifest.json
        └── firmware.bin
```

### Manifest Format

```json
{
  "magic": 575362886,
  "version": 2,
  "url": "http://192.168.1.101:8080/fw/vfd_controller/firmware.bin",
  "size": 1048576,
  "crc32": 305419896
}
```

**Important:**
- `magic` must equal `0x224B5746` (575362886 decimal)
- `version` must be greater than the device's current `APP_FW_VERSION`
- `crc32` is the CRC32 of the firmware binary
- `size` is the firmware file size in bytes

### Generating OTA Manifest

```bash
# Calculate firmware size and CRC32
FIRMWARE="data/firmware/vfd_controller/firmware.bin"
SIZE=$(stat -c%s "$FIRMWARE")
CRC32=$(python3 -c "import zlib; print(zlib.crc32(open('$FIRMWARE','rb').read()) & 0xffffffff)")

# Create manifest
cat > data/firmware/vfd_controller/manifest.json <<EOF
{
  "magic": 575362886,
  "version": 2,
  "url": "http://192.168.1.101:8080/fw/vfd_controller/firmware.bin",
  "size": $SIZE,
  "crc32": $CRC32
}
EOF
```

## Controller Configuration

Configure the VFD controller to point to this server:

### Via NVS Settings (settings.c defaults)

```c
// In vfd_controller/main/settings.c
strcpy(s->udp_ip, "192.168.1.101");      // Server IP
s->udp_port = 5005;                       // UDP port
strcpy(s->upload_url, "http://192.168.1.101:8080/upload");
strcpy(s->ota_url, "http://192.168.1.101:8080/manifest.json");
```

### Via Web Dashboard (if implemented)

The controller's web dashboard at `http://<controller-ip>/` allows changing these settings at runtime.

## Data Storage

### Uploaded Logs
- Location: `data/uploads/`
- Format: CSV files named `{device}_{timestamp}.csv`
- Contents: Telemetry rows uploaded by the controller

### Telemetry History
- Stored in memory (last 3600 samples = 1 hour at 10 Hz)
- Accessible via `/api/history` endpoint
- Used by dashboard for real-time charts

## Dashboard Features

The web dashboard displays:

- **Machine State** — Current state (IDLE, CUTTING, FAULT, etc.) with color coding
- **Main Motor (22kW)** — Torque, setpoint, frequency, current, bus voltage, fault code
- **Travel Motor** — Torque, setpoint, frequency, current, fault code
- **Cutting Metrics** — Wire tension, cut depth, link quality, uptime
- **Torque History Chart** — Real-time chart showing last 60 seconds of torque data
- **Connection Status** — Online/offline indicator with last update timestamp

The dashboard polls `/api/status` every 200ms for smooth updates.

## Protocol Details

The server parses the binary VFD protocol defined in `shared/vfd_protocol/include/vfd_protocol.h`:

- **Packet Header** (12 bytes): magic, version, type, src, dst, seq, len, crc16
- **Telemetry Payload** (34 bytes): machine state, motor torques/frequencies/currents, cutting metrics
- **CRC16**: Modbus polynomial 0xA001, init 0xFFFF

All values are little-endian (ESP32 native).

## Troubleshooting

### No Telemetry Received

1. Check controller settings: `udp_ip` and `udp_port` must match server
2. Verify network connectivity: `ping 192.168.1.101` from controller
3. Check firewall: UDP port 5005 must be open
4. Monitor server console for "UDP: X packets in last 5s" messages

### Dashboard Shows "OFFLINE"

- Server is running but not receiving telemetry
- Check UDP receiver is listening (console should show "UDP receiver listening on port 5005")
- Verify controller is sending packets (check controller logs)

### Upload Fails

- Check controller can reach `http://192.168.1.101:8080/upload`
- Verify `data/uploads/` directory is writable
- Check server console for upload error messages

### OTA Update Not Accepted

- Verify `magic` in manifest equals `0x224B5746` (575362886)
- Verify `version` is greater than controller's `APP_FW_VERSION`
- Verify `crc32` matches the firmware binary
- Verify `size` matches the firmware file size
- Check controller logs for OTA rejection reasons

## Running as a Service

### systemd (Linux)

Create `/etc/systemd/system/vfd-server.service`:

```ini
[Unit]
Description=VFD Controller Server
After=network.target

[Service]
Type=simple
User=muditha
WorkingDirectory=/home/muditha/22kwVFD_RemoteController/server
ExecStart=/usr/bin/python3 vfd_server.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable vfd-server
sudo systemctl start vfd-server
sudo systemctl status vfd-server
```

View logs:

```bash
sudo journalctl -u vfd-server -f
```

## Development

### Testing with Mock Data

You can test the dashboard without a real controller by sending mock UDP packets:

```python
import socket
import struct
import time

# Build a mock telemetry packet
MAGIC = 0x224B5746
VERSION = 0x0001
TYPE_TELEMETRY = 0x10

# Telemetry data (34 bytes)
telemetry = struct.pack('<BBHHhhHhHHhHHHBI',
    4,              # machine_state (CUTTING)
    0,              # alarm (NONE)
    0,              # fault_code_main
    0,              # fault_code_travel
    5000,           # main_torque_pct (50.00%)
    5000,           # main_torque_set (50.00%)
    5000,           # main_freq_hz (50.00 Hz)
    100,            # main_current_a (10.0 A)
    380,            # main_bus_v (380 V)
    2000,           # travel_torque_pct (20.00%)
    2000,           # travel_torque_set (20.00%)
    2500,           # travel_freq_hz (25.00 Hz)
    50,             # travel_current_a (5.0 A)
    1500,           # wire_tension_raw
    100,            # cut_depth_mm (10.0 mm)
    95,             # link_quality (95%)
    12345678        # uptime_ms
)

# Build header
header = struct.pack('<IHBBB BBH',
    MAGIC,
    VERSION,
    TYPE_TELEMETRY,
    0x01,           # src (CONTROLLER)
    0x02,           # dst (REMOTE)
    0,              # seq
    len(telemetry), # len
    0               # crc16 (placeholder)
)

# Calculate CRC16
def crc16_modbus(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

crc_data = header[:10] + telemetry
crc = crc16_modbus(crc_data)
header = header[:10] + struct.pack('<H', crc) + header[12:]

packet = header + telemetry

# Send to server
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server_addr = ('192.168.1.101', 5005)

while True:
    sock.sendto(packet, server_addr)
    time.sleep(0.1)  # 10 Hz
```

## File Structure

```
server/
├── vfd_server.py       # Main server application
├── dashboard.html      # Web dashboard (served by server)
├── README.md           # This file
└── data/               # Created at runtime
    ├── uploads/        # Uploaded log files
    └── firmware/       # OTA firmware files
        ├── vfd_controller/
        └── remote_control/
```

## License

Part of the 22kW VFD Remote Controller project.
