#!/bin/bash
# Reset TAP interface (kills stuck QEMU and recreates tap0)
# Usage: sudo ./scripts/reset-tap.sh

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Re-running with sudo..."
    exec sudo "$0" "$@"
fi

echo "Killing any stuck QEMU processes..."
pkill -9 qemu-system-x86 2>/dev/null || true
sleep 1

echo "Tearing down tap0..."
ip link set tap0 down 2>/dev/null || true
ip tuntap del dev tap0 mode tap 2>/dev/null || true

echo "Recreating tap0..."
exec ./scripts/setup-tap.sh
