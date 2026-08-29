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
 *  add an #elif below.
 *
 *  FLAG MODEL (Kconfig-style): every BOARD_* feature flag has a central
 *  default in the "feature-flag defaults" block below — a board header only
 *  needs to define what it HAS (or what differs from the default). A feature
 *  whose flag is 0 is fully excluded at preprocess time: its module compiles
 *  to no-op stubs (or nothing) and its libraries are never even included, so
 *  call sites never change. The defaults block below is the COMPLETE flag
 *  reference; define new flags there (default off), never as ad-hoc #ifndef
 *  defaults inside feature modules.
 *
 *    Display (exactly one =1, enforced below):
 *      BOARD_DISPLAY_CO5300_QSPI / _JD9853_SPI / _GC9A01_SPI / _MAIX / _TUYA
 *    Pointer input (exactly one =1, enforced below):
 *      BOARD_TOUCH_FT3168 / _AXS5106L / _BUTTONS / _MAIX / _TUYA
 *    Audio backend (at most one =1, enforced below; none = silent stub):
 *      BOARD_HAS_AUDIO_ES8311 / _AUDIO_PWM / _AUDIO_TUYA
 *    SD backend (at most one =1, enforced below; none = FFat-only Files app):
 *      BOARD_HAS_SD_MMC / _SD_SPI / _SD_TUYA
 *    Everything else: see the defaults block below.
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
 * it. (A -DBOARD_SELECT=... build flag overrides this, for CI / multi-target.) */
#define BOARD_ID_S3_206  1   /* Waveshare ESP32-S3-Touch-AMOLED-2.06 */
#define BOARD_ID_C6_147  2   /* Waveshare ESP32-C6-Touch-LCD-1.47 */
#define BOARD_ID_MAIX    3   /* Sipeed MaixCam-Pro on Linux (native MaixCDK app) */
#define BOARD_ID_S3_147  4   /* Waveshare ESP32-S3-Touch-LCD-1.47 */
#define BOARD_ID_CUSTOM  5   /* ESP32-S3 Super Mini, wired GC9A01 round LCD + buttons */
#define BOARD_ID_TUYA_T5 6   /* Waveshare T5-E1-Touch-AMOLED-1.75 (Tuya T5-E1, TuyaOpen) */
#define BOARD_ID_FOSSIL_GEN4 7  /* Fossil Gen 4 firefish/ray (Wear 2100, bare-metal A7) */
#define BOARD_ID_S3_18   8   /* Waveshare ESP32-S3-Touch-AMOLED-1.8 (HW V1, SH8601) */
#define BOARD_ID_S3_164  9   /* Waveshare ESP32-S3-Touch-AMOLED-1.64 */
#define BOARD_ID_S3_LCD2 10  /* Waveshare ESP32-S3-Touch-LCD-2 (OV5640 camera) */
#define BOARD_ID_S3_169  11  /* Waveshare ESP32-S3-Touch-LCD-1.69 (buzzer, soft power latch) */
#define BOARD_ID_FOSSIL_GEN6 12 /* Fossil Gen 6 hoki (Wear 4100+ / SDA429W, bare-metal A53/AArch32) */
#define BOARD_ID_TDECK_PRO 13   /* LilyGo T-Deck Pro (e-paper, QWERTY, LoRa, GNSS) */
#define BOARD_ID_TICWATCH_C2 14 /* Mobvoi TicWatch C2 skipjack (Wear 2100, bare-metal A7) */

#ifndef BOARD_SELECT
#define BOARD_SELECT  BOARD_ID_FOSSIL_GEN6         /* <-- change this line to pick the board */
#endif

#if   BOARD_SELECT == BOARD_ID_S3_206
#define BOARD_WS_S3_TOUCH_AMOLED_206 1
#elif BOARD_SELECT == BOARD_ID_S3_18
#define BOARD_WS_S3_TOUCH_AMOLED_18 1
#elif BOARD_SELECT == BOARD_ID_S3_164
#define BOARD_WS_S3_TOUCH_AMOLED_164 1
#elif BOARD_SELECT == BOARD_ID_S3_LCD2
#define BOARD_WS_S3_TOUCH_LCD_2 1
#elif BOARD_SELECT == BOARD_ID_S3_169
#define BOARD_WS_S3_TOUCH_LCD_169 1
#elif BOARD_SELECT == BOARD_ID_C6_147
#define BOARD_WS_C6_TOUCH_LCD_147 1
#elif BOARD_SELECT == BOARD_ID_S3_147
#define BOARD_WS_S3_TOUCH_LCD_147 1
#elif BOARD_SELECT == BOARD_ID_CUSTOM
#define BOARD_CUSTOM_S3_SUPERMINI 1
#elif BOARD_SELECT == BOARD_ID_MAIX
#define BOARD_MAIX_LINUX 1
#elif BOARD_SELECT == BOARD_ID_TUYA_T5
#define BOARD_TUYA_T5_AMOLED_175 1
#elif BOARD_SELECT == BOARD_ID_FOSSIL_GEN4
#define BOARD_FOSSIL_GEN4 1
#elif BOARD_SELECT == BOARD_ID_FOSSIL_GEN6
#define BOARD_FOSSIL_GEN6 1
#elif BOARD_SELECT == BOARD_ID_TDECK_PRO
#define BOARD_LILYGO_TDECK_PRO 1
#elif BOARD_SELECT == BOARD_ID_TICWATCH_C2
#define BOARD_TICWATCH_C2 1
#else
#error "board.h: BOARD_SELECT is not a known BOARD_ID_* value"
#endif

