/* ============================================================================
 *  board_ws_s3_touch_lcd_169.h — Waveshare ESP32-S3-Touch-LCD-1.69
 *
 *  Hardware: ESP32-S3 (dual core, 8 MB OPI PSRAM, 16 MB flash), 240x280 ST7789V2
 *  LCD on classic SPI + PWM backlight, CST816T capacitive touch, PCF85063 RTC,
 *  QMI8658 IMU, ETA6098 charger with the battery on an ADC divider, a magnetic
 *  BUZZER on a transistor-driven GPIO, and a soft power LATCH (SYS_EN).
 *
 *  Everything below is verified against the vendor schematics (both revisions)
 *  and the vendor demo code in ESP32-waveshare-provided-S3-1.69/.
 *
 *  What makes this board DIFFERENT from the S3-Touch-LCD-2 it is closest to:
 *    - PANEL is an ST7789V2 at 240x280 — the controller's RAM is 240x320, so the
 *      280-tall glass sits at a 20-row offset (the vendor demos construct
 *      Arduino_ST7789 with row offset1 = 20; verbatim below). Same Arduino_ST7789
 *      class, complete library init, no vendor register table.
 *    - LCD_RESET EXISTS here (GPIO8) — unlike the LCD-2 — so BOARD_LCD_HAS_RESET
 *      keeps its default of 1 and the cold-boot reset-hold in the .ino drives it.
 *    - TOUCH is a CST816T with a REAL reset line (TP_RESET 13, pulsed before the
 *      probe) — the LCD-2's CST816D has none. Kept POLLED like the LCD-2
 *      (TP_INT -1): the in-tree cst816 driver has no ISR path, and polling at
 *      LVGL's indev period is the proven arrangement. The physical INT wire is
 *      on GPIO14 if an interrupt path is ever added.
 *    - PCF85063 RTC chip on the shared I2C bus @0x51 (same part as the S3-2.06 /
 *      S3-1.8 / T5) — time survives power-off, board_clock.h drives it directly.
 *      Its INT output is wired (GPIO39 on V2.1, GPIO41 on V1) but unused.
 *    - BUZZER: a magnetic buzzer behind an SS8050 transistor — the PWM audio
 *      backend (BOARD_HAS_AUDIO_PWM) drives it for alarms/chimes/notification
 *      sounds. First board to actually enable that backend.
 *    - SOFT POWER LATCH: battery power flows through a P-FET that the PWR key
 *      (Key2) forces on only while held; SYS_EN must go HIGH to keep the board
 *      alive after release. BOARD_PWR_LATCH_GPIO makes setup() assert it first
 *      thing, board_sleep.h HOLDS it through deep sleep, and enter_power_off()
 *      drops it for a TRUE off (supply collapses on battery; on USB VBUS keeps
 *      the 3V3 rail regardless and the code falls through to deep sleep).
 *    - NO microSD, NO camera, NO motor. Files app is FFat-only.
 *
 *  HARDWARE REVISIONS — V2.1 (current stock) vs V1. The module is an octal-PSRAM
 *  S3 (8 MB OPI), whose PSRAM claims GPIO35-37 inside the module — which is why
 *  V1's SYS_EN=35 / SYS_OUT=36 could never work on these units and Waveshare
 *  moved them. V2.1 (DEFAULT here):
 *      buzzer 42, SYS_EN 41, SYS_OUT 40, RTC_INT 39
 *  V1 (define BOARD_169_HW_V1 to 1 if you really have one):
 *      buzzer 33, SYS_EN 35, SYS_OUT 36, RTC_INT 41
 *  Symptoms of the wrong choice: buzzer silent, and/or the watch dies the moment
 *  the PWR button is released on battery (latch never asserted).
 *
 *  BUILD NOTES for this board:
 *    - Select it: set BOARD_SELECT to BOARD_ID_S3_169 in board.h.
 *    - CORE: ESP32-S3 — same custom esp32s3-libs package + LVGL band-split /
 *      async-flush patches as the other S3 boards. Stock core also builds.
 *    - PARTITIONS: 16 MB flash — use partitions_s3_16mb.csv (Partition Scheme =
 *      Custom, that file as the active partitions.csv), exactly like the LCD-2.
 *    - IDE: enable PSRAM (OPI) — a silent boot loop is the classic symptom of
 *      building an 8 MB-PSRAM board with PSRAM disabled.
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Waveshare ESP32-S3-Touch-LCD-1.69"
#define BOARD_VENDOR "Waveshare"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-ws-s3-lcd-169-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for ws-s3-lcd-169", which names the fix. */
#define BOARD_OTA_KEY "ws-s3-lcd-169"

