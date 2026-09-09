/* ============================================================================
 *  app_heart.h — Heart sub-app: on-demand heart-rate + HRV measurement.
 *
 *  Main screen: heart icon, the big BPM readout, a Measure/Stop pill and a
 *  status line, plus "History". Measure turns the optical sensor on for
 *  HR_MEASURE_S seconds; the readout updates live as beats come in and the
 *  measurement is saved (hr_store.h) when it ends: bpm, RMSSD and SDNN (the two
 *  standard heart-rate-variability figures) and a quality percentage. The
 *  sensor is only ever on during a measurement (never in the background).
 *
 *  History: a list of past measurements, newest first (date/time, bpm, HRV),
 *  and a Trends screen with a 7/30-reading chart of bpm and RMSSD.
 *
 *  Board-neutral: talks to hr_sensor.h only; the tile is gated by BOARD_HAS_HR.
 *  Header-only; INCLUDE AFTER app_menu.h, hr_sensor.h, hr_algo.h, hr_store.h.
 * ========================================================================== */
#pragma once
#include <lvgl.h>
#include <time.h>

#define HR_MEASURE_S   45          // seconds per measurement (HRV needs > 30 s)
#define HR_POLL_MS     50          // sensor drain cadence while measuring

/* LED-drive AGC (2026-09-06). Every HR_AGC_PERIOD_S while measuring: if the
 * wrist is on the sensor (touch) and the detector is not finding beats, or the
 * pulse amplitude is below HR_AGC_ENV_LO, step the green LED drive up; if the
 * amplitude is above HR_AGC_ENV_HI (heading for saturation) step it down. No
 * touch = no change, so an empty sensor never winds the LEDs up to maximum.
 * The level it settles on is kept for the next measurement. */
#define HR_AGC_PERIOD_S  3
#define HR_AGC_MIN_BEATS 2         // accepted beats per period that count as "detecting"
#define HR_AGC_ENV_LO    1         // amplitude floor, applied ONLY when no beats at all: a wrist
                                   // reading fine at env 2-5 must not be wound up to max (v107 did)
#define HR_AGC_ENV_HI    120       // ...above this -> less drive
#define HR_AGC_STEP      4
static uint32_t hrt_agc_t   = 0;
static int      hrt_agc_ok0 = 0;   // beats_ok at the last AGC check

static lv_obj_t   *hrt_bpm_lbl  = nullptr;
static lv_obj_t   *hrt_status   = nullptr;
static lv_obj_t   *hrt_btn_lbl  = nullptr;
static lv_obj_t   *hrt_hrv_lbl  = nullptr;
static lv_timer_t *hrt_timer    = nullptr;
static bool        hrt_measuring = false;
static uint32_t    hrt_t0_ms    = 0;
static int         hrt_last_bpm = 0, hrt_last_rmssd = 0;   // last finished result (for the idle screen)

static void hrt_set_status(const char *txt, uint32_t color) {
  if (!hrt_status) return;
  lv_label_set_text(hrt_status, txt);
  lv_obj_set_style_text_color(hrt_status, lv_color_hex(color), 0);
}

static void hrt_sample_cb(int32_t ppg, bool touch) { hr_algo_push(ppg, touch); }

static void hrt_finish(bool save) {
  hr_sensor_stop();
  hrt_measuring = false;
  if (hrt_timer) { lv_timer_delete(hrt_timer); hrt_timer = nullptr; }
  int bpm = hr_algo_bpm_mean(), rmssd = hr_algo_rmssd(), sdnn = hr_algo_sdnn(), q = hr_algo_quality();
  if (hrt_btn_lbl) lv_label_set_text(hrt_btn_lbl, "Measure");
  if (save && bpm > 0) {
    hrt_last_bpm = bpm; hrt_last_rmssd = rmssd;
    hr_store_append((uint32_t)rtc_now_epoch(), bpm, rmssd, sdnn, q);
    if (hrt_bpm_lbl) lv_label_set_text_fmt(hrt_bpm_lbl, "%d", bpm);
    if (hrt_hrv_lbl) lv_label_set_text_fmt(hrt_hrv_lbl, "HRV %d ms  (SDNN %d)", rmssd, sdnn);
    char s[64]; snprintf(s, sizeof s, "Saved. Signal quality %d%%.", q);
    hrt_set_status(s, q >= 70 ? 0x32D74B : 0xFF9F0A);
  } else if (save) {
    hrt_set_status("No pulse found. Wear the watch snugly\nand keep still.", 0xFF9F0A);
  } else {
    hrt_set_status("Stopped.", 0x8890A0);
  }
}

