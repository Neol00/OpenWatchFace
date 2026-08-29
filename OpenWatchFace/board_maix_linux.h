/* ============================================================================
 *  board_maix_linux.h — Sipeed MaixCam-Pro running Linux (native MaixCDK app)
 *
 *  This is NOT an MCU board. The firmware runs as a native C++ binary on top of
 *  Linux, using MaixCDK for the display (maix::display) and touch
 *  (maix::touchscreen). MaixCDK's bundled LVGL glue (maix::lvgl_init) owns the
 *  LVGL display/indev/tick, so the firmware's own Arduino_GFX panel bring-up and
 *  lv_display_create are gated OUT here (see BOARD_PLATFORM_MAIX use in the .ino).
 *
 *  Everything hardware-specific is OFF: the firmware's existing BOARD_HAS_*
 *  feature-flag stubs then compile the PMU/RTC/IMU/audio/SD/deep-sleep modules
 *  down to no-ops. WiFi/BLE remain stubbed for now (compat/ headers) — they come
 *  back later as Linux sockets / BlueZ.
 *
 *  See maixcam-port/README.md for the porting plan.
 * ========================================================================== */
#pragma once

#define BOARD_NAME   "Sipeed MaixCam-Pro (Linux)"
#define BOARD_VENDOR "Sipeed"

/* ---- Over-the-air update identity ----------------------------------------
 * The key this board looks for in ota/latest.json, and the name its firmware
 * is published under in a GitHub release:
 *
 *     owf-maixcam-pro-<version>.bin
 *
 * It lives HERE, next to the rest of the board's identity, rather than in a
 * per-board ladder inside the updater — adding a board should mean editing one
 * file. It must match the key in the manifest exactly; a mismatch is reported
 * by the update check as "No build for maixcam-pro", which names the fix. */
#define BOARD_OTA_KEY "maixcam-pro"

/* ---- Platform flag: gate ESP/Arduino runtime assumptions ------------------
 * Used in the .ino + board layer to skip MCU-only bring-up (Arduino_GFX panel,
 * the firmware's own lv_init/lv_display_create/indev, deep sleep, ULP, etc.). */
#define BOARD_PLATFORM_MAIX 1

/* ---- Display / touch come from MaixCDK, not an on-board Arduino driver ----- */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_DISPLAY_JD9853_SPI  0
#define BOARD_DISPLAY_MAIX        1   /* external: maix::display::Display */
#define BOARD_TOUCH_FT3168        0
#define BOARD_TOUCH_AXS5106L      0
#define BOARD_TOUCH_MAIX          1   /* external: maix::touchscreen::TouchScreen */

/* ---- Capabilities: all hardware subsystems OFF (stubbed by existing flags) -- */
#define BOARD_HAS_PSRAM           0   /* plain malloc; lv_psram_alloc.cpp self-disables */
#define BOARD_DUAL_CORE           0
#define BOARD_HAS_PMU_AXP2101     0
#define BOARD_HAS_ADC_BATTERY     0   /* no battery sense yet (read from sysfs later if wanted) */
#define BOARD_HAS_RTC_PCF85063    0   /* use the Linux system clock */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0
#define BOARD_HAS_HAPTICS         0
#define BOARD_HAS_SD_MMC          0
#define BOARD_HAS_SD_SPI          0   /* persistence goes to the Linux filesystem */
#define BOARD_HAS_IMU_QMI8658     0
#define BOARD_HAS_BACKLIGHT_PWM   0   /* brightness via maix::display::set_backlight() */
#define BOARD_HAS_LP_STEPS        0
#define BOARD_WAKE_USE_EXT0       0
#define BOARD_HAS_BLE             0   /* NimBLE clients deferred → BlueZ later (ble_compat_stubs.h) */

/* BLE TX-power ladder. BLE is off here, but settings_store.h references these
 * unconditionally to build its radio-power tables — provide a plausible 7-tier
 * ladder (values are the esp_bt.h stub enums; never actually applied). */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9, \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -15, -12, -9, -6, 0, 9, 20 }

/* Hardware summary for Settings > About. */
#define BOARD_HW_SUMMARY \
  "Host:    Sipeed MaixCam-Pro (Linux)\n" \
  "Display: MaixCDK (maix::display)\n" \
  "Touch:   MaixCDK (maix::touchscreen)\n" \
  "Storage: Linux filesystem"

/* ---- Screen geometry ------------------------------------------------------
 * MaixCam-Pro default panel is 640x480. The watch UI is portrait-native; running
 * it on a landscape panel (and any 90-deg rotation) is a layout concern handled
 * after first light. The firmware reads the live size from the display at runtime;
 * these macros drive the static layout decisions (BOARD_SCREEN_PORTRAIT, UI_PX). */
#define LCD_WIDTH  640
#define LCD_HEIGHT 480
#define BOARD_LCD_EVEN_ALIGN 0

/* Unused on this platform (no firmware-owned partial render buffers — MaixCDK's
 * LVGL glue manages its own), but the .ino references it before the platform
 * gate; give it a sane value. */
#define BOARD_PARTIAL_BUF_LINES 64

/* ---- Dummy pin / bus constants -------------------------------------------
 * There are no MCU pins on Linux, but the firmware references these in a few
 * spots that aren't worth gating (the GPIO/I2C calls are no-ops via the compat
 * shim). digitalRead() returns HIGH so BOOT reads "released". */
#define IIC_SDA          0
#define IIC_SCL          0
#define TP_INT           0
#define TP_RESET         0
#define BOOT_BTN_GPIO    0
#define BOARD_WAKE_GPIO  0
#define BOARD_LCD_BUS_HZ 40000000
