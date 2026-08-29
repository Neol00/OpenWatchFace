#!/bin/bash
# qemu-screenshot.sh — boot the qemu build headless, wait, screendump to PNG.
# Runs inside WSL/Linux. Usage: tools/qemu-screenshot.sh [wait_seconds] [out.png]
#
# Uses QMP (machine protocol), NOT the human monitor. The human monitor echoes
# every keystroke back with terminal escapes and only acts once it has processed
# the line, so a "send then sleep then quit" script races it and silently
# produces NO FILE while exiting 0. QMP is request/response with explicit
# {"return": ...} acks, so we wait for the screendump to actually complete.
set -e
cd "$(dirname "$0")/.."

WAIT="${1:-3}"
OUT="${2:-build/qemu/shot.png}"
SOCK="/tmp/owf-qmp-$$.sock"

mkdir -p "$(dirname "$OUT")"
rm -f "$SOCK" "$OUT"

qemu-system-arm -M virt,highmem=off -cpu cortex-a7 -m 512M -display none \
    -device ramfb -serial file:build/qemu/serial.log \
    -qmp "unix:$SOCK,server,nowait" -kernel build/qemu/owf.elf &
QPID=$!
trap 'kill $QPID 2>/dev/null; rm -f "$SOCK"' EXIT

# Wait for the socket rather than assuming it appears instantly.
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done

sleep "$WAIT"

# screendump needs an ABSOLUTE path: qemu's cwd is not necessarily ours.
case "$OUT" in /*) ABS="$OUT" ;; *) ABS="$PWD/$OUT" ;; esac

python3 - "$SOCK" "$ABS" <<'EOF'
import json, socket, sys

sock, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
s.settimeout(15)
f = s.makefile('rw', encoding='utf-8', newline='\n')


def rpc(cmd, **args):
    """Send one QMP command, return its reply, skipping async events."""
    msg = {"execute": cmd}
    if args:
        msg["arguments"] = args
    f.write(json.dumps(msg) + "\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise RuntimeError("QMP closed while waiting for %s" % cmd)
        reply = json.loads(line)
        if "event" in reply:      # async event, not our answer
            continue
        return reply


f.readline()          # greeting
rpc("qmp_capabilities")

# format=png is REQUIRED: without it qemu writes a PPM regardless of the
# filename extension, which every image viewer then rejects.
reply = rpc("screendump", filename=out, format="png")
if "error" in reply:
    # Older qemu builds lack the format argument; fall back to a PPM and
    # let the caller convert, rather than silently emitting a mislabelled file.
    reply = rpc("screendump", filename=out)
    if "error" in reply:
        print("screendump failed:", reply["error"], file=sys.stderr)
        sys.exit(1)
    print("warning: qemu ignored format=png; %s is a PPM" % out, file=sys.stderr)

# The ack means the PNG is fully written; safe to quit now.
try:
    rpc("quit")
except Exception:
    pass
EOF

wait $QPID 2>/dev/null || true
ls -la "$OUT"