#if defined(BOARD_WS_S3_TOUCH_AMOLED_206)
#include "board_ws_s3_touch_amoled_206.h"
#elif defined(BOARD_WS_S3_TOUCH_AMOLED_18)
#include "board_ws_s3_touch_amoled_18.h"
#elif defined(BOARD_WS_S3_TOUCH_AMOLED_164)
#include "board_ws_s3_touch_amoled_164.h"
#elif defined(BOARD_WS_S3_TOUCH_LCD_2)
#include "board_ws_s3_touch_lcd_2.h"
#elif defined(BOARD_WS_S3_TOUCH_LCD_169)
#include "board_ws_s3_touch_lcd_169.h"
#elif defined(BOARD_WS_C6_TOUCH_LCD_147)
#include "board_ws_c6_touch_lcd_147.h"
#elif defined(BOARD_WS_S3_TOUCH_LCD_147)
#include "board_ws_s3_touch_lcd_147.h"
#elif defined(BOARD_CUSTOM_S3_SUPERMINI)
#include "board_custom_s3_supermini.h"
#elif defined(BOARD_MAIX_LINUX)
#include "board_maix_linux.h"
#elif defined(BOARD_TUYA_T5_AMOLED_175)
#include "board_tuya_t5_amoled_175.h"
#elif defined(BOARD_FOSSIL_GEN4)
#include "board_fossil_gen4.h"
#elif defined(BOARD_FOSSIL_GEN6)
#include "board_fossil_gen6.h"
#elif defined(BOARD_LILYGO_TDECK_PRO)
#include "board_lilygo_tdeck_pro.h"
#elif defined(BOARD_TICWATCH_C2)
#include "board_ticwatch_c2.h"
#else
#error "board.h: no board selected — define one BOARD_* (see top of board.h)"
#endif

/* ---- Which bare-metal watches share the Snapdragon Wear 2100 --------------
 * The Fossil Gen 4 and the Mobvoi TicWatch C2 are the same SoC (msm8909w /
 * APQ8009W) from different vendors, so anything that depends on the SILICON
 * rather than on the watch is true of both: the CPU frequency ladder, the
 * fact that the core rail is a readable PM8916 SMPS2, the A7 clock RCG.
 *
 * Before this existed, those places tested `BOARD_SELECT == BOARD_ID_FOSSIL_GEN4`
 * and the C2 silently fell through to the Gen 6 branch — which offered a
 * Wear 4100 frequency ladder (up to 1306 MHz) on a part that cannot reach it.
 * Mirrors PLAT_SOC_MSM8909 in fossil-port/baremetal/platform/platform.h; keep
 * the two in step when a third Wear 2100 watch arrives (the TicWatch S2/E2,
 * codename tunny, is the likely next one). */
#if BOARD_SELECT == BOARD_ID_FOSSIL_GEN4 || BOARD_SELECT == BOARD_ID_TICWATCH_C2
#define BOARD_SOC_MSM8909 1
#else
#define BOARD_SOC_MSM8909 0
#endif

/* ===================== feature-flag defaults (the flag reference) ============
 * Every optional BOARD_* flag defaults OFF here (Kconfig model: a board opts IN
 * to what it has). A board header therefore only defines its features; anything
 * it doesn't mention is 0 and the matching code path is excluded at preprocess
 * time. Two deliberate exceptions default ON (BOARD_HAS_BLE, BOARD_HAS_FFAT) —
 * the historical assumption on the ESP boards; the odd platform out opts OUT.
 * NEW FLAGS GO HERE, not as #ifndef defaults inside feature modules. */

/* -- platform (external-framework ports; everything else is ESP-Arduino) ---- */
#ifndef BOARD_PLATFORM_MAIX
#define BOARD_PLATFORM_MAIX 0        /* Sipeed MaixCAM (Linux, MaixCDK owns display) */
#endif
#ifndef BOARD_PLATFORM_TUYA
#define BOARD_PLATFORM_TUYA 0        /* Tuya T5-E1 (TuyaOpen, tdl display layer) */
#endif
#ifndef BOARD_PLATFORM_FOSSIL
#define BOARD_PLATFORM_FOSSIL 0      /* Fossil watches, bare-metal (fossil-port runtime) */
#endif

