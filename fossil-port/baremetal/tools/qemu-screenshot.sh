#!/bin/bash
# qemu-screenshot.sh — boot the qemu build headless, wait, screendump to PNG.
# Runs inside WSL/Linux. Usage: tools/qemu-screenshot.sh [wait_seconds] [out.png]
set -e
cd "$(dirname "$0")/.."

WAIT="${1:-3}"
OUT="${2:-build/qemu/shot.png}"
SOCK=/tmp/owf-mon-$$.sock

qemu-system-arm -M virt,highmem=off -cpu cortex-a7 -m 512M -display none \
    -device ramfb -serial file:build/qemu/serial.log \
    -monitor unix:$SOCK,server,nowait -kernel build/qemu/owf.elf &
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT
sleep "$WAIT"

python3 - "$SOCK" "$PWD/$OUT" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.settimeout(1)
try: s.recv(4096)
except socket.timeout: pass
s.sendall(f"screendump {sys.argv[2]} format=png\n".encode())
time.sleep(1)
s.sendall(b"quit\n")
time.sleep(0.5)
EOF
wait $QPID 2>/dev/null || true
ls -la "$OUT"
