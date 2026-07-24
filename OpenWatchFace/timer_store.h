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
 *  INCLUDE AFTER settings_store.h (shares its `prefs` handle), power_model.h
 *  (uses rtc_now_epoch()) and storage_fs.h (the alarm clocks' /alarms.csv lives
 *  on store_fs()). Header-only; compiled into the .ino TU.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/Preferences.h"
#else
#include <Preferences.h>
#endif
#include <time.h>          // localtime_r/mktime — alarm-clock wall-time math

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

/* --- alarm-clock state (logic further down in this file) ---
 * Up to ALMC_MAX wall-clock alarms. The LIST lives in /alarms.csv on the shared
 * SD-else-FFat store (hand-editable, like wifi.csv) and is loaded on a full boot;
 * NVS keeps ONLY the earliest armed next-fire epoch ("almc_tgt"), so the
 * deep-sleep wake test works before any filesystem or RTC is up. */
#define ALMC_MAX      8
#define ALMC_CSV_PATH "/alarms.csv"

typedef struct {
  bool     en, rep;
  uint8_t  h, m;          // 0-23 / 0-59
  uint8_t  days;          // bit0=Mon .. bit6=Sun (used when rep)
  uint32_t target;        // next fire epoch (RAM only; 0 = none)
} AlmClk;

static AlmClk   s_almcs[ALMC_MAX];
static uint8_t  s_almc_n      = 0;
static bool     s_almc_loaded = false;   // true once almclk_load() has run this boot. Until
                                         // then s_almcs[] is empty and the only valid target is
                                         // the NVS "almc_tgt" restored by timer_load(); rearm
                                         // must NOT overwrite it from the empty list (that wiped
                                         // armed alarms -> no ring + the value flip-flopping).
static uint32_t s_almc_master = 0;      // earliest enabled target (persisted)

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
  s_almc_master   = prefs.getUInt ("almc_tgt", 0);   // saved ABSOLUTE epoch — usable
                                                     // before the RTC/filesystem are up
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

/* ===================== alarm clocks (multi, CSV-backed) ===================
 * Each alarm rings at its HH:MM — one-shot by default, or on selected weekdays
 * with repeat ON. The list is /alarms.csv on store_fs() (SD if present, else
 * FFat); one alarm per line, hand-editable:
 *     enabled,hour,minute,repeat,days
 * with days a decimal bitmask (bit0=Mon .. bit6=Sun; 31 = Mon-Fri, 127 = all).
 * Lines starting with '#' and blanks are ignored. The whole file is rewritten
 * on every change (it's at most ALMC_MAX lines).
 *
 * The earliest enabled next-fire epoch is mirrored to NVS ("almc_tgt") on every
 * rearm, so the deep-sleep wake decision (which runs before storage mounts) can
 * test it cheaply; the full boot then loads the CSV and rings the right alarm. */

/* How recently-passed an alarm time may still count as "due now" at a deep-sleep WAKE
 * (vs. being rolled to tomorrow). Used ONLY by the wake-due test (almclk_load /
 * almclk_is_due), never by the plain "next future occurrence" math below — so it cannot
 * desync the editor's "Rings in". 90s covers boot latency + a missed minute tick. */
#define ALMC_GRACE_S  90

/* How long PAST an alarm's HH:MM it may still ring (covers deep-sleep wake + full-UI boot
 * latency before the deferred alarm load runs). Wide enough not to drop a ring, but far
 * short of an hour so it never fires at a clearly-wrong time. The wall-clock HH:MM is still
 * checked, so this only ever rings the CORRECT alarm, just possibly a few minutes late. */
#define ALMC_FIRE_WINDOW_S  300

/* Next epoch at which alarm `a` should ring: the STRICTLY-NEXT future occurrence of its
 * HH:MM (0 = never: disabled, clock unset, or repeat with no weekday selected). Walks day
 * by day through localtime/mktime so TZ/DST give the true wall-clock HH:MM. This is the
 * single source of truth for the armed target and the "Rings in" estimate, so it is kept
 * dead simple: always a time in the FUTURE relative to now. (The wake-race for an alarm
 * that arrives during deep sleep is handled in almclk_load(), not here.) */
