/* ============================================================================
 *  app_menu.h — App launcher / menu overlay for the watch.
 *
 *  Self-contained module: it only talks to LVGL and exposes a tiny API. The
 *  main sketch decides WHEN to open/close it (e.g. on a BOOT-button press) and
 *  must call app_menu_init() once after lv_init() + display setup.
 *
 *  The menu is a full-screen overlay shown ON TOP of the watch face. While it's
 *  open the watch face keeps running underneath (just covered). Closing it
 *  reveals the clock again.
 *
 *  Apps are placeholders for now (Settings / Notifications / Timer); each opens
 *  a simple sub-screen with a Back button. Fill in real functionality later.
 *
 *  Fonts FONT_LABEL / FONT_SMALL are #defined in the main sketch before this
 *  file is included.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

/* ---- state ---- */
static lv_obj_t *menu_scr   = nullptr;   // the menu overlay (full screen)
static lv_obj_t *app_scr    = nullptr;   // a currently-open sub-app screen
static bool      menu_open  = false;

/* Forward decl: the main sketch provides this so the menu can return focus to
 * the clock (it lives on lv_scr_act()'s default screen). */
static void app_menu_close(void);

/* Create a full-screen sub-app shell (black bg + title + a top-left "BOOT = back"
 * hint) and return it. Body widgets get added by the caller. Hides the menu while
 * open. There is NO on-screen Back button: the hardware BOOT button backs out one
 * level (sub-app -> menu -> clock; see app_menu_back()), which frees the whole
 * bottom of the screen for content. */
static lv_obj_t *app_screen_begin(const char *title) {
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }      // replace current sub-app
  if (menu_scr) lv_obj_add_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);  // hide menu

  app_scr = lv_obj_create(lv_layer_top());
  lv_obj_set_size(app_scr, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(app_scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(app_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(app_scr, 0, 0);
  lv_obj_clear_flag(app_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *t = lv_label_create(app_scr);
  lv_obj_set_style_text_font(t, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_label_set_text(t, title);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

  // Discoverability hint for the hardware Back: a dim arrow + "BOOT" in the top
  // corner. Uses the Montserrat symbol font so LV_SYMBOL_LEFT renders (our
  // systemui font is ASCII-only and would show tofu for the arrow).
  lv_obj_t *hint = lv_label_create(app_scr);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x777777), 0);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " BOOT");
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 16, 14);
  return app_scr;
}

/* Build one labelled switch row (icon + name on the left, switch on the right)
 * inside `parent`. `on` sets the switch's initial state; `cb` fires on toggle
 * (LV_EVENT_VALUE_CHANGED; read the new state off the target with LV_STATE_CHECKED).
 * Returns the switch. Shared by every settings screen (WiFi/BLE, Power, Appearance)
 * so the toggles all look identical. */
static lv_obj_t *settings_toggle_row(lv_obj_t *parent, const char *symbol,
                                     const char *name, bool on, lv_event_cb_t cb) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));   // fill the column it's dropped into (WiFi/BLE, Power, ...)
  lv_obj_set_height(row, 60);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 14, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ic = lv_label_create(row);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ic, lv_color_white(), 0);
  lv_label_set_text(ic, symbol);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 20, 0);

  lv_obj_t *nm = lv_label_create(row);
  lv_obj_set_style_text_font(nm, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_label_set_text(nm, name);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 60, 0);

  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -16, 0);
  lv_obj_set_style_bg_color(sw, lv_color_hex(ui_accent_hex()), LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  return sw;
}

/* ---------- sub-app entry points (each in its own module) ----------
 * Every Settings sub-app and the Notifications app now live in their own header
 * (app_power.h / app_wifi_ble.h / app_settings.h / app_notifications.h), all
 * included by WatchFace.ino AFTER this one so they can
 * use the screen shell above. These forward declarations let the menu tiles and
 * the Settings list dispatch to them regardless of include order. */
static void app_open_power(void);
static void app_open_appearance(void);
static void app_open_wifi_ble(void);
static void app_open_notifications(void);
static void app_open_player(void);
static void notif_reset_page(void);      // defined in app_notifications.h; rewinds the list to page 0
static void app_open_about(void);
static void app_open_timer(void);
static void app_open_stopwatch(void);
static void app_open_find_phone(void);
static void app_open_files(void);

/* ---------- back-navigation stack ----------
 * Each full-screen sub-app is built by a `screen_fn` (calls app_screen_begin()).
 * Because only one app_scr exists at a time, opening a deeper screen destroys the
 * one behind it — so to make BOOT go back ONE level (e.g. Power -> Settings,
 * not Power -> menu) we remember the builders we navigated through.
 *
 * nav_open(fn)  forward navigation: remembers the current screen, then builds fn.
 * nav_back()    pops and rebuilds the previous screen, or reveals the menu at root.
 * Plain app_open_X() calls (in-place refreshes after an edit) bypass this and just
 * rebuild the current screen, leaving the stack untouched — which is what we want. */
