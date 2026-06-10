/* ============================================================================
 *  app_wifi_ble.h — WiFi & BLE sub-app (radio toggles + saved-network manager).
 *
 *  Split out of app_menu.h. Header-only module compiled into the .ino TU, so it
 *  shares the menu statics. INCLUDE AFTER app_menu.h (uses the screen shell
 *  app_scr / app_screen_begin) and after WatchFace.ino's globals it uses:
 *    - settings_get/set_wifi_enabled, settings_get/set_ble_enabled
 *    - the known-WiFi store: s_wifi_nets / s_wifi_net_count / wifi_nets_remove /
 *      WIFI_NET_MAX, plus store_lock/unlock and the FONT_* macros.
 *
 *  Two switches (WiFi / BLE) at the top, then a scrollable list of saved
 *  networks. Each network has a trash button that opens a Forget confirmation
 *  dialog (destructive, so it can't be a single misclick). BLE provisioning
 *  isn't wired up yet — its switch only records intent.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

static void wifi_sw_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  settings_set_wifi_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}
static void app_open_wifi_ble(void);   // fwd (rebuilt when BLE toggles, to refresh the paired list)

/* While this screen is open, a light timer watches for a bond appearing (a phone
 * finishing pairing, which completes asynchronously on the BLE host task) so the
 * "Paired phones" list refreshes immediately instead of only on re-entry. */
static lv_timer_t *s_wb_timer = nullptr;
static void wb_rebuild_async(void *p) {
  (void)p;
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  app_open_wifi_ble();
}
static void wb_poll_cb(lv_timer_t *t) {
  (void)t;
  // Rebuild only when a new bond actually appeared (one-shot dirty), deferred out of
  // the timer so we never delete app_scr from inside its own callback chain.
  if (ble_take_bond_dirty()) lv_async_call(wb_rebuild_async, nullptr);
}
static void wb_cleanup_cb(lv_event_t *e) {
  (void)e;
  if (s_wb_timer) { lv_timer_del(s_wb_timer); s_wb_timer = nullptr; }
}

static void ble_sw_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);

  // DEBOUNCE: a full BLE on/off cycle does heavy BT-controller init/deinit on the
  // cross-core ipc0 task. Two IDF-internal problems compound under rapid cycling:
  //   (1) btdm_intr_alloc/esp_intr_alloc leaks a little internal RAM per cycle (the
  //       controller's interrupt resources aren't fully balanced across init/deinit) —
  //       eventually "BLE_INIT: Malloc failed".
  //   (2) controller init runs on the SMALL ipc0 stack; re-entering fast overflows its
  //       canary (panic).
  // Both live inside the prebuilt bt.c controller blob — not cleanly patchable from
  // here without rebuilding the IDF BT component. So we make them UNREACHABLE in
  // practice: reject any toggle within BLE_TOGGLE_MIN_MS of the last one (snap the
  // switch back to the real state, do no radio work). Normal use toggles far slower
  // than this; only deliberate mashing is throttled, and at this spacing a real cycle
  // fully settles (the ~52ms host drain + controller teardown) before another can start.
  #define BLE_TOGGLE_MIN_MS 1500
  static uint32_t s_last_toggle_ms = 0;
  uint32_t now = millis();
  if (now - s_last_toggle_ms < BLE_TOGGLE_MIN_MS) {
    // Revert the visual switch to the real BLE state; do NOT touch the radio.
    if (ble_is_up()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else             lv_obj_remove_state(sw, LV_STATE_CHECKED);
    return;
  }
  s_last_toggle_ms = now;

  settings_set_ble_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
  ble_apply_enabled();   // bring the BLE peripheral up/down to match the switch
  // ble_begin()/ble_end() are synchronous, so s_ble_up + the bond store are valid
  // now. Rebuild the screen so the "Paired phones" section reflects the new state
  // instantly (otherwise it only refreshes on next screen entry).
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  app_open_wifi_ble();
}

/* settings_toggle_row() — the shared labelled-switch row — now lives in app_menu.h
 * so every settings screen (here, Power, Appearance) can use the same widget. */

/* ----- Forget-network confirmation dialog -----
 * Forgetting a saved network is destructive, so it can't be a single misclick:
 * tapping a network's trash button opens a modal "Are you sure?" with explicit
 * Forget / Cancel buttons. The target SSID index is carried through user_data. */
static lv_obj_t *wifi_confirm_box = nullptr;

static void wifi_confirm_close(void) {
  if (wifi_confirm_box) { lv_obj_del(wifi_confirm_box); wifi_confirm_box = nullptr; }
}
static void wifi_confirm_cancel_cb(lv_event_t *e) { (void)e; wifi_confirm_close(); }

/* If the dialog is deleted by any path OTHER than wifi_confirm_close() — e.g. its
 * parent app_scr is torn down on Back/BOOT — clear our pointer so it can never
 * dangle into a freed object. */
static void wifi_confirm_deleted_cb(lv_event_t *e) {
  if (lv_event_get_target(e) == wifi_confirm_box) wifi_confirm_box = nullptr;
}

