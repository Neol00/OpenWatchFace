/* ============================================================================
 *  power_model.h — power-draw estimate + measured battery-drain tracker
 *                  + capacity-aware runtime + battery-health proxy.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER settings_store.h (reads
 *  s_cpu_mhz / s_brightness AND uses `prefs` to persist learned health) and
 *  watch_base.h (i2c_lock) and the hardware rtc object (rtc_now_epoch reads the
 *  PCF85063). Consumed by the Power screen (app_power.h) and refresh_battery().
 * ========================================================================== */
#pragma once
#include <Arduino.h>

/* ===================== TUNABLE BATTERY PACK ==============================
 * Set these to YOUR cell. Mine is 400 mAh at 3.7V nominal. These feed the
 * capacity-aware "time left" math and the health estimate below. Changing
 * BATT_DESIGN_MAH only changes the model — it does not touch stored health
 * (health is expressed as a % of design, so it re-scales automatically). */
#define BATT_DESIGN_MAH   400      // <-- design capacity of your pack (mAh)
#define BATT_NOMINAL_MV   3700     // <-- nominal pack voltage (mV), for Wh display

/* ----- Power-draw ESTIMATE (not measured) -----
 * The AXP2101 on this board has NO load-current ADC, so we cannot MEASURE draw.
 * This is a rough model from the few big, known consumers: CPU frequency, screen
 * brightness, and whether WiFi is currently active. Numbers are ballpark mA at
 * the battery; treat them as a relative gauge, not a meter. Tune the constants
 * against a real USB power meter if you ever want them closer.
 *
 * Baselines (screen ON, idle): a rough ESP32-S3 + AMOLED watch profile.
 *   - CPU: ~scales with frequency. 240MHz ~ 45mA core, 80MHz ~ 20mA.
 *   - Screen: AMOLED ~ 10..70mA across brightness 10..255 (content-dependent).
 *   - WiFi active: +~120mA while a fetch is in flight (brief).
 * Deep sleep is separate and not shown here (menu only opens while awake). */
static uint16_t s_wifi_active = 0;   // set by the fetch path while WiFi is up

static uint16_t power_estimate_ma(void) {
  // CPU contribution: linear-ish between 80MHz(~20mA) and 240MHz(~45mA).
  float cpu_ma = 20.0f + (s_cpu_mhz - 80) * (25.0f / 160.0f);
  // Screen contribution: ~10mA at brightness 10, ~70mA at 255.
  float scr_ma = 10.0f + (s_brightness - 10) * (60.0f / 245.0f);
  float base   = 12.0f;                 // misc rails (RTC, touch, regulators)
  float wifi   = s_wifi_active ? 120.0f : 0.0f;
  float total  = cpu_ma + scr_ma + base + wifi;
  return (uint16_t)(total + 0.5f);
}

/* Instantaneous power estimate in milliwatts = est_mA * V_batt. */
static uint32_t power_estimate_mw(uint16_t vbat_mv) {
  return ((uint32_t)power_estimate_ma() * vbat_mv) / 1000;  // mA * mV / 1000 = mW
}

/* ----- Battery drain tracker (1-hour estimate) -----
 * Measures the REAL average drain by watching how battery % falls over time.
 * Anchor (epoch + percent) lives in RTC RAM so it survives deep-sleep wakes.
 * From the anchor we derive %/hour and hours-to-empty. Unlike the instant model
 * above, this is grounded in actual battery behaviour — but it needs some time
 * to converge and resets when charging. */
RTC_DATA_ATTR static uint32_t drain_anchor_epoch = 0;
RTC_DATA_ATTR static int8_t   drain_anchor_pct   = -1;
RTC_DATA_ATTR static float    drain_pct_per_hour = 0.0f;  // last computed rate

/* Current local epoch from the RTC (fields treated as local time). 0 if unset. */
static uint32_t rtc_now_epoch(void) {
  i2c_lock();   // shared bus
  RTC_DateTime c = rtc.getDateTime();
  i2c_unlock();
  if (c.getYear() < 2024) return 0;
  struct tm t = {};
  t.tm_year = c.getYear() - 1900; t.tm_mon = c.getMonth() - 1; t.tm_mday = c.getDay();
  t.tm_hour = c.getHour(); t.tm_min = c.getMinute(); t.tm_sec = c.getSecond();
  t.tm_isdst = -1;
  time_t e = mktime(&t);
  return e > 0 ? (uint32_t)e : 0;
}

