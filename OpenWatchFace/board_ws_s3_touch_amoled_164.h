/* ============================================================================
 *  board_ws_s3_touch_amoled_164.h — Waveshare ESP32-S3-Touch-AMOLED-1.64
 *
 *  ESP32-S3R8 (dual core, 8 MB PSRAM, 16 MB flash), 280x456 SH8601 AMOLED on
 *  QSPI, FT3168 touch, QMI8658 IMU, microSD on SDMMC (1-bit).
 *
 *  Closest sibling is the S3-1.8, not the 2.06: same SH8601-class QSPI AMOLED
 *  and the same panel-command brightness. But this board is a CHEAPER build than
 *  either, and the deltas all come from that:
 *
 *    - NO AXP2101 PMU and NO PCF85063 RTC. Charging is a standalone ETA6098 and
 *      the battery is read on an ADC divider — the same arrangement as the
 *      S3-1.47. Time is kept by the SoC RTC across sleep, and is lost on a full
 *      power-off (no coin cell / RTC chip to hold it); it re-syncs from BLE or
 *      SNTP on the next connect, exactly as on the 1.47.
 *    - NO ES8311 codec and no amp (no audio hardware at all -> silent stub), and
 *      NO vibration motor.
 *    - The PIN MAP IS ITS OWN — it shares almost nothing with the 2.06/1.8 pair.
 *      I2C moved to 47/48, the QSPI block to 9..14+21, SDMMC to 39/40/41.
 *
 *  SOURCE OF THESE VALUES: the vendor demo in
 *  ESP32-waveshare-provided-S3-1.64/ (ESP-IDF/06_LVGL_Test/main/main.c for the
 *  panel + QSPI pins and geometry, components/i2c_bsp for I2C, esp_touch for the
 *  FT3168 address, 03_SD_Card for SDMMC, 01_ADC_Test for the battery ADC).
 *  Values are taken VERBATIM from that demo rather than inferred, except where a
 *  comment below says otherwise.
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Waveshare ESP32-S3-Touch-AMOLED-1.64"
#define BOARD_VENDOR "Waveshare"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-ws-s3-amoled-164-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for ws-s3-amoled-164", which names the fix. */
#define BOARD_OTA_KEY "ws-s3-amoled-164"

/* ---- Feature flags -------------------------------------------------------- */
/* PANEL IS SH8601, on all three pieces of vendor evidence: main.c includes
 * esp_lcd_sh8601.h, builds a sh8601_vendor_config_t + calls
 * esp_lcd_new_panel_sh8601(), and main/idf_component.yml depends on the
 * esp_lcd_sh8601 component. (Waveshare does ship panel-revision variants under
 * one product name — the S3-1.8 exists as both SH8601 and CO5300, see its
 * header. If a future 1.64 revision turns out to be CO5300, flip these two
 * flags and set the offsets per the CO5300 note at LCD_COL_OFFSET1 below;
 * nothing else changes, because the whole QSPI-AMOLED path is shared.) */
#define BOARD_DISPLAY_SH8601_QSPI 1
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
#define BOARD_HAS_ADC_BATTERY     1   /* no PMU; battery sensed on an ADC divider (GPIO4) */
#define BOARD_HAS_SD_MMC          1
#define BOARD_HAS_IMU_QMI8658     1
#define BOARD_HAS_BACKLIGHT_PWM   0   /* brightness = SH8601 panel command 0x51 */

/* Deliberately ABSENT (left at the board.h default of 0), so the matching
 * modules compile to no-op stubs and no call site changes:
 *   BOARD_HAS_PMU_AXP2101  — charger is a standalone ETA6098, no PMU on I2C.
 *   BOARD_HAS_RTC_PCF85063 — no RTC chip; SoC RTC + BLE/SNTP resync (like 1.47).
 *   BOARD_HAS_AUDIO_ES8311 — no codec and no amp on this board -> silent stub.
 *   BOARD_HAS_HAPTICS      — no vibration motor.
 * If your unit differs, define the flag (and its pins) here. */

