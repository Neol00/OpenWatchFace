/* ============================================================================
 *  board_ws_s3_touch_lcd_2.h — Waveshare ESP32-S3-Touch-LCD-2
 *
 *  Hardware: ESP32-S3 (dual core, 8 MB PSRAM, 16 MB flash), 240x320 ST7789T3 LCD
 *  on classic SPI + PWM backlight, CST816D capacitive touch, QMI8658 IMU, microSD
 *  sharing the LCD SPI bus, ETA6096 charger with the battery on an ADC divider,
 *  and a 24-pin DVP camera header (OV5640 fitted on this unit).
 *
 *  What makes this board DIFFERENT from its siblings — none of it is guessable
 *  from the family, so read this before assuming:
 *    - PANEL is a real ST7789T3, NOT the JD9853 of the 1.47 boards. Both are
 *      driven by Arduino_GFX's Arduino_ST7789 class, but the ST7789 needs NO
 *      vendor register table: the library's own init is complete (that is why
 *      there is no display_st7789_2.h next to display_jd9853.h). The panel is
 *      240x320 with ZERO offsets — no 34-px column fudge like the 172-wide ones.
 *    - TOUCH is a CST816D, a new controller for this firmware (BOARD_TOUCH_CST816).
 *      It is POLLED: the vendor bsp hard-codes reset to -1 and never names an INT
 *      GPIO, so TP_INT is -1 (the "no interrupt" sentinel — see BOARD_HAS_TOUCH_INT
 *      in board.h) and TP_RESET is deliberately left UNDEFINED so the #ifdef guards
 *      in board_sleep.h / the .ino skip the reset pulse rather than driving pin -1.
 *    - LCD_RESET does NOT exist: every vendor demo passes -1 for the panel reset.
 *      So BOARD_LCD_HAS_RESET is 0 (the S3-1.8 precedent) and LCD_RESET is left
 *      undefined; the .ino substitutes GFX_NOT_DEFINED.
 *    - microSD SHARES the LCD SPI bus (SCLK 39 / MOSI 38 / MISO 40, CS 41) — the
 *      C6-1.47 arrangement, NOT the S3-1.47's dedicated SDMMC peripheral. That
 *      turns on BOARD_SD_SHARES_LCD_BUS and its display<->SD bus arbitration.
 *      Unlike the C6 the card rail is plain always-on, so NO SD_PWR_LDO_CHAN.
 *    - Battery ADC is GPIO5 = ADC1_CH4. ADC1, so unlike the S3-1.47 (GPIO12 on
 *      ADC2) it never collides with the WiFi driver.
 *    - CAMERA: a 24-pin DVP header wired to the S3's LCD_CAM peripheral, with an
 *      OV5640 fitted. BOARD_HAS_CAMERA gates the Camera app. Pins verified against
 *      BOTH vendor camera demos (Arduino 09_lvgl_camera + ESP-IDF 05_lvgl_camera),
 *      which agree exactly.
 *
 *  BUILD NOTES for this board:
 *    - Select it: set BOARD_SELECT to BOARD_ID_S3_LCD2 in board.h.
 *    - CORE: ESP32-S3, so the same custom esp32s3-libs package + LVGL band-split /
 *      async-flush patches as the other S3 boards apply (lv_conf.h picks the
 *      threaded OS layer from BOARD_DUAL_CORE). Stock core also builds.
 *    - PARTITIONS: 16 MB flash — use partitions_s3_16mb.csv (Partition Scheme =
 *      Custom, with that file as the active partitions.csv), exactly as the
 *      S3-1.47 / S3-1.8 / S3-1.64 do. The 32 MB default overruns this flash.
 *    - IDE: enable PSRAM (OPI) — a silent boot loop is the classic symptom of
 *      building an 8 MB-PSRAM board with PSRAM disabled.
 *    - CAMERA: the Camera app needs esp_camera.h, which ships inside the ESP32
 *      Arduino core's precompiled libs for the S3 target (no extra library to
 *      install). Select an S3 board variant so those libs are fetched.
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Waveshare ESP32-S3-Touch-LCD-2"
#define BOARD_VENDOR "Waveshare"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-ws-s3-lcd-2-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for ws-s3-lcd-2", which names the fix. */
#define BOARD_OTA_KEY "ws-s3-lcd-2"

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_DISPLAY_JD9853_SPI  0
#define BOARD_DISPLAY_ST7789_SPI  1   /* real ST7789T3 — library init, no vendor table */
#define BOARD_TOUCH_FT3168        0
#define BOARD_TOUCH_AXS5106L      0
#define BOARD_TOUCH_CST816        1   /* CST816D, polled over the shared I2C bus */
#define BOARD_HAS_PSRAM           1   /* 8 MB PSRAM on-module */
#define BOARD_DUAL_CORE           1   /* S3 is dual-core (band-split render, task pinning) */
#define BOARD_HAS_PMU_AXP2101     0
#define BOARD_HAS_ADC_BATTERY     1   /* no PMU; ETA6096 charger + ADC divider (GPIO5) */

