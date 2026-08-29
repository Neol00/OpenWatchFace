/* ============================================================================
 *  board_ws_s3_touch_lcd_147.h — Waveshare ESP32-S3-Touch-LCD-1.47
 *
 *  The C6-1.47's twin: SAME 172x320 JD9853 LCD on SPI + PWM backlight, SAME
 *  AXS5106L touch, SAME charge circuit / ADC battery sense (no PMU, no RTC chip).
 *  What DIFFERS from the C6-1.47:
 *    - SoC: ESP32-S3 (DUAL core + 8 MB PSRAM) instead of the single-core C6.
 *    - NO IMU on this unit (the C6-1.47 has a QMI8658; this board omits it), so
 *      no step counter / sleep-tracking IMU path and no LP-core deep-sleep mod.
 *    - microSD is on the dedicated SDMMC peripheral (like the S3-2.06), NOT
 *      sharing the LCD SPI bus as on the C6 — so none of the C6 shared-host
 *      arbitration applies; the proven 1-bit SDMMC backend is reused as-is.
 *    - Different pin map (it's an S3) — see below.
 *    - 16 MB flash (W25Q128JVSI), vs the C6's 8 MB.
 *
 *  BUILD NOTES for this board:
 *    - Select it: set BOARD_SELECT to BOARD_ID_S3_147 in board.h.
 *    - CORE: this is an ESP32-S3, so it can use the SAME custom esp32s3-libs
 *      package + LVGL band-split / async-flush patches as the S3-2.06 (lv_conf.h
 *      picks the threaded OS layer via BOARD_DUAL_CORE automatically). Building
 *      against the stock core also works (no patch-only symbols are required for
 *      bring-up); the patches are a performance win, not a correctness need.
 *    - PARTITIONS: 16 MB flash — the sketch's 32 MB partitions.csv overruns it.
 *      Use partitions_s3_16mb.csv (same layout as the S3-2.06, FFat shrunk): in
 *      the IDE set Partition Scheme=Custom and make that the active partitions.csv
 *      for this build, or wire it via a board build.partitions entry.
 *    - Library: install the demo's esp_lcd_touch_axs5106l into Arduino/libraries
 *      (same library the C6-1.47 uses).
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Waveshare ESP32-S3-Touch-LCD-1.47"
#define BOARD_VENDOR "Waveshare"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-ws-s3-lcd-147-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for ws-s3-lcd-147", which names the fix. */
#define BOARD_OTA_KEY "ws-s3-lcd-147"

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_DISPLAY_JD9853_SPI  1
#define BOARD_TOUCH_FT3168        0
#define BOARD_TOUCH_AXS5106L      1
#define BOARD_HAS_PSRAM           1   /* ESP32-S3 module has 8 MB PSRAM */
#define BOARD_DUAL_CORE           1   /* S3 is dual-core (band-split render, task pinning) */
#define BOARD_HAS_PMU_AXP2101     0
#define BOARD_HAS_ADC_BATTERY     1   /* no PMU; battery sensed on an ADC divider (GPIO12) */

/* Extended NVS: Preferences live in the 1 MB "nvsext" partition (app1's old
 * slot in partitions_s3_16mb.csv — no OTA), not the 20 KB head "nvs" (kept for
 * the system stores: WiFi PHY/cal, BLE bonds). See settings_load(). */
#define BOARD_NVS_EXT_LABEL "nvsext"
#define BOARD_HAS_RTC_PCF85063    0   /* no RTC chip — internal RTC + NVS persist (board_clock.h) */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0
/* Vibration motor — HARDWARE MOD, same recipe as the Tuya T5 (see board_tuya_t5_amoled_175.h):
 * coin ERM switched by a 2N3904 NPN low-side, base driven from IO10 through 1K (102 — NOT
 * 103/10K, which starves the base and the motor barely spins), plus a 10K (103) base->GND
 * pull-down so the motor stays OFF while IO10 floats at boot/reset. IO10 is electrically free
 * on this board: its net runs only to the P1 breakout header (SD is 13-18, LCD/touch 21/38+).
 * Active HIGH: GPIO HIGH -> transistor on -> motor runs. Unlike the Tuya, this is a real
 * ESP32, so haptics.h's gpio_hold_* latch genuinely pins it LOW through deep sleep too. */
