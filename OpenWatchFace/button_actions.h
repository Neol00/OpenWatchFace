/* ============================================================================
 *  button_actions.h — the extra side pushers on the Snapdragon watches
 *  (BOARD_HAS_EXTRA_BUTTONS). Compiled out entirely when the flag is 0, like
 *  button_nav.h / crown_nav.h; only flashlight_is_on()/flashlight_tick() stay
 *  as inline stubs so the .ino needs no extra #if around the dim/idle gates.
 *
 *  Buttons are addressed by ROLE, not by pin:
 *    BTN_TOP    = the primary pusher (the only one on the TicWatch C2/C2+)
 *    BTN_BOTTOM = the second pusher (Fossil Gen 4)
 *  The board header maps each role to a pin (BTN_TOP_GPIO / BTN_BOTTOM_GPIO,
 *  virtual pins served by compat/arduino_glue.cpp on the Snapdragon port).
 *
 *  REBINDABLE BY DESIGN. What a press does is ONE lookup in s_btn_map
 *  (button x short/long -> action id). The values below are only the defaults;
 *  the Buttons app will call button_action_set() and persist the table, and
 *  nothing else in this file changes.
 *
 *  Press model:
 *    short  = released before BA_LONG_MS   -> fires on RELEASE
 *    long   = still held at BA_LONG_MS     -> fires WHILE HELD (+ a buzz)
 *    wake   = the press that woke the watch: a tap only wakes it, holding on
 *             still reaches the long action (flashlight straight from sleep).
 *  Any press while the flashlight is lit, or the alarm rings, only ends that.
 *
 *  Threading: loop thread only (same thread as lv_task_handler).
 *  Include AFTER quick_shade.h, app_menu.h, app_notifications.h, app_timer.h.
 * ========================================================================== */
#pragma once

#ifndef BOARD_HAS_EXTRA_BUTTONS
#define BOARD_HAS_EXTRA_BUTTONS 0
#endif

#if BOARD_HAS_EXTRA_BUTTONS

/* ---- actions ------------------------------------------------------------- */
/* Action id. 0..BA_APP_FIRST-1 are the built-in specials; BA_APP_FIRST + i is
 * "open launcher app i" (MENU_ITEMS[i] in app_menu.h), so EVERY app the board
 * actually has is bindable and the list differs per device automatically.
 * Persisted ids are validated by a name hash -- see button_actions_load(). */
enum BtnAction : uint8_t {
  BA_NONE = 0,
  BA_FLASHLIGHT,      // full-white screen at full brightness
  BA_QUICK_SHADE,     // pull the quick shade down (or push it back up)
  BA_SPECIAL_COUNT,
  BA_APP_FIRST = 8
};
#define BA_APP_ID(i)   ((uint8_t)(BA_APP_FIRST + (i)))
#define BA_IS_APP(a)   ((a) >= BA_APP_FIRST && (int)((a) - BA_APP_FIRST) < MENU_ITEM_COUNT)
#define BA_APP_IDX(a)  ((int)((a) - BA_APP_FIRST))

enum { BTN_TOP = 0, BTN_BOTTOM = 1, BTN_COUNT = 2 };
enum { BP_SHORT = 0, BP_LONG = 1 };

/* djb2-16 over the app name: what makes a saved binding survive the app list
 * changing shape (a new tile, a different board). */
static uint16_t ba_name_hash(const char *s) {
  uint32_t h = 5381u;
  for (; *s; s++) h = ((h << 5) + h) ^ (uint8_t)*s;
  return (uint16_t)h;
}
static int ba_app_by_name(const char *name) {
  for (int i = 0; i < MENU_ITEM_COUNT; i++)
    if (!strcmp(MENU_ITEMS[i].name, name)) return i;
  return -1;
}
static int ba_app_by_hash(uint16_t h) {
  for (int i = 0; i < MENU_ITEM_COUNT; i++)
    if (ba_name_hash(MENU_ITEMS[i].name) == h) return i;
  return -1;
}
static uint8_t ba_default_app(const char *name, uint8_t fallback) {
  int i = ba_app_by_name(name);
  return i >= 0 ? BA_APP_ID(i) : fallback;
}