static uint32_t almclk_next_epoch(const AlmClk *a) {
  if (!a->en) return 0;
  uint32_t now = rtc_now_epoch();
  if (now == 0) return 0;
  for (int d = 0; d < 8; d++) {
    time_t base = (time_t)now + (time_t)d * 86400;
    struct tm lt;
    localtime_r(&base, &lt);
    lt.tm_hour = a->h; lt.tm_min = a->m; lt.tm_sec = 0; lt.tm_isdst = -1;
    time_t t = mktime(&lt);                  // also normalizes lt.tm_wday
    if (t <= (time_t)now) continue;          // strictly future only
    if (a->rep && !(a->days & (1 << ((lt.tm_wday + 6) % 7)))) continue;
    return (uint32_t)t;
  }
  return 0;
}

/* Rewrite the whole CSV from the in-RAM list. */
static void almclk_save_csv(void) {
  if (!store_available()) return;
  File f = store_fs().open(ALMC_CSV_PATH, FILE_WRITE);   // truncates
  if (!f) { USBSerial.println("[almc] csv write failed"); return; }
  f.println("# enabled,hour,minute,repeat,days  (days: bit0=Mon..bit6=Sun; 31=Mon-Fri, 127=all)");
  for (uint8_t i = 0; i < s_almc_n; i++)
    f.printf("%d,%u,%u,%d,%u\n", s_almcs[i].en ? 1 : 0, s_almcs[i].h, s_almcs[i].m,
             s_almcs[i].rep ? 1 : 0, s_almcs[i].days);
  f.close();
}

/* Recompute every alarm's target + persist the earliest to NVS. Call after ANY
 * change and after a fire. Needs a valid RTC (so not from timer_load — the boot
 * wake test uses the previously saved master until almclk_load() runs). */
static void almclk_rearm(void) {
  // GUARD: never rearm from an unloaded list. Before almclk_load() runs (e.g. a light
  // background-check wake, or a clock-set jump handled in loop() that lands before the
  // deferred CSV load), s_almcs[] is empty -> this would compute master=0 and persist 0,
  // WIPING the armed alarm in NVS. The saved "almc_tgt" stays authoritative until load.
  if (!s_almc_loaded) return;
  uint32_t master = 0;
  for (uint8_t i = 0; i < s_almc_n; i++) {
    s_almcs[i].target = almclk_next_epoch(&s_almcs[i]);
    if (s_almcs[i].target && (!master || s_almcs[i].target < master))
      master = s_almcs[i].target;
  }
  s_almc_master = master;
  prefs.putUInt("almc_tgt", master);
}

/* Load the list from CSV (full boot, after storage + RTC are usable). If no CSV
 * exists yet, migrate the pre-multi single alarm out of NVS, then write the CSV. */
