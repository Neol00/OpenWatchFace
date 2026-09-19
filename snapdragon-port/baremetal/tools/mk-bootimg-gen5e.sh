#!/bin/sh
# mk-bootimg-gen5.sh — pack the Gen 5 (triggerfish, Wear 3100) payload into an
# Android boot image that this watch's aboot will accept.
#
# THE DTB IS NOT OPTIONAL AND IT IS NOT THE GEN 4's. aboot on msm8909 matches an
# appended DTB on qcom,msm-id + qcom,board-id + qcom,pmic-id and rejects the
# image outright otherwise -- "dtb not found", before a single instruction runs.
# We proved that on this exact watch on 2026-09-10: twrp-firefish.img was
# rejected, and so was a 40-tree sweep, because triggerfish differs from
# firefish on TWO of the three ids at once:
#
#            firefish (Gen 4)          triggerfish (Gen 5)
#   msm-id   <0x109 0 0x12d 0>         <0x109 0 0x12d 0>        same
#   board-id <0x08 0x105>              <0x08 0x15>              DIFFERENT
#   pmic-id  <0x1000b 0 0 0>           <0x1001b 0 0 0 ...>      DIFFERENT (PM660)
#
# So the default DTB here is the one dumped from THIS watch's own boot
# partition (../dtbs/sole-stock.dtb, byte-identical to the copy in its
# recovery partition). Do not substitute the Gen 4's.
#
# Load addresses are byte-identical to the Gen 4's, read back from the stock
# triggerfish boot.img: kernel 0x80008000, ramdisk 0x82000000, tags 0x81e00000,
# pagesize 2048 -> base 0x80000000.
#
# Usage: ./mk-bootimg-gen5.sh [payload.bin] [dtb]  ->  build/gen5e/owf-boot.img
set -e
cd "$(dirname "$0")/.."

mkdir -p "build/gen5"

PAYLOAD="${1:-build/gen5e/owf.bin}"
DTB="${2:-${DTB:-../dtbs/sole-stock.dtb}}"
OUT="build/gen5e/owf-boot.img"

[ -f "$PAYLOAD" ] || { echo "payload missing: $PAYLOAD (run: ./build.sh gen5)"; exit 1; }
[ -f "$DTB" ]     || { echo "dtb missing: $DTB"; exit 1; }

# RAW binary, not gzipped: aboot jumps straight to byte 0 of the "kernel".
KERNEL="build/gen5e/owf-with-dtb.bin"
cat "$PAYLOAD" "$DTB" > "$KERNEL"
echo "appended DTB $DTB -> $KERNEL ($(stat -c%s "$KERNEL") bytes)"

# Sanity: refuse to ship an image carrying the WRONG watch's board-id. A silent
# mismatch here costs a full flash-and-test cycle to discover.
if command -v fdtget >/dev/null 2>&1; then
  bid=$(fdtget -t x "$DTB" / qcom,board-id 2>/dev/null || echo "")
  case "$bid" in
    *" 1e"|*" 0x1e") : ;;
    "") echo "WARNING: could not read qcom,board-id from $DTB" ;;
    *)  echo "REFUSING: $DTB has board-id '$bid', expected '8 1e' (sole)"; exit 1 ;;
  esac
fi

printf '' | gzip > build/gen5e/empty-ramdisk.gz

python3 tools/mkbootimg_v0.py \
  --kernel "$KERNEL" \
  --ramdisk build/gen5e/empty-ramdisk.gz \
  --pagesize 2048 \
  --base 0x80000000 \
  --kernel_offset 0x00008000 \
  --ramdisk_offset 0x02000000 \
  --tags_offset 0x01e00000 \
  --cmdline "owf_baremetal=1" \
  -o "$OUT"

echo "wrote $OUT"
echo "test on watch (RAM only, nothing flashed):  fastboot boot $OUT"
