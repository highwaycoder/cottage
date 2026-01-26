#!/bin/bash
# Setup TAP networking for QEMU guest
# This allows the host to ping the guest directly
#
# Usage: sudo ./scripts/setup-tap.sh [username]
#
# The TAP interface does NOT persist across reboots.
# Run this script after each reboot before using TAP networking.

set -e

TAP_DEV="tap0"
TAP_IP="10.0.2.1/24"
USER="${1:-$SUDO_USER}"

if [ -z "$USER" ]; then
    echo "Error: Could not determine user. Run with sudo or pass username as argument."
    exit 1
fi

if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root (use sudo)"
    exit 1
fi

# Check if tap0 already exists
if ip link show "$TAP_DEV" &>/dev/null; then
    echo "TAP interface $TAP_DEV already exists"
else
    echo "Creating TAP interface $TAP_DEV for user $USER..."
    ip tuntap add dev "$TAP_DEV" mode tap user "$USER"
fi

# Configure IP if not already set
if ! ip addr show "$TAP_DEV" | grep -q "10.0.2.1"; then
    echo "Configuring IP $TAP_IP on $TAP_DEV..."
    ip addr add "$TAP_IP" dev "$TAP_DEV"
fi

# Bring up the interface
echo "Bringing up $TAP_DEV..."
ip link set "$TAP_DEV" up

echo ""
echo "TAP networking ready!"
echo "  Host IP:  10.0.2.1"
echo "  Guest IP: 10.0.2.15 (configured in kernel)"
echo ""
echo "Run QEMU with: make run-uefi TAP=1"
echo "Then from host: ping 10.0.2.15"
