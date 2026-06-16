/* ============================================================================
 *  power_model.h — power-draw estimate + measured battery-drain tracker
 *                  + capacity-aware runtime + battery-health proxy.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER settings_store.h (reads
 *  s_cpu_mhz / s_brightness AND uses `prefs` to persist learned data) and
 *  watch_base.h (i2c_lock) and the hardware rtc object (rtc_now_epoch reads the
 *  PCF85063). Consumed by the Power screen (app_power.h) and refresh_battery().
 *
 *  ---------------------------------------------------------------------------
 *  WHY THIS IS A MODEL, NOT A MEASUREMENT
 *  The AXP2101 has NO load-current ADC. The ONLY hardware-grounded number we get
 *  is the fuel gauge's battery % (a coulomb-counter integral). Everything else
 *  (per-rail mA, awake vs sleep split) has to be inferred from how that % falls.
 *
 *  Three things are learned, in a STRICT one-way dependency order so there is no
 *  circular bootstrap (the old code deadlocked floor<->capacity<->awake-scale and
 *  produced an impossible 23% health):
 *
 *    1. SLEEP FLOOR (mA)  — learned FIRST, with no other unknowns. We only sample
 *       it from windows that are >=95% asleep, where I_sleep ~= I_avg directly
 *       from the gauge. Needs nothing else.
 *    2. AWAKE SCALE (x)   — learned SECOND, once a sleep floor exists to subtract.
 *       Corrects the SUM of the hand-tuned awake model against the gauge.
 *    3. HEALTH / CAPACITY — learned LAST, once the sleep floor is real (the watch
 *       is asleep ~all the time, so the floor dominates the cycle mAh integral;
 *       integrating on the 1 mA guess is what produced the bogus health). Until
 *       the floor is learned, health reports -1 ("learning..."), never a number.
 *
 *  Each stage simply refuses to run until its prerequisite is satisfied, and the
 *  accessors are defined top-to-bottom so a missing-prerequisite is a real value
 *  (guess / 1.0x / -1), never a forward-reference compile error.
 * ========================================================================== */
#pragma once
#include <Arduino.h>

/* ===================== TUNABLE BATTERY PACK ==============================
 * Set these to YOUR cell. Mine is 400 mAh at 3.7V nominal. Health is expressed
 * as a % of design, so changing BATT_DESIGN_MAH re-scales the % automatically
 * and does not corrupt stored learning. */
#define BATT_DESIGN_MAH   400      // design capacity of your pack (mAh)
#define BATT_NOMINAL_MV   3700     // nominal pack voltage (mV)

/* Sleep-floor guess used ONLY before a real floor is learned. Deliberately a
 * placeholder for cycle integration; capacity learning never runs on it. */
#define SLEEP_FLOOR_GUESS_MA   1.0f

/* Learning thresholds. */
#define CYC_AWAKE_GAP_MAX_S    180    // update gap longer than this = deep sleep
#define HEALTH_MIN_CYCLE_PCT     5    // record discharges of at least this span %
#define CALIB_WIN_MIN_S       1800    // a calibration window needs >=30 min ...
#define CALIB_WIN_MIN_PCT        2    //   ... AND a >=2% gauge drop to be usable

static uint16_t s_wifi_active = 0;    // set by the fetch path while WiFi is up

/* ============================================================================
 *  SECTION 0 — clock helper (RTC epoch, survives nothing but used everywhere)
 * ========================================================================== */
static uint32_t rtc_now_epoch(void) {
  i2c_lock();
  RTC_DateTime c = board_clock_now();
  i2c_unlock();
  if (c.getYear() < 2024) return 0;
  struct tm t = {};
  t.tm_year = c.getYear() - 1900; t.tm_mon = c.getMonth() - 1; t.tm_mday = c.getDay();
  t.tm_hour = c.getHour(); t.tm_min = c.getMinute(); t.tm_sec = c.getSecond();
  t.tm_isdst = -1;
  time_t e = mktime(&t);
  return e > 0 ? (uint32_t)e : 0;
}

/* ============================================================================
 *  SECTION 1 — awake power model (hand-tuned constants + learned scale)
 * ========================================================================== */

