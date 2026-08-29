/* ============================================================================
 *  board_ws_c6_touch_lcd_147.h — Waveshare ESP32-C6-Touch-LCD-1.47
 *
 *  PORT IN PROGRESS — compiles & flashes; bringing up the display next.
 *
 *  Hardware: ESP32-C6 (single RISC-V core, no PSRAM), 172x320 JD9853 LCD on
 *  SPI + PWM backlight, AXS5106L touch, QMI8658 IMU, microSD on SPI.
 *
 *  BUILD NOTES for this board (differ from the S3-2.06):
 *    - Select it: set BOARD_SELECT to BOARD_ID_C6_147 in board.h.
 *    - STOCK core: the custom esp32s3-libs package + the LVGL band-split / async
 *      QSPI patches are S3-only — build the C6 against the stock ESP32 core. (The
 *      shared lv_conf.h picks LV_OS_NONE here automatically via BOARD_DUAL_CORE.)
 *    - Library: install the demo's esp_lcd_touch_axs5106l into Arduino/libraries.
 *    - PARTITIONS: this board has 8 MB flash, not 32 MB — the sketch's 32 MB
 *      partitions.csv overruns it (bootloader: "partition ... exceeds flash chip
 *      size"). Use partitions_c6_8mb.csv: in the IDE set Partition Scheme=Custom
 *      and make that the active partitions.csv for the C6 build (the IDE reads the
 *      file named partitions.csv), or wire it via a board build.partitions entry.
 *    - SD shares the LCD SPI bus (sd_card.h, CS=4) — fine for the tiny CSV logs;
 *      verify no contention once the panel is rendering.
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Waveshare ESP32-C6-Touch-LCD-1.47"
#define BOARD_VENDOR "Waveshare"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-ws-c6-lcd-147-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for ws-c6-lcd-147", which names the fix. */
#define BOARD_OTA_KEY "ws-c6-lcd-147"

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_DISPLAY_JD9853_SPI  1
#define BOARD_TOUCH_FT3168        0
#define BOARD_TOUCH_AXS5106L      1
#define BOARD_HAS_PSRAM           0
#define BOARD_DUAL_CORE           0
#define BOARD_HAS_PMU_AXP2101     0
#define BOARD_HAS_ADC_BATTERY     1   /* no PMU; battery sensed on an ADC divider */

/* Extended NVS: Preferences live in the 1 MB "nvsext" partition (app1's old
 * slot in partitions_c6_8mb.csv — no OTA), not the 20 KB head "nvs" (kept for
 * the system stores: WiFi PHY/cal, BLE bonds). See settings_load(). */
#define BOARD_NVS_EXT_LABEL "nvsext"
#define BOARD_HAS_RTC_PCF85063    0
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0   /* speaker REMOVED — GPIO7 repurposed as deep-sleep wake
                                       * input (BOOT button wired in parallel to GPIO7). */
#define BOARD_HAS_HAPTICS         0
#define BOARD_HAS_SD_MMC          0
#define BOARD_HAS_SD_SPI          1   /* shares the LCD SPI bus; CS below */
#define BOARD_HAS_IMU_QMI8658     1   /* on the touch I2C bus, addr 0x6B */
#define BOARD_HAS_BACKLIGHT_PWM   1   /* brightness = PWM on LCD_BL */

/* ---- Awake power model (mW; see power_model.h BOARD_PWR_* for the model) -----
 * Same 172x320 JD9853 LCD as the S3-1.47, so screen + floor inherit the measured
 * S3-1.47 defaults UNTIL measured here (battery header lets you meter this board).
 * The C6 is SINGLE-core (vs the S3's dual), so its CPU coefficient is ~half — a
 * starting estimate; refine against a meter. It HAS the QMI8658, so add the IMU
 * draw when the step counter / sleep tracker is running. */
#define BOARD_PWR_CPU_K           0.00130f   /* single core ~half the S3 dual-core k (estimate) */
#define BOARD_PWR_IMU_MW          2.0f       /* QMI8658 ~0.5 mA @ ~3.9 V (pedometer); MEASURE to refine */

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: JD9853 LCD 172x320\n" \
  "Touch:   AXS5106L capacitive\n" \
  "RTC:     internal (no chip)\n" \
  "IMU:     QMI8658\n" \
  "PMU:     ETA6098 (ADC battery)"