static void wifi_confirm_forget_cb(lv_event_t *e) {
  uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  store_lock();                          // wifi list shares the store mutex w/ net task
  wifi_nets_remove(idx);
  store_unlock();
  wifi_confirm_close();
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  app_open_wifi_ble();                   // rebuild the list
}

static void wifi_forget_prompt_cb(lv_event_t *e) {
  uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_wifi_net_count) return;

  wifi_confirm_close();                  // only ever one dialog
  // Parent to app_scr (not lv_layer_top) so it's auto-deleted if the user backs
  // out of the screen; created last, so it draws above the list. Still modal in
  // practice because it covers the center and the trash buttons sit behind it.
  wifi_confirm_box = lv_obj_create(app_scr);
  lv_obj_add_event_cb(wifi_confirm_box, wifi_confirm_deleted_cb, LV_EVENT_DELETE, nullptr);
  lv_obj_set_size(wifi_confirm_box, 360, 240);
  lv_obj_center(wifi_confirm_box);
  lv_obj_set_style_bg_color(wifi_confirm_box, lv_color_hex(0x202020), 0);
  lv_obj_set_style_radius(wifi_confirm_box, 16, 0);
  lv_obj_clear_flag(wifi_confirm_box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *q = lv_label_create(wifi_confirm_box);
  lv_obj_set_style_text_font(q, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(q, lv_color_white(), 0);
  lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(q, 320);
  lv_label_set_text_fmt(q, "Forget this network?\n\"%s\"", s_wifi_nets[idx].ssid);
  lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 24);

  // Cancel (safe, left) and Forget (destructive/red, right).
  lv_obj_t *cancel = lv_btn_create(wifi_confirm_box);
  lv_obj_set_size(cancel, 140, 56);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 12, -16);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x333333), 0);
  lv_obj_set_style_radius(cancel, 14, 0);
  lv_obj_add_event_cb(cancel, wifi_confirm_cancel_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *cl = lv_label_create(cancel);
  lv_obj_set_style_text_font(cl, &FONT_SMALL, 0);
  lv_label_set_text(cl, "Cancel");
  lv_obj_center(cl);

  lv_obj_t *forget = lv_btn_create(wifi_confirm_box);
  lv_obj_set_size(forget, 140, 56);
  lv_obj_align(forget, LV_ALIGN_BOTTOM_RIGHT, -12, -16);
  lv_obj_set_style_bg_color(forget, lv_color_hex(0x802020), 0);
  lv_obj_set_style_radius(forget, 14, 0);
  lv_obj_add_event_cb(forget, wifi_confirm_forget_cb, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)idx);
  lv_obj_t *fl = lv_label_create(forget);
  lv_obj_set_style_text_font(fl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(fl, lv_color_hex(0xFFB0B0), 0);
  lv_label_set_text(fl, "Forget");
  lv_obj_center(fl);
}

/* One saved-network row inside the scroll list: SSID label + a trash button that
 * opens the forget confirmation. `idx` is the network's index in s_wifi_nets. */
static void wifi_net_row(lv_obj_t *parent, uint8_t idx) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 320, 56);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 14, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ic = lv_label_create(row);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ic, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);
  lv_label_set_text(ic, LV_SYMBOL_WIFI);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);

  lv_obj_t *nm = lv_label_create(row);
  lv_obj_set_style_text_font(nm, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_label_set_text(nm, s_wifi_nets[idx].ssid);
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nm, 210);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 50, 0);

  lv_obj_t *del = lv_btn_create(row);
  lv_obj_set_size(del, 40, 40);
  lv_obj_align(del, LV_ALIGN_RIGHT_MID, -10, 0);
  lv_obj_set_style_bg_color(del, lv_color_hex(0x3A2020), 0);
  lv_obj_set_style_radius(del, 12, 0);
  lv_obj_add_event_cb(del, wifi_forget_prompt_cb, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)idx);
  lv_obj_t *dl = lv_label_create(del);
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(dl, lv_color_hex(0xFF8888), 0);
  lv_label_set_text(dl, LV_SYMBOL_TRASH);
  lv_obj_center(dl);
}

/* Forget (unpair) a bonded phone, then rebuild the screen. Direct (no confirm):
 * re-pairing is low-stakes (just re-enter the code), unlike forgetting WiFi. */
static void ble_forget_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  ble_bond_forget(i);
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  app_open_wifi_ble();                    // rebuild (same screen)
}