/* ---- Awake power model (mW; see power_model.h BOARD_PWR_* for the model) -----
 * ESTIMATES, derived the same way as the 1.8's — by scaling the 2.06 numbers,
 * which were themselves scaled from the MEASURED S3-1.47.
 *   FLOOR:  markedly LOWER than the 2.06/1.8 pair, because the two biggest
 *           always-on non-SoC consumers on those boards are absent here: the
 *           AXP2101 PMU and the ES8311 codec rail. What is left is the SoC idle
 *           draw plus a small panel controller — so this sits near the 1.47's
 *           floor rather than the AMOLED boards'.
 *   SCREEN: emissive AMOLED, scaling with lit area. 280x456 = 127.7k px vs the
 *           2.06's 410x502 = 205.8k px, i.e. ~62% -> ~0.62x the 2.06's span.
 *   IMU:    identical part, identical duty cycle -> identical.
 * Tune if you ever meter this unit. NOTE this board has no PMU, so there is no
 * gauge-vs-voltage capacity learner to fall back on — battery health leans on
 * THIS model plus the voltage curve more than it does on the 2.06/1.8. */
#define BOARD_PWR_FLOOR_MW        240.0f   /* est: no PMU, no codec rail */
#define BOARD_PWR_SCREEN_MW_FULL  -31.0f   /* est: emissive AMOLED, ~0.62x 2.06 area */
#define BOARD_PWR_IMU_MW          2.0f     /* QMI8658 pedometer; same part */

/* Extended NVS: the firmware's Preferences store lives in the 1 MB "nvsext"
 * partition (app1's old slot — no OTA on this watch), not the 20 KB head "nvs"
 * (which stays for the system consumers: WiFi PHY/cal, NimBLE bonds).
 * Use partitions_s3_16mb.csv for this board — its 16 MB layout already matches
 * this flash size exactly and defines "nvsext". */
#define BOARD_NVS_EXT_LABEL "nvsext"

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: SH8601 AMOLED 280x456\n" \
  "Touch:   FT3168 capacitive\n" \
  "IMU:     QMI8658\n" \
  "PMU:     ETA6098 (ADC battery)"

/* ---- BLE TX-power ladder (7 tiers: Min,VLow,Low,Mid,High,VHigh,Max) -------
 * Same S3 radio as the 2.06/1.8 (-24..+20 dBm), so the same ladder. */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -24, -18, -12, -6, 0, 9, 20 }

/* ---- Display: SH8601 AMOLED over QSPI ------------------------------------
 * Pins from the vendor LVGL demo's EXAMPLE_PIN_NUM_LCD_* block. Note the whole
 * block differs from the 2.06/1.8 pair (both of which use SDIO 4..7, SCLK 11,
 * CS 12) — do not copy those. Unlike the 1.8, this board DOES break out a panel
 * reset (GPIO21, the demo's EXAMPLE_PIN_NUM_LCD_RST), so LCD_RESET is defined
 * and BOARD_LCD_HAS_RESET keeps its default of 1. */
#define LCD_SDIO0 11
#define LCD_SDIO1 12
#define LCD_SDIO2 13
#define LCD_SDIO3 14
#define LCD_SCLK  10
#define LCD_CS    9
#define LCD_RESET 21
#define LCD_WIDTH  280
#define LCD_HEIGHT 456
/* Column offset 20: the vendor flush callback adds 0x14 to both x coordinates
 * (`area->x1 + 0x14`) before esp_lcd_panel_draw_bitmap — that constant IS the
 * panel's column offset, the same role LCD_COL_OFFSET1 plays here. Rows are not
 * offset (y is passed through untouched).
 * (CO5300-revision note: if a future unit is CO5300, re-derive this from that
 * revision's demo rather than reusing 20 — the 2.06 uses 22.) */