static uint8_t s_btn_map[BTN_COUNT][2] = {
  /* short              long          */
  { BA_NONE,            BA_FLASHLIGHT },   // BTN_TOP: short filled in below (Notifications)
  { BA_QUICK_SHADE,     BA_NONE       },   // BTN_BOTTOM: long filled in below (Timer)
};

/* ---- persistence (NVS namespace "watch", opened by settings_load()) -------
 * Keys "btn0s" "btn0l" "btn1s" "btn1l" = button, short/long. Loaded lazily on
 * the first poll / app open, so no setup() change is needed. */
static bool s_btn_loaded = false;
static inline void ba_key(char *k, int btn, int press) {
  k[0] = 'b'; k[1] = 't'; k[2] = 'n'; k[3] = (char)('0' + btn); k[4] = press ? 'l' : 's'; k[5] = 0;
}
static void button_actions_load(void) {
  if (s_btn_loaded) return;
  s_btn_loaded = true;
  /* Defaults that name an app are resolved against THIS board's launcher. */
  s_btn_map[BTN_TOP][BP_SHORT]     = ba_default_app("Notifications", BA_NONE);
  s_btn_map[BTN_BOTTOM][BP_LONG]   = ba_default_app("Timer",         BA_NONE);
  char k[6], kh[7];
  for (int b = 0; b < BTN_COUNT; b++)
    for (int p = 0; p < 2; p++) {
      ba_key(k, b, p);
      if (!prefs.isKey(k)) continue;
      uint8_t v = prefs.getUChar(k, s_btn_map[b][p]);
      if (v < BA_SPECIAL_COUNT) { s_btn_map[b][p] = v; continue; }
      /* An app binding: trust the NAME, not the position. The list differs per
       * board and grows between builds, so a bare index would silently rebind. */
      ba_key(kh, b, p); kh[5] = 'h'; kh[6] = 0;
      uint16_t h = prefs.getUShort(kh, 0);
      int idx = BA_IS_APP(v) && (!h || ba_name_hash(MENU_ITEMS[BA_APP_IDX(v)].name) == h)
                ? BA_APP_IDX(v) : (h ? ba_app_by_hash(h) : -1);
      if (idx >= 0) s_btn_map[b][p] = BA_APP_ID(idx);   // app gone -> keep the default
    }
}

static inline uint8_t button_action_get(int btn, int press) {
  return (btn >= 0 && btn < BTN_COUNT && press >= 0 && press < 2) ? s_btn_map[btn][press] : BA_NONE;
}
static inline void button_action_set(int btn, int press, uint8_t action) {
  if (btn < 0 || btn >= BTN_COUNT || press < 0 || press > 1) return;
  if (action >= BA_SPECIAL_COUNT && !BA_IS_APP(action)) return;
  s_btn_map[btn][press] = action;
  char k[6], kh[7];
  ba_key(k, btn, press);
  prefs.putUChar(k, action);
  ba_key(kh, btn, press); kh[5] = 'h'; kh[6] = 0;
  prefs.putUShort(kh, BA_IS_APP(action) ? ba_name_hash(MENU_ITEMS[BA_APP_IDX(action)].name) : 0);
}

/* ---- flashlight ----------------------------------------------------------
 * A sub-app screen with no menu tile (app_screen_begin on lv_layer_top, like
 * every app), opened as a root screen by ba_open_app(). Tap, BOOT or any
 * pusher turns it off; 5 min auto-off.
 *
 * v478 ROOT CAUSE of the v473-v477 "flicker": flashlight_tick() compared the
 * loop's `ms` (sampled at the TOP of loop()) against s_flash_ms = millis()
 * taken LATER in the same pass by the button handler. ms - s_flash_ms wrapped
 * to ~4 billion, the 5 min auto-off fired on the very first tick, and the
 * screen was deleted right after it was built (log: "flashlight on" then
 * "flashlight off" with nothing in between). The tick now reads millis(). */
#define FLASHLIGHT_MAX_MS  (5UL * 60UL * 1000UL)   // auto-off: a lit panel is the biggest drain there is

static uint32_t s_flash_ms  = 0;
static bool     s_flash_lit = false;   // panel forced to 255 by us (restore owed)

static void app_open_flashlight(void);
static void ba_open_app(screen_fn fn);

static inline bool flashlight_is_on(void) { return app_scr != nullptr && nav_current == app_open_flashlight; }

