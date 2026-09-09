/* ============================================================================
 *  hr_algo.h — PPG beat detector -> heart rate + heart-rate variability.
 *
 *  Sensor-agnostic: feed raw PPG samples at a known rate (hr_algo_begin(rate)),
 *  one hr_algo_push() per sample, read the running results any time.
 *
 *  Pipeline (all integer/float scalar, no buffers beyond a few taps):
 *    1. DC removal: subtract a slow exponential average (~1.5 s).
 *    2. Smoothing: a short moving average (~50 ms) knocks down LED/ADC noise
 *       without moving the systolic peak much.
 *    3. Beat = the signal crosses an adaptive threshold (a fraction of the
 *       tracked peak amplitude) upward, then peaks. The peak time is refined
 *       by parabolic interpolation of the three samples around the maximum,
 *       which is what makes the RR intervals usable for HRV at 200 Hz.
 *    4. Refractory 300 ms (200 bpm cap). RR intervals outside 300..2000 ms or
 *       more than 30 % away from the running median are rejected as artefacts
 *       (they still count toward the quality figure).
 *
 *  Outputs:
 *    bpm      60000 / median of the last 8 accepted RR intervals
 *    RMSSD    root mean square of successive RR differences (ms) — the usual
 *             short-term HRV number
 *    SDNN     standard deviation of the accepted RR intervals (ms)
 *    quality  accepted beats / all detected beats, 0..100
 * ========================================================================== */
#pragma once
#include <math.h>
#include <string.h>

#define HRA_MAX_RR 128

/* ---- SENSITIVITY KNOBS (tune here) ----------------------------------------
 * The PPG pulse on a wrist is small (tens of ADC counts on a ~1500 baseline),
 * so the detector must be sensitive without chasing noise. Lower THR_FRAC and
 * shorter DECAY_S = more sensitive (finds smaller/earlier beats); GLITCH is the
 * |raw-DC| above which a sample is treated as motion/ADC artifact and kept out
 * of the beat logic entirely (a single spike used to blind detection for ~2 s).
 */
#ifndef HR_ALGO_THR_FRAC
#define HR_ALGO_THR_FRAC  0.30f   /* beat threshold = this * tracked peak (was 0.45) */
#endif
#ifndef HR_ALGO_DECAY_S
#define HR_ALGO_DECAY_S   1.1f    /* peak-envelope decay time, seconds (was 2.0)     */
#endif
#ifndef HR_ALGO_GLITCH
#define HR_ALGO_GLITCH    300.0f  /* |raw - DC| over this = artifact, skip the sample */
#endif
#ifndef HR_ALGO_DC_LOCK_S
#define HR_ALGO_DC_LOCK_S 0.35f   /* fast DC settle for the first ~0.7 s               */
#endif
#ifndef HR_ALGO_DC_S
#define HR_ALGO_DC_S      0.40f   /* steady high-pass time constant (~0.4 Hz corner).
                                     Was 1.5 s, which let wrist-pressure baseline wander
                                     (100s of counts over seconds) leak through and swamp
                                     the ~10-count pulse. Applied in TWO cascaded stages
                                     so a sloping baseline is cancelled, not just a level. */
#endif
#ifndef HR_ALGO_LP_S
#define HR_ALGO_LP_S      0.015f  /* extra low-pass after the moving average (s). Kept
                                     short: at 0.030 it shaved a filtered pulse that is
                                     only a few counts tall down toward the floor.      */
#endif
#ifndef HR_ALGO_THR_MIN
#define HR_ALGO_THR_MIN   0.30f   /* absolute floor of the beat threshold, filtered counts.
                                     Was a hard-coded 1.0: on a normally-worn wrist the
                                     filtered pulse peaks at only 1-4, so every beat below
                                     1.0 vanished. Missing alternate beats halves the bpm. */
#endif
#ifndef HR_ALGO_REJ_RESET
#define HR_ALGO_REJ_RESET 4       /* consecutive RR rejections that clear the running median.
                                     If the median ever seeds on missed-beat (2x) intervals,
                                     every true beat afterwards is "30% off" and rejected,
                                     locking the bpm at half the real rate. Re-seed instead. */
#endif
#ifndef HR_ALGO_HALF_HITS
#define HR_ALGO_HALF_HITS 3       /* rejected intervals near HALF the median = real beats at
                                     double the locked rate. This many and the median is
                                     re-seeded at once, instead of waiting for REJ_RESET
                                     consecutive rejections (a noisy start took 27 s).    */
