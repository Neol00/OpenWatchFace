/* ============================================================================
 *  board_lilygo_tdeck_pro.h — LilyGo T-Deck Pro (hardware v1.0)
 *
 *  Hardware: ESP32-S3FN16R8 (dual core, 8 MB PSRAM, 16 MB flash), 3.1" 240x320
 *  GDEQ031T10 E-PAPER (UC8253) on SPI, CST328 capacitive touch, TCA8418 QWERTY
 *  keyboard, SX1262 LoRa, u-blox MIA-M10Q GNSS, Bosch BHI260AP IMU, LTR-553ALS
 *  light sensor, BQ25896 charger + BQ27220 fuel gauge, microSD, vibration motor,
 *  1500 mAh cell, and either an A7682E 4G modem or a PCM5102A codec by variant.
 *
 *  PIN MAP VERIFIED against LilyGo's own examples/factory/utilities.h
 *  (BOARD_T_DECK_PRO_VERSION "v1.0-241106") — not inferred. The e-paper wiring is
 *  cross-checked against examples/test_EPD/test_EPD.ino and the keyboard decode
 *  against examples/factory/peri_keypad.cpp.
 *
 *  ============ WHAT MAKES THIS BOARD DIFFERENT ============
 *  Read this before assuming anything from the other S3 boards carries over.
 *
 *  - THE PANEL IS NOT EMISSIVE. First e-paper target. Two consequences ripple
 *    through the whole config:
 *      * NO PANEL BACKLIGHT. BOARD_HAS_BACKLIGHT_PWM is 0, but the brightness
 *        UI is NOT inert: board_display_set_brightness() is rerouted to the
 *        KEYBOARD backlight (LED_EN, PWM via LEDC) — the only lamp on the
 *        board. The quick-shade slider floor is 0% here (a keyboard light may
 *        legitimately be fully off). Idle-dim dims the keyboard light too.
 *      * THE FLUSH DOES NOT TOUCH THE PANEL. my_disp_flush() only blits into a
 *        1bpp shadow buffer; loop() drives the refresh via epd_service(). See
 *        display_epd_gdeq031t10.h — the real explanation lives there.
 *
 *  - RENDER FORMAT STAYS RGB565. Tempting to switch LVGL to native 1-bit
 *    LV_COLOR_FORMAT_I1 since the panel is monochrome, saving ~150 KB. DON'T, at
 *    least not as part of this port: the colour format is global, screen_cache
 *    snapshots pin LV_COLOR_FORMAT_RGB565, and the band-split renderer assumes a
 *    16-bit lv_color_t. Rendering RGB565 and thresholding in the blit keeps every
 *    existing UI path byte-identical to the other S3 boards, and this board has
 *    8 MB of PSRAM to spend. The threshold is one function (epd_blit_tile).
 *
 *  - NO PANEL RESET LINE. BOARD_EPD_RST is -1 in LilyGo's own header and their
 *    EPD example passes -1 to GxEPD2. So BOARD_LCD_HAS_RESET is 0 and EPD_RST is
 *    -1 (GxEPD2's documented "no reset pin" value, which it checks for).
 *
 *  - TOUCH IS A CST328 AT 0x1A — NOT the CST816 at 0x15 that touch_cst816.h
 *    drives. Different address AND a completely different protocol (a 28-byte
 *    validated status block vs a few single-byte registers). It gets its own
 *    driver, touch_cst328.h. Hardware v1.0 and v1.1 both use it; later units may
 *    ship a CST3530, which would need another driver again.
 *
 *  - A REAL KEYBOARD, first board with one. It registers a SECOND LVGL indev
 *    (keypad) alongside touch. It is a TEXT keyboard with NO arrow keys and no
 *    navigation cluster, so it does NOT replace touch for driving the UI — touch
 *    remains the way you navigate. See keyboard_tca8418.h for the two inverted
 *    encoding details, and BOARD_KB_KEYMAP below for what physically exists.
 *
 *  - SHARED SPI BUS, THREE DEVICES: e-paper (CS 34), microSD (CS 48) and the
 *    SX1262 (CS 3) all sit on one host. LilyGo's EPD example drives ALL of these
 *    CS lines HIGH before SPI.begin(), because a floating CS lets one device
 *    respond to another's traffic. The .ino does the same in setup() — see the
 *    epd_predeselect_bus() call. Do not remove it.
 *
 *  - RAIL ENABLES. GPS, the IMU's 1V8 rail, the modem and LoRa each sit behind an
 *    enable GPIO (BOARD_*_EN below). Anything driving those peripherals must
 *    raise its rail first; nothing in this firmware does yet, so they are left
 *    unasserted and those chips stay powered down (which is the low-power state
 *    we want until there are drivers).
 *
 *  ============ SCREEN TIERS ============
 *  240x320 portrait — the SAME geometry as the Waveshare S3-Touch-LCD-2, so it
 *  lands on exactly the tiers that board already exercises:
 *      BOARD_SCREEN_PORTRAIT    1  (320 > 240)
 *      BOARD_SCREEN_NARROW      0  (240 > 220)
 *      BOARD_SCREEN_MIDNARROW   0  (320*2 > 240*3 is false -> 4:3, not tall)
 *      BOARD_SCREEN_LOWRES_DIAL 1  (240 <= 260 -> 88 px clock glyph, not 110)
 *      BOARD_SCREEN_SUBREF      1  (240 < 340 -> percentage widths)
 *  That is a well-trodden combination in this firmware rather than a new one,
 *  which is a large part of why the UI should come up looking right.
 *
 *  ============ BUILD NOTES ============
 *    - Select it: set BOARD_SELECT to BOARD_ID_TDECK_PRO in board.h.
 *    - LIBRARY: install "GxEPD2" by Jean-Marc Zingg (Library Manager). Only new
 *      dependency this board adds.
 *    - PARTITIONS: 16 MB flash -> partitions_s3_16mb.csv, as the other S3 boards.
 *    - IDE: enable PSRAM (OPI). A silent boot loop is the classic symptom of
 *      building an 8 MB-PSRAM board with PSRAM disabled.
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "LilyGo T-Deck Pro"
#define BOARD_VENDOR "LilyGo"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-tdeck-pro-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for tdeck-pro", which names the fix. */
#define BOARD_OTA_KEY "tdeck-pro"

