/* ============================================================================
 *  ble_provision.h — BLE peripheral: encrypted WiFi provisioning + find-my-phone.
 *
 *  Targets the NimBLE stack that Arduino-ESP32 3.x ships (CONFIG_BT_NIMBLE_ENABLED).
 *  Uses the core's built-in BLE wrapper (BLEDevice/BLEServer/BLESecurity), which
 *  routes to NimBLE — no extra library to install. Header-only, compiled into the
 *  .ino TU. INCLUDE AFTER: device_info.h (DEVICE_NAME), settings_store.h
 *  (settings_get_ble_enabled), wifi_store.h (wifi_nets_add / wifi_nets_save),
 *  watch_base.h (store_lock/unlock) and the FONT_* macros. lvgl.h for the pair code.
 *
 *  DESIGN
 *   - LAZY: brought up only by ble_begin() (toggle ON / boot if enabled) and stopped by
 *     ble_end(). ble_end() does deinit(FALSE): the host/stack stop but the controller's
 *     ~70KB stays RESERVED while off (releasing it is a one-way op that corrupts the
 *     heap on re-init — see ble_end). Deep sleep is a full reboot, so that RAM returns
 *     on wake regardless.
 *   - SECURE: LE Secure Connections + MITM + bonding. The watch DISPLAYS a fresh
 *     6-digit pair code each pairing (display-only IO); the phone enters it. Up to
 *     3 phones bond (NimBLE NVS cap, CONFIG_NIMBLE_MAX_BONDS); bonds persist across
 *     deep sleep / reboot / reflash. The provisioning characteristic requires an
 *     encrypted+authenticated link, so credentials can't be written before pairing.
 *   - GATT (one custom service):
 *       * WiFi-provision (Write, enc+authen): phone writes "SSID,password"
 *         -> wifi_nets_add + wifi_nets_save  (SD /wifi.csv first, flash fallback).
 *       * Find-phone (Notify): ble_ping_phone() notifies a subscribed phone app
 *         so it can ring. Stubbed for the future Find-My-Phone app.
 *
 *  THREADING: BLE callbacks run on the NimBLE host task, NOT the LVGL/loop thread.
 *  They never touch LVGL — they take store_lock for the WiFi store, or set volatile
 *  flags the loop renders via ble_ui_tick() (LVGL-thread safe).
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <host/ble_store.h>   // ble_store_util_bonded_peers() — list bonded phones (NimBLE)

/* ANCS client (ble_ancs.h) is included LATER in the .ino — after the notification
 * store/net headers it depends on — so we only forward-declare the two entry points
 * our BLE callbacks call. The definitions link fine across the single .ino TU. */
static void ancs_on_encrypted(uint16_t conn_handle);
static void ams_on_encrypted(uint16_t conn_handle);   // AMS (media) — same bond as ANCS
static void ams_reset(void);
static void ancs_reset(void);
static void ancs_install_gap_handler(void);

/* Custom 128-bit UUIDs (random; keep in sync with the phone app). */
#define BLE_SVC_UUID       "6b1f0001-9a3e-4c7a-9b2d-2f1a8c5e7d10"
#define BLE_PROV_UUID      "6b1f0002-9a3e-4c7a-9b2d-2f1a8c5e7d10"   // WiFi provision (write, encrypted)
#define BLE_FIND_UUID      "6b1f0003-9a3e-4c7a-9b2d-2f1a8c5e7d10"   // find-phone (notify, watch→phone)
#define BLE_FINDWATCH_UUID "6b1f0004-9a3e-4c7a-9b2d-2f1a8c5e7d10"   // find-watch (write, phone→watch)

/* ---- state (some written from the NimBLE task, read by the loop) ---- */
static bool                 s_ble_up        = false;   // stack initialized + advertising
static volatile bool        s_ble_connected = false;
static volatile bool        s_ble_show_key  = false;   // show the pair code overlay
static volatile uint32_t    s_ble_passkey   = 0;       // 6-digit code to display
static BLEServer           *s_ble_server     = nullptr;
static BLECharacteristic   *s_ble_find_ch    = nullptr;
static lv_obj_t            *s_ble_key_box    = nullptr; // LVGL overlay (loop thread only)
static volatile bool        s_ble_findwatch_req = false; // phone wrote find-watch -> ring (loop consumes)

