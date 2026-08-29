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
/* The three tiers are mutually exclusive (NARROW <=220, MIDNARROW 221..320, else
 * wide), so order is not load-bearing here — MIDNARROW is simply listed first. */
#if BOARD_SCREEN_MIDNARROW
/* MID-NARROW portrait (S3-1.64, 280x456): a widescreen-shaped panel stood upright.
 * Wide enough that the 1-column narrow layout would waste it, but 130 px narrower
 * than the 410 reference — at 3 columns the tiles came out so small that the icon
 * collided with a label that had to wrap. TWO columns x 3 rows gives 6 per page:
 * each tile is roughly half the panel width instead of a third, so the icon and a
 * single-line label fit with room to spare, and the tall panel still shows 3 rows.
 * The larger tile also affords the full-size 34 px icon. */
#define MENU_COLS           2
#define MENU_ROWS           3
#define MENU_TILE_GAP       12
#define MENU_TILE_ICON_FONT lv_font_montserrat_34
#define MENU_HINT_FONT      lv_font_montserrat_16   // HAND-ACCEPTED on the 1.64 — not UI_FONT
#elif BOARD_SCREEN_NARROW
/* TRULY NARROW portrait (C6-1.47, 172 wide): lay each page out as ONE COLUMN of
 * 3 tiles stacked vertically, and swipe horizontally for the next 3. Tiles are
 * full-width and big. */
#define MENU_COLS           1
#define MENU_ROWS           3
#define MENU_TILE_GAP       10
#define MENU_TILE_ICON_FONT lv_font_montserrat_34
#define MENU_HINT_FONT      lv_font_montserrat_10   // "< BOOT" back hint (app screens + menu)
#elif BOARD_SCREEN_SUBREF
/* SUB-REFERENCE but still 3-columns-wide (S3-Touch-LCD-2, 240x320): the panel is
 * square enough for the reference 3x3 grid, but it is 41% narrower than the 410 px
 * panel those tile/label sizes were authored against. So it keeps the grid SHAPE
 * and shrinks only the text:
 *   - MENU_TILE_ICON_FONT stays 34: the icons look right at this size and were
 *     explicitly reported as good — do not scale them with the labels.
 *   - MENU_HINT_FONT auto-scales via UI_FONT(20) (12 at 240 wide): the "< BOOT"
 *     hint is a dim discoverability label, not content, and at the reference
 *     20 px it dominated a 240 px screen.
 * The tile CAPTION font is handled in menu_build_tile() (see the SUBREF branch
 * there) because it also needs the caption's width/offsets to match. */
#define MENU_COLS           3
#define MENU_ROWS           3
/* Tighter gap than the reference 12. The tile side is
 * (panel - 3*gap - 2*gap_between)/3, so on a 240 px panel every px taken off the
 * gap goes almost 1:1 into the tile: gap 12 -> 56 px tiles, gap 6 -> 66 px. That
 * ~18% bigger tile is what stops the caption colliding with the 34 px icon. */
#define MENU_TILE_GAP       6
#define MENU_TILE_ICON_FONT lv_font_montserrat_34
#define MENU_HINT_FONT      UI_FONT(20)
#elif BOARD_SCREEN_ROUND_SMALL
/* SMALL ROUND face (C2/S2, 360x360): keeps the reference 3x3 grid — the panel is
 * wide enough — but the whole page has to fit in 360 px of HEIGHT rather than the
 * reference 502, with a bottom band for the page dots and the "< BOOT" hint that
 * the round bezel also pushes inward. Two knobs do the work:
 *   - MENU_TILE_GAP 8 (from 12): the tile side is roughly
 *     (usable - 2*gap)/3, so every px off the gap goes almost 1:1 into the tile.
 *     Buying tile size back from the gap is what keeps the icon + caption legible
 *     after the vertical budget shrinks (the same trade the SUBREF tier makes).
 *   - MENU_TILE_ICON_FONT stays 34, like every other tier — the tile icons have
 *     been accepted at that size on panels from 240 to 466 wide.
 * The vertical budget itself (title y, pager height, dots strip, hint y) is in
 * app_menu_init; this block is only the grid shape. */