/* No panel RESET line — the vendor demos all pass -1 for it. Opt out so the .ino
 * substitutes GFX_NOT_DEFINED instead of driving a negative GPIO (same as the
 * S3-1.8). LCD_RESET is intentionally NOT defined below. */
#define BOARD_LCD_HAS_RESET       0

/* Extended NVS: Preferences live in the 1 MB "nvsext" partition (app1's old slot
 * in partitions_s3_16mb.csv — no OTA), not the 20 KB head "nvs" (kept for the
 * system stores: WiFi PHY/cal, BLE bonds). See settings_load(). */
#define BOARD_NVS_EXT_LABEL "nvsext"
#define BOARD_HAS_RTC_PCF85063    0   /* no RTC chip — internal RTC + NVS persist (board_clock.h) */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0
#define BOARD_HAS_HAPTICS         0   /* no motor fitted (add a HAPTICS_MOTOR_GPIO mod to enable) */
#define BOARD_HAS_SD_MMC          0
#define BOARD_HAS_SD_SPI          1   /* shares the LCD SPI bus; CS below */
#define BOARD_HAS_IMU_QMI8658     1   /* on the touch I2C bus, addr 0x6B */
#define BOARD_HAS_BACKLIGHT_PWM   1   /* brightness = PWM on LCD_BL */
#define BOARD_HAS_CAMERA          1   /* 24-pin DVP header, OV5640 fitted — gates the Camera app */

/* ---- Awake power model (mW; see power_model.h BOARD_PWR_* for the model) -----
 * Dual-core S3 like the S3-1.47, so inherit that board's CPU coefficient default
 * rather than the C6's halved one. It HAS a QMI8658, so account for the IMU draw
 * while the step counter / sleep tracker runs. MEASURE to refine on hardware. */
#define BOARD_PWR_IMU_MW          2.0f  /* QMI8658 ~0.5 mA @ ~3.9 V (pedometer); MEASURE to refine */

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: ST7789T3 LCD 240x320\n" \
  "Touch:   CST816D capacitive\n" \
  "RTC:     internal (no chip)\n" \
  "IMU:     QMI8658\n" \
  "Camera:  OV5640 (DVP header)\n" \
  "PMU:     ETA6096 (ADC battery)"

/* ---- BLE TX-power ladder (7 tiers: Min,VLow,Low,Mid,High,VHigh,Max) -------
 * ESP32-S3 controller — same enum/range as the other S3 boards (-24..+20 dBm).
 * settings_store.h owns the shared WiFi ladder + tier names. */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -24, -18, -12, -6, 0, 9, 20 }