#define LCD_COL_OFFSET1 20
#define LCD_ROW_OFFSET1 0
#define LCD_COL_OFFSET2 0
#define LCD_ROW_OFFSET2 0
#define BOARD_LCD_BUS_HZ 80000000   /* QSPI clock, as on the 2.06/1.8 */
/* THIS PANEL REQUIRES EVEN-ALIGNED DRAW AREAS — same as the 2.06's CO5300, even
 * though the constraint is usually described as a CO5300 one and the S3-1.8's
 * SH8601 does not need it. Do not "simplify" this back to 0.
 *
 * Evidence: BOTH vendor demos register a rounder that snaps x1/y1 down and x2/y2
 * up to even bounds (example_lvgl_rounder_cb in Arduino/.../lcd_bsp.c and in
 * ESP-IDF/.../main.c), and the Arduino one does so while running at
 * LCD_BIT_PER_PIXEL 16 — the same 16bpp this firmware uses. So the rounding is a
 * PANEL requirement here, not an artifact of a 24bpp demo path.
 *
 * Symptoms when this is 0 (all observed on this board, all gone with it at 1):
 *   - stale trails: an odd-aligned partial flush leaves the boundary column/row
 *     unwritten, so old borders (e.g. the quick shade's) stay on screen until
 *     something else redraws that area;
 *   - fragments of the app menu frozen in place after a swipe;
 *   - occasional warped/twisted text and objects, when an odd-width window
 *     misaligns the panel's write pointer and shears everything after it.
 *
 * Defined explicitly rather than omitted: unlike the BOARD_HAS_* flags,
 * BOARD_LCD_EVEN_ALIGN has NO default in board.h's defaults block, and every
 * other board header defines it outright. */
#define BOARD_LCD_EVEN_ALIGN 1

/* Lines per LVGL partial render buffer (x2 buffers, internal SRAM).
 * SIZING IS FIXED AT COMPILE TIME — never auto-size from boot free-heap
 * (see the PARTIAL render comment in the .ino). 120 lines here vs the 2.06's 90:
 * this panel is 280 px wide against the 2.06's 410, so 120 lines costs LESS
 * SRAM per buffer than the 2.06's 90 (280*120 = 33.6k px vs 410*90 = 36.9k px)
 * while covering a larger fraction of the shorter screen. */
#define BOARD_PARTIAL_BUF_LINES 120

/* ---- Touch: FT3168 + shared I2C bus (touch + IMU) -------------------------
 * I2C is GPIO47/48 here (the 2.06/1.8 use 15/14). From the vendor demo's
 * i2c_bsp.c: I2C_MASTER_SDA_IO 47, I2C_MASTER_SCL_IO 48. The FT3168 answers at
 * 0x38 (esp_touch/touch_bsp.c), which is the driver's default. */
#define IIC_SDA  47
#define IIC_SCL  48
/* TP_INT / TP_RESET: the vendor demo POLLS the FT3168 over I2C and never
 * references an interrupt or reset GPIO, so the demo gives us NO value for
 * either — unlike every pin above, these are not in the vendor source, and they
 * are deliberately NOT guessed here.
 *
 * TP_RESET is left UNDEFINED: board_sleep.h tests it with #ifdef, and the
 * S3-1.8 already ships without one, so the no-reset path is precedented (that
 * board monitors instead of hibernating the touch controller).
 *
 * TP_INT, however, MUST be defined: the .ino passes it unguarded to the
 * Arduino_FT3x68 constructor. -1 is the DriveBus library's own "no pin" sentinel
 * (DRIVEBUS_DEFAULT_VALUE): Arduino_IIC.cpp guards `if (_iqr != -1)` around its
 * pinMode/attachInterrupt, so -1 simply skips interrupt setup and the touch
 * driver runs POLLED — which is exactly what the vendor demo does. That is a
 * supported configuration, not a workaround.
 *
 * The one behavioural cost: the ISR-driven s_touch_activity flag in the .ino
 * never fires, so idle-sleep timer refreshes come only from LVGL's indev
 * polling. A very quick tap can therefore be missed by the idle timeout (this
 * is precisely what that flag exists to avoid). If you trace the schematic and
 * find the INT line broken out, put its GPIO here and that cost disappears. */
#define TP_INT   -1

/* ---- IMU: QMI8658 6-axis, on the shared touch I2C bus (47/48) -------------
 * The vendor QMI8658 demo probes BOTH slave addresses (0x6a then 0x6b) and uses
 * whichever answers, so it does not pin down this board's strap. 0x6B is what
 * every other board in this firmware uses and is the part's default when SA0 is
 * high; if the IMU does not come up on your unit, try 0x6A.
 * INT line: not referenced by the vendor demo (it polls), and our pedometer
 * reads registers polled too, so no INT pin is defined. */