/* ---- Feature flags -------------------------------------------------------- */
#define BOARD_DISPLAY_EPD_GDEQ031T10 1   /* 3.1" 240x320 e-paper, UC8253, via GxEPD2 */
#define BOARD_TOUCH_CST328           1   /* CST328 @ 0x1A — NOT the CST816 driver */
#define BOARD_HAS_KEYBOARD_TCA8418   1   /* 4x10 QWERTY matrix behind a TCA8418 */
#define BOARD_HAS_PSRAM              1   /* 8 MB PSRAM on-module */
#define BOARD_DUAL_CORE              1   /* S3 is dual-core */
#define BOARD_HAS_HAPTICS            1   /* vibration motor on GPIO2 */

/* No PANEL backlight — reflective e-paper. BOARD_HAS_BACKLIGHT_PWM stays 0 so
 * the generic LCD_BL path never runs; instead board_display_set_brightness()
 * routes to the KEYBOARD backlight (board_tdeck_kbd_backlight, below) — the
 * only lamp on the board the brightness UI can meaningfully control. */
#define BOARD_HAS_BACKLIGHT_PWM      0
/* No panel reset line (BOARD_EPD_RST is -1 in the vendor header). */
#define BOARD_LCD_HAS_RESET          0

#define BOARD_HAS_PMU_AXP2101        0   /* BQ25896 charger, not an X-Powers PMU */
#define BOARD_HAS_ADC_BATTERY        0   /* BQ27220 I2C fuel gauge, not an ADC divider */
#define BOARD_HAS_GAUGE_BQ27220      1   /* real coulomb-counting gauge @ 0x55 */
#define BOARD_HAS_CHARGER_BQ25896    1   /* charger @ 0x6B — supplies VBUS-present */
#define BOARD_HAS_RTC_PCF85063       0   /* no RTC chip — internal RTC + NVS (board_clock.h) */
#define BOARD_HAS_SD_SPI             1   /* microSD shares the e-paper SPI host */
#define BOARD_HAS_IMU_QMI8658        0   /* IMU is a Bosch BHI260AP — see the note at the end */
#define BOARD_HAS_CAMERA             0   /* no camera on this board */