/* ----- Battery-health learning (cross-cycle running average) -----
 * We have NO current sensor, so health is a PROXY, not a measurement. Over a
 * discharge we accumulate model mAh (power_estimate_ma * dt) AND watch how many
 * gauge-% the battery actually dropped. The effective full capacity implied by a
 * cycle is:   learned_mah = accumulated_mah / (delta_pct / 100).
 * A single cycle is noisy (the mA model is approximate), so we fold each cycle's
 * result into a RUNNING AVERAGE over the battery's whole life — the mean
 * converges toward an accurate figure as cycles accumulate. The average is stored
 * incrementally in NVS (one 'health' blob, ~12 bytes) so it needs NO history
 * array and survives reboots/power-off. Health % = avg_mah / design_mah * 100.
 *
 * FUTURE (separate task): once avg_mah is trusted, back-calibrate
 * power_estimate_ma() against it so the mA model stops being a pure guess. */
struct BattHealth {
  uint16_t design_mah;     // design capacity this average was built against
  float    avg_mah;        // running mean of learned capacity (mAh)
  uint16_t cycle_count;    // # of cycles folded into the mean
};
static BattHealth s_health = { BATT_DESIGN_MAH, 0.0f, 0 };

/* Optional SD trend-logger hook. Defined in batt_health_sd.h (included after this
 * file). Declared weakly here as a no-op fallback so power_model.h stands alone
 * even if the SD logger isn't compiled in. */
static void batt_health_sd_log(uint32_t epoch, uint16_t cycle, int delta_pct,
                               float learned_mah, int health_pct);

static void health_load(void) {
  size_t got = prefs.getBytes("health", &s_health, sizeof(s_health));
  if (got != sizeof(s_health)) {           // absent/old -> fresh
    s_health.design_mah  = BATT_DESIGN_MAH;
    s_health.avg_mah     = 0.0f;
    s_health.cycle_count = 0;
  }
  // If the design capacity was changed in code, rescale nothing — health is a %
  // of whatever design it learned against; just refresh the stored design field.
  s_health.design_mah = BATT_DESIGN_MAH;
}
static void health_save(void) { prefs.putBytes("health", &s_health, sizeof(s_health)); }

/* Fold one completed discharge cycle into the lifetime running average. */
static void health_add_cycle(float learned_mah, int delta_pct, uint32_t now) {
  if (learned_mah <= 0.0f) return;
  // Incremental mean: avg += (x - avg) / (n + 1). Averages ALL cycles ever in a
  // fixed ~12 bytes — no per-cycle history needed for the headline number.
  s_health.avg_mah += (learned_mah - s_health.avg_mah) / (float)(s_health.cycle_count + 1);
  s_health.cycle_count++;
  health_save();
  // Per-cycle trend point goes to SD (if a card is present) for offline graphing;
  // the average above does NOT depend on it.
  int hpct = (int)(s_health.avg_mah / (float)BATT_DESIGN_MAH * 100.0f + 0.5f);
  batt_health_sd_log(now, s_health.cycle_count, delta_pct, learned_mah, hpct);
}

static int   health_get_pct(void) {
  if (s_health.avg_mah <= 0.0f) return -1;            // not learned yet
  return (int)(s_health.avg_mah / (float)BATT_DESIGN_MAH * 100.0f + 0.5f);
}
static float    health_get_avg_mah(void) { return s_health.avg_mah; }
static uint16_t health_get_cycles(void)  { return s_health.cycle_count; }

/* ----- Discharge mAh accumulator (drives both time-left and health) -----
 * Between drain_update() calls we integrate model current over elapsed time to
 * get mAh consumed, and remember the % at which this discharge run started. Kept
 * in RTC RAM so a deep-sleep wake (a reboot) doesn't lose the in-progress cycle. */
RTC_DATA_ATTR static float    cyc_mah_used    = 0.0f;  // model mAh since cycle start
RTC_DATA_ATTR static int8_t   cyc_start_pct   = -1;    // gauge % at cycle start
RTC_DATA_ATTR static uint32_t cyc_last_epoch  = 0;     // last integration timestamp

#define HEALTH_MIN_CYCLE_PCT  40   // need >=40% of span discharged to trust a cycle

/* Forward decl: drain_update() triggers a calibration attempt once the gauge
 * %/hr has converged; the calibration code is defined below it. */
static void calib_try_learn(void);

/* Call periodically with the current battery % and charging state. Updates the
 * %/hour estimate, integrates model mAh, and on a meaningful discharge cycle
 * folds a learned-capacity sample into the running average. */
