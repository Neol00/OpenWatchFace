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

/* The launcher is a horizontal swipe PAGER: each page is a MENU_COLS x MENU_ROWS
 * grid of tiles; you swipe between pages (scroll-snap), and the page count grows
 * automatically as apps are added. All of this is per-board:
 *   - MENU_COLS / MENU_ROWS : the page grid. S3-2.06 = 3x4 (12/page, the app set
 *     fits one page); C6-1.47 = 3x1 (3/page -> 9 apps = 3 swipe pages).
 *   - MENU_TILE_GAP         : px between tiles.
 *   - MENU_TILE_ICON_FONT   : tile icon size (the watch-face icons are separate and
 *     stay large; the menu tile icon shrinks on the small C6 panel).
 * The tile BOX size is computed from the real screen width at build time (see
 * app_menu_init) so exactly MENU_COLS fit across — no hard-coded tile pixels that
 * could overflow a narrow panel. */
#if BOARD_SCREEN_NARROW
#define MENU_COLS           1
#define MENU_ROWS           3
#define MENU_TILE_GAP       10
#define MENU_TILE_ICON_FONT lv_font_montserrat_34
#define MENU_HINT_FONT      lv_font_montserrat_10   // "< BOOT" back hint (app screens + menu)
#else
/* C6-1.47 is a TALL portrait panel: lay each page out as ONE COLUMN of 3 tiles
 * stacked vertically (top/middle/bottom), and swipe horizontally for the next 3.
 * 9 apps -> 3 pages. Tiles are full-width and big. */
#define MENU_COLS           3
#define MENU_ROWS           3
#define MENU_TILE_GAP       12
#define MENU_TILE_ICON_FONT lv_font_montserrat_34
#define MENU_HINT_FONT      lv_font_montserrat_20
#endif
#define MENU_TILES_PER_PAGE (MENU_COLS * MENU_ROWS)
#define MENU_PAGE_COUNT  ((MENU_ITEM_COUNT + MENU_TILES_PER_PAGE - 1) / MENU_TILES_PER_PAGE)

static lv_obj_t *menu_pager  = nullptr;  // the horizontal scroll-snap container of pages
static lv_obj_t *menu_dots   = nullptr;  // page-indicator dots row
static int       menu_page   = 0;        // last-shown page (remembered across open/close)

/* Forward decl: the main sketch provides this so the menu can return focus to
 * the clock (it lives on lv_scr_act()'s default screen). */
static void app_menu_close(void);

/* Optional per-app BOOT-back interceptor. An app (e.g. Files) sets this while it's
 * open so a BOOT press first navigates WITHIN the app (dir up -> volume -> volumes
 * chooser). It returns true if it consumed the press; false -> fall through to the
 * normal nav_back() (leave the app). app_screen_begin() clears it on every new
 * screen, so an app must re-arm it each time it builds, and leaving the app drops it.
 * Declared here (above app_screen_begin) so the shell can reset it. */
static bool (*nav_back_intercept)(void) = nullptr;

/* Create a full-screen sub-app shell (black bg + title + a top-left "BOOT = back"
 * hint) and return it. Body widgets get added by the caller. Hides the menu while
 * open. There is NO on-screen Back button: the hardware BOOT button backs out one
 * level (sub-app -> menu -> clock; see app_menu_back()), which frees the whole
 * bottom of the screen for content. */