static void flashlight_restore(void) {
  if (!s_flash_lit) return;
  s_flash_lit = false;
  settings_undim_force();   // back to the user's own brightness, bookkeeping included
}

static void flashlight_off(void) {
  if (flashlight_is_on()) app_menu_close();   // straight back to the watch face
  flashlight_restore();
}

/* A tap deletes the screen that received it, so defer out of the event. */
static void flashlight_off_async(void *p) { (void)p; flashlight_off(); }
static void flashlight_click_cb(lv_event_t *e) { (void)e; lv_async_call(flashlight_off_async, nullptr); }

static void app_open_flashlight(void) {
  lv_obj_t *scr = app_screen_begin("");
  lv_obj_clean(scr);                                     // no title, no BOOT hint: just light
  lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(scr, 0, 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, flashlight_click_cb, LV_EVENT_CLICKED, nullptr);
  s_flash_ms  = millis();
  s_flash_lit = true;
  s_dimmed = false;                                      // panel only: s_brightness is left alone
  s_panel_applied = 255;
  board_display_set_brightness(255);
}

static void flashlight_on(void) {
  if (flashlight_is_on()) return;
  ba_open_app(app_open_flashlight);
}

/* Call from loop() right after the auto-dim step (which is skipped while lit).
 * `ms` is unused on purpose: it is older than s_flash_ms (see the header). */
static void flashlight_tick(uint32_t ms) {
  (void)ms;
  if (!flashlight_is_on()) { flashlight_restore(); return; }
  uint32_t now = millis();
  if ((uint32_t)(now - s_flash_ms) >= FLASHLIGHT_MAX_MS && (int32_t)(now - s_flash_ms) > 0) {
    flashlight_off();
    return;
  }
  /* Pulling the quick shade down (touch drag, crown, or a button) cancels the
   * flashlight: the shade's brightness slider must act on the user's level. */
  if (quick_shade_active()) { flashlight_off(); return; }
  if (s_panel_applied != 255) { s_panel_applied = 255; s_dimmed = false; board_display_set_brightness(255); }
}

typedef struct { const char *name; uint32_t mdi; const char *sym; uint32_t color; } ba_info_t;

/* Name/icon/colour for any action. An APP borrows the launcher tile's own icon
 * and tint, so the dropdown row looks like the app it opens. */
static ba_info_t ba_info(uint8_t a) {
  if (a == BA_FLASHLIGHT)  return ba_info_t{ "Flashlight",  MDI_FLASHLIGHT,          LV_SYMBOL_CHARGE, 0xFFD60A };
  if (a == BA_QUICK_SHADE) return ba_info_t{ "Quick shade", MDI_CHEVRON_DOUBLE_DOWN, LV_SYMBOL_DOWN,   0x33A0FF };
  if (BA_IS_APP(a)) {
    const MenuItem *m = &MENU_ITEMS[BA_APP_IDX(a)];
    return ba_info_t{ m->name, m->icon_cp, m->symbol, m->icon_color };
  }
  return ba_info_t{ "None", MDI_CLOSE_CIRCLE_OUTLINE, LV_SYMBOL_CLOSE, 0x8E8E93 };
}

/* ---- dispatch ------------------------------------------------------------ */
/* Open an app as a ROOT screen, the same state a tap on its menu tile leaves:
 * BOOT then walks back to the menu, then the face. Pressing the button again
 * while that app is showing goes straight back to the watch face. */
static void ba_open_app(screen_fn fn) {
  if (quick_shade_is_open()) quick_shade_force_close();
  if (nav_current == fn && nav_depth == 0) { app_menu_close(); return; }
  app_menu_init();          // lazy-built on first use
  app_menu_close();         // drop whatever was open: a fresh back-history
  notif_reset_page();       // same as menu_tile_cb: a fresh open shows the newest
  nav_open(fn);
}

static void button_action_run(int btn, int press) {
  uint8_t a = button_action_get(btn, press);
  USBSerial.printf("[btn] %s %s -> %s\n", btn == BTN_TOP ? "top" : "bottom",
                   press == BP_LONG ? "long" : "short", ba_info(a).name);
  if (flashlight_is_on()) {
    flashlight_off();
    if (a != BA_QUICK_SHADE) return;   // the shade action still opens the shade
  }
  if (g_alarm_active)     { alarm_dismiss();  return; }
  if (BA_IS_APP(a)) { ba_open_app(MENU_ITEMS[BA_APP_IDX(a)].open); return; }
  switch (a) {
    case BA_FLASHLIGHT:    flashlight_on(); break;
    case BA_QUICK_SHADE:
      if (quick_shade_is_open()) quick_shade_close(); else quick_shade_open();
      break;
    default: break;
  }
}