#define BOARD_HAS_HAPTICS         1
#define HAPTICS_MOTOR_GPIO        10  /* NPN base via 1K; 10K pull-down to GND; active HIGH */
#define HAPTICS_ACTIVE_HIGH       1
/* Per-board button-tick length (exact blocking pulse; strength is length-only — see
 * haptics.h). Start at the Tuya's felt-good value; trim on hardware: lower if too hard,
 * raise if too faint, floor ~20 ms or a coin ERM never spins up. */
#define HAPTICS_CLICK_MS          28
#define BOARD_HAS_SD_MMC          1   /* dedicated SDMMC peripheral; 4-bit (see BOARD_HAS_SD_MMC_4BIT below) */
#define BOARD_HAS_SD_SPI          0
#define BOARD_HAS_IMU_QMI8658     0   /* NO IMU on this unit (unlike the C6-1.47) */
#define BOARD_HAS_BACKLIGHT_PWM   1   /* brightness = PWM on LCD_BL */

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: JD9853 LCD 172x320\n" \
  "Touch:   AXS5106L capacitive\n" \
  "RTC:     internal (no chip)\n" \
  "IMU:     none\n" \
  "PMU:     ETA6098 (ADC battery)"

/* ---- BLE TX-power ladder (7 tiers: Min,VLow,Low,Mid,High,VHigh,Max) -------
 * ESP32-S3 controller — same enum/range as the S3-2.06 (-24..+20 dBm). Min =
 * hardware floor (-24); Max (+20) the ceiling. settings_store.h owns the shared
 * WiFi ladder + tier names. */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -24, -18, -12, -6, 0, 9, 20 }

/* ---- Display: JD9853 LCD over classic SPI (ST7789-class driver) -----------
 * Same panel as the C6-1.47 (172x320, col/row offsets 34/0/34/0). Pins per the
 * Waveshare S3-1.47 demo (Arduino_ESP32SPI 45/21/38/39). The microSD does NOT
 * share this bus on the S3-1.47 (it's on the SDMMC peripheral), so unlike the
 * C6 we do NOT need to hand a MISO pin to the display bus for the card — but the
 * shared display-bus constructor in the .ino still takes LCD_MISO, so give it
 * the panel's (unused, write-only) MISO line. */
#define LCD_DC    45
#define LCD_CS    21
#define LCD_SCLK  38
#define LCD_MOSI  39
#define LCD_MISO  -1   /* panel is write-only; no SD on this bus -> MISO unused */
#define LCD_RESET 40
#define LCD_BL    46
#define LCD_WIDTH  172
#define LCD_HEIGHT 320
#define LCD_COL_OFFSET1 34
#define LCD_ROW_OFFSET1 0
#define LCD_COL_OFFSET2 34
#define LCD_ROW_OFFSET2 0
#define BOARD_LCD_BUS_HZ 80000000   /* SPI clock; JD9853 demo runs 80 MHz */
#define BOARD_LCD_EVEN_ALIGN 0

/* Lines per LVGL partial render buffer (x2 buffers, internal SRAM, DMA-capable).
 * Each buffer costs LCD_WIDTH * lines * 2 bytes.
 *
 * The S3 has far more free internal SRAM than the C6, so size up from the C6's
 * value for fewer flushes per frame (flushes/frame = ceil(LCD_HEIGHT / lines)).
 *
 * HARD CEILING — one async tile must fit ONE SPI DMA transaction: the JD9853 async
 * flush (Arduino_ESP32SPIDMA::writePixelsAsync) queues a whole tile as a SINGLE DMA
 * transfer of LCD_WIDTH*lines*2 bytes. Above the SPI master's per-transfer DMA max
 * it rejects every flush with "txdata transfer > hardware max supported len" 
 * FIXED at compile time — never auto-size from boot free-heap (that bootloops; see
 * the PARTIAL render comment in the .ino). */