/* -- display (exactly one =1; checked below) -------------------------------- */
#ifndef BOARD_DISPLAY_CO5300_QSPI
#define BOARD_DISPLAY_CO5300_QSPI 0  /* CO5300 AMOLED on QSPI (Arduino_CO5300) */
#endif
#ifndef BOARD_DISPLAY_SH8601_QSPI
#define BOARD_DISPLAY_SH8601_QSPI 0  /* SH8601 AMOLED on QSPI (Arduino_SH8601) */
#endif
#ifndef BOARD_DISPLAY_JD9853_SPI
#define BOARD_DISPLAY_JD9853_SPI 0   /* JD9853 LCD on classic SPI (ST7789-class) */
#endif
#ifndef BOARD_DISPLAY_GC9A01_SPI
#define BOARD_DISPLAY_GC9A01_SPI 0   /* GC9A01 round LCD on SPI (Super Mini) */
#endif
#ifndef BOARD_DISPLAY_ST7789_SPI
#define BOARD_DISPLAY_ST7789_SPI 0   /* genuine ST7789 LCD on classic SPI (S3-Touch-LCD-2).
                                      * Same Arduino_ST7789 class as the JD9853 boards, but
                                      * the library's init is complete — no vendor table. */
#endif
#ifndef BOARD_DISPLAY_MAIX
#define BOARD_DISPLAY_MAIX 0
#endif
#ifndef BOARD_DISPLAY_TUYA
#define BOARD_DISPLAY_TUYA 0
#endif
#ifndef BOARD_DISPLAY_MSM_DSI
#define BOARD_DISPLAY_MSM_DSI 0      /* Qualcomm MDP3/DSI cmd-mode (fossil-port) */
#endif
#ifndef BOARD_DISPLAY_EPD_GDEQ031T10
#define BOARD_DISPLAY_EPD_GDEQ031T10 0  /* GDEQ031T10 320x240 e-paper via GxEPD2 (T-Deck Pro).
                                         * NOT emissive and NOT per-tile refreshable: the flush
                                         * blits to a 1bpp shadow buffer and loop() drives the
                                         * panel. See display_epd_gdeq031t10.h. */
#endif

/* -- pointer input (exactly one =1; checked below) --------------------------- */
#ifndef BOARD_TOUCH_FT3168
#define BOARD_TOUCH_FT3168 0         /* FT3168 via Arduino_DriveBus */
#endif
#ifndef BOARD_TOUCH_AXS5106L
#define BOARD_TOUCH_AXS5106L 0       /* AXS5106L via esp_lcd_touch_axs5106l */
#endif
#ifndef BOARD_TOUCH_CST816
#define BOARD_TOUCH_CST816 0         /* CST816D/S over I2C, polled (S3-Touch-LCD-2) */
#endif
#ifndef BOARD_TOUCH_CST328
#define BOARD_TOUCH_CST328 0         /* CST328/CST226SE over I2C @ 0x1A (T-Deck Pro).
                                      * Despite the name, NOT the CST816 driver: different
                                      * address and a completely different protocol (a
                                      * 28-byte validated status block). See touch_cst328.h. */
#endif
#ifndef BOARD_TOUCH_BUTTONS
#define BOARD_TOUCH_BUTTONS 0        /* no panel: physical-button nav layer */
#endif
#ifndef BOARD_TOUCH_MAIX
#define BOARD_TOUCH_MAIX 0
#endif
#ifndef BOARD_TOUCH_TUYA
#define BOARD_TOUCH_TUYA 0
#endif
#ifndef BOARD_TOUCH_RAYDIUM
#define BOARD_TOUCH_RAYDIUM 0        /* Raydium RM_TS I2C (fossil-port) */
#endif

/* -- SoC / memory ------------------------------------------------------------ */
#ifndef BOARD_HAS_PSRAM
#define BOARD_HAS_PSRAM 0            /* external PSRAM (stores/snapshot cache) */
#endif
#ifndef BOARD_DUAL_CORE
#define BOARD_DUAL_CORE 0            /* 2nd core (band-split render, net task pin) */
#endif

/* -- power / battery --------------------------------------------------------- */
#ifndef BOARD_HAS_PMU_AXP2101
#define BOARD_HAS_PMU_AXP2101 0      /* AXP2101 PMU (gauge, rails, power-off) */
#endif
#ifndef BOARD_HAS_ADC_BATTERY
#define BOARD_HAS_ADC_BATTERY 0      /* no PMU: battery on an ADC divider */
#endif
/* TI BQ27220 coulomb-counting fuel gauge + BQ25896 charger over I2C (T-Deck Pro).
 * A THIRD battery backend alongside the AXP2101 PMU and the ADC divider: the
 * gauge answers cell questions (percent / mV / temperature / health) and the
 * charger answers input questions (VBUS present, charge state). They are separate
 * chips and separate flags because a board could fit either alone. */
#ifndef BOARD_HAS_GAUGE_BQ27220
#define BOARD_HAS_GAUGE_BQ27220 0
#endif
#ifndef BOARD_HAS_CHARGER_BQ25896
#define BOARD_HAS_CHARGER_BQ25896 0
#endif