static void drain_update(int pct, bool charging) {
  uint32_t now = rtc_now_epoch();
  if (now == 0 || pct < 0) return;

  // --- %/hour anchor (unchanged behaviour) ---
  if (charging || drain_anchor_pct < 0 || pct > drain_anchor_pct) {
    drain_anchor_epoch = now;
    drain_anchor_pct   = (int8_t)pct;
    drain_pct_per_hour = 0.0f;
  } else {
    uint32_t dt = now - drain_anchor_epoch;
    int      dp = drain_anchor_pct - pct;
    if (dt >= 120 && dp >= 1) {
      drain_pct_per_hour = (float)dp * 3600.0f / (float)dt;
      calib_try_learn();   // converged drain rate -> try to learn the sleep floor
    }
  }

  // --- mAh accumulation + health cycle detection ---
  if (charging) {
    // Charging ends/invalidates the discharge run. If we got a big enough span,
    // learn a capacity sample before resetting.
    if (cyc_start_pct >= 0) {
      int span = cyc_start_pct - pct;            // % actually discharged
      if (span < 0) span = 0;
      if (span >= HEALTH_MIN_CYCLE_PCT && cyc_mah_used > 0.0f) {
        float learned = cyc_mah_used / ((float)span / 100.0f);
        health_add_cycle(learned, span, now);
      }
    }
    cyc_mah_used = 0.0f; cyc_start_pct = -1; cyc_last_epoch = 0;
    return;
  }

  // Discharging: integrate model current over the elapsed interval.
  if (cyc_start_pct < 0) {                        // start a fresh discharge run
    cyc_start_pct  = (int8_t)pct;
    cyc_mah_used   = 0.0f;
    cyc_last_epoch = now;
  } else if (now > cyc_last_epoch) {
    float dt_h = (float)(now - cyc_last_epoch) / 3600.0f;
    cyc_mah_used  += (float)power_estimate_ma() * dt_h;   // mA * h = mAh
    cyc_last_epoch = now;
  }
}

static float drain_get_pct_per_hour(void) { return drain_pct_per_hour; }

/* REAL average current (mA) over the current drain window, straight from the fuel
 * gauge:  I_avg = capacity_mAh * (%/hr / 100).  Returns 0 until the %/hr rate has
 * converged. This is the ONLY hardware-grounded current the AXP2101 allows — it has
 * no current ADC, so everything else (power_estimate_ma) is a model. Because it
 * averages over the window (mostly deep sleep), it's exactly the number that shows
 * the deep-sleep savings from rail cutting. */
static float drain_avg_ma(void) {
  if (drain_pct_per_hour <= 0.01f) return 0.0f;
  float cap = (s_health.avg_mah > 0.0f) ? s_health.avg_mah : (float)BATT_DESIGN_MAH;
  return cap * (drain_pct_per_hour / 100.0f);
}

/* Hours until empty from the gauge %/hour rate (0 if not yet converged). */
static float drain_get_hours_left(int pct) {
  if (drain_pct_per_hour <= 0.01f || pct < 0) return 0.0f;
  return (float)pct / drain_pct_per_hour;
}

/* ===================== Self-calibrating average-current model ============
 * We still have NO current sensor, so we cannot measure awake mA directly. But
 * the gauge gives us the REAL average current over a window:
 *     I_avg_real = capacity_mAh * (drain_%/hr / 100)
 * and the firmware can MEASURE the awake duty cycle (awake-ms vs sleep-seconds it
 * accumulates across deep-sleep cycles). With duty known, the only unknown in
 *     I_avg = duty * I_awake + (1 - duty) * I_sleep
 * is I_sleep — the deep-sleep floor current, which dominates week-long runtime.
 * We solve it from windows where the gauge %/hr has converged, and fold the
 * result into a lifetime running average in NVS (key "calib").
 *
 * NOTE: the gauge can only constrain the AVERAGE / sleep-floor — NOT the awake
 * per-component (CPU/screen/WiFi) split, which has no sensor to separate it. So
 * power_estimate_ma() stays the (rough, labeled-est) awake model; what we learn
 * here is I_sleep, and from it a calibrated blended average for runtime. */
struct PowerCalib {
  float    sleep_ma;     // learned deep-sleep floor current (mA)
  uint16_t samples;      // # of windows folded into the average
};
static PowerCalib s_calib = { 0.0f, 0 };

