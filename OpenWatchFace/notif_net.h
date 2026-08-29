/* ============================================================================
 *  notif_net.h — async notification networking (WiFi + HTTP GET + NTP), core 0.
 *
 *  WiFi connect + HTTP GET + NTP sync BLOCK for up to several seconds, which would
 *  freeze the UI if run on the loop (core 1). So a dedicated task pinned to CORE 0
 *  does the slow work: the loop RAISES s_net_request and keeps running; the task
 *  fetches, writes results into the notif store (under store_lock), and sets result
 *  flags the loop reads to pop the card / refresh the bell. The task NEVER touches
 *  LVGL — all UI happens back on the loop side.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER the data modules it uses:
 *    - settings_store.h   (s_wifi_enabled), power_model.h (s_wifi_active)
 *    - wifi_store.h       (s_wifi_nets / WifiNet / WIFI_NET_MAX)
 *    - notif_store.h      (notif_store_add/save, NOTIF_TITLE_MAX)
 *    - notif_archive_sd.h (na_available / na_append — SD full history)
 *    - watch_base.h       (store_lock / i2c_lock / rtc_last_notif_id)
 *  and AFTER the .ino's hardware objects (rtc, USBSerial) + the config macros at
 *  the top of the .ino (WIFI_*, NTP_*, NOTIFY_*). The deep-sleep light-check path
 *  (background_check_has_new in the .ino) calls notif_fetch_raw, so this must be
 *  included before that — it is (this sits with the data modules, above setup()).
 * ========================================================================== */
#pragma once
#include <WiFi.h>
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/WiFiClientSecure.h"   // CA holder; TLS happens inside the Tuya HTTP GET
#include "tuya/compat/HTTPClient.h"         // ESP32 HTTPClient surface over http_client_request
#include "tuya/compat/owf_tuya_sntp.h"      // real UDP SNTP (the core ships no SNTP service)
#else
#include <WiFiClientSecure.h>   // TLS client for HTTPS to the notify-server
#include <HTTPClient.h>
#endif
#include "notify_ca.h"          // pinned Let's Encrypt root (validates the server cert)
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/esp_heap_caps.h"
#else
#include "esp_heap_caps.h"      // heap_caps_get_free_size — WiFi-vs-BLE coexistence guard below
#endif

/* WiFi+BLE coexistence SRAM floor. Bringing the WiFi driver up (WiFi.mode(WIFI_STA)
 * + WiFi.begin) needs some INTERNAL SRAM the BT controller can't share. When BLE is
 * already up and internal SRAM is too tight, the WiFi init fails AND the ESP-IDF
 * WiFi/NVS teardown then leaks a little internal RAM per attempt (the "Failed to
 * deinit Wi-Fi 0x3001" / "init nvs failed ret=101" in the logs — an upstream IDF
 * bug, not ours). The killer is that wifi_connect() runs on a SCHEDULE, so a single
 * user "enable WiFi" turns into an unbounded retry storm that bleeds the leak until
 * an ISR alloc asserts and the watch crashes.
 *
 * So we REFUSE to bring WiFi up when free internal SRAM is below this floor AND BLE
 * is holding memory — the mirror of ble_begin()'s BLE_MIN_FREE_INTERNAL guard. The
 * scheduled retries then become cheap no-ops instead of a crash spiral.
 *
 * HOW MUCH WiFi ACTUALLY NEEDS IS PER-BOARD, so this is a board-overridable default
 * keyed on PSRAM, NOT one constant:
 *   - PSRAM boards (S3-2.06/1.8/1.47): the custom libs route WiFi + BLE buffers
 *     mostly to PSRAM, so WiFi's INTERNAL-SRAM need is small (~10 KB measured on the
 *     S3). A big floor here would wrongly block a coexistence that fits fine, so the
 *     floor is low — just a guard against a genuinely starved heap.
 *   - No-PSRAM boards (C6): WiFi AND BLE buffers both come from internal SRAM, so the
 *     two together are genuinely tight and the floor must be higher.
 * A board header can #define WIFI_MIN_FREE_INTERNAL before this to pin an exact value
 * (measure with the [ble]/[wifi] free-SRAM breadcrumbs if you tune it). */
#ifndef WIFI_MIN_FREE_INTERNAL
#if BOARD_HAS_PSRAM
#define WIFI_MIN_FREE_INTERNAL (16 * 1024)   /* WiFi ~10 KB internal here; small margin */
#else
#define WIFI_MIN_FREE_INTERNAL (40 * 1024)   /* no PSRAM: WiFi+BLE both in internal SRAM */
#endif
#endif

