/* ============================================================================
 *  app_fitness.h — Fitness sub-app: step counter (software detection on the QMI8658).
 *
 *  Header-only module compiled into the .ino TU, sharing the menu statics.
 *  INCLUDE AFTER app_menu.h (app_screen_begin / app_scr), imu_steps.h (the step
 *  API), settings_store.h (ui_accent_hex) and the FONT_* macros.
 *
 *  The QMI8658 hardware pedometer does not count on this unit, so steps are detected
 *  in software from the accel magnitude. The IMU is OFF until the user taps Start; Start
 *  powers the accel on + begins counting, Stop powers it off + stops (and lets normal
 *  deep-sleep resume). While counting, a fast timer samples the accel and a slow timer
 *  repaints the number. The IMU shares the touch I2C bus, so reads take i2c_lock().
 * ========================================================================== */
#pragma once
#include <lvgl.h>

static lv_obj_t  *fit_count_lbl  = nullptr;  // big step number (LVGL thread only)
static lv_obj_t  *fit_btn_lbl    = nullptr;  // "Start"/"Stop" caption
static lv_obj_t  *fit_status     = nullptr;  // small status line
static lv_obj_t  *fit_reset_btn  = nullptr;  // Reset button (re-anchored when status wraps)
static lv_timer_t *fit_paint_tm  = nullptr;  // slow: repaints the count
static bool       fit_counting   = false;

static void fit_show_count(uint32_t steps) {
  if (!fit_count_lbl) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)steps);
  lv_label_set_text(fit_count_lbl, buf);
}

/* Set the status message + its color, then re-anchor the Reset button to the status line's
 * (possibly changed) bottom edge. On narrow panels the message can wrap to two lines, which
 * changes the label height — without re-aligning, Reset would overlap or float away from it. */
static void fit_set_status(const char *txt, uint32_t color) {
  if (!fit_status) return;
  lv_label_set_text(fit_status, txt);
  lv_obj_set_style_text_color(fit_status, lv_color_hex(color), 0);
  if (fit_reset_btn) {
    lv_obj_update_layout(fit_status);   // resolve the new wrapped height before aligning
    lv_obj_align_to(fit_reset_btn, fit_status, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(14));
  }
}

/* Paint tick: repaint the running total. The actual SAMPLING (step detection) is done by
 * imu_steps_tick() in the main loop, so counting continues even when this screen is closed —
 * this app only displays the number. */
static void fit_paint_cb(lv_timer_t *t) {
  (void)t;
  fit_show_count(imu_steps_count());
}

/* Tear down the paint timer + drop pointers on any screen-close path. Does NOT stop counting:
 * the background sampler (imu_steps_tick) keeps the counter running after you leave. */
static void fit_cleanup_cb(lv_event_t *e) {
  (void)e;
  if (fit_paint_tm)  { lv_timer_del(fit_paint_tm);  fit_paint_tm  = nullptr; }
  fit_count_lbl = nullptr;
  fit_btn_lbl   = nullptr;
  fit_status    = nullptr;
  fit_reset_btn = nullptr;
}

/* Start/Stop toggle. Start: power the accel on + begin counting. Stop: power off + stop. */
static void fit_toggle_cb(lv_event_t *e) {
  (void)e;
  if (!imu_steps_available()) return;

  if (!fit_counting) {
    // Mutual exclusion: a sleep-tracking session also owns the IMU/ULP — they can't
    // both run. Refuse to start counting while sleep mode is active (the user must turn
    // sleep mode off first in the Sleep app). See sleep_track.h / app_sleep.h.
    if (sleep_track_active()) {
      fit_set_status("Turn off Sleep mode first.", 0xFF9F0A);
      return;
    }
    i2c_lock();
    bool ok = imu_steps_start();
    i2c_unlock();
    fit_counting = ok;
    if (ok) prefs.putUInt("run", 1);       // persist "counting" so it resumes after a cold boot
    fit_show_count(imu_steps_count());      // RESUME from the stored total — Start doesn't zero it
    if (fit_btn_lbl) lv_label_set_text(fit_btn_lbl, ok ? "Stop" : "Start");
    fit_set_status(ok ? "Counting your steps" : "Couldn't start the sensor.",
                   ok ? ui_accent_hex() : 0xFF9F0A);
  } else {
    fit_counting = false;
    fit_show_count(imu_steps_count());     // freeze on the final value
    i2c_lock();
    imu_steps_stop();                      // accel OFF — normal deep-sleep can resume
    i2c_unlock();
    prefs.putUInt("steps", imu_steps_count());   // persist immediately on Stop
    prefs.putUInt("run", 0);                     // and clear the "counting" flag
    imu_steps_mark_saved(millis());
    if (fit_btn_lbl) lv_label_set_text(fit_btn_lbl, "Start");
    fit_set_status("Stopped. Tap Start to begin.", 0xAAAAAA);
  }
}

/* Reset button: zero the count + restart the detector (keeps the accel on if counting). */
static void fit_reset_cb(lv_event_t *e) {
  (void)e;
  if (!imu_steps_available()) return;
  i2c_lock();
  imu_steps_full_reset();
  i2c_unlock();
  prefs.putUInt("steps", 0);            // clear the NVS floor too, else set_total restores it
  imu_steps_mark_saved(millis());
  fit_show_count(0);
  fit_set_status(fit_counting ? "Counting your steps" : "Counter reset", ui_accent_hex());
}