/* ---- press detection ----------------------------------------------------- */
#define BA_DEBOUNCE_US  1500   // press-edge confirm (same trick as the BOOT key)
#define BA_LONG_MS      600

typedef struct { int pin; bool down, long_fired, from_wake; uint32_t t_down; } ba_btn_t;
static ba_btn_t ba_btn[BTN_COUNT] = {
#ifdef BTN_TOP_GPIO
  { BTN_TOP_GPIO, false, false, false, 0 },
#else
  { -1, false, false, false, 0 },
#endif
#ifdef BTN_BOTTOM_GPIO
  { BTN_BOTTOM_GPIO, false, false, false, 0 },
#else
  { -1, false, false, false, 0 },
#endif
};

static inline bool ba_raw(int pin) { return digitalRead((pin_size_t)pin) == LOW; }

/* Returns true on any press/release edge (real user activity). */
static bool button_actions_poll(uint32_t ms) {
  bool activity = false;
  button_actions_load();
  for (int i = 0; i < BTN_COUNT; i++) {
    ba_btn_t *b = &ba_btn[i];
    if (b->pin < 0) continue;
    bool raw = ba_raw(b->pin);
    if (raw && !b->down) {
      delayMicroseconds(BA_DEBOUNCE_US);
      if (!ba_raw(b->pin)) continue;
      b->down = true; b->long_fired = false; b->from_wake = false; b->t_down = ms;
      activity = true;
    } else if (raw && b->down) {
      if (!b->long_fired && (uint32_t)(ms - b->t_down) >= BA_LONG_MS) {
        b->long_fired = true;
        haptics_pulse(HAPTICS_CLICK_MS);   // "long press registered"
        button_action_run(i, BP_LONG);
        activity = true;
      }
    } else if (!raw && b->down) {
      b->down = false;
      if (!b->long_fired && !b->from_wake) button_action_run(i, BP_SHORT);
      activity = true;
    }
  }
  return activity;
}

/* Call right after a sleep returns. A pusher still held is the press that woke
 * us: releasing it must not also count as a short press, but holding on still
 * reaches the long action, timed from now. */
static void button_actions_after_wake(void) {
  uint32_t now = millis();
  for (int i = 0; i < BTN_COUNT; i++) {
    ba_btn_t *b = &ba_btn[i];
    if (b->pin < 0) continue;
    b->down = ba_raw(b->pin);
    b->long_fired = false;
    b->from_wake = b->down;
    b->t_down = now;
  }
}

/* ============================================================================
 *  Buttons app — one closed dropdown card per button x press. Tapping a card
 *  expands its action list in place (only one open at a time); picking an
 *  action saves it and closes the card. One button (C2/C2+) = 2 cards, two
 *  buttons (Gen 4 / Gen 5 / Sport) = 4 cards.
 * ========================================================================== */
/* The pick list: the specials first, then EVERY app this board has. */
#define BA_OPT_MAX (3 + MENU_ITEM_COUNT)
static uint8_t ba_opt_id[BA_OPT_MAX];
static int     ba_opt_n;
static void ba_build_option_ids(void) {
  ba_opt_n = 0;
  ba_opt_id[ba_opt_n++] = BA_NONE;
  ba_opt_id[ba_opt_n++] = BA_FLASHLIGHT;
  ba_opt_id[ba_opt_n++] = BA_QUICK_SHADE;
  for (int i = 0; i < MENU_ITEM_COUNT && ba_opt_n < BA_OPT_MAX; i++) ba_opt_id[ba_opt_n++] = BA_APP_ID(i);
}

#define BA_SLOTS (BTN_COUNT * 2)
static lv_obj_t *ba_opts[BA_SLOTS];               // expanded option list per slot (hidden when closed)
static lv_obj_t *ba_cur_icon[BA_SLOTS], *ba_cur_name[BA_SLOTS], *ba_chev[BA_SLOTS];
static lv_obj_t *ba_opt_name[BA_SLOTS][BA_OPT_MAX];  // option labels, to mark the current choice

