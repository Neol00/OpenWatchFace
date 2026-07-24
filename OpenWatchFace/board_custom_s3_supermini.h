/* ============================================================================
 *  board_custom_s3_supermini.h — ESP32-S3 Super Mini + wired GC9A01 round LCD
 *
 *  A DIY target: a bare ESP32-S3 Super Mini (no display/touch onboard) with an
 *  externally-wired round display and physical navigation buttons. Unlike every
 *  other board so far, it has NO TOUCH — navigation is by 3 buttons (up/down/
 *  select) plus the onboard BOOT (back/wake) and RST keys.
 *
 *  Hardware:
 *    - SoC: ESP32-S3 (dual core; PSRAM present on the typical N8R2/N16R8 module).
 *    - Display: GC9A01 240x240 ROUND LCD over SPI (write-only; backlight internal,
 *      no BL pin; RST wired). Driver = Arduino_GC9A01 (BOARD_DISPLAY_GC9A01_SPI).
 *    - Input: 3 physical buttons (UP/DOWN/SELECT) + BOOT (back, wakes via EXT0) +
 *      hardware RST. No capacitive touch -> BOARD_TOUCH_BUTTONS.
 *    - Battery: optional LiPo on a battery header, sensed via an ADC divider.
 *    - No PMU, no RTC chip, no IMU, no audio, no SD.
 *
 *  WIRE IT THIS WAY (the firmware drives exactly these pins):
 *    Display (SPI):  SCLK=GPIO12  MOSI/SDA=GPIO11  DC=GPIO13  CS=GPIO10  RST=GPIO14
 *    Buttons:        UP=GPIO5  DOWN=GPIO6  SELECT=GPIO7   (each: button to GND,
 *                    active-LOW, internal pull-up — no external resistor needed)
 *    BOOT/RST:       onboard (BOOT=GPIO0 = back + deep-sleep wake; RST = reset)
 *    Battery:        VBAT -> resistor divider -> GPIO4 (ADC1_CH3). See divider note.
 *
 *  PIN-CHOICE RATIONALE (S3): every GPIO on the Super Mini is broken out, but we
 *  avoid the ones that bite — strapping pins (GPIO0/3/45/46; GPIO0 is intentionally
 *  BOOT), native USB (GPIO19/20, flashing/serial), UART0 (GPIO43/44), and the
 *  flash/PSRAM pins (GPIO26-32). The battery ADC is on ADC1 (GPIO1-10) because ADC2
 *  is unusable while WiFi is on. Buttons sit on safe GPIO5/6/7.
 *
 *  BUILD NOTES:
 *    - Select it: set BOARD_SELECT to BOARD_ID_CUSTOM in board.h.
 *    - ESP32-S3 core (same as the other S3 boards). Round-screen UI relayout +
 *      the button-navigation input layer are added on top (the panel is square
 *      240x240 but physically round, so corners are clipped by the bezel).
 *    - PARTITIONS: pick a scheme matching YOUR module's flash size (the Super Mini
 *      ships in 4/8/16 MB variants). partitions_s3_16mb.csv suits a 16 MB module;
 *      use a smaller custom table for 4/8 MB or the build will overrun the chip.
 * ========================================================================== */
#pragma once

#define BOARD_NAME "ESP32-S3 Super Mini (GC9A01 round)"

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_DISPLAY_JD9853_SPI  0
#define BOARD_DISPLAY_GC9A01_SPI  1   /* GC9A01 240x240 round LCD on SPI */
#define BOARD_TOUCH_FT3168        0
#define BOARD_TOUCH_AXS5106L      0
#define BOARD_TOUCH_BUTTONS       1   /* no touch panel — physical buttons drive the UI */
#define BOARD_HAS_NAV_BUTTONS     1   /* UP/DOWN/SELECT physical nav buttons present */
#define BOARD_HAS_PSRAM           1   /* typical S3 Super Mini has PSRAM (N8R2/N16R8) */
#define BOARD_DUAL_CORE           1   /* S3 is dual-core */
#define BOARD_HAS_PMU_AXP2101     0
#define BOARD_HAS_ADC_BATTERY     1   /* LiPo on a divider into an ADC pin */
#define BOARD_HAS_RTC_PCF85063    0   /* no RTC chip — internal RTC + NVS persist */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0
#define BOARD_HAS_HAPTICS         0
#define BOARD_HAS_SD_MMC          0
#define BOARD_HAS_SD_SPI          0   /* no SD slot wired (storage -> FFat flash) */
#define BOARD_HAS_IMU_QMI8658     0
#define BOARD_HAS_BACKLIGHT_PWM   0   /* GC9A01 module backlight is internal — no BL pin */

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: GC9A01 240x240 round\n" \
  "Input:   3 buttons + BOOT\n" \
  "RTC:     internal (no chip)\n" \
  "IMU:     none\n" \
  "PMU:     none (ADC battery)"

/* ---- BLE TX-power ladder (7 tiers) — ESP32-S3 (-24..+20 dBm), like the other S3s. */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -24, -18, -12, -6, 0, 9, 20 }

/* ---- Display: GC9A01 240x240 round LCD over classic SPI -------------------
 * Write-only panel (no MISO; no SD on this bus -> -1). No backlight pin (internal),
 * so no LCD_BL. RST is wired. GC9A01 is square 240x240 in memory but physically
 * round — the UI must keep content clear of the clipped corners. No col/row offsets.
 *
 * PINS: chosen from the CONFIRMED-FREE GPIOs only (1,2,4-8,15-18,21,47,48). The
 * earlier map (10-14) was WRONG — IO9-14 are the internal-flash bus (FSPICS0/FSPID/
 * FSPICLK/FSPIQ/FSPIWP) and CANNOT be used as GPIO. SPI signals don't need ADC/RTC
 * capability, so they take the upper free pins and leave the ADC-capable ones (1-8)
 * for the battery sense. */