#define MENU_COLS           3
#define MENU_ROWS           3
#define MENU_TILE_GAP       8
#define MENU_TILE_ICON_FONT lv_font_montserrat_34
#define MENU_HINT_FONT      UI_FONT(20)
#else
/* WIDE panels (S3-2.06 410-wide, S3-1.8 368-wide): the reference layout — a
 * 3x3 page of tiles, swipe horizontally for the next page. */
#define MENU_COLS           3
#define MENU_ROWS           3
#define MENU_TILE_GAP       12
#define MENU_TILE_ICON_FONT lv_font_montserrat_34
#define MENU_HINT_FONT      UI_FONT(20)   // identity at the 410 reference
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
#if BOARD_SCREEN_NARROW || BOARD_SCREEN_SUBREF
  // Any panel narrower than the reference: a long centered title (e.g. "Find Phone"
  // or "Notifications") runs horizontally into the top-left "< BOOT" hint — there is
  // not enough width beside the hint for it. Drop the title BELOW the hint row so
  // they never share a horizontal band, and let it wrap centered if still too wide.
  // (SUBREF, not MIDNARROW: this is about WIDTH vs the title, nothing to do with
  // how many launcher columns the panel takes.)
  lv_obj_set_width(t, LV_PCT(100));
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
#if BOARD_SCREEN_NARROW
  // Sit the title well below the BOOT hint row (hint top UI_PX(14) + its line
  // height). UI_PX(44) still left the title's top touching the hint's bottom.
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, UI_PX(72));
#else
  // MID-WIDTH (SUBREF but not narrow — S3-LCD-2 240, S3-1.64 280): the narrow
  // tier's 72 was inherited here and left a ~20 px DEAD BAND between the hint
  // and the title (the hint font on this tier is a small 12, its row ends high),
  // while the title's bottom ran UNDER the content columns that start at
  // UI_PX(80..84). 46 clears the hint's line by a few px and ends just above
  // the content band — the header stops eating into the scroll area.
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, UI_PX(46));
#endif
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
  lv_obj_set_style_text_font(ic, &UI_FONT(20), 0);
  lv_obj_set_style_text_color(ic, lv_color_white(), 0);
  lv_label_set_text(ic, symbol);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 20, 0);

  lv_obj_t *nm = lv_label_create(row);
#if BOARD_SCREEN_SUBREF
  /* Mid-width panels (S3-LCD-2 240, S3-1.64 280): these fixed offsets were
   * authored for the 410 px reference, where FONT_LABEL at x=60 has ~250 px of
   * clear run before the switch. Here it has ~70, so "Auto-dim" ran INTO the
   * switch, and the x=60 start left a dead gutter after the icon (which ends
   * around x=40). Shrink the TEXT only — the icon at 20 px reads right and the
   * user confirmed it — pull it in to x=46, and dot-truncate at a width that
   * stops short of the switch so a longer future caption degrades to "…"
   * instead of clipping under it. */
  lv_obj_set_style_text_font(nm, &FONT_SMALL, 0);   // 12 on this tier (label is 16)
  ui_label_single_line(nm);
  lv_obj_set_width(nm, LV_PCT(44));
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 46, 0);
#else
  lv_obj_set_style_text_font(nm, &FONT_LABEL, 0);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 60, 0);
#endif
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_label_set_text(nm, name);

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
#if BOARD_HAS_CAMERA
static void app_open_camera(void);       // defined in app_camera.h (camera boards only)
static void app_camera_on_close(void);   // releases the sensor + buffers when leaving
#endif
/* Gallery availability: photos/videos need PSRAM decode buffers and the ESP32
 * core's esp_jpeg component — no camera required, so this is its OWN gate (a
 * board with storage but no sensor still gets the app). Same expression as
 * app_gallery.h computes; ifndef-guarded so whichever is seen first wins. */
