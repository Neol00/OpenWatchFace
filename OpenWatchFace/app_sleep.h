/* ============================================================================
 *  app_sleep.h — Sleep sub-app: overnight sleep-quality tracking + Do-Not-Disturb.
 *
 *  A single big Start/Stop pill (like Fitness) toggles the sleep-tracking session
 *  (sleep_track.h). While active:
 *    - the IMU keeps sampling through deep sleep and each periodic wake logs a
 *      movement row to /sleep.csv (the raw timeline; scoring comes later);
 *    - Do-Not-Disturb is in effect: notifications won't wake the screen or pop a
 *      card. Alarms + timers still ring normally.
 *
 *  Mutually exclusive with the Fitness step counter (both own the IMU/ULP): Start
 *  refuses while step counting is running and tells the user to stop it first.
 *
 *  "View data" opens app_open_sleep_data: the FULL history list of every night (parsed
 *  from /sleep.csv by sleep_track.h), newest first, paged at 100/page with prev/next when
 *  longer. Each row = date + duration + restful%. A "Trends" button opens
 *  app_open_sleep_trends: a 3/7/30-night restful%-per-night bar chart + the average over
 *  the window. Tapping a list row opens app_open_sleep_night: Restful%/Duration tiles + a
 *  bar chart of movement through that night (sleep_night_load_points downsamples).
 *
 *  Header-only; INCLUDE AFTER app_menu.h (app_screen_begin / app_scr), sleep_track.h
 *  (the session API), imu_steps.h, settings_store.h (ui_accent_hex) and FONT_*.
 * ========================================================================== */
#pragma once
#include <lvgl.h>
#include <time.h>

/* C6 (JD9853 SPI) PARTIAL-render workaround. When the screen tree changes OUTSIDE the normal
 * full-screen redraw — rows streamed in by the loader's lv_timer, or a chart whose geometry the
 * flex/align engine only resolves on a later pass — the per-object invalidate records the wrong
 * (pre-layout) dirty area, so partial mode never flushes the change. The result is "Loading..."
 * that never updates, or a chart screen that paints as garbage. The fix (proven by the boot code
 * in setup()'s BOARD_DISPLAY_JD9853_SPI block) is to resolve layout, then invalidate the WHOLE
 * active screen, plus a deferred re-invalidate on the NEXT handler once any late geometry has
 * settled. No-op on the S3 (CO5300/QSPI direct path repaints fine on its own). */
static void slp_c6_force_repaint(void) {
#if BOARD_DISPLAY_SPIDMA
  lv_obj_t *scr = lv_screen_active();
  if (!scr) return;
  lv_obj_update_layout(scr);
  lv_obj_invalidate(scr);
  lv_async_call([](void *) { lv_obj_t *s = lv_screen_active(); if (s) lv_obj_invalidate(s); }, nullptr);
#endif
}

static lv_obj_t  *slp_btn_lbl = nullptr;   // "Start"/"Stop" caption
static lv_obj_t  *slp_status  = nullptr;   // status line

static void slp_set_status(const char *txt, uint32_t color) {
  if (!slp_status) return;
  lv_label_set_text(slp_status, txt);
  lv_obj_set_style_text_color(slp_status, lv_color_hex(color), 0);
}

/* Reflect the current session state onto the button + status line. */
static void slp_refresh(void) {
  bool on = sleep_track_active();
  if (slp_btn_lbl) lv_label_set_text(slp_btn_lbl, on ? "Stop" : "Start");
  if (on)
    slp_set_status("Tracking sleep. Notifications are silenced; alarms still ring.",
                   ui_accent_hex());
  else
    slp_set_status("Tap Start at bedtime to track your sleep.", 0xAAAAAA);
}

static void slp_cleanup_cb(lv_event_t *e) {
  (void)e;
  slp_btn_lbl = nullptr;
  slp_status  = nullptr;
}

/* Sleep-data screen state (declared here, ahead of the first callback that uses it). */
static int      slp_trend_range = 7;   // trends window: 3 / 7 / 30 nights
static int      slp_page        = 0;   // current list page (0 = newest)
static uint32_t slp_sel_start   = 0;   // start_epoch of the night opened in detail

static void slp_view_data_cb(lv_event_t *e) {
  (void)e;
  slp_page = 0;                      // always open the list at the newest page
  nav_open(app_open_sleep_data);
}

/* Start/Stop toggle. */
static void slp_toggle_cb(lv_event_t *e) {
  (void)e;
  if (!sleep_track_active()) {
    // Mutual exclusion: the Fitness step counter also owns the IMU/ULP.
    if (imu_steps_running()) {
      slp_set_status("Stop the Fitness step counter first.", 0xFF9F0A);
      return;
    }
    sleep_track_start();
  } else {
    sleep_track_stop();
  }
  slp_refresh();
}