/* ---- Hardware revision (see the REVISIONS block in the header comment) ----- */
#ifndef BOARD_169_HW_V1
#define BOARD_169_HW_V1 0        /* 0 = V2.1 pin set (default); 1 = original V1 */
#endif

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_DISPLAY_JD9853_SPI  0
#define BOARD_DISPLAY_ST7789_SPI  1   /* real ST7789V2 — library init, no vendor table */
#define BOARD_TOUCH_FT3168        0
#define BOARD_TOUCH_AXS5106L      0
#define BOARD_TOUCH_CST816        1   /* CST816T, polled over the shared I2C bus */
#define BOARD_HAS_PSRAM           1   /* 8 MB OPI PSRAM on-module */
#define BOARD_DUAL_CORE           1   /* S3 is dual-core (band-split render, task pinning) */
#define BOARD_HAS_PMU_AXP2101     0
#define BOARD_HAS_ADC_BATTERY     1   /* no PMU; ETA6098 charger + ADC divider (GPIO1) */

/* Extended NVS: Preferences live in the 1 MB "nvsext" partition of
 * partitions_s3_16mb.csv, not the 20 KB head "nvs" (kept for the system stores:
 * WiFi PHY/cal, BLE bonds). Same arrangement as every other 16 MB S3 board. */
#define BOARD_NVS_EXT_LABEL "nvsext"
#define BOARD_HAS_RTC_PCF85063    1   /* PCF85063 @0x51 on the shared I2C bus */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       1   /* magnetic buzzer on AUDIO_PWM_PIN (below) */
#define BOARD_HAS_HAPTICS         0   /* no motor fitted */
#define BOARD_HAS_SD_MMC          0   /* no card slot on this board */
#define BOARD_HAS_SD_SPI          0
#define BOARD_HAS_IMU_QMI8658     1   /* on the touch I2C bus, addr 0x6B */
#define BOARD_HAS_BACKLIGHT_PWM   1   /* brightness = PWM on LCD_BL */

/* ---- Awake power model (mW; see power_model.h BOARD_PWR_* for the model) -----
 * Dual-core S3, so inherit the S3 CPU coefficient defaults (like the LCD-2). It
 * HAS a QMI8658, so account for the IMU draw while the pedometer runs. NB the
 * soft latch's own bias chain (~10k off VBAT while latched) is a constant ~0.4 mA
 * that the sleep-floor learner will fold in on its own. MEASURE to refine. */
#define BOARD_PWR_IMU_MW          2.0f  /* QMI8658 ~0.5 mA @ ~3.9 V (pedometer); MEASURE to refine */

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: ST7789V2 LCD 240x280\n" \
  "Touch:   CST816T capacitive\n" \
  "RTC:     PCF85063\n" \
  "IMU:     QMI8658\n" \
  "Audio:   buzzer (PWM)\n" \
  "PMU:     ETA6098 (ADC battery)"

/* ---- BLE TX-power ladder (7 tiers: Min,VLow,Low,Mid,High,VHigh,Max) -------
 * ESP32-S3 controller — same enum/range as the other S3 boards (-24..+20 dBm).
 * settings_store.h owns the shared WiFi ladder + tier names. */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -24, -18, -12, -6, 0, 9, 20 }

/* ---- Display: ST7789V2 LCD over classic SPI -------------------------------
 * Pins per the vendor schematic + every vendor demo (they agree; identical in
 * both hardware revisions). MISO is -1: nothing else shares this SPI host (no
 * SD slot) and the panel is write-only.
 *
 *  OFFSETS: the ST7789's RAM is 240x320 and the 280-tall glass is windowed at a
 *  20-row offset — the vendor demos construct Arduino_ST7789 with offsets
 *  (col1 0, row1 20, col2 0, row2 0), taken VERBATIM here. We only ever run
 *  rotation 0, which uses the offset1 pair. */
#define LCD_DC    4
#define LCD_CS    5
#define LCD_SCLK  6
#define LCD_MOSI  7
#define LCD_MISO  -1   /* write-only panel, no shared SD on this host */
#define LCD_RESET 8
#define LCD_BL    15
#define LCD_WIDTH  240
#define LCD_HEIGHT 280
#define LCD_COL_OFFSET1 0
#define LCD_ROW_OFFSET1 20
#define LCD_COL_OFFSET2 0
#define LCD_ROW_OFFSET2 0
/* 80 MHz as on the LCD-2's ST7789T3 (same controller family, proven there). The
 * vendor demos leave the GFX default (40 MHz) — if this unit ever shows flush
 * artifacts/tearing at 80, drop this to 40000000 before suspecting anything else. */
#define BOARD_LCD_BUS_HZ 80000000
#define BOARD_LCD_EVEN_ALIGN 0      /* ST7789 has no even-alignment requirement */

/* Lines per LVGL partial render buffer (x2 buffers, internal SRAM, DMA-capable).
 * Each buffer costs LCD_WIDTH * lines * 2 = 240 * lines * 2 bytes — same width
 * as the LCD-2, so its proven value carries over unchanged (see that board's
 * header for the async-DMA per-transfer ceiling rationale). */
#define BOARD_PARTIAL_BUF_LINES 90