#ifndef OWF_HAS_GALLERY
#if BOARD_HAS_PSRAM && !BOARD_PLATFORM_TUYA && __has_include("jpeg_decoder.h")
#define OWF_HAS_GALLERY 1
#else
#define OWF_HAS_GALLERY 0
#endif
#endif
#if OWF_HAS_GALLERY
static void app_open_gallery(void);      // defined in app_gallery.h
static void app_gallery_on_close(void);  // frees the decoded thumbs/frames
#endif
static void app_open_fitness(void);
static void app_open_weather(void);
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
#if BOARD_HAS_CAMERA
  /* Leaving the Camera app (it is a ROOT sub-app, so any back from it exits it):
   * stop the live preview, power the sensor down and free its PSRAM buffers.
   * Without this the preview timer would keep firing against deleted widgets and
   * the sensor would stay powered for as long as the watch is awake. */
  if (nav_current == app_open_camera) app_camera_on_close();
#endif
#if OWF_HAS_GALLERY
  /* Same shape for the Gallery: decoded thumbnails/frames are PSRAM-heavy and
   * the video player may hold an open file + timer. This also covers backing
   * out of a Gallery that was opened FROM the Camera app (nav pops back to it). */
  if (nav_current == app_open_gallery) app_gallery_on_close();
#endif
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
  { "Weather",       LV_SYMBOL_REFRESH,      0xFFD60A, app_open_weather, MDI_WX_PARTLY },
  { "Files",         LV_SYMBOL_DRIVE,        0x9B8CFF, app_open_files },
#if BOARD_HAS_CAMERA
  /* Only on a board with a camera sensor wired up (the S3-Touch-LCD-2's DVP
   * header). Elsewhere the app itself doesn't exist, so the tile must not either
   * — same reasoning as the IMU-gated tiles below. */
  { "Camera",        LV_SYMBOL_VIDEO,        0x5AC8FA, app_open_camera },
#endif
#if OWF_HAS_GALLERY
  /* Deliberately NOT camera-gated: the Gallery browses whatever media is on
   * the SD card / flash, so it is useful on any board that can decode it.
   * Burnt orange, DELIBERATELY not Appearance's amber 0xFF9F0A — two orange
   * tiles at the same hue read as the same app at a glance. */
  { "Gallery",       LV_SYMBOL_IMAGE,        0xFF6B2D, app_open_gallery },
#endif
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

/* THE one caption font for the whole app grid: the largest stock Montserrat, up
 * to the tier's authored caption size, at which EVERY name in MENU_ITEMS fits
 * `cap_w` on one line — i.e. the group is fitted to its worst member, so all
 * tiles render the same size (mixed per-tile sizes read as a glitch). Cached:
 * the answer only depends on cap_w (constant per board) and the static list. */