/* Extended NVS, same as the other 16 MB S3 boards: Preferences in the 1 MB
 * "nvsext" partition, leaving the 20 KB head "nvs" for the system stores. */
#define BOARD_NVS_EXT_LABEL "nvsext"

/* ---- E-paper specifics ---------------------------------------------------- */
/* Polarity of the 1bpp reduction. The UI is authored light-on-dark (white text on
 * black), which on a reflective panel would mean flooding the glass with black —
 * slow, and it throws away e-paper's paper-like readability. Inverting maps the
 * UI's dark background to white paper and its light text to black ink.
 *
 * Set to 0 to see the UI's true polarity instead. Display-side flip only — no UI
 * code or colour changes — so it is safe to toggle while judging the look. */
#define BOARD_EPD_INVERT 1

/* ---- E-paper speed --------------------------------------------------------
 * display_epd_gdeq031t10.h drives the panel through GxEPD2's GDEQ031T10 driver
 * (vendored in LilyGo's own repo at lib/GxEPD2/src/gdeq/). Using the library
 * rather than a hand-rolled UC8253 sequence is deliberate: its command order is
 * already debugged against this exact panel, and the fast path costs two
 * register writes that the library ALREADY makes.
 *
 * The full explanation of where the speed comes from lives in that file's header
 * block. The short version, in order of impact:
 *   1. ONE coalesced refresh per frame instead of one per dirty tile.
 *   2. Partial (~700 ms) vs full (~1100 ms + a black/white flash).
 *   3. Refreshing only the dirty window, not the whole panel.
 *   4. TSSET, the forced-temperature waveform dial (see below).
 *   5. SPI clock — a ~2% effect. Do NOT go hunting here.
 *
 * TSSET IS THE WAVEFORM DIAL. The UC8253 keeps its waveforms in OTP and picks
 * one BY TEMPERATURE, so "fast mode" means forcing a high fake temperature:
 * 0xE0<-0x02 (TSFIX) then 0xE5<-value. Higher = faster and weaker (greyer text,
 * more ghosting). Set via EPD_TSSET_PART / EPD_TSSET_FULL, which need
 * patches/09-gxepd2-tsset-tunable.patch to reach the driver; without the patch
 * you get GxEPD2's stock 0x79 / 0x5A, which still work.
 *
 * CORRECTION TO EARLIER NOTES IN THIS FILE. Two claims that were here before the
 * driver existed are contradicted by the working vendored code, and are recorded
 * here because both would send a debugging session in the wrong direction:
 *   - There is NO frame-count LUT and no EPD_LUT_FRAMES. Nothing reads it. The
 *     waveform is not built from N frames at 200 Hz; it is selected by TSSET.
 *   - The "missing DATA STOP (DSP, 0x11) before DRF (0x12)" diagnosis was wrong.
 *     The vendored driver refreshes correctly and never sends 0x11 at all; its
 *     sequence is 0x50 (data interval) -> 0x04 (power on) -> 0x12 (refresh).
 *
 * IF THE PANEL IS BLANK, read `wait` in the [epd] profile line first. GxEPD2
 * waits on the BUSY pin, not a fixed delay, so `wait` is real panel time: a few
 * ms means the refresh never ran (command/wiring fault), while ~700/1100 ms
 * means it ran and the fault is in the image data or the waveform. */

/* Refresh pacing: defaults in display_epd_gdeq031t10.h. EPD_FULL_EVERY is THE
 * ghosting knob — lower it for a cleaner image at the cost of more full-screen
 * flashes; 0 disables full refreshes entirely (fastest, ghosts forever).
 * EPD_MIN_REFRESH_MS below the panel's own ~700 ms partial time is a no-op. */
/* #define EPD_MIN_REFRESH_MS 700 */
/* #define EPD_FULL_EVERY     12  */
/* #define EPD_TSSET_PART     0x79 */
/* #define EPD_TSSET_FULL     0x5A */

/* ---- Awake power model (mW; see power_model.h BOARD_PWR_*) -----------------
 * Dual-core S3, inherits that CPU coefficient. The e-paper is deliberately NOT
 * given a constant term: its draw is bursty (real current for 0.5-3 s during a
 * refresh, then nothing at all), so a steady mW figure would model it worse than
 * omitting it. The IMU term is moot until a BHI260AP driver exists. */
