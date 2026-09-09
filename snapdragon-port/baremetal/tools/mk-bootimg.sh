#!/bin/sh
# mk-bootimg.sh — pack the gen4 payload into an Android boot image that the
# Fossil Gen 4's aboot will load, using the parameters recovered from the
# AsteroidOS img_info (see ../../HARDWARE.md).
#
# Uses the bundled mkbootimg_v0.py (python3, no dependencies).
# The payload self-relocates, so aboot's exact load address doesn't matter.
# DTB note: aboot on msm8909 REQUIRES a DTB appended to the kernel (zImage-dtb
# convention) to pick by board-id — confirmed on hardware 2026-07-28: without
# one, `fastboot boot` fails immediately with "dtb not found" (nothing runs, so
# it is a safe, non-destructive rejection). The stock DTB dumped from the watch
# (tools/extract-dtb.py boot.img) is appended VERBATIM to our payload so aboot's
# board-id match succeeds. Pass its path as $2 (or set DTB=...).
#
# Usage: ./mk-bootimg.sh [payload.bin] [dtb]   ->  build/gen4/owf-boot.img
#   e.g. ./mk-bootimg.sh build/gen4/owf.bin ~/firefish-dt/dtb-out/dtb-00.dtb
set -e
cd "$(dirname "$0")/.."

# The canonical build flow (build-owf-image*.sh, see BUILD-GEN*.md) never
# creates this directory — only build.sh does — so packaging failed on a
# tree that had only ever been built the documented way.
mkdir -p "build/gen4"

# RAW binary, NOT the .gz: aboot does not decompress — it jumps straight to
# byte 0 of the "kernel". (The stock zImage only works gzipped because a zImage
# carries its own decompressor stub; our payload doesn't, and at ~3 KB it
# doesn't need one.)
PAYLOAD="${1:-build/gen4/owf.bin}"
DTB="${2:-${DTB:-}}"
OUT="build/gen4/owf-boot.img"

[ -f "$PAYLOAD" ] || { echo "payload missing: $PAYLOAD (run: make PLATFORM=gen4)"; exit 1; }

# Append the stock DTB to the payload (zImage-dtb convention). aboot scans past
# the kernel for the FDT magic and matches board-id. Our payload self-relocates,
# so the DTB simply riding along after it does not disturb execution.
KERNEL="$PAYLOAD"
if [ -n "$DTB" ]; then
  [ -f "$DTB" ] || { echo "dtb missing: $DTB"; exit 1; }
  KERNEL="build/gen4/owf-with-dtb.bin"
  cat "$PAYLOAD" "$DTB" > "$KERNEL"
  echo "appended DTB $DTB -> $KERNEL ($(stat -c%s "$KERNEL") bytes)"
else
  echo "WARNING: no DTB appended. aboot will reject with 'dtb not found'."
  echo "  pass the stock DTB: ./mk-bootimg.sh $PAYLOAD path/to/dtb-00.dtb"
fi

# minimal empty ramdisk (aboot checks the field exists, not its contents)
printf '' | gzip > build/gen4/empty-ramdisk.gz

# base = 0x80000000 (DDR base of this SoC). CORRECTED 2026-07-28: the stock
# boot.img dumped from the watch uses kernel_addr=0x80008000 / ramdisk=0x82000000
# / tags=0x81e00000 — i.e. base 0x80000000, NOT the 0x0 the AsteroidOS img_info
# implied. With base 0x0 aboot loaded to physical 0x8000 (below DDR, which starts
# at 0x80000000) and never ran; the appended DTB scan hit stale memory, giving
# the alternating "dtb not found" / "No such device" failures. Match the stock
# image's absolute load addresses exactly.
python tools/mkbootimg_v0.py \
  --kernel "$KERNEL" \
  --ramdisk build/gen4/empty-ramdisk.gz \
  --pagesize 2048 \
  --base 0x80000000 \
  --kernel_offset 0x00008000 \
  --ramdisk_offset 0x02000000 \
  --tags_offset 0x01e00000 \
  --cmdline "owf_baremetal=1" \
  -o "$OUT"

echo "wrote $OUT"
echo "test on watch (RAM only, nothing flashed):  fastboot boot $OUT"
