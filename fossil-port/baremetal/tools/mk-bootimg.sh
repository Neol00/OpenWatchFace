#!/bin/sh
# mk-bootimg.sh — pack the gen4 payload into an Android boot image that the
# Fossil Gen 4's aboot will load, using the parameters recovered from the
# AsteroidOS img_info (see ../../HARDWARE.md).
#
# Uses the bundled mkbootimg_v0.py (python3, no dependencies).
# The payload self-relocates, so aboot's exact load address doesn't matter.
# DTB note: aboot on msm8909 wants a DTB appended to the kernel (zImage-dtb
# convention) to pick by board-id. Until the stock DTB is dumped from the
# watch, we append nothing — first hardware test will tell if aboot refuses
# the image without one (then: append the stock boot.img's DTB verbatim).
#
# Usage: ./mk-bootimg.sh [payload.gz]  ->  build/gen4/owf-boot.img
set -e
cd "$(dirname "$0")/.."

# RAW binary, NOT the .gz: aboot does not decompress — it jumps straight to
# byte 0 of the "kernel". (The stock zImage only works gzipped because a zImage
# carries its own decompressor stub; our payload doesn't, and at ~3 KB it
# doesn't need one.)
PAYLOAD="${1:-build/gen4/owf.bin}"
OUT="build/gen4/owf-boot.img"

[ -f "$PAYLOAD" ] || { echo "payload missing: $PAYLOAD (run: make PLATFORM=gen4)"; exit 1; }

# minimal empty ramdisk (aboot checks the field exists, not its contents)
printf '' | gzip > build/gen4/empty-ramdisk.gz

python tools/mkbootimg_v0.py \
  --kernel "$PAYLOAD" \
  --ramdisk build/gen4/empty-ramdisk.gz \
  --pagesize 2048 \
  --base 0x00000000 \
  --kernel_offset 0x00008000 \
  --ramdisk_offset 0x02000000 \
  --tags_offset 0x01e00000 \
  --cmdline "owf_baremetal=1" \
  -o "$OUT"

echo "wrote $OUT"
echo "test on watch (RAM only, nothing flashed):  fastboot boot $OUT"
