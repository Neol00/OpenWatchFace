#!/bin/sh
# build-owf-image.sh — link the REAL OpenWatchFace firmware into a Gen 6 boot
# payload: snapdragon-port runtime + compat layer + app TU + LVGL (firmware lv_conf).
set -e
cd "$(dirname "$0")"
CROSS="${CROSS:-arm-none-eabi-}"
CC="${CROSS}gcc"; CXX="${CROSS}g++"
LVGL_DIR="${LVGL_DIR:-$HOME/Arduino/libraries/lvgl}"
LIBS=../../libraries          # firmware lv_conf.h
OWF=../../OpenWatchFace
B=build/gen6-owf
mkdir -p "$B"

COMMON="-mcpu=cortex-a53 -marm -mfpu=neon-vfpv4 -mfloat-abi=hard -O2 -g \
  -DPLAT_BOARD_FOSSIL_GEN6 -DLV_CONF_INCLUDE_SIMPLE ${CFLAGS_EXTRA:-}"
# runtime C (freestanding, like build.sh). -I $LIBS FIRST so <lv_conf.h> resolves
# to the FIRMWARE's config, not rtos/lv_conf.h (the demo one) — that mismatch
# silently compiled every LVGL font out.
# -I compat FIRST so <lv_conf.h> resolves to compat/lv_conf.h (firmware config +
# LV_USE_OS=NONE), identical to what the app TU uses — same config both sides.
CFLAGS="$COMMON -ffreestanding -I compat -I $LIBS -Irtos -Iplatform -Ifreertos/include \
  -Ifreertos/portable/GCC/ARM_CA9 -I $LVGL_DIR -I lwip_port -I third_party/lwip/src/include"
# app + compat C++ (HOSTED — freestanding strips shared_ptr; exceptions/rtti off)
CXXFLAGS="$COMMON -std=gnu++17 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
  --specs=nano.specs --specs=nosys.specs -DBOARD_SELECT=BOARD_ID_FOSSIL_GEN6 \
  -I compat -Iplatform -iquote compat/arduino-api -iquote $OWF -I $LIBS -I $LVGL_DIR \
  -Irtos -Ifreertos/include -Ifreertos/portable/GCC/ARM_CA9 \
  -Wno-unused-variable -Wno-unused-function -Wno-unused-parameter"

# NimBLE host + mbedTLS. The Gen 6's controller is a WCN3990 on blsp2_uart2,
# so the SAME host stack the 8909w watches run rides on the H4 UART transport
# instead of the shared-memory one.
NIMBLE_TRANSPORT=nimble_transport_uart.c
. nimble_port/sources.sh
CFLAGS="$CFLAGS $NIMBLE_INC"
CXXFLAGS="$CXXFLAGS $NIMBLE_INC"