/* Forward use only: calib_awake_scale() is defined in SECTION 2 but the model
 * below wants it. A single forward declaration (no ordering trap). */
static float calib_awake_scale(void);

/* RAW awake model — hand-tuned, never auto-corrected per-component (the gauge
 * can't separate CPU/screen/WiFi). Tune against a USB meter if desired. */
static uint16_t power_estimate_ma_raw(void) {
  float cpu_ma = 20.0f + (s_cpu_mhz - 80) * (25.0f / 160.0f);   // 80MHz~20, 240MHz~45
  float scr_ma = 10.0f + (s_brightness - 10) * (60.0f / 245.0f);// b10~10, b255~70
  float base   = 12.0f;                                         // RTC/touch/regulators
  float wifi   = s_wifi_active ? 120.0f : 0.0f;
  return (uint16_t)(cpu_ma + scr_ma + base + wifi + 0.5f);
}

/* CALIBRATED awake estimate (raw x learned scale). UI/consumers use this. */
static uint16_t power_estimate_ma(void) {
  return (uint16_t)((float)power_estimate_ma_raw() * calib_awake_scale() + 0.5f);
}
static uint32_t power_estimate_mw(uint16_t vbat_mv) {
  return ((uint32_t)power_estimate_ma() * vbat_mv) / 1000;   // mA*mV/1000 = mW
}

/* ============================================================================
 *  SECTION 2 — calibration state + accessors (sleep floor, awake scale, duty)
 *
 *  Persisted as one NVS blob "calib". All accessors defined here so anything
 *  below can call them with no forward declarations.
 * ========================================================================== */
struct PowerCalib {
  float    sleep_ma;     // learned deep-sleep floor current (mA); 0 = not learned
  uint16_t samples;      // # of floor windows folded into the mean
  float    awake_scale;  // learned multiplier on the raw awake model (1.0 = as-coded)
  uint16_t k_samples;    // # of cycles folded into awake_scale
};
static PowerCalib s_calib = { 0.0f, 0, 1.0f, 0 };

static void calib_save(void) { prefs.putBytes("calib", &s_calib, sizeof(s_calib)); }

static void calib_load(void) {
  size_t got = prefs.getBytes("calib", &s_calib, sizeof(s_calib));
  if (got == 8) {                       // legacy: sleep_ma/samples only
    s_calib.awake_scale = 1.0f;
    s_calib.k_samples   = 0;
  } else if (got != sizeof(s_calib)) {  // absent/unknown -> fresh
    s_calib = (PowerCalib){ 0.0f, 0, 1.0f, 0 };
  }
  // Drop a stale floor learned by the old buggy solver (a real S3 deep-sleep
  // floor is well under ~3 mA; higher = poisoned data). Relearn from scratch.
  if (s_calib.sleep_ma > 3.0f) {
    s_calib.sleep_ma = 0.0f;
    s_calib.samples  = 0;
    calib_save();
  }
}

static bool     calib_is_learned(void)    { return s_calib.samples > 0; }
static float    calib_get_sleep_ma(void)  { return s_calib.sleep_ma; }
static uint16_t calib_get_samples(void)   { return s_calib.samples; }
static uint16_t calib_get_k_samples(void) { return s_calib.k_samples; }
static float    calib_awake_scale(void) {
  return (s_calib.k_samples > 0 && s_calib.awake_scale > 0.0f) ? s_calib.awake_scale : 1.0f;
}
/* Sleep-floor current for cycle integration: learned value, else the guess. */
static float calib_sleep_floor_ma(void) {
  return calib_is_learned() ? s_calib.sleep_ma : SLEEP_FLOOR_GUESS_MA;
}

/* ---- duty-cycle accounting (awake-ms vs sleep-s, in RTC RAM across sleeps) ----
 * The window carries its OWN start epoch + start %, so the gauge %-drop (I_avg)
 * and the duty are measured over EXACTLY the same span — mismatching them is what
 * made the old back-solve collapse. */
RTC_DATA_ATTR static uint64_t calib_awake_ms  = 0;
RTC_DATA_ATTR static uint64_t calib_sleep_ms  = 0;
RTC_DATA_ATTR static uint32_t calib_win_epoch = 0;   // window start epoch; 0 = none
RTC_DATA_ATTR static int8_t   calib_win_pct   = -1;  // gauge % at window start

