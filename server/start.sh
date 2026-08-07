#!/bin/bash
# Quick start script for VFD Controller Server

echo "============================================================"
echo "VFD Controller Server - Quick Start"
echo "============================================================"
echo ""
echo "Starting server on:"
echo "  Dashboard:  http://192.168.1.101:8080/"
echo "  API:        http://192.168.1.101:8080/api/status"
echo "  UDP Port:   5005 (telemetry receiver)"
echo ""
echo "Press Ctrl+C to stop the server"
echo "============================================================"
echo ""

cd "$(dirname "$0")"
python3 vfd_server.py --udp-port 5005 --http-port 8080
