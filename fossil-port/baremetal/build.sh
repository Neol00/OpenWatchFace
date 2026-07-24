#!/bin/sh
# build.sh — make-less build for Git Bash / any POSIX sh (same logic as the
# Makefile; use whichever you prefer).
#   ./build.sh [qemu|gen4] [run]
set -e
cd "$(dirname "$0")"

PLATFORM="${1:-qemu}"
CROSS="${CROSS:-arm-none-eabi-}"

case "$PLATFORM" in
  qemu) PLAT_DEF=PLAT_BOARD_QEMU_VIRT;   LINK_BASE=0x40008000 ;;
  gen4) PLAT_DEF=PLAT_BOARD_FOSSIL_GEN4; LINK_BASE=0x80008000 ;;
  *) echo "usage: $0 [qemu|gen4] [run]"; exit 1 ;;
esac

BUILD="build/$PLATFORM"
mkdir -p "$BUILD"

# LVGL 9.5.0 — same source tree the ESP32/Arduino builds use.
LVGL_DIR="${LVGL_DIR:-$HOME/Documents/Arduino/libraries/lvgl}"

CFLAGS="-mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=hard \
        -ffreestanding -O2 -g \
        -Wall -Wextra -D$PLAT_DEF \
        -DLV_CONF_INCLUDE_SIMPLE \
        -Irtos -Iplatform -Ifreertos/include -Ifreertos/portable/GCC/ARM_CA9 \
        -I$LVGL_DIR"

# LVGL compiles once into a cached archive (delete $BUILD/liblvgl.a to force).
LVGL_LIB="$BUILD/liblvgl.a"
if [ ! -f "$LVGL_LIB" ]; then
    echo "building LVGL (one-time, ~460 files)..."
    mkdir -p "$BUILD/lvgl"
    for src in $(find "$LVGL_DIR/src" -name '*.c'); do
        rel="${src#"$LVGL_DIR/src/"}"
        obj="$BUILD/lvgl/$(printf '%s' "$rel" | tr '/' '_' | sed 's/\.c$/.o/')"
        ${CROSS}gcc $CFLAGS -w -c "$src" -o "$obj" || exit 1
    done
    ${CROSS}ar rcs "$LVGL_LIB" "$BUILD"/lvgl/*.o
    echo "LVGL archived: $LVGL_LIB"
fi

SRCS="platform/startup.S main.c ui_demo.c platform/console.c platform/ramlog.c \
      platform/timer.c platform/uart_pl011.c platform/uart_msm.c \
      platform/gic.c platform/irq.c platform/fb_ramfb.c platform/mmu.c \
      platform/msm_dsi.c platform/dsi_panel.c platform/msm_mdp3.c platform/msm_i2c.c platform/touch_raydium.c \
      freertos/tasks.c freertos/queue.c freertos/list.c freertos/timers.c \
      freertos/event_groups.c freertos/stream_buffer.c \
      freertos/portable/MemMang/heap_4.c \
      freertos/portable/GCC/ARM_CA9/port.c freertos/portable/GCC/ARM_CA9/portASM.S"

OBJS=""
for src in $SRCS; do
    obj="$BUILD/$(basename "$src" | sed 's/\.[cS]$/.o/')"
    echo "CC  $src"
    ${CROSS}gcc $CFLAGS -c "$src" -o "$obj"
    OBJS="$OBJS $obj"
done

echo "LD  $BUILD/owf.elf"
${CROSS}gcc $CFLAGS -nostartfiles --specs=nano.specs --specs=nosys.specs \
    -Wl,--build-id=none \
    -Wl,--defsym=LINK_BASE=$LINK_BASE -T platform/linker.ld \
    -Wl,-Map="$BUILD/owf.map" $OBJS "$LVGL_LIB" -lm -lgcc -o "$BUILD/owf.elf"
${CROSS}size "$BUILD/owf.elf"
${CROSS}objcopy -O binary "$BUILD/owf.elf" "$BUILD/owf.bin"

if [ "$PLATFORM" = "gen4" ]; then
    gzip -9 -k -f "$BUILD/owf.bin"
    echo "payload: $BUILD/owf.bin.gz  (pack with tools/mk-bootimg.sh)"
fi

# `run` needs a NATIVE-arch qemu. On Windows-on-ARM64 that means WSL:
#   wsl -e bash -c "cd <this dir> && ./build.sh qemu run"
# Under WSLg the ramfb window pops up on the Windows desktop.
if [ "$2" = "run" ]; then
    qemu-system-arm -M virt,highmem=off -cpu cortex-a7 -m 512M \
        -device ramfb -serial stdio -kernel "$BUILD/owf.elf"
fi