/* -- timekeeping ------------------------------------------------------------- */
#ifndef BOARD_HAS_RTC_PCF85063
#define BOARD_HAS_RTC_PCF85063 0     /* external RTC chip (time across power-off) */
#endif

/* -- audio backend (at most one =1; none = silent no-op stub) ---------------- */
#ifndef BOARD_HAS_AUDIO_ES8311
#define BOARD_HAS_AUDIO_ES8311 0     /* ES8311 codec over I2S + amp gate */
#endif
#ifndef BOARD_HAS_AUDIO_PWM
#define BOARD_HAS_AUDIO_PWM 0        /* LEDC PWM piezo/speaker beeper */
#endif
#ifndef BOARD_HAS_AUDIO_TUYA
#define BOARD_HAS_AUDIO_TUYA 0       /* T5 internal codec via tdl_audio */
#endif

/* -- SD card backend (at most one =1; none = FFat-only Files app) ------------ */
#ifndef BOARD_HAS_SD_MMC
#define BOARD_HAS_SD_MMC 0           /* SDMMC peripheral (S3) */
#endif
#ifndef BOARD_HAS_SD_MMC_4BIT
#define BOARD_HAS_SD_MMC_4BIT 0      /* 4-bit SDMMC wiring (else 1-bit D0 only) */
#endif
#ifndef BOARD_HAS_SD_SPI
#define BOARD_HAS_SD_SPI 0           /* SD over SPI (C6-1.47) */
#endif
#ifndef BOARD_HAS_SD_TUYA
#define BOARD_HAS_SD_TUYA 0          /* tkl_fs DEV_SDCARD (T5) */
#endif

/* -- motion / steps ----------------------------------------------------------- */
#ifndef BOARD_HAS_IMU_QMI8658
#define BOARD_HAS_IMU_QMI8658 0      /* QMI8658 accel (steps + sleep tracking) */
#endif
#ifndef BOARD_HAS_ULP_STEPS
#define BOARD_HAS_ULP_STEPS 0        /* deep-sleep steps on the S3 RISC-V ULP */
#endif
#ifndef BOARD_HAS_LP_STEPS
#define BOARD_HAS_LP_STEPS 0         /* deep-sleep steps on the C6 LP core */
#endif

/* -- misc peripherals ---------------------------------------------------------- */
#ifndef BOARD_HAS_HAPTICS
#define BOARD_HAS_HAPTICS 0          /* vibration motor */
#endif
#ifndef BOARD_HAS_BACKLIGHT_PWM
#define BOARD_HAS_BACKLIGHT_PWM 0    /* PWM backlight (else panel brightness cmd) */
#endif
#ifndef BOARD_HAS_NAV_BUTTONS
#define BOARD_HAS_NAV_BUTTONS 0      /* physical prev/select/next buttons */
#endif
/* A ROTATING CROWN reported as relative motion. Not a quadrature encoder on
 * two GPIOs -- the Fossil Gen 4 puts an optical motion sensor (PixArt PAT9126,
 * platform/rot_pat9126.c) under the wheel, so the crown arrives as signed
 * counts from a driver rather than as edges on pins. A board that has one
 * defines this and provides crown_take_delta(); crown_nav.h does the rest and
 * compiles to nothing when the flag is 0. */
#ifndef BOARD_HAS_CROWN
#define BOARD_HAS_CROWN 0            /* rotating crown -> scroll / quick-shade */
#endif
/* A full physical keyboard behind a TCA8418 I2C matrix scanner (T-Deck Pro).
 * Distinct from BOARD_HAS_NAV_BUTTONS (three GPIO buttons acting as the sole
 * pointer substitute): this is an ADDITIONAL LVGL keypad indev registered
 * alongside a working touch panel, not a replacement for one. */
#ifndef BOARD_HAS_KEYBOARD_TCA8418
#define BOARD_HAS_KEYBOARD_TCA8418 0
#endif
/* A DVP camera sensor wired to the SoC's camera peripheral (esp32-camera driver).
 * Gates the whole Camera app INCLUDING its menu tile, so a board with no sensor
 * never shows an app that could only fail. A board that has one defines this plus
 * the CAM_PIN_* map and CAM_XCLK_HZ. */
#ifndef BOARD_HAS_CAMERA
#define BOARD_HAS_CAMERA 0           /* DVP camera sensor (OV5640 on the S3-Touch-LCD-2) */
#endif

/* -- deep-sleep wake ----------------------------------------------------------- */
#ifndef BOARD_WAKE_USE_EXT0
#define BOARD_WAKE_USE_EXT0 0        /* BOOT press wakes deep sleep via EXT0 (S3) */
#endif
#ifndef BOARD_TRY_GPIO9_WAKE
#define BOARD_TRY_GPIO9_WAKE 0       /* C6 experiment: GPIO9 as deep-sleep wake */
#endif
/* (BOARD_WAKE_GPIO is an optional PIN macro, tested with #ifdef — no default.) */

/* -- default-ON exceptions ------------------------------------------------------ */
/* BLE is assumed present on the ESP32 boards (NimBLE always built in); only the
 * Maix port turns it off. Default to on so the ESP boards are unaffected. */
