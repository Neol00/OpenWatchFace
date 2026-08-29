/* ============================================================================
 *  app_weather.h — Weather app: current conditions + multi-day forecast + graph.
 *
 *  Reads the model in weather_store.h (populated by the WiFi fetch in notif_net.h
 *  and located by a BLE push / the compile-time preset). Two screens:
 *    - app_open_weather()          : current conditions (big icon, temp, condition,
 *                                    location, feels-like, humidity, wind) + a
 *                                    "Show on watch face" toggle + a Forecast button.
 *    - app_open_weather_forecast() : the next few days as a list + a hi/lo temp graph.
 *
 *  ICONS: ONE tinted MDI weather glyph per condition (wx_pick_glyph / wx_make_icon),
 *  icons88 for the big current icon, icons34 for the forecast rows. The MDI weather
 *  glyphs are hollow outlines, so we do NOT layer two (the back one shows through the
 *  middle) — the "several colors" come from the per-condition tint across the set
 *  (yellow sun, blue rain, white snow, ...). The watch-face dial widget (watchface.h)
 *  uses the SAME wx_pick_glyph() so the two always match.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER app_menu.h (screen shell +
 *  settings_toggle_row + nav_open), weather_store.h (the model), settings_store.h
 *  (the face-widget toggle), and notif_net.h (weather_request_fetch). app_open_weather
 *  is forward-declared in app_menu.h so the tile can dispatch to it.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

static void app_open_weather(void);
static void app_open_weather_forecast(void);

static int s_wx_fc_page = 0;   // current forecast page (remembered across in-place rebuilds)

/* Scroll preservation, but ONLY across an in-place rebuild (a Refresh landing repaints
 * the screen with fresh data). A FRESH open from the menu must start at the TOP, so the
 * restore is gated by s_wx_preserve_scroll: the loop sets it true right before a rebuild
 * (via weather_capture_scroll), and the builder consumes it. When it's false (fresh
 * open), the builder scrolls to the top instead. */
static lv_obj_t *s_wx_col            = nullptr;
static int       s_wx_scroll_y       = 0;
static bool      s_wx_preserve_scroll = false;

/* Save the live scroll offset AND arm the one-shot restore, so the next rebuild keeps
 * the position. Called from the loop before nav_current() re-runs the builder. */
static void weather_capture_scroll(void) {
  if (s_wx_col) s_wx_scroll_y = lv_obj_get_scroll_y(s_wx_col);
  s_wx_preserve_scroll = true;
}

/* Apply the scroll position at the end of a builder: restore the saved offset if a
 * rebuild armed it (one-shot), else scroll to the top for a fresh open. */
static void weather_apply_scroll(lv_obj_t *col) {
  lv_obj_update_layout(col);   // layout must be computed before scrolling
  lv_obj_scroll_to_y(col, s_wx_preserve_scroll ? s_wx_scroll_y : 0, LV_ANIM_OFF);
  s_wx_preserve_scroll = false;   // consume the one-shot; next open starts at the top
}

/* Short weekday labels for the forecast list. Local to this file on purpose: the
 * full WEEKDAYS[] lives in watchface.h, which is included AFTER this header in the
 * single-TU build, so its identifier isn't visible here yet. 3-letter abbreviations
 * also read better in a tight forecast row than the full day names. Indexed 0=Sun..6=Sat
 * to match RTC_DateTime::getWeek() (the same index stored in WeatherDay::wday). */
static const char *const WX_WDAY_ABBR[7] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/* Format a temperature (°C, INT16_MIN = unknown) into buf as e.g. "17°" or "--". */
static void wx_fmt_temp(int16_t c, char *buf, size_t n) {
  if (c == INT16_MIN) snprintf(buf, n, "--");
  else                snprintf(buf, n, "%d\xC2\xB0", (int)c);   // UTF-8 degree sign
}