static lv_obj_t *app_screen_begin(const char *title) {
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }      // replace current sub-app
  if (menu_scr) lv_obj_add_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);  // hide menu
  nav_back_intercept = nullptr;   // each screen re-arms its own BOOT-back hook if it wants one
  
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
#if BOARD_SCREEN_NARROW
  // Narrow panels: a long centered title (e.g. "Find Phone") runs horizontally
  // into the top-left "< BOOT" hint. Drop the title BELOW the hint row so they
  // never share a horizontal band, and let it wrap centered if still too wide.
  lv_obj_set_width(t, LV_PCT(100));
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  // Sit the title well below the BOOT hint row (hint top UI_PX(14) + its line
  // height). UI_PX(44) still left the title's top touching the hint's bottom.
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, UI_PX(72));
#else
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, UI_PX(40));
#endif

  // Discoverability hint for the hardware Back: a dim arrow + "BOOT" in the top
  // corner. Uses the Montserrat symbol font so LV_SYMBOL_LEFT renders (our
  // systemui font is ASCII-only and would show tofu for the arrow).
  lv_obj_t *hint = lv_label_create(app_scr);
  lv_obj_set_style_text_font(hint, &MENU_HINT_FONT, 0);   // per-board (small on C6)
  lv_obj_set_style_text_color(hint, lv_color_hex(0x777777), 0);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " BOOT");
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, UI_PX(16), UI_PX(14));
  return app_scr;
}

/* Build one labelled switch row (icon + name on the left, switch on the right)
 * inside `parent`. `on` sets the switch's initial state; `cb` fires on toggle
 * (LV_EVENT_VALUE_CHANGED; read the new state off the target with LV_STATE_CHECKED).
 * Returns the switch. Shared by every settings screen (WiFi/BLE, Power, Appearance)
 * so the toggles all look identical. */
