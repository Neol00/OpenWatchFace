/* ============================================================================
 *  watch_base.h — cross-cutting primitives shared by the data modules.
 *
 *  Header-only, compiled into the single .ino translation unit. INCLUDE FIRST of
 *  the data modules (after the hardware objects, before settings/power/stores),
 *  because everything below needs these locks/globals.
 *
 *  Holds the two FreeRTOS mutexes (and their lock helpers) plus the couple of
 *  RTC-session globals that several modules reference, so the modules don't have
 *  to depend on each other's definition order:
 *    - store_lock / i2c_lock: guard the shared notif/wifi store and the shared
 *      I2C bus (touch + RTC + PMU) across the UI core and the network core.
 *    - s_check_interval_min: notification-check cadence (settings + sleep arm).
 *    - rtc_last_notif_id: dedup of the largest notification id shown this session.
 *
 *  The mutex objects are created in setup(); the helpers are safe no-ops until
 *  then (single-threaded early boot).
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/freertos/FreeRTOS.h"
#include "tuya/compat/freertos/semphr.h"
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

/* Guards the shared notification/WiFi stores (UI core 1 dismiss/clear vs network
 * core 0 add/save). */
static SemaphoreHandle_t s_store_mutex = nullptr;
static inline void store_lock(void)   { if (s_store_mutex) xSemaphoreTake(s_store_mutex, portMAX_DELAY); }
static inline void store_unlock(void) { if (s_store_mutex) xSemaphoreGive(s_store_mutex); }

/* I2C BUS LOCK. The FT3168 touch chip, PCF85063 RTC and AXP2101 PMU all share one
 * I2C bus (SDA15/SCL14). The UI runs on core 1 (touch + battery + RTC reads) and
 * the network task runs on core 0 (RTC reads/writes during NTP sync). Two cores
 * driving the same bus with no lock interleave their transactions and fault the
 * I2C peripheral -> crash (black screen, watch alive in background). This bit only
 * over battery because NTP sync only fires on a fresh-wake WiFi reconnect, i.e.
 * exactly while you're touching the screen. EVERY shared-bus access takes this. */
static SemaphoreHandle_t s_i2c_mutex = nullptr;

/* DIAGNOSTIC (temporary): portMAX_DELAY makes a lock-ordering mistake look like a
 * dead device rather than a crash — the task blocks forever, the task watchdog
 * never fires (a blocked task yields, so it is not "starving" in the WDT's sense),
 * and NOTHING is printed. That is indistinguishable from a hung loop() and it is
 * exactly the state being debugged: setup() completes, the net task runs, loop()
 * produces no output ever, and the panel stays on whatever epd_begin() left.
 *
 * Time out instead of waiting forever, and SAY SO. If this fires, the message
 * names the caller that could not get the bus and the firmware keeps running
 * (degraded) instead of dying silently. If it never fires, I2C is exonerated and
 * the hang is elsewhere — which is equally useful to know.
 *
 * Revert to a plain portMAX_DELAY take once the hang is located. */
#ifndef OWF_I2C_LOCK_DEBUG
#define OWF_I2C_LOCK_DEBUG 1
#endif
/* NOTE: this MUST stay a plain function, not a macro. `i2c_lock` is FORWARD
 * DECLARED as `static inline void i2c_lock(void);` in sleep_track.h and
 * imu_steps.h, and the preprocessor reads that `(void)` as an argument list —
 * a function-like `#define i2c_lock()` then fails with "macro passed 1
 * arguments, but takes just 0" before the compiler ever sees the declaration.
 * Keeping the same signature also means no call site has to change. */
static inline void i2c_lock(void) {
  if (!s_i2c_mutex) return;
#if OWF_I2C_LOCK_DEBUG
  if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    /* Timed out. Say so and CONTINUE without the bus rather than blocking
     * forever: a silent permanent block is the failure being diagnosed. The
     * caller proceeds unlocked, which may garble one I2C transaction — an
     * acceptable trade for a device that stays alive and tells us what happened.
     * Deliberately no unlock bookkeeping: i2c_unlock()'s give is harmless on a
     * mutex this task does not hold (it just fails), and adding per-task state
     * here would be more machinery than a temporary diagnostic warrants. */
    USBSerial.println("[i2c] LOCK TIMEOUT — proceeding without the bus lock");
    USBSerial.flush();
  }
#else
  xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
#endif
}
static inline void i2c_unlock(void) { if (s_i2c_mutex) xSemaphoreGive(s_i2c_mutex); }

/* Notification check cadence (minutes), persisted in NVS. Read by settings_load()
 * and by the deep-sleep timer-arm; lives here so both see it without ordering
 * games. */
static uint16_t s_check_interval_min = 10;

/* Repaint the watchface bell badge from the current unread count. DEFINED in the
 * .ino (after watchface_set_bell / notif_unread); forward-declared here so the
 * menu/notification modules — included BEFORE those definitions — can ask for an
 * immediate refresh (e.g. when an item is opened, or on returning to the face)
 * instead of waiting for the loop's 20 s poll. */
static void watchface_refresh_bell(void);

/* Apply the "show voltage readout" setting (settings_get_show_volt) to the watch-
 * face corner label. DEFINED in watchface.h; forward-declared here so settings_store.h
 * — included BEFORE watchface.h — can call it from settings_set_show_volt() to update
 * the face live when the Appearance toggle flips. Null-safe before the face is built. */
static void watchface_apply_volt_visible(void);

/* Apply the "swap WiFi/BLE indicators" setting (settings_get_swap_wifi_ble) to the
 * watch face: choose whether WiFi or BLE occupies the right stat column vs. the
 * top-right tray. DEFINED in watchface.h; forward-declared here so settings_store.h
 * — included BEFORE watchface.h — can call it from settings_set_swap_wifi_ble() to
 * update the face live when the Appearance toggle flips. Null-safe before build. */
static void watchface_apply_indicator_layout(void);

/* Apply the "show weather widget" setting (settings_get_show_weather) to the watch
 * face: show/hide the under-dial weather icon+temp and shift the weekday/date row
 * down to make room (or back up when hidden). DEFINED in watchface.h; forward-declared
 * here so settings_store.h — included BEFORE watchface.h — can call it from
 * settings_set_show_weather() to update the face live. Null-safe before build. */
static void watchface_apply_weather_visible(void);

/* Refresh the watch-face weather widget from the current weather_store snapshot
 * (icon glyph + tint + temperature), in place. DEFINED in watchface.h; called from
 * the loop when a fetch lands and once at build. Null-safe before build / when the
 * widget is hidden. */
static void watchface_refresh_weather(void);

/* Ask the core-0 net task to (re)fetch weather. DEFINED in notif_net.h (included
 * LATE, after the app modules); forward-declared here so app_weather.h's "Refresh"
 * button — included before notif_net.h — can call it. No-op safe anytime. */
static void weather_request_fetch(void);

/* Largest notification id we've already shown. Resets to 0 each power-on (full
 * AXP2101 shutdown doesn't preserve RAM), so the dedup is only within a single
 * power session — across a full off we may re-show the latest once, harmlessly. */
static uint64_t rtc_last_notif_id = 0;