/* Pick the SINGLE MDI glyph + tint for a WMO code. The MDI weather glyphs are HOLLOW
 * outline icons that are each already complete (the precip ones bake in their own cloud),
 * so we draw exactly ONE tinted glyph per condition — layering two hollow glyphs just
 * lets the back one show through the empty middle of the front one. The "several colors"
 * come from the per-condition TINT across the set (yellow sun, blue rain, white snow,
 * yellow bolt, ...), and MDI's own weather-partly-cloudy glyph is a designed sun+cloud in
 * one shape. is_day swaps clear sky between the sun and the moon. */
static uint32_t wx_pick_glyph(uint8_t wmo, bool is_day, uint32_t *tint) {
  uint32_t cp, ti;
  if (wmo == 0 && is_day)                             { cp = MDI_WX_SUNNY;     ti = 0xFFD60A; } // yellow sun
  else if (wmo == 0 || (wmo <= 2 && !is_day))         { cp = MDI_WX_NIGHT;     ti = 0xDDE3FF; } // moon
  else if (wmo <= 2)                                  { cp = MDI_WX_PARTLY;    ti = 0xE6E6E6; } // sun+cloud glyph
  else if (wmo == 3)                                  { cp = MDI_WX_CLOUDY;    ti = 0xC9C9C9; } // overcast
  else if (wmo >= 45 && wmo <= 48)                    { cp = MDI_WX_FOG;       ti = 0xB0B4BA; } // fog
  else if (wmo >= 51 && wmo <= 57)                    { cp = MDI_WX_RAINY;     ti = 0x5AB0FF; } // drizzle
  else if ((wmo >= 61 && wmo <= 67) || (wmo >= 80 && wmo <= 82)) { cp = MDI_WX_POURING; ti = 0x3D8BFF; } // rain
  else if ((wmo >= 71 && wmo <= 77) || (wmo >= 85 && wmo <= 86)) { cp = MDI_WX_SNOWY;   ti = 0xBFE3FF; } // snow
  else if (wmo >= 95)                                 { cp = MDI_WX_LIGHTNING; ti = 0xFFC93D; } // storm
  else                                                { cp = MDI_WX_CLOUDY;    ti = 0xC9C9C9; } // fallback
  if (tint) *tint = ti;
  return cp;
}

/* Build the weather icon: ONE tinted glyph in the given font. `glyph_h` is unused now
 * (kept in the signature so call sites don't change) — the label content-sizes itself. */
static lv_obj_t *wx_make_icon(lv_obj_t *parent, const lv_font_t *font,
                              uint8_t wmo, bool is_day, int glyph_h) {
  (void)glyph_h;
  uint32_t tint;
  uint32_t cp = wx_pick_glyph(wmo, is_day, &tint);
  lv_obj_t *g = lv_label_create(parent);
  lv_obj_set_style_text_font(g, font, 0);
  lv_obj_set_style_text_color(g, lv_color_hex(ui_deco_hex(tint)), 0);
  char u[5]; lv_label_set_text(g, mdi_utf8(cp, u));
  return g;
}

/* "Forecast" button -> the multi-day screen, starting at the first page. */
static void wx_forecast_btn_cb(lv_event_t *e) {
  (void)e;
  s_wx_fc_page = 0;                 // fresh entry always starts at day 1
  nav_open(app_open_weather_forecast);
}

/* "Refresh" button -> ask the net task to re-fetch now (result repaints on the loop).
 * Give immediate feedback by relabeling the button, so the multi-second fetch never
 * looks like a dead tap. The label reverts on the next screen rebuild (which the
 * loop's s_weather_ready consumer triggers when the fetch lands). */
static void wx_refresh_btn_cb(lv_event_t *e) {
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  if (lv_obj_get_child_count(btn) > 0) {
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_label_set_text(lbl, "Refreshing...");   // plain dots (…/U+2026 not in font)
  }
  weather_request_fetch();
}