#define BOARD_PWR_IMU_MW 0.0f

/* Hardware summary for the Settings > About screen (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "Display: GDEQ031T10 e-paper 240x320\n" \
  "Touch:   CST328 capacitive\n" \
  "Keys:    TCA8418 QWERTY (4x10)\n" \
  "RTC:     internal (no chip)\n" \
  "IMU:     BHI260AP (no driver)\n" \
  "GNSS:    u-blox MIA-M10Q\n" \
  "Radio:   SX1262 LoRa\n" \
  "Charger: BQ25896\n" \
  "Gauge:   BQ27220 (coulomb-counting)"

/* ---- BLE TX-power ladder (7 tiers: Min,VLow,Low,Mid,High,VHigh,Max) -------
 * ESP32-S3 controller — same enum/range as the other S3 boards (-24..+20 dBm). */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N24, ESP_PWR_LVL_N18, ESP_PWR_LVL_N12, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9,  \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -24, -18, -12, -6, 0, 9, 20 }

/* ---- Panel geometry -------------------------------------------------------
 * 240x320 PORTRAIT — the panel's native orientation, matching LilyGo's
 * LCD_HOR_SIZE 240 / LCD_VER_SIZE 320. No rotation, no coordinate transform. */
#define LCD_WIDTH  240
#define LCD_HEIGHT 320

/* Lines per LVGL partial render buffer (x2, RGB565). Cost = 240 * lines * 2.
 * UNLIKE the SPI-DMA boards there is NO hardware ceiling:
 * the flush writes to RAM and never queues a DMA transaction, so the 32 KB
 * per-transfer SPI-DMA limit that constrains those boards does not apply here. */
#define BOARD_PARTIAL_BUF_LINES 90

/* Use ONE full-screen LVGL framebuffer in PSRAM instead of the two partial
 * SRAM line-buffers (see OWF_FULL_FB_MODE in OpenWatchFace.ino).
 *
 * This is not a speed change — the panel waveform dominates everything here. It
 * makes LVGL hand the flush a single whole-panel area every frame, so the EPD
 * shadow buffer always receives a complete frame and the dirty box is always
 * the full panel. That removes partial-window addressing as a variable, which
 * the custom register-LUT waveform needs: the fast LUT was observed to render
 * only on large redraws, so small windows have to be ruled out before the LUT
 * itself can be tuned.
 *
 * Costs ~150 KB PSRAM (240*320*2), frees the ~84 KB of SRAM the pair used.
 * Set to 0 to go back to partial line-buffers. */
#define BOARD_LVGL_FULL_PSRAM_FB 1

/* ===========================================================================
 *  PIN MAP — from LilyGo examples/factory/utilities.h (hw v1.0-241106)
 * ======================================================================== */

/* ---- E-paper on SPI (shared host with microSD + LoRa) ---------------------
 * MISO (47) belongs to the shared host for the SD and the SX1262; the panel
 * itself is write-only. RST is -1: this panel has no reset line broken out. */
#define EPD_SCLK   36
#define EPD_MOSI   33
#define EPD_MISO   47
#define EPD_CS     34
#define EPD_DC     35
#define EPD_RST    -1   /* no reset line — GxEPD2's "not connected" value */
#define EPD_BUSY   37

/* ---- Shared I2C bus: touch, keyboard, light sensor, IMU, charger, gauge ----
 * Device addresses: touch 0x1A, light 0x23, IMU 0x28, keyboard 0x34,
 * BQ27220 gauge 0x55, BQ25896 charger 0x6B. */
#define IIC_SDA    13
#define IIC_SCL    14

/* Touch INT and RESET are BOTH broken out on this board (unlike the
 * S3-Touch-LCD-2, where TP_INT is the -1 "no interrupt" sentinel). A real INT
 * pin means BOARD_HAS_TOUCH_INT resolves to 1, which enables my_touchpad_read()'s
 * interrupt-flag fast path — see the long explanation in board.h. */
#define TP_INT     12
#define TP_RESET   45

/* ---- Keyboard matrix (TCA8418 at 0x34) ------------------------------------
 * 4 rows x 10 cols, per LilyGo's peri_keypad.cpp. INT on 15, backlight on 42. */
