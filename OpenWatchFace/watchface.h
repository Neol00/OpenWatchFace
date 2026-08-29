/* ============================================================================
 *  watchface.h — the clock face: stat row + enormous HH:MM clock.
 *
 *  Top row, three colored stat columns (bell/unread · battery · wifi) with a
 *  top-right indicator tray (caffeine mug, BLE glyph) and corner voltage; center
 *  is the big clock with weekday + date below. Builds the widgets once
 *  (watchface_create) and exposes setters the loop calls as state changes.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER the app_*.h screens and
 *  ui_fonts.h: it uses the FONT_* aliases, the shared store lock + notif_unread()
 *  (via watchface_refresh_bell, forward-declared in watch_base.h), and the
 *  screenWidth global from the .ino. Nothing here is referenced by the app
 *  modules except watchface_refresh_bell(), so it can safely live after them.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

/* ---- Watch-face widgets (3 colored stat columns + big clock) --
 * Top row, three columns, each = a colored icon with a value label below it:
 *   [battery icon + NN%]   [blue calendar + "June 4"]   [red day + "Thu"]
 * Center: enormous HH:MM clock.
 * The battery icon is a custom two-part widget (outline + colored fill bar) so
 * only the FILL is tinted green/yellow/red by level — LVGL's symbol glyphs are
 * single-color and can't do that. */
static lv_obj_t *lbl_time, *lbl_weekday, *lbl_date;
static lv_obj_t *lbl_volt;                                  // top-right corner
static lv_obj_t *lbl_bell_icon, *lbl_bell_cnt;              // LEFT column (red)
static lv_obj_t *batt_shell, *batt_fill, *lbl_batt_pct;    // MIDDLE column
static lv_obj_t *lbl_wifi_icon, *lbl_wifi_txt;             // RIGHT column (blue)
static lv_obj_t *caf_ind = nullptr;                        // top-right caffeine (keep-awake) mug, gray
static lv_obj_t *lbl_ble_icon = nullptr;                   // top-right BLE glyph, shown when a phone is connected
/* Mirror indicators for the swap-WiFi/BLE layout: the WiFi glyph ALSO exists in the
 * top-right tray, and the BLE column ALSO exists in the right stat column. Both
 * representations always track real state (watchface_set_wifi/_set_ble update both);
 * watchface_apply_indicator_layout() shows exactly one of each pair per the setting. */
static lv_obj_t *lbl_wifi_tray = nullptr;                  // top-right WiFi glyph (used when swapped)
static lv_obj_t *lbl_mute_icon = nullptr;                  // top-right muted-speaker glyph, shown when muted
static lv_obj_t *lbl_sleep_icon = nullptr;                 // top-right sleep-mode glyph, shown while a sleep session is active
static lv_obj_t *lbl_ble_col_icon = nullptr, *lbl_ble_col_txt = nullptr;  // RIGHT column BLE (used when swapped)

/* Under-dial WEATHER widget (optional; settings_get_show_weather). A small centered
 * cluster below the clock: a condition icon + the current temperature. The icon is a
 * SINGLE tinted MDI weather glyph (wx_cloud_layer) chosen by wx_pick_glyph() — the same
 * picker the in-app icon uses, so they match. The MDI weather glyphs are hollow outlines,
 * so layering two would let the back one show through; the second label (wx_sun_layer)
 * is retained but kept HIDDEN. Shown/hidden by watchface_apply_weather_visible(); when
 * shown, the weekday/date row shifts down (WF_WEEKDAY_Y_WX) to make room. */
static lv_obj_t *wf_weather   = nullptr;   // container (icon + temp)
static lv_obj_t *wx_sun_layer = nullptr;   // retained-but-hidden second label (see above)
static lv_obj_t *wx_cloud_layer = nullptr; // the single tinted weather glyph
static lv_obj_t *wx_temp_lbl  = nullptr;   // temperature text

/* Vertical positions (LV_ALIGN_CENTER y-offset, pre-UI_PX). The weekday/date row
 * normally sits at _DEFAULT; when the weather widget is shown it drops to _WX so the
 * widget (at WF_WEATHER_Y) has room between the clock and the date. */
#define WF_WEEKDAY_Y_DEFAULT  95
#define WF_WEEKDAY_Y_WX       150
#define WF_WEATHER_Y          98

/* Container handles, kept so the deep-dim "minimal face" (watchface_set_minimal)
 * can hide everything except the center clock in one shot. The three stat columns
 * and the corner indicator tray are otherwise local to watchface_create. */
static lv_obj_t *wf_corner_tray = nullptr;                 // holds BLE + caffeine icons
static lv_obj_t *wf_lcol = nullptr, *wf_mcol = nullptr, *wf_rcol = nullptr;  // bell / battery / wifi columns

/* Bell (notification) column colors. The reds are deliberate on emissive
 * panels ("keep the bell RED always"), but on the 1-bit INVERTED e-paper both
 * of them threshold to white — an invisible bell (red's luminance is low: even
 * bright 0xFF3030 lands under mid-grey once green-weighted). There, use the
 * accent for both states — it's the color proven to render as black ink; the
 * unread count text next to it still carries the state. */
#if BOARD_DISPLAY_EPD_GDEQ031T10
#define WF_BELL_BASE   ui_accent_hex()
#define WF_BELL_UNREAD ui_accent_hex()
#define WF_BELL_IDLE   ui_accent_hex()
#else
#define WF_BELL_BASE   0xFF4D4D
#define WF_BELL_UNREAD 0xFF3030
#define WF_BELL_IDLE   0x661414
#endif

