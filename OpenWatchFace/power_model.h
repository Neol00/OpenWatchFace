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
 *    3. HEALTH / CAPACITY — folded on EVERY discharge cycle of >=5%, from the drain
 *       integrated over the cycle (awake model + sleep floor) divided by the gauge
 *       %-drop. It does NOT wait for the floor: before a floor is learned the floor
 *       guess feeds the integral, so the first qualifying cycle records a number;
 *       the value then sharpens as the floor/awake-scale converge. (The old code
 *       blocked health on the floor and, when the floor never learned — e.g. heavy
 *       use, or background checks off — health was stuck at "learning..." forever.)
 *
 *  Stages 1-2 still gate on their prerequisite for ACCURACY, and the accessors are
 *  defined top-to-bottom so a missing-prerequisite is a real value (guess / 1.0x),
 *  never a forward-reference compile error.
 * ========================================================================== */
#pragma once
#include <Arduino.h>

/* ===================== TUNABLE BATTERY PACK ==============================
 * Set these to YOUR cell. Mine is 400 mAh at 3.7V nominal. Health is expressed
 * as a % of design, so changing BATT_DESIGN_MAH re-scales the % automatically
 * and does not corrupt stored learning. */
#define BATT_DESIGN_MAH   1000      // design capacity of your pack (mAh)
#define BATT_NOMINAL_MV   3700     // nominal pack voltage (mV)

/* PURE deep-sleep floor guess (mA), used before a real floor is learned. Feeds
 * the cycle-drain integral so capacity/health can record from the first >=5%
 * cycle; the learned floor replaces it as soon as one exists. This is the SLEEP
 * current only — awake/periodic-wake draw is integrated separately (cyc_mah_awake),
 * so do NOT set this to the blended gauge average (~10 mA on this board, which
 * already includes the 15-min wakes); ~2 mA is a sane pure-sleep midpoint and the
 * learner (capped at the <=3 mA sane band in calib_load) refines it from real data. */
#define SLEEP_FLOOR_GUESS_MA   2.0f

/* ===================== PER-BOARD AWAKE POWER MODEL =======================
 * The awake draw is modelled in MILLIWATTS (what a USB meter reads), then
 * converted to battery-mA at the live cell voltage by the consumers — so a sagging
 * cell correctly pulls more mA for the same power, and the constants below are board
 * power, free of any regulator-efficiency fudge.
 *
 * Each board header may override these; the defaults here are the MEASURED
 * S3-1.47 (Waveshare ESP32-S3-Touch-LCD-1.47) figures, taken on a 5 V USB meter
 * (rail sits ~4.23 V), undervolted. Derivation from the raw measurements:
 *   - Idle 3% bright = 0.31 W, 100% = 0.50 W  -> backlight ~linear: span 0.19 W,
 *     0%-intercept (non-screen floor) ~0.304 W.
 *   - CPU dynamic (floor subtracted) fits P ~= k * fMHz * Vcore^2 * load, with
 *     k ~= 0.00244 mW per (MHz * V^2 * load-fraction) from the 240/160/80 MHz,
 *     1067/1020/980 mV, 40%/70%-load points. Modelled against the LIVE core mV so
 *     undervolt vs stock is just a different V in the same formula (no separate
 *     table). Stock-voltage measurements, when taken, validate/ą refine k.
 *   - BLE active adds ~0.15 W. WiFi showed NO measurable delta (radio seems always
 *     on), so it's folded into the floor, not a separate adder.
 *   - Deep sleep read 0.00 W (below meter resolution) — sleep draw is handled by
 *     the floor learner, NOT this awake model.
 * Boards with bigger/AMOLED panels (S3-2.06) or no IMU override the screen / IMU
 * terms in their own header. Tune any number against a meter without touching code. */