static void calib_load(void) {
  if (prefs.getBytes("calib", &s_calib, sizeof(s_calib)) != sizeof(s_calib)) {
    s_calib.sleep_ma = 0.0f; s_calib.samples = 0;
  }
}
static void calib_save(void) { prefs.putBytes("calib", &s_calib, sizeof(s_calib)); }

static float    calib_get_sleep_ma(void) { return s_calib.sleep_ma; }
static uint16_t calib_get_samples(void)  { return s_calib.samples; }
static bool     calib_is_learned(void)   { return s_calib.samples > 0; }

/* Awake/sleep time accounting for the duty cycle (RTC RAM -> survives sleep
 * reboots). The lifecycle code notes how long each wake stayed awake and how long
 * the following deep sleep lasted; over a window that gives duty = awake/total. */
RTC_DATA_ATTR static uint64_t calib_awake_ms  = 0;
RTC_DATA_ATTR static uint64_t calib_sleep_ms  = 0;
RTC_DATA_ATTR static uint32_t calib_win_epoch = 0;   // %/hr window start
RTC_DATA_ATTR static int8_t   calib_win_pct   = -1;

static void calib_note_awake_ms(uint32_t ms) { calib_awake_ms += ms; }
static void calib_note_sleep_s(uint32_t s)   { calib_sleep_ms += (uint64_t)s * 1000ULL; }

/* Current measured awake duty (0..1), or -1 if we don't have enough span yet. */
static float calib_duty(void) {
  uint64_t total = calib_awake_ms + calib_sleep_ms;
  if (total < 60000ULL) return -1.0f;          // <1 min total -> not meaningful
  return (float)calib_awake_ms / (float)total;
}

/* Try to learn one I_sleep sample. Called from drain_update() once the gauge
 * %/hr has converged over a window AND we have duty + a learned capacity. Folds
 * the solved sleep-floor into the lifetime running average and resets the window. */
static void calib_try_learn(void) {
  float duty = calib_duty();
  if (duty < 0.0f) return;
  if (drain_pct_per_hour <= 0.01f) return;     // need a converged drain rate
  // Real average current from the gauge + learned (or design) capacity.
  float cap = (s_health.avg_mah > 0.0f) ? s_health.avg_mah : (float)BATT_DESIGN_MAH;
  float i_avg_real = cap * (drain_pct_per_hour / 100.0f);   // mAh * (1/h) = mA
  // Solve I_sleep from I_avg = duty*I_awake + (1-duty)*I_sleep.
  float i_awake = (float)power_estimate_ma();
  if (duty >= 0.999f) return;                  // can't separate if always awake
  float i_sleep = (i_avg_real - duty * i_awake) / (1.0f - duty);
  if (i_sleep < 0.0f) i_sleep = 0.0f;          // model noise floor
  if (i_sleep > 50.0f) return;                 // implausible for deep sleep -> reject
  // Fold into the lifetime running average (incremental mean).
  s_calib.sleep_ma += (i_sleep - s_calib.sleep_ma) / (float)(s_calib.samples + 1);
  s_calib.samples++;
  calib_save();
  // Reset the duty window so the next sample is independent.
  calib_awake_ms = 0; calib_sleep_ms = 0;
}

/* Calibrated blended AVERAGE current (mA): the learned sleep floor plus the awake
 * model weighted by the current measured duty. Falls back to the awake model if
 * we haven't learned a floor yet. This is what runtime should use. */
static float power_avg_ma(void) {
  float i_awake = (float)power_estimate_ma();
  if (!calib_is_learned()) return i_awake;     // no floor learned -> best guess
  float duty = calib_duty();
  if (duty < 0.0f) duty = 0.0f;                // assume mostly-asleep if unknown
  return duty * i_awake + (1.0f - duty) * s_calib.sleep_ma;
}

/* Capacity-aware hours-left: remaining_mAh / calibrated average current. Uses the
 * learned pack capacity and the CALIBRATED average draw (sleep floor + awake
 * duty), so once calibration converges this reflects real week-scale runtime —
 * not just the awake-instant model. */
static float runtime_hours_left_capacity(int pct) {
  if (pct < 0) return 0.0f;
  float cap = (s_health.avg_mah > 0.0f) ? s_health.avg_mah : (float)BATT_DESIGN_MAH;
  float remaining_mah = cap * (float)pct / 100.0f;
  float ma = power_avg_ma();
  if (ma <= 0.0f) return 0.0f;
  return remaining_mah / ma;
}