static void calib_note_awake_ms(uint32_t ms) { calib_awake_ms += ms; }
static void calib_note_sleep_s(uint32_t s)   { calib_sleep_ms += (uint64_t)s * 1000ULL; }

/* Measured awake duty (0..1), or -1 if not enough span yet. */
static float calib_duty(void) {
  uint64_t total = calib_awake_ms + calib_sleep_ms;
  if (total < 60000ULL) return -1.0f;          // <1 min total -> meaningless
  return (float)calib_awake_ms / (float)total;
}

static void calib_window_open(uint32_t now, int pct) {
  calib_win_epoch = now;
  calib_win_pct   = (int8_t)pct;
  calib_awake_ms  = 0;
  calib_sleep_ms  = 0;
}

/* ============================================================================
 *  SECTION 3 — battery-health running average (% of design)
 *
 *  Health = lifetime running mean of learned full capacity, as a % of design.
 *  Each cycle's implied capacity = cycle_model_mAh / (delta% / 100). Samples are
 *  span-weighted (gauge is 1%-quantized, so a big cycle is much less noisy than a
 *  small one). Folded ONLY when the sleep floor is real (SECTION 4 enforces it).
 * ========================================================================== */
struct BattHealth {
  uint16_t design_mah;
  float    avg_mah;       // running mean learned capacity (mAh)
  uint16_t cycle_count;
  float    weight;        // accumulated span-% weight
};
static BattHealth s_health = { BATT_DESIGN_MAH, 0.0f, 0, 0.0f };

/* Optional SD trend-logger hook (defined in batt_health_sd.h, included later;
 * weak no-op fallback so this file stands alone). */
static void batt_health_sd_log(uint32_t epoch, uint16_t cycle, int delta_pct,
                               float learned_mah, int health_pct);

static void health_save(void) { prefs.putBytes("health", &s_health, sizeof(s_health)); }

static void health_load(void) {
  size_t got = prefs.getBytes("health", &s_health, sizeof(s_health));
  if (got == 12) {                       // legacy: design/avg/count (no weight)
    s_health.weight = (float)s_health.cycle_count * 40.0f;   // old 40% gate
  } else if (got != sizeof(s_health)) {  // absent/unknown -> fresh
    s_health = (BattHealth){ BATT_DESIGN_MAH, 0.0f, 0, 0.0f };
  }
  s_health.design_mah = BATT_DESIGN_MAH;
}

static void health_add_cycle(float learned_mah, int delta_pct, uint32_t now) {
  if (learned_mah <= 0.0f || delta_pct <= 0) return;
  float w = (float)delta_pct;            // span-weighted incremental mean
  s_health.avg_mah += w * (learned_mah - s_health.avg_mah) / (s_health.weight + w);
  s_health.weight  += w;
  s_health.cycle_count++;
  health_save();
  int hpct = (int)(s_health.avg_mah / (float)BATT_DESIGN_MAH * 100.0f + 0.5f);
  batt_health_sd_log(now, s_health.cycle_count, delta_pct, learned_mah, hpct);
}

static int health_get_pct(void) {
  if (s_health.avg_mah <= 0.0f) return -1;       // not learned -> "learning..."
  return (int)(s_health.avg_mah / (float)BATT_DESIGN_MAH * 100.0f + 0.5f);
}
static float    health_get_avg_mah(void) { return s_health.avg_mah; }
static uint16_t health_get_cycles(void)  { return s_health.cycle_count; }

/* Effective capacity for runtime math: learned if available, else design. */
static float eff_capacity_mah(void) {
  return (s_health.avg_mah > 0.0f) ? s_health.avg_mah : (float)BATT_DESIGN_MAH;
}

/* ============================================================================
 *  SECTION 4 — the learners (run while discharging, prerequisite-gated)
 * ========================================================================== */

/* Fold one awake-scale sample: k = (real_awake_mAh) / raw_model_mAh, where
 * real_awake_mAh = cycle_real_mAh - sleep_mAh. Guarded against noise. */