/* Result toast: set by a NimBLE callback, shown by the loop. PAIRED fires once when
 * a phone finishes pairing; SAVED/DUP/FULL report a WiFi-provisioning write. */
enum BleToast { BLE_TOAST_NONE = 0, BLE_TOAST_PAIRED, BLE_TOAST_SAVED, BLE_TOAST_DUP, BLE_TOAST_FULL };
static volatile BleToast    s_ble_toast      = BLE_TOAST_NONE;
static volatile bool        s_ble_pairing    = false;   // a passkey was shown -> this is a real pairing (not a bonded reconnect)
static char                 s_ble_toast_ssid[WIFI_SSID_MAX] = "";
static lv_obj_t            *s_ble_toast_box  = nullptr;   // loop thread only
static uint32_t             s_ble_toast_until = 0;
static volatile bool        s_ble_dirty      = false;     // one-shot: repaint when an overlay appears/clears
static volatile bool        s_ble_bond_dirty = false;     // one-shot: a bond was added/removed (refresh the paired list)

/* ----------------------------- callbacks --------------------------------- */

/* Connection state: re-advertise after a disconnect so the next paired phone can
 * reconnect (single connection at a time). */
class BleServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { s_ble_connected = true; }
  void onDisconnect(BLEServer *) override {
    s_ble_connected = false;
    s_ble_show_key  = false;                  // drop any stale pair-code overlay
    ancs_reset();                             // forget discovered ANCS handles for this conn
    ams_reset();                              // ...and AMS (media) handles
    if (s_ble_up) BLEDevice::startAdvertising();
  }
};

/* Pairing: display-only -> the stack hands us the passkey to SHOW; the phone types
 * it. We never touch LVGL here (wrong thread) — just stash the code + raise a flag. */
class BleSecurityCB : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }                 // we display, not enter
  void onPassKeyNotify(uint32_t key) override {
    s_ble_passkey = key; s_ble_show_key = true;
    s_ble_pairing = true;                     // a code was displayed -> this is a fresh pairing
  }
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {  // NimBLE signature
    s_ble_show_key = false;                   // pairing finished (ok or not) -> hide code
    bool ok = desc && desc->sec_state.encrypted;
    // Toast only a genuine pairing (one that showed a passkey), not a bonded
    // reconnect — the latter restores encryption without onPassKeyNotify.
    if (ok && s_ble_pairing) {
      s_ble_toast = BLE_TOAST_PAIRED;
      s_ble_bond_dirty = true;   // a new bond exists now -> let an open WiFi&BLE screen refresh its paired list
    }
    s_ble_pairing = false;
    USBSerial.printf("[ble] auth %s\n", ok ? "OK (encrypted/bonded)" : "FAILED");
    // Link is now encrypted+bonded — the prerequisite for ANCS. Kick off discovery
    // of the iPhone's notification service over this same connection.
    // ANCS discovery runs first; it chains into AMS discovery when done (NimBLE
    // allows only one GATT procedure per connection at a time, so they must serialize).
    if (ok && desc) ancs_on_encrypted(desc->conn_handle);
  }
};

/* WiFi provisioning write: payload is "SSID,password" (split on the FIRST comma so
 * the password may contain commas; SSID may not). Runs on the NimBLE task — guards
 * the shared WiFi store, then persists (SD CSV first, flash fallback). */
class BleProvCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *ch) override {
    String v = ch->getValue();
    if (v.length() == 0) return;
    int comma = v.indexOf(',');
    String ssid = (comma < 0) ? v : v.substring(0, comma);
    String pass = (comma < 0) ? String("") : v.substring(comma + 1);
    ssid.trim();
    if (ssid.length() == 0) return;

