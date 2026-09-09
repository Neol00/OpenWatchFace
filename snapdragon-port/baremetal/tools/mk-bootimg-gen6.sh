#!/bin/sh
# mk-bootimg-gen6.sh — pack the gen6 payload into an Android boot image for the
# Fossil Gen 6 (hoki, SDA429W).
#
# Parameters are the stock ones, taken from AsteroidOS meta-hoki's img_info
# (which was derived from a real hoki boot.img):
#     page_size      = 4096          <- NOT 2048 like the Gen 4
#     base_addr      = 0x80000000
#     kernel_offset  = 0x00008000    -> kernel loads at 0x80008000
#     ramdisk_offset = 0x01000000
#     tags_offset    = 0x00000100
#
# DTB: the Gen 4 needs an appended DTB or aboot rejects the image with
# "dtb not found". Whether hoki's bootloader does the same is UNVERIFIED — its
# 4.14-era chain may take the DTB from a separate `dtbo`/`dtb` partition
# instead. So the DTB argument here is OPTIONAL:
#   - with no DTB, we try the simple case first (fastest thing to test)
#   - if the flashed image fails with a dtb complaint, re-pack passing the
#     decompiled hoki DTB (../sda429-hoki-decompiled.dts recompiled with dtc,
#     or better: the real dtb dumped from your watch).
# SETTLED: pass ../sda429-hoki.dtb. A DTB-less image is ~232 KB smaller and
# does not work; BUILD-GEN6.md's verifier checks for exactly this.
#
# Usage: ./mk-bootimg-gen6.sh [payload.bin] [dtb]  -> build/gen6/owf-boot.img
set -e
cd "$(dirname "$0")/.."

# The canonical build flow (build-owf-image*.sh, see BUILD-GEN*.md) never
# creates this directory — only build.sh does — so packaging failed on a
# tree that had only ever been built the documented way.
mkdir -p "build/gen6"

# RAW binary, NOT the .gz: aboot jumps straight to byte 0 of the "kernel".
# (A stock zImage only works gzipped because it carries its own decompressor
# stub; our payload doesn't have one and doesn't need it.)
PAYLOAD="${1:-build/gen6/owf.bin}"
DTB="${2:-${DTB:-}}"
OUT="build/gen6/owf-boot.img"

[ -f "$PAYLOAD" ] || { echo "payload missing: $PAYLOAD (run: ./build.sh gen6)"; exit 1; }

KERNEL="$PAYLOAD"
if [ -n "$DTB" ]; then
  [ -f "$DTB" ] || { echo "dtb missing: $DTB"; exit 1; }
  KERNEL="build/gen6/owf-with-dtb.bin"
  cat "$PAYLOAD" "$DTB" > "$KERNEL"
  echo "appended DTB $DTB -> $KERNEL ($(stat -c%s "$KERNEL") bytes)"
else
  echo "note: no DTB appended (trying the simple case first)."
  echo "  if fastboot rejects this with a dtb error, re-run passing a dtb."
fi

# minimal empty ramdisk (the header field must exist; contents are unused)
printf '' | gzip > build/gen6/empty-ramdisk.gz

python tools/mkbootimg_v0.py \
  --kernel "$KERNEL" \
  --ramdisk build/gen6/empty-ramdisk.gz \
  --pagesize 4096 \
  --base 0x80000000 \
  --kernel_offset 0x00008000 \
  --ramdisk_offset 0x01000000 \
  --tags_offset 0x00000100 \
  --cmdline "owf_baremetal=1" \
  -o "$OUT"

echo "wrote $OUT"
# `fastboot boot` DOES NOT WORK ON THIS DEVICE — it transfers only a partial
# image and never runs. This line used to suggest it, which is why it kept
# being recommended. The image must be FLASHED. See BUILD-GEN6.md
# "Booting, flashing and recovery".
echo "flash it:  fastboot flash boot $OUT      (or: flash recovery, see BUILD-GEN6.md)"