static void calib_learn_awake_scale(float real_awake_mah, float awake_raw_mah) {
  if (awake_raw_mah < 5.0f) return;            // too little awake model-mAh -> noisy
  float k = real_awake_mah / awake_raw_mah;
  if (k < 0.3f || k > 3.0f) return;            // implausible -> reject
  s_calib.awake_scale += (k - s_calib.awake_scale) / (float)(s_calib.k_samples + 1);
  s_calib.k_samples++;
  calib_save();
}

/* STAGE 1 + 2: try to learn the sleep floor (and, once a floor exists, refine via
 * the awake scale indirectly through capacity in STAGE 3). Called every
 * discharging update; self-gates on window age/%-drop.
 *
 * The floor is learned ONLY from windows that are >=95% asleep, where the awake
 * term is negligible and I_sleep ~= I_avg straight from the gauge — no awake
 * scale needed, so there is no bootstrap. Once a floor exists we additionally
 * accept partly-awake windows (subtracting the calibrated awake draw). */
static void calib_try_learn(uint32_t now, int pct) {
  if (calib_win_epoch == 0 || calib_win_pct < 0 || pct > calib_win_pct) {
    calib_window_open(now, pct);
    return;
  }
  uint32_t win_dt = now - calib_win_epoch;
  int      win_dp = calib_win_pct - pct;
  if (win_dt < CALIB_WIN_MIN_S || win_dp < CALIB_WIN_MIN_PCT) return;

  float duty = calib_duty();
  if (duty < 0.0f || duty >= 0.999f) return;   // need a separable split

  float hours = (float)win_dt / 3600.0f;
  float i_avg = eff_capacity_mah() * ((float)win_dp / 100.0f) / hours;   // mA, gauge truth

  // Prerequisite gate: with no floor yet, ONLY trust near-fully-asleep windows.
  float i_awake;
  if (!calib_is_learned()) {
    if (duty > 0.05f) return;                  // not asleep enough yet -> wait
    i_awake = 0.0f;                            // awake term negligible
  } else {
    i_awake = (float)power_estimate_ma();      // calibrated awake draw
  }

  float i_sleep = (i_avg - duty * i_awake) / (1.0f - duty);
  if (i_sleep < 0.0f || i_sleep > i_avg) return;   // physical sanity only (no clamp)

  s_calib.sleep_ma += (i_sleep - s_calib.sleep_ma) / (float)(s_calib.samples + 1);
  s_calib.samples++;
  calib_save();
  calib_window_open(now, pct);                 // next window starts fresh here
}

/* ============================================================================
 *  SECTION 5 — drain tracker + cycle integration + health detection
 *
 *  drain_update() runs only while AWAKE, so a long gap between calls IS deep
 *  sleep. We integrate awake intervals at the raw awake model and sleep gaps at
 *  the learned floor, separately, so the residual genuinely belongs to the awake
 *  model (which is what lets the awake scale be solved). On charge we close the
 *  discharge run and, IF the floor is real, fold a capacity + awake-scale sample.
 * ========================================================================== */
RTC_DATA_ATTR static uint32_t drain_anchor_epoch = 0;
RTC_DATA_ATTR static int8_t   drain_anchor_pct   = -1;
RTC_DATA_ATTR static float    drain_pct_per_hour = 0.0f;

RTC_DATA_ATTR static float    cyc_mah_awake   = 0.0f;  // raw awake-model mAh this cycle
RTC_DATA_ATTR static float    cyc_mah_sleep   = 0.0f;  // sleep-floor mAh this cycle
RTC_DATA_ATTR static int8_t   cyc_start_pct   = -1;    // gauge % at cycle start
RTC_DATA_ATTR static uint32_t cyc_last_epoch  = 0;     // last integration timestamp

