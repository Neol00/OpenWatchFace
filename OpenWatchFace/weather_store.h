/* ============================================================================
 *  weather_store.h — weather model: location, current conditions, forecast,
 *                    NVS persistence, and the WMO-code -> icon/tint mapping.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE ORDER:
 *    - AFTER settings_store.h  (uses the shared `prefs` Preferences handle + NVS)
 *    - AFTER ui_fonts.h        (MDI_WX_* glyph codepoints)
 *    - AFTER watch_base.h      (store_lock/store_unlock — the net task on core 0
 *                               writes the snapshot, the UI on core 1 reads it)
 *    - BEFORE watchface.h and app_weather.h  (both render from this model)
 *    - BEFORE notif_net.h      (the WiFi fetch writes into this model)
 *
 *  DATA FLOW (per the design the user chose):
 *    - LOCATION is set once — either the compile-time preset below, or pushed
 *      from the phone over BLE (Gadgetbridge, ble_gadgetbridge.h) which only
 *      updates the location. Either way it PERSISTS to NVS, so the last-known
 *      location is assumed correct until changed again.
 *    - The actual WEATHER for that location is fetched from the internet over
 *      WiFi (open-meteo, no API key) in notif_net.h. The last snapshot persists
 *      to NVS so the face/app show something immediately at boot, before the
 *      first fetch of the session completes.
 * ========================================================================== */
#pragma once
#include <lvgl.h>
#include <string.h>
#include "ui_fonts.h"

/* ---- Compile-time PRESET location -----------------------------------------
 * ►► CHANGE THESE THREE LINES to set your default location. ◄◄
 * Set once in code; overridden at runtime by a location push from the phone
 * (Gadgetbridge weather, or the iOS companion app), which then persists to NVS.
 * If you never connect a phone, the watch fetches weather for THIS location.
 * lat/lon are decimal degrees (N and E positive; S and W negative).
 *
 * Default is Stockholm, Sweden — matching the NTP/timezone default. Look up your
 * own coordinates at e.g. latlong.net, or just set WEATHER_PRESET_NAME and let the
 * geocoder resolve it (the name is geocoded whenever coords aren't known). */
#ifndef WEATHER_PRESET_NAME
#define WEATHER_PRESET_NAME  "Stockholm"
#endif
#ifndef WEATHER_PRESET_LAT
#define WEATHER_PRESET_LAT   59.3293f
#endif
#ifndef WEATHER_PRESET_LON
#define WEATHER_PRESET_LON   18.0686f
#endif

#define WEATHER_LOC_MAX      32     // stored place-name length (chars incl. NUL)
#define WEATHER_FC_DAYS      10     // forecast days kept (open-meteo daily block; it
                                    // supports up to 16). Shown 5 per page in the app.
#define WEATHER_FC_PER_PAGE  5      // forecast rows per page (paged with < > arrows)

/* One forecast day: date parts + hi/lo (°C) + a WMO condition code. */
struct WeatherDay {
  int16_t hi_c;        // daily max °C (rounded); INT16_MIN = unknown
  int16_t lo_c;        // daily min °C
  uint8_t wmo;         // WMO weather-interpretation code (mapped to icons in wx_make_icon)
  uint8_t wday;        // 0=Sun..6=Sat (for the list/graph labels)
};

/* The whole weather snapshot. Small + POD, so cross-core sharing is just a
 * struct copy under store_lock(). Written by the net task (fetch) and by BLE;
 * read by the watch face + the Weather app on the UI core. */
struct WeatherModel {
  // Location (persisted).
  char    loc_name[WEATHER_LOC_MAX];
  float   lat, lon;
  bool    needs_geocode;    // name set (e.g. via BLE) but lat/lon not yet resolved

  // Current conditions.
  bool    have_current;     // a real fetch/BLE has populated `cur_*`
  int16_t cur_temp_c;       // current temperature °C
  int16_t cur_feels_c;      // apparent temperature °C (INT16_MIN = unknown)
  uint8_t cur_wmo;          // current WMO code
  uint8_t cur_humidity;     // % relative humidity (0xFF = unknown)
  uint8_t cur_wind_kmh;     // wind speed km/h (0xFF = unknown)
  bool    cur_is_day;       // true = daytime (picks sun vs moon glyph)