#define BOARD_PARTIAL_BUF_LINES 90

/* ---- Touch: AXS5106L over I2C --------------------------------------------- */
#define IIC_SDA  42
#define IIC_SCL  41
#define TP_INT   48
#define TP_RESET 47

/* ---- microSD on the SDMMC peripheral (4-bit) -----------------------------
 * Dedicated bus (NOT the LCD SPI bus). This board breaks out ALL FOUR SD data
 * lines (D0..D3), so it runs the 4-bit SDMMC bus for ~4x the transfer rate of
 * 1-bit — a real win for loading images / larger files off the card. (The S3-2.06
 * only wires D0, so it stays 1-bit; that's why BOARD_HAS_SD_MMC_4BIT is per-board.)
 * The 4-bit SDMMC backend in sd_card.h uses SDMMC_CLK/CMD/DATA(=D0)/D1/D2/D3. As
 * #define (not const int): board.h is pulled into assembler TUs via lv_conf.h where
 * `const int` is illegal and a double-include redefines; plain macros are safe. */
#define BOARD_HAS_SD_MMC_4BIT 1
#ifndef SDMMC_CLK
#define SDMMC_CLK  16
#endif
#ifndef SDMMC_CMD
#define SDMMC_CMD  15
#endif
#ifndef SDMMC_DATA   /* d0 */
#define SDMMC_DATA 17
#endif
#ifndef SDMMC_D1
#define SDMMC_D1   18
#endif
#ifndef SDMMC_D2
#define SDMMC_D2   13
#endif
#ifndef SDMMC_D3
#define SDMMC_D3   14
#endif

/* ---- Battery sense (ADC, no PMU) ------------------------------------------
 * Per the S3-1.47 schematic the battery reaches BAT_ADC through a 200k/100k
 * divider -> the pin sees ONE THIRD of the cell voltage, so battery mV =
 * reading_mv * 3 (same ratio as the C6-1.47). The demo reads it on GPIO12.
 *
 * IMPORTANT (S3-specific): GPIO12 is on ADC2 (ADC2_CH1), NOT ADC1 — and ADC2 is
 * owned by the WiFi driver while the radio is active. board_power.h's ADC path
 * resolves GPIO->(unit,channel) for the S3 and keeps the last good reading when a
 * sample collides with WiFi (the battery is polled infrequently, so this is fine).
 *   - BOARD_BATT_ADC_GPIO : the ADC-capable pin the divider feeds (12).
 *   - BOARD_BATT_ADC_MUL  : 1/divider-ratio = scale the pin reading back up (3).
 * The divider is always connected (no enable FET) -> a small constant leak. */
#define BOARD_BATT_ADC_GPIO 12
#define BOARD_BATT_ADC_MUL  3     /* battery_mv = pin_mv * 3 (200k/100k -> 1/3) */

/* Per-unit calibration trim (integer scale = NUM/DEN) for the divider's resistor
 * tolerance. Default 1:1 (no trim) — measure 4.07 V at the terminal vs the
 * reported value on YOUR unit and set NUM/DEN if it reads off (it's a MULTIPLIER,
 * since divider error scales with voltage). */
#define BOARD_BATT_CAL_NUM  1
#define BOARD_BATT_CAL_DEN  1

/* ---- Buttons / deep-sleep wake --------------------------------------------
 * Key1 (BOOT) is on GPIO0 (Key2 is the EN/reset key) — same as the S3-2.06.
 * GPIO0 is RTC-capable, so the S3 wakes from deep sleep on the BOOT press via
 * EXT0 (board_sleep.h arms it and classifies the wake cause). */
#define BOOT_BTN_GPIO 0   /* Key1; pull-up, LOW = pressed */
#define BOARD_WAKE_USE_EXT0 1