/* ---- Display: ST7789T3 LCD over classic SPI -------------------------------
 * Pins per the vendor demos (Arduino 09_lvgl_camera + ESP-IDF 05_lvgl_camera,
 * which agree). MISO (40) is passed to the display bus because the microSD SHARES
 * this SPI host and does read data back — without it the bus comes up MISO-less
 * and the card can never be read (mount fails). The panel itself is write-only.
 *
 * Full 240x320 panel with NO window offsets: unlike the 172-wide JD9853 boards
 * there is no column fudge, so all four offsets are 0. */
#define LCD_DC    42
#define LCD_CS    45
#define LCD_SCLK  39
#define LCD_MOSI  38
#define LCD_MISO  40   /* needed for the shared microSD on this host */
/* LCD_RESET intentionally NOT defined — see BOARD_LCD_HAS_RESET above. */
#define LCD_BL    1
#define LCD_WIDTH  240
#define LCD_HEIGHT 320
#define LCD_COL_OFFSET1 0
#define LCD_ROW_OFFSET1 0
#define LCD_COL_OFFSET2 0
#define LCD_ROW_OFFSET2 0
#define BOARD_LCD_BUS_HZ 80000000   /* SPI clock; vendor demo runs 80 MHz */
#define BOARD_LCD_EVEN_ALIGN 0      /* ST7789 has no even-alignment requirement */

/* Lines per LVGL partial render buffer (x2 buffers, internal SRAM, DMA-capable).
 * Each buffer costs LCD_WIDTH * lines * 2 = 240 * lines * 2 bytes.
 *
 * HARD CEILING — one async tile must fit ONE SPI DMA transaction: the async flush
 * (Arduino_ESP32SPIDMA::writePixelsAsync) queues a whole tile as a SINGLE DMA
 * transfer of LCD_WIDTH*lines*2 bytes. Above the SPI master's per-transfer DMA max
 * it rejects every flush with "txdata transfer > hardware max supported len". */
#define BOARD_PARTIAL_BUF_LINES 90

/* ---- Touch: CST816D over I2C (shared with the QMI8658 IMU) -----------------
 * POLLED, not interrupt-driven: the vendor bsp_cst816 library exposes no INT pin
 * and hard-codes its reset to -1. TP_INT = -1 makes BOARD_HAS_TOUCH_INT resolve to
 * 0, which compiles out my_touchpad_read()'s interrupt-flag fast path — mandatory,
 * because that path early-outs on a flag only a touch ISR ever sets, so leaving it
 * in on a board with no INT line makes touch permanently dead (see board.h).
 * TP_RESET is deliberately left UNDEFINED (not -1) so #ifdef guards skip it. */
#define IIC_SDA  48
#define IIC_SCL  47
#define TP_INT   -1   /* no INT line broken out -> polled (DriveBus "no pin" sentinel) */

/* ---- microSD on SPI (MISO 40 / MOSI 38 / CLK 39 — the LCD bus — CS 41) ----
 * Second device on the display's SPI host, so BOARD_SD_SHARES_LCD_BUS turns on the
 * cross-task display<->SD arbitration in the .ino. The card rail here is plain
 * always-on (NOT an on-chip LDO as on the C6-1.47), so SD_PWR_LDO_CHAN is NOT
 * defined and sd_card.h skips the power-up step. */
#define SD_SPI_MISO 40
#define SD_SPI_MOSI 38
#define SD_SPI_CLK  39
#define SD_SPI_CS   41

/* ---- Battery sense (ADC, no PMU) ------------------------------------------
 * The ETA6096 charges the cell; the battery reaches the ADC through a 200k/100k
 * divider, so the pin sees ONE THIRD of the cell voltage and battery mV =
 * reading_mv * 3 (same ratio as the other Waveshare ADC boards). The vendor
 * battery demo reads GPIO5 (its ESP-IDF twin names ADC1_CHANNEL_4 = GPIO5,
 * confirming both the pin and that it is on ADC1).
 *
 * ADC1 matters: unlike the S3-1.47 (GPIO12 = ADC2, which the WiFi driver owns and
 * which therefore loses samples while the radio is up), ADC1 is always readable.
 *   - BOARD_BATT_ADC_GPIO : the ADC-capable pin the divider feeds (5).
 *   - BOARD_BATT_ADC_MUL  : 1/divider-ratio = scale the pin reading back up (3).
 * The divider is always connected (no enable FET) -> a small constant leak. */