static const char *const WEEKDAYS[7] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const MONTHS[12] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};

/* Place a child at horizontal position cx_pct (% of screen width) along the top
 * stat row, top-aligned y px down. Returns a transparent column container with
 * vertical-center flex so an icon + value label stack neatly. */
static lv_obj_t *make_stat_column(lv_obj_t *scr, int cx_pct, int y) {
  lv_obj_t *col = lv_obj_create(scr);
  lv_obj_remove_style_all(col);
#if BOARD_SCREEN_ROUND_SMALL
  // Raising the stat row (see TOP_Y) moves it into a NARROWER chord of the circle:
  // on the 360 C2 the row now sits at y=82, where the inscribed circle spans only
  // x 29..331 instead of the full 360. A UI_PX(110)=97 wide column centred at 22%
  // would start at x=31 — 2 px from the arc. Trim the column so the outer two have
  // real clearance from the bezel. Paired with the 24%/76% centres below.
  lv_obj_set_size(col, UI_PX(100), LV_SIZE_CONTENT);
#else
  lv_obj_set_size(col, UI_PX(110), LV_SIZE_CONTENT);
#endif
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, UI_PX(4), 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_x(col, (int)(((int)screenWidth * cx_pct) / 100) - (int)screenWidth / 2);
  return col;
}

/* Re-anchor the voltage label to the LEFT of the indicator tray, so it sits snug
 * against the icons that are ACTUALLY visible (the tray is content-sized: it
 * shrinks when an icon hides). Call after any tray icon shows/hides. We update the
 * tray's layout first so its width reflects the current icon set before we align
 * to its edge. The 8 px gap matches the tray's own column padding. Null-safe. */
static void watchface_realign_volt(void) {
  if (!lbl_volt || !wf_corner_tray) return;
  lv_obj_update_layout(wf_corner_tray);     // recompute content-size width NOW
#if BOARD_SCREEN_ROUND
  // ROUND: the tray is parked lower-and-inset (see watchface_create). The voltage sits to
  // its left on the SAME row, so it shares the wide part of the circle and stays visible
  // whether or not any tray icon is showing. Tighter gap since the tray is already inset.
  lv_obj_align_to(lbl_volt, wf_corner_tray, LV_ALIGN_OUT_LEFT_MID, UI_PX(-12), 0);
#else
  lv_obj_align_to(lbl_volt, wf_corner_tray, LV_ALIGN_OUT_LEFT_MID, UI_PX(-20), 0);
#endif
}

static void watchface_create(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);  // AMOLED black = low power
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ROUND panel (T5-1.75): the corners are clipped by the bezel, so anything parked in
  // the top corners (the indicator tray + voltage) or near the very top edge is cut off,
  // and the stat row sat low enough to crowd the big clock. Pull the stat row UP and pull
  // corner content DOWN+IN so it lands inside the inscribed circle. Square panels keep their
  // original values (this whole block is gated on BOARD_SCREEN_ROUND).
#if BOARD_SCREEN_ROUND_SMALL
  // SMALL ROUND face (C2/S2, 360x360). The stat row and the dial are two fixed
  // blocks stacked around the vertical centre, and on a 360-tall panel they
  // collided — the reported "dial clips into the notification/wifi icons".
  //
  // The budget, in real pixels on the C2 (UI_PX factor = 360/410 = 87%):
  //   stat column = icon line (~27) + pad UI_PX(4)=3 + FONT_TOP line (~29) = ~59
  //   dial        = montserrat_clock_88 line_height 61, CENTER-aligned
  //                 -> spans 180 +/- 30 = y 150..211
  // So the stat row must END by ~142 to keep a real gap. UI_PX(94) = 82 puts its
  // bottom at ~141. (The generic round value UI_PX(112) = 98 ended at ~157, i.e.
  // 7 px INTO the dial even after the font drop to 88 — which is why the smaller
  // dial alone did not fix this.)
  const int TOP_Y = UI_PX(94);
#elif BOARD_SCREEN_ROUND
  const int TOP_Y = UI_PX(112);  // stat row higher -> clears the clock below
#else
  const int TOP_Y = UI_PX(128);  // vertical offset of the stat row (scaled to panel)