static void drain_update(int pct, bool charging) {
  uint32_t now = rtc_now_epoch();
  if (now == 0 || pct < 0) return;

  // --- %/hour anchor (drives the %/hr, Gauge-avg, Runtime DISPLAY lines). Resets
  // on charge or when % rises. Independent of the calibration window below.
  if (charging || drain_anchor_pct < 0 || pct > drain_anchor_pct) {
    drain_anchor_epoch = now;
    drain_anchor_pct   = (int8_t)pct;
    drain_pct_per_hour = 0.0f;
  } else {
    uint32_t dt = now - drain_anchor_epoch;
    int      dp = drain_anchor_pct - pct;
    if (dt >= 120 && dp >= 1)
      drain_pct_per_hour = (float)dp * 3600.0f / (float)dt;
  }

  // --- sleep-floor calibration (its own window). Invalidated by charging.
  if (charging) calib_win_epoch = 0;
  else          calib_try_learn(now, pct);

  // --- discharge-cycle integration + health detection ---
  if (charging) {
    if (cyc_start_pct >= 0) {
      int span = cyc_start_pct - pct;
      if (span < 0) span = 0;
      // CAPACITY LEARNING REQUIRES A REAL SLEEP FLOOR. The watch is asleep almost
      // all the time, so cyc_mah_sleep (= floor * hours) dominates the cycle mAh.
      // On the 1 mA guess that integral is ~4-5x too small -> impossible health.
      // So only fold a sample once the floor is learned; until then health is -1.
      if (span >= HEALTH_MIN_CYCLE_PCT && calib_is_learned()) {
        float total_mah = cyc_mah_awake * calib_awake_scale() + cyc_mah_sleep;
        if (total_mah > 0.0f) {
          float learned = total_mah / ((float)span / 100.0f);
          float lo = (float)BATT_DESIGN_MAH * 0.25f;   // sanity band: 25%..200%
          float hi = (float)BATT_DESIGN_MAH * 2.00f;   // of design (one cycle)
          if (learned >= lo && learned <= hi)
            health_add_cycle(learned, span, now);
          // Back-calibrate the awake model: real awake mAh = cycle real mAh - sleep.
          float real_total = eff_capacity_mah() * (float)span / 100.0f;
          calib_learn_awake_scale(real_total - cyc_mah_sleep, cyc_mah_awake);
        }
      }
    }
    cyc_mah_awake = 0.0f; cyc_mah_sleep = 0.0f; cyc_start_pct = -1; cyc_last_epoch = 0;
    return;
  }

  // Discharging: integrate. Short gaps = awake (raw model); long = sleep (floor).
  if (cyc_start_pct < 0) {
    cyc_start_pct  = (int8_t)pct;
    cyc_mah_awake  = 0.0f;
    cyc_mah_sleep  = 0.0f;
    cyc_last_epoch = now;
  } else if (now > cyc_last_epoch) {
    uint32_t gap  = now - cyc_last_epoch;
    float    dt_h = (float)gap / 3600.0f;
    if (gap <= CYC_AWAKE_GAP_MAX_S) cyc_mah_awake += (float)power_estimate_ma_raw() * dt_h;
    else                            cyc_mah_sleep += calib_sleep_floor_ma() * dt_h;
    cyc_last_epoch = now;
  }
}

/* ============================================================================
 *  SECTION 6 — public derived readouts (gauge-grounded where possible)
 * ========================================================================== */
static float drain_get_pct_per_hour(void) { return drain_pct_per_hour; }

/* REAL average current (mA) from the gauge: cap * (%/hr / 100). 0 until converged. */
static float drain_avg_ma(void) {
  if (drain_pct_per_hour <= 0.01f) return 0.0f;
  return eff_capacity_mah() * (drain_pct_per_hour / 100.0f);
}

/* Hours until empty from the gauge %/hour rate (0 if not converged). */
static float drain_get_hours_left(int pct) {
  if (drain_pct_per_hour <= 0.01f || pct < 0) return 0.0f;
  return (float)pct / drain_pct_per_hour;
}

/* Calibrated blended average current (sleep floor + awake model x duty). */
static float power_avg_ma(void) {
  float i_awake = (float)power_estimate_ma();
  if (!calib_is_learned()) return i_awake;
  float duty = calib_duty();
  if (duty < 0.0f) duty = 0.0f;
  return duty * i_awake + (1.0f - duty) * s_calib.sleep_ma;
}

/* Capacity-aware hours-left: remaining_mAh / calibrated average current. */
static float runtime_hours_left_capacity(int pct) {
  if (pct < 0) return 0.0f;
  float remaining_mah = eff_capacity_mah() * (float)pct / 100.0f;
  float ma = power_avg_ma();
  return (ma <= 0.0f) ? 0.0f : remaining_mah / ma;
}