#define BOARD_KB_ROWS 4
#define BOARD_KB_COLS 10
#define BOARD_KB_INT  15
#define BOARD_KB_LED  42

/* Matrix position -> key, transcribed from LilyGo's own keymap table.
 *
 * THIS KEYBOARD HAS NO ARROW KEYS, no d-pad, and no dedicated navigation cluster
 * — it is a BlackBerry-style TEXT keyboard: 26 letters, space, shift, symbol,
 * backspace, enter, and two hardware-function keys. Do NOT invent LV_KEY_UP /
 * DOWN / LEFT / RIGHT mappings for it: there are no physical keys to press, so
 * any such mapping is unreachable code that misleads whoever reads it next.
 * TOUCH is the navigation method on this board; the keyboard is for text entry.
 *
 * NOTE THE COLUMN ORDER IS REVERSED IN THE DECODE, not here: this table is in
 * natural left-to-right order and keyboard_tca8418.h applies the flip. See the
 * header of that file — getting it wrong mirrors each row ("poiuytrewq").
 *
 * LilyGo's table uses single-char placeholders for the non-letter keys; the
 * mapping below preserves their positions exactly:
 *   row1 col9 '0'  -> backspace              -> LV_KEY_BACKSPACE
 *   row2 col0 '2'  -> ALT                    -> KB_KEY_SYM (alt selects the
 *                     printed secondary legends, same as the sym key)
 *   row2 col9 'E'  -> enter                  -> LV_KEY_ENTER
 *   row3 col5      -> SHIFT (hw-verified)    -> KB_KEY_SHIFT
 *   row3 col6      -> mic key                -> 0 alone; '0' in the sym layer
 *                     (the keycap's printed secondary legend is the digit 0)
 *   row3 col7      -> SPACEBAR (hw-verified: it logged "unmapped" here; the
 *                     five col0-4 space entries LilyGo's table implies never
 *                     fire and are kept only as harmless placeholders)
 *   row3 col8      -> SYM (hw-verified)      -> KB_KEY_SYM
 *   row3 col9      -> second SHIFT          -> KB_KEY_SHIFT
 *
 * The PHYSICAL bottom two rows are (user-confirmed 2026-08-11):
 *   row 3: alt  z x c v b n m $  enter
 *   row 4: shift  mic  [ spacebar ]  sym  shift
 * LilyGo's placeholder comments for row3 cols 5-9 do NOT match this hardware
 * (they call col5 "symbol", col8 "right shift", col7 a "speaker" key that does
 * not exist). Col5=shift and col8=sym are VERIFIED by on-device behavior; the
 * mic/second-shift split across col6/7/9 is inferred — if the second shift is
 * dead, press it and read the "[kb] raw=" line my_keypad_read logs over USB
 * serial, then move KB_KEY_SHIFT to that column here.
 *
 * The five leading spaces on the table's last row are the spacebar's contacts.
 *
 * The reachable LVGL key codes are LV_KEY_ENTER and LV_KEY_BACKSPACE. Everything
 * else is a literal character for text entry, KB_KEY_SHIFT/KB_KEY_SYM (modifier
 * sentinels the keypad read callback tracks as STATE — they never reach LVGL),
 * or 0 for a hardware-function key this firmware does not consume.
 *
 * Table is uint16_t so LV_KEY_* values (some exceed 255) are never truncated. */

/* Modifier sentinels. Private-use codepoints so they can never collide with a
 * real character or an LV_KEY_* value. The read callback consumes these to set
 * shift/sym state; they are never delivered to LVGL as keys. */
#define KB_KEY_SHIFT 0xF8F0
#define KB_KEY_SYM   0xF8F1

#define BOARD_KB_KEYMAP { \
  { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p' }, \
  { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', LV_KEY_BACKSPACE }, \
  { KB_KEY_SYM, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', LV_KEY_ENTER }, \
  { ' ', ' ', ' ', ' ', ' ', KB_KEY_SHIFT,   0, ' ', KB_KEY_SYM, KB_KEY_SHIFT } \
}

/* Symbol (sym-key) layer: what each key types while sym is active, matching the
 * secondary legends printed on the keycaps. Transcribed from the standard
 * BlackBerry-Q10-style layout this keyboard family uses — VERIFY AGAINST THE
 * PHYSICAL KEYCAPS and correct here if any position differs. A 0 entry means
 * "no secondary legend": the decode falls back to the base map, so backspace/
 * enter/space and the modifiers keep working with sym held. */
#define BOARD_KB_ALTMAP { \
  { '#', '1', '2', '3', '(', ')', '_', '-', '+', '@' }, \
  { '*', '4', '5', '6', '/', ':', ';', '\'', '"', 0 }, \
  {   0, '7', '8', '9', '?', '!', ',', '.',   0, 0 }, \
  {   0,   0,   0,   0,   0,   0, '0',   0,   0, 0 } \
}