static const lv_font_t *menu_caption_font(int cap_w) {
  static const lv_font_t *cached = nullptr;
  static int cached_w = -1;
  if (cached && cached_w == cap_w) return cached;
#if BOARD_SCREEN_NARROW
  const int authored_px = 10;   // the C6 tier's hand caption size
#else
  const int authored_px = 12;   // authored on the 2.06 (UI_FONT identity there)
#endif
  const lv_font_t *worst = ui_font_fit(MENU_ITEMS[0].name, cap_w, authored_px);
  for (int i = 1; i < MENU_ITEM_COUNT; i++) {
    const lv_font_t *f = ui_font_fit(MENU_ITEMS[i].name, cap_w, authored_px);
    if (lv_font_get_line_height(f) < lv_font_get_line_height(worst)) worst = f;
  }
  cached = worst;
  cached_w = cap_w;
  return cached;
}

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
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(name, item->name);

    // CAPTION FONT IS AUTO-FITTED **UNIFORMLY** (menu_caption_font): ONE font for
    // every tile — the largest stock Montserrat, up to the tier's authored size,
    // at which EVERY app name in MENU_ITEMS fits its caption on ONE line. Two
    // rules meet here and both are hard requirements:
    //   1. a caption must NEVER wrap — the second line runs up into the 34 px
    //      icon (the reported bug: "Notifications" wrapping on the 240-wide
    //      panels), and
    //   2. the size must be THE SAME on every tile — per-label fitting made
    //      "Timer" render bigger than "Notifications" next to it, which reads as
    //      a layout glitch, not a design.
    // So the group is fitted to its WORST member: the 2.06's ~100 px captions
    // keep the authored 12 everywhere; the 240-wide panels drop the whole set to
    // whatever "Notifications" needs.
    //
    // Belt-and-braces: even the 8 px floor can lose to some future long name, so
    // the label ALSO gets DOT truncation with a FIXED ONE-LINE box. Both parts of
    // that are load-bearing: DOT against an auto-height label still wraps (LVGL
    // only truncates against a fixed size — the silent gotcha that defeated the
    // previous montserrat_10+DOT fix on the LCD-2), so the height must be pinned
    // to the font's line height.
    const lv_font_t *cap = menu_caption_font(w - 4);
    lv_obj_set_style_text_font(name, cap, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);   // truncate, never wrap
    lv_obj_set_size(name, w - 4, lv_font_get_line_height(cap));  // fixed 1-line box

#if BOARD_SCREEN_NARROW
    // C6: scale the icon/name placement to the small tile.
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, h / 6);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -(h / 12));
#elif BOARD_SCREEN_SUBREF
    // SUB-REFERENCE 3-column panel (S3-LCD-2 / S3-1.69, 240 wide). The tile here
    // is ~66 px against the 2.06's ~112 px — barely over half — but the icon stays
    // at 34 px (deliberate: the icons read well at that size), so placement is
    // tightened: the icon sits higher (h*10/104 vs h*18/104) and the caption hugs
    // the bottom, maximising the clear band between them.
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, h * 10 / 104);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -(h * 6 / 104));
#else
    // S3-2.06 look: montserrat_34 icon near the top, caption pinned just up from
    // the bottom. The offsets were originally HARD-CODED for that board's 104 px
    // tile on a 410 px panel (icon +18, caption -12, width 100).
    //
    // The caption WIDTH especially must derive from the tile, not be a constant:
    // a label wider than its parent makes the tile demand more width than we set,
    // so the flex row needs more space than the tile size suggests and ROW_WRAP
    // drops the last column onto a new row. That is why shrinking the tile alone
    // never fixed the "3 columns became 2" bug on the 368 px S3-1.8 — the tile got
    // smaller but its 100 px caption did not, so the row still overflowed.
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, h * 18 / 104);
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
#if BOARD_SCREEN_ROUND_SMALL
  // SMALL ROUND face: the requested "move Apps up so the tiles don't have to
  // shrink so much". Two things buy that height back:
  //   - a 22 px title instead of the 28 px FONT_LABEL (~27 px line instead of ~34)
  //   - y = UI_PX(4) instead of UI_PX(8)
  // The title can't simply be slid to y=0: this is a CIRCLE, and the usable chord
  // narrows fast near the top. At y=4 on the 360 C2 the chord is only ~2*38 px
  // wide, and "Apps" at 28 px is ~62 px — it would touch the arc. At 22 px it is
  // ~48 px and clears. So the font drop is what makes the move up possible, not a
  // separate cosmetic change.
  lv_obj_set_style_text_font(title, &UI_FONT(22), 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_label_set_text(title, "Apps");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_PX(2));
#else
  lv_obj_set_style_text_font(title, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_label_set_text(title, "Apps");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_PX(8));