/* ---- core-0 task handshake flags (loop <-> net task) ---- */
static TaskHandle_t   s_net_task        = nullptr;
static volatile bool  s_net_request     = false;     // loop -> task: please fetch
static volatile bool  s_net_busy        = false;     // task is currently working
static volatile int   s_net_new_count   = 0;         // task -> loop: # new this fetch
static volatile bool  s_net_result_ready= false;     // task -> loop: result to consume
static volatile bool  s_weather_request = false;     // loop/BLE -> task: please fetch weather
static volatile bool  s_weather_ready   = false;     // task -> loop: new weather to render

/* Minimal JSON string-field extractor for our known, simple server payload:
 *   {"items":[{"id":..,"app":"..","title":"..","body":"..","ts":..}]}
 * Finds the FIRST occurrence of "key":"value" after `from` and copies value.
 * Not a general JSON parser — adequate for this fixed schema, avoids a lib. */
static bool json_find_string(const String &src, int from, const char *key,
                             String &out) {
  String pat = String("\"") + key + "\":\"";
  int k = src.indexOf(pat, from);
  if (k < 0) return false;
  int vstart = k + pat.length();
  // find closing quote, honoring backslash escapes
  int i = vstart;
  out = "";
  while (i < (int)src.length()) {
    char c = src[i];
    if (c == '\\' && i + 1 < (int)src.length()) {  // simple unescape
      char n = src[i + 1];
      if (n == 'n') out += '\n';
      else out += n;
      i += 2;
      continue;
    }
    if (c == '"') break;
    out += c;
    i++;
  }
  return true;
}

/* Bring up WiFi (STA). Returns true if connected within the timeout. Runs on the
 * network task (core 0), so its blocking is invisible to the UI. */
static bool wifi_connect(void) {
  if (!s_wifi_enabled) return false;     // never bring up the radio if wifi is disabled
  if (WiFi.status() == WL_CONNECTED) return true;

  // WiFi+BLE coexistence guard (see WIFI_MIN_FREE_INTERNAL above). If BLE is up and
  // internal SRAM is below the floor, bringing WiFi up would fail-and-leak, and since
  // this runs on a schedule that leak would repeat until the watch crashes. Refuse
  // the bring-up instead: a scheduled no-op, no radio touched, nothing leaked. This
  // does NOT drop an already-connected WiFi (handled by the WL_CONNECTED check above)
  // — it only blocks a NEW init while BLE holds the memory. Turning BLE off (or the
  // next reboot) frees the SRAM and WiFi comes back on its own. Rate-limit the log so
  // the ~scheduled retries don't spam the console.
  // The coexistence guard + refusal toast only exist on the real-BLE backends
  // (BOARD_HAS_BLE -> ble_provision.h, or the Tuya T5 -> tuya/ble_tuya.h). Both define
  // ble_is_up() and the BleToast enum/s_ble_toast we use here. The Maix build stubs
  // WiFi/BLE out entirely (no ble_is_up, no toast system), so this whole block would
  // fail to compile there — gate it out. On Maix there is nothing to guard anyway.
#if BOARD_HAS_BLE || BOARD_PLATFORM_TUYA
  {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < WIFI_MIN_FREE_INTERNAL) {
      // Refuse + advise. If BLE is the thing holding the SRAM, tell the user to turn
      // it off (that frees it and WiFi comes back on its own). Otherwise the heap is
      // just too full/fragmented, so a reboot is the fix. The toast is set here (core
      // 0 net task) and rendered by ble_ui_tick() on the loop — same one-shot flag
      // pattern the other toasts use; a volatile enum write is safe cross-core.
      bool ble_up = ble_is_up();
      // Rate-limit the log + toast so the ~scheduled retries don't spam. First refusal
      // always fires (s_last starts unset); thereafter at most once per 5 s.
      static bool     s_wifi_refused_once = false;
      static uint32_t s_last_wifi_refused_ms = 0;
      uint32_t now = millis();
      if (!s_wifi_refused_once || (uint32_t)(now - s_last_wifi_refused_ms) > 5000) {
        s_wifi_refused_once = true;
        s_last_wifi_refused_ms = now;
        USBSerial.printf("[wifi] REFUSED: %u KB internal free < %u KB (%s). WiFi bring-up "
                         "now would fail+leak.\n",
                         (unsigned)(free_int / 1024), (unsigned)(WIFI_MIN_FREE_INTERNAL / 1024),
                         ble_up ? "BLE is up" : "heap too full");
        s_ble_toast = ble_up ? BLE_TOAST_WIFI_OOM_BLE : BLE_TOAST_WIFI_OOM_BOOT;
      }
      return false;
    }
  }
