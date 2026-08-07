#!/usr/bin/env python3
"""
VFD Controller Server
Receives telemetry via UDP, serves OTA manifests, accepts log uploads,
and provides a real-time web dashboard.
"""

import socket
import struct
import json
import time
import threading
import zlib
from pathlib import Path
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import argparse

# Protocol constants (must match vfd_protocol.h)
VFD_PROTO_MAGIC = 0x224B5746
VFD_PROTO_VERSION = 0x0001

# Packet types
VFD_PKT_TELEMETRY = 0x10
VFD_PKT_EVENT = 0x11
VFD_PKT_CMD = 0x20
VFD_PKT_TORQUE_SET = 0x21
VFD_PKT_HEARTBEAT = 0x22
VFD_PKT_OTA_HDR = 0x30
VFD_PKT_ACK = 0x40

# Machine states
MACHINE_STATES = {
    0: "IDLE",
    1: "CONNECTING",
    2: "READY",
    3: "STARTING",
    4: "CUTTING",
    5: "FRESH_CUT",
    6: "REVERSING",
    7: "STOPPING",
    8: "ESTOP",
    9: "FAULT",
}

# Event codes
EVENT_CODES = {
    0: "NONE",
    1: "COMM_LOSS",
    2: "VFD_FAULT_MAIN",
    3: "VFD_FAULT_TRAVEL",
    4: "OVER_TORQUE",
    5: "STUCK_WIRE",
    6: "ESTOP",
    7: "WIFI_CONNECTED",
    8: "WIFI_LOST",
    9: "OTA_AVAILABLE",
}


def crc16_modbus(data):
    """Calculate Modbus CRC16 (poly 0xA001, init 0xFFFF)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


class TelemetryData:
    """Stores latest telemetry and recent history."""
    
    def __init__(self):
        self.lock = threading.Lock()
        self.latest = None
        self.history = []
        self.max_history = 3600  # 1 hour at 10 Hz
        self.last_update = 0
        
    def update(self, telemetry):
        with self.lock:
            self.latest = telemetry
            self.last_update = time.time()
            self.history.append({
                'timestamp': time.time(),
                **telemetry
            })
            if len(self.history) > self.max_history:
                self.history = self.history[-self.max_history:]
    
    def get_latest(self):
        with self.lock:
            return self.latest, self.last_update
    
    def get_history(self, seconds=60):
        with self.lock:
            cutoff = time.time() - seconds
            return [h for h in self.history if h['timestamp'] >= cutoff]


class UploadStorage:
    """Stores uploaded log data."""
    
    def __init__(self, storage_dir):
        self.storage_dir = Path(storage_dir)
        self.storage_dir.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        
    def store(self, device, rows):
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"{device}_{timestamp}.csv"
        filepath = self.storage_dir / filename
        
        with self.lock:
            with open(filepath, 'w') as f:
                # Write header
                f.write("ts,state,alarm,mtq,mtqs,mfq,mca,mbv,ttq,ttqs,tfq,tca,tension,depth,lq\n")
                # Write rows
                for row in rows:
                    f.write(row + '\n')
        
        return filename


class OTAManager:
    """Manages OTA manifests and firmware files."""
    
    def __init__(self, firmware_dir):
        self.firmware_dir = Path(firmware_dir)
        self.firmware_dir.mkdir(parents=True, exist_ok=True)
        self.manifests = {}
        self.load_manifests()
        
    def load_manifests(self):
        """Load all manifest.json files from firmware directory."""
        for manifest_file in self.firmware_dir.glob("*/manifest.json"):
            try:
                with open(manifest_file) as f:
                    manifest = json.load(f)
                    device = manifest_file.parent.name
                    self.manifests[device] = manifest
                    print(f"Loaded OTA manifest for {device}")
            except Exception as e:
                print(f"Error loading {manifest_file}: {e}")
    
    def get_manifest(self, device):
        return self.manifests.get(device)
    
    def get_firmware_path(self, device):
        manifest = self.manifests.get(device)
        if not manifest:
            return None
        url = manifest.get('url', '')
        # Extract filename from URL
        filename = url.split('/')[-1]
        path = self.firmware_dir / device / filename
        return path if path.exists() else None


def parse_telemetry_packet(payload):
    """Parse vfd_telemetry_t struct (34 bytes)."""
    if len(payload) < 34:
        return None
    
    # Unpack packed struct (little-endian)
    fmt = '<BBHHhhHhHHhHHHBI'
    fields = struct.unpack(fmt, payload[:34])
    
    return {
        'machine_state': fields[0],
        'machine_state_str': MACHINE_STATES.get(fields[0], "UNKNOWN"),
        'alarm': fields[1],
        'alarm_str': EVENT_CODES.get(fields[1], "UNKNOWN"),
        'fault_code_main': fields[2],
        'fault_code_travel': fields[3],
        'main_torque_pct': fields[4] / 100.0,  # 0.01% units
        'main_torque_set': fields[5] / 100.0,
        'main_freq_hz': fields[6] / 100.0,  # 0.01 Hz units
        'main_current_a': fields[7] / 10.0,  # 0.1 A units
        'main_bus_v': fields[8],
        'travel_torque_pct': fields[9] / 100.0,
        'travel_torque_set': fields[10] / 100.0,
        'travel_freq_hz': fields[11] / 100.0,
        'travel_current_a': fields[12] / 10.0,
        'wire_tension_raw': fields[13],
        'cut_depth_mm': fields[14] / 10.0,  # 0.1 mm units
        'link_quality': fields[15],
        'uptime_ms': fields[16],
    }


def parse_vfd_packet(data):
    """Parse a VFD protocol packet."""
    if len(data) < 12:  # Minimum header size
        return None, None
    
    # Parse header (12 bytes)
    header_fmt = '<IHBBB BBH'
    magic, version, pkt_type, src, dst, seq, payload_len, crc16 = \
        struct.unpack(header_fmt, data[:12])
    
    # Validate magic
    if magic != VFD_PROTO_MAGIC:
        print(f"Invalid magic: 0x{magic:08X} (expected 0x{VFD_PROTO_MAGIC:08X})")
        return None, None
    
    # Validate CRC
    crc_data = data[:10] + data[12:]  # Header (excl CRC) + payload
    calculated_crc = crc16_modbus(crc_data)
    if calculated_crc != crc16:
        print(f"CRC mismatch: calculated 0x{calculated_crc:04X}, received 0x{crc16:04X}")
        return None, None
    
    # Extract payload
    payload = data[12:12+payload_len]
    
    header = {
        'magic': magic,
        'version': version,
        'type': pkt_type,
        'src': src,
        'dst': dst,
        'seq': seq,
        'len': payload_len,
        'crc16': crc16,
    }
    
    return header, payload


def udp_receiver(port, telemetry_data):
    """UDP server to receive telemetry packets."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', port))
    print(f"UDP receiver listening on port {port}")
    
    packet_count = 0
    last_print = time.time()
    
    while True:
        try:
            data, addr = sock.recvfrom(512)
            
            header, payload = parse_vfd_packet(data)
            if not header:
                continue
            
            if header['type'] == VFD_PKT_TELEMETRY:
                telemetry = parse_telemetry_packet(payload)
                if telemetry:
                    telemetry_data.update(telemetry)
                    packet_count += 1
                    
                    # Print stats every 5 seconds
                    now = time.time()
                    if now - last_print >= 5.0:
                        print(f"UDP: {packet_count} packets in last 5s "
                              f"({packet_count/5.0:.1f} Hz) from {addr[0]}:{addr[1]}")
                        packet_count = 0
                        last_print = now
            
        except Exception as e:
            print(f"UDP receive error: {e}")