#endif
#ifndef HR_ALGO_ENV_RISE
#define HR_ALGO_ENV_RISE  3.0f    /* max envelope growth per sample (x). A motion spike used
                                     to set the envelope to its full height in one sample,
                                     lifting the threshold above the pulse for the next ~1 s.
                                     Real pulse amplitude changes gradually.               */
#endif
#ifndef HR_ALGO_FINAL_S
#define HR_ALGO_FINAL_S   28.0f   /* the SAVED result (bpm, RMSSD, SDNN, quality) uses only
                                     the beats from the last this-many seconds. The first
                                     part of a measurement is where the baseline settles
                                     and the detector may still be locked at half rate;
                                     the end is where it has converged. Needs >= 6
                                     intervals in the window, else the whole run is used. */
#endif
#ifndef HR_ALGO_SETTLE_S
#define HR_ALGO_SETTLE_S  1.5f    /* ignore beats this long after start: the sensor's own
                                     auto-exposure warm-up is a huge transient that used
                                     to seed the envelope and hide the real pulse.       */
#endif

typedef struct {
  int    rate;                 // samples per second
  float  dc, dc_a;             // high-pass stage 1 (DC tracker) + its coefficient
  float  dc2;                  // high-pass stage 2: cancels the slope residual of stage 1
  float  lp, lp_a;             // extra low-pass after the moving average
  float  ma[8]; int ma_n, ma_i; float ma_sum;
  float  peak_env;             // tracked peak amplitude (decays)
  float  y_prev, y_prev2;      // last two filtered samples
  bool   armed;                // above threshold, waiting for the maximum
  float  best, best_t;         // maximum since arming (value, sample time)
  double t;                    // sample clock (seconds)
  double last_beat_t;
  int    beats_all, beats_ok;
  float  rr[HRA_MAX_RR]; float rr_t[HRA_MAX_RR]; int rr_n;   // accepted intervals + their beat time
  float  bt[HRA_MAX_RR]; uint8_t bok[HRA_MAX_RR]; int bt_n;   // every detected beat's time + accepted?
  float  rr_last8[8]; int rr8_n;
  bool   touch;
  int    n_samples;             // total samples seen (for the fast initial DC lock)
  int    rej_run;               // consecutive RR rejections (harmonic lock-in guard)
  int    half_hits;             // rejected intervals near half the median (half-rate lock signature)
  float  dc_a_fast;             // fast DC coefficient used during the initial lock
  int32_t dbg_min, dbg_max; int dbg_n;          // per-second raw-signal diagnostics
} hr_algo_t;

static hr_algo_t s_hra;

static inline void hr_algo_begin(int rate_hz) {
  memset(&s_hra, 0, sizeof s_hra);
  s_hra.rate = rate_hz > 0 ? rate_hz : 100;
  s_hra.dc_a = 1.0f / (HR_ALGO_DC_S * (float)s_hra.rate);   // steady high-pass corner
  s_hra.dc_a_fast = 1.0f / (HR_ALGO_DC_LOCK_S * (float)s_hra.rate); // fast initial lock
  s_hra.lp_a = 1.0f / (HR_ALGO_LP_S * (float)s_hra.rate);
  s_hra.ma_n = s_hra.rate / 20; if (s_hra.ma_n < 1) s_hra.ma_n = 1; if (s_hra.ma_n > 8) s_hra.ma_n = 8;
  s_hra.peak_env = 0;
}