#endif

  // Snapshot the saved-network list under the store mutex, then connect from the
  // copy. This runs on core 0 and each attempt can block for seconds, so we must
  // NOT hold the lock across the connect loop (the UI dismiss/forget runs on core
  // 1) — and we must NOT iterate s_wifi_nets directly while the UI may be
  // compacting it (forget). The snapshot is small (5 * ~96B).
  WifiNet nets[WIFI_NET_MAX];
  uint8_t count;
  store_lock();
  count = s_wifi_net_count;
  memcpy(nets, s_wifi_nets, sizeof(WifiNet) * count);
  store_unlock();
  if (count == 0) return false;          // no saved networks

  WiFi.mode(WIFI_STA);
  // Try each saved network in turn until one connects within the per-network
  // timeout (insertion order; the seeded home network is first).
  for (uint8_t i = 0; i < count; i++) {
#if BOARD_PLATFORM_TUYA
    // The T5 WiFi connect is ASYNCHRONOUS (tkl_wifi_station_connect returns at once;
    // status walks IDLE->CONNECTING->GOT_IP, or to a terminal fail). A SINGLE begin()
    // misses the AP intermittently (the "connects half the time, needs a reboot" bug).
    // So retry a few times per network: disconnect+settle, begin, poll until GOT_IP or
    // a TERMINAL fail (don't burn the whole 8s sitting on a known failure), then retry.
    bool ok = false;
    for (uint8_t attempt = 0; attempt < 3 && !ok; attempt++) {
      WiFi.disconnect(false);
      delay(150);                                    // let the driver settle between tries
      WiFi.begin(nets[i].ssid, nets[i].pass);
      uint32_t start = millis();
      while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        WF_STATION_STAT_E st = WiFi.status();
        if (st == WSS_GOT_IP) { ok = true; break; }
        // Terminal failures: stop waiting and retry immediately (or move on).
        if (st == WSS_CONN_FAIL || st == WSS_PASSWD_WRONG ||
            st == WSS_NO_AP_FOUND || st == WSS_DHCP_FAIL) break;
        delay(100);
      }
    }
    if (ok) {
      settings_apply_wifi_txp();   // driver restarts per fetch and resets TX power
      return true;
    }
#else
    WiFi.begin(nets[i].ssid, nets[i].pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) break;
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      settings_apply_wifi_txp();   // driver restarts per fetch and resets TX power
      return true;
    }
    WiFi.disconnect(false);
#endif
  }
  return false;
}

/* Sync the PCF85063 RTC from NTP, but only if it's been > NTP_SYNC_INTERVAL_S
 * since the last successful sync (tracked in RTC RAM across deep-sleep wakes).
 * Call this right after a successful WiFi connect. Best-effort: on any failure
 * it leaves the RTC untouched and returns false. Applies NTP_TZ so the RTC ends
 * up holding LOCAL wall-clock time (with DST), matching how the face reads it.
 *
 * `force` bypasses the interval gate (used on cold boot for an immediate sync). */