/* ---- microSD on the shared SPI host --------------------------------------- */
#define SD_SPI_MISO EPD_MISO
#define SD_SPI_MOSI EPD_MOSI
#define SD_SPI_CLK  EPD_SCLK
#define SD_SPI_CS   48

/* ---- SX1262 LoRa (same SPI host, own CS) ----------------------------------
 * Pins recorded so the radio can be brought up without re-deriving them; no
 * driver in this firmware yet. BOARD_LORA_EN gates its rail. */
#define LORA_CS    3
#define LORA_RST   4
#define LORA_BUSY  6
#define LORA_IRQ   5

/* ---- GNSS (u-blox MIA-M10Q on Serial2) ------------------------------------ */
#define GPS_TX     43
#define GPS_RX     44
#define GPS_PPS     1

/* ---- Peripheral rail enables ----------------------------------------------
 * Each of these powers a chip that currently has no driver. They MUST be driven
 * LOW, not merely left unconfigured: "unasserted" used to mean floating inputs,
 * and a floating enable on the SGM6609 boost drifted HIGH — which powered the
 * A7682E modem, booted it, and lit its red STATUS LED permanently (the
 * "constantly-on red LED" bug) while the idle modem burned battery. The factory
 * firmware drives all of these as outputs (HIGH, because it ships drivers); we
 * drive them LOW at boot via board_tdeck_rails_off() and LATCH them low through
 * deep sleep in board_isolate_peripherals_for_sleep().
 * Raise the matching rail BEFORE talking to a peripheral. */
#define BOARD_GPS_EN   39   /* GPS module power */
#define BOARD_1V8_EN   38   /* BHI260AP IMU 1V8 rail */
#define BOARD_6609_EN  41   /* A7682E modem power (SGM6609 boost EN) */
#define BOARD_LORA_EN  46   /* SX1262 power */
#define BOARD_A7682E_PWRKEY 40  /* modem power-key transistor (HIGH = press) */
#define BOARD_KEYBOARD_LED  42  /* keyboard backlight enable (LED_EN via Q6) */

/* Everything below this guard is C, not preprocessor macros. This header is
 * ALSO included from assembly (.S) compilation units, where the assembler
 * chokes on C code — keep anything that isn't a #define inside the guard. */
#ifndef __ASSEMBLER__
#include <driver/gpio.h>   /* gpio_hold_en/dis, gpio_deep_sleep_hold_en */

/* Drive every unused peripheral enable to its OFF level. Called early in
 * setup() (right after the boot banner). The gpio_hold_dis() first: after a
 * deep-sleep WAKE (not a reset) the sleep holds below are still latched, and a
 * held pad ignores writes — release, then re-own.
 *
 * Raw IDF gpio driver, NOT Arduino pinMode/digitalWrite: this header is also
 * included from plain-C compilation units (the LVGL font/icon .c files pull in
 * board.h via lv_conf.h), where the Arduino C++ API does not exist. */
static inline void board_tdeck_rails_off(void) {
  const gpio_num_t pins[] = { (gpio_num_t)BOARD_GPS_EN, (gpio_num_t)BOARD_1V8_EN,
                              (gpio_num_t)BOARD_6609_EN, (gpio_num_t)BOARD_LORA_EN,
                              (gpio_num_t)BOARD_A7682E_PWRKEY, (gpio_num_t)BOARD_KEYBOARD_LED };
  for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
    gpio_hold_dis(pins[i]);
    gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
    gpio_set_level(pins[i], 0);
  }
}

