/* ============================================================================
 *  board_fossil_gen4.h — Fossil Gen 4 (firefish/ray), BARE-METAL on one Cortex-A7
 *
 *  This is NOT an MCU board and NOT a Linux port. The firmware runs bare-metal
 *  on core 0 of the watch's Snapdragon Wear 2100 (APQ8009w / msm8909w),
 *  on top of the fossil-port/baremetal runtime (FreeRTOS Cortex-A port + ported
 *  MSM drivers). See fossil-port/README.md (plan) and fossil-port/HARDWARE.md
 *  (verified addresses / peripherals).
 *
 *  Fossil-fleet note: this is the FIRST of several Fossil targets (a Gen 6 /
 *  Wear 4100+ "hoki" and Fossil Q models exist in hardware). Everything
 *  SoC-generation-specific lives in fossil-port/baremetal/boards/<device>.h;
 *  this header only describes what the FIRMWARE needs to know about the Gen 4.
 *  Future watches add board_fossil_<device>.h + a BOARD_ID_FOSSIL_<DEVICE>.
 *
 *  Feature flags mirror the Maix port's philosophy: start with everything OFF
 *  (modules compile to stubs), switch subsystems ON as their bare-metal driver
 *  lands (roadmap phases in fossil-port/README.md).
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Fossil Gen 4"
#define BOARD_VENDOR "Fossil"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-fossil-gen4-firefish-<version>.bin
 *
 * TWO KEYS, because the Gen 4 is two watches. firefish (44mm) and ray (40mm)
 * share the SoC and every driver but not necessarily the panel, so they are
 * separate builds and must be separate release assets — publishing one image
 * under a name that matches both is how a 40mm watch ends up installing a
 * 44mm panel geometry. The variant is chosen by the build
 * (build.sh gen4-firefish | gen4-ray) and defaults to firefish, the unit whose
 * DTB is confirmed; see fossil-port/baremetal/boards/fossil_gen4.h.
 *
 * A key with no matching entry in the manifest is reported by the update check
 * as "No build for <key>", which is the correct answer for a variant that has
 * not been published rather than an error. */
#if defined(PLAT_FOSSIL_VARIANT_RAY)
#define BOARD_OTA_KEY "fossil-gen4-ray"
#else
#define BOARD_OTA_KEY "fossil-gen4-firefish"
#endif

/* ---- Platform flag: bare-metal Qualcomm MSM (no Arduino/ESP runtime) ------
 * Gates out ESP-only bring-up in the .ino exactly like BOARD_PLATFORM_MAIX does,
 * but display/touch/tick come from the fossil-port runtime instead of MaixCDK. */
#define BOARD_PLATFORM_FOSSIL 1

/* ---- Display / touch: served by the fossil-port bare-metal drivers --------- */
#define BOARD_DISPLAY_MSM_DSI 1   /* MDP3/DSI command-mode panel (Phase 3) */
#define BOARD_TOUCH_RAYDIUM   1   /* Raydium RM_TS on BLSP I2C (Phase 4) */

/* ---- Capabilities: all OFF until their phase lands ------------------------- */
#define BOARD_HAS_PSRAM           0   /* plain malloc into DDR (512 MB — no tiers) */
#define BOARD_DUAL_CORE           0   /* cores 1-3 parked; single-core for now */
#define BOARD_HAS_PMU_AXP2101     0   /* PMIC is PM8916-class over SPMI (own driver later) */
#define BOARD_HAS_ADC_BATTERY     0   /* battery via SMB231/PMIC gauge (Phase 6) */
#define BOARD_HAS_RTC_PCF85063    0   /* PMIC RTC over SPMI (Phase 5) */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0
#define BOARD_HAS_HAPTICS         1   /* PM8916 qpnp vibrator over SPMI (pmic_vib.c),
                                         driven through the virtual motor pin below */
#define BOARD_HAS_SD_MMC          0   /* eMMC via sdhci-msm (Phase 5) */
#define BOARD_HAS_SD_SPI          0
#define BOARD_HAS_IMU_QMI8658     0   /* Gen 4 IMU is a different part — own flag later */
#define BOARD_HAS_BACKLIGHT_PWM   0   /* AMOLED: brightness by panel command */
#define BOARD_HAS_LP_STEPS        0
#define BOARD_WAKE_USE_EXT0       0   /* wake = PMIC PON / RTC alarm (Phase 6) */
#define BOARD_HAS_BLE             0   /* Phase 7: NimBLE host over HCI-on-SMD (WCNSS) */
#define BOARD_HAS_FFAT            0   /* storage lands with eMMC in Phase 5 */

/* BLE TX-power ladder placeholder (same contract as the Maix port: referenced
 * unconditionally by settings_store.h, never applied while BLE is off). */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9, \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -15, -12, -9, -6, 0, 9, 20 }