static bool ntp_sync_if_due(bool force) {
  // PRIMARY rate-limit: monotonic millis() since the last SUCCESSFUL sync. This does NOT
  // depend on the RTC readback or on epoch bases matching, so it can't be defeated the way
  // the epoch gate below can. On the T5 the epoch gate was failing to block (the RTC-readback
  // comparison didn't hold), so ntp_sync_if_due() re-ran on every ~15 s notification fetch —
  // each one a blocking DNS + UDP round-trip that stalled the UI. This floor guarantees at
  // most one sync per NTP_SYNC_INTERVAL_S within a wake session regardless of RTC state.
  // (millis() wraps ~every 49 days; the unsigned subtraction handles the wrap.)
  static uint32_t s_last_sync_ms = 0;
  static bool     s_have_synced  = false;
  if (!force && s_have_synced &&
      (uint32_t)(millis() - s_last_sync_ms) < NTP_SYNC_INTERVAL_S * 1000UL)
    return false;

  // SECONDARY gate on the interval unless forced. Compute the RTC's current epoch (treating
  // its stored fields as local time) and compare to the last-sync epoch. If the
  // RTC is unset/garbage, now_rtc will be small/0 and we'll sync anyway.
  if (!force && rtc_last_ntp_epoch != 0) {
    i2c_lock();   // shared bus: this runs on core 0, the UI reads touch on core 1
    RTC_DateTime c = board_clock_now();
    i2c_unlock();
    struct tm t = {};
    t.tm_year = c.getYear() - 1900; t.tm_mon = c.getMonth() - 1; t.tm_mday = c.getDay();
    t.tm_hour = c.getHour(); t.tm_min = c.getMinute(); t.tm_sec = c.getSecond();
    t.tm_isdst = -1;
    time_t now_rtc = mktime(&t);
    if (now_rtc > 0 && (uint32_t)now_rtc - rtc_last_ntp_epoch < NTP_SYNC_INTERVAL_S)
      return false;                       // not due yet
  }
  if (WiFi.status() != WL_CONNECTED) return false;

  // Apply our timezone so localtime()/getLocalTime() convert the (UTC) system clock
  // to local wall-clock time with DST. On the ESP path configTzTime ALSO starts the
  // built-in SNTP service; on the Tuya T5 it only sets TZ (no SNTP in the core), so we
  // run our own UDP SNTP client below to actually fetch network time.
  configTzTime(NTP_TZ, NTP_SERVER1, NTP_SERVER2);

  struct tm tmv = {};

#if BOARD_PLATFORM_TUYA
#if OWF_TUYA_SNTP_ENABLE
  // Real SNTP over UDP (the core has no SNTP). It returns the fetched UTC epoch directly;
  // we convert THAT with localtime_r (TZ applied above) instead of reading the system clock
  // back via getLocalTime() - the readback on this board is corrupted (gave year 3855160),
  // so we never trust it. localtime_r applies the POSIX TZ to give local wall-clock time.
  uint32_t utc_epoch = 0;
  if (!owf_tuya_sntp_sync(&utc_epoch)) return false;
  time_t et = (time_t)utc_epoch;
  localtime_r(&et, &tmv);
  if (tmv.tm_year + 1900 < 2024) return false;      // sanity
#else
  // SNTP disabled (OWF_TUYA_SNTP_ENABLE=0): rely on the battery-backed RTC / phone sync.
  return false;
#endif
#else  /* non-Tuya: the platform SNTP set the system clock; read it back via getLocalTime */
  uint32_t start = millis();
  while (millis() - start < 5000) {       // up to 5s for a response
    if (getLocalTime(&tmv, 200) && tmv.tm_year + 1900 >= 2024) break;
    delay(50);
  }
  if (tmv.tm_year + 1900 < 2024) return false;  // no valid time received
#endif

  // Write the local wall-clock time into the RTC. (On a board with no RTC chip,
  // configTzTime above already set the internal clock — this is a harmless
  // re-stamp through the same board API.)
  i2c_lock();   // shared bus (core 0)
  board_clock_set(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  i2c_unlock();
  rtc_last_ntp_epoch = (uint32_t)mktime(&tmv);  // mktime here = local epoch
  s_last_sync_ms = millis();                    // arm the monotonic rate-limit (see top)
  s_have_synced  = true;
  USBSerial.printf("[ntp] synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return true;
}

/* Newest accepted notification, kept for the popup card so it works regardless of
 * WHERE the item was stored (SD archive when a card is present, flash otherwise).
 * The body here is only a short preview — the full text lives in the store and is
 * read in the Notifications app. Written under store_lock in the fetch loop. */
static uint64_t s_pop_id = 0;
static char     s_pop_title[NOTIF_TITLE_MAX];
static char     s_pop_body[96];      // glance preview only — sized to fit the popup card
static bool     s_pop_have = false;

/* Network-only fetch (NO LVGL/display use — safe in the light-check path).
 * GETs the server (which DRAINS its queue), parses the items, and writes the
 * most-recent title/body into the out params. Returns the number of items, and
 * sets maxId to the largest notification id seen. */
static int notif_fetch_raw(String &outTitle, String &outBody, uint64_t &maxId) {
  outTitle = ""; outBody = ""; maxId = 0;
  s_wifi_active = 1;                      // radio up -> reflect in power estimate
  if (!wifi_connect()) { s_wifi_active = 0; return 0; }

  // While we have the radio up anyway, keep the RTC synced (rate-limited inside).
  ntp_sync_if_due(false);

  // HTTPS to the notify-server. WiFiClientSecure validates the server's certificate
  // against our pinned Let's Encrypt root (NOTIFY_ROOT_CA) — so the Bearer token and
  // the notification bodies are encrypted in transit AND we're sure we're talking to
  // the real server (not a man-in-the-middle), on any network. NOTIFY_URL must be an
  // https:// URL (Tailscale Funnel hostname or a reverse-proxy domain). The TLS
  // handshake checks the cert's validity dates, so the RTC must hold ~correct time —
  // ntp_sync_if_due() above runs first, and the RTC is battery-backed, so it does.
  WiFiClientSecure client;
  client.setCACert(NOTIFY_ROOT_CA);
  HTTPClient http;
  if (!http.begin(client, NOTIFY_URL)) { s_wifi_active = 0; return 0; }
  http.setConnectTimeout(5000);          // TLS handshake needs a bit more headroom than plain HTTP
  http.setTimeout(5000);
  http.addHeader("Authorization", "Bearer " NOTIFY_TOKEN);
  int code = http.GET();
  int added = 0;     // how many NEW items we accepted into the store
  if (code == 200) {
    String payload = http.getString();
    int idx = 0;
    // Server returns items oldest-first; pushing each to the front of our store
    // leaves the last-pushed (newest) at index 0. notif_store_add() de-dups by id
    // and rolls (evicts the oldest) when full; the save below is batched once per
    // GET, so a flood stays bounded to one NVS write per fetch.
    bool changed = false;
    store_lock();                     // the store is shared with the UI (loop)
    while (true) {
      int it = payload.indexOf("\"id\":", idx);
      if (it < 0) break;
      uint64_t id = strtoull(payload.c_str() + it + 5, nullptr, 10);
      if (id > maxId) maxId = id;
      String t, b, ap;
      json_find_string(payload, it, "title", t);
      json_find_string(payload, it, "body", b);
      json_find_string(payload, it, "app", ap);
      if (t.isEmpty()) t = "Notification";
      // Derive the per-app icon category from the server's "app" field. It may be a
      // bundle id ("com.apple.MobileSMS") or a friendly name ("Messages") — the
      // substring matcher handles both ("message", "mail", "call", ...).
      uint8_t cat = notif_cat_from_appid(ap.c_str());
      // Storage policy (see notif_archive_sd.h header). The ARCHIVE now always
      // exists: it's the SD card when one is mounted, else the on-flash FAT
      // partition (~16 MB), so the full unlimited history works WITH OR WITHOUT a
      // card. We append EVERY item to that archive AND mirror it into the NVS
      // newest-32 cache (notif_store.h): the cache powers the instant wake/popup
      // path with zero mount, the archive is the source of truth the app/bell read.
      // Only if BOTH the card and FFat fail to mount do we degrade to flash-only.
      bool accepted;
      if (na_available()) {
        accepted = na_append(id, t.c_str(), b.c_str(), cat);       // SD or FFat: full history
        // Mirror into the NVS cache too. It de-dups by id and drops when full
        // (the newest 32), so this is a no-op past the cap — exactly the cache we want.
        if (notif_store_add(id, t.c_str(), b.c_str(), cat)) changed = true;
      } else {
        accepted = notif_store_add(id, t.c_str(), b.c_str(), cat); // last-resort flash only
        if (accepted) changed = true;
      }
      if (accepted) {
        added++;
        outTitle = t;   // newest accepted, for the popup card
        outBody  = b;
        // Stash the newest for the popup, independent of the store used.
        s_pop_id = id;
        strncpy(s_pop_title, t.c_str(), sizeof(s_pop_title) - 1);
        s_pop_title[sizeof(s_pop_title) - 1] = '\0';
        strncpy(s_pop_body, b.c_str(), sizeof(s_pop_body) - 1);
        s_pop_body[sizeof(s_pop_body) - 1] = '\0';
        s_pop_have = true;
      }
      idx = it + 5;
    }
    if (changed) notif_store_save();  // one write for the whole batch
    store_unlock();
  }
  http.end();
  s_wifi_active = 0;                  // radio work done
  return added;
}

/* ===========================================================================
 *  WEATHER fetch (open-meteo, no API key) — runs on the SAME core-0 net task.
 *  ---------------------------------------------------------------------------
 *  The phone (BLE) only sets the LOCATION; the actual weather for that lat/lon
 *  is fetched here over WiFi and written into s_wx (weather_store.h). It shares
 *  wifi_connect() with the notification fetch, so the WiFi+BLE coexistence guard
 *  above protects it too. Plain HTTP (open-meteo serves it) — no TLS cert, no
 *  pinned CA, and nothing secret in the request, so the handshake cost is saved.
 * ========================================================================= */

/* Extract a JSON NUMBER field ("key":<num>) as a double. Companion to
 * json_find_string (which is string-values only). Returns false if absent.
 * `from` lets the caller scope the search (e.g. inside the "current" object). */
static bool json_find_number(const String &src, int from, const char *key, double &out) {
  String pat = String("\"") + key + "\":";
  int k = src.indexOf(pat, from);
  if (k < 0) return false;
  int vstart = k + pat.length();
  while (vstart < (int)src.length() && (src[vstart] == ' ' || src[vstart] == '\t')) vstart++;
  const char *p = src.c_str() + vstart;
  char *end = nullptr;
  double v = strtod(p, &end);
  if (end == p) return false;    // "key":null / non-numeric
  out = v;
  return true;
}

/* Copy ONE JSON array's first N numbers (e.g. "temperature_2m_max":[..]) into
 * out[]. Returns how many were parsed (<= maxn). Used for the daily forecast
 * arrays. Tolerant of nulls (skips to the next value). */
static int json_find_num_array(const String &src, const char *key, double *out, int maxn) {
  String pat = String("\"") + key + "\":[";
  int k = src.indexOf(pat);
  if (k < 0) return 0;
  int i = k + pat.length();
  int n = 0;
  while (n < maxn && i < (int)src.length()) {
    while (i < (int)src.length() && (src[i] == ' ' || src[i] == ',')) i++;
    if (i >= (int)src.length() || src[i] == ']') break;
    const char *p = src.c_str() + i;
    char *end = nullptr;
    double v = strtod(p, &end);
    if (end == p) {                        // null or a quoted date string -> skip to next comma/]
      while (i < (int)src.length() && src[i] != ',' && src[i] != ']') i++;
      continue;
    }
    out[n++] = v;
    i += (int)(end - p);
  }
  return n;
}

/* Round a double to a clamped int16 (°C). Guards NaN/insane values from a bad
 * parse so the UI never shows garbage temperatures. */
static int16_t wx_round_c(double v) {
  if (!(v > -120.0 && v < 120.0)) return INT16_MIN;   // also catches NaN
  return (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
}

/* URL scheme per platform: plain http on ESP32 (no TLS to hang), https on Tuya
 * (its HTTPClient compat is port-443-only). See wx_http_get() for the rationale. */
#if BOARD_PLATFORM_TUYA
#define WX_SCHEME "https://"
#else
#define WX_SCHEME "http://"
#endif

/* Build the open-meteo request URL for the stored lat/lon into `buf`. Asks for
 * the current block + a daily block (max/min temp + weather code + weekday index
 * is derived from the returned dates). temperature in °C, wind in km/h. */
static void weather_build_url(char *buf, size_t n, float lat, float lon) {
  snprintf(buf, n,
    WX_SCHEME "api.open-meteo.com/v1/forecast"
    "?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
    "weather_code,wind_speed_10m,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min"
    "&timezone=auto&forecast_days=%d",
    (double)lat, (double)lon, WEATHER_FC_DAYS);
}

/* GET a (keyless, public) open-meteo endpoint -> `out` body. Returns true on HTTP 200.
 *
 * TRANSPORT DIFFERS BY PLATFORM, and this is deliberate:
 *   - ESP32 core: PLAIN HTTP (a bare WiFiClient). open-meteo serves http:// directly,
 *     and using it here SIDESTEPS TLS entirely — no handshake to stall (the "refresh
 *     hangs for a minute" symptom), no 16 KB TLS RX buffer to size for a big CDN cert
 *     chain, no cert-date dependency on the RTC. Nothing secret is sent, so there is
 *     no security cost. This is why the notification path (a private server) pins a CA
 *     but weather does not need to.
 *   - Tuya compat (tuya/compat/HTTPClient.h): it is HTTPS-only (hardcoded port 443,
 *     unverified when no CA is set) and has no begin(WiFiClient&,...) overload, so the
 *     Tuya build uses https:// + the single-arg begin(). weather_build_url / the
 *     geocode URL are built per-platform to match (http on ESP32, https on Tuya).
 * Assumes WiFi is already up. The client is static so its buffers live off the 8 KB
 * net-task stack; the net task is the only, serialized caller so this is safe. */
static bool wx_http_get(const char *url, String &out) {
  HTTPClient http;
  bool started;
#if BOARD_PLATFORM_TUYA
  started = http.begin(String(url));                 // https, unverified TLS on the Tuya layer
#else
  static WiFiClient client;                           // PLAIN http — no TLS, no handshake to hang
  started = http.begin(client, url);
#endif
  if (!started) { USBSerial.println("[wx] http begin() failed"); return false; }
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
#if !BOARD_PLATFORM_TUYA
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // in case the http endpoint 3xx-redirects
#endif
  int code = http.GET();
  bool ok = (code == 200);
  if (ok) out = http.getString();
  else    USBSerial.printf("[wx] GET -> %d\n", code);
  http.end();
  return ok;
}

/* Resolve the stored place NAME to lat/lon via open-meteo's geocoding API (keyless).
 * Used when a BLE push set a name but no coordinates. Returns true and records the
 * coords on success; on failure the old coords stay (so weather still works for the
 * previous place). Assumes WiFi is already up (called from weather_fetch_raw). */
static bool weather_geocode(void) {
  char name[WEATHER_LOC_MAX];
  store_lock(); strncpy(name, s_wx.loc_name, sizeof(name)); store_unlock();
  name[sizeof(name) - 1] = '\0';
  if (!*name) return false;

  // URL-encode the name (spaces -> %20; keep it simple, city names are mostly ASCII).
  String q; q.reserve(64);
  for (const char *c = name; *c; c++) {
    if (*c == ' ') q += "%20";
    else if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
             (*c >= '0' && *c <= '9') || *c == '-' || *c == '.') q += *c;
    // drop anything else (commas, accents) — the API tolerates a partial name
  }
  char url[192];
  snprintf(url, sizeof(url),
           WX_SCHEME "geocoding-api.open-meteo.com/v1/search?name=%s&count=1&format=json",
           q.c_str());

  bool ok = false;
  String p;
  if (wx_http_get(url, p)) {
    // First result object: {"results":[{"name":..,"latitude":..,"longitude":..}]}
    int r = p.indexOf("\"results\":");
    double la, lo;
    if (r >= 0 &&
        json_find_number(p, r, "latitude",  la) &&
        json_find_number(p, r, "longitude", lo)) {
      weather_set_resolved_coords((float)la, (float)lo);
      ok = true;
      USBSerial.printf("[wx] geocode \"%s\" -> %.4f,%.4f\n", name, la, lo);
    }
  }
  if (!ok) USBSerial.printf("[wx] geocode FAIL for \"%s\"\n", name);
  return ok;
}

/* Fetch + parse weather for the stored location into s_wx. Network-only (no LVGL),
 * safe on the net task. Returns true on a successful parse. */
static bool weather_fetch_raw(void) {
  s_wifi_active = 1;
  if (!wifi_connect()) {
    USBSerial.println("[wx] wifi_connect() failed -> no fetch");
    s_wifi_active = 0;
    return false;
  }

  // If a BLE push set a NAME but no coordinates, resolve them first. A geocode
  // failure is non-fatal — fall through and fetch for the previous coords.
  bool need_geo; store_lock(); need_geo = s_wx.needs_geocode; store_unlock();
  if (need_geo) weather_geocode();

  float lat, lon;
  store_lock(); lat = s_wx.lat; lon = s_wx.lon; store_unlock();

  char url[256];
  weather_build_url(url, sizeof(url), lat, lon);
  USBSerial.printf("[wx] fetching %.4f,%.4f\n", (double)lat, (double)lon);

  bool ok = false;
  String p;
  if (wx_http_get(url, p)) {
    {
      // --- current block ---
      int cur = p.indexOf("\"current\":");
      if (cur < 0) cur = 0;
      double t, feels, hum, wmo, wind, isday;
      bool have_t = json_find_number(p, cur, "temperature_2m", t);
      if (have_t) {
        // Read the RTC ONCE, up front, under i2c_lock (shared bus; the UI reads touch
        // on core 1). Done BEFORE store_lock so the two locks never nest. today_wd
        // labels the forecast days; the epoch stamps "last fetched".
        RTC_DateTime dt; int today_wd; uint32_t fetch_epoch;
        i2c_lock();
        dt = board_clock_now();
        i2c_unlock();
        today_wd = dt.getWeek();                     // 0=Sun..6=Sat
        {
          struct tm tmv = {0};
          tmv.tm_year = dt.getYear() - 1900; tmv.tm_mon = dt.getMonth() - 1;
          tmv.tm_mday = dt.getDay(); tmv.tm_hour = dt.getHour();
          tmv.tm_min  = dt.getMinute(); tmv.tm_sec = dt.getSecond();
          fetch_epoch = (uint32_t)mktime(&tmv);
        }

        store_lock();
        s_wx.have_current = true;
        s_wx.cur_temp_c  = wx_round_c(t);
        s_wx.cur_feels_c = json_find_number(p, cur, "apparent_temperature", feels)
                             ? wx_round_c(feels) : INT16_MIN;
        s_wx.cur_humidity = json_find_number(p, cur, "relative_humidity_2m", hum)
                             ? (uint8_t)(hum < 0 ? 0 : hum > 100 ? 100 : hum) : 0xFF;
        s_wx.cur_wmo     = json_find_number(p, cur, "weather_code", wmo) ? (uint8_t)wmo : 3;
        s_wx.cur_wind_kmh = json_find_number(p, cur, "wind_speed_10m", wind)
                             ? (uint8_t)(wind < 0 ? 0 : wind > 254 ? 254 : wind + 0.5) : 0xFF;
        s_wx.cur_is_day  = json_find_number(p, cur, "is_day", isday) ? (isday >= 0.5) : true;

        // --- daily forecast arrays (parallel arrays, index-aligned) ---
        double dmax[WEATHER_FC_DAYS], dmin[WEATHER_FC_DAYS], dcode[WEATHER_FC_DAYS];
        int nmax  = json_find_num_array(p, "temperature_2m_max", dmax,  WEATHER_FC_DAYS);
        int nmin  = json_find_num_array(p, "temperature_2m_min", dmin,  WEATHER_FC_DAYS);
        int ncode = json_find_num_array(p, "weather_code",       dcode, WEATHER_FC_DAYS);
        int days  = nmax; if (nmin < days) days = nmin; if (ncode < days) days = ncode;
        if (days > WEATHER_FC_DAYS) days = WEATHER_FC_DAYS;
        s_wx.fc_n = (uint8_t)days;
        // Weekday for each forecast day: TODAY's RTC weekday + offset (read above).
        for (int i = 0; i < days; i++) {
          s_wx.fc[i].hi_c = wx_round_c(dmax[i]);
          s_wx.fc[i].lo_c = wx_round_c(dmin[i]);
          s_wx.fc[i].wmo  = (uint8_t)dcode[i];
          s_wx.fc[i].wday = (today_wd >= 0) ? (uint8_t)((today_wd + i) % 7) : 0;
        }
        s_wx.last_fetch_epoch = fetch_epoch;
        store_unlock();

        weather_save_snapshot();   // persist so the face shows this at next boot
        ok = true;
      }
    }
  }
  s_wifi_active = 0;
  USBSerial.printf("[wx] fetch %s: %s\n", ok ? "ok" : "FAIL", s_wx.loc_name);
  return ok;
}

/* The network task body (pinned to core 0). It sleeps until the loop raises
 * s_net_request (notifications) or s_weather_request (weather), runs the
 * (blocking) fetch, and publishes the result for the loop to act on. It NEVER
 * calls LVGL — the loop does the card display / face refresh. */
static void net_task_fn(void *arg) {
  (void)arg;
  for (;;) {
#if WIFI_SCAN_SUPPORTED
    // The user just joined a network from the scan list (app_wifi_ble.h): promote
    // it to an immediate fetch so the new credentials are tried within seconds
    // instead of on the next scheduled sync.
    if (s_wifi_join_kick) { s_wifi_join_kick = false; s_net_request = true; }
#endif
    if (s_net_request) {
      s_net_request = false;
      s_net_busy = true;

      String title, body; uint64_t maxId = 0;
      int count = notif_fetch_raw(title, body, maxId);   // blocks here, on core 0
      if (count > 0 && maxId > rtc_last_notif_id) rtc_last_notif_id = maxId;

      s_net_new_count    = count;     // hand the result to the loop
      s_net_result_ready = true;
      s_net_busy = false;
    }
    if (s_weather_request) {
      s_weather_request = false;
      s_net_busy = true;
      if (weather_fetch_raw()) s_weather_ready = true;   // loop repaints the face/app
      s_net_busy = false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));    // poll the request flags ~20x/sec
  }
}

/* Ask the network task to do a fetch (non-blocking). Ignored if one is already
 * in flight. Called from the loop (core 1). */
static void notif_request_fetch(void) {
  if (!s_net_busy) s_net_request = true;
}

/* Ask the network task to refresh weather (non-blocking). Called from the loop on
 * a slow timer and immediately after a BLE location change. The task serializes it
 * behind any in-flight notification fetch, so it's safe to raise anytime. */
static void weather_request_fetch(void) {
  s_weather_request = true;
}
