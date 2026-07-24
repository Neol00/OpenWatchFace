/* ============================================================================
 *  ble_gadgetbridge_tuya.h - Android notifications via Gadgetbridge on the Tuya
 *  T5-E1 (Bangle.js protocol), raw-NimBLE port of ble_gadgetbridge.h.
 *
 *  Same protocol and the SAME store/UI handoff as the ESP32 build: Gadgetbridge
 *  writes newline-terminated JSON lines to the Nordic UART Service RX
 *  characteristic; we reassemble + parse them and push notifications into the
 *  shared notif store (notif_store_add + na_append under store_lock), then stash
 *  s_pop_* and raise s_ancs_ui_dirty / s_ancs_removed - exactly the flags the
 *  loop already consumes for ANCS/HTTP. So an Android phone needs ZERO changes in
 *  the .ino UI path.
 *
 *  ONLY the BLE transport differs from ble_gadgetbridge.h:
 *    - RX arrives via owf_gatt_access_cb (ble_tuya.h) -> gb_rx_bytes() here.
 *    - TX (watch->phone notify) goes out via ble_gattc_notify_custom() on the NUS
 *      TX value handle, instead of BLECharacteristic::notify(). ble_tuya.h hands
 *      us the conn + TX handle through gb_set_tx_handle().
 *
 *  The message handlers (notify / notify- / musicinfo / musicstate / find /
 *  setTime) are copied from ble_gadgetbridge.h unchanged - they touch only the
 *  shared store / player / RTC, which are identical across platforms.
 *
 *  THREADING: gb_rx_bytes runs on the NimBLE host task (from the GATT write cb);
 *  it guards the store and only flags the loop, never touching LVGL. gb_send is
 *  safe to call from the loop thread (NimBLE host APIs are task-safe), used by
 *  ble_report_battery / the Player.
 *
 *  INCLUDE AFTER ble_tuya.h (provides s_ble_connected and the gb_* forward
 *  declares it expects), and after notif_store/archive/net + player_state (for
 *  notif_store_add, na_*, s_pop_*, json_find_string, player_*). Header-only.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <time.h>

/* Map a Gadgetbridge 32-bit id into the store's 64-bit id space, distinct from
 * HTTP ids (~1e12 epoch millis) and ANCS_ID_BASE (0x5...). Same base as ESP32. */
#define GB_ID_BASE  ((uint64_t)0x6000000000000000ULL)
#define GB_LINE_MAX 1024

static char    *s_gb_line     = nullptr;   // [GB_LINE_MAX], lazy
static uint16_t s_gb_len      = 0;
static bool     s_gb_overflow = false;
static uint16_t s_gb_tx_conn  = BLE_HS_CONN_HANDLE_NONE;   // set by gb_set_tx_handle
static uint16_t s_gb_tx_h     = 0;                          // NUS TX value handle

static bool gb_buf_ok(void) {
  if (!s_gb_line) s_gb_line = (char *)malloc(GB_LINE_MAX);
  return s_gb_line != nullptr;
}

/* ble_tuya.h calls this on connect/disconnect to point gb_send at the live NUS
 * TX characteristic (or clear it). */
static void gb_set_tx_handle(uint16_t conn, uint16_t val_handle) {
  s_gb_tx_conn = conn;
  s_gb_tx_h    = val_handle;
}

/* ---------------------------- tiny JSON helper --------------------------- */
/* "key":<int>. Mirrors gb_json_u32 from ble_gadgetbridge.h. */
static bool gb_json_u32(const String &src, const char *key, uint32_t *out) {
  String pat = String("\"") + key + "\":";
  int k = src.indexOf(pat);
  if (k < 0) return false;
  k += pat.length();
  while (k < (int)src.length() && src[k] == ' ') k++;
  bool neg = (k < (int)src.length() && src[k] == '-');
  if (neg) k++;
  uint64_t v = 0; bool any = false;
  while (k < (int)src.length() && src[k] >= '0' && src[k] <= '9') { v = v * 10 + (src[k] - '0'); k++; any = true; }
  if (!any) return false;
  *out = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
  return true;
}