#ifndef BOARD_HAS_BLE
#define BOARD_HAS_BLE 1
#endif
/* A real panel RESET GPIO. Every board so far breaks one out, and the cold-boot
 * reset-hold in the .ino drives it directly — so this defaults ON and a board
 * with no reset line (S3-1.8: LCD_RESET is GFX_NOT_DEFINED) opts OUT with 0.
 * Note this is the one flag that must default 1 rather than 0, because the
 * historical code unconditionally assumed a reset pin existed. */
#ifndef BOARD_LCD_HAS_RESET
#define BOARD_LCD_HAS_RESET 1
#endif
/* On-flash FAT partition (FFat) for persistence/Files. Present on the ESP boards; ABSENT on
 * the T5 (its flash layout has no user FS partition — only Tuya's KV store), so DEV_INNER_FLASH
 * never mounts there. A board with no flash FS sets this 0 so the Files app hides the "Flash"
 * volume and consumers don't offer it. Default 1 (the historical assumption). */
#ifndef BOARD_HAS_FFAT
#define BOARD_HAS_FFAT 1
#endif

/* I2C bring-up is spelled differently per Arduino core: ESP32 takes the pins as
 * begin(sda,scl); TuyaOpen's TwoWire has no-arg begin() + setSDA/setSCL. OWF_WIRE_BEGIN
 * hides that so the call sites stay board-neutral. (Expanded only where Wire exists.) */
#if BOARD_PLATFORM_TUYA
#define OWF_WIRE_BEGIN(sda, scl)  do { Wire.setSDA(sda); Wire.setSCL(scl); Wire.begin(); } while (0)
#else
#define OWF_WIRE_BEGIN(sda, scl)  Wire.begin((sda), (scl))
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
/* KEEP THIS AT 220 — it means "slim panel" (the C6-1.47 at 172) and nothing else.
 * It is tempting to widen it to pull a mid-size panel onto the fluid layouts,
 * because ~70 sites use it as a de-facto "fluid vs fixed-410 geometry" switch.
 * DON'T: it also drives screens that are already correct on a mid-size panel
 * (the watch-face dial and the quick shade both shrink to their slim variants),
 * so widening it regresses those to fix others. Use BOARD_SCREEN_MIDNARROW for a
 * middle tier and opt individual screens in. */
#define BOARD_SCREEN_NARROW    (BOARD_SCREEN_PORTRAIT && (LCD_WIDTH <= 220))
/*   BOARD_SCREEN_MIDNARROW — the MIDDLE tier: portrait, too wide to be "narrow"
 *                           (so the one-column narrow layouts would waste it) but
 *                           well short of the 410-wide reference the UI was
 *                           authored against, AND noticeably TALL for its width.
 *                           The S3-1.64 (280x456) is the case it exists for: a
 *                           widescreen-shaped panel stood upright.
 *
 *   Why this tier exists: layouts that simply pick "narrow vs not" gave that panel
 *   the full 410-wide treatment — e.g. the launcher's 3-column grid — which fits
 *   across but leaves tiles so small the icon collides with a wrapped label. A
 *   derived tier lets those screens choose a middle layout (2 columns there)
 *   without hard-coding a board ID.
 *
 *   THE ASPECT TERM IS LOAD-BEARING, not a refinement of the width test. What
 *   actually cramps the 3-column grid is the panel being TALL for its width: the
 *   tile box is sized to fit MENU_COLS across, so on a 280-wide panel a third of the
 *   width is a small tile, and the tall panel then leaves that small tile with a
 *   label that must wrap into the icon. A panel that is merely mid-WIDTH but close
 *   to 4:3 does not have that problem — 3 columns of it are proportionally the same
 *   as 3 columns of the 410-wide reference.
 *
 *   Concretely: S3-1.64 is 280x456 = 1.63:1 -> MIDNARROW (2 columns).
 *               S3-Touch-LCD-2 is 240x320 = 1.33:1 -> NOT midnarrow, keeps the
 *               reference 3x3 grid, which fits comfortably on a squarer panel.
 *   Threshold 1.5:1 (h*2 > w*3) sits between those two and well clear of both.
 *   Width bounds unchanged: above the 220 narrow cutoff, below the 368-wide S3-1.8. */
#define BOARD_SCREEN_MIDNARROW (BOARD_SCREEN_PORTRAIT && (LCD_WIDTH > 220) && (LCD_WIDTH <= 320) \
                                && ((LCD_HEIGHT) * 2 > (LCD_WIDTH) * 3))
