/* ============================================================================
 *  timer_store.h — countdown-timer state + persistence (alarm backend).
 *
 *  The countdown is stored as an ABSOLUTE target epoch (RTC wall-clock seconds),
 *  not a remaining count, so it stays correct across deep sleep: every wake is a
 *  full reboot that reloads this from NVS and re-derives the remaining time from
 *  the PCF85063. That's why a timer can fire while the screen was off — see the
 *  wake handling in the .ino (timer_is_due() on a TIMER wake) and the alarm UI in
 *  app_timer.h.
 *
 *  INCLUDE AFTER settings_store.h (shares its `prefs` handle) and power_model.h
 *  (uses rtc_now_epoch()). Header-only; compiled into the .ino TU.
 * ========================================================================== */
#pragma once
#include <Preferences.h>

/* --- timer state (persisted in the shared "watch" NVS namespace) --- */
static bool     s_tmr_active    = false;  // a countdown exists (running OR paused)
static bool     s_tmr_paused    = false;
static uint32_t s_tmr_target    = 0;      // epoch (s) at which it fires (when running)
static uint32_t s_tmr_duration  = 300;    // last-set length (s) — seeds the setup UI
static uint32_t s_tmr_rem_pause = 0;      // remaining seconds captured at pause

/* Whether a touch (not just BOOT) may dismiss a ringing alarm. Persisted. */
static bool     s_alarm_touch   = true;

/* Set true while the alarm is actually ringing. Owned by the alarm UI
 * (app_timer.h); read by the loop (keep-awake) and BOOT handler (dismiss). */
static bool     g_alarm_active  = false;

static void timer_save(void) {
  prefs.putBool("tmr_act", s_tmr_active);
  prefs.putBool("tmr_pau", s_tmr_paused);
  prefs.putUInt("tmr_tgt", s_tmr_target);
  prefs.putUInt("tmr_dur", s_tmr_duration);
  prefs.putUInt("tmr_rem", s_tmr_rem_pause);
}

/* Load persisted timer + alarm settings. Call once at boot (after settings_load
 * has opened `prefs`). */
static void timer_load(void) {
  s_tmr_active    = prefs.getBool("tmr_act", false);
  s_tmr_paused    = prefs.getBool("tmr_pau", false);
  s_tmr_target    = prefs.getUInt("tmr_tgt", 0);
  s_tmr_duration  = prefs.getUInt("tmr_dur", 300);
  s_tmr_rem_pause = prefs.getUInt("tmr_rem", 0);
  s_alarm_touch   = prefs.getBool("alm_tch", true);
}

static bool     timer_is_active(void) { return s_tmr_active; }
static bool     timer_is_paused(void) { return s_tmr_active && s_tmr_paused; }
static uint32_t timer_duration_s(void){ return s_tmr_duration; }

static bool     alarm_touch_dismiss(void) { return s_alarm_touch; }
static void     alarm_set_touch_dismiss(bool en) {
  s_alarm_touch = en;
  prefs.putBool("alm_tch", en);
}

/* Seconds left (0 = expired/none). RTC-derived, so correct across sleep reboots. */
static uint32_t timer_remaining_s(void) {
  if (!s_tmr_active) return 0;
  if (s_tmr_paused)  return s_tmr_rem_pause;
  uint32_t now = rtc_now_epoch();
  if (now == 0 || s_tmr_target == 0 || now >= s_tmr_target) return 0;
  return s_tmr_target - now;
}

/* True when a running timer has reached its target (needs a valid clock). */
static bool timer_is_due(void) {
  if (!s_tmr_active || s_tmr_paused || s_tmr_target == 0) return false;
  uint32_t now = rtc_now_epoch();
  return now != 0 && now >= s_tmr_target;
}

/* Arm a fresh countdown of `seconds`. */
static void timer_start(uint32_t seconds) {
  if (seconds == 0) return;
  uint32_t now = rtc_now_epoch();
  s_tmr_duration  = seconds;
  s_tmr_target    = now + seconds;   // now==0 (clock unset) -> fires at once; clock is set on this watch
  s_tmr_active    = true;
  s_tmr_paused    = false;
  s_tmr_rem_pause = 0;
  timer_save();
}

static void timer_pause(void) {
  if (!s_tmr_active || s_tmr_paused) return;
  s_tmr_rem_pause = timer_remaining_s();
  s_tmr_paused = true;
  timer_save();
}

static void timer_resume(void) {
  if (!s_tmr_active || !s_tmr_paused) return;
  uint32_t now = rtc_now_epoch();
  s_tmr_target = now + s_tmr_rem_pause;
  s_tmr_paused = false;
  timer_save();
}

/* Stop + clear the countdown (keeps s_tmr_duration as the last-used length). */
static void timer_cancel(void) {
  s_tmr_active = false;
  s_tmr_paused = false;
  s_tmr_target = 0;
  s_tmr_rem_pause = 0;
  timer_save();
}

/* Nudge a running/paused timer by delta seconds (e.g. the +1:00 button), never
 * below zero. */
static void timer_adjust(int32_t delta_s) {
  if (!s_tmr_active) return;
  if (s_tmr_paused) {
    int64_t r = (int64_t)s_tmr_rem_pause + delta_s;
    s_tmr_rem_pause = (r < 0) ? 0 : (uint32_t)r;
  } else {
    uint32_t now = rtc_now_epoch();
    int64_t t = (int64_t)s_tmr_target + delta_s;
    if (now && t < (int64_t)now) t = now;
    s_tmr_target = (uint32_t)t;
  }
  timer_save();
}