  // Forecast.
  uint8_t     fc_n;         // number of valid days in fc[] (0..WEATHER_FC_DAYS)
  WeatherDay  fc[WEATHER_FC_DAYS];

  uint32_t last_fetch_epoch; // when the current/forecast was last refreshed (RTC epoch, 0=never)
};

static WeatherModel s_wx;    // the one live model

/* NOTE: the WMO-code -> icon glyph + tint mapping lives in ONE place, wx_pick_glyph()
 * in app_weather.h (which both the in-app icon and the watch-face dial widget use, so
 * they match). wx_text_for_wmo() (below) is the parallel text label — one string per
 * condition. */

/* Short human label for a WMO code (for the current-conditions caption + list). */
static const char *wx_text_for_wmo(uint8_t wmo) {
  if (wmo == 0)                    return "Clear";
  if (wmo <= 2)                    return "Partly cloudy";
  if (wmo == 3)                    return "Overcast";
  if (wmo >= 45 && wmo <= 48)      return "Fog";
  if (wmo >= 51 && wmo <= 57)      return "Drizzle";
  if (wmo >= 61 && wmo <= 67)      return "Rain";
  if (wmo >= 71 && wmo <= 77)      return "Snow";
  if (wmo >= 80 && wmo <= 82)      return "Showers";
  if (wmo >= 85 && wmo <= 86)      return "Snow showers";
  if (wmo >= 95)                   return "Thunderstorm";
  return "Cloudy";
}

/* ---- NVS persistence -------------------------------------------------------
 * Location is the durable "set once" value. The last weather snapshot is also
 * persisted so the face shows real data at boot before the first fetch lands.
 * Uses the shared `prefs` handle (settings_store.h) in the "watch" namespace.
 * Keys are short (NVS key limit is 15 chars). */
static void weather_save_location(void) {
  prefs.putString("wxloc", s_wx.loc_name);
  prefs.putFloat ("wxlat", s_wx.lat);
  prefs.putFloat ("wxlon", s_wx.lon);
}

/* Persist the current+forecast snapshot as one small blob (POD, so a raw copy is
 * safe — no pointers inside). Cheap; called once per successful fetch. */
static void weather_save_snapshot(void) {
  prefs.putBytes("wxsnap", &s_wx, sizeof(s_wx));
}

/* Load location + last snapshot from NVS, falling back to the compile-time
 * preset. Call from settings_load() (or right after). Safe if nothing stored. */
static void weather_load(void) {
  // Seed with the preset first, so any missing key keeps a sane default.
  memset(&s_wx, 0, sizeof(s_wx));
  strncpy(s_wx.loc_name, WEATHER_PRESET_NAME, WEATHER_LOC_MAX - 1);
  s_wx.lat = WEATHER_PRESET_LAT;
  s_wx.lon = WEATHER_PRESET_LON;
  s_wx.cur_feels_c = INT16_MIN;
  s_wx.cur_humidity = 0xFF;
  s_wx.cur_wind_kmh = 0xFF;

  // A previously-saved snapshot restores current+forecast in one shot. If its
  // size doesn't match (struct changed across a firmware update) we ignore it and
  // keep the preset — the next fetch repopulates everything.
  size_t got = prefs.getBytesLength("wxsnap");
  if (got == sizeof(s_wx)) {
    WeatherModel tmp;
    if (prefs.getBytes("wxsnap", &tmp, sizeof(tmp)) == sizeof(tmp)) s_wx = tmp;
  }

  // LOCATION precedence — this is what makes the compile-time preset actually take
  // effect when you change it in code:
  //   - If the PHONE has set a location (wxphone == true), that is the durable value
  //     and wins over both the snapshot and the preset. It persists until the phone
  //     changes it again.
  //   - If the phone has NEVER set one, the COMPILE-TIME PRESET is authoritative:
  //     re-seed from it every boot, so editing WEATHER_PRESET_* + reflashing moves
  //     the watch (instead of being stuck on a stale coord baked into an old snapshot
  //     from a previous flash — the confusing "I changed the default but it kept the
  //     old city" trap).
  bool phone_set = prefs.getBool("wxphone", false);
  if (phone_set && prefs.isKey("wxloc")) {
    String n = prefs.getString("wxloc", s_wx.loc_name);
    strncpy(s_wx.loc_name, n.c_str(), WEATHER_LOC_MAX - 1);
    s_wx.loc_name[WEATHER_LOC_MAX - 1] = '\0';
    s_wx.lat = prefs.getFloat("wxlat", s_wx.lat);
    s_wx.lon = prefs.getFloat("wxlon", s_wx.lon);
    s_wx.needs_geocode = prefs.getBool("wxgeo", false);
  } else {
    // No phone location -> the preset (already seeded above) is the source of truth.
    strncpy(s_wx.loc_name, WEATHER_PRESET_NAME, WEATHER_LOC_MAX - 1);
    s_wx.loc_name[WEATHER_LOC_MAX - 1] = '\0';
    s_wx.lat = WEATHER_PRESET_LAT;
    s_wx.lon = WEATHER_PRESET_LON;
    s_wx.needs_geocode = false;             // preset carries real coords
  }
}

