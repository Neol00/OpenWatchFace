#!/bin/sh
# dump-gen4.sh — full backup of a Fossil Gen 4 (firefish) before its stock
# Wear OS install is replaced, plus the usual bring-up artifacts.
#
# RUN THIS ON THE HOST, with the watch showing an adb shell that has ROOT.
# The practical way to get one is to RAM-boot AsteroidOS, which touches no
# partition:
#
#     fastboot boot ~/asteroid-firefish-boot.img
#     adb wait-for-device && adb shell id      # expect uid=0(root)
#     sh fossil-port/tools/dump-gen4.sh
#
# This script is READ-ONLY on the device: it only ever reads block devices.
# Nothing is flashed, erased or written.
#
#   sh fossil-port/tools/dump-gen4.sh [outdir]
#
# Default outdir: fossil-port/dumps/gen4-<date>.
#
# WHAT IT PRODUCES, and why each piece matters:
#   partitions/<name>.img   every partition, byte for byte. This is the
#                           restore set — see RESTORE.md, written alongside.
#   manifest.tsv            name, block device, size, md5 for each image.
#                           The md5s are computed ON THE DEVICE and again on
#                           the host, so a truncated pull cannot pass silently.
#   device-tree/            the live merged tree.
#   *.txt                   cmdline, iomem, interrupts, partition table,
#                           mmc registers, driver bindings, dmesg.
#
# ABOUT userdata: it is by far the largest partition and holds nothing you
# need to rebuild the watch — just the previous owner's app data. It is
# SKIPPED by default because pulling several GB over adb takes hours. Pass
# --with-userdata to include it.
set -e

WITH_USERDATA=0
OUT=
for arg in "$@"; do
  case "$arg" in
    --with-userdata) WITH_USERDATA=1 ;;
    -*) echo "unknown option: $arg" >&2; exit 1 ;;
    *)  OUT="$arg" ;;
  esac
done
OUT="${OUT:-$(dirname "$0")/../dumps/gen4-$(date +%Y%m%d)}"
mkdir -p "$OUT/partitions"
echo "[dump] target: $OUT"

cap() { echo "  <- $1 (as $2)"; adb shell "cat $1" > "$OUT/$2" 2>/dev/null || echo "     (skipped: $1)"; }

echo "[dump] waiting for a device..."
adb wait-for-device

# --- root check ----------------------------------------------------------
# Without root every partition read comes back empty, which looks exactly
# like "this device has no such partition" — a failure mode worth refusing
# outright rather than discovering after a long, silent, useless run.
ID=$(adb shell id 2>/dev/null | tr -d '\r')
echo "[dump] shell identity: $ID"
case "$ID" in
  *uid=0*) ;;
  *) echo
     echo "REFUSING TO CONTINUE: this adb shell is not root, so every"
     echo "partition read would silently produce an empty file."
     echo "RAM-boot AsteroidOS first:  fastboot boot <asteroid-boot.img>"
     exit 1 ;;
esac

echo "[dump] 1/4 merged device tree"
adb pull /proc/device-tree "$OUT/device-tree" >/dev/null 2>&1 \
  || echo "     (blocked)"

echo "[dump] 2/4 kernel + platform info"
cap /proc/cmdline    cmdline.txt
cap /proc/iomem      iomem.txt
cap /proc/interrupts interrupts.txt
cap /proc/partitions partitions.txt
cap /proc/version    version.txt
adb shell "zcat /proc/config.gz" > "$OUT/config.gz.txt" 2>/dev/null || true
adb shell "dmesg"                > "$OUT/dmesg.txt"     2>/dev/null || true
for s in /sys/bus/i2c/devices /sys/bus/spmi/devices /sys/class/input /sys/class/graphics; do
  adb shell "ls -l $s 2>/dev/null" > "$OUT/$(basename $s)-devices.txt" 2>/dev/null || true
done
adb shell "cat /sys/kernel/debug/mmc0/ios 2>/dev/null" > "$OUT/mmc-ios.txt" 2>/dev/null || true

echo "[dump] 3/4 locating the partition table"
# The by-name directory moves depending on which image is running. firefish's
# own fstab (from its DTB) says /dev/block/platform/soc/7824900.sdhci/by-name;
# an AsteroidOS ramdisk usually exposes /dev/block/bootdevice/by-name. Probe
# rather than hardcode: a wrong path is indistinguishable from "no root".
BYNAME=
for cand in /dev/block/bootdevice/by-name \
            /dev/block/platform/soc/7824900.sdhci/by-name \
            /dev/block/platform/7824900.sdhci/by-name; do
  if adb shell "[ -d $cand ] && echo yes" 2>/dev/null | grep -q yes; then BYNAME="$cand"; break; fi
done
if [ -z "$BYNAME" ]; then
  BYNAME=$(adb shell "ls -d /dev/block/*/by-name /dev/block/platform/*/by-name \
                           /dev/block/platform/*/*/by-name 2>/dev/null | head -1" \
           2>/dev/null | tr -d '\r')
