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

/* ---- core-0 task handshake flags (loop <-> net task) ---- */
static TaskHandle_t   s_net_task        = nullptr;
static volatile bool  s_net_request     = false;     // loop -> task: please fetch
static volatile bool  s_net_busy        = false;     // task is currently working
static volatile int   s_net_new_count   = 0;         // task -> loop: # new this fetch
static volatile bool  s_net_result_ready= false;     // task -> loop: result to consume

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

/* The network task body (pinned to core 0). It sleeps until the loop raises
 * s_net_request, runs the (blocking) fetch, and publishes the result for the
 * loop to act on. It NEVER calls LVGL — the loop does the card display. */
static void net_task_fn(void *arg) {
  (void)arg;
  for (;;) {
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
    vTaskDelay(pdMS_TO_TICKS(50));    // poll the request flag ~20x/sec
  }
}

/* Ask the network task to do a fetch (non-blocking). Ignored if one is already
 * in flight. Called from the loop (core 1). */
static void notif_request_fetch(void) {
  if (!s_net_busy) s_net_request = true;
}