#define BOARD_HW_SUMMARY \
  "Host:    Fossil Gen 4 (Wear 2100, APQ8009W, bare-metal A7)\n" \
  "Display: 454x454 AUO h139 AMOLED, command mode\n" \
  "         MDP3 DMA_P over MIPI-DSI, 24bpp\n" \
  "Touch:   Raydium RM_TS @0x39 on BLSP1 QUP5 (2 pt)\n" \
  "Power:   PM8916 VM-BMS (voltage-mode; no current sense)\n" \
  "Storage: eMMC 7824900.sdhci (not yet mounted)"

/* ---- Screen geometry ------------------------------------------------------
 * Round AMOLED, 454x454 — CONFIRMED from the stock DTB dumped off a DW6F1
 * (the "AUO h139 AMOLED command mode dsi panel" node, and the Raydium touch
 * node's display-coords 0x1c6 = 454). The 390x390 this file used to carry was
 * a pre-dump community guess and was simply wrong. The smaller "ray" variant
 * shares the SoC but needs its own dump before these numbers are trusted on
 * it; fossil-port/baremetal/boards/fossil_gen4.h holds the runtime copy. */
#define LCD_WIDTH  454
#define LCD_HEIGHT 454
#define BOARD_SCREEN_ROUND   1
#define BOARD_LCD_EVEN_ALIGN 0
#define BOARD_PARTIAL_BUF_LINES 64

/* ---- Boot-progress markers (bring-up only) --------------------------------
 * setup() is long and this watch has no console at all (its UART is not bonded
 * out), so each marker arms a distinct watchdog timeout: the time until the
 * watch reboots names the last marker reached. Table in
 * fossil-port/baremetal/platform/msm_wdog.c. Identical contract to the Gen 6. */
#if defined(WDOG_TRACE)
extern "C" void wdog_stage(unsigned stage);
#define OWF_STAGE(n) wdog_stage(n)
#endif

/* VISUAL_TRACE: solid-colour milestone frames on the glass (fb_mdp3.c).
 * Last colour visible at death = last milestone survived. */
#if defined(VISUAL_TRACE)
#include <cstdint>
extern "C" void fb_trace(uint32_t xrgb);
#define OWF_VTRACE(c) fb_trace(c)
#endif

/* ---- Panel brightness: DCS 0x51 over the fossil-port DSI host --------------
 * The AUO h139 is qcom "bl_ctrl_dcs" — there is no PWM backlight on an AMOLED.
 * board_display_set_brightness() in the .ino routes here under
 * BOARD_PLATFORM_FOSSIL. */
extern "C" int dsi_dcs_set_brightness(unsigned char level);

/* ---- Dummy pin / bus constants (same contract as the Maix port) ------------ */
#define IIC_SDA          0
#define IIC_SCL          0
#define TP_INT           0
#define TP_RESET         0
/* Virtual pins, exactly as on the Gen 6 (see compat/arduino_glue.cpp): the
 * motor is behind the PMIC's vibrator block and the crown button behind
 * qpnp-power-on's RT status, so neither is a real GPIO. digitalWrite(200)
 * drives vib_set(); digitalRead(201) reads LOW while the button is held. */
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

/* ---- Rotating crown -------------------------------------------------------
 * The Gen 4's crown is a PixArt PAT9126 optical motion sensor on the touch I2C
 * bus at 0x75 (DTB pixart_pat9126@75), driven by platform/rot_pat9126.c. This
 * is the ONE watch of the three that can have it: the Gen 6 routes its crown
 * through a separate co-processor, and the TicWatch C2's bezel is decorative
 * (no pixart node in skipjack.dts at all).
 *
 * crown_take_delta() hands up RAW sensor counts, so the three numbers below are
 * where the feel is tuned -- all of them in this header, none in the driver.
 * They are FIRST GUESSES: nobody has yet turned this wheel with a log attached,
 * and the counts-per-revolution of an optical sensor reading a moulded pattern
 * is not something the device tree tells us. Expect to adjust them once. */
#define BOARD_HAS_CROWN          1
#define CROWN_SCROLL_PX_PER_CNT  18   /* pixels of scroll per sensor count.
                                      * Was 6, which felt pinned at the lowest
                                      * sensitivity -- partly this number and
                                      * partly the ANIMATED scroll that threw
                                      * away each unfinished step (crown_nav.h).
                                      * The animation is gone, so this figure is
                                      * now the whole story: counts x 18 px is
                                      * exactly what the view moves. At the
                                      * measured ~40 counts for a fast roll,
                                      * that is ~720 px per flick. */
#define CROWN_SHADE_OPEN_CNT     12   /* counts of "roll down" that open the shade */
#define CROWN_IDLE_RESET_MS      400  /* quiet gap that clears a part-made gesture */

extern "C" {
  int  crown_init(void);        /* 0 = sensor answered, -1 = absent/unpowered */
  void crown_poll(void);        /* rate-limited internally; call every loop */
  int  crown_take_delta(void);  /* signed counts since the last call */
  int  crown_present(void);
}

#define BOOT_BTN_GPIO    201
#define BOARD_WAKE_GPIO  0
#define BOARD_LCD_BUS_HZ 0