static void hrt_tick(lv_timer_t *t) {
  (void)t;
  if (!hrt_measuring) return;
  hr_sensor_poll(hrt_sample_cb);
  uint32_t el = (millis() - hrt_t0_ms) / 1000;
  int bpm = hr_algo_bpm();
  if (hrt_bpm_lbl) { if (bpm > 0) lv_label_set_text_fmt(hrt_bpm_lbl, "%d", bpm); else lv_label_set_text(hrt_bpm_lbl, "--"); }
  if (hrt_hrv_lbl) { int r = hr_algo_rmssd(); if (r > 0) lv_label_set_text_fmt(hrt_hrv_lbl, "HRV %d ms", r); else lv_label_set_text(hrt_hrv_lbl, ""); }
  char s[64];
  snprintf(s, sizeof s, "Measuring, keep still... %lus", (unsigned long)(HR_MEASURE_S - el));
  hrt_set_status(s, 0x8890A0);
  // ---- LED-drive AGC ----
  if (millis() - hrt_agc_t >= HR_AGC_PERIOD_S * 1000u) {
    hrt_agc_t = millis();
    int got = hr_algo_beats() - hrt_agc_ok0; hrt_agc_ok0 = hr_algo_beats();
    int env = (int)s_hra.peak_env, g = hr_sensor_get_gain(), ng = g;
    if (hr_algo_touch()) {
      if (env > HR_AGC_ENV_HI)                         ng = g - HR_AGC_STEP;
      else if (got < HR_AGC_MIN_BEATS || (got == 0 && env < HR_AGC_ENV_LO)) ng = g + HR_AGC_STEP;
    }
    if (ng > HR_GAIN_MAX) ng = HR_GAIN_MAX; if (ng < 4) ng = 4;
    if (ng != g) { hr_sensor_set_gain(ng); USBSerial.printf("[hr] agc: beats=%d env=%d -> led 0x%02x\n", got, env, ng); }
  }
  static uint32_t s_last_dbg = 0;
  if (millis() - s_last_dbg >= 1000) {          // one diagnostic line per second
    s_last_dbg = millis();
    USBSerial.printf("[hr] t=%lus samples=%d touch=%d raw=%ld..%ld dc=%ld env=%d beats=%d/%d bpm=%d\n",
                     (unsigned long)el, s_hra.dbg_n, hr_algo_touch() ? 1 : 0,
                     (long)s_hra.dbg_min, (long)s_hra.dbg_max, (long)s_hra.dc, (int)s_hra.peak_env, hr_algo_beats(), s_hra.beats_all, bpm);
    s_hra.dbg_n = 0;
#ifdef HR_DIAG
    if (el < 5 || (el % 10) == 0) hr_sensor_diag();   // sensor register snapshot while bringing the PPG up
#endif
  }
  if (el >= HR_MEASURE_S) hrt_finish(true);
}

static void hrt_toggle_cb(lv_event_t *e) {
  (void)e;
  if (hrt_measuring) { hrt_finish(false); return; }
  if (!hr_sensor_available()) return;
  hr_algo_begin(hr_sensor_rate_hz());
  if (!hr_sensor_start()) { hrt_set_status("Sensor did not start.", 0xFF453A); return; }
  hrt_measuring = true;
  hrt_t0_ms = millis();
  hrt_agc_t = millis() + 2000u;   // first AGC check after the settle window, then every period
  hrt_agc_ok0 = 0;
  if (hrt_btn_lbl) lv_label_set_text(hrt_btn_lbl, "Stop");
  if (hrt_bpm_lbl) lv_label_set_text(hrt_bpm_lbl, "--");
  if (hrt_hrv_lbl) lv_label_set_text(hrt_hrv_lbl, "");
  hrt_set_status("Measuring, keep still...", 0x8890A0);
  hrt_timer = lv_timer_create(hrt_tick, HR_POLL_MS, nullptr);
}

static void hrt_cleanup_cb(lv_event_t *e) {
  (void)e;
  if (hrt_measuring) hrt_finish(false);         // leaving the screen ends the measurement
  hrt_bpm_lbl = hrt_status = hrt_btn_lbl = hrt_hrv_lbl = nullptr;
}

static void app_open_heart_history(void);
static void app_open_heart_trends(void);
static void hrt_history_cb(lv_event_t *e) { (void)e; nav_open(app_open_heart_history); }