/* Non-screen idle floor (mW): CPU-idle + regulators + RTC + touch + always-on radio. */
#ifndef BOARD_PWR_FLOOR_MW
#define BOARD_PWR_FLOOR_MW        304.0f
#endif
/* Backlight/emitter span (mW) added linearly from brightness 0 -> 255. */
#ifndef BOARD_PWR_SCREEN_MW_FULL
#define BOARD_PWR_SCREEN_MW_FULL  196.0f
#endif
/* CPU dynamic coefficient: mW = k * fMHz * (Vcore_volts^2) * load_fraction. */
#ifndef BOARD_PWR_CPU_K
#define BOARD_PWR_CPU_K           0.00244f
#endif
/* Radio (BLE/active connection) adder (mW), applied when BLE is up. */
#ifndef BOARD_PWR_RADIO_MW
#define BOARD_PWR_RADIO_MW        150.0f
#endif
/* IMU active adder (mW). Default 0; boards WITH an IMU set this. ~0.5 mA @ 3.7 V
 * (pedometer/accel-only) ~= 1.9 mW; ~1.5 mA (accel+gyro) ~= 5.5 mW. Placeholder
 * until measured on a board that has the QMI8658 (the S3-1.47 has none). */
#ifndef BOARD_PWR_IMU_MW
#define BOARD_PWR_IMU_MW          0.0f
#endif

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

/* Resting-voltage -> state-of-charge (%), single LiPo cell. This is an INDEPENDENT
 * view of SoC from the coulomb-counter %: the curve maps open-circuit-ish cell
 * voltage to charge remaining. Used to cross-check the gauge and back out true
 * capacity (SECTION 5). Piecewise-linear over a typical LiPo discharge curve;
 * anchored at this hardware's 4.10 V full ceiling (the charger tops at ~4.1 V, not
 * 4.2 V) down to ~3.05 V empty. Returns -1 if the voltage is implausible (0/charging
 * spike) so callers can skip the sample rather than trust a bad SoC.
 *
 * NOTE: voltage sags under load, so a single instantaneous read is noisy. The
 * capacity learner only uses the ENDPOINTS of a multi-%% discharge (where the error
 * averages down) and weights by span — it never trusts one spot reading. */
static float volt_soc_pct(uint16_t mv) {
  if (mv < 2800 || mv > 4400) return -1.0f;          // implausible -> unusable
  static const struct { uint16_t mv; float pct; } C[] = {
    {3050,0},{3300,15},{3500,32},{3650,55},{3750,65},{3850,82},{3900,90},{4000,99},{4200,100}
  };
  const int N = sizeof(C) / sizeof(C[0]);
  if (mv <= C[0].mv)   return 0.0f;
  if (mv >= C[N-1].mv) return 100.0f;
  for (int i = 1; i < N; i++) {
    if (mv < C[i].mv) {
      float span = (float)(C[i].mv - C[i-1].mv), into = (float)(mv - C[i-1].mv);
      return C[i-1].pct + into * (C[i].pct - C[i-1].pct) / span;
    }
  }
  return 100.0f;
}

/* ============================================================================
 *  SECTION 1 — awake power model (per-board, in MILLIWATTS)
 *
 *  Built per-component from the BOARD_PWR_* constants + LIVE signals:
 *    - screen: floor + brightness-linear emitter span
 *    - CPU:    k * fMHz * Vcore^2 * load, where Vcore is the live applied core mV
 *              (so undervolt vs stock is automatic) and load is the live per-core
 *              CPU usage (so idle vs heavy redraw differ, matching the 40%/70% data)
 *    - radio:  BLE-active adder (WiFi folded into the floor — no measurable delta)
 *    - IMU:    adder when the part exists AND is actively sampling
 *  Output is BOARD POWER in mW; consumers convert to battery-mA at the live cell
 *  voltage. The learned awake_scale still multiplies the whole thing so the gauge
 *  can trim residual model error over cycles.
 * ========================================================================== */

/* Forward use only: calib_awake_scale() is defined in SECTION 2 but the model
 * below wants it. A single forward declaration (no ordering trap). */
static float calib_awake_scale(void);

/* Radio-active flag, set by the BLE layer (included AFTER this file, so it can't be
 * called directly here — mirrors s_wifi_active). 1 while a BLE link/advertising is up. */
static uint8_t s_ble_active = 0;

/* Live applied core voltage in VOLTS (undervolt-aware). Falls back to a nominal
 * 1.10 V if the core-voltage layer isn't available on this board. */
