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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
static inline void i2c_lock(void)   { if (s_i2c_mutex) xSemaphoreTake(s_i2c_mutex, portMAX_DELAY); }
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

/* Largest notification id we've already shown. Resets to 0 each power-on (full
 * AXP2101 shutdown doesn't preserve RAM), so the dedup is only within a single
 * power session — across a full off we may re-show the latest once, harmlessly. */
static uint64_t rtc_last_notif_id = 0;
