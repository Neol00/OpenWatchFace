#!/usr/bin/env bash
# Apply the ESP32-S3-WatchFace library patches (LVGL, Arduino_GFX, ESP32 core BLE).
#
# Mirrors apply_patches.ps1: uses `git apply` as a portable patch tool, dry-runs every
# patch first and aborts if ANY would not apply cleanly (so it never leaves your tree
# half-patched), and skips patches that are already applied.
#
# No arguments needed — the target directories are derived from the CURRENT USER'S HOME,
# exactly like the Windows script. Override only if your install lives elsewhere:
#
# Usage:
#   ./apply_patches.sh [LIBRARIES_DIR] [ESP32_CORE_DIR]
#
#   LIBRARIES_DIR   your Arduino libraries folder (has lvgl/, lv_conf.h,
#                   GFX_Library_for_Arduino/).  Default: ~/Arduino/libraries
#   ESP32_CORE_DIR  installed ESP32 core root (has libraries/BLE).
#                   Default: ~/.arduino15/packages/esp32/hardware/esp32/3.3.10
#
# Requires `git` (used as a portable patch tool via `git apply`, matching the PS1 script).
set -euo pipefail

PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBRARIES_DIR="${1:-$HOME/Arduino/libraries}"
ESP32_CORE_DIR="${2:-$HOME/.arduino15/packages/esp32/hardware/esp32/3.3.10}"

LVGL_DIR="$LIBRARIES_DIR/lvgl"
GFX_DIR="$LIBRARIES_DIR/GFX_Library_for_Arduino"
BLE_DIR="$ESP32_CORE_DIR/libraries/BLE"
I2S_DIR="$ESP32_CORE_DIR/libraries/ESP_I2S"

command -v git >/dev/null 2>&1 || { echo "ERROR: 'git' not found on PATH. Install git, or apply the patches manually (see README.md)."; exit 1; }

# job: "patchfile|applyroot|description"  (applyroot = the -p1 root, same as the PS1 jobs)
JOBS=(
  "01-lvgl-freertos-corepin.patch|$LVGL_DIR|LVGL render-thread core pin"
  "02-lv_conf-snapshot.patch|$LIBRARIES_DIR|lv_conf.h snapshot enable"
  "03-gfx-qspi-dma.patch|$GFX_DIR|GFX QSPI async-DMA flush"
  "04-gfx-qspi-header.patch|$GFX_DIR|GFX QSPI second transaction struct"
  "05-esp32-ble-gap.patch|$BLE_DIR|ESP32 core BLE GAP-listener unregister"
  "06-esp32-psram-size.patch|$ESP32_CORE_DIR|ESP32 core getPsramSize physical chip size"
  "08-esp32-i2s-channel-leak.patch|$I2S_DIR|ESP32 core ESP_I2S channel-leak fix"
)

for d in "$LVGL_DIR" "$LIBRARIES_DIR" "$GFX_DIR" "$BLE_DIR" "$I2S_DIR"; do
  [ -d "$d" ] || { echo "ERROR: target directory not found: $d"; echo "Check the LIBRARIES_DIR / ESP32_CORE_DIR arguments."; exit 1; }
done

echo "ESP32-S3-WatchFace — applying library patches"
echo "  libraries:  $LIBRARIES_DIR"
echo "  esp32 core: $ESP32_CORE_DIR"
echo

# state: echoes applies | applied | fails  (matches Test-PatchState in the PS1 script:
# a clean forward dry-run = applies; a clean reverse dry-run = already applied; else fails)
patch_state() {
  local pf="$1" root="$2"
  if git -C "$root" apply --check "$pf" >/dev/null 2>&1; then echo applies; return; fi
  if git -C "$root" apply --reverse --check "$pf" >/dev/null 2>&1; then echo applied; return; fi
  echo fails
}

# --- Phase 1: dry-run everything, decide, abort on any hard failure ---
declare -a STATES
hard_fail=0
i=0
for job in "${JOBS[@]}"; do
  IFS='|' read -r pf root desc <<<"$job"
  pp="$PATCH_DIR/$pf"
  [ -f "$pp" ] || { echo "ERROR: missing patch file: $pp"; exit 1; }
  st="$(patch_state "$pp" "$root")"
  STATES[$i]="$st"
  case "$st" in
    applies) echo "  [will apply] $desc" ;;
    applied) echo "  [already ok] $desc" ;;
    fails)   echo "  [CANNOT APPLY] $desc  ($pf)"; hard_fail=1 ;;
  esac
  i=$((i+1))
done

if [ "$hard_fail" -ne 0 ]; then
  echo
  echo "Aborting: at least one patch does not apply cleanly. Nothing was changed."
  echo "Most likely your library version differs. Required: LVGL 9.5.0, Arduino_GFX 1.6.5, ESP32 core 3.3.10."
  exit 1
fi

# --- Phase 2: apply the ones that need applying ---
echo
i=0
for job in "${JOBS[@]}"; do
  IFS='|' read -r pf root desc <<<"$job"
  if [ "${STATES[$i]}" = "applies" ]; then
    git -C "$root" apply "$PATCH_DIR/$pf"
    echo "  applied: $desc"
  fi
  i=$((i+1))
done

echo
echo "Done. Now clear the Arduino build cache and rebuild, e.g.:"
echo "  rm -rf ~/.cache/arduino/sketches/*"