/* One bonded-phone row: BLE icon + MAC address + a trash button to unpair. */
static void ble_paired_row(lv_obj_t *parent, int idx) {
  char mac[18];
  if (!ble_bond_addr_str(idx, mac, sizeof(mac))) return;

  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 320, 56);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 14, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ic = lv_label_create(row);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ic, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);   // BLE blue (matches the toggle)
  lv_label_set_text(ic, LV_SYMBOL_BLUETOOTH);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);

  lv_obj_t *nm = lv_label_create(row);
  lv_obj_set_style_text_font(nm, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_label_set_text(nm, mac);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 50, 0);

  lv_obj_t *del = lv_btn_create(row);
  lv_obj_set_size(del, 40, 40);
  lv_obj_align(del, LV_ALIGN_RIGHT_MID, -10, 0);
  lv_obj_set_style_bg_color(del, lv_color_hex(0x3A2020), 0);
  lv_obj_set_style_radius(del, 12, 0);
  lv_obj_add_event_cb(del, ble_forget_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
  lv_obj_t *dl = lv_label_create(del);
  lv_obj_set_style_text_font(dl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(dl, lv_color_hex(0xFF8888), 0);
  lv_label_set_text(dl, LV_SYMBOL_TRASH);
  lv_obj_center(dl);
}

static void app_open_wifi_ble(void) {
  app_screen_begin("WiFi & BLE");

  // Scrollable column holding the toggles, the hint, and the saved-network list,
  // so the screen grows past one viewport as networks are added (the screen shell
  // itself is fixed; this inner container scrolls).
  lv_obj_t *list = lv_obj_create(app_scr);
  lv_obj_set_size(list, 360, 400);       // runs to the screen bottom (BOOT = back)
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 84);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 4, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(list, 10, 0);

  settings_toggle_row(list, LV_SYMBOL_WIFI, "WiFi", settings_get_wifi_enabled(),
                      wifi_sw_cb);
  settings_toggle_row(list, LV_SYMBOL_BLUETOOTH, "BLE", settings_get_ble_enabled(),
                      ble_sw_cb);

  // Hint: WiFi off = no fetches; BLE on = pair a phone (it shows a code to enter)
  // to send WiFi credentials, which save to the SD card first, flash as fallback.
  lv_obj_t *hint = lv_label_create(list);
  lv_obj_set_style_text_font(hint, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x777777), 0);
  lv_label_set_text(hint, "WiFi off = go dark (no fetches).\n"
                          "BLE on = pair phone to send WiFi.");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

  // Section header for the saved networks. The cap depends on the backing store:
  // The CSV backend (SD card if present, else the on-flash FAT partition) holds up to
  // WIFI_NET_CAP; only the rare no-backend case is limited to the NVS WIFI_NET_MAX. We
  // tag the actual source so it's clear where the authoritative list lives.
  lv_obj_t *hdr = lv_label_create(list);
  lv_obj_set_style_text_font(hdr, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(0xAAAAAA), 0);
  lv_label_set_text_fmt(hdr, "Known networks (%u/%u)  %s",
                        s_wifi_net_count, s_wifi_sd ? WIFI_NET_CAP : WIFI_NET_MAX,
                        s_wifi_sd ? (store_on_sd() ? "[SD]" : "[flash]") : "[NVS]");

  if (s_wifi_net_count == 0) {
    lv_obj_t *none = lv_label_create(list);
    lv_obj_set_style_text_font(none, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(none, lv_color_hex(0x777777), 0);
    lv_label_set_text(none, "No saved networks");
  } else {
    for (uint8_t i = 0; i < s_wifi_net_count; i++) wifi_net_row(list, i);
  }

  // ---- Paired BLE devices (separate section, BLE-blue icons) ----
  // Bonds live in NimBLE's NVS store and can only be read while BLE is running,
  // so this populates when the BLE toggle is on. Up to BLE_BOND_MAX phones.
  lv_obj_t *bhdr = lv_label_create(list);
  lv_obj_set_style_text_font(bhdr, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(bhdr, lv_color_hex(0xAAAAAA), 0);
  if (ble_is_up())
    lv_label_set_text_fmt(bhdr, "Paired phones (%d/%d)", ble_bond_count(), BLE_BOND_MAX);
  else
    lv_label_set_text(bhdr, "Paired phones");

  if (!ble_is_up()) {
    lv_obj_t *off = lv_label_create(list);
    lv_obj_set_style_text_font(off, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(off, lv_color_hex(0x777777), 0);
    lv_label_set_text(off, "Turn on BLE to manage");
  } else {
    int bn = ble_bond_count();
    if (bn == 0) {
      lv_obj_t *none = lv_label_create(list);
      lv_obj_set_style_text_font(none, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(none, lv_color_hex(0x777777), 0);
      lv_label_set_text(none, "No paired phones");
    } else {
      for (int i = 0; i < bn; i++) ble_paired_row(list, i);
    }
  }

  // Watch for an async pairing completing while this screen is up, so a newly-paired
  // phone appears without leaving + re-entering. Recreated on each (re)build; torn
  // down with the screen via the DELETE handler so it never fires into a dead screen.
  if (s_wb_timer) { lv_timer_del(s_wb_timer); s_wb_timer = nullptr; }
  s_wb_timer = lv_timer_create(wb_poll_cb, 400, nullptr);
  lv_obj_add_event_cb(app_scr, wb_cleanup_cb, LV_EVENT_DELETE, nullptr);
}