/*   BOARD_SCREEN_LOWRES_DIAL — the panel is big enough for the reference layouts
 *                           (so NOT narrow, and it keeps the 3-column launcher)
 *                           but too small for the 110 px watch-face dial glyph.
 *
 *   Separate from the layout tiers on purpose: this is about ONE oversized
 *   pre-rendered font, not about how screens are laid out. The S3-Touch-LCD-2
 *   (240x320) is the case — its layouts are fine at the reference geometry, but
 *   "HH:MM" at 110 px nearly spans the 240 px width with no margin left for the
 *   rows around it. ui_fonts.h swaps in the 88 px cut of the same glyph set here.
 *
 *   Threshold 260 wide, and it is deliberately TIGHT. The next panel up is the
 *   S3-1.64 at 280 wide, which has been running the 110 px dial happily — so the
 *   cutoff must sit BELOW 280 or this flag would silently re-style a known-good
 *   board that nobody asked to change. 260 leaves clear air on both sides: this
 *   panel (240) is in, the 1.64 (280) is out.
 *
 *   Width only, not height: the dial constraint is "HH:MM" running out of
 *   HORIZONTAL room. A tall-but-slim panel is already handled by
 *   BOARD_SCREEN_NARROW, which gives it a much smaller stock tier. */
#define BOARD_SCREEN_LOWRES_DIAL (!BOARD_SCREEN_NARROW && ((LCD_WIDTH) <= 260))
/*   BOARD_SCREEN_SUBREF — the panel is narrower than the 410 px reference the app
 *                        screens were authored against, so any FIXED reference
 *                        pixel width (374 / 372 / 360 / 330 ...) overhangs it.
 *
 *   This is deliberately SEPARATE from the layout tiers above, because it answers
 *   a different question. MIDNARROW asks "how many launcher columns fit"; this asks
 *   "does a 374 px container fit". A panel can easily be wide enough for a 3-column
 *   grid yet far too narrow for a 374 px column — the S3-Touch-LCD-2 (240 wide) is
 *   exactly that case, and while it was MIDNARROW it got the percentage widths for
 *   the WRONG reason (as a side effect of the grid tier). Tying the two together
 *   meant fixing the grid silently re-broke every app screen's width.
 *
 *   Anything below ~340 px cannot hold the widest fixed container (374) plus a
 *   margin, so it must use percentage widths instead. The S3-1.8 (368) and the
 *   S3-2.06 (410) are reference-class and keep the ORIGINAL fixed values, which
 *   matters: several of those containers hold rows with pixel offsets tuned to
 *   that exact width. See UI_COL_W() in ui_scale.h. */
#define BOARD_SCREEN_SUBREF    ((LCD_WIDTH) < 340)
/* A ROUND panel (GC9A01) is square in memory but the corners are clipped by the
 * bezel, so layout must keep content within an inscribed circle (no corner UI, no
 * edge-anchored scrollbars). A board declares it explicitly since it can't be
 * derived from W/H alone (a square panel may be round or just square). */
#ifndef BOARD_SCREEN_ROUND
#define BOARD_SCREEN_ROUND 0
#endif

/*   BOARD_SCREEN_ROUND_SMALL — a round panel small enough that the reference
 * layout no longer fits VERTICALLY. This is a genuinely different case from every
 * other tier here, and the reason is that the existing scaling asks the wrong
 * question of a round panel:
 *
 *   - UI_PX()/ui_px_scale() key off WIDTH, and the height penalty in ui_px_scale
 *     is gated on BOARD_SCREEN_PORTRAIT — so a SQUARE panel gets no height term
 *     at all. That is right for a tall panel and wrong for a round one.
 *   - BOARD_SCREEN_SUBREF is (LCD_WIDTH < 340), so a 360-wide round face reads as
 *     a "reference-class" panel and keeps the full 410x502 layout... except it is
 *     only 360 px TALL, i.e. 72% of the reference height, and the watch face and
 *     launcher are both vertically stacked. The result is the reported symptom on
 *     the TicWatch C2: the 110 px dial ran up into the top stat row, and the
 *     launcher's bottom tile row sat on top of the page dots and the "< BOOT"
 *     hint.
 *
 * The accepted round boards all clear the bar comfortably (T5 466, Fossil Gen 4
 * 454, Gen 6 416 — all >= 82% of the 502 reference height), so the threshold is
 * set below them and above the 360-class faces. The TicWatch C2 and the S2/E2
 * (also 360x360) are what this selects, and they pick up the whole compact tier
 * automatically — nothing here is keyed to a board id. */
#define BOARD_SCREEN_ROUND_SMALL (BOARD_SCREEN_ROUND && ((LCD_HEIGHT) <= 380))

/* ---- Sanity checks -------------------------------------------------------- */
#if (BOARD_DISPLAY_CO5300_QSPI + BOARD_DISPLAY_SH8601_QSPI + BOARD_DISPLAY_JD9853_SPI + BOARD_DISPLAY_GC9A01_SPI + BOARD_DISPLAY_ST7789_SPI + BOARD_DISPLAY_MAIX + BOARD_DISPLAY_TUYA + BOARD_DISPLAY_MSM_DSI + BOARD_DISPLAY_EPD_GDEQ031T10) != 1
#error "board config: exactly one BOARD_DISPLAY_* must be 1"
#endif
/* An e-paper panel has no backlight to modulate. Catch the contradiction here
 * rather than letting analogWrite() drive a pin that isn't a backlight. */