# Global server instance — set by VFDServer.start(), read by request handler.
_server_instance = None


class VFDRequestHandler(BaseHTTPRequestHandler):
    """HTTP request handler for VFD server."""
    
    def log_message(self, format, *args):
        """Override to use print instead of stderr."""
        print(f"HTTP: {args[0]}")
    
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        
        if path == '/':
            self.serve_dashboard()
        elif path == '/api/status':
            self.serve_status()
        elif path == '/api/history':
            self.serve_history(parsed)
        elif path == '/manifest.json':
            self.serve_manifest('vfd_controller')
        elif path.startswith('/ota/') and path.endswith('/manifest.json'):
            device = path.split('/')[2]
            self.serve_manifest(device)
        elif path.startswith('/fw/') or path.endswith('.bin'):
            self.serve_firmware(path)
        else:
            self.send_error(404, "Not found")
    
    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        
        if path == '/upload':
            self.handle_upload()
        else:
            self.send_error(404, "Not found")
    
    def serve_dashboard(self):
        """Serve the web dashboard HTML."""
        html_path = Path(__file__).parent / 'dashboard.html'
        try:
            with open(html_path, 'rb') as f:
                content = f.read()
            self.send_response(200)
            self.send_header('Content-type', 'text/html')
            self.send_header('Content-length', len(content))
            self.end_headers()
            self.wfile.write(content)
        except FileNotFoundError:
            self.send_error(500, "Dashboard HTML not found")
    
    def serve_status(self):
        """Serve current telemetry status as JSON."""
        telemetry, last_update = _server_instance.telemetry_data.get_latest()
        
        if telemetry:
            response = {
                'status': 'online',
                'last_update': last_update,
                'age_seconds': time.time() - last_update,
                'telemetry': telemetry,
            }
        else:
            response = {
                'status': 'offline',
                'last_update': 0,
                'age_seconds': None,
                'telemetry': None,
            }
        
        content = json.dumps(response, indent=2).encode()
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.send_header('Content-length', len(content))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(content)
    
    def serve_history(self, parsed):
        """Serve telemetry history as JSON."""
        params = parse_qs(parsed.query)
        seconds = int(params.get('seconds', [60])[0])
        
        history = _server_instance.telemetry_data.get_history(seconds)
        
        response = {
            'count': len(history),
            'seconds': seconds,
            'history': history,
        }
        
        content = json.dumps(response, indent=2).encode()
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.send_header('Content-length', len(content))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(content)
    
    def serve_manifest(self, device):
        """Serve OTA manifest for a device."""
        manifest = _server_instance.ota_manager.get_manifest(device)
        
        if not manifest:
            self.send_error(404, f"No manifest for device '{device}'")
            return
        
        content = json.dumps(manifest, indent=2).encode()
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.send_header('Content-length', len(content))
        self.end_headers()
        self.wfile.write(content)
    
    def serve_firmware(self, path):
        """Serve firmware binary files."""
        # Extract device and filename from path
        if path.startswith('/fw/'):
            parts = path[4:].split('/')
            if len(parts) == 2:
                device, filename = parts
                fw_path = server.ota_manager.firmware_dir / device / filename
            else:
                self.send_error(400, "Invalid firmware path")
                return
        else:
            # Try to find firmware in any device directory
            filename = path.split('/')[-1]
            fw_path = None
            for device_dir in _server_instance.ota_manager.firmware_dir.iterdir():
                candidate = device_dir / filename
                if candidate.exists():
                    fw_path = candidate
                    break
        
        if not fw_path or not fw_path.exists():
            self.send_error(404, "Firmware file not found")
            return
        
        try:
            with open(fw_path, 'rb') as f:
                content = f.read()
            
            self.send_response(200)
            self.send_header('Content-type', 'application/octet-stream')
            self.send_header('Content-length', len(content))
            self.end_headers()
            self.wfile.write(content)
            print(f"Served firmware: {fw_path} ({len(content)} bytes)")
        except Exception as e:
            print(f"Error serving firmware: {e}")
            self.send_error(500, "Error reading firmware file")
    
    def handle_upload(self):
        """Handle log data upload from controller."""
        try:
            content_length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(content_length)
            
            data = json.loads(body.decode())
            device = data.get('device', 'unknown')
            rows = data.get('rows', [])
            
            if not rows:
                self.send_error(400, "No rows in upload")
                return
            
            filename = _server_instance.upload_storage.store(device, rows)
            
            response = {
                'status': 'ok',
                'rows_received': len(rows),
                'filename': filename,
            }
            
            content = json.dumps(response).encode()
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.send_header('Content-length', len(content))
            self.end_headers()
            self.wfile.write(content)
            
            print(f"Upload: {len(rows)} rows from {device} -> {filename}")
            
        except Exception as e:
            print(f"Upload error: {e}")
            self.send_error(400, f"Upload failed: {e}")