/* ---------------------------------------------------------------------------
 * MAIN screen: current conditions.
 * ------------------------------------------------------------------------- */
static void app_open_weather(void) {
  app_screen_begin("Weather");

  WeatherModel w;
  weather_get(&w);

  // Scrollable column (matches Appearance/Power geometry).
  lv_obj_t *col = lv_obj_create(app_scr);
  s_wx_col = col;                 // for scroll-position preservation across rebuilds
#if BOARD_SCREEN_NARROW
  lv_obj_set_width(col, LV_PCT(92));
  lv_obj_set_height(col, (int)screenHeight - UI_PX(84));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(124));
  lv_obj_set_style_pad_all(col, UI_PX(6), 0);
#else
  // Width/height/offsets live in ui_app_column_layout() (ui_scale.h): the four
  // raw-pixel lines that used to sit here were authored on the 410x502 reference
  // and are wider than the glass on a 360 px round face. Shared so Power, Weather
  // and Appearance cannot drift apart again.
  ui_app_column_layout(col, 374);
#endif
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(col);
  // MAIN axis (vertical, 1st arg) = START: content that overflows grows DOWNWARD and
  // stays scrollable from the top. (CENTER here pushed the first items — Stockholm +
  // the icon — ABOVE the visible top, where they couldn't be scrolled to.) CROSS axis
  // (horizontal, 2nd arg) = CENTER so each row sits centered.
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, UI_PX(10), 0);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_pad_bottom(col, UI_PX(78), 0);
#else
  lv_obj_set_style_pad_bottom(col, UI_PX(56), 0);
