#!/bin/sh
# build-bringup-gen5.sh — the FIRST image for the Fossil Gen 5 (triggerfish).
#
# NOT the full firmware. This is the sign-of-life build: main() without
# -DOWF_APP, so it buzzes the motor, probes the framebuffer, and reports over
# the ramlog / USB console. No LVGL app, no radio stack, no BLE.
#
# WHY THIS AND NOT build-owf-image-gen5.sh: nothing has ever executed on this
# watch. The full firmware would bring up LVGL, storage, WCNSS and NimBLE in
# one step, so a failure anywhere in that stack looks identical to "aboot
# rejected the image". This build answers exactly one question first — does OUR
# CODE RUN ON A WEAR 3100 — and answers it through the motor, which needs only
# SPMI and no display at all. That is the same ladder the Gen 4 bring-up used.
#
# The PM660 is why the motor is a real test here and not a formality: the Gen 5
# haptics are the waveform player at 0xc000, not the PM8916 enable bit, so a
# buzz also proves the board header picked the right vibrator block.
set -e
cd "$(dirname "$0")"
CROSS="${CROSS:-arm-none-eabi-}"
CC="${CROSS}gcc"
_LVGL_IN_REPO="$(cd "$(dirname "$0")/../.." && pwd)/libraries/lvgl"
[ -z "$LVGL_DIR" ] && [ -d "$_LVGL_IN_REPO/src" ] && LVGL_DIR="$_LVGL_IN_REPO"
LIBS=../../libraries
B=build/gen5
mkdir -p "$B"

COMMON="-mcpu=cortex-a7 -marm -mfpu=neon-vfpv4 -mfloat-abi=hard -O2 -g \
  -DPLAT_BOARD_FOSSIL_GEN5 -DLV_CONF_INCLUDE_SIMPLE ${CFLAGS_EXTRA:-}"
CFLAGS="$COMMON -ffreestanding -I compat -I $LIBS -Irtos -Iplatform \
  -Ifreertos/include -Ifreertos/portable/GCC/ARM_CA9 -I $LVGL_DIR"

LVLIB="$B/liblvgl.a"
if [ ! -f "$LVLIB" ]; then
  echo "[gen5] building LVGL (one-time)..."
  mkdir -p "$B/lvgl"
  for src in $(find "$LVGL_DIR/src" -name '*.c'); do
    obj="$B/lvgl/$(printf '%s' "${src#"$LVGL_DIR/src/"}" | tr '/' '_' | sed 's/\.c$/.o/')"
    $CC $CFLAGS -w -c "$src" -o "$obj"
  done
  ${CROSS}ar rcs "$LVLIB" "$B"/lvgl/*.o
fi

OBJS=""
cc_one() { echo "CC  $1"; $CC $CFLAGS $2 -c "$1" -o "$3"; OBJS="$OBJS $3"; }

cc_one platform/startup.S "" "$B/startup.o"
cc_one platform/cpu_suspend.S "" "$B/cpu_suspend.o"
cc_one platform/smp_entry.S "" "$B/smp_entry.o"
cc_one main.c "" "$B/main.o"
cc_one ui_demo.c "" "$B/ui_demo.o"

# Platform drivers. Deliberately EXCLUDED from this first image:
#   smem/scm/smd/wcnss/wcn36xx/wlan_*/bt_hci  — the radio stack; its rails are
#       transcribed but unproven, and it needs lwIP + a console to debug.
#   rot_pat9126  — there is no PixArt crown on this watch (the crown is behind
#       the QCC1110 co-processor over GLINK).
#   chg_smb231   — wrong charger. This board has an SMB1357 on I2C, and the
#       stock tree even marks it disabled in favour of the PM660's own block.
for c in console ramlog timer uart_pl011 uart_msm gic irq fb_mdp3 gcc_mdss gcc_blsp \
         tlmm mmu msm_dsi dsi_panel msm_mdp3 msm_i2c touch_raydium reboot_msm \
         spmi_arb pmic_vib pmic_rtc pmic_fg pmic_pon psci pmic_irq msm_wdog bootmark \
         gfx_text recovery_gate cpu_clk_a7 cpu_volt_a7 tsens_8909 \
         gcc_usb usb_phy_msm usb_ci gen4_stubs \
         sdhci_msm gcc_sdcc ddr_size mpm tlmm_irq rng_msm \
         spm_8909 cpu_pc8909 sys_pc8909 smp_8909 sleep_floor smem scm smd bringup_stubs; do
  cc_one platform/$c.c "" "$B/$c.o"
done
for f in tasks queue list timers event_groups stream_buffer; do
  cc_one freertos/$f.c "" "$B/$f.o"
done
cc_one freertos/portable/MemMang/heap_4.c "" "$B/heap_4.o"
cc_one freertos/portable/GCC/ARM_CA9/port.c "" "$B/port.o"
cc_one freertos/portable/GCC/ARM_CA9/portASM.S "" "$B/portASM.o"

echo "LD  $B/owf.elf"
$CC $CFLAGS -nostartfiles --specs=nano.specs --specs=nosys.specs \
  -Wl,--build-id=none -Wl,--defsym=LINK_BASE=0x80008000 -T platform/linker.ld \
  -Wl,-Map="$B/owf.map" $OBJS "$LVLIB" -lm -lgcc -o "$B/owf.elf"
${CROSS}size "$B/owf.elf"
${CROSS}objcopy -O binary "$B/owf.elf" "$B/owf.bin"
echo "[gen5] payload: $B/owf.bin"
echo "[gen5] pack with: sh tools/mk-bootimg-gen5.sh $B/owf.bin"