/* ---- BLE TX-power ladder (7 tiers: Min,VLow,Low,Mid,High,VHigh,Max) -------
 * The C6 controller's ESP_PWR_LVL_* enum has NO -24/-18 (its floor is -15) but
 * goes to +20. Mapped to the same 7-tier shape: Min = floor (-15), Max = ceiling
 * (+20). settings_store.h owns the shared WiFi ladder + tier names. */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9, \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -15, -12, -9, -6, 0, 9, 20 }

/* ---- Display: JD9853 LCD over classic SPI (ST7789-class driver) ----------- */
#define LCD_DC    15
#define LCD_CS    14
#define LCD_SCLK  1
#define LCD_MOSI  2
/* MISO: the display is write-only and doesn't need it, BUT the microSD card
 * shares this SPI bus (SD_SPI_MISO below = same pin) and DOES read data back. The
 * display's Arduino_HWSPI does the shared SPI.begin(), so it must declare MISO or
 * the bus comes up with MISO disabled and the SD card can never be read (mount
 * fails). So we hand the SD's MISO line to the display bus construction. */
#define LCD_MISO  3
#define LCD_RESET 22
#define LCD_BL    23
#define LCD_WIDTH  172
#define LCD_HEIGHT 320
#define LCD_COL_OFFSET1 34
#define LCD_ROW_OFFSET1 0
#define LCD_COL_OFFSET2 34
#define LCD_ROW_OFFSET2 0
#define BOARD_LCD_BUS_HZ 80000000   /* SPI clock; JD9853 demo runs 80 MHz */
#define BOARD_LCD_EVEN_ALIGN 0

/* Lines per LVGL partial render buffer (x2). The buffers cost
 * 172 * lines * 2 bytes EACH */
#define BOARD_PARTIAL_BUF_LINES 43

/* ---- Touch: AXS5106L over I2C (shared with the QMI8658 IMU) --------------- */
#define IIC_SDA  18
#define IIC_SCL  19
#define TP_INT   21
#define TP_RESET 20

/* ---- microSD on SPI (MISO 3 / MOSI 2 / CLK 1 — the LCD bus — CS 4) --------
 * The card's VDD is fed by an ESP32-C6 ON-CHIP LDO (channel VO4), NOT a plain
 * always-on rail — so it must be powered up via the IDF sd_pwr_ctrl API before
 * SD.begin(), or the card never responds and mount fails. sd_card.h does that on
 * this board when SD_PWR_LDO_CHAN is defined. (4 = LDO_VO4, the documented C6
 * default; change only if the schematic shows a different channel.) */
#define SD_SPI_MISO 3
#define SD_SPI_MOSI 2
#define SD_SPI_CLK  1
#define SD_SPI_CS   4
#define SD_PWR_LDO_CHAN 4

/* ---- Battery sense (ADC, no PMU) ------------------------------------------
 * The board brings the battery to an ADC pin through a resistor divider so the
 * cell's >3.3 V stays within the ADC's range. Per Waveshare's schematic, the
 * C6-1.47 wires VBAT to GPIO0 (ADC1_CH0) through a 200k/100k divider -> the pin
 * sees ONE THIRD of the battery voltage, so true battery mV = reading_mv * 3.
 * (A 1/2 divider was assumed at first; that over-scaled at higher cell voltages
 * and the pin saturated near full charge — confirmed 1/3 against the wiki.)
 *   - BOARD_BATT_ADC_GPIO : the ADC-capable pin the divider feeds.
 *   - BOARD_BATT_ADC_MUL  : 1/divider-ratio = scale the pin reading back up.
 * NOTE: the divider is always connected (no enable FET) -> a small constant
 * leak through the 300k path even in deep sleep. If your wiring differs, change
 * these two. */
#define BOARD_BATT_ADC_GPIO 0
#define BOARD_BATT_ADC_MUL  3     /* battery_mv = pin_mv * BOARD_BATT_ADC_MUL (200k/100k -> 1/3) */