#define LCD_DC    17
#define LCD_CS    18
#define LCD_SCLK  15
#define LCD_MOSI  16
#define LCD_MISO  -1   /* panel is write-only; no SD on this bus -> MISO unused */
#define LCD_RESET 8
#define LCD_WIDTH  240
#define LCD_HEIGHT 240
#define LCD_COL_OFFSET1 0
#define LCD_ROW_OFFSET1 0
#define LCD_COL_OFFSET2 0
#define LCD_ROW_OFFSET2 0
#define BOARD_LCD_BUS_HZ 80000000   /* GC9A01 runs happily at 80 MHz SPI */
#define BOARD_LCD_EVEN_ALIGN 0
#define BOARD_SCREEN_ROUND 1        /* round bezel: keep UI within an inscribed circle */

/* UI scale: pin the layout factor to exactly HALF the 410x502 reference instead of
 * the width-derived 59% (240/410). The height ratio is only 48% (240/502), so the
 * width factor overflows the panel vertically; 50% fits both axes and matches the
 * half-size font tier (ui_fonts.h round tier: 48px clock, 12-16px text). */
#define BOARD_UI_SCALE_PCT 50

/* Lines per LVGL partial render buffer (x2 buffers, internal SRAM, DMA-capable).
 * Each buffer costs LCD_WIDTH * lines * 2 bytes. Same SPI-DMA per-transfer ceiling
 * as the JD9853 path (one async tile = one DMA transfer of LCD_WIDTH*lines*2 bytes,
 * which must stay under the SPI master's max). 240*60*2 = 28.8 KB/tile is safely
 * under that; ceil(240/60) = 4 flushes/frame. Plenty of SRAM headroom on the S3. */
#define BOARD_PARTIAL_BUF_LINES 60

/* ---- I2C bus (no devices, but Wire.begin() is called unconditionally) ------
 * This board has NO I2C peripherals (no PMU/RTC/touch/IMU). But the .ino calls
 * Wire.begin(IIC_SDA, IIC_SCL) unconditionally, and the i2c_lock() wrappers stay in
 * place, so define a pin pair to keep that harmless: the bus comes up but nothing
 * ever transacts on it. GPIO47/48 are free GPIO on this (quad-PSRAM, non-octal) part
 * and aren't used by the display/buttons/ADC — perfect for an unused placeholder bus.
 * TP_INT/TP_RESET are likewise unused (no touch) but referenced in a couple of spots
 * that aren't worth gating — give them safe placeholders (-1). */
#define IIC_SDA  47
#define IIC_SCL  48
#define TP_INT   -1
#define TP_RESET -1

/* ---- Physical navigation buttons (no touch) -------------------------------
 * Each button shorts its GPIO to GND when pressed; we enable the internal pull-up
 * so the pin idles HIGH and reads LOW when pressed (active-LOW, like BOOT). No
 * external resistors needed. The button-input layer (added on top) debounces these
 * and feeds an LVGL encoder/keypad indev so they drive the existing menus.
 *   UP / DOWN : move the selection (and increment/decrement value steppers).
 *   SELECT    : activate the focused item.
 *   BOOT      : back / wake (BOOT_BTN_GPIO below) — same role as on the touch boards. */
#define BTN_UP_GPIO     5
#define BTN_DOWN_GPIO   6
#define BTN_SELECT_GPIO 7
#define BTN_ACTIVE_LOW  1

/* ---- Battery sense (ADC, no PMU) ------------------------------------------
 * LiPo from the battery header through a resistor divider into GPIO4 (ADC1_CH3 —
 * ADC1 so it still reads while WiFi is active; board_power.h resolves the unit).
 *   - BOARD_BATT_ADC_GPIO : the ADC pin the divider feeds (4).
 *   - BOARD_BATT_ADC_MUL  : 1/divider-ratio. DEFAULT 2 assumes a 1/2 divider (two
 *     EQUAL resistors, e.g. 100k/100k). If you used 200k/100k (1/3), change this to
 *     3. Wrong value = the voltage reads proportionally off, so set it to match the
 *     resistors you actually solder. */
#define BOARD_BATT_ADC_GPIO 4
#define BOARD_BATT_ADC_MUL  2     /* battery_mv = pin_mv * 2 (1/2 divider). 200k/100k -> set 3 */

/* Per-unit calibration trim (integer scale = NUM/DEN). Default 1:1 (no trim);
 * measure the real terminal voltage vs the reported value and set NUM/DEN if off. */
#define BOARD_BATT_CAL_NUM  1
#define BOARD_BATT_CAL_DEN  1

/* ---- Buttons / deep-sleep wake --------------------------------------------
 * Onboard BOOT key = GPIO0 (RTC-capable on the S3), so the watch wakes from deep
 * sleep on a BOOT press via EXT0 (board_sleep.h arms it). RST is a hardware reset.
 * NOTE: we deliberately do NOT define BOARD_WAKE_GPIO here — that macro is the C6's
 * GPIO7-hardware-mod wake path; this board uses EXT0 like the other S3 boards, and
 * its GPIO7 is the SELECT button (unrelated to wake). */
#define BOOT_BTN_GPIO 0   /* BOOT key; pull-up, LOW = pressed; back + EXT0 wake */
#define BOARD_WAKE_USE_EXT0 1
