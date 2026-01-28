#!/bin/bash
# Debug script: run QEMU with -d int to log all interrupts
# Interrupt log goes to /tmp/qemu_int.log, serial output to stdout
rm -f /tmp/qemu_int.log

qemu-system-x86_64 \
  -d cpu_reset,int -D /tmp/qemu_int.log \
  -smp cpus=1 -M q35 -m 2G \
  -serial stdio -action panic=none \
  -netdev user,id=net0 -device e1000e,netdev=net0 \
  -display none \
  -bios /usr/share/ovmf/OVMF.fd \
  -cdrom cottage.iso -boot d &

QEMU_PID=$!
sleep 5
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

echo "---"
echo "Interrupt log: $(wc -c < /tmp/qemu_int.log) bytes, $(wc -l < /tmp/qemu_int.log) lines"
echo "Last 50 lines:"
tail -50 /tmp/qemu_int.log