static inline float hra_median8(const float *v, int n) {
  float t[8]; memcpy(t, v, n * sizeof(float));
  for (int i = 1; i < n; i++) { float x = t[i]; int j = i - 1; while (j >= 0 && t[j] > x) { t[j + 1] = t[j]; j--; } t[j + 1] = x; }
  return (n & 1) ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

static inline void hra_note_beat(float t, bool accepted) {
  if (s_hra.bt_n < HRA_MAX_RR) { s_hra.bt[s_hra.bt_n] = t; s_hra.bok[s_hra.bt_n] = accepted ? 1 : 0; s_hra.bt_n++; }
}
static inline void hra_accept_rr(float rr_ms) {
  if (s_hra.rr_n < HRA_MAX_RR) { s_hra.rr_t[s_hra.rr_n] = (float)s_hra.t; s_hra.rr[s_hra.rr_n++] = rr_ms; }
  if (s_hra.rr8_n < 8) s_hra.rr_last8[s_hra.rr8_n++] = rr_ms;
  else { memmove(s_hra.rr_last8, s_hra.rr_last8 + 1, 7 * sizeof(float)); s_hra.rr_last8[7] = rr_ms; }
}

static inline void hr_algo_push(int32_t raw, bool touch) {
  hr_algo_t *a = &s_hra;
  a->touch = touch;
  a->t += 1.0 / a->rate;
  float x = (float)raw;
  if (a->dbg_n == 0 || raw < a->dbg_min) a->dbg_min = raw;
  if (a->dbg_n == 0 || raw > a->dbg_max) a->dbg_max = raw;
  a->dbg_n++;
  if (a->dc == 0) a->dc = x;
  a->n_samples++;

  // GLITCH GUARD: a one-sample excursion far larger than any real pulse is
  // motion or an ADC artifact (e.g. the 13302 spike seen on hardware). Let it
  // nudge the DC tracker so we re-centre on a genuine baseline shift, but keep
  // it OUT of the moving average, the amplitude envelope and the beat logic,
  // and disarm — one spike used to hold the threshold high for ~2 s and kill
  // detection. This is the main sensitivity fix.
  float dev = x - a->dc;
  if (fabsf(dev) > HR_ALGO_GLITCH) {
    a->dc += dev * a->dc_a;
    a->armed = false;
    a->y_prev2 = a->y_prev; a->y_prev = 0;
    return;
  }

  // 2nd-ORDER HIGH-PASS: two cascaded DC trackers. One stage leaves a residual
  // proportional to the baseline SLOPE (wrist pressure wanders the baseline by
  // hundreds of counts over seconds, and that residual swamped the ~10-count
  // pulse). The second stage removes the slope residual as well. Both settle
  // fast for the first ~0.7 s so a measurement locks in seconds.
  float a_dc = a->n_samples < (int)(2.0f * HR_ALGO_DC_LOCK_S * a->rate) ? a->dc_a_fast : a->dc_a;
  a->dc += dev * a_dc;
  float ac1 = x - a->dc;
  a->dc2 += (ac1 - a->dc2) * a_dc;
  float ac = ac1 - a->dc2;
  // 2nd-ORDER LOW-PASS: 40 ms moving average, then a ~30 ms exponential stage,
  // so LED/ADC noise cannot masquerade as a peak.
  a->ma_sum += ac - a->ma[a->ma_i]; a->ma[a->ma_i] = ac; a->ma_i = (a->ma_i + 1) % a->ma_n;
  float ym = a->ma_sum / a->ma_n;
  a->lp += (ym - a->lp) * a->lp_a;
  float y = a->lp;

  // SETTLE GATE: keep the filters running but detect nothing for the first
  // HR_ALGO_SETTLE_S. The sensor's own warm-up transient is far bigger than
  // any pulse and used to seed the envelope with a threshold nothing could cross.
  if (a->n_samples < (int)(HR_ALGO_SETTLE_S * a->rate)) {
    a->y_prev2 = a->y_prev; a->y_prev = y;
    return;
  }

  // amplitude envelope: rise fast on a new peak, decay over HR_ALGO_DECAY_S
  if (y > a->peak_env) {
    float cap = a->peak_env * HR_ALGO_ENV_RISE;
    a->peak_env = (a->peak_env > HR_ALGO_THR_MIN && y > cap) ? cap : y;   // blunt one-sample spikes
  } else a->peak_env -= a->peak_env * (1.0f / (HR_ALGO_DECAY_S * a->rate));
  float thr = HR_ALGO_THR_FRAC * a->peak_env;
  if (thr < HR_ALGO_THR_MIN) thr = HR_ALGO_THR_MIN;

  if (!a->armed) {
    if (y > thr && a->y_prev <= thr && (a->t - a->last_beat_t) > 0.30) { a->armed = true; a->best = y; a->best_t = (float)a->t; a->y_prev2 = a->y_prev; }
  } else {
    if (y > a->best) { a->best = y; a->best_t = (float)a->t; }
    if (y < thr) {                                          // fell back: the maximum is the beat
      // parabolic refinement around the maximum (uses the two neighbours we kept)
      float t_peak = a->best_t;
      float l = a->y_prev2, c = a->best, r = y;
      float den = l - 2 * c + r;
      if (den < 0) { float d = 0.5f * (l - r) / den; if (d > -1 && d < 1) t_peak += d / a->rate; }
      a->armed = false;
      a->beats_all++;
      int ok_before = a->beats_ok;
      if (a->last_beat_t > 0) {
        float rr = (float)((t_peak - a->last_beat_t) * 1000.0);
        bool ok = rr >= 300 && rr <= 2000;
        float m = 0;
        if (ok && a->rr8_n >= 3) { m = hra_median8(a->rr_last8, a->rr8_n); if (fabsf(rr - m) > 0.30f * m) ok = false; }
        if (ok) { hra_accept_rr(rr); a->beats_ok++; a->rej_run = 0; a->half_hits = 0; }   // touch flag is informational only
        else if (rr >= 300 && rr <= 2000) {
          // A rejected interval near HALF the median is a real beat at twice the
          // locked rate: the median was seeded on missed-beat intervals.
          if (m > 0 && fabsf(rr - 0.5f * m) < 0.30f * 0.5f * m) a->half_hits++;
          if (++a->rej_run >= HR_ALGO_REJ_RESET || a->half_hits >= HR_ALGO_HALF_HITS) {
            // The median is the thing that is wrong. Drop it and let the current
            // beat stream re-seed it.
            a->rr8_n = 0; a->rej_run = 0; a->half_hits = 0;
            hra_accept_rr(rr); a->beats_ok++;
          }
        }
      }
      hra_note_beat(t_peak, a->beats_ok != ok_before);
      a->last_beat_t = t_peak;
    }
  }
  a->y_prev2 = a->y_prev;
  a->y_prev = y;
}

/* First accepted-interval index inside the final window (0 = use everything). */
static inline int hra_final_start(void) {
  float t0 = (float)s_hra.t - HR_ALGO_FINAL_S;
  int i = 0;
  while (i < s_hra.rr_n && s_hra.rr_t[i] < t0) i++;
  return (s_hra.rr_n - i >= 6) ? i : 0;
}

/* Current heart rate, 0 until enough beats. */
static inline int hr_algo_bpm(void) {
  if (s_hra.rr8_n < 3) return 0;
  float m = hra_median8(s_hra.rr_last8, s_hra.rr8_n);
  return m > 0 ? (int)(60000.0f / m + 0.5f) : 0;
}
static inline int hr_algo_beats(void) { return s_hra.beats_ok; }
/* Quality over the final window: accepted / detected beats in the last HR_ALGO_FINAL_S. */
static inline int hr_algo_quality(void) {
  float t0 = (float)s_hra.t - HR_ALGO_FINAL_S;
  int all = 0, ok = 0;
  for (int i = 0; i < s_hra.bt_n; i++) if (s_hra.bt[i] >= t0) { all++; ok += s_hra.bok[i]; }
  if (all < 6) { all = s_hra.beats_all; ok = s_hra.beats_ok; }
  return all ? (100 * ok) / all : 0;
}
static inline bool hr_algo_touch(void) { return s_hra.touch; }

/* RMSSD in ms over all accepted intervals (0 until >= 6). */
static inline int hr_algo_rmssd(void) {
  if (s_hra.rr_n < 6) return 0;
  int i0 = hra_final_start();
  double s = 0; int n = 0;
  for (int i = i0 + 1; i < s_hra.rr_n; i++) { double d = s_hra.rr[i] - s_hra.rr[i - 1]; s += d * d; n++; }
  return n ? (int)(sqrt(s / n) + 0.5) : 0;
}
/* SDNN in ms (0 until >= 6). */
static inline int hr_algo_sdnn(void) {
  if (s_hra.rr_n < 6) return 0;
  int i0 = hra_final_start(), n = s_hra.rr_n - i0;
  double m = 0; for (int i = i0; i < s_hra.rr_n; i++) m += s_hra.rr[i]; m /= n;
  double v = 0; for (int i = i0; i < s_hra.rr_n; i++) { double d = s_hra.rr[i] - m; v += d * d; }
  return (int)(sqrt(v / n) + 0.5);
}
/* Mean bpm of the FINAL window (0 until >= 6 intervals). */
static inline int hr_algo_bpm_mean(void) {
  if (s_hra.rr_n < 6) return 0;
  int i0 = hra_final_start(), n = s_hra.rr_n - i0;
  double m = 0; for (int i = i0; i < s_hra.rr_n; i++) m += s_hra.rr[i]; m /= n;
  return (int)(60000.0 / m + 0.5);
}