/* MDI glyph if it is in the font, otherwise the LV_SYMBOL at the same size. */
static void ba_set_icon(lv_obj_t *lbl, uint8_t a) {
  ba_info_t in = ba_info(a);
  const lv_font_t *mf = &UI_ICON_MDI(22);
  lv_font_glyph_dsc_t g;
  lv_obj_set_style_text_color(lbl, lv_color_hex(ui_deco_hex(in.color)), 0);
  if (in.mdi && lv_font_get_glyph_dsc(mf, &g, in.mdi, 0)) {
    char u[5];
    lv_obj_set_style_text_font(lbl, mf, 0);
    lv_label_set_text(lbl, mdi_utf8(in.mdi, u));
  } else {
    lv_obj_set_style_text_font(lbl, &UI_ICON_SYM(22), 0);
    lv_label_set_text(lbl, in.sym);
  }
}

static void ba_mark_current(int slot) {
  uint8_t cur = button_action_get(slot / 2, slot % 2);
  for (int i = 0; i < ba_opt_n; i++) {
    if (!ba_opt_name[slot][i]) continue;
    lv_obj_set_style_text_color(ba_opt_name[slot][i],
      ba_opt_id[i] == cur ? lv_color_hex(ui_accent_hex()) : lv_color_white(), 0);
  }
}