/* --------------------------- message handlers ---------------------------- */

static void gb_handle_notify(const String &js) {
  uint32_t nid = 0;
  gb_json_u32(js, "id", &nid);

  String src, title, body;
  json_find_string(js, 0, "src", src);
  json_find_string(js, 0, "title", title);
  json_find_string(js, 0, "body", body);
  if (title.length() == 0) json_find_string(js, 0, "subject", title);
  if (title.length() == 0) title = src;
  if (title.length() == 0 && body.length() == 0) return;

  if (nid == 0) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)title.length(); i++) { h ^= (uint8_t)title[i]; h *= 16777619u; }
    for (size_t i = 0; i < (size_t)body.length();  i++) { h ^= (uint8_t)body[i];  h *= 16777619u; }
    nid = h ? h : 1;
  }
  uint64_t id  = GB_ID_BASE | (uint64_t)nid;
  uint8_t  cat = notif_cat_from_appid(src.c_str());

  store_lock();
  bool added = notif_store_add(id, title.c_str(), body.c_str(), cat);
  if (added && na_available()) na_append(id, title.c_str(), body.c_str(), cat);
  if (added) {
    notif_store_save();
    s_pop_id = id;
    strncpy(s_pop_title, title.c_str(), sizeof(s_pop_title) - 1);
    s_pop_title[sizeof(s_pop_title) - 1] = '\0';
    strncpy(s_pop_body, body.c_str(), sizeof(s_pop_body) - 1);
    s_pop_body[sizeof(s_pop_body) - 1] = '\0';
    s_pop_have = true;
    s_ancs_ui_dirty = true;
    s_ancs_added_total++;
  }
  store_unlock();
  USBSerial.printf("[gb] %s id=%lu src=\"%s\" title=\"%s\"\n",
                   added ? "stored" : "dup", (unsigned long)nid, src.c_str(), title.c_str());
}

static void gb_handle_dismiss(const String &js) {
  uint32_t nid = 0;
  if (!gb_json_u32(js, "id", &nid)) return;
  uint64_t id = GB_ID_BASE | (uint64_t)nid;
  store_lock();
  bool hit = false;
  if (na_available() && na_remove(id)) hit = true;
  if (notif_store_remove_by_id(id))    hit = true;
  store_unlock();
  if (hit) { s_ancs_removed = true; USBSerial.printf("[gb] removed id=%lu\n", (unsigned long)nid); }
}

/* ---------------- music: Player app integration ---------------- */
static void gb_player_cmd(PlayerCmd cmd) {
  switch (cmd) {
    case PCMD_TOGGLE: gb_send("{\"t\":\"music\",\"n\":\"playpause\"}"); break;
    case PCMD_NEXT:   gb_send("{\"t\":\"music\",\"n\":\"next\"}");      break;
    case PCMD_PREV:   gb_send("{\"t\":\"music\",\"n\":\"previous\"}");  break;
  }
}
static void gb_handle_musicinfo(const String &js) {
  String artist, album, track;
  json_find_string(js, 0, "artist", artist);
  json_find_string(js, 0, "album",  album);
  json_find_string(js, 0, "track",  track);
  player_set_sink(PSRC_ANDROID, gb_player_cmd, nullptr);
  player_set_track(track.c_str(), artist.c_str(), album.c_str());
}
static void gb_handle_musicstate(const String &js) {
  String st;
  if (!json_find_string(js, 0, "state", st)) return;
  player_set_sink(PSRC_ANDROID, gb_player_cmd, nullptr);
  if      (st == "play")  player_set_state(PLAYING);
  else if (st == "pause") player_set_state(PLAY_PAUSED);
  else                    player_set_state(PLAY_STOPPED);
}