/* Latch the same pins LOW through deep sleep. Digital (non-RTC) pads float once
 * the CPU sleeps, which would re-float the modem boost EN and let the modem
 * power back up mid-sleep — gpio_hold_en pins the level, and
 * gpio_deep_sleep_hold_en() keeps the holds active in deep sleep. */
static inline void board_tdeck_rails_hold_for_sleep(void) {
  const gpio_num_t pins[] = { (gpio_num_t)BOARD_GPS_EN, (gpio_num_t)BOARD_1V8_EN,
                              (gpio_num_t)BOARD_6609_EN, (gpio_num_t)BOARD_LORA_EN,
                              (gpio_num_t)BOARD_A7682E_PWRKEY, (gpio_num_t)BOARD_KEYBOARD_LED };
  for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
    gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
    gpio_set_level(pins[i], 0);
    gpio_hold_en(pins[i]);
  }
  gpio_deep_sleep_hold_en();
}

/* Keyboard backlight brightness (0-255) via LEDC PWM on LED_EN. On this board
 * the UI's brightness control drives THIS, not the panel — the e-paper is
 * reflective, so the keyboard light is the only lamp there is to dim.
 *
 * Re-runs ledc_channel_config() on every call rather than just updating the
 * duty: board_tdeck_rails_off()/..._hold_for_sleep() reclaim the pad as plain
 * GPIO (gpio_set_direction re-routes the matrix away from LEDC), so the pin
 * must be re-attached after every boot/wake, and the config call is the
 * cheapest way to make that unconditional. Timer 3 / channel 7 to stay clear
 * of anything the Arduino core hands out from the low end. */
#include <driver/ledc.h>
static inline void board_tdeck_kbd_backlight(uint8_t b) {
  static char timer_ready = 0;
  if (!timer_ready) {
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.timer_num       = LEDC_TIMER_3;
    t.duty_resolution = LEDC_TIMER_8_BIT;
    t.freq_hz         = 5000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    timer_ready = 1;
  }
  ledc_channel_config_t c = {};
  c.gpio_num   = BOARD_KEYBOARD_LED;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel    = LEDC_CHANNEL_7;
  c.timer_sel  = LEDC_TIMER_3;
  c.duty       = b;                 /* 8-bit timer: 0..255 maps 1:1 */
  ledc_channel_config(&c);
}
#endif  /* !__ASSEMBLER__ */

/* ---- Haptics --------------------------------------------------------------
 * Plain GPIO-driven motor (no DRV2605 driver chip), same model as the other
 * boards with BOARD_HAS_HAPTICS: haptics.h toggles the pin for a pulse. */
#define HAPTICS_MOTOR_GPIO 2
#define HAPTICS_ACTIVE_HIGH 1   /* pin HIGH drives the motor (as on every other board) */

/* ---- Audio / mic (no driver yet) ------------------------------------------
 * I2S to the PCM5102A on the audio variant; PDM mic on 17/18. Recorded only. */
#define BOARD_I2S_BCLK 7
#define BOARD_I2S_DOUT 8
#define BOARD_I2S_LRC  9
#define BOARD_MIC_DATA  17
#define BOARD_MIC_CLOCK 18

/* ---- Buttons / deep-sleep wake -------------------------------------------- */
#define BOOT_BTN_GPIO 0     /* BOOT key, pull-up, LOW = pressed */
#define BOARD_WAKE_USE_EXT0 1   /* GPIO0 is RTC-capable on the S3 */

/* ---- Notes on hardware this port does NOT yet drive -----------------------
 * Recorded so the gaps are explicit rather than discovered later:
 *
 *   BHI260AP IMU — BOARD_HAS_IMU_QMI8658 is 0 because imu_steps.h speaks to a
 *     QMI8658, and the BHI260AP is a completely different part: a programmable
 *     sensor hub that needs a firmware blob uploaded at boot, not a register-map
 *     accelerometer. Steps therefore do not count on this board yet. Adding it
 *     means a BOARD_HAS_IMU_BHI260 flag plus a driver alongside imu_steps.h, and
 *     raising BOARD_1V8_EN first. Deliberately out of scope for bring-up.
 *
 *   SX1262 LoRa, MIA-M10Q GNSS, A7682E modem, PCM5102A audio, LTR-553ALS light
 *     sensor — pins recorded above, no drivers. These are the genuinely new
 *     capabilities of this board and each deserves its own module.
 */