typedef void (*screen_fn)(void);
#define NAV_STACK_MAX 8
static screen_fn nav_stack[NAV_STACK_MAX];   // builders of the screens behind the current one
static int       nav_depth   = 0;
static screen_fn nav_current = nullptr;       // builder of the on-screen sub-app (null = at menu/clock)

static void nav_open(screen_fn fn) {
  if (nav_current && nav_depth < NAV_STACK_MAX)
    nav_stack[nav_depth++] = nav_current;     // remember the screen we're leaving
  nav_current = fn;
  fn();                                       // app_screen_begin() inside replaces app_scr
}

static void nav_back(void) {
  if (nav_depth > 0) {                        // deeper than a root sub-app -> previous screen
    nav_current = nav_stack[--nav_depth];
    nav_current();                            // rebuild it (app_screen_begin replaces app_scr)
  } else {                                    // root sub-app -> reveal the menu
    nav_current = nullptr;
    if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
    if (menu_scr) lv_obj_clear_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);
    menu_open = true;
  }
}

/* ---------- menu tiles (flat 3-column app grid; no Settings folder) ----------
 * The old Settings sub-menu is gone: its screens (Appearance/Power/WiFi&BLE/About)
 * are now first-class tiles here, each one tap away. Each tile carries the screen
 * it opens and a fixed icon color. */
struct MenuItem {
  const char *name;
  const char *symbol;       // LVGL built-in symbol
  uint32_t    icon_color;   // fixed per-app icon tint (label stays white)
  screen_fn   open;         // screen this tile opens
};
static const MenuItem MENU_ITEMS[] = {
  { "Notifications", LV_SYMBOL_BELL,         0xFF3030, app_open_notifications },
  { "Timer",         LV_SYMBOL_LOOP,         0xFF99FF, app_open_timer },
  { "Appearance",    LV_SYMBOL_TINT,         0xFF9F0A, app_open_appearance },
  { "Power",         LV_SYMBOL_BATTERY_FULL, 0x32D74B, app_open_power },
  { "WiFi & BLE",    LV_SYMBOL_WIFI,         0x33A0FF, app_open_wifi_ble },
  { "Player",        LV_SYMBOL_AUDIO,        0xFF375F, app_open_player },
  { "Find Phone",    LV_SYMBOL_CALL,         0x00C2A8, app_open_find_phone },
  { "Files",         LV_SYMBOL_DRIVE,        0x9B8CFF, app_open_files },
  { "About",         LV_SYMBOL_LIST,         0xFFFF80, app_open_about },
};
static const int MENU_ITEM_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

static void menu_tile_cb(lv_event_t *e) {
  const MenuItem *it = (const MenuItem *)lv_event_get_user_data(e);
  // Opening from the menu starts a fresh back-history (the menu is the root).
  // Rewind the notifications list to page 0 so a fresh open always shows the
  // newest (it's preserved across in-app rebuilds like dismiss / reader-back).
  notif_reset_page();
  if (it && it->open) nav_open(it->open);
}

/* Build the menu overlay (created hidden; shown via app_menu_open). A title at
 * the top, a scrollable 3-column grid of app tiles (icon + label), and a BOOT
 * hint pinned at the bottom. The grid wraps + scrolls, so it grows past 9 apps. */
static void app_menu_init(void) {
  menu_scr = lv_obj_create(lv_layer_top());     // top layer => above watch face
  lv_obj_set_size(menu_scr, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(menu_scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(menu_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(menu_scr, 0, 0);
  lv_obj_clear_flag(menu_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(menu_scr);
  lv_obj_set_style_text_font(title, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_label_set_text(title, "Apps");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  // Scrollable 3-up grid (flex wrap).
  lv_obj_t *grid = lv_obj_create(menu_scr);
  lv_obj_set_size(grid, 392, 430);
  lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 46);     // start higher (uses the top clearance)
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 4, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 14, 0);
  lv_obj_set_style_pad_column(grid, 12, 0);

  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    lv_obj_t *tile = lv_btn_create(grid);
    lv_obj_set_size(tile, 104, 104);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(tile, 18, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tile, menu_tile_cb, LV_EVENT_CLICKED,
                        (void *)&MENU_ITEMS[i]);

    // Big colored icon up top (Montserrat has the symbol glyphs; our systemui
    // font is ASCII-only and would show tofu).
    lv_obj_t *icon = lv_label_create(tile);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_34, 0);
    // Per-app tile color, unified to the accent in monochrome-accent mode.
    lv_obj_set_style_text_color(icon, lv_color_hex(ui_deco_hex(MENU_ITEMS[i].icon_color)), 0);
    lv_label_set_text(icon, MENU_ITEMS[i].symbol);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 18);

    // Small caption pinned to the bottom of the tile — well below the icon, in the
    // tile's lower (background) area. The small font fits even "Notifications" on
    // one line, so no wrapping onto the icon.
    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_width(name, 100);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(name, MENU_ITEMS[i].name);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -12);
  }

  // No on-screen close button — BOOT toggles menu<->clock and backs out of apps
  // (see app_menu_back() / the loop's BOOT handler).
  lv_obj_t *hint = lv_label_create(menu_scr);
  lv_obj_set_style_text_font(hint, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " BOOT");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

  lv_obj_add_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);   // start hidden
}