#define IMU_QMI8658_ADDR 0x6B
/* Deep-sleep step counting via the RISC-V ULP bit-bangs I2C to the IMU. The blob
 * is built for the 2.06's GPIO14/15 bus; this board's IMU is on GPIO47/48, and
 * the pins are baked into the blob — so ULP steps are OFF here until the blob is
 * rebuilt for this pin pair. Steps still count normally while awake. */
/* #define BOARD_HAS_ULP_STEPS 1 */

/* ---- microSD on SDMMC (1-bit) ---------------------------------------------
 * From 03_SD_Card/sd_card_bsp.c, which configures the SDMMC slot as
 * clk=41, cmd=39, d0=40 (its PIN_NUM_MOSI/MISO names are SPI-flavoured leftovers
 * — the SDMMC slot_config assignment below them is what this board actually
 * runs). Only D0 is wired, so this stays 1-bit (BOARD_HAS_SD_MMC_4BIT off).
 *
 * As #define (not const int): board.h is pulled in by lv_conf.h, which reaches
 * both C++ and ASSEMBLER translation units in the LVGL/IDF build — a `const int`
 * statement is illegal in those ("unknown opcode 'const'") and also redefined on
 * a double include. Plain macros are safe everywhere and redefine-to-same-value
 * cleanly. */
#ifndef SDMMC_CLK
#define SDMMC_CLK  41
#endif
#ifndef SDMMC_CMD
#define SDMMC_CMD  39
#endif
#ifndef SDMMC_DATA  /* variant exposes this as SDMMC_D0, so we always define it */
#define SDMMC_DATA 40
#endif
#ifndef SDMMC_CS
#define SDMMC_CS   38
#endif

/* ---- Battery sense (ADC, no PMU) ------------------------------------------
 * The vendor ADC demo samples ADC_UNIT_1 / ADC_CHANNEL_3 and scales the result
 * by 3 (`*value = 0.001 * vol * 3`) — i.e. the usual 200k/100k divider giving
 * one third of the cell voltage, the same ratio as the 1.47 and the C6-1.47.
 *
 * ADC1_CH3 is GPIO4 on the ESP32-S3 (the demo names only the channel, so the pin
 * is resolved from the S3's fixed ADC1 channel->GPIO map, where ADC1 covers
 * GPIO1..10 as CH0..CH9). Unlike the S3-1.47 — whose divider lands on GPIO12 =
 * ADC2, a unit the WiFi driver takes over while the radio is up — this board is
 * on ADC1, so battery samples never collide with WiFi and always read live.
 *   - BOARD_BATT_ADC_GPIO : the ADC-capable pin the divider feeds (4).
 *   - BOARD_BATT_ADC_MUL  : 1/divider-ratio = scale the pin reading back up (3).
 * The divider is always connected (no enable FET) -> a small constant leak. */
#define BOARD_BATT_ADC_GPIO 4
#define BOARD_BATT_ADC_MUL  3     /* battery_mv = pin_mv * 3 (200k/100k -> 1/3) */

/* Per-unit calibration trim (integer scale = NUM/DEN) for the divider's resistor
 * tolerance. Default 1:1 (no trim) — measure 4.07 V at the terminal vs the
 * reported value on YOUR unit and set NUM/DEN if it reads off (it's a MULTIPLIER,
 * since divider error scales with voltage). */
#define BOARD_BATT_CAL_NUM  1
#define BOARD_BATT_CAL_DEN  1

/* ---- Buttons / deep-sleep wake --------------------------------------------
 * BOOT on GPIO0, as on every S3 board here. GPIO0 is RTC-capable, so the S3
 * wakes from deep sleep on the BOOT press via EXT0 (board_sleep.h arms it and
 * classifies the wake cause). */
#define BOOT_BTN_GPIO 0   /* pull-up, LOW = pressed */
#define BOARD_WAKE_USE_EXT0 1