/* Per-unit calibration trim (integer scale = NUM/DEN) for the divider's resistor
 * tolerance. Measured 4.07 V at the terminal vs 4.00 V reported -> ~+1.7% low, so
 * scale up by 4070/4000. It's a MULTIPLIER (not a flat offset) because divider
 * error scales with voltage. Measured UNPLUGGED at rest (charge current shifts
 * the reading). Set both to 1 to disable; re-trim per board if a unit reads off. */
#define BOARD_BATT_CAL_NUM  4070
#define BOARD_BATT_CAL_DEN  4000

/* ---- Audio: REMOVED on this unit. The PWM speaker (was AUDIO_PWM_PIN=GPIO7) has been
 * desoldered and GPIO7 repurposed as the deep-sleep WAKE input (see below). BOARD_HAS_AUDIO_PWM
 * is 0, so no audio code drives GPIO7. */

/* ---- IMU ------------------------------------------------------------------- */
#define IMU_QMI8658_ADDR 0x6B
/* HARDWARE MOD for LP-core deep-sleep step counting: the IMU's SDA/SCL pads are ALSO wired to
 * GPIO5/GPIO6 (in addition to the original GPIO18/19 touch-bus routing through the IMU chip,
 * which can't be cut). This BRIDGES GPIO5<->GPIO18 (IMU SDA net) and GPIO6<->GPIO19 (IMU SCL
 * net) into single nets. Touch is also on GPIO18/19. So it's ONE shared bus reachable from
 * either pin-pair. GPIO5/6 are in the C6 LP power domain (GPIO0..7), so the LP core can reach
 * the IMU there during deep sleep.
 *
 * CONTENTION RULE (critical): GPIO5 and GPIO18 are the SAME net; GPIO6 and GPIO19 the same.
 * NEVER drive both pins of a pair at once. So:
 *   - AWAKE: the IMU stays on the normal shared touch bus (GPIO18/19, hardware I2C via Wire,
 *     exactly as stock). GPIO5/6 are left as idle INPUTS — never driven by the HP CPU.
 *   - ASLEEP: the LP core drives GPIO5/6 (GPIO18/19 are powered down with the HP domain, so
 *     no conflict). The IMU's INT pins that were on GPIO5/6 are unused (pedometer engine dead).
 *
 * Hence the IMU is NOT on its own bus awake — it's shared (no IMU_HAS_OWN_BUS). GPIO5/6 are
 * used ONLY by the LP-core sleep program. */
#define IMU_LP_SDA_GPIO   5     /* LP-core (sleep) IMU SDA — bridged to GPIO18 at the IMU pad */
#define IMU_LP_SCL_GPIO   6     /* LP-core (sleep) IMU SCL — bridged to GPIO19 at the IMU pad */
#define BOARD_HAS_LP_STEPS 1    /* C6 LP-core deep-sleep step counting (analog of S3 ULP) */

/* ---- Buttons / deep-sleep wake --------------------------------------------
 * BOOT button is on GPIO9 (used for the in-app menu while awake; pull-up, LOW = pressed).
 * GPIO9 is a STRAPPING pin and is OUTSIDE the C6 deep-sleep wake mask (silicon allows only
 * GPIO0..7), so it cannot wake from deep sleep — confirmed by a failed lib rebuild.
 *
 * HARDWARE MOD on this unit: a wire ties the BOOT button's GPIO9 node ALSO to GPIO7 (which
 * IS in the GPIO0..7 wake mask and is NOT a strapping pin). The speaker that used GPIO7 was
 * removed. So one BOOT press pulls BOTH GPIO9 (awake menu, unchanged) and GPIO7 (deep-sleep
 * wake) LOW. board_sleep.h arms an esp_deep_sleep_enable_gpio_wakeup() on GPIO7, wake-on-LOW. */
#define BOOT_BTN_GPIO 9   /* C6 BOOT button -> GPIO9 for the awake in-app menu (unchanged) */
#define BOARD_WAKE_GPIO 7 /* deep-sleep wake input: BOOT button also wired to GPIO7 (HW mod) */
/* No EXT0 on the C6 (RISC-V); the wake is the GPIO7 deep-sleep wake armed in board_sleep.h. */
#define BOARD_WAKE_USE_EXT0 0
#define BOARD_TRY_GPIO9_WAKE 0   /* GPIO9 wake proven impossible — now using the GPIO7 HW mod */