    store_lock();
    bool existed = false;                       // distinguish duplicate vs full for the toast
    for (uint8_t i = 0; i < s_wifi_net_count; i++)
      if (strncmp(s_wifi_nets[i].ssid, ssid.c_str(), WIFI_SSID_MAX) == 0) { existed = true; break; }
    bool added = wifi_nets_add(ssid.c_str(), pass.c_str());
    if (added) wifi_nets_save();              // -> /wifi.csv (precedence) + flash mirror
    store_unlock();

    strncpy(s_ble_toast_ssid, ssid.c_str(), sizeof(s_ble_toast_ssid) - 1);
    s_ble_toast_ssid[sizeof(s_ble_toast_ssid) - 1] = '\0';
    s_ble_toast = added ? BLE_TOAST_SAVED : (existed ? BLE_TOAST_DUP : BLE_TOAST_FULL);
    USBSerial.printf("[ble] provision \"%s\" -> %s\n", ssid.c_str(),
                     added ? "saved" : (existed ? "duplicate" : "full"));
  }
};

/* Find-watch write (phone → watch): any write rings the watch. Runs on the NimBLE
 * task, so it only raises a flag the loop turns into the alarm overlay. */
class BleFindWatchCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *) override {
    s_ble_findwatch_req = true;
    USBSerial.println("[ble] find-watch ping");
  }
};

/* Callback singletons (not new'd per begin) so toggling BLE never leaks. */
static BleServerCB    s_cb_server;
static BleSecurityCB  s_cb_security;
static BleProvCB      s_cb_prov;
static BleFindWatchCB s_cb_findwatch;

/* ----------------------------- public API -------------------------------- */

static bool ble_is_up(void) { return s_ble_up; }

/* True while a phone is actively connected over BLE (drives the watchface BLE
 * icon). Note: iOS keeps the link only while it has reason to (e.g. the companion
 * app is in use), so this reflects a live connection, not merely a saved bond. */
static bool ble_phone_connected(void) { return s_ble_up && s_ble_connected; }

/* For the watchface present-gate: true while any BLE overlay (pair code or toast)
 * is on screen, plus a one-shot dirty that fires when one appears/clears so the
 * final clear frame is pushed even from the idle clock face. */
static bool ble_overlay_active(void) { return s_ble_show_key || s_ble_key_box || s_ble_toast_box; }
static bool ble_take_dirty(void) { bool d = s_ble_dirty; s_ble_dirty = false; return d; }

/* One-shot: true once after a bond is added (pairing completes). The WiFi & BLE
 * screen polls this on a light timer so a newly-paired phone appears in the paired
 * list immediately, without leaving and re-entering the screen. */
static bool ble_take_bond_dirty(void) { bool d = s_ble_bond_dirty; s_ble_bond_dirty = false; return d; }

