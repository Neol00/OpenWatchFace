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

/* Defined in the .ino: drains any in-flight async display-flush DMA so the SPI bus + draw
 * buffers are idle. No-op on boards without the async path. Declared board-neutrally here
 * (sd_card.h only declares it on the C6) so app_screen_begin can fence before tearing down a
 * screen. */
void display_bus_drain(void);

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

#if MENU_COLS > 1
/* Grid track descriptors for the multi-column launcher pages (see app_menu_init).
 * FILE SCOPE ON PURPOSE: lv_obj_set_grid_dsc_array() stores the POINTER and reads
 * it on every layout pass — it does not copy — so these must outlive the function
 * that installs them. Every page uses the same MENU_COLS x MENU_ROWS geometry, so
 * one shared pair serves them all.
 * int32_t is the API's own parameter type (const int32_t[]); on LVGL v9 lv_coord_t
 * is the same, but spelling it this way avoids depending on that alias.
 * Each track is LV_GRID_CONTENT: size to the tile placed in it. The array is one
 * longer than the track count for the LV_GRID_TEMPLATE_LAST terminator. */
static int32_t menu_grid_cols[MENU_COLS + 1];
static int32_t menu_grid_rows[MENU_ROWS + 1];
static void menu_grid_dsc_init(void) {
  for (int i = 0; i < MENU_COLS; i++) menu_grid_cols[i] = LV_GRID_CONTENT;
  for (int i = 0; i < MENU_ROWS; i++) menu_grid_rows[i] = LV_GRID_CONTENT;
  menu_grid_cols[MENU_COLS] = LV_GRID_TEMPLATE_LAST;
  menu_grid_rows[MENU_ROWS] = LV_GRID_TEMPLATE_LAST;
}
#endif

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
  // Fence any in-flight async display-flush DMA BEFORE mutating the screen tree. On the C6 the
  // flush queues a tile DMA and returns with the panel CS held low until the transfer-done ISR;
  // tearing down app_scr + rendering a new screen into the SAME alternating draw buffers while
  // that DMA is still reading one of them corrupts the in-flight transfer = garbled panel that
  // compounds as you nav deeper (rebuild after rebuild). Draining first makes the buffers/bus
  // idle so the rebuild is clean. No-op on the S3 (sync flush).
  display_bus_drain();
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
#if BOARD_SCREEN_ROUND
  // Round face: the top-left CORNER is clipped by the curved bezel, so the hint clips out
  // the side. Every header has empty space ABOVE the title, so park the hint TOP-CENTER in
  // that band (above the title) where the round face is at full width. The title sits at
  // TOP_MID +UI_PX(40), so a hint at +UI_PX(12) clears it.
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, UI_PX(12));
#else
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, UI_PX(16), UI_PX(14));
#endif
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
static void app_open_fitness(void);
static void app_open_sleep(void);
static void app_open_sleep_data(void);
static void app_open_sleep_trends(void);
static void app_open_sleep_night(void);

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
  const char *symbol;       // LVGL built-in symbol (used when icon_cp == 0)
  uint32_t    icon_color;   // fixed per-app icon tint (label stays white)
  screen_fn   open;         // screen this tile opens
  uint32_t    icon_cp;      // 0 = use `symbol` in MENU_TILE_ICON_FONT; else this MDI
                            // codepoint, drawn in the icons34 MDI font (migration path
                            // away from the limited built-in LV_SYMBOL_* set)
};
static const MenuItem MENU_ITEMS[] = {
  { "Notifications", LV_SYMBOL_BELL,         0xFF3030, app_open_notifications },
  { "Timer",         LV_SYMBOL_LOOP,         0xFF99FF, app_open_timer, MDI_WATCH },
  { "Appearance",    LV_SYMBOL_TINT,         0xFF9F0A, app_open_appearance },
  { "Power",         LV_SYMBOL_BATTERY_FULL, 0x32D74B, app_open_power },
  { "WiFi & BLE",    LV_SYMBOL_WIFI,         0x33A0FF, app_open_wifi_ble },
  { "Player",        LV_SYMBOL_AUDIO,        0xFF375F, app_open_player },
  { "Find Phone",    LV_SYMBOL_CALL,         0x00C2A8, app_open_find_phone },
  { "Files",         LV_SYMBOL_DRIVE,        0x9B8CFF, app_open_files },
  { "About",         LV_SYMBOL_LIST,         0xFFFF80, app_open_about },
#if BOARD_HAS_IMU_QMI8658
  /* Both are IMU-driven: Fitness is the hardware step counter, Sleep tracks
   * overnight movement. A board without a QMI8658 (e.g. the S3-1.47) would only
   * ever show 0 steps / never detect sleep, so omit the tiles entirely there. */
  { "Fitness",       LV_SYMBOL_LOOP,         0x32D74B, app_open_fitness, MDI_RUN_FAST },
  { "Sleep",         LV_SYMBOL_POWER,        0x9B8CFF, app_open_sleep, MDI_SLEEP },
#endif
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

/* Set a tile's icon label from a MenuItem: a built-in LV_SYMBOL in the per-board
 * MENU_TILE_ICON_FONT (montserrat_34), OR — when item->icon_cp is set — a Material
 * Design Icons glyph in the 34px icons34 MDI font. Same tint either way. */
static void menu_tile_set_icon(lv_obj_t *icon, const MenuItem *item) {
  lv_obj_set_style_text_color(icon, lv_color_hex(ui_deco_hex(item->icon_color)), 0);
  if (item->icon_cp) {
    lv_obj_set_style_text_font(icon, &icons34, 0);
    char u[5];
    lv_label_set_text(icon, mdi_utf8(item->icon_cp, u));
  } else {
    lv_obj_set_style_text_font(icon, &MENU_TILE_ICON_FONT, 0);
    lv_label_set_text(icon, item->symbol);
  }
}

/* Build ONE app tile (icon + caption) into `parent`, sized w x h. The caller sizes
 * tiles so MENU_COLS fit across and MENU_ROWS fit down. The icon FONT is the
 * per-board MENU_TILE_ICON_FONT. Layout adapts to the tile shape:
 *   - WIDE tile (w > h, e.g. the C6's full-width column rows): icon on the LEFT,
 *     name to its RIGHT (a list row).
 *   - SQUARE-ish tile (the S3 grid): icon on top, name underneath. */
/* Returns the tile object so a grid-layout caller can place it in an explicit
 * cell (lv_obj_set_grid_cell); flex-layout callers can ignore the return. */
static lv_obj_t *menu_build_tile(lv_obj_t *parent, const MenuItem *item, int w, int h) {
  lv_obj_t *tile = lv_btn_create(parent);
  lv_obj_set_size(tile, w, h);
  lv_obj_set_style_bg_color(tile, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_radius(tile, (h < w ? h : w) / 6, 0);
  lv_obj_set_style_shadow_width(tile, 0, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tile, HAPTICS_NO_BUZZ_FLAG);   // opening an app is silent (it's an lv_btn,
                                                 // so the global haptic hook would otherwise
                                                 // buzz it like a real button — see haptics.h)
  lv_obj_add_event_cb(tile, menu_tile_cb, LV_EVENT_CLICKED, (void *)item);

  if (w > h) {
    // WIDE list row: icon left, name right, via a flex row (no pre-layout width math).
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(tile, w / 8, 0);
    lv_obj_set_style_pad_column(tile, w / 12, 0);

    lv_obj_t *icon = lv_label_create(tile);
    menu_tile_set_icon(icon, item);

    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_style_text_font(name, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_label_set_text(name, item->name);
  } else {
    // SQUARE tile: icon on top, name underneath (the S3 grid look).
    lv_obj_t *icon = lv_label_create(tile);
    menu_tile_set_icon(icon, item);

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
    // S3-2.06 look: montserrat_34 icon near the top, montserrat_12 caption pinned
    // just up from the bottom. The offsets were originally HARD-CODED for that
    // board's 104 px tile on a 410 px panel (icon +18, caption -12, width 100).
    //
    // The caption WIDTH especially must derive from the tile, not be a constant:
    // a label wider than its parent makes the tile demand more width than we set,
    // so the flex row needs more space than the tile size suggests and ROW_WRAP
    // drops the last column onto a new row. That is why shrinking the tile alone
    // never fixed the "3 columns became 2" bug on the 368 px S3-1.8 — the tile got
    // smaller but its 100 px caption did not, so the row still overflowed.
    //
    // Derive all three from the tile box (w/h). On a 104 px tile these evaluate to
    // ~100 / 18 / 12 — i.e. the S3-2.06 keeps its original look — while a smaller
    // tile now scales its contents down with it instead of overflowing.
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, h * 18 / 104);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
    lv_obj_set_width(name, w - 4);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -(h * 12 / 104));
#endif
  }
  return tile;
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
#if BOARD_SCREEN_ROUND
  // ROUND panel (T5 466x466): pull the whole 3x3 grid UP toward the "Apps" header so the
  // bottom row clears the "< BOOT" hint (was clipping Files). Tiles stay FULL SIZE — only
  // the grid's vertical position moves; the "Apps" title is NOT moved.
  const int qs_pager_top  = UI_PX(40);          // first tile row sits right under "Apps"
  const int qs_pager_h    = (int)screenHeight - qs_pager_top - qs_dots_strip;
#else
  const int qs_pager_top  = UI_PX(40);
  const int qs_pager_h    = (int)screenHeight - qs_pager_top - qs_dots_strip;
#endif
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
  //
  // MENU_TILE_BORDER_SLACK: a tile is an lv_btn, and LVGL's default button style
  // carries a BORDER (and outline) whose width is NOT covered by lv_obj_set_size()
  // or by the tile's own pad_all(0) — the drawn box is a couple of px wider than
  // the size we set. On the 410-wide S3-2.06 the leftover slack swallowed that; on
  // a 368-wide panel it did not, and the 3rd column wrapped to a 4th row that then
  // ran off the bottom. Reserve a few px PER TILE so the fit math is honest on any
  // panel width instead of relying on there happening to be slack.
  const int MENU_TILE_BORDER_SLACK = 4;   // per tile, both axes
  // The page's REAL content box: pad_all(side_pad) on every side, plus the extra
  // top padding added below (side_pad + UI_PX(20)) on non-round, non-narrow boards.
  // The old math subtracted only 2*side_pad and ignored that extra top pad, so the
  // vertical fit was overstated by ~UI_PX(20) — one half of the clipping.
#if BOARD_SCREEN_ROUND || BOARD_SCREEN_NARROW
  const int page_pad_top = side_pad;
#else
  const int page_pad_top = side_pad + UI_PX(20);
#endif
  int avail_w = page_w - 2 * side_pad - MENU_TILE_GAP;   // usable width inside the page
  int avail_h = page_h - page_pad_top - side_pad - MENU_TILE_GAP;  // usable height
  int fit_w = (avail_w - (MENU_COLS - 1) * MENU_TILE_GAP) / MENU_COLS - MENU_TILE_BORDER_SLACK;
  int fit_h = (avail_h - (MENU_ROWS - 1) * MENU_TILE_GAP) / MENU_ROWS - MENU_TILE_BORDER_SLACK;
  int tile_sz = (fit_w < fit_h) ? fit_w : fit_h;   // square side = the limiting axis
  if (tile_sz < 32) tile_sz = 32;

#if BOARD_SCREEN_ROUND
  // ROUND (T5): full 104 px tiles sat a touch tight; shrink VERY slightly to 96 so the
  // grid is a bit more compact (the user wanted only a small reduction). Cap to fit_w.
  tile_sz = (fit_w < 96) ? fit_w : 96;
#elif !BOARD_SCREEN_NARROW
  // The ORIGINAL menu used FIXED 104x104 tiles, and the S3-2.06 look is defined by
  // that size — so 104 stays the PREFERRED size. But it is only a PREFERENCE: cap it
  // to the computed fit on BOTH axes (tile_sz above is already min(fit_w, fit_h)), so
  // a panel that can't afford 104 shrinks instead of overflowing. Previously this
  // capped to fit_w ONLY, which is why a shorter panel (S3-1.8, 448 tall) kept 102 px
  // tiles that didn't fit 3 rows vertically and pushed a row off the bottom.
  if (tile_sz > 104) tile_sz = 104;
#endif

  // HARD ANTI-WRAP GUARANTEE — runs LAST, after every per-board cap above, so
  // nothing can re-inflate the tile past what actually fits.
  //
  // Everything above is a PREDICTION of LVGL's flex box model (page padding +
  // per-track gaps + each tile's own border/outline). If that prediction is off by
  // even ONE pixel, ROW_WRAP silently pushes the last column onto a new row — which
  // is precisely the "3 columns became 2, with extra rows running off the bottom"
  // bug. Rather than trust the estimate, SHRINK until the grid provably fits,
  // measuring the way LVGL lays it out: N tiles + (N-1) gaps inside the content box.
  //
  // WRAP_SAFETY is a whole-row/column reserve (not per-tile) absorbing the rounding
  // and border/outline the per-tile slack may still under-count. It costs a couple
  // of px of tile size and removes an entire class of layout bug.
  {
    const int WRAP_SAFETY = MENU_TILE_GAP;      // one extra gap's worth, per axis
    const int inner_w = page_w - 2 * side_pad;                  // real content width
    const int inner_h = page_h - page_pad_top - side_pad;       // real content height
    while (tile_sz > 32 &&
           (MENU_COLS * (tile_sz + MENU_TILE_BORDER_SLACK)
            + (MENU_COLS - 1) * MENU_TILE_GAP) > (inner_w - WRAP_SAFETY)) {
      tile_sz--;
    }
    while (tile_sz > 32 &&
           (MENU_ROWS * (tile_sz + MENU_TILE_BORDER_SLACK)
            + (MENU_ROWS - 1) * MENU_TILE_GAP) > (inner_h - WRAP_SAFETY)) {
      tile_sz--;
    }
  }
  int tile_w = tile_sz, tile_h = tile_sz;

#if MENU_COLS > 1
  menu_grid_dsc_init();   // fill the shared grid track descriptors (see their note)
#endif

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
    // Multi-column grids (S3): an EXPLICIT GRID of exactly MENU_COLS x MENU_ROWS.
    //
    // This deliberately does NOT use LV_FLEX_FLOW_ROW_WRAP. A wrapping flex row
    // reflows whenever the tiles + gaps + ANY child's requested width exceed the
    // row, so a single mis-estimated pixel silently moved the 3rd tile onto a 4th
    // row (the long-standing "3 columns show as 2, extra rows clip off the bottom"
    // bug on the 368 px S3-1.8). Repeated attempts to fix that by re-deriving the
    // tile size could not work: flex asks the CHILD how much width it wants, so any
    // child that wants more re-wraps regardless of the tile size we set.
    //
    // A grid places each child at an EXPLICIT (col,row) cell by index. The track
    // count is fixed, so the layout cannot reflow — worst case a cell's content is
    // clipped, never re-arranged. That makes MENU_COLS x MENU_ROWS a structural
    // guarantee instead of an arithmetic prediction.
    //
    // Tracks are LV_GRID_CONTENT (each sizes to the tile placed in it) so the
    // existing tile_w/tile_h sizing still drives the look. The descriptor arrays
    // are file-scope statics (menu_grid_cols/rows, defined above): LVGL KEEPS THE
    // POINTER rather than copying, so they must outlive this function — a local
    // array here would dangle the moment app_menu_init() returns.
    lv_obj_set_grid_dsc_array(page, menu_grid_cols, menu_grid_rows);
    // Whole-grid placement inside the page: columns CENTERED horizontally, rows
    // START-aligned so the grid hugs the top like the original menu (centering it
    // vertically left a gap under the "Apps" title and crammed the last row into
    // the BOOT hint).
    lv_obj_set_grid_align(page, LV_GRID_ALIGN_CENTER, LV_GRID_ALIGN_START);
#endif
    lv_obj_set_style_pad_row(page, MENU_TILE_GAP, 0);
    lv_obj_set_style_pad_column(page, MENU_TILE_GAP, 0);
    lv_obj_set_style_pad_all(page, side_pad, 0);
#if BOARD_SCREEN_ROUND
    // ROUND (T5): keep the grid HIGH so all three rows + the BOOT hint fit on the
    // shorter usable height of a round face. Just a small gap below the "Apps" title
    // (no big UI_PX(20) push-down — that shoved the bottom row into the BOOT hint).
    lv_obj_set_style_pad_top(page, side_pad, 0);
#elif !BOARD_SCREEN_NARROW
    // Push the top-aligned grid DOWN a little so the first row isn't crammed against
    // the "Apps" title. Pure top padding — the grid stays top-anchored (a lone tile
    // still starts at the top), it just starts lower. C6 untouched.
    //
    // The push-down is BUDGETED against the REAL leftover space, not a flat
    // constant. UI_PX(20) alone was tuned on the 2.06, which has ~60 px of slack
    // below the grid. The shorter S3-1.8 has only ~30 px, so the same nudge pushed
    // the bottom row down onto the "< BOOT" hint.
    //
    // Rule: keep a guaranteed BOTTOM clearance for the hint first, and only spend
    // what is genuinely left over on the top nudge. A roomy panel still gets the
    // full 20 px (2.06 keeps its exact original look); a tight panel gives the top
    // nudge up rather than crowd the hint, which is what pulls the grid back up
    // toward the "Apps" title.
    {
      const int grid_h    = MENU_ROWS * (tile_h + MENU_TILE_BORDER_SLACK)
                            + (MENU_ROWS - 1) * MENU_TILE_GAP;
      const int slack     = page_h - side_pad - grid_h;  // free space under the grid
      const int hint_room = UI_PX(44);                   // reserved for "< BOOT"
      int push = slack - hint_room;                      // whatever survives that
      if (push > UI_PX(20)) push = UI_PX(20);            // never more than the original
      if (push < 0) push = 0;                            // tight panel: hug the title
      lv_obj_set_style_pad_top(page, side_pad + push, 0);
    }
#endif

    int first = pg * MENU_TILES_PER_PAGE;
    int last  = first + MENU_TILES_PER_PAGE;
    if (last > MENU_ITEM_COUNT) last = MENU_ITEM_COUNT;
    for (int i = first; i < last; i++) {
      lv_obj_t *tile = menu_build_tile(page, &MENU_ITEMS[i], tile_w, tile_h);
#if MENU_COLS > 1
      // Explicit cell for the grid layout: index within the page -> (col,row).
      // This is what makes MENU_COLS columns structural — the tile is PLACED at
      // its column, it is not flowed into whatever space happens to be left.
      const int slot = i - first;
      lv_obj_set_grid_cell(tile,
                           LV_GRID_ALIGN_CENTER, slot % MENU_COLS, 1,
                           LV_GRID_ALIGN_CENTER, slot / MENU_COLS, 1);
#else
      (void)tile;   // single-column boards use the COLUMN flex flow above
#endif
    }
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
#if BOARD_SCREEN_ROUND
  // Round face: lift the dots well clear of the curved bottom edge, sitting them in
  // the reserved strip just under the last tile row (above the BOOT hint).
  lv_obj_align(menu_dots, LV_ALIGN_BOTTOM_MID, 0, UI_PX(-44));
#else
  lv_obj_align(menu_dots, LV_ALIGN_BOTTOM_MID, 0, 8);
#endif

  // No on-screen close button — BOOT toggles menu<->clock and backs out of apps.
  lv_obj_t *hint = lv_label_create(menu_scr);
  lv_obj_set_style_text_font(hint, &MENU_HINT_FONT, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " BOOT");
#if BOARD_SCREEN_ROUND
  // Round face: the very bottom-center is the lowest point of the circle; pull the
  // hint up into the visible band so it isn't clipped by the curved bezel.
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, UI_PX(-18));
#elif !BOARD_SCREEN_NARROW
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, UI_PX(-10));   // wide panel: bottom-center
#else
  // Narrow panel (C6-1.47 / S3-1.47): the bottom row of the page grid reaches the
  // bottom edge, so a bottom hint overlaps the last app. Put it in the top-left
  // corner instead (out of the grid). Gated on the SCREEN, not PSRAM.
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
