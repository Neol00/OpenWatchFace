/* ============================================================================
 *  board_ticwatch_c2.h — Mobvoi TicWatch C2 / C2+ (skipjack, WG12036),
 *  BARE-METAL on one Cortex-A7.
 *
 *  Not an MCU board and not a Linux port: this runs bare-metal on core 0 of the
 *  watch's Snapdragon Wear 2100 (APQ8009W / msm8909w) on top of the
 *  fossil-port/baremetal runtime — the SAME SoC as the Fossil Gen 4, which is
 *  why this port is mostly inheritance rather than new work. SoC-level detail
 *  lives in fossil-port/baremetal/boards/ticwatch_c2.h; this header describes
 *  only what the FIRMWARE needs to know.
 *
 *  Every hardware fact below was read out of the watch's own merged device
 *  tree, reconstructed in fossil-port/dumps/c2-skipjack-fromsource/. The C2 is
 *  an appended-DTB device with no dtbo partition, so the tree lives inside the
 *  stock boot image rather than in a partition that can be pulled; it was
 *  rebuilt from the skipjack kernel source instead, and the watch's own
 *  `ro.hardware = skipjack` confirms it is the right tree.
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "TicWatch C2"
#define BOARD_VENDOR "Mobvoi"

/* ---- Over-the-air update identity ---------------------------------------- */
#define BOARD_OTA_KEY "ticwatch-c2-skipjack"

/* ---- Platform flag: bare-metal Qualcomm MSM (no Arduino/ESP runtime) ------
 * Same gate the two Fossil watches use; display/touch/tick come from the
 * fossil-port runtime. */
#define BOARD_PLATFORM_FOSSIL 1

/* ---- Display / touch ------------------------------------------------------
 * BOARD_TOUCH_RAYDIUM is the runtime's generic "an MSM I2C touch controller
 * the runtime owns" gate rather than a claim about the part — the C2's
 * controller is a FocalTech FTS at 0x38 (touch_ft.c), not a Raydium. Renaming
 * the flag would touch every board header for no behavioural gain. */
#define BOARD_DISPLAY_MSM_DSI 1
#define BOARD_TOUCH_RAYDIUM   1

/* ---- Capabilities ---------------------------------------------------------
 * Same stub-first philosophy as the Gen 4 and Maix ports: everything starts
 * OFF so its module compiles to stubs, and each switches ON when its driver
 * lands. Haptics is ON from the start because it is inherited outright — the
 * C2's DTB gives qcom,vib-vtg-level-mV = 3100, identical to the Gen 4, so
 * pmic_vib.c needs no change at all. */
#define BOARD_HAS_PSRAM           0   /* plain malloc into DDR (512 MB) */
#define BOARD_DUAL_CORE           0   /* cores 1-3 parked */
#define BOARD_HAS_PMU_AXP2101     0   /* PMIC is PM8916 over SPMI */
#define BOARD_HAS_ADC_BATTERY     0   /* PM8916 VM-BMS; pmic_fg.c, ported */
#define BOARD_HAS_RTC_PCF85063    0   /* PM8916 RTC over SPMI */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0
#define BOARD_HAS_HAPTICS         1   /* PM8916 qpnp-vibrator @0xc000, 3100 mV */
#define BOARD_HAS_SD_MMC          0   /* eMMC via sdhci-msm (7824900.sdhci) */
#define BOARD_HAS_SD_SPI          0
#define BOARD_HAS_IMU_QMI8658     0
#define BOARD_HAS_BACKLIGHT_PWM   0   /* AMOLED: brightness by DCS command */
#define BOARD_HAS_LP_STEPS        0
#define BOARD_WAKE_USE_EXT0       0
#define BOARD_HAS_BLE             0
#define BOARD_HAS_FFAT            0

/* BLE TX-power ladder placeholder (same contract as the other bare-metal
 * ports: referenced unconditionally by settings_store.h, never applied). */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9, \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -15, -12, -9, -6, 0, 9, 20 }