static float core_voltage_v(void) {
#if defined(CORE_UV_VIA_REGI2C) || defined(CORE_UV_VIA_PMU)
  return (float)core_dbias_to_mv(core_get_dig_dbias()) / 1000.0f;
#else
  return 1.10f;
#endif
}

/* Live average CPU load fraction (0..1) across both cores. */
static float cpu_load_frac(void) {
  uint16_t sum = (uint16_t)cpu_usage_pct(0) + (uint16_t)cpu_usage_pct(1);
  float f = (float)sum / 200.0f;            // two cores, each 0..100
  return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

/* RAW awake model in MILLIWATTS (board power). Per-component, per-board, live. */
static float power_estimate_mw_raw(void) {
  float scr = BOARD_PWR_FLOOR_MW
            + BOARD_PWR_SCREEN_MW_FULL * ((float)s_brightness / 255.0f);
  float vc  = core_voltage_v();
  float cpu = BOARD_PWR_CPU_K * (float)s_cpu_mhz * vc * vc * cpu_load_frac();
  float radio = s_ble_active ? (float)BOARD_PWR_RADIO_MW : 0.0f;
#if BOARD_HAS_IMU_QMI8658
  float imu = imu_steps_running() ? (float)BOARD_PWR_IMU_MW : 0.0f;
#else
  float imu = 0.0f;
#endif
  return scr + cpu + radio + imu;
}

/* CALIBRATED awake estimate in mW (raw x gauge-learned scale). */
static uint32_t power_estimate_mw_model(void) {
  float mw = power_estimate_mw_raw() * calib_awake_scale();
  return (uint32_t)(mw + 0.5f);
}

/* Awake draw as battery-mA at a given cell voltage: mW / V_cell. The model is board
 * power, so a sagging cell correctly converts to MORE mA. Pass the live cell mV;
 * falls back to nominal if 0. */
static uint16_t power_estimate_ma_at(uint16_t vbat_mv) {
  uint16_t v = vbat_mv ? vbat_mv : BATT_NOMINAL_MV;
  return (uint16_t)(((float)power_estimate_mw_model() * 1000.0f) / (float)v + 0.5f);
}

/* RAW awake draw as battery-mA at nominal voltage — used by the cycle integrator
 * (cyc_mah_awake), kept in mA for continuity with the sleep-floor mA integral. */
static uint16_t power_estimate_ma_raw(void) {
  return (uint16_t)((power_estimate_mw_raw() * 1000.0f) / (float)BATT_NOMINAL_MV + 0.5f);
}

/* CALIBRATED awake mA at nominal voltage (UI "Draw" line uses this). */
static uint16_t power_estimate_ma(void) {
  return power_estimate_ma_at(BATT_NOMINAL_MV);
}

/* Awake power in mW at a cell voltage (legacy signature kept for callers; the model
 * is already in mW so vbat is only used to stay battery-referenced/consistent). */
static uint32_t power_estimate_mw(uint16_t vbat_mv) {
  (void)vbat_mv;
  return power_estimate_mw_model();
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
 *  Each cycle's implied capacity = cycle_drain_mAh / (delta% / 100), where
 *  cycle_drain_mAh is the average drain integrated over the cycle (awake model +
 *  sleep floor). Samples are span-weighted (gauge is 1%-quantized, so a big cycle
 *  is much less noisy than a small one) and folded on every discharge of at least
 *  HEALTH_MIN_CYCLE_PCT. The sleep floor sharpens the integral as it converges but
 *  is NOT a prerequisite — before one is learned the floor guess is used, so the
 *  first qualifying cycle already produces a number (no "learning..." deadlock).
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
 *  SECTION 5 — drain tracker + capacity (health) learner
 *
 *  drain_update() runs only while AWAKE, so a long gap between calls IS deep sleep.
 *  Two things happen each call: (1) a %/hour anchor for the live draw readout, and
 *  (2) per-discharge-cycle capacity learning, closed when the charger is plugged in.
 *
 *  CAPACITY (health) is learned two ways, best-first:
 *    - SMART (S3/AXP2101, has a coulomb gauge): the coulomb % and the cell-voltage
 *      SoC are INDEPENDENT views of the same discharge. Coulomb says we drew span%
 *      of design charge; voltage says the true SoC fell vsoc_drop%. Their ratio is
 *      the real capacity:  C = (design*span/100) / (vsoc_drop/100). No current
 *      sensor, no power model — just two charge views. This is what makes long-term
 *      degradation measurable: as the pack ages, the same coulomb-span empties the
 *      VOLTAGE faster, so C falls.
 *    - FALLBACK (ADC boards, or no usable voltage span): integrate the drain model
 *      over the cycle / coulomb-% dropped. Only as good as the model; on ADC boards
 *      pct is already voltage-derived so there's no independent second signal.
 *  A sample is folded on every discharge of >=HEALTH_MIN_CYCLE_PCT, span-weighted.
 *  The awake-scale model correction is additionally folded once a real sleep floor
 *  exists (it needs the floor to separate awake vs sleep mAh).
 *
 *  CAVEAT: cell voltage right off the charger sits high (surface charge), biasing
 *  the start SoC up a little. Span-weighting + the 25%..200% sanity band bound the
 *  error, and the bias is roughly constant cycle-to-cycle so RELATIVE fade over
 *  months — the actual goal — still tracks. For an accurate ABSOLUTE number, learn
 *  from deep discharges (large vsoc_drop), where the endpoint error averages down.
 * ========================================================================== */
RTC_DATA_ATTR static uint32_t drain_anchor_epoch = 0;
RTC_DATA_ATTR static int8_t   drain_anchor_pct   = -1;
RTC_DATA_ATTR static float    drain_pct_per_hour = 0.0f;

RTC_DATA_ATTR static float    cyc_mah_awake   = 0.0f;  // raw awake-model mAh this cycle
RTC_DATA_ATTR static float    cyc_mah_sleep   = 0.0f;  // sleep-floor mAh this cycle
RTC_DATA_ATTR static int8_t   cyc_start_pct   = -1;    // coulomb gauge % at cycle start
RTC_DATA_ATTR static int16_t  cyc_start_vsoc  = -1;    // voltage-SoC %% (x10) at cycle start; -1 = none
RTC_DATA_ATTR static int16_t  cyc_last_vsoc   = -1;    // most-recent DISCHARGING voltage-SoC %% (x10)
RTC_DATA_ATTR static uint32_t cyc_last_epoch  = 0;     // last integration timestamp

/* drain_update(pct, charging, vbat_mv): pct = coulomb gauge % (or voltage-% on ADC
 * boards), vbat_mv = measured CELL voltage for the independent SoC cross-check.
 * Pass 0 for vbat_mv if no voltage is available (disables the cross-check; falls
 * back to the model integral). */
static void drain_update(int pct, bool charging, uint16_t vbat_mv) {
  uint32_t now = rtc_now_epoch();
  if (now == 0 || pct < 0) return;

  // Independent voltage-derived SoC for the capacity cross-check (S3/AXP2101 only:
  // there pct is a COULOMB integral, so voltage is a genuinely separate signal. On
  // ADC boards pct is ALREADY voltage-derived, so this would be the same number —
  // we gate the cross-check to the PMU board below and let ADC boards fall back to
  // the model integral). -1 when unavailable.
  float vsoc = (vbat_mv && !charging) ? volt_soc_pct(vbat_mv) : -1.0f;

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

  // --- discharge-cycle close: learn capacity (health) + back-calibrate model ---
  if (charging) {
    if (cyc_start_pct >= 0) {
      int span = cyc_start_pct - pct;            // coulomb-% dropped this discharge
      if (span < 0) span = 0;
      if (span >= HEALTH_MIN_CYCLE_PCT) {
        float learned = -1.0f;                   // learned full-pack capacity (mAh)

#if BOARD_HAS_PMU_AXP2101
        // SMART PATH (coulomb gauge present): the cell voltage is an INDEPENDENT
        // view of SoC from the coulomb %. Over this discharge the coulomb counter
        // says we drew (span%) of design charge = BATT_DESIGN_MAH*span/100 mAh. The
        // VOLTAGE curve says the true SoC fell by (vsoc_drop)%. If the pack were
        // exactly design capacity those agree; if voltage emptied FASTER, the pack
        // holds less:  C_real = charge_drawn_mAh / (vsoc_drop/100).
        // This needs NO current sensor and NO power model — two charge views only.
        // The END SoC is the LAST DISCHARGING read (cyc_last_vsoc): vbat_mv is 0/VBUS
        // now that the charger is in, so we can't read a clean cell voltage here.
        if (cyc_start_vsoc >= 0 && cyc_last_vsoc >= 0) {
          float vsoc_drop = (float)(cyc_start_vsoc - cyc_last_vsoc) / 10.0f;   // %%
          // Need a real, monotone voltage drop; tiny drops sit on the curve's flat
          // mid-region where voltage SoC is too noisy to divide by.
          if (vsoc_drop >= (float)HEALTH_MIN_CYCLE_PCT) {
            float charge_drawn = (float)BATT_DESIGN_MAH * (float)span / 100.0f;
            learned = charge_drawn / (vsoc_drop / 100.0f);
          }
        }
#endif
        // FALLBACK (no coulomb gauge, or no usable voltage span this cycle): the
        // model integral — drain integrated over the cycle / coulomb-% dropped.
        // On ADC boards pct IS voltage-derived, so there's no second signal and this
        // is the only option; it's only as good as the power model.
        if (learned <= 0.0f) {
          float total_mah = cyc_mah_awake * calib_awake_scale() + cyc_mah_sleep;
          if (total_mah > 0.0f) learned = total_mah / ((float)span / 100.0f);
        }

        if (learned > 0.0f) {
          float lo = (float)BATT_DESIGN_MAH * 0.25f;   // sanity band: 25%..200%
          float hi = (float)BATT_DESIGN_MAH * 2.00f;   // of design (one cycle)
          if (learned >= lo && learned <= hi)
            health_add_cycle(learned, span, now);
        }

        // Back-calibrate the awake model from the now-trusted capacity (real awake
        // mAh = cycle real mAh - sleep). Only meaningful once a real floor exists.
        if (calib_is_learned()) {
          float real_total = eff_capacity_mah() * (float)span / 100.0f;
          calib_learn_awake_scale(real_total - cyc_mah_sleep, cyc_mah_awake);
        }
      }
    }
    cyc_mah_awake = 0.0f; cyc_mah_sleep = 0.0f;
    cyc_start_pct = -1; cyc_start_vsoc = -1; cyc_last_vsoc = -1; cyc_last_epoch = 0;
    return;
  }

  // Track the most-recent usable DISCHARGING voltage-SoC as the cycle's running end
  // point (the value used at charge-close, since we can't read a clean cell voltage
  // once the charger is in). Updated every call that has a valid voltage.
  if (vsoc >= 0.0f) cyc_last_vsoc = (int16_t)(vsoc * 10.0f + 0.5f);

  // Discharging: integrate. Short gaps = awake (raw model); long = sleep (floor).
  if (cyc_start_pct < 0) {
    cyc_start_pct  = (int8_t)pct;
    // Anchor the cycle's voltage-SoC start for the capacity cross-check. Stored x10
    // so a half-percent of curve resolution survives the int. -1 if no usable read.
    cyc_start_vsoc = (vsoc >= 0.0f) ? (int16_t)(vsoc * 10.0f + 0.5f) : -1;
    cyc_mah_awake  = 0.0f;
    cyc_mah_sleep  = 0.0f;
    cyc_last_epoch = now;
  } else if (now > cyc_last_epoch) {
    uint32_t gap  = now - cyc_last_epoch;
    float    dt_h = (float)gap / 3600.0f;
    if (gap <= CYC_AWAKE_GAP_MAX_S) {
      cyc_mah_awake += (float)power_estimate_ma_raw() * dt_h;
      calib_note_awake_ms(gap * 1000UL);   // measured awake span -> duty learner
    } else {
      cyc_mah_sleep += calib_sleep_floor_ma() * dt_h;
      calib_note_sleep_s(gap);             // MEASURED sleep span -> duty learner. This is the
                                           // single source of the floor-learner's duty, so it
                                           // works even with periodic background checks OFF
                                           // (the old path only noted sleep when checks were on,
                                           // so duty stayed ~1.0 and the floor never learned ->
                                           // health was stuck "learning..." forever).
    }
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