#endif

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
#if BOARD_SCREEN_ROUND_SMALL
  // SMALL ROUND face: the reported bug was the bottom tile row sitting ON the page
  // dots and covering half the "< BOOT" hint. The old strip was 22 RAW px, which
  // reserved room for the dots ALONE and left the hint to overlap whatever was
  // above it — invisible on a 466 px face that has ~30 px of slack anyway, fatal
  // on a 360 px one that has none. Reserve the WHOLE bottom band instead:
  //
  //     hint  "< BOOT" at 20 px, line ~25, bottom-anchored at UI_PX(-12) = -10
  //           -> y 325..350
  //     dots  8 px tall, bottom-anchored at UI_PX(-46) = -40   -> y 312..320
  //     band  = 360 - 316 = 44
  //
  // FIRST PASS reserved 44 and predicted a 12 px gap between the last tile row and
  // the dots; on glass the bottom row still just touched the dot strip, so the
  // prediction was a little optimistic (LVGL's grid CONTENT tracks size to the
  // tile's drawn box, which carries the button border the estimate under-counts).
  // Reserve 60 instead and stop predicting the margin down to single px.
  //
  // Note the strip cannot be widened ALONE: the tile size is derived from whatever
  // height is left over, so growing the strip shrinks the tiles 1:1. Pairing it
  // with the smaller "Apps" header (which frees ~10 px at the top) is what keeps
  // the tiles at 75 px — 2 px off the previous 77 — instead of paying the full
  // 16 px out of the tile.
  const int qs_dots_strip = 60;
#else
  const int qs_dots_strip = 22;                 // bottom strip height for the page dots
#endif
#if BOARD_SCREEN_ROUND_SMALL
  // Title is now ~27 px tall at y=UI_PX(4)=3, so it ends at ~30. Start the pager
  // just below it rather than at the generic UI_PX(40)=35 — on this panel the old
  // value both overlapped the 34 px title AND wasted the height the tiles need.
  const int qs_pager_top  = UI_PX(26);
  const int qs_pager_h    = (int)screenHeight - qs_pager_top - qs_dots_strip;
#elif BOARD_SCREEN_ROUND
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

#if BOARD_SCREEN_ROUND_SMALL
  // SMALL ROUND face: 96 is still the PREFERRED size (same look as the other round
  // boards), but cap it to what actually fits on BOTH axes. The generic round line
  // below caps to fit_w only, which is wrong here: on the 360 C2 fit_w is ~102 and
  // fit_h is ~77, so a width-only cap hands back 96 and relies entirely on the
  // anti-wrap loop to claw it back. That works, but it means the tile size is set
  // by a fallback rather than by the fit — cap honestly instead.
  if (tile_sz > 96) tile_sz = 96;
#elif BOARD_SCREEN_ROUND
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
#if BOARD_SCREEN_ROUND_SMALL
  // Small round face: sit the dots in the reserved 44 px band, clear of BOTH the
  // last tile row above and the "< BOOT" hint below (see the band arithmetic at
  // qs_dots_strip). UI_PX(-46) = -40 on the C2 -> the 8 px row occupies y 312..320.
  lv_obj_align(menu_dots, LV_ALIGN_BOTTOM_MID, 0, UI_PX(-42));
#elif BOARD_SCREEN_ROUND
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
#if BOARD_SCREEN_ROUND_SMALL
  // Small round face: the dots now own the band from UI_PX(-46) upward, so the hint
  // drops to UI_PX(-12) (= -10 on the C2, y 325..350). It still clears the bezel:
  // at y=340 the inscribed circle is ~164 px wide and "< BOOT" at 20 px is ~60 px.
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, UI_PX(-8));
#elif BOARD_SCREEN_ROUND
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
#if BOARD_HAS_CAMERA
  /* Same teardown as nav_back(), for the path that jumps straight to the clock
   * (BOOT from the menu, or anything else that closes the UI outright) without
   * walking back through the app. */
  if (nav_current == app_open_camera) app_camera_on_close();
#endif
#if OWF_HAS_GALLERY
  if (nav_current == app_open_gallery) app_gallery_on_close();
#endif
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