#define BOARD_BATT_ADC_GPIO 5
#define BOARD_BATT_ADC_MUL  3     /* battery_mv = pin_mv * 3 (200k/100k -> 1/3) */

/* Per-unit calibration trim (integer scale = NUM/DEN) for the divider's resistor
 * tolerance. Default 1:1 (no trim) — measure the true cell voltage at the terminal
 * (unplugged, at rest) against the reported value on YOUR unit and set NUM/DEN if
 * it reads off. It is a MULTIPLIER, since divider error scales with voltage. */
#define BOARD_BATT_CAL_NUM  1
#define BOARD_BATT_CAL_DEN  1

/* ---- IMU ------------------------------------------------------------------- */
#define IMU_QMI8658_ADDR 0x6B
/* No ULP deep-sleep step counting on this board: BOARD_HAS_ULP_STEPS stays at its
 * central default of 0. The S3 ULP blob drives the IMU over bit-banged RTC-domain
 * GPIOs, and the pins it needs are not free here (48/47 are plain digital and the
 * camera claims most of the low GPIOs). Steps count while awake, as on the S3-1.47. */

/* ---- Camera: 24-pin DVP header (OV5640 fitted) ----------------------------
 * Wired to the ESP32-S3's LCD_CAM peripheral. Pins are IDENTICAL in both vendor
 * camera demos (Arduino 09_lvgl_camera and ESP-IDF 05_lvgl_camera), so they are
 * the board's fixed header wiring rather than one demo's choice.
 *
 * PWDN is GPIO17 and is REAL on this board (the vendor comment "power down is not
 * used" describes their usage, not the wiring) — esp_camera drives it low to
 * enable the sensor. RESET is -1: the sensor is reset over SCCB in software.
 * The camera's SCCB (SIOD/SIOC on 21/16) is its OWN 2-wire bus, SEPARATE from the
 * touch/IMU I2C on 48/47 — do not confuse the two or share a lock between them. */
#define CAM_PIN_PWDN   17
#define CAM_PIN_RESET  -1   /* software reset over SCCB */
#define CAM_PIN_XCLK    8
#define CAM_PIN_SIOD   21   /* SCCB data — camera's own bus, NOT the touch I2C */
#define CAM_PIN_SIOC   16   /* SCCB clock */
#define CAM_PIN_D7      2   /* Y9 */
#define CAM_PIN_D6      7   /* Y8 */
#define CAM_PIN_D5     10   /* Y7 */
#define CAM_PIN_D4     14   /* Y6 */
#define CAM_PIN_D3     11   /* Y5 */
#define CAM_PIN_D2     15   /* Y4 */
#define CAM_PIN_D1     13   /* Y3 */
#define CAM_PIN_D0     12   /* Y2 */
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    4
#define CAM_PIN_PCLK    9
#define CAM_XCLK_HZ     20000000   /* 20 MHz XCLK — the OV5640's standard rate */

/* ---- Buttons / deep-sleep wake --------------------------------------------
 * Key1 (BOOT) is on GPIO0 (Key2 is the EN/reset key) — same as the other S3
 * boards. GPIO0 is RTC-capable, so the S3 wakes from deep sleep on the BOOT press
 * via EXT0 (board_sleep.h arms it and classifies the wake cause). */
#define BOOT_BTN_GPIO 0   /* Key1; pull-up, LOW = pressed */
#define BOARD_WAKE_USE_EXT0 1