static void almclk_load(void) {
  s_almc_n = 0;
  s_almc_loaded = true;   // from here on rearm may persist; before this the NVS target rules
  if (store_available() && store_fs().exists(ALMC_CSV_PATH)) {
    File f = store_fs().open(ALMC_CSV_PATH, FILE_READ);
    while (f && f.available() && s_almc_n < ALMC_MAX) {
      String ln = f.readStringUntil('\n');
      ln.trim();
      if (ln.length() == 0 || ln[0] == '#') continue;
      int en, h, m, rep, days;
      if (sscanf(ln.c_str(), "%d,%d,%d,%d,%d", &en, &h, &m, &rep, &days) == 5 &&
          h >= 0 && h < 24 && m >= 0 && m < 60) {
        s_almcs[s_almc_n++] = { en != 0, rep != 0, (uint8_t)h, (uint8_t)m,
                                (uint8_t)(days & 0x7F), 0 };
      }
    }
    if (f) f.close();
  } else if (prefs.isKey("almc_h")) {
    // one-time migration from the single-alarm NVS keys
    s_almcs[0] = { prefs.getBool("almc_en", false), prefs.getBool("almc_rep", false),
                   prefs.getUChar("almc_h", 7), prefs.getUChar("almc_m", 0),
                   prefs.getUChar("almc_day", 0x1F), 0 };
    s_almc_n = 1;
    prefs.remove("almc_en"); prefs.remove("almc_h"); prefs.remove("almc_m");
    prefs.remove("almc_rep"); prefs.remove("almc_day");
    almclk_save_csv();
    USBSerial.println("[almc] migrated single NVS alarm -> alarms.csv");
  }
  almclk_rearm();

  // Wake-race recovery: a deep-sleep wake fires the timer AT the alarm minute, but the
  // rearm just above computed each target as the next FUTURE occurrence (tomorrow), so the
  // loop would see nothing due and never ring. If the live wall clock is right now reading
  // an enabled alarm's HH:MM (within grace), pull that alarm's target back to "now" so the
  // loop rings it this minute; almclk_fired() then advances it cleanly. This stays out of
  // almclk_next_epoch(), so the editor's "Rings in" is never affected.
  uint32_t now = rtc_now_epoch();
  if (now != 0) {
    time_t tnow = (time_t)now; struct tm lt; localtime_r(&tnow, &lt);
    int now_mod = lt.tm_hour * 60 + lt.tm_min;
    uint32_t master = s_almc_master;
    for (uint8_t i = 0; i < s_almc_n; i++) {
      AlmClk *a = &s_almcs[i];
      if (!a->en) continue;
      int diff = now_mod - (a->h * 60 + a->m);
      if (diff < 0) diff += 1440;
      if (diff * 60 > (int)ALMC_FIRE_WINDOW_S) continue;  // not this alarm's recent minute
      // This alarm's time just passed (within boot latency) -> mark it due so the loop
      // fires it. almclk_due_index() uses the same window, so the ring won't be rejected.
      a->target = now;
      if (!master || now < master) master = now;
    }
    s_almc_master = master;
  }
}

/* Used ONLY as the deep-sleep WAKE gate (whether to full-boot vs. go back to sleep). It is
 * deliberately generous: if the RTC timer woke us anywhere near the armed alarm epoch, full
 * boot and let the loop's HH:MM matcher (almclk_match_now via almclk_fire) decide whether to
 * actually ring. The +5s covers an early wake; the trailing window covers a late wake / a
 * master that already ticked. The ring itself no longer depends on this — it's purely the
 * "don't sleep through the alarm" gate. */
static bool almclk_is_due(void) {
  if (s_almc_master == 0) return false;
  uint32_t now = rtc_now_epoch();
  if (now == 0) return false;
  return (now + 5 >= s_almc_master) && (now <= s_almc_master + (uint32_t)ALMC_FIRE_WINDOW_S);
}

/* Seconds until the earliest armed fire (0 = nothing armed; 1 = due right now). */
static uint32_t almclk_seconds_until(void) {
  if (s_almc_master == 0) return 0;
  uint32_t now = rtc_now_epoch();
  if (now == 0) return 0;
  return (s_almc_master > now) ? (s_almc_master - now) : 1;
}

/* Which alarm is due right now? Earliest-due index, or -1.
 *
 * An alarm is due only when its target epoch has arrived AND the live wall clock still
 * reads that alarm's HH:MM (within ALMC_GRACE_S). The HH:MM cross-check is the guard that
 * makes "rings only at the set time" hold even if the absolute target epoch and the wall
 * clock have drifted apart — e.g. after the user changes the clock while an alarm is armed,
 * which previously left a stale epoch that could fire at the wrong minute. */
