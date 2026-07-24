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
 * it. (A -DBOARD_SELECT=... build flag overrides this, for CI / multi-target.)
 *
 *   BOARD_ID_S3_206  — Waveshare ESP32-S3-Touch-AMOLED-2.06
 *   BOARD_ID_S3_18   — Waveshare ESP32-S3-Touch-AMOLED-1.8 (HW V1, SH8601 panel)
 *   BOARD_ID_C6_147  — Waveshare ESP32-C6-Touch-LCD-1.47
 *   BOARD_ID_S3_147  — Waveshare ESP32-S3-Touch-LCD-1.47 (C6-1.47 twin, S3 SoC, no IMU)
 *   BOARD_ID_CUSTOM  — ESP32-S3 Super Mini + wired GC9A01 round LCD + 3 nav buttons (no touch)
 *   BOARD_ID_TUYA_T5 - Waveshare T5-E1-Touch-AMOLED-1.75 (Tuya T5-E1, TuyaOpen)
 */
#define BOARD_ID_S3_206  1   /* Waveshare ESP32-S3-Touch-AMOLED-2.06 */
#define BOARD_ID_C6_147  2   /* Waveshare ESP32-C6-Touch-LCD-1.47 */
#define BOARD_ID_MAIX    3   /* Sipeed MaixCam-Pro on Linux (native MaixCDK app) */
#define BOARD_ID_S3_147  4   /* Waveshare ESP32-S3-Touch-LCD-1.47 */
#define BOARD_ID_CUSTOM  5   /* ESP32-S3 Super Mini, wired GC9A01 round LCD + buttons */
#define BOARD_ID_TUYA_T5 6   /* Waveshare T5-E1-Touch-AMOLED-1.75 (Tuya T5-E1, TuyaOpen) */
#define BOARD_ID_FOSSIL_GEN4 7  /* Fossil Gen 4 firefish/ray (Wear 2100, bare-metal A7) */
#define BOARD_ID_S3_18   8   /* Waveshare ESP32-S3-Touch-AMOLED-1.8 (HW V1, SH8601) */

#ifndef BOARD_SELECT
#define BOARD_SELECT  BOARD_ID_S3_206      /* <-- change this line to pick the board */
#endif

#if   BOARD_SELECT == BOARD_ID_S3_206
#define BOARD_WS_S3_TOUCH_AMOLED_206 1
#elif BOARD_SELECT == BOARD_ID_S3_18
#define BOARD_WS_S3_TOUCH_AMOLED_18 1
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
#else
#error "board.h: BOARD_SELECT is not a known BOARD_ID_* value"
#endif

#if defined(BOARD_WS_S3_TOUCH_AMOLED_206)
#include "board_ws_s3_touch_amoled_206.h"
#elif defined(BOARD_WS_S3_TOUCH_AMOLED_18)
#include "board_ws_s3_touch_amoled_18.h"
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
#else
#error "board.h: no board selected — define one BOARD_* (see top of board.h)"
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
#ifndef BOARD_DISPLAY_MAIX
#define BOARD_DISPLAY_MAIX 0
#endif
#ifndef BOARD_DISPLAY_TUYA
#define BOARD_DISPLAY_TUYA 0
#endif
#ifndef BOARD_DISPLAY_MSM_DSI
#define BOARD_DISPLAY_MSM_DSI 0      /* Qualcomm MDP3/DSI cmd-mode (fossil-port) */
#endif

/* -- pointer input (exactly one =1; checked below) --------------------------- */
#ifndef BOARD_TOUCH_FT3168
#define BOARD_TOUCH_FT3168 0         /* FT3168 via Arduino_DriveBus */
#endif
#ifndef BOARD_TOUCH_AXS5106L
#define BOARD_TOUCH_AXS5106L 0       /* AXS5106L via esp_lcd_touch_axs5106l */
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
#define BOARD_SCREEN_NARROW    (BOARD_SCREEN_PORTRAIT && (LCD_WIDTH <= 220))
/* A ROUND panel (GC9A01) is square in memory but the corners are clipped by the
 * bezel, so layout must keep content within an inscribed circle (no corner UI, no
 * edge-anchored scrollbars). A board declares it explicitly since it can't be
 * derived from W/H alone (a square panel may be round or just square). */
#ifndef BOARD_SCREEN_ROUND
#define BOARD_SCREEN_ROUND 0
#endif

/* ---- Sanity checks -------------------------------------------------------- */
#if (BOARD_DISPLAY_CO5300_QSPI + BOARD_DISPLAY_SH8601_QSPI + BOARD_DISPLAY_JD9853_SPI + BOARD_DISPLAY_GC9A01_SPI + BOARD_DISPLAY_MAIX + BOARD_DISPLAY_TUYA + BOARD_DISPLAY_MSM_DSI) != 1
#error "board config: exactly one BOARD_DISPLAY_* must be 1"
#endif
/* "Touch" here means the POINTER input source: a capacitive panel, the Maix
 * touchscreen, or — on a button-only board — the physical-button nav layer. Exactly
 * one input source must be selected. */
#if (BOARD_TOUCH_FT3168 + BOARD_TOUCH_AXS5106L + BOARD_TOUCH_BUTTONS + BOARD_TOUCH_MAIX + BOARD_TOUCH_TUYA + BOARD_TOUCH_RAYDIUM) != 1
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

/* ---- Derived display-bus flag --------------------------------------------
 * Both the JD9853 and the GC9A01 panels are classic-SPI displays driven through the
 * GFX library's Arduino_ESP32SPIDMA bus with the LOCAL async-flush patch (the
 * writePixelsAsync/waitAsync/set_sync_only path + the spidma_bus/spidma_tft typed
 * pointers). Code that touches that BUS (not the panel registers) gates on this so a
 * new SPIDMA panel works without editing every site. PANEL-specific bring-up (e.g.
 * the JD9853 vendor register table) still gates on the exact display flag. */
#define BOARD_DISPLAY_SPIDMA  (BOARD_DISPLAY_JD9853_SPI + BOARD_DISPLAY_GC9A01_SPI)

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

/* ---- Derived shared-bus flag ----------------------------------------------
 * True only where the microSD is a SECOND DEVICE on the display's SPI host (the C6-1.47:
 * BOARD_HAS_SD_SPI). That combination needs the cross-task display<->SD arbitration
 * (s_dispsd_mtx / DispBusGuard / sd_bus_lock sync-only handshake in the .ino). SPIDMA
 * boards WITHOUT a shared SD (S3-1.47, S3 super mini) skip all of it — their bus has
 * one device, so there is nothing to arbitrate and no reason to pay a mutex per flush. */
#define BOARD_SD_SHARES_LCD_BUS  (BOARD_DISPLAY_SPIDMA && BOARD_HAS_SD_SPI)

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

#endif /* BOARD_H_INCLUDED */
