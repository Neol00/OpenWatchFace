/* ============================================================================
 *  board_ws_s3_touch_amoled_18.h — Waveshare ESP32-S3-Touch-AMOLED-1.8 (HW V1)
 *
 *  ESP32-S3R8 (dual core, 8 MB PSRAM, 16 MB flash), 368x448 SH8601 AMOLED on
 *  QSPI, FT3168 touch, PCF85063 RTC, AXP2101 PMU, QMI8658 IMU, ES8311 codec +
 *  amp, microSD on SDMMC (1-bit).
 *
 *  Close sibling of the S3-2.06 (same SoC, same I2C peripherals, same QSPI and
 *  SDMMC pin assignments). The deltas that matter:
 *
 *    - PANEL IS SH8601, NOT CO5300. Waveshare ships TWO revisions of this board
 *      under one name: HW V1 = SH8601 (examples/Arduino-v3.3.5), HW V2 = CO5300
 *      (examples/Arduino-v3.3.5-v2). The pin maps are byte-identical; only the
 *      panel driver differs. THIS HEADER IS V1. For a V2 unit, flip the two
 *      BOARD_DISPLAY_* flags below (and the offsets — see the V2 note there).
 *    - 368x448, and NO even-alignment constraint (see BOARD_LCD_EVEN_ALIGN).
 *    - NO touch reset line (see TP_RESET note).
 *    - ES8311 is on different pins than the 2.06, and the vendor's pin_config.h
 *      contradicts itself about DIN/DOUT (see the audio block).
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Waveshare ESP32-S3-Touch-AMOLED-1.8"
#define BOARD_VENDOR "Waveshare"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-ws-s3-amoled-18-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for ws-s3-amoled-18", which names the fix. */
#define BOARD_OTA_KEY "ws-s3-amoled-18"

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_SH8601_QSPI 1   /* HW V1. V2 unit: set 0 and CO5300 to 1. */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_TOUCH_FT3168        1
/* PSRAM IS MANDATORY, and it is an IDE/build setting as well as a board fact.
 * This flag only says "the hardware has it" — the build must ALSO enable it
 * (Arduino IDE: Tools > PSRAM = "OPI PSRAM"; arduino-cli: PSRAM=opi). With the
 * flag on but PSRAM disabled in the build, the PSRAM-backed allocations (LVGL
 * buffers, screen cache, stores) fail during early init — BEFORE serial is
 * usable — and the watch sits in a silent boot loop: a truncated/garbled panic,
 * no readable message, and a Saved PC that changes every reset. If you ever see
 * that pattern, CHECK THE PSRAM BUILD SETTING FIRST; it looks exactly like a
 * code fault but no source change will fix it. */
#define BOARD_HAS_PSRAM           1
#define BOARD_DUAL_CORE           1
#define BOARD_HAS_PMU_AXP2101     1
#define BOARD_HAS_RTC_PCF85063    1
#define BOARD_HAS_AUDIO_ES8311    1
#define BOARD_HAS_SD_MMC          1
#define BOARD_HAS_IMU_QMI8658     1
#define BOARD_HAS_BACKLIGHT_PWM   0   /* brightness = SH8601 panel command 0x51 */

/* Haptics: this board has NO vibration motor (the 2.06 does). The Waveshare
 * schematic and pin_config.h have no motor GPIO, and none of the vendor demos
 * drive one. haptics.h compiles to no-ops with the flag off, so every
 * haptics_*() call site stays unchanged. If your unit does have one wired,
 * define BOARD_HAS_HAPTICS 1 + HAPTICS_MOTOR_GPIO/HAPTICS_ACTIVE_HIGH here. */
/* #define BOARD_HAS_HAPTICS 1 */

/* ---- Awake power model (mW; see power_model.h BOARD_PWR_* for the model) -----
 * ESTIMATES, like the 2.06's — scaled from that board's numbers, which were
 * themselves scaled from the MEASURED S3-1.47. Same SoC, same PMU, same rails;
 * the ONLY significant difference is panel area: 368x448 = 164.9k px vs the
 * 2.06's 410x502 = 205.8k px, i.e. ~80%.
 *   FLOOR:  the non-screen draw (SoC idle + PMU + panel controller) barely moves
 *           with panel size, so this is nearly unchanged — trimmed a little for
 *           the smaller panel controller and the absent vibration motor driver.
 *   SCREEN: emissive AMOLED, so it scales roughly with lit area -> ~0.8x the
 *           2.06's span. (Negative by convention, same as the 2.06.)
 *   IMU:    identical part, identical duty cycle -> identical.
 * Tune if you ever meter this unit; until then battery health leans on the
 * gauge-vs-voltage capacity learner (the AXP2101 smart path), not this model. */
