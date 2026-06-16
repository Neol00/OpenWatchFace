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

#define BOARD_NAME "Waveshare ESP32-C6-Touch-LCD-1.47"

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_DISPLAY_JD9853_SPI  1
#define BOARD_TOUCH_FT3168        0
#define BOARD_TOUCH_AXS5106L      1
#define BOARD_HAS_PSRAM           0
#define BOARD_DUAL_CORE           0
#define BOARD_HAS_PMU_AXP2101     0
#define BOARD_HAS_ADC_BATTERY     1   /* no PMU; battery sensed on an ADC divider */
#define BOARD_HAS_RTC_PCF85063    0
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_HAPTICS         0
#define BOARD_HAS_SD_MMC          0
#define BOARD_HAS_SD_SPI          1   /* shares the LCD SPI bus; CS below */
#define BOARD_HAS_IMU_QMI8658     1   /* on the touch I2C bus, addr 0x6B */
#define BOARD_HAS_BACKLIGHT_PWM   1   /* brightness = PWM on LCD_BL */

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: JD9853 LCD 172x320\n" \
  "Touch:   AXS5106L capacitive\n" \
  "RTC:     internal (no chip)\n" \
  "IMU:     QMI8658\n" \
  "PMU:     none (ADC battery)"

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
 * 172 * lines * 2 bytes EACH, in scarce internal SRAM. 80 lines was ~27 KB x2 =
 * ~54 KB — too much on a board that also needs a big LVGL heap (no PSRAM). 40
 * lines = ~13.5 KB x2 = ~27 KB, freeing ~27 KB for the LVGL pool, at the cost of
 * 8 flushes per full frame instead of 4 (negligible: partial rendering rarely
 * redraws the whole screen, and the small frame flushes fast). */
#define BOARD_PARTIAL_BUF_LINES 40

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

/* ---- IMU ------------------------------------------------------------------- */
#define IMU_QMI8658_ADDR 0x6B

/* ---- Buttons / deep-sleep wake --------------------------------------------
 * BOOT button is on GPIO9 (used for the in-app menu while awake; pull-up, LOW =
 * pressed). The C6 uses real DEEP sleep, but it CANNOT arm a GPIO wake: the
 * silicon restricts deep-sleep wake to GPIO0..7, and both the BOOT button
 * (GPIO9) and the touch INT (GPIO21) are outside it. So the watch is brought
 * back from deep sleep with the hardware RST button — a reset is a clean cold
 * boot, which is exactly what every deep-sleep wake on this firmware already is.
 * board_sleep.h therefore arms NO button wake here (only the RTC timer). */
#define BOOT_BTN_GPIO 9   /* C6 BOOT button; pull-up, LOW = pressed (awake-only) */
/* No EXT0 on the C6 (RISC-V); board_sleep.h skips the GPIO wake-arm when this is
 * 0 and relies on the RTC timer + the hardware RST button. */
#define BOARD_WAKE_USE_EXT0 0
