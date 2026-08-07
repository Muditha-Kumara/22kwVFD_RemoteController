#!/usr/bin/env python3
"""
Generate OTA manifest for VFD firmware updates.

Usage:
    python3 generate_manifest.py <firmware.bin> <version> [--device DEVICE] [--server-ip IP]

Example:
    python3 generate_manifest.py firmware.bin 2 --device vfd_controller --server-ip 192.168.1.101
"""

import argparse
import json
import zlib
from pathlib import Path

VFD_PROTO_MAGIC = 0x224B5746  # 575362886


def calculate_crc32(filepath):
    """Calculate CRC32 of a file."""
    with open(filepath, 'rb') as f:
        data = f.read()
    return zlib.crc32(data) & 0xffffffff


def generate_manifest(firmware_path, version, device, server_ip, http_port):
    """Generate OTA manifest JSON."""
    firmware_path = Path(firmware_path)
    
    if not firmware_path.exists():
        raise FileNotFoundError(f"Firmware file not found: {firmware_path}")
    
    # Calculate file size and CRC32
    size = firmware_path.stat().st_size
    crc32 = calculate_crc32(firmware_path)
    
    # Build firmware URL
    filename = firmware_path.name
    url = f"http://{server_ip}:{http_port}/fw/{device}/{filename}"
    
    manifest = {
        "magic": VFD_PROTO_MAGIC,
        "version": version,
        "url": url,
        "size": size,
        "crc32": crc32
    }
    
    return manifest


def main():
    parser = argparse.ArgumentParser(
        description='Generate OTA manifest for VFD firmware',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Example:
  python3 generate_manifest.py build/vfd_controller.bin 2 \\
      --device vfd_controller --server-ip 192.168.1.101

This creates data/firmware/vfd_controller/manifest.json with:
  - magic: 575362886 (0x224B5746)
  - version: 2
  - url: http://192.168.1.101:8080/fw/vfd_controller/vfd_controller.bin
  - size: <calculated>
  - crc32: <calculated>
        """
    )
    
    parser.add_argument('firmware', help='Path to firmware binary file')
    parser.add_argument('version', type=int, help='Firmware version (must be > current APP_FW_VERSION)')
    parser.add_argument('--device', default='vfd_controller',
                       choices=['vfd_controller', 'remote_control'],
                       help='Device type (default: vfd_controller)')
    parser.add_argument('--server-ip', default='192.168.1.101',
                       help='Server IP address (default: 192.168.1.101)')
    parser.add_argument('--http-port', type=int, default=8080,
                       help='HTTP port (default: 8080)')
    parser.add_argument('--output', '-o', help='Output manifest path (default: auto)')
    
    args = parser.parse_args()
    
    try:
        # Generate manifest
        manifest = generate_manifest(
            args.firmware,
            args.version,
            args.device,
            args.server_ip,
            args.http_port
        )
        
        # Determine output path
        if args.output:
            output_path = Path(args.output)
        else:
            output_path = Path('data/firmware') / args.device / 'manifest.json'
        
        # Create directory and write manifest
        output_path.parent.mkdir(parents=True, exist_ok=True)
        
        with open(output_path, 'w') as f:
            json.dump(manifest, f, indent=2)
        
        # Copy firmware to data directory
        firmware_dest = output_path.parent / Path(args.firmware).name
        if not firmware_dest.exists():
            import shutil
            shutil.copy2(args.firmware, firmware_dest)
            print(f"Copied firmware to: {firmware_dest}")
        
        # Print summary
        print("\n" + "=" * 60)
        print("OTA Manifest Generated")
        print("=" * 60)
        print(f"Device:      {args.device}")
        print(f"Version:     {args.version}")
        print(f"Firmware:    {args.firmware}")
        print(f"Size:        {manifest['size']} bytes")
        print(f"CRC32:       0x{manifest['crc32']:08X} ({manifest['crc32']})")
        print(f"Magic:       0x{manifest['magic']:08X} ({manifest['magic']})")
        print(f"URL:         {manifest['url']}")
        print(f"Manifest:    {output_path}")
        print("=" * 60)
        print("\nThe controller will check this manifest at startup.")
        print(f"Manifest URL: http://{args.server_ip}:{args.http_port}/manifest.json")
        
    except FileNotFoundError as e:
        print(f"Error: {e}")
        return 1
    except Exception as e:
        print(f"Error generating manifest: {e}")
        return 1
    
    return 0


if __name__ == '__main__':
    exit(main())