#define BOARD_PWR_FLOOR_MW        360.0f   /* est: ~2.06 floor, smaller panel ctrl */
#define BOARD_PWR_SCREEN_MW_FULL  -40.0f   /* est: emissive AMOLED, ~0.8x 2.06 area */
#define BOARD_PWR_IMU_MW          2.0f     /* QMI8658 pedometer; same part */

#define XPOWERS_CHIP_AXP2101          /* required before XPowersLib.h */

/* Extended NVS: the firmware's Preferences store lives in the 1 MB "nvsext"
 * partition (app1's old slot — no OTA on this watch), not the 20 KB head "nvs"
 * (which stays for the system consumers: WiFi PHY/cal, NimBLE bonds).
 * Use partitions_s3_16mb.csv for this board — its 16 MB layout already matches
 * this flash size exactly and defines "nvsext". */
#define BOARD_NVS_EXT_LABEL "nvsext"

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: SH8601 AMOLED 368x448\n" \
  "Touch:   FT3168 capacitive\n" \
  "RTC:     PCF85063\n" \
  "IMU:     QMI8658\n" \
  "PMU:     AXP2101"

/* ---- BLE TX-power ladder (7 tiers: Min,VLow,Low,Mid,High,VHigh,Max) -------
 * Same ESP32-S3 radio as the 2.06 (-24..+20 dBm), so the same ladder. */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -24, -18, -12, -6, 0, 9, 20 }

/* ---- Display: SH8601 AMOLED over QSPI ------------------------------------
 * QSPI pins are IDENTICAL to the 2.06 (Waveshare reused the layout). */
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  11
#define LCD_CS    12
/* NO panel reset GPIO is broken out on this board: the vendor demos construct
 * the panel with GFX_NOT_DEFINED for RST and the SH8601 is brought up by the
 * software-reset/sleep-out path in its init table instead.
 *
 * LCD_RESET IS DELIBERATELY NOT DEFINED HERE. Several places test a pin macro
 * with #ifdef and then hand it straight to a GPIO API:
 *     #ifdef LCD_RESET
 *       gpio_hold_dis((gpio_num_t)LCD_RESET);   // -1 -> invalid GPIO -> abort
 * A "#define LCD_RESET GFX_NOT_DEFINED" (-1) is VISIBLE to those #ifdefs, so it
 * would satisfy the guard and then pass an invalid pin number — an IDF assert
 * that fires before serial is up (silent boot loop, no readable panic). Leaving
 * it undefined makes every one of those guards do the right thing automatically.
 * BOARD_LCD_HAS_RESET is the explicit flag for code that must ask, and the panel
 * constructor passes the library's GFX_NOT_DEFINED directly (see the .ino) —
 * that constant belongs to Arduino_GFX and is only in scope there anyway. */
#define BOARD_LCD_HAS_RESET 0
#define LCD_WIDTH  368
#define LCD_HEIGHT 448
/* The SH8601 on this 368x448 module addresses from (0,0) — the vendor demo
 * constructs it with no offsets at all. (Contrast the 2.06's CO5300, which
 * needs col_offset1=22. A V2/CO5300 unit of THIS board uses 16 — see the V2
 * example's Arduino_CO5300(..., 16, 0, 0, 0).) */
#define LCD_COL_OFFSET1 0
#define LCD_ROW_OFFSET1 0
#define LCD_COL_OFFSET2 0
#define LCD_ROW_OFFSET2 0
#define BOARD_LCD_BUS_HZ 80000000   /* QSPI clock; panel default is 40 MHz */
/* The CO5300 needs even-aligned draw areas; the SH8601 does NOT — its
 * writeAddrWindow() passes x/w and y/h straight to CASET/PASET with no rounding
 * or parity requirement (verified in Arduino_SH8601.cpp). So the LVGL rounder
 * callback stays off here, which also means slightly less over-draw per tile. */
#define BOARD_LCD_EVEN_ALIGN 0

/* Lines per LVGL partial render buffer (x2 buffers, internal SRAM).
 * SIZING IS FIXED AT COMPILE TIME — never auto-size from boot free-heap
 * (see the PARTIAL render comment in the .ino). */
#define BOARD_PARTIAL_BUF_LINES 90

/* ---- Touch: FT3168 + shared I2C bus (touch/PMU/RTC/IMU) -------------------
 * Same SDA/SCL as the 2.06. TP_INT differs: 21 here (it is 38 on the 2.06).
 * NOTE GPIO21 is the 2.06's IMU INT — on THIS board 21 is the TOUCH int, and the
 * IMU int is not broken out (see the IMU block). */