#if BOARD_DISPLAY_EPD_GDEQ031T10 && BOARD_HAS_BACKLIGHT_PWM
#error "board config: an e-paper panel cannot have BOARD_HAS_BACKLIGHT_PWM"
#endif
/* "Touch" here means the POINTER input source: a capacitive panel, the Maix
 * touchscreen, or — on a button-only board — the physical-button nav layer. Exactly
 * one input source must be selected. */
#if (BOARD_TOUCH_FT3168 + BOARD_TOUCH_AXS5106L + BOARD_TOUCH_CST816 + BOARD_TOUCH_CST328 + BOARD_TOUCH_BUTTONS + BOARD_TOUCH_MAIX + BOARD_TOUCH_TUYA + BOARD_TOUCH_RAYDIUM) != 1
#error "board config: exactly one input source (BOARD_TOUCH_* / BOARD_TOUCH_BUTTONS) must be 1"
#endif
/* At most one backend each; ZERO is valid (audio -> silent stub, SD -> FFat only). */
#if (BOARD_HAS_AUDIO_ES8311 + BOARD_HAS_AUDIO_PWM + BOARD_HAS_AUDIO_TUYA) > 1
#error "board config: at most one BOARD_HAS_AUDIO_* backend may be 1"
#endif
#if (BOARD_HAS_SD_MMC + BOARD_HAS_SD_SPI + BOARD_HAS_SD_TUYA) > 1
#error "board config: at most one BOARD_HAS_SD_* backend may be 1"
#endif
/* The deep-sleep step paths ride on the IMU driver; a blob with no accel is a bug. */
#if (BOARD_HAS_ULP_STEPS || BOARD_HAS_LP_STEPS) && !BOARD_HAS_IMU_QMI8658
#error "board config: BOARD_HAS_ULP_STEPS/LP_STEPS requires BOARD_HAS_IMU_QMI8658"
#endif
#ifndef BOARD_NAME
#error "board config: BOARD_NAME missing"
#endif

/* ---- Derived touch-interrupt flag -----------------------------------------
 * Is the touch controller's INT line actually wired to a GPIO we can attach an
 * ISR to? Most boards break it out; some do not (the S3-1.64, whose vendor demos
 * poll the FT3168 and never reference an INT pin), and those set TP_INT to -1 —
 * which is also the DriveBus library's DRIVEBUS_DEFAULT_VALUE "no pin" sentinel,
 * so its Arduino_IIC ctor skips pinMode/attachInterrupt entirely.
 *
 * This MATTERS beyond the ISR itself: my_touchpad_read()'s zero-I2C fast path
 * early-outs when IIC_Interrupt_Flag is false, and that flag is ONLY ever set by
 * the touch ISR. On a board with no INT line the flag is therefore permanently
 * false, the fast path returns "released" forever, and TOUCH IS COMPLETELY DEAD
 * — the full I2C read is never reached. So the fast path must be compiled out
 * where there is no interrupt, and the driver polled instead (exactly what the
 * vendor demo does). Gate on this, not on a board ID.
 *
 * A board with no pointer panel at all (the button-only Super Mini) also sets
 * TP_INT -1, but it selects BOARD_TOUCH_BUTTONS so none of this code is built.
 *
 * NB: resolved with #ifdef/#if here rather than as a one-line
 * "(defined(TP_INT) && (TP_INT) >= 0)" — using defined() inside a macro body is
 * undefined behaviour in the preprocessor, and some boards genuinely leave
 * TP_INT undefined. */
#ifdef TP_INT
#if (TP_INT) >= 0
#define BOARD_HAS_TOUCH_INT 1
#else
#define BOARD_HAS_TOUCH_INT 0
#endif
#else
#define BOARD_HAS_TOUCH_INT 0
#endif

/* ---- Derived display-bus flag --------------------------------------------
 * Both the JD9853 and the GC9A01 panels are classic-SPI displays driven through the
 * GFX library's Arduino_ESP32SPIDMA bus with the LOCAL async-flush patch (the
 * writePixelsAsync/waitAsync/set_sync_only path + the spidma_bus/spidma_tft typed
 * pointers). Code that touches that BUS (not the panel registers) gates on this so a
 * new SPIDMA panel works without editing every site. PANEL-specific bring-up (e.g.
 * the JD9853 vendor register table) still gates on the exact display flag. */
#define BOARD_DISPLAY_SPIDMA  (BOARD_DISPLAY_JD9853_SPI + BOARD_DISPLAY_GC9A01_SPI + BOARD_DISPLAY_ST7789_SPI)