/* Every Heart screen body hangs from HRT_BODY_Y, just under the title that
 * app_screen_begin() puts at TOP_MID +UI_PX(40), and grows DOWNWARD (flex
 * START). Bodies used to be centred or bottom-anchored, so whenever their
 * content was taller than expected they grew UPWARD into the title: the heart
 * icon under "Heart", the Trends button under "History", the range buttons
 * under "Trends". Nothing can climb above HRT_BODY_Y now. */
#define HRT_BODY_Y  UI_PX(78)

static lv_obj_t *hrt_body(void) {
  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_remove_style_all(col);
  lv_obj_set_width(col, LV_PCT(92));
  lv_obj_set_height(col, LCD_HEIGHT - HRT_BODY_Y - UI_PX(6));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, HRT_BODY_Y);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, UI_PX(6), 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  return col;
}

static lv_obj_t *hrt_pill(lv_obj_t *parent, const char *txt, int w_pct, int h, uint32_t bg,
                          bool dark_text, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, LV_PCT(w_pct), h);
  lv_obj_set_style_radius(b, UI_PX(12), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(l, dark_text ? lv_color_black() : lv_color_white(), 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}

static void app_open_heart(void) {
  app_screen_begin("Heart");
  bool have = hr_sensor_available();
  lv_obj_t *col = hrt_body();

  lv_obj_t *icon = lv_label_create(col);
  lv_obj_set_style_text_font(icon, &icons34, 0);
  lv_obj_set_style_text_color(icon, lv_color_hex(ui_deco_hex(0xFF453A)), 0);
  { char u[5]; lv_label_set_text(icon, mdi_utf8(MDI_HEART_PULSE, u)); }

  /* BPM number + unit share a flex ROW so the unit tracks the number's width.
   * The number uses a FULL-GLYPH Montserrat size, NOT FONT_TIME: the clock
   * font on these round watches carries digits and a colon only. */
  lv_obj_t *nrow = lv_obj_create(col);
  lv_obj_remove_style_all(nrow);
  lv_obj_set_size(nrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(nrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(nrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(nrow, UI_PX(6), 0);
  lv_obj_clear_flag(nrow, LV_OBJ_FLAG_SCROLLABLE);

  hrt_bpm_lbl = lv_label_create(nrow);
  lv_obj_set_style_text_font(hrt_bpm_lbl, &UI_FONT(52), 0);
  lv_obj_set_style_text_color(hrt_bpm_lbl, lv_color_white(), 0);
  if (hrt_last_bpm > 0) lv_label_set_text_fmt(hrt_bpm_lbl, "%d", hrt_last_bpm); else lv_label_set_text(hrt_bpm_lbl, "--");

  lv_obj_t *unit = lv_label_create(nrow);
  lv_obj_set_style_text_font(unit, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(unit, lv_color_hex(0x8890A0), 0);
  lv_label_set_text(unit, "bpm");
  lv_obj_set_style_pad_bottom(unit, UI_PX(8), 0);

  hrt_hrv_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(hrt_hrv_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hrt_hrv_lbl, lv_color_hex(0x8890A0), 0);
  if (hrt_last_rmssd > 0) lv_label_set_text_fmt(hrt_hrv_lbl, "HRV %d ms", hrt_last_rmssd); else lv_label_set_text(hrt_hrv_lbl, " ");

  lv_obj_t *btn = lv_btn_create(col);
  lv_obj_set_size(btn, LV_PCT(64), UI_PX(54));
  lv_obj_set_style_radius(btn, UI_PX(16), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(have ? ui_accent_hex() : 0x3A3A3A), 0);
  if (have) lv_obj_add_event_cb(btn, hrt_toggle_cb, LV_EVENT_CLICKED, nullptr);
  else      lv_obj_add_state(btn, LV_STATE_DISABLED);
  hrt_btn_lbl = lv_label_create(btn);
  lv_obj_set_style_text_font(hrt_btn_lbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(hrt_btn_lbl, have ? lv_color_black() : lv_color_hex(0x777777), 0);
  lv_label_set_text(hrt_btn_lbl, "Measure");
  lv_obj_center(hrt_btn_lbl);

  hrt_status = lv_label_create(col);
  lv_obj_set_style_text_font(hrt_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_align(hrt_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(hrt_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(hrt_status, LV_PCT(96));
  if (!have) hrt_set_status("Sensor not available on this device.", 0xFF9F0A);
  else       hrt_set_status("Tap Measure. Takes 45 seconds.", 0x8890A0);

  hrt_pill(col, "History", 44, UI_PX(40), 0x3A3A3A, false, hrt_history_cb, nullptr);

  lv_obj_add_event_cb(app_scr, hrt_cleanup_cb, LV_EVENT_DELETE, nullptr);
}

/* ---- shared stats over the newest `win` rows ------------------------------ */
typedef struct { long bpm_avg, bpm_min, bpm_max, hrv_avg, hrv_min, hrv_max, q_avg; int n; } hrt_stats_t;
static hrt_stats_t hrt_stats(int win) {
  hrt_stats_t s; memset(&s, 0, sizeof s);
  if (win <= 0) return s;
  long bs = 0, hs = 0, qs = 0; s.bpm_min = 999; s.hrv_min = 9999;
  for (int i = 0; i < win; i++) {
    const hr_row_t *r = &s_hr_rows[i];
    bs += r->bpm; hs += r->rmssd; qs += r->quality;
    if (r->bpm < s.bpm_min) s.bpm_min = r->bpm;   if (r->bpm > s.bpm_max) s.bpm_max = r->bpm;
    if (r->rmssd < s.hrv_min) s.hrv_min = r->rmssd; if (r->rmssd > s.hrv_max) s.hrv_max = r->rmssd;
  }
  s.n = win; s.bpm_avg = bs / win; s.hrv_avg = hs / win; s.q_avg = qs / win;
  return s;
}

/* ---- History: overall averages on top, then every reading ----------------- */
static void hrt_fmt_when(uint32_t epoch, char *buf, size_t n) {
  time_t t = (time_t)epoch; struct tm lt; localtime_r(&t, &lt);
  strftime(buf, n, "%a %d %b  %H:%M", &lt);
}
static uint32_t hrt_bpm_color(int bpm) {
  if (bpm < 50 || bpm > 100) return 0xFF9F0A;
  return 0x32D74B;
}
static void hrt_trends_cb(lv_event_t *e) { (void)e; nav_open(app_open_heart_trends); }

static lv_obj_t *hrt_text(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, txt);
  return l;
}

/* ---- History rows: swipe left to reveal a delete button --------------------
 * A reading is a SLOT holding two children: the red delete button pinned to the
 * slot's right edge, and the opaque card on top of it. Dragging the card left
 * uncovers the button (nothing is drawn over a reading until you swipe), and it
 * snaps open or shut on release. The card only starts following the finger once
 * the drag is clearly horizontal, so a vertical drag still scrolls the list. */
#define HRT_SWIPE_W    UI_PX(56)     // how far a card slides = delete button width
#define HRT_SWIPE_MIN  UI_PX(8)      // movement before a drag commits to an axis

static lv_obj_t *hrt_swipe_open  = nullptr;   // the one card currently swiped open
static lv_obj_t *hrt_drag_card   = nullptr;   // card tracking this press (null once the press is the list's)
static bool      hrt_drag_horiz  = false;
static lv_point_t hrt_drag_p0;
static int32_t   hrt_drag_x0     = 0;

static void hrt_swipe_close(lv_obj_t *card) {
  if (!card) return;
  lv_obj_set_x(card, 0);
  if (hrt_swipe_open == card) hrt_swipe_open = nullptr;
}

static void hrt_swipe_cb(lv_event_t *e) {
  lv_obj_t *card = (lv_obj_t *)lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p; lv_indev_get_point(indev, &p);

  if (code == LV_EVENT_PRESSED) {
    hrt_drag_card = card; hrt_drag_horiz = false;
    hrt_drag_p0 = p; hrt_drag_x0 = lv_obj_get_x(card);
  } else if (code == LV_EVENT_PRESSING) {
    if (hrt_drag_card != card) return;
    int32_t dx = p.x - hrt_drag_p0.x, dy = p.y - hrt_drag_p0.y;
    if (!hrt_drag_horiz) {
      if (LV_ABS(dy) > HRT_SWIPE_MIN && LV_ABS(dy) >= LV_ABS(dx)) { hrt_drag_card = nullptr; return; }  // it's a scroll
      if (LV_ABS(dx) < HRT_SWIPE_MIN) return;
      hrt_drag_horiz = true;
      if (hrt_swipe_open && hrt_swipe_open != card) hrt_swipe_close(hrt_swipe_open);   // only one open at a time
    }
    int32_t x = hrt_drag_x0 + dx;
    if (x > 0) x = 0;
    if (x < -HRT_SWIPE_W) x = -HRT_SWIPE_W;
    lv_obj_set_x(card, x);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (hrt_drag_card != card) return;
    hrt_drag_card = nullptr;
    if (!hrt_drag_horiz) { hrt_swipe_close(card); return; }   // a tap on an open card shuts it
    hrt_drag_horiz = false;
    if (lv_obj_get_x(card) < -HRT_SWIPE_W / 2) { lv_obj_set_x(card, -HRT_SWIPE_W); hrt_swipe_open = card; }
    else                                        hrt_swipe_close(card);
  }
}

/* The revealed button: drop the reading and rebuild the screen (nav_current is
 * already this screen, so it is a redraw, not another step on the back stack). */
static void hrt_delete_cb(lv_event_t *e) {
  uint32_t epoch = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  hr_store_delete(epoch);
  app_open_heart_history();
}

static void app_open_heart_history(void) {
  app_screen_begin("History");
  hrt_swipe_open = hrt_drag_card = nullptr;    // the old cards are gone with the old screen
  hrt_drag_horiz = false;
  int n = hr_store_load();
  lv_obj_t *col = hrt_body();

  if (n == 0) {
    hrt_text(col, "No measurements yet.", &FONT_SMALL, 0x8890A0);
    return;
  }

  /* Overall card: the HRV score is the headline (higher = better recovery),
   * with the average bpm, its range and the number of readings under it. */
  hrt_stats_t st = hrt_stats(n);
  char buf[64];
  snprintf(buf, sizeof buf, "HRV %ld ms", st.hrv_avg);
  hrt_text(col, buf, &UI_FONT(30), 0x5AC8FA);
  snprintf(buf, sizeof buf, "Avg %ld bpm  (%ld-%ld)", st.bpm_avg, st.bpm_min, st.bpm_max);
  hrt_text(col, buf, &FONT_SMALL, 0xC8CCD4);
  snprintf(buf, sizeof buf, "%d reading%s, quality %ld%%", n, n == 1 ? "" : "s", st.q_avg);
  hrt_text(col, buf, &FONT_SMALL, 0x8890A0);

  hrt_pill(col, "Trends", 40, UI_PX(36), 0x3A3A3A, false, hrt_trends_cb, nullptr);

  lv_obj_t *list = lv_obj_create(col);
  lv_obj_remove_style_all(list);
  lv_obj_set_width(list, LV_PCT(100));
  lv_obj_set_flex_grow(list, 1);                    // takes whatever height is left
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, UI_PX(6), 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);

  for (int i = 0; i < n; i++) {
    const hr_row_t *r = &s_hr_rows[i];

    lv_obj_t *slot = lv_obj_create(list);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, LV_PCT(100), UI_PX(50));
    lv_obj_clear_flag(slot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *del = lv_btn_create(slot);
    lv_obj_set_size(del, HRT_SWIPE_W, LV_PCT(100));
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(del, UI_PX(8), 0);
    lv_obj_set_style_shadow_width(del, 0, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x3A2020), 0);
    lv_obj_add_event_cb(del, hrt_delete_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)r->epoch);
    lv_obj_t *dl = lv_label_create(del);
    lv_obj_set_style_text_font(dl, &UI_FONT(20), 0);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xFF8888), 0);
    lv_label_set_text(dl, LV_SYMBOL_TRASH);
    lv_obj_center(dl);

    lv_obj_t *row = lv_obj_create(slot);          // the card that slides
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_PX(50));
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_PX(8), 0);
    lv_obj_set_style_pad_hor(row, UI_PX(12), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, hrt_swipe_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t *lcol = lv_obj_create(row);
    lv_obj_remove_style_all(lcol);
    lv_obj_set_size(lcol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lcol, UI_PX(2), 0);
    lv_obj_clear_flag(lcol, LV_OBJ_FLAG_SCROLLABLE);

    char when[32]; hrt_fmt_when(r->epoch, when, sizeof when);
    lv_obj_t *d = lv_label_create(lcol);
    lv_obj_set_style_text_font(d, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(d, lv_color_hex(0xC8CCD4), 0);
    lv_label_set_text(d, when);

    lv_obj_t *h = lv_label_create(lcol);
    lv_obj_set_style_text_font(h, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(0x8890A0), 0);
    lv_label_set_text_fmt(h, "HRV %u ms", (unsigned)r->rmssd);

    lv_obj_t *b = lv_label_create(row);
    lv_obj_set_style_text_font(b, &UI_FONT(30), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(hrt_bpm_color(r->bpm)), 0);
    lv_label_set_text_fmt(b, "%u", (unsigned)r->bpm);
  }
}

/* ---- Trends: bpm and HRV, one point per MEASUREMENT (not per day) --------- */
static int hrt_trend_range = 7;
static void hrt_range_cb(lv_event_t *e) {
  hrt_trend_range = (int)(uintptr_t)lv_event_get_user_data(e);
  nav_open(app_open_heart_trends);
}
static void app_open_heart_trends(void) {
  app_screen_begin("Trends");
  int n = hr_store_load();
  int win = hrt_trend_range < n ? hrt_trend_range : n;
  lv_obj_t *col = hrt_body();
  lv_obj_set_style_pad_row(col, UI_PX(5), 0);
  lv_obj_set_height(col, LCD_HEIGHT - HRT_BODY_Y - UI_PX(30));   // chart stops short of the round bezel

  /* range row: last 7 / 30 / 90 readings */
  lv_obj_t *rrow = lv_obj_create(col);
  lv_obj_remove_style_all(rrow);
  lv_obj_set_size(rrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(rrow, UI_PX(8), 0);
  lv_obj_clear_flag(rrow, LV_OBJ_FLAG_SCROLLABLE);
  static const int ranges[3] = { 7, 30, 90 };
  for (int i = 0; i < 3; i++) {
    bool sel = ranges[i] == hrt_trend_range;
    char t[4]; snprintf(t, sizeof t, "%d", ranges[i]);
    lv_obj_t *b = lv_btn_create(rrow);
    lv_obj_set_size(b, UI_PX(52), UI_PX(32));
    lv_obj_set_style_radius(b, UI_PX(8), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(sel ? ui_accent_hex() : 0x3A3A3A), 0);
    lv_obj_add_event_cb(b, hrt_range_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)ranges[i]);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, sel ? lv_color_black() : lv_color_white(), 0);
    lv_label_set_text(l, t);
    lv_obj_center(l);
  }

  if (win == 0) {
    hrt_text(col, "No measurements yet.", &FONT_SMALL, 0x8890A0);
    return;
  }

  hrt_stats_t st = hrt_stats(win);
  char buf[32];
  /* The averages ARE the legend: bpm in the red of its line, HRV in the blue of
   * its line. (A caption under the chart sat on the round bezel and clipped.) */
  lv_obj_t *lrow = lv_obj_create(col);
  lv_obj_remove_style_all(lrow);
  lv_obj_set_size(lrow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(lrow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(lrow, UI_PX(18), 0);
  lv_obj_clear_flag(lrow, LV_OBJ_FLAG_SCROLLABLE);
  snprintf(buf, sizeof buf, "%ld bpm", st.bpm_avg);
  hrt_text(lrow, buf, &FONT_SMALL, 0xFF453A);
  snprintf(buf, sizeof buf, "%ld ms HRV", st.hrv_avg);
  hrt_text(lrow, buf, &FONT_SMALL, 0x5AC8FA);

  /* bpm (red, left axis) and HRV (blue, right axis) overlaid, oldest on the left */
  lv_obj_t *chart = lv_chart_create(col);
  lv_obj_set_width(chart, LV_PCT(84));
  lv_obj_set_flex_grow(chart, 1);
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart, win);
  lv_obj_set_style_bg_color(chart, lv_color_hex(0x1C1C1E), 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_radius(chart, UI_PX(8), 0);
  lv_obj_set_style_size(chart, win > 30 ? 0 : UI_PX(4), win > 30 ? 0 : UI_PX(4), LV_PART_INDICATOR);
  lv_obj_set_style_line_width(chart, UI_PX(2), LV_PART_ITEMS);
  lv_chart_set_div_line_count(chart, 3, 0);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, st.bpm_min > 10 ? st.bpm_min - 10 : 0, st.bpm_max + 10);
  lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, 0, st.hrv_max + 10);
  lv_chart_series_t *sb = lv_chart_add_series(chart, lv_color_hex(0xFF453A), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_series_t *sh = lv_chart_add_series(chart, lv_color_hex(0x5AC8FA), LV_CHART_AXIS_SECONDARY_Y);
  for (int i = 0; i < win; i++) {
    const hr_row_t *r = &s_hr_rows[win - 1 - i];
    lv_chart_set_next_value(chart, sb, r->bpm);
    lv_chart_set_next_value(chart, sh, r->rmssd);
  }

}