#define IIC_SDA  15
#define IIC_SCL  14
#define TP_INT   21
/* NO touch reset line on this board. The vendor's FT3168 constructor passes
 * DRIVEBUS_DEFAULT_VALUE for the reset argument (i.e. "unused"), and GPIO9 —
 * which IS the touch reset on the 2.06 — is the I2S bit clock here, so it
 * definitively is not a touch reset. Left undefined; the touch bring-up skips
 * the reset pulse when TP_RESET is not defined. */
/* #define TP_RESET <none> */

/* ---- IMU: QMI8658 6-axis, on the shared touch I2C bus (SCL=14 / SDA=15) ----
 * Same part and same address as the 2.06/C6. The INT line is NOT broken out on
 * this board (GPIO21, the 2.06's IMU INT, is the touch INT here). That is fine:
 * the INT is only needed for interrupt-driven data-ready / step-event wakeups,
 * and our pedometer uses POLLED register reads. IMU_QMI8658_INT is therefore
 * left undefined rather than pointed at a wrong pin. */
#define IMU_QMI8658_ADDR 0x6B
/* #define IMU_QMI8658_INT <not broken out> */
/* Deep-sleep step counting via the RISC-V ULP (bit-banged I2C to the IMU on
 * GPIO14/15 — same pins as the 2.06, so the existing ULP blob applies as-is).
 * Requires the custom libs built with CONFIG_ULP_COPROC_TYPE_RISCV=y. */
#define BOARD_HAS_ULP_STEPS 1

/* ---- microSD on SDMMC (1-bit) — same pins as the 2.06. Core 3.3.x variants
 * may already define these as macros in pins_arduino.h; guard so we only add
 * what's missing. ---------------------------------------------------------- */
/* As #define (not const int): board.h is pulled in by lv_conf.h, which reaches
 * both C++ and ASSEMBLER translation units in the LVGL/IDF build — a `const int`
 * statement is illegal in those ("unknown opcode 'const'") and also redefined on
 * a double include. Plain macros are safe everywhere and redefine cleanly.
 * (The vendor pin_config.h declares these as `const int`, which is exactly the
 * form that breaks here — hence the restatement as macros.) */
#ifndef SDMMC_CLK
#define SDMMC_CLK  2
#endif
#ifndef SDMMC_CMD
#define SDMMC_CMD  1
#endif
#ifndef SDMMC_DATA  /* variant exposes this as SDMMC_D0, so we always define it */
#define SDMMC_DATA 3
#endif
/* No SDMMC_CS: 1-bit SDMMC has no chip select (the 2.06 defines one for an
 * unused SPI-mode fallback; nothing on this board needs it). */

/* ---- Audio: ES8311 codec (I2S) + amp gate --------------------------------
 * DIFFERENT PINS than the 2.06 — do not copy that board's values.
 *
 * CAREFUL: Waveshare's pin_config.h defines two contradictory alias sets for
 * the data lines:
 *     I2S_DI_IO 10 / I2S_DO_IO 8      vs      DOPIN 10 / DIPIN 8
 * i.e. GPIO10 is called "DI" by one and "DO" by the other. The tie-breaker is
 * the working 15_ES8311 demo, which calls:
 *     i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO)
 * -> Arduino's setPins(bclk, ws, dout, din, mclk) order, so the ESP's DOUT
 * (codec input / playback) is GPIO8 and the ESP's DIN (codec output / mic
 * capture) is GPIO10. The I2S_*_IO names are the correct ones; the DOPIN/DIPIN
 * aliases are swapped. Naming below follows OUR convention (DOUT = ESP -> codec,
 * matching the 2.06's AUDIO_PIN_DOUT), so playback goes out on 8. */
#define AUDIO_PIN_BCLK 9
#define AUDIO_PIN_LRCK 45
#define AUDIO_PIN_DOUT 8    /* ESP -> codec (playback). Vendor: I2S_DO_IO. */
#define AUDIO_PIN_DIN  10   /* codec -> ESP (mic capture). Vendor: I2S_DI_IO. */
#define AUDIO_PIN_MCLK 16
#define AUDIO_PIN_CE   46   /* amp/codec power gate ("PA"), active HIGH */

/* ---- Buttons / deep-sleep wake -------------------------------------------- */
#define BOOT_BTN_GPIO 0   /* pull-up, LOW = pressed */
/* The S3 wakes from deep sleep on the BOOT press via EXT0 (an RTC-IO; GPIO0 is
 * RTC-capable). board_sleep.h arms it and classifies the wake cause. */
#define BOARD_WAKE_USE_EXT0 1