#endif

  /* --- Voltage readout: tucked in the top-right corner (its own little label).
   *     Its X is NOT fixed — it's re-anchored to the LEFT of the indicator tray
   *     (watchface_realign_volt) so it sits snug against whatever icons are actually
   *     showing. A fixed offset used to reserve space for BOTH icons, leaving a gap
   *     when only one (or none) was visible. --- */
  lbl_volt = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_volt, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(lbl_volt, lv_color_hex(0x777777), 0);
  lv_label_set_text(lbl_volt, "--.--V");

  /* --- Top-right indicator tray: a right-anchored, content-sized flex row that
   *     holds the small status icons (caffeine mug, BLE glyph, …). LVGL flex skips
   *     HIDDEN children, so icons pack tightly from the rounded corner inward and
   *     gaps collapse: the rightmost VISIBLE child always lands in the corner. Add
   *     future corner icons as children (left of the mug) and they slot in.
   *     The -56 inset pulls the tray in far enough that the rightmost icon clears
   *     the rounded display corner (the old -46 let the BLE glyph clip off-edge). --- */
  lv_obj_t *corner_tray = lv_obj_create(scr);
  wf_corner_tray = corner_tray;             // kept for watchface_set_minimal
  lv_obj_remove_style_all(corner_tray);
  lv_obj_set_size(corner_tray, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(corner_tray, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(corner_tray, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(corner_tray, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(corner_tray, UI_PX(6), 0);
#if BOARD_SCREEN_ROUND
  // ROUND: the top-right corner is clipped, so park the tray LOWER (where the circle is
  // wide) and well INSET so the rightmost icon — and the voltage label anchored to the
  // tray's left — stay inside the bezel. Aligned top-CENTER-ish-right: down UI_PX(64),
  // pulled in from the right edge by UI_PX(96).
  lv_obj_align(corner_tray, LV_ALIGN_TOP_RIGHT, UI_PX(-96), UI_PX(64));
#else
  lv_obj_align(corner_tray, LV_ALIGN_TOP_RIGHT, UI_PX(-56), UI_PX(14));  // inset clears the rounded corner
#endif

  /* --- Sleep-mode indicator: a PURPLE moon glyph shown while a sleep-tracking session
   *     is active (Do-Not-Disturb + overnight movement logging). Leftmost tray child so
   *     it sits at the far left of the icon group. Uses the MDI moon glyph in the
   *     icons14/22 font, sized to match the caffeine mug (14px C6 / 22px S3). Flex skips
   *     it while hidden. --- */
  lbl_sleep_icon = lv_label_create(corner_tray);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(lbl_sleep_icon, &icons14, 0);
#else
  lv_obj_set_style_text_font(lbl_sleep_icon, &UI_ICON_MDI(22), 0);
#endif
  lv_obj_set_style_text_color(lbl_sleep_icon, lv_color_hex(ui_deco_hex(0x9B8CFF)), 0);  // purple
  { char u[5]; lv_label_set_text(lbl_sleep_icon, mdi_utf8(MDI_SLEEP, u)); }
  lv_obj_add_flag(lbl_sleep_icon, LV_OBJ_FLAG_HIDDEN);

  /* --- BLE indicator: a BLUE Bluetooth glyph, hidden unless a phone is connected.
   *     First (leftmost) tray child, so it sits to the LEFT of the caffeine mug when
   *     both show, and slides into the corner when the mug is hidden. --- */
  lbl_ble_icon = lv_label_create(corner_tray);
  // Built-in glyph fonts are FIXED-size (not UI_PX-scaled), so the 22px glyph that
  // fits the S3 is oversized on the narrow C6 and clips into the voltage label /
  // caffeine mug. Use a smaller glyph there; S3 keeps montserrat_22.
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(lbl_ble_icon, &lv_font_montserrat_14, 0);
#else
  lv_obj_set_style_text_font(lbl_ble_icon, &UI_ICON_SYM(22), 0);
#endif
  lv_obj_set_style_text_color(lbl_ble_icon, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);  // blue, matches wifi
  lv_label_set_text(lbl_ble_icon, LV_SYMBOL_BLUETOOTH);
  lv_obj_add_flag(lbl_ble_icon, LV_OBJ_FLAG_HIDDEN);

  /* --- WiFi indicator (tray copy): the BLUE wifi glyph that takes the corner when
   *     the user swaps WiFi/BLE. Hidden unless the swap layout is active (and then
   *     shown/dimmed by connection state, like the column copy). Sits between the BLE
   *     glyph and the caffeine mug; flex skips it while hidden so nothing shifts. --- */
  lbl_wifi_tray = lv_label_create(corner_tray);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(lbl_wifi_tray, &lv_font_montserrat_14, 0);   // narrow: shrink (fixed glyph)
#else
  lv_obj_set_style_text_font(lbl_wifi_tray, &UI_ICON_SYM(22), 0);
#endif
  lv_obj_set_style_text_color(lbl_wifi_tray, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);  // blue
  lv_label_set_text(lbl_wifi_tray, LV_SYMBOL_WIFI);
  lv_obj_add_flag(lbl_wifi_tray, LV_OBJ_FLAG_HIDDEN);

  /* --- Mute indicator: the empty/no-sound speaker glyph, shown only when the watch is
   *     muted. A gray at-a-glance cue, same treatment as the other tray icons (smaller
   *     fixed glyph on the narrow C6 so it doesn't clip; full size on the S3). Flex
   *     skips it while hidden so the tray collapses the gap. --- */
  lbl_mute_icon = lv_label_create(corner_tray);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(lbl_mute_icon, &lv_font_montserrat_14, 0);
#else
  lv_obj_set_style_text_font(lbl_mute_icon, &UI_ICON_SYM(22), 0);
#endif
  lv_obj_set_style_text_color(lbl_mute_icon, lv_color_hex(0x999999), 0);   // gray, like the mug
  lv_label_set_text(lbl_mute_icon, LV_SYMBOL_MUTE);                        // empty speaker / no sound
  lv_obj_add_flag(lbl_mute_icon, LV_OBJ_FLAG_HIDDEN);

  /* --- Caffeine (keep-awake) indicator: a GRAY coffee-outline glyph (Material Design
   *     Icons font), hidden unless caffeine is on. Last (rightmost) tray child, so it
   *     holds the corner. Was hand-built from shapes (crude at the small corner size);
   *     now the MDI glyph, sized to MATCH the WiFi/BLE icons (14px C6 / 22px S3) and
   *     crisply anti-aliased at both. --- */
  caf_ind = lv_label_create(corner_tray);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(caf_ind, &icons14, 0);
#else
  lv_obj_set_style_text_font(caf_ind, &UI_ICON_MDI(22), 0);
#endif
  lv_obj_set_style_text_color(caf_ind, lv_color_hex(0x999999), 0);   // gray
  { char u[5]; lv_label_set_text(caf_ind, mdi_utf8(MDI_COFFEE, u)); }
  lv_obj_add_flag(caf_ind, LV_OBJ_FLAG_HIDDEN);       // shown only when caffeine is on

  /* --- LEFT column: RED bell + unread count --- */
  // Outer stat columns sit further IN on a small round face (see make_stat_column):
  // at the raised TOP_Y the usable chord is narrower, so 22/78 would graze the arc.
#if BOARD_SCREEN_ROUND_SMALL
#define WF_STAT_L_PCT 24
#define WF_STAT_R_PCT 76
#else
#define WF_STAT_L_PCT 22
#define WF_STAT_R_PCT 78
#endif
  lv_obj_t *lcol = make_stat_column(scr, WF_STAT_L_PCT, TOP_Y);
  wf_lcol = lcol;                           // kept for watchface_set_minimal
  lbl_bell_icon = lv_label_create(lcol);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(lbl_bell_icon, &lv_font_montserrat_14, 0);   // narrow panel: smaller stat icon
#else
  lv_obj_set_style_text_font(lbl_bell_icon, &UI_ICON_SYM(22), 0);
#endif
  lv_obj_set_style_text_color(lbl_bell_icon, lv_color_hex(WF_BELL_BASE), 0);  // red (accent on e-paper)
  lv_label_set_text(lbl_bell_icon, LV_SYMBOL_BELL);
  lbl_bell_cnt = lv_label_create(lcol);
  lv_obj_set_style_text_font(lbl_bell_cnt, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_bell_cnt, lv_color_white(), 0);
  lv_label_set_text(lbl_bell_cnt, "0");

  /* --- MIDDLE column: custom battery widget (colored fill) + percent --- */
  lv_obj_t *mcol = make_stat_column(scr, 50, TOP_Y);
  wf_mcol = mcol;                           // kept for watchface_set_minimal
  batt_shell = lv_obj_create(mcol);
  lv_obj_remove_style_all(batt_shell);
  lv_obj_set_size(batt_shell, UI_PX(40), UI_PX(20));
  lv_obj_set_style_radius(batt_shell, UI_PX(3), 0);
  lv_obj_set_style_border_width(batt_shell, UI_PX(2), 0);
  lv_obj_set_style_border_color(batt_shell, lv_color_hex(0x999999), 0);
  lv_obj_set_style_pad_all(batt_shell, UI_PX(2), 0);
  lv_obj_clear_flag(batt_shell, LV_OBJ_FLAG_SCROLLABLE);
  // little positive terminal nub on the right end of the battery
  lv_obj_t *nub = lv_obj_create(batt_shell);
  lv_obj_remove_style_all(nub);
  lv_obj_set_size(nub, UI_PX(3), UI_PX(8));
  lv_obj_set_style_bg_color(nub, lv_color_hex(0x999999), 0);
  lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
  lv_obj_align(nub, LV_ALIGN_RIGHT_MID, UI_PX(5), 0);
  // inner fill bar: width = %, color = level (set in watchface_set_battery)
  batt_fill = lv_obj_create(batt_shell);
  lv_obj_remove_style_all(batt_fill);
  lv_obj_set_style_radius(batt_fill, 1, 0);
  lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
  lv_obj_set_height(batt_fill, LV_PCT(100));
  lv_obj_set_width(batt_fill, LV_PCT(100));
  lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE);

  lbl_batt_pct = lv_label_create(mcol);
  lv_obj_set_style_text_font(lbl_batt_pct, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_batt_pct, lv_color_white(), 0);
  lv_label_set_text(lbl_batt_pct, "--%");

  /* --- RIGHT column: BLUE wifi + status --- */
  lv_obj_t *rcol = make_stat_column(scr, WF_STAT_R_PCT, TOP_Y);
  wf_rcol = rcol;                           // kept for watchface_set_minimal
  lbl_wifi_icon = lv_label_create(rcol);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(lbl_wifi_icon, &lv_font_montserrat_14, 0);   // narrow panel: smaller stat icon
#else
  lv_obj_set_style_text_font(lbl_wifi_icon, &UI_ICON_SYM(22), 0);
#endif
  lv_obj_set_style_text_color(lbl_wifi_icon, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);  // blue
  lv_label_set_text(lbl_wifi_icon, LV_SYMBOL_WIFI);
  lbl_wifi_txt = lv_label_create(rcol);
  lv_obj_set_style_text_font(lbl_wifi_txt, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_wifi_txt, lv_color_white(), 0);
  lv_label_set_text(lbl_wifi_txt, "--");

  /* --- BLE indicator (column copy): the BLUE Bluetooth glyph + status text that
   *     takes the right stat column when the user swaps WiFi/BLE. Built into the SAME
   *     column as wifi; watchface_apply_indicator_layout() hides one pair and shows the
   *     other so the column holds exactly one indicator. Hidden by default. --- */
  lbl_ble_col_icon = lv_label_create(rcol);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(lbl_ble_col_icon, &lv_font_montserrat_14, 0);   // narrow panel: smaller stat icon
#else
  lv_obj_set_style_text_font(lbl_ble_col_icon, &UI_ICON_SYM(22), 0);
#endif
  lv_obj_set_style_text_color(lbl_ble_col_icon, lv_color_hex(ui_deco_hex(0x33A0FF)), 0);  // blue
  lv_label_set_text(lbl_ble_col_icon, LV_SYMBOL_BLUETOOTH);
  lv_obj_add_flag(lbl_ble_col_icon, LV_OBJ_FLAG_HIDDEN);
  lbl_ble_col_txt = lv_label_create(rcol);
  lv_obj_set_style_text_font(lbl_ble_col_txt, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_ble_col_txt, lv_color_white(), 0);
  lv_label_set_text(lbl_ble_col_txt, "--");
  lv_obj_add_flag(lbl_ble_col_txt, LV_OBJ_FLAG_HIDDEN);

  /* --- CENTER: enormous clock --- */
  lbl_time = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_time, &FONT_TIME, 0);
  lv_obj_set_style_text_color(lbl_time, lv_color_white(), 0);
  lv_label_set_text(lbl_time, "--:--");
  lv_obj_align(lbl_time, LV_ALIGN_CENTER, 0, 0);

  /* --- BELOW clock: weekday · date --- */
  lbl_weekday = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_weekday, &FONT_TOP, 0);
  lv_obj_set_style_text_color(lbl_weekday, lv_color_white(), 0);
  lv_label_set_text(lbl_weekday, "");
  lv_obj_align(lbl_weekday, LV_ALIGN_CENTER, 0, UI_PX(WF_WEEKDAY_Y_DEFAULT));

  lbl_date = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_date, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(lbl_date, lv_color_hex(0xAAAAAA), 0);
  lv_label_set_text(lbl_date, "");
  lv_obj_align_to(lbl_date, lbl_weekday, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(8));

  /* --- Under-dial WEATHER widget (optional) ---
   * Sits in the gap between the clock and the weekday/date at the SAME baseline the
   * date normally occupies (WF_WEEKDAY_Y_DEFAULT). When it's shown, the weekday/date
   * row drops to WF_WEEKDAY_Y_WX so the widget has the space — the shift is applied by
   * watchface_apply_weather_visible(), called at the end of build and on the toggle.
   * Built hidden; populated by watchface_refresh_weather(). */
  wf_weather = lv_obj_create(scr);
  lv_obj_remove_style_all(wf_weather);
  lv_obj_set_size(wf_weather, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(wf_weather, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(wf_weather, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(wf_weather, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(wf_weather, UI_PX(8), 0);
  lv_obj_align(wf_weather, LV_ALIGN_CENTER, 0, UI_PX(WF_WEATHER_Y));

  // Icon cluster: a fixed-size cell holding the two overlapped glyph layers so the
  // sun (behind) and cloud (front) can be offset relative to each other.
  lv_obj_t *iconcell = lv_obj_create(wf_weather);
  lv_obj_remove_style_all(iconcell);
  lv_obj_set_size(iconcell, UI_PX(50), UI_PX(50));   // room for the ±offset layer stack
  lv_obj_clear_flag(iconcell, LV_OBJ_FLAG_SCROLLABLE);

  wx_sun_layer = lv_label_create(iconcell);
  lv_obj_set_style_text_font(wx_sun_layer, &icons34, 0);
  lv_obj_set_style_text_color(wx_sun_layer, lv_color_hex(0xFFD60A), 0);   // yellow sun
  { char u[5]; lv_label_set_text(wx_sun_layer, mdi_utf8(MDI_WX_SUNNY, u)); }
  lv_obj_align(wx_sun_layer, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_flag(wx_sun_layer, LV_OBJ_FLAG_HIDDEN);

  wx_cloud_layer = lv_label_create(iconcell);
  lv_obj_set_style_text_font(wx_cloud_layer, &icons34, 0);
  lv_obj_set_style_text_color(wx_cloud_layer, lv_color_white(), 0);
  { char u[5]; lv_label_set_text(wx_cloud_layer, mdi_utf8(MDI_WX_CLOUDY, u)); }
  lv_obj_align(wx_cloud_layer, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

  wx_temp_lbl = lv_label_create(wf_weather);
  lv_obj_set_style_text_font(wx_temp_lbl, &FONT_TOP, 0);
  lv_obj_set_style_text_color(wx_temp_lbl, lv_color_white(), 0);
  lv_label_set_text(wx_temp_lbl, "--");

  watchface_apply_volt_visible();   // honor the saved show-voltage setting from boot
  watchface_apply_indicator_layout(); // honor the saved swap-WiFi/BLE setting from boot
  watchface_realign_volt();         // position the voltage label against the (empty) tray
  watchface_apply_weather_visible(); // show/hide the weather widget + shift date per the setting
  watchface_refresh_weather();       // fill it from the last-known snapshot
}

static void watchface_update(const RTC_DateTime &dt) {
  lv_label_set_text_fmt(lbl_time, "%02d:%02d", dt.getHour(), dt.getMinute());

  int wd = dt.getWeek();  // 0=Sun..6=Sat
  if (wd >= 0 && wd < 7) lv_label_set_text(lbl_weekday, WEEKDAYS[wd]);

  int mo = dt.getMonth();
  if (mo >= 1 && mo <= 12)
    lv_label_set_text_fmt(lbl_date, "%d %s %d", dt.getDay(), MONTHS[mo - 1], dt.getYear());
  lv_obj_align_to(lbl_date, lbl_weekday, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(8));
}

/* Update the MIDDLE battery widget + the top-right voltage.
 *   pct      : 0-100, or <0 if unknown/no battery
 *   charging : true when the cell is being charged
 *   mv       : voltage in mV, shown in the corner readout
 * The fill bar's WIDTH tracks % and its COLOR tracks level (green/yellow/red);
 * only the fill is tinted — the shell outline stays grey. */
static void watchface_set_battery(int pct, bool charging, uint16_t mv) {
  lv_label_set_text_fmt(lbl_volt, "%u.%02uV", mv / 1000, (mv % 1000) / 10);

  if (pct < 0 && !charging) {  // no battery / not readable AND not on USB
    lv_obj_set_width(batt_fill, LV_PCT(0));
    lv_label_set_text(lbl_batt_pct, "USB");
    return;
  }
  if (pct > 100) pct = 100;

  // While charging (USB attached) the %-from-voltage reads high and isn't the cell's
  // true charge, so show "USB" instead of a misleading number — but still fill the
  // icon (purple = charging) so it reads as "plugged in", not empty. A clamp keeps a
  // visible sliver even at a low/unknown %.
  int fill_pct = (pct < 0) ? 100 : (pct < 8 ? 8 : pct);
  lv_obj_set_width(batt_fill, LV_PCT(fill_pct));
  if (charging) lv_label_set_text(lbl_batt_pct, "USB");
  else          lv_label_set_text_fmt(lbl_batt_pct, "%d%%", pct);

  lv_color_t fill;
  if (charging)        fill = lv_color_hex(0x8080FF);   // purple while charging / on USB
  else if (pct >= 30)  fill = lv_color_hex(0x33CC55);   // green: healthy
  else if (pct >= 15)  fill = lv_color_hex(0xFFC233);   // yellow: middle
  else                 fill = lv_color_hex(0xFF4D4D);   // red: low
  lv_obj_set_style_bg_color(batt_fill, fill, 0);
}

/* Update the LEFT bell column with the unread notification count, and the RIGHT
 * wifi column with connection status. Called from the loop alongside battery. */
static void watchface_set_bell(uint32_t count) {
  if (count > 999) lv_label_set_text(lbl_bell_cnt, "999+");   // SD archive can be huge
  else             lv_label_set_text_fmt(lbl_bell_cnt, "%u", (unsigned)count);
  // Keep the bell RED always: dim/dark red when nothing unread, bright red when
  // there are notifications (pops without losing its color identity).
  lv_obj_set_style_text_color(lbl_bell_icon,
      count ? lv_color_hex(WF_BELL_UNREAD)     // bright red = unread
            : lv_color_hex(WF_BELL_IDLE), 0);  // dim dark red = idle (accent on e-paper)
}
/* Repaint the bell badge from the current unread count (see watch_base.h). Takes
 * the store lock because notif_unread() reads the shared notif counters that the
 * core-0 network task updates. UI-core only (touches LVGL via watchface_set_bell). */
static void watchface_refresh_bell(void) {
  store_lock();
  uint32_t u = notif_unread();
  store_unlock();
  watchface_set_bell(u);
}

/* Show/hide the top-right caffeine mug. Re-anchors the voltage label since the
 * tray's width changes when the mug appears/disappears. */
static void watchface_set_caffeine(bool on) {
  if (!caf_ind) return;
  if (on) lv_obj_clear_flag(caf_ind, LV_OBJ_FLAG_HIDDEN);
  else    lv_obj_add_flag(caf_ind, LV_OBJ_FLAG_HIDDEN);
  watchface_realign_volt();
}

/* Show/hide the top-right muted-speaker icon. Re-anchors the voltage label since the
 * tray's width changes when the icon appears/disappears. Reads the live mute setting. */
static void watchface_set_mute(bool muted) {
  if (!lbl_mute_icon) return;
  if (muted) lv_obj_clear_flag(lbl_mute_icon, LV_OBJ_FLAG_HIDDEN);
  else       lv_obj_add_flag(lbl_mute_icon, LV_OBJ_FLAG_HIDDEN);
  watchface_realign_volt();
}

/* Show/hide the top-right sleep-mode glyph. Re-anchors the voltage label since the
 * tray width changes when the icon appears/disappears. Driven from the loop off the
 * live s_sleep_mode setting (watchface_refresh_sleep_badge). */
static void watchface_set_sleep(bool on) {
  if (!lbl_sleep_icon) return;
  if (on) lv_obj_clear_flag(lbl_sleep_icon, LV_OBJ_FLAG_HIDDEN);
  else    lv_obj_add_flag(lbl_sleep_icon, LV_OBJ_FLAG_HIDDEN);
  watchface_realign_volt();
}

/* Last-known indicator states, so watchface_apply_indicator_layout() can repaint the
 * newly-shown copy correctly after a swap (the setters cache here; the layout fn replays). */
static bool wf_ble_connected = false;
static bool wf_wifi_connected = false;

/* Paint the BLE glyph in the corner tray: visible only when a phone is connected.
 * Used as BLE's home when NOT swapped. Null-safe. */
static void watchface_paint_ble_tray(bool connected) {
  if (!lbl_ble_icon) return;
  if (connected) lv_obj_clear_flag(lbl_ble_icon, LV_OBJ_FLAG_HIDDEN);
  else           lv_obj_add_flag(lbl_ble_icon, LV_OBJ_FLAG_HIDDEN);
  watchface_realign_volt();   // tray width changed -> re-anchor voltage so it makes room
}

/* Paint the BLE indicator in the right stat column (icon dim/bright + status text),
 * mirroring how wifi paints its column. Used as BLE's home when swapped. The column
 * copy stays present (icon dimmed) when disconnected, matching the wifi column's look,
 * rather than vanishing like the tray glyph. Null-safe. */
static void watchface_paint_ble_column(bool connected) {
  if (!lbl_ble_col_icon) return;
  lv_label_set_text(lbl_ble_col_txt, connected ? "ok" : "off");
  lv_obj_set_style_text_color(lbl_ble_col_icon,
      connected ? lv_color_hex(ui_deco_hex(0x33A0FF))    // bright blue = connected
                : lv_color_hex(ui_deco_dim_hex(0x123A5C)), 0); // dim dark blue = disconnected
}

/* Paint the WiFi indicator in the right stat column (its default home). */
static void watchface_paint_wifi_column(bool connected) {
  if (!lbl_wifi_icon) return;
  lv_label_set_text(lbl_wifi_txt, connected ? "ok" : "off");
  // Keep the wifi BLUE always: dim/dark blue when offline, bright blue when up.
  lv_obj_set_style_text_color(lbl_wifi_icon,
      connected ? lv_color_hex(ui_deco_hex(0x33A0FF))    // bright blue = connected
                : lv_color_hex(ui_deco_dim_hex(0x123A5C)), 0); // dim dark blue = offline
}

/* Paint the WiFi glyph in the corner tray (its home when swapped): visible ONLY when
 * connected, hidden otherwise — mirroring the BLE tray glyph. A corner tray icon is an
 * at-a-glance "this is active" cue, so an off/disconnected WiFi shouldn't sit dimmed in
 * the corner; it just disappears (and flex collapses the gap). Null-safe. */
static void watchface_paint_wifi_tray(bool connected) {
  if (!lbl_wifi_tray) return;
  if (connected) lv_obj_clear_flag(lbl_wifi_tray, LV_OBJ_FLAG_HIDDEN);
  else           lv_obj_add_flag(lbl_wifi_tray, LV_OBJ_FLAG_HIDDEN);
  watchface_realign_volt();   // tray width changed -> re-anchor voltage so it makes room
}

/* Apply the swap-WiFi/BLE layout: pick which indicator owns the right stat column and
 * which owns the top-right tray, hide the unused copy of each, and replay the cached
 * states so the now-visible copies are painted correctly. Re-anchors the voltage label
 * since the tray's visible-icon set (hence width) changes. Null-safe before build. */
static void watchface_apply_indicator_layout(void) {
  if (!lbl_wifi_icon || !lbl_ble_icon) return;     // face not built yet
  bool swap = settings_get_swap_wifi_ble();

  // Column: show wifi pair OR ble pair (never both).
  if (swap) {
    lv_obj_add_flag(lbl_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_wifi_txt,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_ble_col_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_ble_col_txt,  LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(lbl_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_wifi_txt,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_ble_col_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_ble_col_txt,  LV_OBJ_FLAG_HIDDEN);
  }

  // Tray: exactly one of the WiFi/BLE glyphs may live there at a time. Force-hide the
  // one that's NOT in the tray for this layout; the in-tray one's own visibility is
  // connection-driven and set by its paint helper in the replay below (both are
  // "visible only when connected"). This keeps the off/disconnected glyph from sitting
  // in the corner where the other layout's glyph belongs.
  if (swap) lv_obj_add_flag(lbl_ble_icon,   LV_OBJ_FLAG_HIDDEN);   // BLE leaves the tray
  else      lv_obj_add_flag(lbl_wifi_tray,  LV_OBJ_FLAG_HIDDEN);   // WiFi leaves the tray

  // Replay cached states into whichever copies are now live (this also sets the
  // in-tray glyph's connection-driven visibility).
  if (swap) {
    watchface_paint_ble_column(wf_ble_connected);
    watchface_paint_wifi_tray(wf_wifi_connected);
  } else {
    watchface_paint_ble_tray(wf_ble_connected);
    watchface_paint_wifi_column(wf_wifi_connected);
  }
  watchface_realign_volt();   // tray's visible icon set changed -> re-anchor voltage
}

/* Reflect a BLE phone connect/disconnect. Caches the state and paints whichever BLE
 * copy is currently live (tray glyph when default, right column when swapped).
 * Re-anchors the voltage label (tray width can change). */
static void watchface_set_ble(bool connected) {
  wf_ble_connected = connected;
  if (settings_get_swap_wifi_ble()) watchface_paint_ble_column(connected);
  else                              watchface_paint_ble_tray(connected);
  watchface_realign_volt();
}

/* Reflect WiFi connect/disconnect. Caches the state and paints whichever WiFi copy is
 * currently live (right column when default, tray glyph when swapped). */
static void watchface_set_wifi(bool connected) {
  wf_wifi_connected = connected;
  if (settings_get_swap_wifi_ble()) watchface_paint_wifi_tray(connected);
  else                              watchface_paint_wifi_column(connected);
}

/* Deep-dim "minimal face": when true, hide EVERYTHING on the watch face except the
 * center clock — the three stat columns (bell/battery/wifi), the date/weekday, the
 * corner voltage, and the indicator tray. On an AMOLED a hidden (black) pixel is a
 * pixel that's physically OFF, so blanking the periphery saves real power ON TOP of
 * the brightness drop the panel dim already does. When false, everything comes back.
 *
 * Only meaningful on the watch face itself — the loop gates this so it's never
 * called while a menu/app/shade is covering the face. Idempotent (tracks its own
 * state) and null-safe (no-op before watchface_create has built the widgets), so
 * the loop can call it every iteration cheaply. The clock (lbl_time) is never
 * touched, so the bare time stays readable while dimmed. */
static bool wf_minimal = false;

/* Apply the user's "show voltage readout" setting to the corner label — but never
 * show it while the minimal face is active (a dimmed face shows the clock only).
 * Single source of truth for lbl_volt's visibility, so the minimal-face restore
 * below can't override the user's choice to hide it. Null-safe. */
static void watchface_apply_volt_visible(void) {
  if (!lbl_volt) return;
  if (settings_get_show_volt() && !wf_minimal)
    lv_obj_clear_flag(lbl_volt, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(lbl_volt, LV_OBJ_FLAG_HIDDEN);
}

/* Show or hide the under-dial weather widget per settings_get_show_weather(), and
 * shift the weekday/date row down (to WF_WEEKDAY_Y_WX) when it's shown, back up (to
 * WF_WEEKDAY_Y_DEFAULT) when hidden. The date follows the weekday (it re-anchors to
 * it every watchface_update() and here), so only the weekday's Y needs setting.
 * In-place — only the widget + the two moved labels repaint, never the whole face.
 * Null-safe before the face is built (called from settings_set_show_weather too). */
static void watchface_apply_weather_visible(void) {
  if (!wf_weather || !lbl_weekday) return;
  bool show = settings_get_show_weather() && !wf_minimal;
  if (show) {
    lv_obj_clear_flag(wf_weather, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(lbl_weekday, LV_ALIGN_CENTER, 0, UI_PX(WF_WEEKDAY_Y_WX));
  } else {
    lv_obj_add_flag(wf_weather, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(lbl_weekday, LV_ALIGN_CENTER, 0, UI_PX(WF_WEEKDAY_Y_DEFAULT));
  }
  // Re-anchor the date under the (possibly moved) weekday.
  if (lbl_date) lv_obj_align_to(lbl_date, lbl_weekday, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(8));
}

/* Repaint the weather widget from the current weather_store snapshot: pick the glyph
 * + tint for the current condition, drive the two-tone sun/cloud layering, and set the
 * temperature. In-place (only these labels repaint). No-op when the widget is hidden or
 * the face isn't built. Called at build, on the toggle, and when a fetch lands. */
static void watchface_refresh_weather(void) {
  if (!wf_weather || !wx_cloud_layer || !wx_temp_lbl) return;
  if (!settings_get_show_weather()) return;          // hidden -> nothing to paint

  WeatherModel w;
  weather_get(&w);

  // ONE tinted glyph per condition, via the shared picker in app_weather.h (wx_pick_glyph)
  // so the dial matches the in-app icon exactly. The MDI weather glyphs are HOLLOW
  // outlines, so a second layer would just show through the middle — the back layer
  // (wx_sun_layer) stays hidden; wx_cloud_layer is the single glyph. The per-condition
  // TINT is what gives the set several colors (yellow sun, blue rain, white snow, ...).
  char u[5];
  uint32_t tint;
  uint32_t cp = wx_pick_glyph(w.cur_wmo, w.cur_is_day, &tint);
  lv_obj_add_flag(wx_sun_layer, LV_OBJ_FLAG_HIDDEN);      // unused; single glyph only
  lv_obj_set_style_text_color(wx_cloud_layer, lv_color_hex(ui_deco_hex(tint)), 0);
  lv_label_set_text(wx_cloud_layer, mdi_utf8(cp, u));
  lv_obj_align(wx_cloud_layer, LV_ALIGN_CENTER, 0, 0);

  if (w.have_current && w.cur_temp_c != INT16_MIN)
    lv_label_set_text_fmt(wx_temp_lbl, "%d\xC2\xB0", (int)w.cur_temp_c);
  else
    lv_label_set_text(wx_temp_lbl, "--");
}

static void watchface_set_minimal(bool minimal) {
  if (minimal == wf_minimal) return;        // debounce: only touch the tree on a transition
  wf_minimal = minimal;
  // Hide/show as whole containers so a single flag covers each column's children.
  // lbl_volt is handled separately (watchface_apply_volt_visible) because its
  // visibility ALSO depends on the user's show-voltage setting — a blind clear_flag
  // here would re-show it on un-dim even if the user turned it off. wf_weather is
  // likewise gated by its setting (watchface_apply_weather_visible below), so it's
  // NOT in this blind list — a clear_flag here would re-show it against the setting.
  lv_obj_t *parts[] = { wf_lcol, wf_mcol, wf_rcol, wf_corner_tray,
                        lbl_weekday, lbl_date };
  for (lv_obj_t *p : parts) {
    if (!p) continue;
    if (minimal) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);
  }
  watchface_apply_volt_visible();           // respects both minimal + the user setting
  watchface_apply_weather_visible();        // hide the weather widget in minimal, restore after
  if (!minimal) {
    watchface_realign_volt();               // tray is back -> re-anchor voltage to it
    watchface_refresh_weather();            // repaint the widget now it's visible again
  }
}
static bool watchface_is_minimal(void) { return wf_minimal; }