static void app_open_fitness(void) {
  app_screen_begin("Fitness");

  bool have = imu_steps_available();
  // Mutual exclusion: the step counter and a sleep-tracking session share the one accel/ULP, so
  // only one may run. If sleep mode is active, the step counter is BLOCKED — grey the controls
  // out + show a note telling the user to turn sleep mode off first (rather than only refusing
  // on tap). "usable" gates the interactive Start/Reset; "have" still gates sensor presence.
  bool blocked = have && sleep_track_active();
  bool usable  = have && !blocked;

  // --- Big step number. ---
  fit_count_lbl = lv_label_create(app_scr);
  lv_obj_set_style_text_font(fit_count_lbl, &FONT_TIME, 0);
  lv_obj_set_style_text_color(fit_count_lbl, lv_color_white(), 0);
  fit_show_count(imu_steps_count());   // show the stored total immediately (don't flash 0 until the paint timer)
  lv_obj_align(fit_count_lbl, LV_ALIGN_CENTER, 0, UI_PX(-56));

  lv_obj_t *unit = lv_label_create(app_scr);
  lv_obj_set_style_text_font(unit, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(unit, lv_color_hex(0x9090A0), 0);
  lv_label_set_text(unit, "steps");
  lv_obj_align_to(unit, fit_count_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(4));

  // --- Start/Stop button. Narrow panels (C6-1.47) get a wider, taller pill so it's
  //     an easy touch target; the S3 keeps its original size. ---
  lv_obj_t *btn = lv_btn_create(app_scr);
#if BOARD_SCREEN_NARROW
  lv_obj_set_size(btn, LV_PCT(82), UI_PX(80));
#else
  lv_obj_set_size(btn, LV_PCT(60), UI_PX(64));
#endif
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, UI_PX(48));
  lv_obj_set_style_radius(btn, UI_PX(16), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(usable ? ui_accent_hex() : 0x3A3A3A), 0);
  if (usable) lv_obj_add_event_cb(btn, fit_toggle_cb, LV_EVENT_CLICKED, nullptr);
  else        lv_obj_add_state(btn, LV_STATE_DISABLED);

  fit_btn_lbl = lv_label_create(btn);
  lv_obj_set_style_text_font(fit_btn_lbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(fit_btn_lbl, usable ? lv_color_black() : lv_color_hex(0x777777), 0);
  lv_label_set_text(fit_btn_lbl, "Start");
  lv_obj_center(fit_btn_lbl);

  // --- Status line. On the S3 it fits on one line (ellipsis as a safety net). On narrow
  //     panels (C6-1.47) the message is too long for 172 px, so we WRAP it instead of
  //     clipping/ellipsizing — and the Reset button is anchored to its actual bottom edge
  //     (align_to re-runs after wrap), so a two-line message just pushes Reset down rather
  //     than overlapping it. ---
  fit_status = lv_label_create(app_scr);
  lv_obj_set_style_text_font(fit_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_align(fit_status, LV_TEXT_ALIGN_CENTER, 0);
#if BOARD_SCREEN_NARROW
  lv_label_set_long_mode(fit_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(fit_status, LV_PCT(96));
#else
  ui_label_single_line(fit_status);   // single line, ellipsis if long
  lv_obj_set_width(fit_status, LV_PCT(90));
#endif
  lv_obj_align_to(fit_status, btn, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(14));

  // --- Reset button: anchored BELOW the status line (not an absolute offset) so it never
  //     overlaps it — even when the status wraps to two lines on a narrow panel. Narrow
  //     panels get a wider, taller button to match the Start/Stop pill. ---
  if (usable) {
    lv_obj_t *rbtn = lv_btn_create(app_scr);
#if BOARD_SCREEN_NARROW
    lv_obj_set_size(rbtn, LV_PCT(66), UI_PX(64));
#else
    lv_obj_set_size(rbtn, LV_PCT(40), UI_PX(48));
#endif
    lv_obj_align_to(rbtn, fit_status, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(14));
    lv_obj_set_style_radius(rbtn, UI_PX(12), 0);
    lv_obj_set_style_shadow_width(rbtn, 0, 0);
    lv_obj_set_style_bg_color(rbtn, lv_color_hex(0x3A3A3A), 0);
    lv_obj_add_event_cb(rbtn, fit_reset_cb, LV_EVENT_CLICKED, nullptr);
    fit_reset_btn = rbtn;   // tracked so fit_set_status() can re-anchor it when the status wraps

    lv_obj_t *rlbl = lv_label_create(rbtn);
    lv_obj_set_style_text_font(rlbl, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(rlbl, lv_color_white(), 0);
    lv_label_set_text(rlbl, "Reset");
    lv_obj_center(rlbl);
  }

  // Reflect the hardware truth: counting persists across screen opens (until Stop).
  fit_counting = imu_steps_running();

  if (!have) {
    fit_set_status("Step sensor not available on this device.", 0xFF9F0A);
    fit_counting = false;
  } else if (blocked) {
    // Sleep mode owns the accel — the step counter is disabled until it's turned off.
    fit_set_status("Sleep mode is on.\nTurn off Sleep to count steps.", 0xFF9F0A);
    fit_counting = false;
  } else if (fit_counting) {
    lv_label_set_text(fit_btn_lbl, "Stop");
    fit_set_status("Counting your steps", ui_accent_hex());
    fit_show_count(imu_steps_count());
  } else {
    fit_set_status("Tap Start to count your steps.", 0xAAAAAA);
  }

  // Paint timer only — sampling/detection runs globally in imu_steps_tick() (main loop), so
  // the count keeps advancing after this screen closes. This timer just refreshes the label.
  // Skip it when blocked (nothing counts) — but still register cleanup if it was created.
  if (usable) {
    fit_paint_tm = lv_timer_create(fit_paint_cb, 500, nullptr);
    lv_obj_add_event_cb(app_scr, fit_cleanup_cb, LV_EVENT_DELETE, nullptr);
  }
}
