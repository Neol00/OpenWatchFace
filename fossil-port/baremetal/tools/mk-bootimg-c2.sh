#!/bin/sh
# mk-bootimg-c2.sh — pack the c2 payload into an Android boot image that the
# TicWatch C2's aboot will load.
#
# Same SoC as the Fossil Gen 4 (APQ8009W), so the same load addresses apply and
# this is mk-bootimg.sh with the paths and the DTB changed.
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
# Usage: ./mk-bootimg.sh [payload.bin] [dtb]   ->  build/c2/owf-boot.img
#   e.g. ./mk-bootimg.sh build/c2/owf.bin ~/firefish-dt/dtb-out/dtb-00.dtb
set -e
cd "$(dirname "$0")/.."

# The canonical build flow (build-owf-image*.sh, see BUILD-GEN*.md) never
# creates this directory — only build.sh does — so packaging failed on a
# tree that had only ever been built the documented way.
mkdir -p "build/c2"

# RAW binary, NOT the .gz: aboot does not decompress — it jumps straight to
# byte 0 of the "kernel". (The stock zImage only works gzipped because a zImage
# carries its own decompressor stub; our payload doesn't, and at ~3 KB it
# doesn't need one.)
PAYLOAD="${1:-build/c2/owf.bin}"
DTB="${2:-${DTB:-../dumps/c2-skipjack-fromsource/skipjack.dtb}}"
OUT="build/c2/owf-boot.img"

[ -f "$PAYLOAD" ] || { echo "payload missing: $PAYLOAD (run: ./build.sh c2)"; exit 1; }

# Append the stock DTB to the payload (zImage-dtb convention). aboot scans past
# the kernel for the FDT magic and matches board-id. Our payload self-relocates,
# so the DTB simply riding along after it does not disturb execution.
KERNEL="$PAYLOAD"
if [ -n "$DTB" ]; then
  [ -f "$DTB" ] || { echo "dtb missing: $DTB"; exit 1; }
  KERNEL="build/c2/owf-with-dtb.bin"
  cat "$PAYLOAD" "$DTB" > "$KERNEL"
  echo "appended DTB $DTB -> $KERNEL ($(stat -c%s "$KERNEL") bytes)"
else
  echo "WARNING: no DTB appended. aboot will reject with 'dtb not found'."
  echo "  pass the stock DTB: ./mk-bootimg.sh $PAYLOAD path/to/dtb-00.dtb"
fi

# minimal empty ramdisk (aboot checks the field exists, not its contents)
printf '' | gzip > build/c2/empty-ramdisk.gz

# base = 0x80000000 (DDR base of this SoC), with the Gen 4's absolute load
# addresses: kernel 0x80008000 / ramdisk 0x82000000 / tags 0x81e00000. Those
# were recovered from a stock Fossil boot.img and are reused here because the
# SoC and its DDR base are the same. UNVERIFIED against a stock C2 boot image —
# if the watch takes the image but never draws, this is the first thing to
# check against the real header (TWRP + tools/extract-dtb.py).
python tools/mkbootimg_v0.py \
  --kernel "$KERNEL" \
  --ramdisk build/c2/empty-ramdisk.gz \
  --pagesize 2048 \
  --base 0x80000000 \
  --kernel_offset 0x00008000 \
  --ramdisk_offset 0x02000000 \
  --tags_offset 0x01e00000 \
  --cmdline "owf_baremetal=1" \
  -o "$OUT"

echo "wrote $OUT"
echo
echo "test on watch (RAM only, NOTHING is flashed):"
echo "    fastboot boot $OUT"
echo
echo "expected: the OpenWatchFace watch face on the glass. A blank screen that"
echo "stays blank means the payload ran but the display takeover failed; an"
echo "immediate reboot means it never ran at all. Either way a power cycle"
echo "restores the watch — this image is never written to any partition."