static int almclk_due_index(void) {
  uint32_t now = rtc_now_epoch();
  if (now == 0) return -1;
  // Live wall-clock minute-of-day, and how far we are past the alarm's HH:MM today.
  time_t tnow = (time_t)now;
  struct tm lt; localtime_r(&tnow, &lt);
  int now_mod = lt.tm_hour * 60 + lt.tm_min;          // 0..1439
  int best = -1;
  for (uint8_t i = 0; i < s_almc_n; i++) {
    const AlmClk *a = &s_almcs[i];
    if (!a->en || !a->target || a->target > now) continue;
    // Wall clock must actually read this alarm's HH:MM (allow up to the grace window
    // PAST it, so a slightly-late wake still rings the right minute). Diff is the signed
    // minutes since the alarm minute, wrapped into 0..1439.
    int diff = now_mod - (a->h * 60 + a->m);
    if (diff < 0) diff += 1440;
    if (diff * 60 > (int)ALMC_FIRE_WINDOW_S) continue; // not this alarm's minute (within latency)
    if (best < 0 || a->target < s_almcs[best].target) best = i;
  }
  return best;
}

/* ---- simple "ring when the clock matches" detector -------------------------------------
 * Independent of the absolute-epoch target machinery (which was fragile across sleep). The
 * rule is exactly: if any ENABLED alarm's HH:MM equals the current wall clock (and, for a
 * repeat alarm, today is one of its days), it is due. A "last rang" stamp (minute-of-day +
 * day-of-year) makes it fire ONCE per occurrence and never twice in the same minute. The
 * caller is responsible for not ringing while the alarm-settings screen is open. */
static int  s_almc_rang_min = -1;   // minute-of-day we last rang at (-1 = none)
static int  s_almc_rang_yday = -1;  // day-of-year of that ring (so the next day re-arms)

/* Index of an enabled alarm matching the current wall clock that we have NOT already rung
 * this minute, or -1. */
static int almclk_match_now(void) {
  uint32_t now = rtc_now_epoch();
  if (now == 0) return -1;
  time_t tnow = (time_t)now; struct tm lt; localtime_r(&tnow, &lt);
  int now_min  = lt.tm_hour * 60 + lt.tm_min;     // 0..1439
  int now_dow  = (lt.tm_wday + 6) % 7;            // 0=Mon..6=Sun (match a->days bits)
  // Already rang this exact minute today? Then we're done until the minute changes.
  if (now_min == s_almc_rang_min && lt.tm_yday == s_almc_rang_yday) return -1;
  for (uint8_t i = 0; i < s_almc_n; i++) {
    const AlmClk *a = &s_almcs[i];
    if (!a->en) continue;
    if (a->h * 60 + a->m != now_min) continue;    // not this alarm's minute
    if (a->rep && !(a->days & (1 << now_dow))) continue;   // repeat: not a selected day
    return i;
  }
  return -1;
}

/* Record that we rang at the current minute (so almclk_match_now() won't fire again until
 * the clock minute changes). */
static void almclk_mark_rang(void) {
  uint32_t now = rtc_now_epoch();
  if (now == 0) return;
  time_t tnow = (time_t)now; struct tm lt; localtime_r(&tnow, &lt);
  s_almc_rang_min  = lt.tm_hour * 60 + lt.tm_min;
  s_almc_rang_yday = lt.tm_yday;
}

/* Advance alarm `i` past a fire: one-shot disables itself; repeat re-arms. */
static void almclk_fired(int i) {
  if (i < 0 || i >= s_almc_n) return;
  // After ringing, advance past this occurrence. One-shot disables itself. For a repeat,
  // almclk_rearm() -> almclk_next_epoch() returns the next FUTURE occurrence: "now" is a
  // few seconds past the alarm minute, so today's slot is already in the past and skipped,
  // landing on the next valid day. No re-ring loop, no extra state needed.
  if (!s_almcs[i].rep) { s_almcs[i].en = false; almclk_save_csv(); }
  almclk_rearm();
}

/* Append a fresh alarm (07:00, on, one-shot). Returns its index, or -1 if full. */
static int almclk_add(void) {
  if (s_almc_n >= ALMC_MAX) return -1;
  s_almcs[s_almc_n] = { true, false, 7, 0, 0x1F, 0 };
  return s_almc_n++;
}

static void almclk_remove(int i) {
  if (i < 0 || i >= s_almc_n) return;
  for (int k = i + 1; k < s_almc_n; k++) s_almcs[k - 1] = s_almcs[k];
  s_almc_n--;
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