static void ba_set_open(int slot, bool open) {
  if (!ba_opts[slot]) return;
  if (open) lv_obj_clear_flag(ba_opts[slot], LV_OBJ_FLAG_HIDDEN);
  else      lv_obj_add_flag(ba_opts[slot], LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(ba_chev[slot], open ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
}

static void ba_card_cb(lv_event_t *e) {
  int slot = (int)(intptr_t)lv_event_get_user_data(e);
  bool was_open = !lv_obj_has_flag(ba_opts[slot], LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < BA_SLOTS; i++) ba_set_open(i, false);
  if (!was_open) {
    ba_set_open(slot, true);
    lv_obj_scroll_to_view(ba_opts[slot], LV_ANIM_ON);
  }
}

static void ba_opt_cb(lv_event_t *e) {
  int code = (int)(intptr_t)lv_event_get_user_data(e);
  int slot = code / 256, a = code % 256;
  button_action_set(slot / 2, slot % 2, (uint8_t)a);
  ba_set_icon(ba_cur_icon[slot], (uint8_t)a);
  lv_label_set_text(ba_cur_name[slot], ba_info((uint8_t)a).name);
  ba_mark_current(slot);
  ba_set_open(slot, false);
}

static void ba_build_slot(lv_obj_t *col, int slot) {
  uint8_t cur = button_action_get(slot / 2, slot % 2);

  // Closed card: [icon] "Short press" / <action>  [chevron]
  lv_obj_t *card = lv_obj_create(col);
  lv_obj_remove_style_all(card);
  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(card, UI_PX(64), 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x2A2A2A), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, UI_PX(14), 0);
  lv_obj_set_style_pad_hor(card, UI_PX(14), 0);
  lv_obj_set_style_pad_ver(card, UI_PX(10), 0);
  lv_obj_set_style_pad_column(card, UI_PX(12), 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, ba_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);

  ba_cur_icon[slot] = lv_label_create(card);
  ba_set_icon(ba_cur_icon[slot], cur);

  lv_obj_t *txt = lv_obj_create(card);
  lv_obj_remove_style_all(txt);
  lv_obj_set_flex_grow(txt, 1);
  lv_obj_set_height(txt, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(txt, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_t *cap = lv_label_create(txt);
  lv_obj_set_style_text_font(cap, &UI_FONT(16), 0);
  lv_obj_set_style_text_color(cap, lv_color_hex(0x8E8E93), 0);
  lv_label_set_text(cap, slot % 2 ? "Long press" : "Short press");
  ba_cur_name[slot] = lv_label_create(txt);
  lv_obj_set_style_text_font(ba_cur_name[slot], &FONT_SMALL, 0);
  lv_obj_set_style_text_color(ba_cur_name[slot], lv_color_white(), 0);
  lv_label_set_text(ba_cur_name[slot], ba_info(cur).name);

  ba_chev[slot] = lv_label_create(card);
  lv_obj_set_style_text_font(ba_chev[slot], &UI_ICON_SYM(22), 0);
  lv_obj_set_style_text_color(ba_chev[slot], lv_color_hex(0x8E8E93), 0);

  // Expanded list (hidden until the card is tapped): [icon] <action name>
  lv_obj_t *opts = lv_obj_create(col);
  lv_obj_remove_style_all(opts);
  ba_opts[slot] = opts;
  lv_obj_set_width(opts, LV_PCT(100));
  lv_obj_set_height(opts, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(opts, lv_color_hex(0x111111), 0);
  lv_obj_set_style_bg_opa(opts, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(opts, UI_PX(14), 0);
  lv_obj_set_style_pad_all(opts, UI_PX(6), 0);
  lv_obj_set_style_pad_row(opts, UI_PX(2), 0);
  lv_obj_set_flex_flow(opts, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(opts, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < ba_opt_n; i++) {
    uint8_t a = ba_opt_id[i];
    lv_obj_t *row = lv_obj_create(opts);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, UI_PX(52), 0);
    lv_obj_set_style_radius(row, UI_PX(10), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2A2A2A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(row, UI_PX(10), 0);
    lv_obj_set_style_pad_column(row, UI_PX(12), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, ba_opt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(slot * 256 + a));
    lv_obj_t *ic = lv_label_create(row);
    ba_set_icon(ic, a);
    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_style_text_font(nm, &FONT_SMALL, 0);
    lv_label_set_text(nm, ba_info(a).name);
    ba_opt_name[slot][i] = nm;
  }
  ba_mark_current(slot);
  ba_set_open(slot, false);
}

static void app_open_buttons(void) {
  button_actions_load();
  app_screen_begin("Buttons");
  ba_build_option_ids();
  for (int i = 0; i < BA_SLOTS; i++) { ba_opts[i] = nullptr; for (int a = 0; a < BA_OPT_MAX; a++) ba_opt_name[i][a] = nullptr; }

  lv_obj_t *col = lv_obj_create(app_scr);
#if BOARD_SCREEN_NARROW
  lv_obj_set_width(col, LV_PCT(92));
  lv_obj_set_height(col, (int)screenHeight - UI_PX(84));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(124));
  lv_obj_set_style_pad_all(col, UI_PX(6), 0);
#else
  ui_app_column_layout(col, 374);    // same geometry as Appearance / Power
#endif
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(col);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(col, UI_PX(10), 0);
  lv_obj_set_style_pad_bottom(col, UI_PX(78), 0);

  int buttons = 0;
  for (int b = 0; b < BTN_COUNT; b++) if (ba_btn[b].pin >= 0) buttons++;
  for (int b = 0; b < BTN_COUNT; b++) {
    if (ba_btn[b].pin < 0) continue;
    lv_obj_t *hdr = lv_label_create(col);
    lv_obj_set_style_text_font(hdr, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(ui_accent_soft_hex()), 0);
    lv_label_set_text(hdr, buttons == 1 ? "SIDE BUTTON" : (b == BTN_TOP ? "TOP BUTTON" : "BOTTOM BUTTON"));
    lv_obj_set_style_pad_top(hdr, UI_PX(b ? 14 : 4), 0);
    ba_build_slot(col, b * 2 + BP_SHORT);
    ba_build_slot(col, b * 2 + BP_LONG);
  }
}

#else  /* !BOARD_HAS_EXTRA_BUTTONS */
/* The flashlight is reached only from a pusher, so it does not exist on a watch
 * without one -- but OpenWatchFace.ino calls these unconditionally, and the
 * stub list has to match that use EXACTLY. flashlight_off() was missing here
 * from v479 until 2026-09-17: the S2 and Gen 5E app TU stopped compiling at
 * OpenWatchFace.ino's BOOT first-tap chain ("'flashlight_off' was not declared
 * in this scope"), and because cc_one()/cxx_one() do not abort the build
 * script, the S2 link silently reused a STALE OpenWatchFace.o and produced an
 * image that looked fine. If you add a flashlight entry point that the .ino
 * calls outside a BOARD_HAS_EXTRA_BUTTONS guard, it needs a stub here too. */
static inline bool flashlight_is_on(void) { return false; }
static inline void flashlight_tick(uint32_t) {}
static inline void flashlight_off(void) {}
#endif