/* Set a new location (from BLE or code) and persist it. Marks the current data
 * stale (have_current stays, so the app still shows the OLD reading until the
 * next fetch replaces it, rather than blanking). Returns true if it actually
 * changed (caller triggers a fetch). Thread: called on core 0 (BLE task); the
 * copy is under store_lock so the UI never reads a half-written name. */
static bool weather_set_location(const char *name, float lat, float lon) {
  bool changed = (lat != s_wx.lat) || (lon != s_wx.lon) ||
                 (name && strncmp(name, s_wx.loc_name, WEATHER_LOC_MAX) != 0);
  if (!changed) return false;
  store_lock();
  if (name && *name) {
    strncpy(s_wx.loc_name, name, WEATHER_LOC_MAX - 1);
    s_wx.loc_name[WEATHER_LOC_MAX - 1] = '\0';
  }
  s_wx.lat = lat;
  s_wx.lon = lon;
  s_wx.needs_geocode = false;
  store_unlock();
  weather_save_location();
  prefs.putBool("wxphone", true);           // a phone location now overrides the preset
  return true;
}

/* Set the location by NAME only (from a BLE push, which carries no coordinates).
 * Arms needs_geocode so the next WiFi fetch resolves the name to lat/lon first.
 * Persists the name immediately (the durable "set once" value). Returns true if
 * the name actually changed (so the caller only re-geocodes on a real change).
 * Thread: called on core 0 (BLE task); the write is under store_lock. */
static bool weather_set_name_for_geocode(const char *name) {
  if (!name || !*name) return false;
  if (strncmp(name, s_wx.loc_name, WEATHER_LOC_MAX) == 0) return false;  // same place
  store_lock();
  strncpy(s_wx.loc_name, name, WEATHER_LOC_MAX - 1);
  s_wx.loc_name[WEATHER_LOC_MAX - 1] = '\0';
  s_wx.needs_geocode = true;
  store_unlock();
  prefs.putString("wxloc", s_wx.loc_name);
  prefs.putBool  ("wxgeo", true);          // survive a reboot before the geocode lands
  prefs.putBool  ("wxphone", true);        // a phone location now overrides the preset
  return true;
}

/* Record resolved coordinates for the current name (called by the fetch after a
 * successful geocode). Clears needs_geocode and persists lat/lon. Core 0. */
static void weather_set_resolved_coords(float lat, float lon) {
  store_lock();
  s_wx.lat = lat; s_wx.lon = lon; s_wx.needs_geocode = false;
  store_unlock();
  prefs.putFloat("wxlat", lat);
  prefs.putFloat("wxlon", lon);
  prefs.putBool ("wxgeo", false);
}

/* Snapshot the model for the UI core (small struct copy under the shared lock).
 * The face/app call this once per refresh and read from their own copy, so they
 * never tear a multi-field read against the net task's write. */
static void weather_get(WeatherModel *out) {
  store_lock();
  *out = s_wx;
  store_unlock();
}