fi
if [ -z "$BYNAME" ]; then
  echo "  no by-name directory found — falling back to PARTNAME uevents"
  adb shell "grep -H PARTNAME /sys/block/mmcblk0/mmcblk0p*/uevent 2>/dev/null" \
    > "$OUT/partnames.txt" 2>/dev/null || true
  NAMES=$(sed 's/.*PARTNAME=//' "$OUT/partnames.txt" 2>/dev/null | tr -d '\r')
  DEVDIR=/dev/block
  RESOLVE_BY_UEVENT=1
else
  echo "  by-name: $BYNAME"
  adb shell "ls -l $BYNAME" > "$OUT/partitions-by-name.txt" 2>/dev/null || true
  NAMES=$(adb shell "ls $BYNAME" 2>/dev/null | tr -d '\r')
  DEVDIR="$BYNAME"
  RESOLVE_BY_UEVENT=0
fi
[ -n "$NAMES" ] || { echo "no partitions found — aborting"; exit 1; }
echo "  $(echo "$NAMES" | wc -w) partitions"

echo "[dump] 4/4 pulling partitions"
: > "$OUT/manifest.tsv"
printf 'name\tdevice\tbytes\tmd5_device\tmd5_host\tstatus\n' >> "$OUT/manifest.tsv"
for name in $NAMES; do
  if [ "$RESOLVE_BY_UEVENT" = 1 ]; then
    node=$(grep "PARTNAME=$name\$" "$OUT/partnames.txt" | head -1 \
           | sed 's#/sys/block/mmcblk0/\([^/]*\)/uevent.*#/dev/block/\1#')
  else
    node="$DEVDIR/$name"
  fi
  [ -n "$node" ] || continue

  if [ "$name" = "userdata" ] && [ "$WITH_USERDATA" = 0 ]; then
    echo "  -- userdata SKIPPED (pass --with-userdata to include it)"
    printf '%s\t%s\t-\t-\t-\tskipped\n' "$name" "$node" >> "$OUT/manifest.tsv"
    continue
  fi

  size=$(adb shell "blockdev --getsize64 $node 2>/dev/null || stat -c %s $node 2>/dev/null" 2>/dev/null | tr -d '\r')
  printf '  <- %-14s %-40s %s bytes\n' "$name" "$node" "${size:-?}"
  adb exec-out "dd if=$node 2>/dev/null" > "$OUT/partitions/$name.img" 2>/dev/null || true

  if [ ! -s "$OUT/partitions/$name.img" ]; then
    rm -f "$OUT/partitions/$name.img"
    echo "     (empty / unreadable)"
    printf '%s\t%s\t%s\t-\t-\tunreadable\n' "$name" "$node" "${size:-?}" >> "$OUT/manifest.tsv"
    continue
  fi

  # Integrity: hash on the device and on the host and compare. adb's transport
  # has mangled binary streams before; an unverified backup you only discover
  # is corrupt at restore time is worse than no backup.
  md5d=$(adb shell "md5sum $node 2>/dev/null || md5 $node 2>/dev/null" 2>/dev/null | awk '{print $1}' | tr -d '\r')
  md5h=$(md5sum "$OUT/partitions/$name.img" | awk '{print $1}')
  if [ -n "$md5d" ] && [ "$md5d" = "$md5h" ]; then st=ok
  elif [ -z "$md5d" ]; then st="pulled (device md5 unavailable)"
  else st="MISMATCH"; echo "     !! md5 MISMATCH: device=$md5d host=$md5h"; fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$node" "${size:-?}" "${md5d:--}" "$md5h" "$st" >> "$OUT/manifest.tsv"
done

# --- restore notes, written next to the images ---------------------------
cat > "$OUT/RESTORE.md" <<'EOF'
# Restoring this Fossil Gen 4 to stock

These images were taken with `tools/dump-gen4.sh`. Check `manifest.tsv`
first: only rows marked `ok` were verified byte-for-byte against a hash
computed on the device.

Most partitions go back with fastboot, from the bootloader:

```sh
fastboot flash <name> partitions/<name>.img
```

Read this before you use it:

- **Restore only what you actually changed.** Re-flashing everything is
  slower, riskier and gains nothing. If only `boot` was replaced, only
  `boot` needs restoring.
- **`aboot`, `sbl1`, `tz`, `rpm` and their `*bak` twins are the boot chain.**
  A bad write to any of them bricks the watch past fastboot's reach, and
  EDL (9008) + QFIL becomes the only way back. They are backed up so that
  path exists — not so they can be casually re-flashed.
- **`modemst1`, `modemst2`, `fsg`, `fsc`, `persist` hold per-device
  calibration and radio state.** They are unique to this unit and are the
  real reason to take this backup at all.
- **`userdata` is skipped by default** and is not needed to restore a
  working watch; `fastboot -w` (or a stock recovery wipe) recreates it.
- If a partition is not writable by fastboot, boot AsteroidOS and use
  `dd` to the block device from `partitions-by-name.txt`.
EOF

echo
echo "[dump] done."
echo "  images:   $OUT/partitions/"
echo "  manifest: $OUT/manifest.tsv"
echo "  restore:  $OUT/RESTORE.md"
echo
awk -F'\t' 'NR>1 {print "  " $6 "\t" $1}' "$OUT/manifest.tsv" | sort | uniq -c | sort -rn
du -sh "$OUT" 2>/dev/null || true