/* setTime(EPOCH);E.setTimeZone(H);... - Gadgetbridge time sync (raw JS). */
static void gb_handle_settime(const char *line) {
  const char *p = strstr(line, "setTime(");
  uint32_t utc = (uint32_t)strtoul(p + 8, nullptr, 10);
  if (utc < 1600000000UL) return;
  float tzh = 0.0f;
  const char *z = strstr(line, "setTimeZone(");
  if (z) tzh = strtof(z + 12, nullptr);
  time_t local = (time_t)utc + (time_t)(tzh * 3600.0f);
  struct tm tmv;
  gmtime_r(&local, &tmv);
  i2c_lock();
  board_clock_set(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  i2c_unlock();
  rtc_last_ntp_epoch = (uint32_t)local;
  USBSerial.printf("[gb] time sync %04d-%02d-%02d %02d:%02d:%02d (tz %+.1f)\n",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (double)tzh);
}

static void gb_handle_line(char *line) {
  while (*line && (uint8_t)*line <= ' ') line++;
  if (!*line) return;
  if (strstr(line, "setTime(")) { gb_handle_settime(line); return; }
  const char *o = strchr(line, '{');
  if (!o) return;
  String js(o);
  String t;
  if (!json_find_string(js, 0, "t", t)) return;
  if      (t == "notify")     gb_handle_notify(js);
  else if (t == "notify-")    gb_handle_dismiss(js);
  else if (t == "musicinfo")  gb_handle_musicinfo(js);
  else if (t == "musicstate") gb_handle_musicstate(js);
  else if (t == "find") { if (js.indexOf("\"n\":true") >= 0) s_ble_findwatch_req = true; }
  else USBSerial.printf("[gb] ignored t=\"%s\"\n", t.c_str());
}

/* ----------------------------- public entry ------------------------------- */

/* NUS RX bytes (NimBLE host task): reassemble into '\n'-terminated lines. */
static void gb_rx_bytes(const uint8_t *data, uint16_t len) {
  if (len == 0 || !gb_buf_ok()) return;
  for (uint16_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\n') {
      if (!s_gb_overflow) { s_gb_line[s_gb_len] = '\0'; gb_handle_line(s_gb_line); }
      s_gb_len = 0; s_gb_overflow = false;
      continue;
    }
    if (s_gb_overflow) continue;
    if (s_gb_len < GB_LINE_MAX - 1) s_gb_line[s_gb_len++] = c;
    else { s_gb_len = 0; s_gb_overflow = true; }
  }
}

/* Drop any half-received line + clear the Player if WE were its source. */
static void gb_reset(void) {
  s_gb_len = 0;
  s_gb_overflow = false;
  if (s_play_src == PSRC_ANDROID) player_clear(PSRC_ANDROID);
}

/* Send one JSON line watch -> phone over the NUS TX characteristic. Must end
 * "\r\n" (Gadgetbridge strips the byte before '\n' as a presumed '\r'), and is
 * chunked at 20 bytes so it survives the default 23-byte ATT MTU regardless of
 * negotiation - exactly like the ESP32 gb_send(). */
static bool gb_send(const char *json) {
  if (!s_gb_tx_h || s_gb_tx_conn == BLE_HS_CONN_HANDLE_NONE || !s_ble_connected) return false;
  String v = String(json) + "\r\n";
  const uint8_t *p = (const uint8_t *)v.c_str();
  size_t left = v.length();
  while (left) {
    size_t n = left > 20 ? 20 : left;
    struct os_mbuf *om = ble_hs_mbuf_from_flat((void *)p, (uint16_t)n);
    if (!om) return false;
    ble_gattc_notify_custom(s_gb_tx_conn, s_gb_tx_h, om);
    p += n; left -= n;
  }
  USBSerial.printf("[gb] tx: %s", v.c_str());
  return true;
}