static void app_open_sleep(void) {
  app_screen_begin("Sleep");

  bool have = imu_steps_available();
  // Mutual exclusion: sleep tracking and the step counter share the one accel/ULP, so only one
  // may run. If the step counter is active, Sleep is BLOCKED — grey the Start button + show a
  // note to turn steps off first. "View data" stays available (past nights are still viewable).
  bool blocked = have && imu_steps_running();
  bool usable  = have && !blocked;

  // Title icon up top: the MDI moon (sleep) glyph in the 34px icons34 font, tinted to
  // match the Sleep app accent.
  lv_obj_t *icon = lv_label_create(app_scr);
  lv_obj_set_style_text_font(icon, &icons34, 0);
  lv_obj_set_style_text_color(icon, lv_color_hex(ui_deco_hex(0x9B8CFF)), 0);
  { char u[5]; lv_label_set_text(icon, mdi_utf8(MDI_SLEEP, u)); }
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, UI_PX(-72));

  // --- Start/Stop button (matches the Fitness pill sizing per board). ---
  lv_obj_t *btn = lv_btn_create(app_scr);
#if BOARD_SCREEN_NARROW
  lv_obj_set_size(btn, LV_PCT(82), UI_PX(80));
#else
  lv_obj_set_size(btn, LV_PCT(60), UI_PX(64));