#endif

  // Location name.
  lv_obj_t *loc = lv_label_create(col);
  lv_obj_set_style_text_font(loc, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(loc, lv_color_white(), 0);
  ui_label_single_line(loc);
  lv_obj_set_width(loc, LV_PCT(100));
  lv_obj_set_style_text_align(loc, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(loc, w.loc_name[0] ? w.loc_name : "(no location)");

  if (!w.have_current) {
    // Nothing fetched yet — explain + offer a refresh.
    lv_obj_t *msg = lv_label_create(col);
    lv_obj_set_style_text_font(msg, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, "No weather yet.\nConnect WiFi and refresh.");
  } else {
    // Big multi-color condition icon — native 88px weather font (icons88), no scaling.
    wx_make_icon(col, &icons88, w.cur_wmo, w.cur_is_day, UI_PX(88));

    // Temperature (large) + condition text.
    char tb[12]; wx_fmt_temp(w.cur_temp_c, tb, sizeof(tb));
    lv_obj_t *temp = lv_label_create(col);
    lv_obj_set_style_text_font(temp, &UI_FONT(48), 0);
    lv_obj_set_style_text_color(temp, lv_color_white(), 0);
    lv_label_set_text(temp, tb);

    lv_obj_t *cond = lv_label_create(col);
    lv_obj_set_style_text_font(cond, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(cond, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(cond, wx_text_for_wmo(w.cur_wmo));

    // Detail rows: feels-like, humidity, wind — only those the fetch delivered.
    char row[48];
    if (w.cur_feels_c != INT16_MIN) {
      char fb[12]; wx_fmt_temp(w.cur_feels_c, fb, sizeof(fb));
      lv_obj_t *l = lv_label_create(col);
      lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(0x9090A0), 0);
      snprintf(row, sizeof(row), "Feels like %s", fb);
      lv_label_set_text(l, row);
    }
    if (w.cur_humidity != 0xFF) {
      lv_obj_t *l = lv_label_create(col);
      lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(0x9090A0), 0);
      snprintf(row, sizeof(row), "Humidity %u%%", (unsigned)w.cur_humidity);
      lv_label_set_text(l, row);
    }
    if (w.cur_wind_kmh != 0xFF) {
      lv_obj_t *l = lv_label_create(col);
      lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(0x9090A0), 0);
      snprintf(row, sizeof(row), "Wind %u km/h", (unsigned)w.cur_wind_kmh);
      lv_label_set_text(l, row);
    }
  }

  // "Forecast" button (only meaningful if we have forecast days).
  if (w.fc_n > 0) {
    lv_obj_t *fbtn = lv_btn_create(col);
    lv_obj_set_width(fbtn, LV_PCT(100));
    lv_obj_set_style_bg_color(fbtn, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_shadow_width(fbtn, 0, 0);
    lv_obj_add_event_cb(fbtn, wx_forecast_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *fl = lv_label_create(fbtn);
    lv_obj_set_style_text_font(fl, &FONT_SMALL, 0);
    lv_label_set_text(fl, "Forecast");
    lv_obj_center(fl);
  }

  // "Refresh now" button.
  lv_obj_t *rbtn = lv_btn_create(col);
  lv_obj_set_width(rbtn, LV_PCT(100));
  lv_obj_set_style_bg_color(rbtn, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_shadow_width(rbtn, 0, 0);
  lv_obj_add_event_cb(rbtn, wx_refresh_btn_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *rl = lv_label_create(rbtn);
  lv_obj_set_style_text_font(rl, &FONT_SMALL, 0);
  lv_label_set_text(rl, "Refresh now");
  lv_obj_center(rl);

  // (The "show weather on the watch face" toggle now lives in the Appearance app,
  // under WATCH FACE, next to the voltage/indicator toggles.)

  // Top on a fresh open; preserve position across a Refresh-driven rebuild.
  weather_apply_scroll(col);
}

/* ---------------------------------------------------------------------------
 * FORECAST screen: multi-day list + hi/lo temp graph, PAGED WEATHER_FC_PER_PAGE
 * days at a time with < > arrows (10 days -> 2 pages of 5).
 * ------------------------------------------------------------------------- */

/* Deferred rebuild of the forecast screen (async): deleting app_scr while the arrow
 * button's CLICKED event is still dispatching is LVGL UB (the button lives inside
 * app_scr). lv_async_call runs it after the event unwinds — same guard the Appearance
 * screen uses for its toggle-driven rebuild. */
static void wx_fc_rebuild_async(void *p) {
  (void)p;
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  app_open_weather_forecast();
}

/* Arrow callbacks: step the page (user_data carries the delta -1/+1) and rebuild in
 * place. Rebuild via app_open_weather_forecast() (not nav_open) so the back-stack is
 * untouched — it's an in-place refresh, so BOOT still leaves straight to the main screen. */
static void wx_fc_page_cb(lv_event_t *e) {
  int delta = (int)(intptr_t)lv_event_get_user_data(e);
  s_wx_fc_page += delta;
  lv_async_call(wx_fc_rebuild_async, nullptr);
}

static void app_open_weather_forecast(void) {
  app_screen_begin("Forecast");

  WeatherModel w;
  weather_get(&w);

  if (w.fc_n == 0) {
    lv_obj_t *msg = lv_label_create(app_scr);
    lv_obj_set_style_text_font(msg, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(msg, "No forecast data.");
    lv_obj_center(msg);
    return;
  }

  // Page bounds. Clamp the remembered page to what the current data supports (a
  // shorter fetch could shrink the page count between visits).
  const int per   = WEATHER_FC_PER_PAGE;
  const int pages = (w.fc_n + per - 1) / per;
  if (s_wx_fc_page < 0)       s_wx_fc_page = 0;
  if (s_wx_fc_page >= pages)  s_wx_fc_page = pages - 1;
  const int first = s_wx_fc_page * per;
  int last = first + per; if (last > w.fc_n) last = w.fc_n;   // exclusive

  lv_obj_t *col = lv_obj_create(app_scr);
  s_wx_col = col;                 // for scroll-position preservation across rebuilds
#if BOARD_SCREEN_NARROW
  lv_obj_set_width(col, LV_PCT(92));
  lv_obj_set_height(col, (int)screenHeight - UI_PX(84));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(104));
  lv_obj_set_style_pad_all(col, UI_PX(6), 0);
#else
  // Width/height/offsets live in ui_app_column_layout() (ui_scale.h): the four
  // raw-pixel lines that used to sit here were authored on the 410x502 reference
  // and are wider than the glass on a 360 px round face. Shared so Power, Weather
  // and Appearance cannot drift apart again.
  ui_app_column_layout(col, 374);
#endif
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(col);
  // MAIN axis (vertical) = START so overflow scrolls from the top (CENTER hid the top rows).
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, UI_PX(8), 0);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_pad_bottom(col, UI_PX(78), 0);
#else
  lv_obj_set_style_pad_bottom(col, UI_PX(56), 0);
#endif

  // --- Pager header: [ < ]  "Days N–M of K"  [ > ] --- (only if >1 page).
  if (pages > 1) {
    lv_obj_t *pager = lv_obj_create(col);
    lv_obj_remove_style_all(pager);
    lv_obj_set_width(pager, LV_PCT(100));
    lv_obj_set_height(pager, LV_SIZE_CONTENT);
    lv_obj_clear_flag(pager, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // "<" — disabled/hidden on the first page.
    lv_obj_t *prev = lv_btn_create(pager);
    lv_obj_set_size(prev, UI_PX(48), UI_PX(40));
    lv_obj_set_style_bg_color(prev, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_t *pl = lv_label_create(prev);
    lv_obj_set_style_text_font(pl, &FONT_LABEL, 0);   // Montserrat: carries LV_SYMBOL glyphs
    lv_label_set_text(pl, LV_SYMBOL_LEFT);
    lv_obj_center(pl);
    if (s_wx_fc_page > 0)
      lv_obj_add_event_cb(prev, wx_fc_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    else
      lv_obj_add_state(prev, LV_STATE_DISABLED);

    lv_obj_t *rng = lv_label_create(pager);
    lv_obj_set_style_text_font(rng, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(rng, lv_color_hex(0xCCCCCC), 0);
    char rb[24]; snprintf(rb, sizeof(rb), "Days %d-%d of %d", first + 1, last, w.fc_n);
    lv_label_set_text(rng, rb);

    // ">" — disabled on the last page.
    lv_obj_t *next = lv_btn_create(pager);
    lv_obj_set_size(next, UI_PX(48), UI_PX(40));
    lv_obj_set_style_bg_color(next, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_t *nl = lv_label_create(next);
    lv_obj_set_style_text_font(nl, &FONT_LABEL, 0);   // Montserrat: carries LV_SYMBOL glyphs
    lv_label_set_text(nl, LV_SYMBOL_RIGHT);
    lv_obj_center(nl);
    if (s_wx_fc_page < pages - 1)
      lv_obj_add_event_cb(next, wx_fc_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)+1);
    else
      lv_obj_add_state(next, LV_STATE_DISABLED);
  }

  // One row per day on THIS page, laid out with ABSOLUTE aligns inside a no-layout row
  // (mirrors app_files.h fm_row, which renders left-aligned): [icon] [Weekday] ... [hi/lo].
  // Bigger icon than before (icons34, ~2.5x the old icons22) sitting in a fixed left slot.
  for (int i = first; i < last; i++) {
    const WeatherDay *d = &w.fc[i];
    lv_obj_t *rowc = lv_obj_create(col);
    lv_obj_remove_style_all(rowc);
    lv_obj_set_width(rowc, LV_PCT(100));
    lv_obj_set_height(rowc, UI_PX(58));
    lv_obj_set_style_bg_color(rowc, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(rowc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rowc, UI_PX(10), 0);
    lv_obj_set_style_pad_hor(rowc, UI_PX(10), 0);
    lv_obj_clear_flag(rowc, LV_OBJ_FLAG_SCROLLABLE);

    // Icon: fixed left slot, vertically centered. Forecast is a daily summary -> day form.
    lv_obj_t *ic = wx_make_icon(rowc, &icons34, d->wmo, true, UI_PX(34));
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

    // Weekday: LEFT-aligned, right of the icon slot (NOT centered).
    lv_obj_t *wd = lv_label_create(rowc);
    lv_obj_set_style_text_font(wd, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(wd, lv_color_white(), 0);
    lv_label_set_text(wd, (d->wday < 7) ? WX_WDAY_ABBR[d->wday] : "");
    lv_obj_align(wd, LV_ALIGN_LEFT_MID, UI_PX(54), 0);   // clears the ~46px icon box

    // Temps: RIGHT-aligned.
    char hb[12], lb[12];
    wx_fmt_temp(d->hi_c, hb, sizeof(hb));
    wx_fmt_temp(d->lo_c, lb, sizeof(lb));
    char tt[28]; snprintf(tt, sizeof(tt), "%s / %s", hb, lb);
    lv_obj_t *tl = lv_label_create(rowc);
    lv_obj_set_style_text_font(tl, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(0xCCCCCC), 0);
    lv_label_set_text(tl, tt);
    lv_obj_align(tl, LV_ALIGN_RIGHT_MID, 0, 0);
  }

  // Hi/lo temperature graph across THIS page's days (two line series).
  const int npage = last - first;
  lv_obj_t *glbl = lv_label_create(col);
  lv_obj_set_style_text_font(glbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(glbl, lv_color_hex(0x808080), 0);
  lv_label_set_text(glbl, "High / low (\xC2\xB0""C)");

  lv_obj_t *chart = lv_chart_create(col);
  lv_obj_set_width(chart, LV_PCT(96));
  lv_obj_set_height(chart, UI_PX(120));
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(chart, 4, npage);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, UI_PX(4), 0);
  lv_obj_set_style_line_color(chart, lv_color_hex(0x303030), LV_PART_MAIN);
  lv_obj_set_style_width(chart,  UI_PX(4), LV_PART_INDICATOR);   // point markers (w x h)
  lv_obj_set_style_height(chart, UI_PX(4), LV_PART_INDICATOR);

  // Y range: pad the observed hi/lo (THIS page) by a couple of degrees.
  int16_t ymin = 100, ymax = -100;
  for (int i = first; i < last; i++) {
    if (w.fc[i].lo_c != INT16_MIN && w.fc[i].lo_c < ymin) ymin = w.fc[i].lo_c;
    if (w.fc[i].hi_c != INT16_MIN && w.fc[i].hi_c > ymax) ymax = w.fc[i].hi_c;
  }
  if (ymin > ymax) { ymin = 0; ymax = 20; }   // no valid points -> a sane default range
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, ymin - 2, ymax + 2);
  lv_chart_set_point_count(chart, npage);

  lv_chart_series_t *hs = lv_chart_add_series(chart, lv_color_hex(0xFF7043),  // hi = warm
                                              LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_series_t *ls = lv_chart_add_series(chart, lv_color_hex(0x42A5F5),  // lo = cool
                                              LV_CHART_AXIS_PRIMARY_Y);
  for (int i = first; i < last; i++) {
    lv_chart_set_next_value(chart, hs,
        w.fc[i].hi_c == INT16_MIN ? LV_CHART_POINT_NONE : (lv_coord_t)w.fc[i].hi_c);
    lv_chart_set_next_value(chart, ls,
        w.fc[i].lo_c == INT16_MIN ? LV_CHART_POINT_NONE : (lv_coord_t)w.fc[i].lo_c);
  }
  lv_chart_refresh(chart);

  // Top on a fresh open / page flip; preserve position across a Refresh-driven rebuild.
  weather_apply_scroll(col);
}