/* ---- Touch: CST816T over I2C (shared with the QMI8658 IMU + PCF85063 RTC) --
 * POLLED like the LCD-2's CST816D: TP_INT is -1 (the "no interrupt" sentinel ->
 * BOARD_HAS_TOUCH_INT 0), which compiles out the interrupt-flag fast path that
 * would otherwise leave touch permanently dead (see board.h). The panel's INT
 * wire physically exists on GPIO14 if an ISR path is ever built.
 * UNLIKE the LCD-2 there is a REAL touch reset line: TP_RESET 13. Defining it
 * turns on the #ifdef TP_RESET sites — the pre-probe reset pulse in the .ino's
 * CST816 bring-up, and board_sleep.h's hold-in-reset-through-deep-sleep (the
 * C6-1.47 pattern that stops the controller floating/scanning while "off"). */
#define IIC_SDA  11
#define IIC_SCL  10
#define TP_INT   -1   /* polled (INT wire exists on GPIO14 but no ISR path is built) */
#define TP_RESET 13

/* ---- Battery sense (ADC, no PMU) ------------------------------------------
 * The ETA6098 charges the cell; the battery reaches the ADC through a 200k/100k
 * divider (schematic R3/R7), so the pin sees ONE THIRD of the cell voltage and
 * battery mV = reading_mv * 3 — the exact LCD-2 arrangement. GPIO1 = ADC1_CH0:
 * ADC1, so it never collides with the WiFi driver (unlike ADC2 boards).
 * The divider is always connected (no enable FET) -> a small constant leak. */
#define BOARD_BATT_ADC_GPIO 1
#define BOARD_BATT_ADC_MUL  3     /* battery_mv = pin_mv * 3 (200k/100k -> 1/3) */

/* Per-unit calibration trim (integer scale = NUM/DEN) for the divider's resistor
 * tolerance. Default 1:1 (no trim) — measure the true cell voltage at the terminal
 * (unplugged, at rest) against the reported value on YOUR unit and set NUM/DEN if
 * it reads off. It is a MULTIPLIER, since divider error scales with voltage. */
#define BOARD_BATT_CAL_NUM  1
#define BOARD_BATT_CAL_DEN  1

/* ---- IMU ------------------------------------------------------------------- */
#define IMU_QMI8658_ADDR 0x6B     /* schematic marks the strap: 0x6B */
/* No ULP deep-sleep step counting: the S3 ULP step blob bit-bangs the IMU on
 * GPIO14/15 (baked into the blob — see the S3-1.64's identical note), but this
 * board's IMU bus is GPIO10/11 and 14/15 are the touch INT / backlight. Steps
 * count while awake, as on the LCD-2. INT1 is wired to GPIO38 but unused (the
 * pedometer reads registers polled). */

/* ---- Buzzer: PWM audio backend --------------------------------------------
 * A magnetic buzzer behind an SS8050 NPN (base via 1k off the GPIO) — exactly
 * what BOARD_HAS_AUDIO_PWM's LEDC square-wave backend expects on one pin. Drives
 * alarm/timer melodies and the notification/hour chimes via audio_alarm.h.
 * Active HIGH; audio_alarm parks it LOW and LATCHES it through deep sleep. */
#if BOARD_169_HW_V1
#define AUDIO_PWM_PIN 33
#else
#define AUDIO_PWM_PIN 42
#endif

/* ---- Soft power latch + PWR key -------------------------------------------
 * Battery power reaches the 3V3 LDO through a P-FET (Q5). Pressing the PWR key
 * (Key2) forces the FET on only WHILE HELD; SYS_EN HIGH (via an NPN that pins
 * the FET gate) is what keeps the board alive after release. So:
 *   - setup() drives BOARD_PWR_LATCH_GPIO HIGH before anything else,
 *   - board_sleep.h holds it HIGH through deep sleep (digital pad -> rides the
 *     global gpio_deep_sleep_hold_en switch),
 *   - enter_power_off() drops it for a TRUE power-off on battery. On USB, VBUS
 *     feeds the LDO around the latch (Q4), so "off" there falls through to the
 *     usual no-wake deep sleep and the unit stays flashable.
 * The latch's bias chain burns ~0.4 mA off VBAT whenever the system is on —
 * including deep sleep. That is the vendor circuit, not a firmware choice; the
 * only lower state is the true off above (revived by a PWR-key press).
 *
 * BOARD_PWR_KEY_GPIO is the PWR key's sense line (SYS_OUT, reads LOW when
 * pressed per the vendor key demo). No firmware consumer yet — navigation and
 * deep-sleep wake use the BOOT key below; this is documentation for a future
 * side-key feature. NB it is NOT deep-sleep-wake capable (not an RTC pad). */
#if BOARD_169_HW_V1
#define BOARD_PWR_LATCH_GPIO 35
#define BOARD_PWR_KEY_GPIO   36
#else
#define BOARD_PWR_LATCH_GPIO 41
#define BOARD_PWR_KEY_GPIO   40
#endif

/* ---- Buttons / deep-sleep wake --------------------------------------------
 * Key1 (BOOT) is on GPIO0 (Key3 is the EN/reset key; Key2 is the PWR key above).
 * GPIO0 is RTC-capable, so the S3 wakes from deep sleep on the BOOT press via
 * EXT0 (board_sleep.h arms it and classifies the wake cause). */
#define BOOT_BTN_GPIO 0   /* Key1; pull-up, LOW = pressed */
#define BOARD_WAKE_USE_EXT0 1