static lv_obj_t *settings_toggle_row(lv_obj_t *parent, const char *symbol,
                                     const char *name, bool on, lv_event_cb_t cb) {
#if BOARD_SCREEN_NARROW
  /* ---- C6-1.47 (narrow) layout ----
   * The S3 row used fixed pixel offsets (icon @20, name @60, full-size switch @right)
   * authored for a 410 px-wide panel; on the 172 px C6 the name was crammed off the
   * row. Rebuild as a TRANSPARENT flex row holding TWO things side by side:
   *   [ card: icon + name ]  [ switch ]
   * The switch lives OUTSIDE the dark card (its own element to the right) and is kept
   * BIG so it's easy to press; the card grows to fill the rest and the name wraps.
   * (S3 keeps its original single-card pixel layout below.) */
  // Outer container is a COLUMN: the icon+label card on TOP, the switch on its OWN
  // row UNDERNEATH (wrapped to a new "level"). This way the switch never eats into
  // the label's width — the label gets the full row, the switch sits below it.
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(row, UI_PX(8), 0);        // gap between the card and the switch below it

  // The dark card: icon + name only (the switch is NOT inside it anymore).
  lv_obj_t *card = lv_obj_create(row);
  lv_obj_remove_style_all(card);
  lv_obj_set_width(card, LV_PCT(100));               // full width — label uses the whole row
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(card, UI_PX(56), 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, UI_PX(14), 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_hor(card, UI_PX(12), 0);
  lv_obj_set_style_pad_ver(card, UI_PX(8), 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(card, UI_PX(10), 0);

  lv_obj_t *ic = lv_label_create(card);
  lv_obj_set_style_text_font(ic, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(ic, lv_color_white(), 0);
  lv_label_set_text(ic, symbol);

  lv_obj_t *nm = lv_label_create(card);
  // Slightly bigger than FONT_SMALL (montserrat_10) now that the label owns the full
  // row (the switch moved to its own line below). C6-only — this whole branch is
  // inside #if BOARD_SCREEN_NARROW, so the S3 layout is unaffected.
  lv_obj_set_style_text_font(nm, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_obj_set_flex_grow(nm, 1);
  lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
  lv_label_set_text(nm, name);

  // The switch: a BIG, easy-to-press element on its OWN row BELOW the card.
  // FIXED pixel size (NOT UI_PX): UI_PX would scale this down to ~27x14 px on the
  // narrow C6 panel — the exact "tiny toggle" problem. A switch is a touch target,
  // not body text, so it should stay a comfortable absolute size on a small screen.
  lv_obj_t *sw = lv_switch_create(row);
  lv_obj_set_size(sw, 64, 34);                       // large hit target (absolute px)
  lv_obj_set_style_bg_color(sw, lv_color_hex(ui_accent_hex()), LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  return sw;
#else
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
#endif
}

/* ---------- sub-app entry points (each in its own module) ----------
 * Every Settings sub-app and the Notifications app now live in their own header
 * (app_power.h / app_wifi_ble.h / app_settings.h / app_notifications.h), all
 * included by OpenWatchFace.ino AFTER this one so they can
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

/* Build ONE app tile (icon + caption) into `parent`, sized w x h. The caller sizes
 * tiles so MENU_COLS fit across and MENU_ROWS fit down. The icon FONT is the
 * per-board MENU_TILE_ICON_FONT. Layout adapts to the tile shape:
 *   - WIDE tile (w > h, e.g. the C6's full-width column rows): icon on the LEFT,
 *     name to its RIGHT (a list row).
 *   - SQUARE-ish tile (the S3 grid): icon on top, name underneath. */
static void menu_build_tile(lv_obj_t *parent, const MenuItem *item, int w, int h) {
  lv_obj_t *tile = lv_btn_create(parent);
  lv_obj_set_size(tile, w, h);
  lv_obj_set_style_bg_color(tile, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_radius(tile, (h < w ? h : w) / 6, 0);
  lv_obj_set_style_shadow_width(tile, 0, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(tile, menu_tile_cb, LV_EVENT_CLICKED, (void *)item);

  if (w > h) {
    // WIDE list row: icon left, name right, via a flex row (no pre-layout width math).
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(tile, w / 8, 0);
    lv_obj_set_style_pad_column(tile, w / 12, 0);

    lv_obj_t *icon = lv_label_create(tile);
    lv_obj_set_style_text_font(icon, &MENU_TILE_ICON_FONT, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(ui_deco_hex(item->icon_color)), 0);
    lv_label_set_text(icon, item->symbol);

    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_style_text_font(name, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_label_set_text(name, item->name);
  } else {
    // SQUARE tile: icon on top, name underneath (the S3 grid look).
    lv_obj_t *icon = lv_label_create(tile);
    lv_obj_set_style_text_font(icon, &MENU_TILE_ICON_FONT, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(ui_deco_hex(item->icon_color)), 0);
    lv_label_set_text(icon, item->symbol);

    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(name, item->name);

#if BOARD_SCREEN_NARROW
    // C6: scale the icon/name placement to the small tile.
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, h / 6);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_10, 0);
    lv_obj_set_width(name, w - 4);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -(h / 12));
#else
    // S3-2.06: the ORIGINAL fixed look — montserrat_34 icon @ +18 from the top,
    // montserrat_12 caption pinned 12 px up from the bottom, 100 px wide. (Restored
    // verbatim from the pre-refactor menu so the grid looks exactly as before.)
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
    lv_obj_set_width(name, 100);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -12);
#endif
  }
}

/* Light up the page dot for `page`, dim the rest. Null-safe. */
static void menu_update_dots(int page) {
  if (!menu_dots) return;
  uint32_t n = lv_obj_get_child_count(menu_dots);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *dot = lv_obj_get_child(menu_dots, i);
    lv_obj_set_style_bg_color(dot,
        lv_color_hex((int)i == page ? 0xFFFFFF : 0x444444), 0);
  }
}

/* Scroll-snap landed on a new page -> remember it + update the dots. */
static void menu_pager_scroll_cb(lv_event_t *e) {
  (void)e;
  if (!menu_pager) return;
  // The snapped page = horizontal scroll offset / page width (rounded).
  int pw = lv_obj_get_width(menu_pager);
  if (pw <= 0) return;
  int sx = lv_obj_get_scroll_x(menu_pager);
  int p  = (sx + pw / 2) / pw;
  if (p < 0) p = 0;
  if (p >= MENU_PAGE_COUNT) p = MENU_PAGE_COUNT - 1;
  if (p != menu_page) { menu_page = p; menu_update_dots(p); }
}

/* Build the menu overlay (created hidden; shown via app_menu_open). A title at the
 * top, a horizontal SWIPE PAGER of app tiles (MENU_TILES_PER_PAGE per page, scroll-
 * snap), page dots above the BOOT hint. Pages are added automatically as apps grow
 * past one page. Per-board tiles-per-page keeps the tiles full size on every panel. */
static void app_menu_init(void) {
  if (menu_scr) return;                         // lazy + idempotent: build only once
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
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_PX(8));

  // The PAGER: a horizontal scroll-snap row that fills the area between the title
  // and the dots. Each child is one full-screen-width PAGE holding a wrapped grid.
  // Pager height is EXACTLY the area between the title and the dots row — NOT
  // LV_PCT(100), which (aligned at y=UI_PX(46)) overflowed the bottom and pushed
  // the centered tiles down OVER the page dots. page_h (computed below, same value)
  // reserves UI_PX(56) at the bottom for the dots + BOOT hint, so the tiles now
  // stop above them and the dots sit cleanly underneath the last tile.
  // Reserve a fixed strip at the very bottom for the page dots, and give the pager
  // the rest. The dots live in that strip — BELOW the pager — so they can never
  // overlap a tile, and the tiles get the full remaining height (no shrinking).
  const int qs_dots_strip = 22;                 // bottom strip height for the page dots
  const int qs_pager_top  = UI_PX(40);
  const int qs_pager_h    = (int)screenHeight - qs_pager_top - qs_dots_strip;
  menu_pager = lv_obj_create(menu_scr);
  lv_obj_remove_style_all(menu_pager);
  lv_obj_set_width(menu_pager, LV_PCT(100));
  lv_obj_set_height(menu_pager, qs_pager_h);
  lv_obj_align(menu_pager, LV_ALIGN_TOP_MID, 0, qs_pager_top);
  lv_obj_set_flex_flow(menu_pager, LV_FLEX_FLOW_ROW);
  lv_obj_set_scroll_snap_x(menu_pager, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_scrollbar_mode(menu_pager, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_dir(menu_pager, LV_DIR_HOR);
  lv_obj_add_event_cb(menu_pager, menu_pager_scroll_cb, LV_EVENT_SCROLL_END, nullptr);

  // Tile box size from the real panel: width fits MENU_COLS across, height fits
  // MENU_ROWS down within the page area, each with MENU_TILE_GAP between + a side
  // margin. (On the C6: 1 col x 3 rows -> wide, tall full-width rows. On the S3:
  // 3 cols x 4 rows -> square-ish tiles.) Computed live, so it fits any board.
  const int side_pad   = MENU_TILE_GAP;
  const int page_w     = (int)screenWidth;
  const int page_h     = qs_pager_h;   // the pager's actual height (tiles fit within it)
  // Tiles are SQUARE: side = the largest that fits BOTH the per-column width and the
  // per-row height, so MENU_COLS x MENU_ROWS squares fit without distortion. The grid
  // is then centered in the page (flex center), so a 1-column layout is a centered
  // vertical stack of squares.
  // Subtract the page's pad_all (side_pad) AND an extra safety margin so MENU_COLS
  // tiles RELIABLY fit across without the flex ROW_WRAP wrapping one to the next line
  // (the bug where COLS=3 only showed 2). Integer rounding + scroll-snap slack made a
  // tight fit overflow by a pixel or two and wrap; the -MENU_TILE_GAP slack prevents it.
  int avail_w = page_w - 2 * side_pad - MENU_TILE_GAP;   // usable width inside the page
  int avail_h = page_h - 2 * side_pad - MENU_TILE_GAP;   // usable height inside the page
  int fit_w = (avail_w - (MENU_COLS - 1) * MENU_TILE_GAP) / MENU_COLS;
  int fit_h = (avail_h - (MENU_ROWS - 1) * MENU_TILE_GAP) / MENU_ROWS;
  int tile_sz = (fit_w < fit_h) ? fit_w : fit_h;   // square side = the limiting axis
  if (tile_sz < 32) tile_sz = 32;
#if !BOARD_SCREEN_NARROW
  // S3-2.06: the ORIGINAL menu used FIXED 104x104 tiles. Computing the size from the
  // tall pager made them far bigger than 104, so MENU_COLS no longer fit across (3
  // cols wrapped to 2). Pin the S3 back to the original 104 — but cap to fit_w so a
  // higher MENU_COLS still fits the panel width. (C6 keeps its computed size.)
  tile_sz = (fit_w < 104) ? fit_w : 104;
#endif
  int tile_w = tile_sz, tile_h = tile_sz;

  for (int pg = 0; pg < MENU_PAGE_COUNT; pg++) {
    lv_obj_t *page = lv_obj_create(menu_pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
#if MENU_COLS == 1
    // C6 single column: a real COLUMN flex (not ROW_WRAP). Main axis = vertical, so
    // LV_FLEX_ALIGN_START TOP-aligns the stack — all vertical slack collects at the
    // bottom, leaving clear space below the last tile for the dots. Cross axis
    // (horizontal) centered so the squares sit mid-screen.
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
#else
    // Multi-column grids (S3): wrapping row, columns CENTERED horizontally, but the
    // ROWS TOP-aligned (3rd arg = track cross-place = START) so the grid hugs the top
    // like the original menu — not vertically centered, which left a big gap under the
    // "Apps" title and crammed the last row against the BOOT hint.
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
#endif
    lv_obj_set_style_pad_row(page, MENU_TILE_GAP, 0);
    lv_obj_set_style_pad_column(page, MENU_TILE_GAP, 0);
    lv_obj_set_style_pad_all(page, side_pad, 0);

    int first = pg * MENU_TILES_PER_PAGE;
    int last  = first + MENU_TILES_PER_PAGE;
    if (last > MENU_ITEM_COUNT) last = MENU_ITEM_COUNT;
    for (int i = first; i < last; i++)
      menu_build_tile(page, &MENU_ITEMS[i], tile_w, tile_h);
  }

  // Page-indicator dots (only meaningful with >1 page, but harmless with one).
  menu_dots = lv_obj_create(menu_scr);
  lv_obj_remove_style_all(menu_dots);
  lv_obj_set_size(menu_dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(menu_dots, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(menu_dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(menu_dots, UI_PX(8), 0);
  if (MENU_PAGE_COUNT > 1) {
    for (int i = 0; i < MENU_PAGE_COUNT; i++) {
      lv_obj_t *dot = lv_obj_create(menu_dots);
      lv_obj_remove_style_all(dot);
      lv_obj_set_size(dot, UI_PX(8), UI_PX(8));
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(dot, lv_color_hex(i == 0 ? 0xFFFFFF : 0x444444), 0);
    }
  }
  // Align AFTER the dots are added: the row is LV_SIZE_CONTENT, so its size is only
  // known once it has children. Aligning before (size 0) put it in the wrong place.
  // Anchor 8px BELOW the bottom edge, in the strip reserved below the pager.
  lv_obj_align(menu_dots, LV_ALIGN_BOTTOM_MID, 0, 8);

  // TEMP diag: print the REAL post-layout coords of the last tile vs the dots.
  lv_obj_update_layout(menu_scr);
  {
    lv_obj_t *pg0 = lv_obj_get_child(menu_pager, 0);
    int tiles = pg0 ? lv_obj_get_child_count(pg0) : 0;
    lv_obj_t *lastTile = (pg0 && tiles) ? lv_obj_get_child(pg0, tiles - 1) : nullptr;
    int tile_y = lastTile ? lv_obj_get_y(lastTile) : -1;
    int tile_h = lastTile ? lv_obj_get_height(lastTile) : -1;
    int page_abs_y = pg0 ? lv_obj_get_y(pg0) : -1;
    int pager_y = lv_obj_get_y(menu_pager);
    int pager_h = lv_obj_get_height(menu_pager);
    int dots_y = lv_obj_get_y(menu_dots);
    int dots_h = lv_obj_get_height(menu_dots);
    USBSerial.printf("[menudiag2] pager y=%d h=%d -> bottom=%d | lastTile y=%d h=%d "
                     "(abs bottom~%d) | dots y=%d h=%d\n",
                     pager_y, pager_h, pager_y + pager_h,
                     tile_y, tile_h, pager_y + tile_y + tile_h,
                     dots_y, dots_h);
  }

  // No on-screen close button — BOOT toggles menu<->clock and backs out of apps.
  lv_obj_t *hint = lv_label_create(menu_scr);
  lv_obj_set_style_text_font(hint, &MENU_HINT_FONT, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " BOOT");
#if BOARD_HAS_PSRAM
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, UI_PX(-10));   // S3: bottom-center
#else
  // C6: the bottom row of the page grid reaches the bottom edge, so a bottom hint
  // overlaps the last app. Put it in the top-left corner instead (out of the grid).
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, UI_PX(8), UI_PX(8));
#endif

  lv_obj_add_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);   // start hidden
}

/* Recolor every tile's icon live from the current accent/mono-mode, WITHOUT
 * rebuilding the menu. Walks pager -> each page -> each tile -> icon (tile's first
 * child). Tracks a running MENU_ITEMS index across pages so each icon gets its
 * matching color. Null/shape-safe. */
static void menu_restyle(void) {
  if (!menu_pager) return;
  int idx = 0;
  uint32_t pages = lv_obj_get_child_count(menu_pager);
  for (uint32_t p = 0; p < pages; p++) {
    lv_obj_t *page = lv_obj_get_child(menu_pager, p);
    if (!page) continue;
    uint32_t tiles = lv_obj_get_child_count(page);
    for (uint32_t t = 0; t < tiles && idx < MENU_ITEM_COUNT; t++, idx++) {
      lv_obj_t *tile = lv_obj_get_child(page, t);
      if (!tile || lv_obj_get_child_count(tile) == 0) continue;
      lv_obj_t *icon = lv_obj_get_child(tile, 0);   // icon is the tile's first child
      if (icon)
        lv_obj_set_style_text_color(icon,
            lv_color_hex(ui_deco_hex(MENU_ITEMS[idx].icon_color)), 0);
    }
  }
}

/* ---------- open/close API used by the main sketch ---------- */
static void app_menu_open(void) {
  app_menu_init();              // lazy-build on first open (deferred from boot for fast wake)
  if (!menu_scr) return;
  // The menu is the root of navigation: entering it clears any back-history.
  nav_current = nullptr;
  nav_depth   = 0;
  lv_obj_clear_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(menu_scr);
  menu_open = true;

  // Restore the last-viewed page (the pager remembers where you were). Done after
  // unhiding so the layout is valid for the scroll. No cache: the menu is a live
  // swipe pager now, so it renders fresh each open (one frame of flex layout).
  if (menu_pager && MENU_PAGE_COUNT > 1) {
    lv_obj_update_layout(menu_pager);
    int pw = lv_obj_get_width(menu_pager);
    lv_obj_scroll_to_x(menu_pager, menu_page * pw, LV_ANIM_OFF);
    menu_update_dots(menu_page);
  }
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
    if (nav_back_intercept && nav_back_intercept()) return;  // app handled BOOT itself
    nav_back();
  } else if (menu_open) {                 // on menu -> back to clock
    app_menu_close();
  } else {                                // on clock -> open menu
    app_menu_open();
  }
}
