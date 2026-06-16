/* ============================================================================
 *  board.h — board selection + hardware-config dispatch
 *
 *  ONE firmware, several hardware targets. Each supported board has its own
 *  `board_*.h` describing EVERYTHING hardware-specific about it:
 *    - pins (display, touch, I2C, SD, audio, motor, button)
 *    - display geometry / driver type / bus type
 *    - feature flags (BOARD_HAS_*) that gate whole modules: a module whose
 *      feature is absent compiles to no-op stubs, so call sites never change.
 *
 *  TO SELECT A BOARD: uncomment exactly one BOARD_* line below (or pass
 *  -DBOARD_...=1 via build flags). Everything else follows from the flags.
 *
 *  ADDING A BOARD: copy an existing board_*.h, fill in the pin map and flags,
 *  add an #elif below. The flag reference (every board header must define ALL
 *  of these, 0 or 1):
 *
 *    Display (exactly one =1):
 *      BOARD_DISPLAY_CO5300_QSPI   CO5300 AMOLED on QSPI (Arduino_CO5300)
 *      BOARD_DISPLAY_JD9853_SPI    JD9853 LCD on classic SPI (ST7789-class)
 *    Touch (exactly one =1):
 *      BOARD_TOUCH_FT3168          FT3168 via Arduino_DriveBus
 *      BOARD_TOUCH_AXS5106L        AXS5106L via esp_lcd_touch_axs5106l
 *    Capabilities:
 *      BOARD_HAS_PSRAM             external PSRAM present (stores/snapshot cache)
 *      BOARD_DUAL_CORE             2nd core exists (band-split render, net task pinning)
 *      BOARD_HAS_PMU_AXP2101       AXP2101 PMU (battery gauge, rails, power-off)
 *      BOARD_HAS_RTC_PCF85063      external RTC chip (timekeeping across power-off)
 *      BOARD_HAS_AUDIO_ES8311      ES8311 codec + speaker amp (chime/ding)
 *      BOARD_HAS_HAPTICS           vibration motor
 *      BOARD_HAS_SD_MMC            microSD on the SDMMC peripheral (S3)
 *      BOARD_HAS_SD_SPI            microSD on SPI (C6 — not implemented yet)
 *      BOARD_HAS_IMU_QMI8658      QMI8658 IMU (not used by the firmware yet)
 *      BOARD_HAS_BACKLIGHT_PWM     brightness = PWM backlight (vs. panel command)
 * ========================================================================== */
/* Traditional include guard (NOT just #pragma once): lv_conf.h #includes board.h
 * via a forward-slash path while the .ino includes it via a backslash path, and the
 * compiler treats those two spellings as DIFFERENT files — so #pragma once does not
 * dedupe them and the board header (with its const-int pin definitions) gets pulled
 * in twice -> "redefinition of SDMMC_DATA". A name-based guard works regardless of
 * how the path is spelled. */
#ifndef BOARD_H_INCLUDED
#define BOARD_H_INCLUDED
#pragma once

/* ---- Select the build target ---------------------------------------------
 * Set BOARD_SELECT to ONE of the BOARD_ID_* values below. This single line is
 * the only thing to change when switching boards — everything else (pins,
 * display/touch driver, feature flags, and lv_conf.h's OS layer) follows from
 * it. (A -DBOARD_SELECT=... build flag overrides this, for CI / multi-target.)
 *
 *   BOARD_ID_S3_206  — Waveshare ESP32-S3-Touch-AMOLED-2.06
 *   BOARD_ID_C6_147  — Waveshare ESP32-C6-Touch-LCD-1.47
 */
#define BOARD_ID_S3_206  1
#define BOARD_ID_C6_147  2

#ifndef BOARD_SELECT
#define BOARD_SELECT  BOARD_ID_S3_206      /* <-- change this line to pick the board */
#endif

#if   BOARD_SELECT == BOARD_ID_S3_206
#define BOARD_WS_S3_TOUCH_AMOLED_206 1
#elif BOARD_SELECT == BOARD_ID_C6_147
#define BOARD_WS_C6_TOUCH_LCD_147 1
#else
#error "board.h: BOARD_SELECT is not a known BOARD_ID_* value"
#endif

#if defined(BOARD_WS_S3_TOUCH_AMOLED_206)
#include "board_ws_s3_touch_amoled_206.h"
#elif defined(BOARD_WS_C6_TOUCH_LCD_147)
#include "board_ws_c6_touch_lcd_147.h"
#else
#error "board.h: no board selected — define one BOARD_* (see top of board.h)"
#endif

/* ---- Derived screen-geometry flags ---------------------------------------
 * Layout decisions should key off the SCREEN, not the chip/board ID — a future
 * S3 with this same tall panel, or a C6 driving the S3's wide panel, must each
 * get the layout that matches their actual display. These are derived from the
 * board's LCD_WIDTH/LCD_HEIGHT so they "just work" for any new board.
 *
 *   BOARD_SCREEN_PORTRAIT — taller than wide.
 *   BOARD_SCREEN_NARROW   — a slim portrait panel (<= 220 px wide). On these the
 *                           centered scroll containers sit far from the edges, so
 *                           the scrollbar needs nudging toward the screen edge.
 *                           (S3-2.06 410-wide is NOT narrow; C6-1.47 172-wide is.)
 */
#define BOARD_SCREEN_PORTRAIT  (LCD_HEIGHT > LCD_WIDTH)
#define BOARD_SCREEN_NARROW    (BOARD_SCREEN_PORTRAIT && (LCD_WIDTH <= 220))

/* ---- Sanity checks -------------------------------------------------------- */
#if (BOARD_DISPLAY_CO5300_QSPI + BOARD_DISPLAY_JD9853_SPI) != 1
#error "board config: exactly one BOARD_DISPLAY_* must be 1"
#endif
#if (BOARD_TOUCH_FT3168 + BOARD_TOUCH_AXS5106L) != 1
#error "board config: exactly one BOARD_TOUCH_* must be 1"
#endif
#ifndef BOARD_NAME
#error "board config: BOARD_NAME missing"
#endif

#endif /* BOARD_H_INCLUDED */
