#!/bin/sh
# build.sh — make-less build for Git Bash / any POSIX sh (same logic as the
# Makefile; use whichever you prefer).
#   ./build.sh [qemu|gen4] [run]
set -e
cd "$(dirname "$0")"

PLATFORM="${1:-qemu}"
CROSS="${CROSS:-arm-none-eabi-}"

# gen4 selects the Fossil Gen 4 SoC guard (PLAT_BOARD_FOSSIL_GEN4, shared by all
# drivers) plus a size sub-variant. firefish=44mm (DTB-confirmed 454x454),
# ray=40mm (TODO, needs its own DTB). Plain "gen4" == firefish. BUILD dir stays
# "gen4" so mk-bootimg.sh / packaging paths are unchanged.
VARIANT_DEF=""
BUILD_TAG="$PLATFORM"
case "$PLATFORM" in
  qemu)          PLAT_DEF=PLAT_BOARD_QEMU_VIRT;   LINK_BASE=0x40008000 ;;
  gen4|gen4-firefish)
                 PLAT_DEF=PLAT_BOARD_FOSSIL_GEN4; LINK_BASE=0x80008000
                 VARIANT_DEF=PLAT_FOSSIL_VARIANT_FIREFISH; BUILD_TAG=gen4 ;;
  gen4-ray)      PLAT_DEF=PLAT_BOARD_FOSSIL_GEN4; LINK_BASE=0x80008000
                 VARIANT_DEF=PLAT_FOSSIL_VARIANT_RAY;      BUILD_TAG=gen4 ;;
  # Fossil Gen 6 (hoki, SDA429W / Wear 4100+). 32-BIT ARM despite the A53 —
  # the vendor DT lives in arch/arm/ and AsteroidOS tunes it armv7vehf-neon,
  # so the same -mcpu=cortex-a7 codegen is valid (A53 runs ARMv7-A code in
  # AArch32). Link base matches the stock boot.img: 0x80000000 + 0x8000.
  gen6|gen6-hoki)
                 PLAT_DEF=PLAT_BOARD_FOSSIL_GEN6; LINK_BASE=0x80008000
                 BUILD_TAG=gen6 ;;
  # Mobvoi TicWatch C2 (skipjack) — same APQ8009W as the Gen 4, so the same
  # link base and the same SoC drivers (PLAT_SOC_MSM8909); only the board
  # header, panel and touch differ.
  c2|ticwatch-c2|skipjack)
                 PLAT_DEF=PLAT_BOARD_TICWATCH_C2; LINK_BASE=0x80008000
                 BUILD_TAG=c2 ;;
  *) echo "usage: $0 [qemu|gen4|gen4-firefish|gen4-ray|gen6|c2] [run]"; exit 1 ;;
esac

BUILD="build/$BUILD_TAG"
mkdir -p "$BUILD"

# LVGL 9.5.0 — same source tree the ESP32/Arduino builds use.
# LVGL source. The repo now carries its own copy (libraries/lvgl) — prefer that,
# so a build does not depend on where an Arduino sketchbook happens to live.
_LVGL_IN_REPO="$(cd "$(dirname "$0")/../.." && pwd)/libraries/lvgl"
if [ -z "$LVGL_DIR" ] && [ -d "$_LVGL_IN_REPO/src" ]; then
    LVGL_DIR="$_LVGL_IN_REPO"
fi
LVGL_DIR="${LVGL_DIR:-$HOME/Documents/Arduino/libraries/lvgl}"

CFLAGS="-mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=hard \
        -ffreestanding -O2 -g \
        -Wall -Wextra -D$PLAT_DEF ${VARIANT_DEF:+-D$VARIANT_DEF} ${CFLAGS_EXTRA:-} \
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
      platform/gic.c platform/irq.c platform/fb_ramfb.c platform/fb_splash.c platform/fb_mdp3.c platform/gfx_text.c platform/gcc_mdss.c platform/gcc_blsp.c platform/tlmm.c platform/dsi_dcs.c platform/dsi_pll_12nm.c platform/mmu.c \
      platform/msm_dsi.c platform/dsi_panel.c platform/msm_mdp3.c platform/msm_i2c.c platform/touch_raydium.c platform/reboot_msm.c \
      platform/spmi_arb.c platform/pmic_vib.c platform/pmic_rtc.c platform/pmic_fg.c platform/pmic_pon.c \
      platform/sdhci_msm.c platform/gcc_sdcc.c platform/msm_wdog.c platform/bootmark.c \
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

case "$BUILD_TAG" in
  gen4|gen6|c2)
    gzip -9 -k -f "$BUILD/owf.bin"
    echo "payload: $BUILD/owf.bin  (pack with tools/mk-bootimg.sh)"
    ;;
esac

# `run` needs a NATIVE-arch qemu. On Windows-on-ARM64 that means WSL:
#   wsl -e bash -c "cd <this dir> && ./build.sh qemu run"
# Under WSLg the ramfb window pops up on the Windows desktop.
if [ "$2" = "run" ]; then
    qemu-system-arm -M virt,highmem=off -cpu cortex-a7 -m 512M \
        -device ramfb -serial stdio -kernel "$BUILD/owf.elf"
fi