# --- LVGL, built once against the FIRMWARE's lv_conf.h ---
LVLIB="$B/liblvgl-owf.a"
if [ ! -f "$LVLIB" ]; then
  echo "[owf] building LVGL against firmware lv_conf (one-time)..."
  mkdir -p "$B/lvgl"
  for src in $(find "$LVGL_DIR/src" -name '*.c'); do
    obj="$B/lvgl/$(printf '%s' "${src#"$LVGL_DIR/src/"}" | tr '/' '_' | sed 's/\.c$/.o/')"
    ${CROSS}gcc $CFLAGS -w -c "$src" -o "$obj"
  done
  ${CROSS}ar rcs "$LVLIB" "$B"/lvgl/*.o
  echo "[owf] LVGL archived: $LVLIB"
fi

OBJS=""
cc_one()  { echo "CC  $1";  $CC  $CFLAGS   $2 -c "$1" -o "$3"; OBJS="$OBJS $3"; }
cxx_one() { echo "CXX $1"; $CXX $CXXFLAGS $2 -c "$1" -o "$3"; OBJS="$OBJS $3"; }

# runtime (NO ui_demo.c; main.c gets -DOWF_APP)
cc_one platform/startup.S "" "$B/startup.o"
cc_one platform/cpu_suspend.S "" "$B/cpu_suspend.o"
cc_one main.c "-DOWF_APP" "$B/main.o"
for c in console ramlog timer uart_pl011 uart_msm gic irq fb_ramfb fb_splash gcc_mdss gcc_blsp tlmm dsi_dcs dsi_pll_12nm mmu \
         msm_dsi dsi_panel msm_mdp3 msm_i2c touch_raydium reboot_msm \
         smem scm smd wcnss wcn36xx wlan_crypto wlan_sta wlan_net bt_hci spmi_arb pmic_vib pmic_rtc pmic_fg pmic_pon pwr_diag suspend_msm sleep_stats psci pmic_irq cpu_pc mpm sdhci_msm msm_wdog bootmark ddr_size bootimg_write \
         storage_gen6 nvs_store gcc_sdcc gfx_text logfile gcc_usb usb_phy_msm usb_ci recovery_gate uart_bt bt_wcn3990 bt_fw rng_msm kmsg_forensics sleep_quiesce; do
  cc_one platform/$c.c "" "$B/$c.o"
done
# lwIP 2.1.3 (third_party/lwip, NO_SYS; options in lwip_port/lwipopts.h)
LW=third_party/lwip/src
for c in core/init core/def core/dns core/inet_chksum core/ip core/mem core/memp core/netif core/pbuf core/stats core/sys core/tcp core/tcp_in core/tcp_out core/timeouts core/udp core/raw \
         core/ipv4/dhcp core/ipv4/etharp core/ipv4/icmp core/ipv4/ip4 core/ipv4/ip4_addr core/ipv4/ip4_frag core/ipv4/igmp core/ipv4/autoip \
         netif/ethernet apps/sntp/sntp; do
  cc_one $LW/$c.c "-w" "$B/lwip_$(basename $c).o"
done
cc_one lwip_port/sys_arch.c "" "$B/lwip_sys_arch.o"
# NimBLE host + mbedTLS crypto (nimble_port/sources.sh)
for s in $NIMBLE_SRCS $MBEDTLS_SRCS; do
  cc_one $s "-w" "$B/nb_$(echo $s | tr '/' '_' | sed 's/\.c$/.o/')"
done
# FatFs (Gen 6 userdata FFAT region; diskio.c is ours)
for c in ff ffunicode diskio; do
  cc_one fatfs/$c.c "" "$B/fatfs_$c.o"
done
for c in tasks queue list timers event_groups stream_buffer; do
  cc_one freertos/$c.c "" "$B/frt_$c.o"
done
cc_one freertos/portable/MemMang/heap_4.c "" "$B/heap_4.o"
cc_one freertos/portable/GCC/ARM_CA9/port.c "" "$B/port.o"
cc_one freertos/portable/GCC/ARM_CA9/portASM.S "" "$B/portASM.o"

# compat + ArduinoCore-API
for cpp in arduino_glue wire_glue wifi_glue ble_glue fs_glue arduino_main owf_arduino_extra owf_time owf_sbrk owf_meminfo; do
  cxx_one compat/$cpp.cpp "" "$B/$cpp.o"
done
for cpp in String Print Stream Common IPAddress; do
  cxx_one compat/arduino-api/$cpp.cpp "" "$B/api_$cpp.o"
done

# firmware font + icon data (LVGL C tables)
for f in icons14 icons22 icons28 icons34 icons88 \
         montserrat_clock_72 montserrat_clock_80 montserrat_clock_88 montserrat_clock_110; do
  cc_one "$OWF/$f.c" "" "$B/fw_$f.o"
done

# firmware LVGL custom allocator (heap_caps backend -> our malloc shim)
cxx_one "$OWF/lv_psram_alloc.cpp" "" "$B/lv_psram_alloc.o"

# the firmware itself
# -x c++ is REQUIRED: gcc does not know the .ino extension and silently treats
# the file as linker input, which quietly drops conditionally-compiled code.
cxx_one "$OWF/OpenWatchFace.ino" "-x c++" "$B/OpenWatchFace.o"

echo "LD  $B/owf.elf"
$CXX $CXXFLAGS -nostartfiles \
  -Wl,--build-id=none -Wl,-u,_printf_float -Wl,-u,_scanf_float -Wl,--defsym=LINK_BASE=0x80008000 -T platform/linker.ld \
  -Wl,-Map="$B/owf.map" $OBJS "$LVLIB" -lstdc++ -lm -lgcc -lc -o "$B/owf.elf"
${CROSS}size "$B/owf.elf"
${CROSS}objcopy -O binary "$B/owf.elf" "$B/owf.bin"
echo "[owf] image: $B/owf.bin"