#endif
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, UI_PX(0));
  lv_obj_set_style_radius(btn, UI_PX(16), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(usable ? ui_accent_hex() : 0x3A3A3A), 0);
  if (usable) lv_obj_add_event_cb(btn, slp_toggle_cb, LV_EVENT_CLICKED, nullptr);
  else        lv_obj_add_state(btn, LV_STATE_DISABLED);

  slp_btn_lbl = lv_label_create(btn);
  lv_obj_set_style_text_font(slp_btn_lbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(slp_btn_lbl, usable ? lv_color_black() : lv_color_hex(0x777777), 0);
  lv_label_set_text(slp_btn_lbl, "Start");
  lv_obj_center(slp_btn_lbl);

  // --- Status line (wraps on narrow panels, like Fitness). ---
  slp_status = lv_label_create(app_scr);
  lv_obj_set_style_text_font(slp_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_align(slp_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(slp_status, LV_LABEL_LONG_WRAP);
#if BOARD_SCREEN_NARROW
  lv_obj_set_width(slp_status, LV_PCT(96));
#else
  lv_obj_set_width(slp_status, LV_PCT(80));
#endif
  lv_obj_align_to(slp_status, btn, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(18));

  if (!have) {
    slp_set_status("Motion sensor not available on this device.", 0xFF9F0A);
  } else if (blocked) {
    // Step counter owns the accel — sleep tracking is disabled until it's turned off.
    slp_set_status("Step counter is on.\nTurn off Fitness to track sleep.", 0xFF9F0A);
  } else {
    slp_refresh();
  }

  // --- "View data" button: opens the sleep-history screen (charts + per-night stats). ---
  lv_obj_t *vbtn = lv_btn_create(app_scr);
#if BOARD_SCREEN_NARROW
  lv_obj_set_size(vbtn, LV_PCT(66), UI_PX(56));
#else
  lv_obj_set_size(vbtn, LV_PCT(40), UI_PX(48));
#endif
  lv_obj_align_to(vbtn, slp_status, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(16));
  lv_obj_set_style_radius(vbtn, UI_PX(12), 0);
  lv_obj_set_style_shadow_width(vbtn, 0, 0);
  lv_obj_set_style_bg_color(vbtn, lv_color_hex(0x3A3A3A), 0);
  lv_obj_add_event_cb(vbtn, slp_view_data_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *vlbl = lv_label_create(vbtn);
  lv_obj_set_style_text_font(vlbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(vlbl, lv_color_white(), 0);
  lv_label_set_text(vlbl, "View data");
  lv_obj_center(vlbl);

  lv_obj_add_event_cb(app_scr, slp_cleanup_cb, LV_EVENT_DELETE, nullptr);
}

/* ============================================================================
 *  Sleep DATA screens:
 *    app_open_sleep_data   — the full, paged history list (all nights) + a Trends button.
 *    app_open_sleep_trends — restful%-per-night chart + average over a 3/7/30 window.
 *    app_open_sleep_night  — per-night detail (movement graph + stats).
 * ========================================================================== */

/* Format a night's date as "Mon 17 Jun" into buf. Uses the night's END epoch (when you
 * woke) so a session that crossed midnight reads as the morning you got up. */
static void slp_fmt_date(uint32_t epoch, char *buf, size_t n) {
  time_t t = (time_t)epoch;
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(buf, n, "%a %d %b", &lt);
}
/* Format "Hh Mm" duration. */
static void slp_fmt_dur(uint32_t minutes, char *buf, size_t n) {
  if (minutes >= 60) snprintf(buf, n, "%uh %um", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
  else               snprintf(buf, n, "%um", (unsigned)minutes);
}
/* Color for a restful %: green high, amber mid, red low. */
static uint32_t slp_quality_color(uint8_t pct) {
  if (pct >= 80) return 0x32D74B;
  if (pct >= 55) return 0xFF9F0A;
  return 0xFF453A;
}

/* Add a Y-axis SCALE to a chart: `ticks` value labels (top=max .. bottom=min) placed
 * OUTSIDE the chart's left edge so they never overlap the bars. The caller must leave a
 * left gutter for them: narrow the chart by SLP_AXIS_GUTTER and shift it right by the same
 * (see slp_chart_make_room), so this function just drops the labels into that empty strip.
 *   suffix: appended to each number (e.g. "%" for the trends chart, "" for movement).
 * Call AFTER the chart's geometry is set (size + align) and its range is applied. */
#define SLP_AXIS_GUTTER  UI_PX(36)     // width of the label strip to the LEFT of the chart

/* Narrow a chart by the axis gutter and nudge it right, freeing a strip on its left for
 * the Y-axis labels. Call right after sizing/aligning the chart, BEFORE add_yaxis. */
static void slp_chart_make_room(lv_obj_t *chart) {
  lv_obj_update_layout(chart);
  int w = lv_obj_get_width(chart);
  int neww = w - SLP_AXIS_GUTTER;
  if (neww < UI_PX(40)) neww = UI_PX(40);
  lv_obj_set_width(chart, neww);
  lv_obj_set_style_translate_x(chart, SLP_AXIS_GUTTER / 2, 0);
  lv_obj_update_layout(chart);
}

static void slp_chart_add_yaxis(lv_obj_t *chart, long vmin, long vmax,
                                int ticks, const char *suffix) {
  if (!chart || ticks < 2) return;
  lv_obj_update_layout(chart);                 // resolve real geometry before placing labels
  int ch = lv_obj_get_height(chart);
  for (int i = 0; i < ticks; i++) {
    // i=0 -> top (vmax), i=ticks-1 -> bottom (vmin).
    long val = vmax - (long)((vmax - vmin) * i / (ticks - 1));
    int  y   = (ch - 1) * i / (ticks - 1);     // pixel offset from the chart's top

    lv_obj_t *t = lv_label_create(lv_obj_get_parent(chart));
    lv_obj_set_style_text_font(t, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8890A0), 0);
    lv_label_set_text_fmt(t, "%ld%s", val, suffix ? suffix : "");
    lv_obj_set_width(t, LV_SIZE_CONTENT);
    lv_obj_update_layout(t);
    int lh = lv_obj_get_height(t);
    int lw = lv_obj_get_width(t);
    lv_obj_align_to(t, chart, LV_ALIGN_TOP_LEFT, -(lw + UI_PX(6)), y - lh / 2);
  }
}

/* A night row was tapped -> remember its start epoch + open the detail graph. */
static void slp_night_row_cb(lv_event_t *e) {
  slp_sel_start = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  nav_open(app_open_sleep_night);
}

static void slp_open_trends_cb(lv_event_t *e) { (void)e; nav_open(app_open_sleep_trends); }

/* ---- Incremental (async) list load -----------------------------------------
 * The CSV is read NEWEST-first by a backward cursor (sleep_track.h) pumped by an
 * lv_timer a few nights per tick, so the screen paints instantly and rows pop in while
 * the UI stays responsive. State below tracks the in-flight load for the data screen. */
static sleep_rev_cursor slp_rev;          // backward read cursor (open while loading)
static lv_timer_t *slp_load_timer = nullptr;
static lv_obj_t   *slp_list       = nullptr;   // the scroll container rows are added to
static lv_obj_t   *slp_empty_lbl  = nullptr;   // "No sleep recorded yet" placeholder (removed on 1st row)
static lv_obj_t   *slp_pnum_lbl   = nullptr;   // "Page X" label (updated as we learn there's more)
static lv_obj_t   *slp_next_btn   = nullptr;   // next-page button (enabled once a next page is known)
static int      slp_load_skip   = 0;   // nights still to skip before this page's first row
static int      slp_load_added  = 0;   // rows appended to the list so far this page
static bool     slp_load_active = false;
static uint32_t slp_load_until  = 0;   // millis() before which we only show "Loading..." (no rows)
#define SLP_LOAD_HOLD_MS 150           // wall-clock window for the "Loading..." label to paint

/* Stop the in-flight load (timer + open file). Safe to call repeatedly. */
static void slp_load_cancel(void) {
  if (slp_load_timer) { lv_timer_del(slp_load_timer); slp_load_timer = nullptr; }
  if (slp_load_active) { sleep_rev_end(&slp_rev); slp_load_active = false; }
}

/* Build one list row for a night summary and append it to slp_list. */
static void slp_add_night_row(const sleep_night_t *n) {
  if (slp_empty_lbl) { lv_obj_del(slp_empty_lbl); slp_empty_lbl = nullptr; }

  uint8_t  pct  = sleep_night_restful_pct(n);
  uint32_t mins = sleep_night_minutes(n);
  char dbuf[20], durbuf[16];
  slp_fmt_date(n->end_epoch, dbuf, sizeof(dbuf));
  slp_fmt_dur(mins, durbuf, sizeof(durbuf));

  lv_obj_t *row = lv_btn_create(slp_list);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(row, UI_PX(56), 0);
  lv_obj_set_style_radius(row, UI_PX(12), 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_pad_hor(row, UI_PX(12), 0);
  lv_obj_set_style_pad_ver(row, UI_PX(8), 0);
  lv_obj_add_event_cb(row, slp_night_row_cb, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)n->start_epoch);

  // Left: date (top) + duration (below, dim).
  lv_obj_t *date = lv_label_create(row);
  lv_obj_set_style_text_font(date, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(date, lv_color_white(), 0);
  lv_label_set_text(date, dbuf);
  lv_obj_align(date, LV_ALIGN_LEFT_MID, 0, UI_PX(-9));
  lv_obj_t *dur = lv_label_create(row);
  lv_obj_set_style_text_font(dur, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(dur, lv_color_hex(0x9090A0), 0);
  lv_label_set_text(dur, durbuf);
  lv_obj_align(dur, LV_ALIGN_LEFT_MID, 0, UI_PX(12));

  // Right: big restful % in its quality color.
  lv_obj_t *q = lv_label_create(row);
  lv_obj_set_style_text_font(q, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(q, lv_color_hex(slp_quality_color(pct)), 0);
  lv_label_set_text_fmt(q, "%u%%", (unsigned)pct);
  lv_obj_align(q, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* Timer callback: emit a few nights per tick. We over-skip earlier pages, fill this page
 * up to SLEEP_PAGE rows, then peek one more night to know if a NEXT page exists. */
static void slp_pump_night(const sleep_night_t *n, void *ud) {
  (void)ud;
  if (slp_load_skip > 0) { slp_load_skip--; return; }   // belongs to a newer (earlier-shown) page
  if (slp_load_added < SLEEP_PAGE) {
    slp_add_night_row(n);
    slp_load_added++;
  } else {
    // One extra night beyond the page => there IS a next page. Enable Next + reveal the
    // page bar (it starts hidden on page 0 when we don't yet know paging is needed).
    if (slp_next_btn) {
      lv_obj_clear_state(slp_next_btn, LV_STATE_DISABLED);
      lv_obj_set_style_bg_color(slp_next_btn, lv_color_hex(0x2A2A2A), 0);
      lv_obj_t *nl = lv_obj_get_child(slp_next_btn, 0);
      if (nl) lv_obj_set_style_text_color(nl, lv_color_hex(0xFFFFFF), 0);
      lv_obj_t *pbar = lv_obj_get_parent(slp_next_btn);
      if (pbar) lv_obj_clear_flag(pbar, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void slp_load_tick(lv_timer_t *t) {
  (void)t;
  if (!slp_load_active) { slp_load_cancel(); return; }
  // Hold off the first row by a WALL-CLOCK window so the "Loading..." label is actually
  // rendered before rows replace it. The display flush is async/DMA and the loop runs many
  // times in this window, so real frames get pushed. A tick-count hold was unreliable: when
  // a loop iteration overruns the timer period, LVGL runs several ticks back-to-back with no
  // flush between, draining the count before any frame. Page 0 (no skip phase) hit this and
  // never showed the label; page 2+ only worked because skipping delayed the first row.
  if (slp_load_until && (int32_t)(slp_load_until - millis()) > 0) return;
  // Pump a small batch of nights; the UI runs between ticks. Skipping is cheap (no row
  // built), so take bigger bites while still skipping earlier pages.
  int batch = (slp_load_skip > 0) ? 24 : 6;
  int before = slp_load_added;
  sleep_rev_step(&slp_rev, batch, slp_pump_night, nullptr);

  // If this batch actually ADDED rows, force the layout to settle and repaint NOW. On the C6
  // (JD9853 PARTIAL render) content laid out by flex/LV_SIZE_CONTENT is only resolved on a LATER
  // handler pass, so the invalidate from merely adding a child records the wrong/empty dirty area
  // -> the rows are never flushed and the screen reads "Loading..." forever until some unrelated
  // full repaint (opening the quick shade) finally pushes them. Invalidating the SUB-OBJECT alone
  // proved insufficient; invalidate the WHOLE active screen (the proven boot-time workaround in
  // the BOARD_DISPLAY_JD9853_SPI block of setup). Harmless on the S3.
  if (slp_load_added != before) slp_c6_force_repaint();

  if (slp_rev.done) {
    // Whole file consumed. If this page came up empty, we flipped past the end — drop back.
    if (slp_load_added == 0) {
      if (slp_page > 0) { slp_load_cancel(); slp_page--; app_open_sleep_data(); return; }
      if (slp_empty_lbl) {
        lv_label_set_text(slp_empty_lbl,
                          "No sleep recorded yet.\nTrack a night to see data here.");
        slp_c6_force_repaint();   // same C6 full-screen repaint — the text change must flush
      }
    }
    slp_load_cancel();
    return;
  }

  // Page filled: one more step peeks the next night (slp_pump_night enables Next), then stop.
  if (slp_load_added >= SLEEP_PAGE) {
    sleep_rev_step(&slp_rev, 1, slp_pump_night, nullptr);
    slp_load_cancel();
  }
}

/* List paging: prev/next flip the page and rebuild in place (no nav push). */
static void slp_page_prev_cb(lv_event_t *e) {
  (void)e;
  if (slp_page > 0) { slp_page--; app_open_sleep_data(); }
}
static void slp_page_next_cb(lv_event_t *e) {
  (void)e;
  // Next is only enabled when the loader found a night past this page, so it's safe to flip.
  if (slp_next_btn && !lv_obj_has_state(slp_next_btn, LV_STATE_DISABLED)) {
    slp_page++; app_open_sleep_data();
  }
}

/* Trends window selector (3/7/30) -> rebuild the trends screen in place. */
static void slp_trend_range_cb(lv_event_t *e) {
  slp_trend_range = (int)(intptr_t)lv_event_get_user_data(e);
  app_open_sleep_trends();
}

static void slp_data_cleanup_cb(lv_event_t *e) {
  (void)e;
  slp_load_cancel();              // tear down the timer + open file if we leave mid-load
  slp_list = slp_empty_lbl = slp_pnum_lbl = slp_next_btn = nullptr;
}

/* ---- the full, paged history list (all nights, newest first) ----
 * Scaffolds the screen IMMEDIATELY, then streams nights NEWEST-first off the SD card via
 * an lv_timer so the UI never blocks (a big history used to freeze for seconds here). */
static void app_open_sleep_data(void) {
  slp_load_cancel();             // cancel any prior in-flight load (e.g. page flip)
  app_screen_begin("Sleep data");
  slp_empty_lbl = slp_pnum_lbl = slp_next_btn = nullptr;

  int top = BOARD_SCREEN_NARROW ? UI_PX(96) : UI_PX(76);

  // --- "Trends" button at the top (opens the per-night quality graph). ---
  lv_obj_t *tbtn = lv_btn_create(app_scr);
  lv_obj_set_size(tbtn, LV_PCT(60), UI_PX(44));
  lv_obj_align(tbtn, LV_ALIGN_TOP_MID, 0, top);
  lv_obj_set_style_radius(tbtn, UI_PX(12), 0);
  lv_obj_set_style_shadow_width(tbtn, 0, 0);
  lv_obj_set_style_bg_color(tbtn, lv_color_hex(ui_accent_hex()), 0);
  lv_obj_add_event_cb(tbtn, slp_open_trends_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *tlbl = lv_label_create(tbtn);
  lv_obj_set_style_text_font(tlbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(tlbl, lv_color_black(), 0);
  lv_label_set_text(tlbl, "Trends " LV_SYMBOL_RIGHT);
  lv_obj_center(tlbl);

  // --- Page bar: prev / "Page N" / next. ALWAYS shown (so page 1 looks the same as later
  // pages while loading). We can't know the TOTAL page count without scanning the whole
  // file, so we don't show "/ Y". Prev is enabled only when not on page 0; Next is enabled
  // lazily by the loader once it finds a night beyond this page. ---
  lv_obj_t *anchor = tbtn;
  {
    lv_obj_t *pbar = lv_obj_create(app_scr);
    lv_obj_remove_style_all(pbar);
    lv_obj_set_size(pbar, LV_PCT(92), LV_SIZE_CONTENT);
    lv_obj_clear_flag(pbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align_to(pbar, tbtn, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(8));

    lv_obj_t *prev = lv_btn_create(pbar);
    lv_obj_set_size(prev, UI_PX(56), UI_PX(38));
    lv_obj_set_style_radius(prev, UI_PX(10), 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_set_style_bg_color(prev, lv_color_hex(slp_page > 0 ? 0x2A2A2A : 0x171717), 0);
    lv_obj_add_event_cb(prev, slp_page_prev_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *pl = lv_label_create(prev);
    lv_obj_set_style_text_color(pl, lv_color_hex(slp_page > 0 ? 0xFFFFFF : 0x555555), 0);
    lv_label_set_text(pl, LV_SYMBOL_LEFT);
    lv_obj_center(pl);

    slp_pnum_lbl = lv_label_create(pbar);
    lv_obj_set_style_text_font(slp_pnum_lbl, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(slp_pnum_lbl, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_text_fmt(slp_pnum_lbl, "Page %d", slp_page + 1);

    slp_next_btn = lv_btn_create(pbar);                          // child 0 = its arrow label
    lv_obj_set_size(slp_next_btn, UI_PX(56), UI_PX(38));
    lv_obj_set_style_radius(slp_next_btn, UI_PX(10), 0);
    lv_obj_set_style_shadow_width(slp_next_btn, 0, 0);
    lv_obj_set_style_bg_color(slp_next_btn, lv_color_hex(0x171717), 0);   // disabled until loader enables
    lv_obj_add_state(slp_next_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(slp_next_btn, slp_page_next_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *nl = lv_label_create(slp_next_btn);
    lv_obj_set_style_text_color(nl, lv_color_hex(0x555555), 0);
    lv_label_set_text(nl, LV_SYMBOL_RIGHT);
    lv_obj_center(nl);
    anchor = pbar;
  }

  // --- Scrollable list of this page's nights (newest first). Anchored under the page bar
  // even when it's hidden, so the layout is stable whether or not paging turns out to exist. ---
  slp_list = lv_obj_create(app_scr);
  lv_obj_remove_style_all(slp_list);
  lv_obj_set_width(slp_list, LV_PCT(96));
  lv_obj_set_flex_flow(slp_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(slp_list, UI_PX(8), 0);
  lv_obj_align_to(slp_list, anchor, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(10));
  lv_obj_update_layout(slp_list);
  int list_top = lv_obj_get_y(slp_list);
  lv_obj_set_height(slp_list, (int)screenHeight - list_top - UI_PX(12));
  lv_obj_set_scroll_dir(slp_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(slp_list, LV_SCROLLBAR_MODE_AUTO);

  // Placeholder shown until the first row streams in (removed by slp_add_night_row).
  slp_empty_lbl = lv_label_create(slp_list);
  lv_obj_set_style_text_font(slp_empty_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(slp_empty_lbl, lv_color_hex(0xAAAAAA), 0);
  lv_label_set_long_mode(slp_empty_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(slp_empty_lbl, LV_PCT(100));
  lv_obj_set_style_text_align(slp_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(slp_empty_lbl, "Loading...");   // becomes "No sleep..." if none found

  // --- Kick off the async backward stream. ---
  slp_load_skip   = slp_page * SLEEP_PAGE;   // skip the newer pages already shown elsewhere
  slp_load_added  = 0;
  slp_load_active = false;
  slp_load_until  = millis() + SLP_LOAD_HOLD_MS;   // show "Loading..." until this time
  lv_obj_add_event_cb(app_scr, slp_data_cleanup_cb, LV_EVENT_DELETE, nullptr);

  if (sleep_rev_begin(&slp_rev)) {
    slp_load_active = true;
    // ~30ms period (≈ one display refresh): guarantees a frame flushes between ticks so the
    // scaffold/rows actually paint as they stream, instead of the load monopolizing the
    // handler and nothing drawing until it finishes.
    slp_load_timer  = lv_timer_create(slp_load_tick, 30, nullptr);
  } else {
    // No file / no store: show the empty state right away.
    lv_label_set_text(slp_empty_lbl, "No sleep recorded yet.\nTrack a night to see data here.");
  }
}

/* ---- trends: restful%-per-night chart + average over a 3/7/30 window ---- */
static void app_open_sleep_trends(void) {
  app_screen_begin("Sleep trends");
  sleep_trends_load(slp_trend_range);

  int top = BOARD_SCREEN_NARROW ? UI_PX(96) : UI_PX(76);

  // --- Window selector: 3 / 7 / 30 nights. ---
  lv_obj_t *seg = lv_obj_create(app_scr);
  lv_obj_remove_style_all(seg);
  lv_obj_set_size(seg, LV_PCT(92), LV_SIZE_CONTENT);
  lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(seg, UI_PX(8), 0);
  lv_obj_align(seg, LV_ALIGN_TOP_MID, 0, top);
  static const int ranges[3] = { 3, 7, 30 };
  static const char *rlabels[3] = { "3", "7", "30" };
  for (int i = 0; i < 3; i++) {
    lv_obj_t *p = lv_btn_create(seg);
    lv_obj_set_size(p, UI_PX(64), UI_PX(40));
    lv_obj_set_style_radius(p, UI_PX(10), 0);
    lv_obj_set_style_shadow_width(p, 0, 0);
    bool active = (ranges[i] == slp_trend_range);
    lv_obj_set_style_bg_color(p, lv_color_hex(active ? ui_accent_hex() : 0x2A2A2A), 0);
    lv_obj_add_event_cb(p, slp_trend_range_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ranges[i]);
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, active ? lv_color_black() : lv_color_white(), 0);
    lv_label_set_text(l, rlabels[i]);
    lv_obj_center(l);
  }

  // --- Average restful % over the window (big, colored). ---
  // Use the biggest STOCK LVGL font (montserrat_48, full glyph set) rather than FONT_TIME:
  // FONT_TIME is the custom clock font (digits + colon only) so a '%' fed to it renders as a
  // missing-glyph box, and at 110px it's oversized for this number anyway.
  lv_obj_t *avg = lv_label_create(app_scr);
  lv_obj_set_style_text_font(avg, &lv_font_montserrat_48, 0);
  lv_obj_align_to(avg, seg, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(14));
  lv_obj_t *avgcap = lv_label_create(app_scr);
  lv_obj_set_style_text_font(avgcap, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(avgcap, lv_color_hex(0x9090A0), 0);

  if (s_trend_n == 0) {
    lv_label_set_text(avg, "");
    lv_obj_set_style_text_color(avg, lv_color_hex(0x808080), 0);
    lv_label_set_text(avgcap, "No nights in this window");
    lv_obj_align_to(avgcap, avg, LV_ALIGN_OUT_BOTTOM_MID, UI_PX(50), UI_PX(6));
    return;
  }
  lv_label_set_text_fmt(avg, "%u%%", (unsigned)s_trend_avg);
  lv_obj_set_style_text_color(avg, lv_color_hex(slp_quality_color(s_trend_avg)), 0);
  lv_label_set_text_fmt(avgcap, "Avg restful over %d night%s", s_trend_n,
                        s_trend_n == 1 ? "" : "s");
  lv_obj_align_to(avgcap, avg, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(6));

  // --- Bar chart: restful% per night (left = oldest, right = most recent). ---
  lv_obj_t *chart = lv_chart_create(app_scr);
  lv_obj_set_width(chart, LV_PCT(92));
  lv_obj_set_height(chart, UI_PX(120));
  lv_obj_align_to(chart, avgcap, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(16));
  slp_chart_make_room(chart);                  // free a left strip for the Y-axis labels
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
  lv_chart_set_div_line_count(chart, 5, 0);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(chart, lv_color_hex(0x303030), LV_PART_MAIN);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_point_count(chart, s_trend_n);
  lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(ui_accent_hex()),
                                               LV_CHART_AXIS_PRIMARY_Y);
  for (int i = 0; i < s_trend_n; i++)
    lv_chart_set_next_value(chart, ser, (lv_coord_t)s_trend_pct[i]);
  lv_chart_refresh(chart);

  // Y-axis scale: 0..100% in 20% steps (6 ticks, matching the 5 grid lines).
  slp_chart_add_yaxis(chart, 0, 100, 6, "%");
}

static void app_open_sleep_night(void) {
  app_screen_begin("Night");
  // Re-assemble the selected night's summary by epoch (the list no longer keeps a RAM array;
  // it streams rows in directly). One short forward scan to the night that starts here.
  sleep_night_t night;
  const sleep_night_t *n = sleep_night_summary(slp_sel_start, &night) ? &night : nullptr;

  if (!n) {
    lv_obj_t *err = lv_label_create(app_scr);
    lv_obj_set_style_text_font(err, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(err, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(err, "Night not found.");
    lv_obj_center(err);
    return;
  }

  uint8_t  pct  = sleep_night_restful_pct(n);
  uint32_t mins = sleep_night_minutes(n);
  char dbuf[20], durbuf[16];
  slp_fmt_date(n->end_epoch, dbuf, sizeof(dbuf));
  slp_fmt_dur(mins, durbuf, sizeof(durbuf));

  int top = BOARD_SCREEN_NARROW ? UI_PX(104) : UI_PX(72);

  // Date headline.
  lv_obj_t *date = lv_label_create(app_scr);
  lv_obj_set_style_text_font(date, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(date, lv_color_white(), 0);
  lv_label_set_text(date, dbuf);
  lv_obj_align(date, LV_ALIGN_TOP_MID, 0, top);

  // Two stat tiles: Restful % and Duration.
  lv_obj_t *stats = lv_obj_create(app_scr);
  lv_obj_remove_style_all(stats);
  lv_obj_set_size(stats, LV_PCT(92), LV_SIZE_CONTENT);
  lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align_to(stats, date, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(12));

  struct { const char *cap; char val[16]; uint32_t color; } tiles[2];
  tiles[0].cap = "Restful"; snprintf(tiles[0].val, sizeof(tiles[0].val), "%u%%", (unsigned)pct);
  tiles[0].color = slp_quality_color(pct);
  tiles[1].cap = "Duration"; snprintf(tiles[1].val, sizeof(tiles[1].val), "%s", durbuf);
  tiles[1].color = 0xFFFFFF;
  for (int i = 0; i < 2; i++) {
    lv_obj_t *card = lv_obj_create(stats);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(46), UI_PX(72));
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_PX(12), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_font(v, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(tiles[i].color), 0);
    lv_label_set_text(v, tiles[i].val);
    lv_obj_align(v, LV_ALIGN_CENTER, 0, UI_PX(-8));
    lv_obj_t *c = lv_label_create(card);
    lv_obj_set_style_text_font(c, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(0x9090A0), 0);
    lv_label_set_text(c, tiles[i].cap);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, UI_PX(16));
  }

  // Movement graph for this night. Raw `accum` is in tens of thousands (summed accel
  // deviation) — not eye-readable — so we plot a RELATIVE MOVEMENT INDEX 0..100: each
  // point scaled by the night's own peak. The bar SHAPE is identical; the axis now reads
  // 0..100 (0 = stillest, 100 = the night's most active stretch).
  lv_obj_t *glbl = lv_label_create(app_scr);
  lv_obj_set_style_text_font(glbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(glbl, lv_color_hex(0x808080), 0);
  lv_obj_set_style_text_align(glbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(glbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(glbl, LV_PCT(92));
  lv_label_set_text(glbl, BOARD_SCREEN_NARROW ? "Movement index" : "Movement index through the night");
  lv_obj_align_to(glbl, stats, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(16));

  sleep_night_load_points(n->start_epoch);

  lv_obj_t *chart = lv_chart_create(app_scr);
  lv_obj_set_width(chart, LV_PCT(92));
  lv_obj_set_height(chart, UI_PX(110));
  lv_obj_align_to(chart, glbl, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(8));
  slp_chart_make_room(chart);                     // free a left strip for the Y-axis labels
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
  lv_chart_set_type(chart, LV_CHART_TYPE_BAR);    // bars read well as "movement bursts"
  lv_chart_set_div_line_count(chart, 5, 0);       // 4 inner lines -> 0/25/50/75/100 bands
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(chart, lv_color_hex(0x303030), LV_PART_MAIN);

  int pts = s_night_pts_n > 0 ? s_night_pts_n : 1;
  lv_chart_set_point_count(chart, pts);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);   // movement index 0..100
  lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(ui_accent_hex()),
                                               LV_CHART_AXIS_PRIMARY_Y);
  uint32_t denom = (s_night_pts_max > 0) ? s_night_pts_max : 1;
  for (int i = 0; i < s_night_pts_n; i++) {
    uint32_t idx = (uint32_t)s_night_pts[i] * 100u / denom;     // scale to 0..100
    if (idx > 100) idx = 100;
    lv_chart_set_next_value(chart, ser, (lv_coord_t)idx);
  }
  if (s_night_pts_n == 0)
    lv_chart_set_next_value(chart, ser, 0);
  lv_chart_refresh(chart);

  // Y-axis scale: 0..100 in 25-step ticks (5 ticks, matching the 4 inner grid lines).
  slp_chart_add_yaxis(chart, 0, 100, 5, "");
}
