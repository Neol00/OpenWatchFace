/* ============================================================================
 *  board_fossil_gen6.h — Fossil Gen 6 (hoki), BARE-METAL on one Cortex-A53 (AArch32)
 *
 *  Snapdragon Wear 4100+ (SDA429W / sdm429w), quad A53 booted as 32-bit ARM.
 *  Runs bare-metal on core 0 atop the fossil-port/baremetal runtime (FreeRTOS
 *  Cortex-A port + ported MSM drivers). This header describes only what the
 *  FIRMWARE needs to know about the Gen 6; SoC addresses live in
 *  fossil-port/baremetal/boards/fossil_gen6.h. See fossil-port/HARDWARE-GEN6.md.
 *
 *  Like the Gen 4 header (and the Maix port before it): every capability starts
 *  OFF so its module compiles to stubs, and each switches ON only when its
 *  bare-metal driver lands. This is the Tuya-style "stub first, boot, then fill
 *  in drivers" approach — the whole point is to reach a compiling, linking image
 *  before any peripheral driver exists.
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Fossil Gen 6"
#define BOARD_VENDOR "Fossil"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-fossil-gen6-hoki-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for fossil-gen6-hoki", which names the fix. */
#define BOARD_OTA_KEY "fossil-gen6-hoki"

/* ---- Platform flag: bare-metal Qualcomm MSM (no Arduino/ESP runtime) ------
 * Gates out ESP-only bring-up in the .ino exactly like BOARD_PLATFORM_MAIX /
 * _TUYA do; display/touch/tick come from the fossil-port runtime. */
#define BOARD_PLATFORM_FOSSIL 1

/* ---- Display / touch: served by the fossil-port bare-metal drivers ---------
 * The Gen 6 display path is the continuous-splash framebuffer (fb_splash.c) for
 * now, not a from-scratch DSI driver — but from the FIRMWARE's side the panel is
 * still "an MSM display the runtime owns", so the same BOARD_DISPLAY_MSM_DSI gate
 * the Gen 4 uses applies (it only selects the non-Arduino display path in the
 * .ino). Touch controller is unknown until the dtbo dump; RAYDIUM keeps the same
 * stubbed input path compiling until the real driver is identified. */
#define BOARD_DISPLAY_MSM_DSI 1   /* MSM display owned by the runtime (splash-fb today) */
#define BOARD_TOUCH_RAYDIUM   1   /* placeholder input path; real controller = Phase 4 */

/* ---- Capabilities: all OFF until their phase lands ------------------------- */
#define BOARD_HAS_PSRAM           0   /* plain malloc into DDR (1 GB — no PSRAM tiers) */
#define BOARD_DUAL_CORE           0   /* cores 1-3 parked; single-core for now */
#define BOARD_HAS_PMU_AXP2101     0   /* PMIC is PM660 over SPMI (own driver later) */
#define BOARD_HAS_ADC_BATTERY     0   /* battery via PM660 fuel gauge (Phase 6) */
#define BOARD_HAS_RTC_PCF85063    0   /* PM660 RTC over SPMI (Phase 5) */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0
#define BOARD_HAS_HAPTICS         1   /* PM660 qpnp-haptics over SPMI (pmic_vib.c);
                                         driven via the virtual motor pin below */
#define BOARD_HAS_SD_MMC          0   /* eMMC via sdhci-msm (Phase 5) */
#define BOARD_HAS_SD_SPI          0
#define BOARD_HAS_IMU_QMI8658     0   /* Gen 6 IMU part TBD — own flag later */
#define BOARD_HAS_BACKLIGHT_PWM   0   /* AMOLED: brightness by panel command */
#define BOARD_HAS_LP_STEPS        0
#define BOARD_WAKE_USE_EXT0       0   /* wake = PMIC PON / RTC alarm (Phase 6) */
#define BOARD_HAS_BLE             0   /* Phase 7: NimBLE host over HCI-on-SMD (WCNSS) */
#define BOARD_HAS_FFAT            1   /* eMMC userdata FFAT region (2026-08-04) */

