/* ============================================================================
 *  board_ws_s3_touch_amoled_206.h — Waveshare ESP32-S3-Touch-AMOLED-2.06
 *
 *  The original target of this firmware: ESP32-S3R8 (dual core, 8 MB PSRAM),
 *  410x502 CO5300 AMOLED on QSPI, FT3168 touch, PCF85063 RTC, AXP2101 PMU,
 *  ES8311 codec + NS4150B amp, vibration motor, microSD on SDMMC (1-bit).
 *  Values match the previous hard-coded ones exactly (Mylibrary/pin_config.h,
 *  audio_alarm.h, haptics.h, the .ino) — this header just centralizes them.
 * ========================================================================== */
#pragma once

#define BOARD_NAME "Waveshare ESP32-S3-Touch-AMOLED-2.06"

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_CO5300_QSPI 1
#define BOARD_DISPLAY_JD9853_SPI  0
#define BOARD_TOUCH_FT3168        1
#define BOARD_TOUCH_AXS5106L      0
#define BOARD_HAS_PSRAM           1
#define BOARD_DUAL_CORE           1
#define BOARD_HAS_PMU_AXP2101     1
#define BOARD_HAS_RTC_PCF85063    1
#define BOARD_HAS_AUDIO_ES8311    1
#define BOARD_HAS_HAPTICS         1
#define BOARD_HAS_SD_MMC          1
#define BOARD_HAS_SD_SPI          0
#define BOARD_HAS_IMU_QMI8658     0
#define BOARD_HAS_BACKLIGHT_PWM   0   /* brightness = CO5300 panel command 0x51 */

#define XPOWERS_CHIP_AXP2101          /* required before XPowersLib.h */

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: CO5300 AMOLED 410x502\n" \
  "Touch:   FT3168 capacitive\n" \
  "RTC:     PCF85063\n" \
  "IMU:     QMI8658\n" \
  "PMU:     AXP2101"

/* ---- BLE TX-power ladder (7 tiers: Min,VLow,Low,Mid,High,VHigh,Max) -------
 * The ESP_PWR_LVL_* enum is per-radio; the S3 controller spans -24..+20 dBm.
 * Min = hardware floor (-24); VHigh (+9) was the old stock max; Max (+20) is the
 * ceiling. settings_store.h owns the shared WiFi ladder + names; only the BLE
 * enum mapping and its dBm labels are board-specific (see BLE_TXP_LVL there). */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -24, -18, -12, -6, 0, 9, 20 }

/* ---- Display: CO5300 AMOLED over QSPI ------------------------------------ */
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  11
#define LCD_CS    12
#define LCD_RESET 8
#define LCD_WIDTH  410
#define LCD_HEIGHT 502
#define LCD_COL_OFFSET1 22
#define LCD_ROW_OFFSET1 0
#define LCD_COL_OFFSET2 0
#define LCD_ROW_OFFSET2 0
#define BOARD_LCD_BUS_HZ 80000000   /* QSPI clock; panel default is 40 MHz */
#define BOARD_LCD_EVEN_ALIGN 1      /* CO5300 needs even-aligned draw areas */

/* Lines per LVGL partial render buffer (x2 buffers, internal SRAM).
 * SIZING IS FIXED AT COMPILE TIME — never auto-size from boot free-heap
 * (see the PARTIAL render comment in the .ino). */
#define BOARD_PARTIAL_BUF_LINES 104

/* ---- Touch: FT3168 + shared I2C bus (touch/PMU/RTC) ----------------------- */
#define IIC_SDA  15
#define IIC_SCL  14
#define TP_INT   38
#define TP_RESET 9

/* ---- microSD on SDMMC (1-bit). Core 3.3.x variants may already define these
 * as macros in pins_arduino.h; guard so we only add what's missing. ----------*/
/* As #define (not const int): board.h is pulled in by lv_conf.h, which reaches both
 * C++ and ASSEMBLER translation units in the LVGL/IDF build — a `const int` statement
 * is illegal in those ("unknown opcode 'const'") and also redefined on a double
 * include. Plain macros are safe everywhere and redefine-to-same-value cleanly. */
#ifndef SDMMC_CLK
#define SDMMC_CLK  2
#endif
#ifndef SDMMC_CMD
#define SDMMC_CMD  1
#endif
#ifndef SDMMC_DATA  /* variant exposes this as SDMMC_D0, so we always define it */
#define SDMMC_DATA 3
#endif
#ifndef SDMMC_CS
#define SDMMC_CS   17
#endif

/* ---- Audio: ES8311 codec (I2S) + NS4150B amp gate ------------------------- */
#define AUDIO_PIN_BCLK 41
#define AUDIO_PIN_LRCK 45
#define AUDIO_PIN_DOUT 40
#define AUDIO_PIN_MCLK 16
#define AUDIO_PIN_CE   46   /* Codec_CE: codec + NS4150B power gate, active HIGH */

/* ---- Haptics: vibration motor (Q1 MMBT3904 driver) ------------------------ */
#define HAPTICS_MOTOR_GPIO 18
#define HAPTICS_ACTIVE_HIGH 1

/* ---- Buttons / deep-sleep wake -------------------------------------------- */
#define BOOT_BTN_GPIO 0   /* Key1; pull-up, LOW = pressed */
/* The S3 wakes from deep sleep on the BOOT press via EXT0 (an RTC-IO; GPIO0 is
 * RTC-capable). board_sleep.h arms it and classifies the wake cause. */
#define BOARD_WAKE_USE_EXT0 1
