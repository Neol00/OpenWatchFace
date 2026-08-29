#!/bin/sh
# build-owf.sh — compile the REAL OpenWatchFace firmware for the Fossil Gen 6
# bare-metal target (Milestone 1: compile + link with peripherals stubbed).
#
# Case-insensitive exFAT: the compat + arduino-api dirs hold headers whose names
# collide with system headers (String.h vs <string.h>), so they go on -iquote
# (quote-includes only), NEVER -I.
set -e
cd "$(dirname "$0")"

CXX="${CROSS:-arm-none-eabi-}g++"
OWF=../../OpenWatchFace
LVGL_DIR="${LVGL_DIR:-$HOME/Arduino/libraries/lvgl}"
LIBS=../../libraries          # firmware's lv_conf.h lives here

CXXFLAGS="-mcpu=cortex-a53 -marm -mfpu=neon-vfpv4 -mfloat-abi=hard \
  -O2 -g -std=gnu++17 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
  --specs=nano.specs --specs=nosys.specs \
  -DBOARD_SELECT=BOARD_ID_FOSSIL_GEN6 -DPLAT_BOARD_FOSSIL_GEN6 -DLV_CONF_INCLUDE_SIMPLE \
  -Wno-unused-variable -Wno-unused-function -Wno-unused-parameter"

# Include search (order matters):
#   -iquote: our shims + arduino-api + the firmware dir (quote-includes; keeps
#            <string.h> etc. resolving to newlib on the case-insensitive FS)
#   -I     : LVGL + firmware lv_conf + FreeRTOS config/port (system-style)
# compat is on -I (firmware uses <Wire.h>/<esp_system.h> etc.); it holds no
# header that case-collides with a system <> header. arduino-api STAYS on
# -iquote — it has String.h, which would shadow <string.h> on the exFAT FS.
INCS="-I compat -iquote compat/arduino-api -iquote $OWF \
  -I $LIBS -I $LVGL_DIR -I rtos -I freertos/include -I freertos/portable/GCC/ARM_CA9"

MODE="${1:-syntax}"
case "$MODE" in
  syntax) echo "[owf] syntax-only compile of OpenWatchFace.ino for fossil-gen6";
          $CXX $CXXFLAGS $INCS -fsyntax-only -x c++ "$OWF/OpenWatchFace.ino" ;;
  obj)    mkdir -p build/gen6-owf
          echo "[owf] compiling OpenWatchFace.ino -> object"
          $CXX $CXXFLAGS $INCS -c -x c++ "$OWF/OpenWatchFace.ino" -o build/gen6-owf/OpenWatchFace.o ;;
  *) echo "usage: $0 [syntax|obj]"; exit 1 ;;
esac