/* BLE TX-power ladder placeholder (same contract as the Gen 4 / Maix ports:
 * referenced unconditionally by settings_store.h, never applied while BLE off). */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9, \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -15, -12, -9, -6, 0, 9, 20 }

#define BOARD_HW_SUMMARY \
  "Host:    Fossil Gen 6 (Wear 4100+, SDA429W, bare-metal A53/AArch32)\n" \
  "Display: MIPI-DSI AMOLED via MDP5 (continuous-splash reuse)\n" \
  "Touch:   TBD (needs dtbo dump)\n" \
  "Storage: eMMC (pending)"

/* ---- Screen geometry ------------------------------------------------------
 * Round AMOLED. 416x416 is the widely-reported hoki panel; UNCONFIRMED — the
 * panel node is not in the published DTB. The runtime's fb_splash.c AUTO-DETECTS
 * the real geometry from what aboot programmed into the MDP, so this is only a
 * fallback for compile-time sizing. */
#define LCD_WIDTH  416
#define LCD_HEIGHT 416
#define BOARD_SCREEN_ROUND   1
#define BOARD_LCD_EVEN_ALIGN 0
#define BOARD_PARTIAL_BUF_LINES 64

/* ---- Boot-progress markers (bring-up only) --------------------------------
 * setup() is a long function and the watch has no console, so each marker sets
 * a distinct watchdog timeout; the time until the watch reboots names the last
 * marker reached. See fossil-port/baremetal/platform/msm_wdog.c for the table. */
#if defined(WDOG_TRACE)
extern "C" void wdog_stage(unsigned stage);
#define OWF_STAGE(n) wdog_stage(n)
#endif

/* VISUAL_TRACE: solid-color milestone frames on the glass (fb_splash.c).
 * Last color visible at death = last milestone survived. */
#if defined(VISUAL_TRACE)
#include <cstdint>
extern "C" void fb_trace(uint32_t xrgb);
#define OWF_VTRACE(c) fb_trace(c)
#endif

/* ---- Panel brightness: DCS 0x51 over the fossil-port DSI host --------------
 * The AUO AMOLED is qcom "bl_ctrl_dcs" — no PWM backlight exists. Implemented
 * in fossil-port/baremetal/platform/dsi_dcs.c; board_display_set_brightness()
 * in the .ino routes here under BOARD_PLATFORM_FOSSIL. */
extern "C" int dsi_dcs_set_brightness(unsigned char level);

/* ---- Haptics: PM660 qpnp-haptics over SPMI ---------------------------------
 * There is no GPIO-switched motor on this watch; the ERM sits behind the PMIC's
 * haptics block (pmic_vib.c, configured by vib_init() during runtime bring-up).
 * haptics.h only ever drives HAPTICS_MOTOR_GPIO through digitalWrite(), so the
 * port routes this VIRTUAL pin number to vib_set() in compat/arduino_glue.cpp.
 * 200 is far outside any real pin numbering — a plain no-op for every other
 * digitalWrite. Keep the two definitions in sync. */
#define HAPTICS_MOTOR_GPIO  200   /* virtual pin -> vib_set(); see arduino_glue.cpp */
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

/* ---- Dummy pin / bus constants (same contract as the Gen 4 / Maix ports) --- */
#define IIC_SDA          0
#define IIC_SCL          0
#define TP_INT           0
#define TP_RESET         0
/* The power/crown button, via the PMIC's qpnp-power-on RT status — another
 * VIRTUAL pin (like the motor above): digitalRead(201) returns LOW while the
 * button is physically held. The .ino's BOOT-button logic (active-LOW, its own
 * debounce) works unmodified on it. 202 = the second pusher ("resin"), unused
 * by the firmware so far. See compat/arduino_glue.cpp. */
#define BOOT_BTN_GPIO    201
#define BOARD_WAKE_GPIO  0
#define BOARD_LCD_BUS_HZ 0