/* ---- Derived AMOLED-QSPI flag ---------------------------------------------
 * The CO5300 (S3-2.06) and the SH8601 (S3-1.8 HW V1) are different panels but
 * the SAME integration: both are Arduino_OLED subclasses driven through
 * Arduino_ESP32QSPI, both take brightness via panel command 0x51 (setBrightness
 * on the derived class, not on Arduino_GFX), and both ride the LOCAL async-DMA
 * patch (writePixelsBeAsync/waitAsync on the QSPI bus). Code that touches that
 * BUS or that shared API gates on THIS flag, so a third QSPI AMOLED works
 * without editing every site — exactly like BOARD_DISPLAY_SPIDMA above unifies
 * the two classic-SPI panels.
 *
 * PANEL-specific differences still gate on the exact display flag:
 *   - construction (which class, and its offsets)
 *   - BOARD_LCD_EVEN_ALIGN (CO5300 requires even-aligned draw areas; SH8601
 *     does not)
 * The shared object is reached through the `amoled` pointer declared in the
 * .ino (typed as the concrete panel class per board). */
#define BOARD_DISPLAY_AMOLED_QSPI  (BOARD_DISPLAY_CO5300_QSPI + BOARD_DISPLAY_SH8601_QSPI)

/* ---- Derived deferred-refresh flag ----------------------------------------
 * True for panels whose flush callback does NOT reach the glass: it renders into
 * a shadow buffer and a SEPARATE service call, driven from loop(), pushes the
 * panel later. E-paper is the case that forces this (a refresh costs 0.5-3 s and
 * flashes, so it cannot happen per LVGL dirty tile).
 *
 * Gate on THIS rather than on the e-paper flag wherever the question is "has my
 * drawing actually reached the screen yet" — a future memory-LCD or any other
 * slow/bistable panel wants the same treatment. Consequences for call sites:
 *   - a flush returning does not mean the user can see the change;
 *   - anything that draws and then immediately sleeps must service the panel
 *     first, or the last frame is lost;
 *   - brightness/backlight controls are meaningless (reflective panel). */
#define BOARD_DISPLAY_DEFERRED_REFRESH  (BOARD_DISPLAY_EPD_GDEQ031T10)

/* ---- Derived shared-bus flag ----------------------------------------------
 * True only where the microSD is a SECOND DEVICE on the display's SPI host (the C6-1.47:
 * BOARD_HAS_SD_SPI). That combination needs the cross-task display<->SD arbitration
 * (s_dispsd_mtx / DispBusGuard / sd_bus_lock sync-only handshake in the .ino). SPIDMA
 * boards WITHOUT a shared SD (S3-1.47, S3 super mini) skip all of it — their bus has
 * one device, so there is nothing to arbitrate and no reason to pay a mutex per flush. */
#define BOARD_SD_SHARES_LCD_BUS  (BOARD_DISPLAY_SPIDMA && BOARD_HAS_SD_SPI)
/* NOTE — the T-Deck Pro also puts its microSD (and its SX1262) on the panel's SPI
 * host, yet resolves this to 0, because it is NOT a SPIDMA board. That is
 * deliberate, not an oversight: the arbitration this flag turns on exists to stop
 * an SD access landing in the middle of a flush's DMA burst, and on a deferred-
 * refresh panel the flush issues NO bus traffic at all (it writes to RAM). The
 * panel touches the bus only inside epd_service() in loop(), which is a single
 * synchronous call on the main task — so there is no window for a flush and an SD
 * op to interleave, and nothing to arbitrate.
 *
 * This stops being true the moment anything drives the panel from ANOTHER task,
 * or the LoRa radio is driven from a task other than the one calling
 * epd_service(). Either change means this board needs real arbitration — most
 * likely a bus mutex covering all three chip selects rather than this display<->SD
 * pair flag. */

/* ---- Tuya T5: force LVGL's swapped-RGB565 sw blender ON -------------------
 * The T5 renders directly in the CO5300's native byte order (LV_COLOR_FORMAT_RGB565_SWAPPED)
 * so there is no per-frame software byte-swap. That needs LVGL's swapped sw blender compiled
 * in. We pre-define the support flag HERE (board.h is included before <lvgl.h> in every TU);
 * lv_conf_internal guards it with #ifndef, so this wins regardless of how the conf resolves.
 * (The vendor SDK's older LVGL headers used to shadow ours via -iwithprefixbefore and force
 * this flag to 0 through KConfig; those include paths have been removed from the T5 vendor
 * flags so only our v9.5 library is seen. This pre-define is kept as belt-and-suspenders.) */
#if BOARD_PLATFORM_TUYA
#ifndef LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 1
#endif
#endif

/* ---- OWF_STAGE: boot-progress marker for bare-metal bring-up --------------
 * On the Fossil port the ONLY working debug channel is when the watchdog
 * reboots the watch, so setup() marks its progress and a stopwatch reads it
 * back (see fossil-port/baremetal/platform/msm_wdog.c). Compiles to nothing
 * everywhere else, and on the Fossil unless -DWDOG_TRACE is set. */
#ifndef OWF_STAGE
#define OWF_STAGE(n) ((void)0)
#endif

/* OWF_VTRACE: paint-a-color boot milestone (Fossil VISUAL_TRACE builds only —
 * the display path is proven, so the glass replaces the wdog stopwatch). */
#ifndef OWF_VTRACE
#define OWF_VTRACE(c) ((void)0)
#endif

#endif /* BOARD_H_INCLUDED */