/* Bring up the BLE peripheral (idempotent). */
static void ble_begin(void) {
  if (s_ble_up) return;

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setSecurityCallbacks(&s_cb_security);
  ancs_install_gap_handler();   // observe inbound ANCS notifications (no core patch needed)

  // LE Secure Connections + MITM + bonding; we DISPLAY a fresh random 6-digit code
  // each pairing (display-only IO). A passkey MUST be "set" for the display action
  // to fire; regen-on-connect rotates it per pairing.
  BLESecurity::setAuthenticationMode(true, true, true);   // bonding, MITM, secure connections
  BLESecurity::setCapability(ESP_IO_CAP_OUT);             // display only
  BLESecurity::setPassKey(false);                         // dynamic passkey (marks one as set)
  BLESecurity::regenPassKeyOnConnect(true);               // new code every pairing

  s_ble_server = BLEDevice::createServer();
  s_ble_server->setCallbacks(&s_cb_server);

  BLEService *svc = s_ble_server->createService(BLE_SVC_UUID);

  // WiFi provisioning: write requires an ENCRYPTED + AUTHENTICATED (paired) link.
  BLECharacteristic *prov = svc->createCharacteristic(
      BLE_PROV_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_ENC |
      BLECharacteristic::PROPERTY_WRITE_AUTHEN);
  prov->setCallbacks(&s_cb_prov);

  // Find-phone: notify a subscribed phone app (it rings). NimBLE adds the CCC
  // descriptor automatically for NOTIFY characteristics.
  s_ble_find_ch = svc->createCharacteristic(
      BLE_FIND_UUID, BLECharacteristic::PROPERTY_NOTIFY);

  // Find-watch: the phone writes here to ring the watch. Same enc+authen (paired)
  // requirement as provisioning, so only a bonded phone can sound the alarm.
  BLECharacteristic *fw = svc->createCharacteristic(
      BLE_FINDWATCH_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_ENC |
      BLECharacteristic::PROPERTY_WRITE_AUTHEN);
  fw->setCallbacks(&s_cb_findwatch);

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  // NOTE (ANCS, milestone 1): we do NOT advertise an ANCS Service Solicitation
  // (AD type 0x15) here. iOS usually exposes ANCS to a bonded peer on GATT discovery
  // without it, and a 2nd 128-bit UUID won't fit alongside our service UUID in the
  // 31-byte adv budget. If discovery logs "no ANCS service exposed", add solicitation
  // then (and move BLE_SVC_UUID to the scan response to make room).
  BLEDevice::startAdvertising();

  s_ble_up = true;
  // Log free internal SRAM each bring-up so the known (IDF-internal) controller leak
  // across init/deinit cycles is OBSERVABLE — if this trends downward across genuine
  // toggles over a long uptime, that's the leak creeping. The toggle debounce keeps it
  // from being reachable quickly; this just makes it visible if it ever matters.
  USBSerial.printf("[ble] up (advertising, secure provisioning ready) — SRAM %u KB free\n",
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

/* True while a NimBLE host task is still alive. The exact task name varies by build,
 * so we scan the live FreeRTOS task list and substring-match "nimble"/"ble" (case-
 * insensitive). Used by ble_end() to wait out the host task's async self-deletion
 * before returning, so the next ble_begin()'s `while(!m_synced)` busy-wait can't spin
 * forever (Task WDT). Requires configUSE_TRACE_FACILITY (on by default in ESP-IDF). */
static bool ble_host_task_present(void) {
  UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t *st = (TaskStatus_t *)malloc(n * sizeof(TaskStatus_t));
  if (!st) { delay(60); return false; }       // can't enumerate -> fall back to a settle
  n = uxTaskGetSystemState(st, n, nullptr);
  bool found = false;
  for (UBaseType_t i = 0; i < n && !found; i++) {
    const char *nm = st[i].pcTaskName;
    if (!nm) continue;
    // lowercase substring scan for "nimble" or "ble"
    char low[configMAX_TASK_NAME_LEN];
    size_t j = 0;
    for (; nm[j] && j < sizeof(low) - 1; j++)
      low[j] = (nm[j] >= 'A' && nm[j] <= 'Z') ? (nm[j] + 32) : nm[j];
    low[j] = '\0';
    if (strstr(low, "nimble") || strstr(low, "ble")) found = true;
  }
  free(st);
  return found;
}

/* Fully tear down BLE and free its memory (toggle OFF / before deep sleep).
 * Deinitializing the stack with a link still UP panics NimBLE, so we first ask the
 * peer to disconnect and wait (briefly) for it to settle before freeing anything. */
static void ble_end(void) {
  if (!s_ble_up) return;
  s_ble_up = false;                           // do this FIRST so onDisconnect won't re-advertise mid-teardown

  // Gracefully drop a live connection before deinit. onDisconnect (NimBLE host
  // task) clears s_ble_connected; delay() yields so that task can run.
  if (s_ble_server && s_ble_connected) {
    s_ble_server->disconnect(s_ble_server->getConnId());
    for (uint32_t t0 = millis(); s_ble_connected && millis() - t0 < 300; ) delay(10);
  }

  s_ble_connected = false;
  s_ble_show_key  = false;

  // deinit(false), NOT deinit(true): `true` calls esp_bt_controller_mem_release(BTDM),
  // which is a ONE-WAY release of the controller's BSS/data region back to the general
  // heap. The next ble_begin() -> esp_bt_controller_init() then tries to reclaim that
  // exact region, but the allocator has since carved it up (partial buffers, LVGL, …),
  // so the controller inits over memory the heap believes is free -> heap free-list
  // corruption -> malloc() faults (LoadProhibited in tlsf_malloc) on the SECOND enable.
  // Passing false keeps the controller memory RESERVED across the off state (~70KB held
  // while BLE is off) so re-init is clean and repeatable. The watch toggles BLE, it
  // doesn't need that RAM back while off — correctness beats reclaiming it.
  BLEDevice::deinit(false);                   // stop stack but KEEP controller memory (re-init-safe)

  // CRITICAL for BLE off->on->on: BLEDevice::deinit() calls nimble_port_stop() then
  // returns, but the NimBLE HOST TASK deletes ITSELF asynchronously (host_task() runs
  // nimble_port_run(), then on stop calls nimble_port_freertos_deinit()). If the next
  // ble_begin() calls BLEDevice::init() while that old task is still alive, the fresh
  // stack never reaches sync and init() BUSY-WAITS `while (!m_synced)` forever -> Task
  // WDT reset. (That's the "first enable fine, SECOND enable WDT-resets" bug.) So WAIT
  // for the host task to be gone before returning. The task's exact name varies by
  // build, so ble_host_task_present() substring-matches the live task list. Bounded so
  // it can never hang; plus a short controller settle.
  uint32_t t0 = millis();
  while (ble_host_task_present() && millis() - t0 < 1500) delay(10);
  delay(50);                                  // let the controller fully settle post-stop
  if (ble_host_task_present())                // should never happen; warn if the drain timed out
    USBSerial.println("[ble] WARN: host task still present after drain timeout");

  s_ble_server  = nullptr;
  s_ble_find_ch = nullptr;
  USBSerial.println("[ble] down");
}

/* Find-My-Phone: notify a connected+subscribed phone app so it can ring. No-op if
 * BLE is down or nothing is subscribed. (For the future Find-My-Phone app.) */
static bool ble_ping_phone(void) {
  if (!s_ble_up || !s_ble_connected || !s_ble_find_ch) return false;
  s_ble_find_ch->setValue("RING");
  s_ble_find_ch->notify();
  return true;
}

/* Find-My-Watch: true once (read-and-clear) when the phone wrote the find-watch
 * characteristic. The loop turns this into the alarm overlay (LVGL-thread safe). */
static bool ble_take_find_watch_req(void) {
  bool r = s_ble_findwatch_req; s_ble_findwatch_req = false; return r;
}

/* ---- bonded-device (paired phone) management ----
 * NimBLE keeps bonds in NVS; these read/forget them. They need the host running,
 * so they return 0 / false when BLE is off (turn BLE on to manage paired phones). */
#define BLE_BOND_MAX 3   // mirrors CONFIG_NIMBLE_MAX_BONDS

/* Number of bonded phones (0 if BLE is off). */
static int ble_bond_count(void) {
  if (!s_ble_up) return 0;
  ble_addr_t addrs[BLE_BOND_MAX];
  int n = 0;
  if (ble_store_util_bonded_peers(addrs, &n, BLE_BOND_MAX) != 0) return 0;
  return n;
}

/* Format bond index i as "XX:XX:XX:XX:XX:XX" (addr bytes are little-endian).
 * Returns false if BLE is off or i is out of range. */
static bool ble_bond_addr_str(int i, char *buf, size_t cap) {
  if (!s_ble_up) return false;
  ble_addr_t addrs[BLE_BOND_MAX];
  int n = 0;
  if (ble_store_util_bonded_peers(addrs, &n, BLE_BOND_MAX) != 0 || i < 0 || i >= n) return false;
  const uint8_t *v = addrs[i].val;
  snprintf(buf, cap, "%02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
  return true;
}

/* Forget (unpair) bond index i. Returns true if removed. */
static bool ble_bond_forget(int i) {
  if (!s_ble_up) return false;
  ble_addr_t addrs[BLE_BOND_MAX];
  int n = 0;
  if (ble_store_util_bonded_peers(addrs, &n, BLE_BOND_MAX) != 0 || i < 0 || i >= n) return false;
  return ble_gap_unpair(&addrs[i]) == 0;
}

/* Apply the saved toggle preference: bring BLE up or down to match. Call at boot
 * and whenever the WiFi&BLE switch changes. */
static void ble_apply_enabled(void) {
  if (settings_get_ble_enabled()) ble_begin();
  else                            ble_end();
}

/* LVGL-thread renderer for the pair-code overlay. Call every loop iteration. It
 * creates/destroys a small system-layer box showing the 6-digit code while a phone
 * is pairing (flags set by the NimBLE task). Must run on the loop (LVGL) thread. */
static void ble_ui_tick(void) {
  // --- pair-code overlay (while pairing) ---
  if (s_ble_show_key && !s_ble_key_box) {
    s_ble_key_box = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(s_ble_key_box, 300, 140);
    lv_obj_center(s_ble_key_box);
    lv_obj_set_style_bg_color(s_ble_key_box, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(s_ble_key_box, 16, 0);
    lv_obj_clear_flag(s_ble_key_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(s_ble_key_box);
    lv_obj_set_style_text_font(t, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(t, "BLE pairing - enter code:");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *k = lv_label_create(s_ble_key_box);
    lv_obj_set_style_text_font(k, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(k, lv_color_white(), 0);
    lv_label_set_text_fmt(k, "%06u", (unsigned)s_ble_passkey);
    lv_obj_align(k, LV_ALIGN_CENTER, 0, 14);
    s_ble_dirty = true;
  } else if (!s_ble_show_key && s_ble_key_box) {
    lv_obj_del(s_ble_key_box);
    s_ble_key_box = nullptr;
    s_ble_dirty = true;
  }

  // --- provisioning result toast (auto-dismisses after ~3.5 s) ---
  if (s_ble_toast != BLE_TOAST_NONE && !s_ble_toast_box) {
    BleToast r = s_ble_toast;
    s_ble_toast = BLE_TOAST_NONE;

    s_ble_toast_box = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(s_ble_toast_box, 340, 92);
    lv_obj_align(s_ble_toast_box, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_color(s_ble_toast_box, lv_color_hex(0x141414), 0);
    lv_obj_set_style_radius(s_ble_toast_box, 16, 0);
    lv_obj_clear_flag(s_ble_toast_box, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t col; char msg[80];
    if (r == BLE_TOAST_PAIRED) {
      col = 0x33A0FF; snprintf(msg, sizeof(msg), LV_SYMBOL_BLUETOOTH "  Phone paired");
    } else if (r == BLE_TOAST_SAVED) {
      col = 0x32D74B; snprintf(msg, sizeof(msg), LV_SYMBOL_OK "  WiFi saved\n%s", s_ble_toast_ssid);
    } else if (r == BLE_TOAST_DUP) {
      col = 0xFF9F0A; snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  Already saved\n%s", s_ble_toast_ssid);
    } else {
      col = 0xFF453A; snprintf(msg, sizeof(msg), LV_SYMBOL_CLOSE "  Network list full");
    }
    lv_obj_t *l = lv_label_create(s_ble_toast_box);
    lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 312);
    lv_label_set_text(l, msg);
    lv_obj_center(l);

    s_ble_toast_until = millis() + 3500;
    s_ble_dirty = true;
  } else if (s_ble_toast_box && (int32_t)(millis() - s_ble_toast_until) > 0) {
    lv_obj_del(s_ble_toast_box);
    s_ble_toast_box = nullptr;
    s_ble_dirty = true;
  }
}
