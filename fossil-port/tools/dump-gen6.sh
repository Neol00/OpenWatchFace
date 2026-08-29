#!/bin/sh
# dump-gen6.sh — pull everything useful off a Fossil Gen 6 (hoki) into this repo.
#
# RUN THIS ON THE HOST (not the watch), with the watch in a state that gives an
# adb shell — either stock Wear OS, or a Linux image booted via `fastboot boot`.
# It is READ-ONLY on the device: nothing is flashed, erased or written.
#
#   sh fossil-port/tools/dump-gen6.sh [outdir]
#
# Default outdir: fossil-port/dumps/gen6-<date>  (pass one to override; the
# date is taken from the host so repeat runs do not overwrite each other).
#
# WHY THESE ARTIFACTS, in value order:
#   1. /proc/device-tree — the MERGED tree (base DTB + dtbo overlay). This is
#      the single highest-value artifact: it contains the panel node (real
#      resolution + DSI init command tables) and the touch node (I2C bus,
#      address, IRQ GPIO) that the published AsteroidOS DTB is missing.
#   2. dtbo partition — the raw overlay, in case /proc/device-tree is blocked.
#   3. /sys/bus/i2c + /sys/class — which drivers bound to which bus, live.
#   4. kernel cmdline/config, /proc/iomem — address map confirmation.
set -e

OUT="${1:-$(dirname "$0")/../dumps/gen6-$(date +%Y%m%d)}"
mkdir -p "$OUT"
echo "[dump] target: $OUT"

have() { adb shell "command -v $1" >/dev/null 2>&1; }
pull() { echo "  <- $1"; adb pull "$1" "$OUT/" >/dev/null 2>&1 || echo "     (skipped: $1)"; }
cap()  { echo "  <- $1 (as $2)"; adb shell "cat $1" > "$OUT/$2" 2>/dev/null || echo "     (skipped: $1)"; }

echo "[dump] waiting for a device (run this once adb sees the watch)..."
adb wait-for-device

echo "[dump] 1/4 merged device tree (the big one)"
adb pull /proc/device-tree "$OUT/device-tree" >/dev/null 2>&1 \
  || echo "     (blocked — rely on the dtbo partition below)"

echo "[dump] 2/4 kernel + platform info"
cap /proc/cmdline        cmdline.txt
cap /proc/iomem          iomem.txt
cap /proc/interrupts     interrupts.txt
cap /proc/partitions     partitions.txt
cap /proc/version        version.txt
adb shell "zcat /proc/config.gz" > "$OUT/config.gz.txt" 2>/dev/null || true

echo "[dump] 3/4 live driver bindings (touch/panel/sensors)"
adb shell "ls -l /sys/bus/i2c/devices/ 2>/dev/null"      > "$OUT/i2c-devices.txt" 2>/dev/null || true
adb shell "ls -l /sys/bus/spmi/devices/ 2>/dev/null"     > "$OUT/spmi-devices.txt" 2>/dev/null || true
adb shell "ls -l /sys/class/input/ 2>/dev/null"          > "$OUT/input-devices.txt" 2>/dev/null || true
adb shell "ls -l /sys/class/graphics/ 2>/dev/null"       > "$OUT/graphics.txt" 2>/dev/null || true
adb shell "cat /sys/class/graphics/fb0/modes 2>/dev/null; cat /sys/class/graphics/fb0/virtual_size 2>/dev/null" \
                                                          > "$OUT/fb0-mode.txt" 2>/dev/null || true
adb shell "dmesg" > "$OUT/dmesg.txt" 2>/dev/null || true

echo "[dump] 4/4 raw partitions (needs a root adbd; skipped silently without one)"
# The by-name symlink directory moves depending on which image is running:
# stock Wear OS exposes it under the sdhci controller path taken from the hoki
# DTB, while an AsteroidOS recovery ramdisk uses /dev/block/bootdevice. Probe
# the known locations instead of hardcoding one — a wrong path looks exactly
# like "no root", which cost a debugging round on 2026-08-06.
BYNAME=
for cand in /dev/block/bootdevice/by-name \
            /dev/block/platform/soc/7824900.sdhci/by-name \
            /dev/block/platform/7824900.sdhci/by-name; do
  if adb shell "[ -d $cand ] && echo yes" 2>/dev/null | grep -q yes; then BYNAME="$cand"; break; fi
done
if [ -z "$BYNAME" ]; then
  # last resort: let the shell find any by-name dir at all
  BYNAME=$(adb shell "ls -d /dev/block/*/by-name /dev/block/platform/*/by-name \
                          /dev/block/platform/*/*/by-name 2>/dev/null | head -1" \
           2>/dev/null | tr -d '\r')
fi
echo "  by-name: ${BYNAME:-<not found>}"
[ -n "$BYNAME" ] && adb shell "ls -l $BYNAME 2>/dev/null" > "$OUT/partitions-by-name.txt" 2>/dev/null || true
for p in dtbo boot; do
  echo "  <- partition $p"
  adb exec-out "dd if=$BYNAME/$p 2>/dev/null" > "$OUT/$p.img" 2>/dev/null || true
  [ -s "$OUT/$p.img" ] || { rm -f "$OUT/$p.img"; echo "     (not readable at $BYNAME)"; }
done

echo
echo "[dump] done. Contents:"
ls -la "$OUT"
echo
echo "Most valuable: $OUT/device-tree (panel + touch nodes) or $OUT/dtbo.img"