/* Recolor the menu tile icons live from the current accent/mono-mode, WITHOUT
 * rebuilding the menu (it's built once and hidden/shown, never rebuilt). Called when
 * the accent or monochrome toggle changes so the grid updates immediately instead of
 * only after a reboot. Walks: menu_scr -> grid (child 1) -> each tile -> icon (its
 * first child, created before the name label in app_menu_init). Null/shape-safe. */
static void menu_restyle(void) {
  if (!menu_scr) return;
  // The grid is the title's sibling: find it as the first scrollable-flex container.
  // app_menu_init adds title (child 0), grid (child 1), hint (child 2).
  if (lv_obj_get_child_count(menu_scr) < 2) return;
  lv_obj_t *grid = lv_obj_get_child(menu_scr, 1);
  if (!grid) return;
  uint32_t tiles = lv_obj_get_child_count(grid);
  for (uint32_t i = 0; i < tiles && i < (uint32_t)MENU_ITEM_COUNT; i++) {
    lv_obj_t *tile = lv_obj_get_child(grid, i);
    if (!tile || lv_obj_get_child_count(tile) == 0) continue;
    lv_obj_t *icon = lv_obj_get_child(tile, 0);   // icon is the tile's first child
    if (icon)
      lv_obj_set_style_text_color(icon, lv_color_hex(ui_deco_hex(MENU_ITEMS[i].icon_color)), 0);
  }
  // The grid's appearance just changed -> the cached snapshot is stale. Drop it so
  // the next open re-blits nothing (falls through to a live render) and re-captures.
  screen_cache_invalidate(SC_SLOT_MENU);
}

/* ---------- open/close API used by the main sketch ---------- */
static void app_menu_open(void) {
  if (!menu_scr) return;
  // The menu is the root of navigation: entering it clears any back-history.
  nav_current = nullptr;
  nav_depth   = 0;
  lv_obj_clear_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(menu_scr);
  menu_open = true;

  // PSRAM pre-render: the launcher grid is fully static (built once, only restyled
  // on accent change). If we have a cached snapshot, blit it straight to the panel
  // NOW so the menu appears instantly — no flex layout / 9-tile rasterization on the
  // open frame. LVGL still re-renders the (identical) menu on the next lv_task_handler
  // and that flush overwrites with pixel-identical content, so there's no flicker, but
  // the *first* thing the user sees is the instant blit. If not yet cached, this is a
  // no-op and the normal render paints it; we capture below once it's on screen.
  screen_cache_blit(SC_SLOT_MENU);
}

/* Capture the rendered launcher grid into its PSRAM cache slot. Call AFTER the menu
 * has actually rendered at least once (so the snapshot is complete). Cheap to call
 * repeatedly — it just refreshes the slot; we gate on validity so it snapshots only
 * when needed (first open, or after a restyle invalidated it). */
static inline void app_menu_cache_capture(void) {
  // Only snapshot the BARE launcher grid: menu shown AND no sub-app overlaying it
  // (app_menu_is_open() is also true inside a sub-app — we must not capture then).
  if (menu_scr && menu_open && app_scr == nullptr && !screen_cache_valid(SC_SLOT_MENU))
    screen_cache_capture(SC_SLOT_MENU, menu_scr);
}

static void app_menu_close(void) {
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  if (menu_scr) lv_obj_add_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);
  nav_current = nullptr;
  nav_depth   = 0;
  menu_open = false;
  // Back on the clock face: reflect any notifications read/dismissed in the app
  // immediately, rather than waiting for the loop's 20 s bell poll.
  watchface_refresh_bell();
}

static bool app_menu_is_open(void) {
  return menu_open || app_scr != nullptr;
}

/* BOOT-button navigation: one level "back" each press.
 *   - In a sub-app (e.g. Power)  -> previous screen (Settings, not the menu)
 *   - On a root sub-app (e.g. Settings) -> back to the menu
 *   - On the menu                     -> back to the clock face
 *   - On the clock face               -> open the menu
 * This replaces the on-screen Close button. */
static void app_menu_back(void) {
  if (app_scr) {                          // in a sub-app -> pop one level
    nav_back();
  } else if (menu_open) {                 // on menu -> back to clock
    app_menu_close();
  } else {                                // on clock -> open menu
    app_menu_open();
  }
}
