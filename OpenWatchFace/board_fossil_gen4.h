/* ============================================================================
 *  board_fossil_gen4.h — Fossil Gen 4 (firefish/ray), BARE-METAL on one Cortex-A7
 *
 *  This is NOT an MCU board and NOT a Linux port. The firmware runs bare-metal
 *  on core 0 of the watch's Snapdragon Wear 3100 (APQ8009w / msm8909w-class),
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

#define BOARD_NAME "Fossil Gen 4 (bare-metal)"

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
#define BOARD_HAS_HAPTICS         0   /* vibration motor (PMIC vib / GPIO) — Phase 4+ */
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
  "Host:    Fossil Gen 4 (Wear 2100, bare-metal A7)\n" \
  "Display: MIPI-DSI cmd-mode AMOLED via MDP3\n" \
  "Touch:   Raydium RM_TS (I2C)\n" \
  "Storage: eMMC (pending)"

/* ---- Screen geometry ------------------------------------------------------
 * Round AMOLED. 390x390 is the community-reported firefish resolution; CONFIRM
 * from the stock DTB when the device arrives (HARDWARE.md open question #1) —
 * the smaller "ray" variant differs and gets its own numbers when supported. */
#define LCD_WIDTH  390
#define LCD_HEIGHT 390
#define BOARD_SCREEN_ROUND   1
#define BOARD_LCD_EVEN_ALIGN 0
#define BOARD_PARTIAL_BUF_LINES 64

/* ---- Dummy pin / bus constants (same contract as the Maix port) ------------ */
#define IIC_SDA          0
#define IIC_SCL          0
#define TP_INT           0
#define TP_RESET         0
#define BOOT_BTN_GPIO    0
#define BOARD_WAKE_GPIO  0
#define BOARD_LCD_BUS_HZ 0