#define BOARD_HW_SUMMARY \
  "Host:    TicWatch C2 (Wear 2100, APQ8009W, bare-metal A7)\n" \
  "Display: 360x360 round AMOLED, command mode\n" \
  "         MDP3 DMA_P over 1-lane MIPI-DSI, 24bpp\n" \
  "Touch:   FocalTech FTS @0x38 on BLSP1 QUP5 (2 pt)\n" \
  "Power:   PM8916 VM-BMS (voltage-mode; no current sense)\n" \
  "Storage: eMMC 7824900.sdhci (not yet mounted)"

/* ---- Screen geometry ------------------------------------------------------
 * 360x360, CONFIRMED ON HARDWARE. The merged device tree's panel node claims
 * 400x400 ("EDO E1392AMC AMOLED command mode dsi panel", width/height 0x190)
 * and is wrong for this unit; aboot programs 360 and the glass agrees, as do
 * the spec sheets and the touch controller's own coordinate space.
 *
 * Worth keeping as a caution: on the Gen 4 the community figure (390) was
 * wrong and the DTB (454) was right, so the DTB was trusted here — and on this
 * watch the DTB is the one that lies. Neither source is authoritative. The
 * runtime does not depend on this constant: LVGL sizes itself from
 * fb_width()/fb_height(), which come from what the bootloader actually
 * programmed, and this value is only a fallback hint. */
#define LCD_WIDTH  360
#define LCD_HEIGHT 360
#define BOARD_SCREEN_ROUND   1
#define BOARD_LCD_EVEN_ALIGN 0
#define BOARD_PARTIAL_BUF_LINES 64

/* ---- Boot-progress markers (bring-up only) -------------------------------- */
#if defined(WDOG_TRACE)
extern "C" void wdog_stage(unsigned stage);
#define OWF_STAGE(n) wdog_stage(n)
#endif
#if defined(VISUAL_TRACE)
#include <cstdint>
extern "C" void fb_trace(uint32_t xrgb);
#define OWF_VTRACE(c) fb_trace(c)
#endif

/* ---- Panel brightness: DCS 0x51 over the fossil-port DSI host --------------
 * The panel is "bl_ctrl_dcs" with min level 1 and max 255 — the same scheme as
 * the Gen 4's AUO h139, so dsi_dcs_set_brightness() and the whole auto-dim
 * path apply unchanged. One of the larger pieces of Gen 4 bring-up that this
 * watch simply inherits. */
extern "C" int dsi_dcs_set_brightness(unsigned char level);

/* ---- Dummy pin / bus constants -------------------------------------------
 * Virtual pins exactly as on the Fossil watches (compat/arduino_glue.cpp):
 * the motor sits behind the PMIC's vibrator block and the button behind
 * qpnp-power-on, so neither is a real GPIO. digitalWrite(200) drives vib_set();
 * digitalRead(201) reads LOW while the button is held.
 *
 * The C2 has exactly ONE button — gpio_keys/stem_1, "STEM_1", TLMM GPIO 91,
 * active low, wakeup-capable — and NO rotating crown: no pixart node survives
 * into the merged tree. So there is no encoder to map to LVGL, which removes a
 * subsystem the Gen 4 still has outstanding. */
#define IIC_SDA          0
#define IIC_SCL          0
#define TP_INT           0
#define TP_RESET         0
#define HAPTICS_MOTOR_GPIO  200
#define HAPTICS_ACTIVE_HIGH 1
/* Per-board UI click length. haptics.h's 6 ms DEFAULT is an ESP32-coin-ERM
 * number and it is far too short for this watch: 6 ms of enable does not spin
 * the motor at all, which is why every on-screen control felt dead here while
 * the alarm buzzed fine (the alarm plays H_DOT_MS 45 / H_DASH_MS 100 through
 * the same vib_set() path, so the plumbing was always working -- only the
 * duration was below the motor's spin-up). Every other board with a real motor
 * already overrides this: 28 on the Tuya T5, 40 on the S3 2.06. 35 sits
 * between them. Buzz strength is pulse-LENGTH only (no amplitude PWM), so this
 * is THE knob: raise if too faint, lower if too strong, but below ~25 it stops
 * registering. The pulse blocks the click callback for exactly this long. */
#define HAPTICS_CLICK_MS    35
#define BOOT_BTN_GPIO    201
#define BOARD_WAKE_GPIO  0
#define BOARD_LCD_BUS_HZ 0