class VFDServer:
    """Main server orchestrator."""
    
    def __init__(self, udp_port, http_port, data_dir):
        self.udp_port = udp_port
        self.http_port = http_port
        self.data_dir = Path(data_dir)
        
        self.telemetry_data = TelemetryData()
        self.upload_storage = UploadStorage(self.data_dir / 'uploads')
        self.ota_manager = OTAManager(self.data_dir / 'firmware')
        
    def start(self):
        """Start UDP receiver and HTTP server."""
        global _server_instance
        _server_instance = self

        # Start UDP receiver in background thread
        udp_thread = threading.Thread(
            target=udp_receiver,
            args=(self.udp_port, self.telemetry_data),
            daemon=True
        )
        udp_thread.start()
        print(f"UDP receiver started on port {self.udp_port}")
        
        # Start HTTP server
        http_server = HTTPServer(('0.0.0.0', self.http_port), VFDRequestHandler)
        print(f"HTTP server listening on port {self.http_port}")
        print(f"Dashboard: http://192.168.1.101:{self.http_port}/")
        print(f"API status: http://192.168.1.101:{self.http_port}/api/status")
        print(f"Upload endpoint: http://192.168.1.101:{self.http_port}/upload")
        print(f"OTA manifest: http://192.168.1.101:{self.http_port}/manifest.json")
        
        try:
            http_server.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down server...")
            http_server.shutdown()


def main():
    parser = argparse.ArgumentParser(description='VFD Controller Server')
    parser.add_argument('--udp-port', type=int, default=5005,
                       help='UDP port for telemetry (default: 5005)')
    parser.add_argument('--http-port', type=int, default=8080,
                       help='HTTP port for dashboard/API (default: 8080)')
    parser.add_argument('--data-dir', default='./data',
                       help='Data directory for uploads/firmware (default: ./data)')
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("VFD Controller Server")
    print("=" * 60)
    print(f"UDP port: {args.udp_port}")
    print(f"HTTP port: {args.http_port}")
    print(f"Data directory: {args.data_dir}")
    print("=" * 60)
    
    server = VFDServer(args.udp_port, args.http_port, args.data_dir)
    server.start()


if __name__ == '__main__':
    main()
