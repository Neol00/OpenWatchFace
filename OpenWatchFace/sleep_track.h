/* ============================================================================
 *  sleep_track.h — overnight sleep-quality tracking session.
 *
 *  Owns the "Sleep mode" session that the Sleep app (app_sleep.h) toggles on/off.
 *  While a session is active:
 *    - the accel is powered on via the SLEEP path — imu_sleep_start()/stop() (its OWN owner,
 *      s_sleep_running), NOT imu_steps_start(). Sleep tracking and the step counter share ONLY
 *      the physical QMI8658 + the single ULP/LP core; they have entirely separate start/stop,
 *      arm, collect, flags, blobs, and RTC offsets. They are MUTUALLY EXCLUSIVE (one chip):
 *      the Sleep app refuses to start if step counting is running, and Fitness refuses while a
 *      sleep session is active.
 *    - the accel is handed to a DEDICATED sleep-movement program — S3 RISC-V ULP
 *      (ulp_sleep_blob.h) or C6 LP core (ulp_sleep_blob_c6.h) — that samples CONTINUOUSLY
 *      through the deep-sleep span and accumulates per-epoch movement metrics into RTC memory,
 *      NOT the step program (different thresholds, never touches the step count).
 *      arm_wakes_and_sleep() arms it (imu_sleep_ulp_arm); on each periodic wake the main CPU
 *      folds the metrics out of RTC (imu_sleep_ulp_collect) and sleep_track_log_wake() reads
 *      them (imu_sleep_*) + appends one row to the FFat STAGING file. A later pass (the actual
 *      sleep-quality scoring) reads it back; for now we collect the timeline.
 *
 *  STORAGE — staging vs canonical:
 *    Periodic sleep/background wakes run WITHOUT the display+SD bus up (sd_mount() needs
 *    s_gfx_ready, set only on a full boot), so the SD card is NOT mountable on those wakes.
 *    Each live row is therefore appended to a fixed FFat staging file (/sleep_stage.csv),
 *    which is always writable regardless of the bus. When the session STOPS (display awake
 *    -> SD mounted), sleep_track_flush_staging() merges the staged rows onto the canonical
 *    /sleep.csv on store_fs() (SD if present) and deletes the staging file. The whole UI
 *    reads ONLY the canonical file. (Earlier this logged straight to store_fs(), so the
 *    night landed on FFat while the UI later read SD -> "no sleep data written".)
 *
 *  The per-epoch metrics are: accum (summed gravity-removed deviation = intensity), peak
 *  (max single-sample deviation), events (# samples over a low restlessness threshold), and
 *  sample_count. High accum/events = restless; near-zero = still/asleep. The threshold is
 *  MUCH lower than the step thresholds (a sleep toss is far weaker than a footstep).
 *
 *  C6 has no sleep ULP yet, so there sleep_track_log_wake() falls back to a short HP-side
 *  accel burst (imu_read_accel_dev — decoupled, does not touch the step count) producing the
 *  same metric shape from just the wake window. Cross-board CSV rows mean the same thing.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER:
 *    - settings_store.h   (s_sleep_mode + settings_set_sleep_mode)
 *    - imu_steps.h        (imu_steps_* — the shared IMU/ULP path)
 *    - storage_fs.h       (store_fs / store_available)
 *    - the .ino's rtc_now_epoch() + i2c_lock()/i2c_unlock() forward decls.
 * ========================================================================== */
#pragma once

/* The shared-bus lock + wall-clock helpers live in the .ino; declared there. */
static inline void i2c_lock(void);
static inline void i2c_unlock(void);

/* CANONICAL history (read by all the UI) lives on store_fs() = SD-if-mounted-else-FFat.
 * But periodic sleep/background wakes run WITHOUT the display+SD bus up (sd_mount() needs
 * s_gfx_ready), so the SD card is NOT mountable on those wakes. Writing the live rows to
 * store_fs() there would silently land them on FFat while the UI later reads SD -> the
 * night's data is stranded ("no sleep data written"). So the LIVE log goes to a fixed
 * FFat STAGING file (FFat is always available on every wake, bus or not); when the
 * session STOPS (display awake -> SD mounted), we MERGE the staged rows into the canonical
 * store_fs() /sleep.csv and clear the staging file. See sleep_track_flush_staging(). */
#define SLEEP_CSV_PATH       "/sleep.csv"        // canonical, on store_fs() (SD if present)
#define SLEEP_STAGE_PATH     "/sleep_stage.csv"  // live append target, ALWAYS on FFat
#define SLEEP_BURST_SAMPLES  12     // HP-fallback accel reads per wake (~12 * 20ms = ~0.24s)
#define SLEEP_BURST_GAP_MS   20     // spacing between fallback burst samples
#define SLEEP_BURST_EVENT_TH 300    // HP-fallback "restlessness" threshold (matches the ULP's)

#define SLEEP_CSV_HEADER \
  "# epoch,accum,peak,events,samples   " \
  "(accum=summed accel deviation; peak=max; events=restless samples)"

/* Append one movement row for THIS wake/epoch to the STAGING file on FFat. The metrics
 * mirror the ULP's per-epoch accumulators so an S3 (ULP-fed) and a C6 (HP-burst) row mean
 * the same thing:
 *   epoch  = wall-clock seconds at the wake
 *   accum  = summed gravity-removed accel deviation over the epoch (movement intensity)
 *   peak   = largest single-sample deviation in the epoch
 *   events = # samples above the restlessness threshold
 *   samples= # samples that contributed (normalizer / liveness)
 * Goes to FFat (not store_fs()) because periodic wakes have no SD bus — see the staging
 * note above. Header is NOT written here (the staging file holds raw rows only; the
 * header is on the canonical file). Best-effort: no-ops if FFat won't mount. */
static void sleep_csv_append(uint32_t epoch, uint32_t accum, uint32_t peak,
                             uint32_t events, uint32_t samples) {
  if (!ffat_mount()) { USBSerial.println("[sleep] stage append: no FFat"); return; }
  File f = FFat.open(SLEEP_STAGE_PATH, FILE_APPEND);
  if (!f) { USBSerial.println("[sleep] stage append failed"); return; }
  f.printf("%lu,%lu,%lu,%lu,%lu\n", (unsigned long)epoch, (unsigned long)accum,
           (unsigned long)peak, (unsigned long)events, (unsigned long)samples);
  f.close();
}

/* Merge any staged rows (FFat /sleep_stage.csv) onto the canonical /sleep.csv on store_fs()
 * (SD if mounted, else FFat), then delete the staging file. Called when a sleep session
 * STOPS — by then the display is awake so the SD card is mounted and store_fs() is the SD.
 * The canonical file is append-only oldest..newest and the staging rows are likewise in
 * chronological order, so a plain append preserves global ordering (the just-finished
 * night is the newest). Creates the canonical header on first write. No-op if nothing is
 * staged. Best-effort: if store_fs() can't be written, the staging file is LEFT IN PLACE
 * so a later stop can retry (data is never dropped). */
static void sleep_track_flush_staging(void) {
  if (!ffat_mount() || !FFat.exists(SLEEP_STAGE_PATH)) return;   // nothing staged

  File src = FFat.open(SLEEP_STAGE_PATH, FILE_READ);
  if (!src) return;
  if (src.size() == 0) { src.close(); FFat.remove(SLEEP_STAGE_PATH); return; }

  if (!store_available()) { src.close(); return; }   // keep staging; retry on a later stop
  bool fresh = !store_fs().exists(SLEEP_CSV_PATH);
  File dst = store_fs().open(SLEEP_CSV_PATH, FILE_APPEND);
  if (!dst) { src.close(); USBSerial.println("[sleep] flush: canonical open failed"); return; }
  if (fresh) dst.println(SLEEP_CSV_HEADER);

  // Stream FFat -> store_fs() (SD) in 2 KB chunks. The buffer is STATIC, not on the stack:
  // sleep_track_stop() runs on the LVGL/loop task, and on the C6 the SD write goes through a
  // deep IDF call chain (VFS -> FATFS f_write -> sdspi -> spi_master) that is itself very
  // stack-hungry — a large buffer ON THE STACK on top of that overflowed it and crashed on stop.
  // Static keeps the stack footprint at zero; 2 KB keeps the SD write count (and each write's
  // SD SPI transaction) low so the copy stays fast. If a write short-counts
  // (SD full/error) we STOP and LEAVE the staging file so a later stop can retry — never delete
  // it after a partial copy, or the unwritten rows would be lost.
  static uint8_t buf[2048];
  size_t  n, total = 0;
  bool    ok = true;
  while ((n = src.read(buf, sizeof(buf))) > 0) {
    size_t w = dst.write(buf, n);
    if (w != n) { ok = false; USBSerial.println("[sleep] flush: short write -> keep staging"); break; }
    total += n;
  }
  dst.close();
  src.close();
  if (ok) {
    FFat.remove(SLEEP_STAGE_PATH);                               // staged rows now canonical
    USBSerial.printf("[sleep] flushed %u staged bytes -> %s\n",
                     (unsigned)total, SLEEP_CSV_PATH);
  }
}

/* ---- session start/stop ----------------------------------------------------
 * START: turn the IMU on (so it samples now AND gets handed to the ULP for deep
 * sleep, exactly like the step counter) and set the persisted s_sleep_mode flag.
 * Caller is responsible for the mutual-exclusion check against step counting (the
 * Sleep app does that before calling, so it can show a message); we assert it here
 * too as a safety net. Returns true if the session is now active. */
static bool sleep_track_start(void) {
  if (imu_steps_running()) {              // step counter owns the IMU -> refuse
    USBSerial.println("[sleep] refused: step counter is running");
    return false;
  }
  bool ok = true;
  if (imu_steps_available()) {
    i2c_lock();
    ok = imu_sleep_start();              // accel ON via the SLEEP path (sets s_sleep_running);
    i2c_unlock();                        // the sleep ULP/LP arm happens at deep-sleep entry
  }
  settings_set_sleep_mode(true);

  // Anchor the night's START time with a zero-movement marker row NOW. Logging otherwise only
  // happens on the periodic background wakes, so the first row would land one full check
  // interval (e.g. 10 min) AFTER you started — making the night's start_epoch late and the
  // computed duration short by ~one interval. samples=0 so it doesn't skew restful% (that
  // divides by total samples); it only fixes start_epoch / duration. rtc_now_epoch() is valid
  // here (started from the UI, clock is up). 0-epoch guard: skip if the clock isn't set.
  uint32_t now = (uint32_t)rtc_now_epoch();
  if (now) {
    sleep_csv_append(now, 0, 0, 0, 0);
    USBSerial.printf("[sleep] start marker row: epoch=%lu\n", (unsigned long)now);
  }

  USBSerial.printf("[sleep] session started (imu=%d)\n", (int)ok);
  return true;
}

/* STOP: clear the flag and power the IMU back off (lets normal full-power-down deep
 * sleep resume). Safe to call even if no session was active. */
static void sleep_track_stop(void) {
  settings_set_sleep_mode(false);
  if (imu_steps_available()) {
    i2c_lock();
    imu_sleep_stop();                    // accel OFF via the SLEEP path (clears s_sleep_running)
    i2c_unlock();
  }
  // The display is awake now (you stopped from the UI) so the SD card is mounted: move
  // the night's rows staged on FFat during the screen-less periodic wakes onto the
  // canonical /sleep.csv (SD if present). Without this the staged data never reaches the
  // file the Sleep app reads.
  sleep_track_flush_staging();
  USBSerial.println("[sleep] session stopped");
}

static inline bool sleep_track_active(void) { return s_sleep_mode; }

/* ============================================================================
 *  Read-back / analysis: group the flat /sleep.csv rows into NIGHTS for the UI.
 *
 *  The CSV is one row per periodic wake (epoch,accum,peak,events,samples). Rows from
 *  the same sleep session are spaced ~check-interval apart; a long gap (you stopped
 *  tracking, or a new night) ends a session. We stream the file (never a full load) and
 *  collapse consecutive rows into per-night summaries.
 *
 *  The history is UNBOUNDED (can be hundreds of nights). The UI wants NEWEST nights first
 *  but the file is appended OLDEST..newest, so a BACKWARD streaming cursor (sleep_rev_*)
 *  reads it from the END in blocks and emits nights newest-first — stopping as soon as the
 *  visible page (or trends window) is filled, so a 500-night file is never fully scanned.
 *  The cursor is RESUMABLE so the UI can pump it a few nights per LVGL tick (app_sleep.h)
 *  and stay responsive. Two views sit on top:
 *    - the data LIST: streams the newest SLEEP_PAGE nights, then peeks one more to know if
 *      a next page exists (no total-count scan needed);
 *    - the TRENDS graph: the newest N (=7/30) nights' restful% + their average.
 *  The per-night detail still uses a small FORWARD scan (sleep_night_for_each /
 *  sleep_night_summary) that touches only the one selected night.
 * ========================================================================== */
#define SLEEP_NIGHT_POINTS  48      // movement points kept per night for its detail graph
#define SLEEP_PAGE          100     // nights per list page (flip when history exceeds this)
#define SLEEP_TREND_MAX     30      // deepest trends window (the 30 in 1/7/30)
// A gap longer than this (seconds) between consecutive rows starts a NEW night. 90 min
// comfortably exceeds the longest check interval (20 min) so a normal session never
// splits, but a daytime stop/restart or the next night always does.
#define SLEEP_NIGHT_GAP_S   (90 * 60)

typedef struct {
  uint32_t start_epoch;     // first row's epoch in this night
  uint32_t end_epoch;       // last row's epoch
  uint32_t rows;            // # CSV rows (epochs) in the night
  uint32_t sum_events;      // summed restless-sample count
  uint32_t sum_samples;     // summed sample count (normalizer for restful %)
  uint32_t sum_accum;       // summed movement intensity
  uint32_t peak;            // max single-epoch peak
} sleep_night_t;

/* Trends: newest-N restful% (index 0 = oldest in the window .. n-1 = newest), so the
 * chart reads left=old -> right=recent. Plus the average over the window. */
static uint8_t  s_trend_pct[SLEEP_TREND_MAX];
static int      s_trend_n = 0;
static uint8_t  s_trend_avg = 0;

/* Downsampled movement timeline (per-epoch accum) for ONE night, used by the detail
 * graph. s_night_pts_n points, oldest..newest. Filled by sleep_night_load_points(). */
static uint16_t s_night_pts[SLEEP_NIGHT_POINTS];
static int      s_night_pts_n = 0;
static uint32_t s_night_pts_max = 1;   // max value among the points (for chart Y-range)

/* Tracked duration of a night in minutes (end-start, +1 epoch so a 1-row night isn't 0). */
static inline uint32_t sleep_night_minutes(const sleep_night_t *n) {
  if (!n || n->rows == 0) return 0;
  uint32_t span = (n->end_epoch > n->start_epoch) ? (n->end_epoch - n->start_epoch) : 0;
  return (span / 60) + 1;
}

/* Restful percentage: fraction of samples that were BELOW the restlessness threshold,
 * i.e. 100 - (restless samples / total samples). 0 samples -> 0%. */
static inline uint8_t sleep_night_restful_pct(const sleep_night_t *n) {
  if (!n || n->sum_samples == 0) return 0;
  uint32_t restless = (n->sum_events > n->sum_samples) ? n->sum_samples : n->sum_events;
  uint32_t pct = 100u - (restless * 100u) / n->sum_samples;
  return (uint8_t)pct;
}

/* Parse one CSV data line "epoch,accum,peak,events,samples" into fields. Returns false
 * for blank/comment/malformed lines. */
static bool sleep_parse_row(const char *line, uint32_t *epoch, uint32_t *accum,
                            uint32_t *peak, uint32_t *events, uint32_t *samples) {
  if (!line || line[0] == '#' || line[0] == '\0' || line[0] == '\n') return false;
  unsigned long e, a, p, ev, s;
  if (sscanf(line, "%lu,%lu,%lu,%lu,%lu", &e, &a, &p, &ev, &s) != 5) return false;
  if (e == 0) return false;
  *epoch = e; *accum = a; *peak = p; *events = ev; *samples = s;
  return true;
}

/* ============================================================================
 *  BACKWARD streaming reader — emit nights NEWEST-first without scanning the whole
 *  file. The CSV is append-only (oldest..newest), so to show the newest page we read
 *  it from the END in blocks, walk lines bottom-to-top, and assemble nights in reverse.
 *
 *  Why: the forward iterator above must read the entire file before it can know which
 *  rows are "newest", which blocked the UI for seconds on a big history. This cursor is
 *  RESUMABLE — sleep_rev_step() advances by up to `max_nights` nights per call and keeps
 *  the File open across calls — so an LVGL timer can pump it a few nights per tick and let
 *  the UI breathe in between. For page 0 we stop after SLEEP_PAGE nights, so a 500-night
 *  file never gets fully scanned just to show the newest 100.
 *
 *  Rows arrive newest-row-first within a night; the per-night SUMMARY is order-independent
 *  (sums + max), so that's fine. (The detail graph still uses the forward
 *  sleep_night_for_each, which only touches one night.)
 * ========================================================================== */
#define SLEEP_REV_BLOCK  512        // bytes read per backward block (one stack buffer)

typedef struct {
  File     f;
  uint32_t pos;          // file offset of the start of the unconsumed region (we read [.. ,pos))
  char     buf[SLEEP_REV_BLOCK + 1];
  int      blen;         // valid bytes currently in buf (the most recently read block)
  bool     have_cur;     // a night is being assembled
  bool     done;         // hit start of file AND flushed the final night
  sleep_night_t cur;     // night under assembly (rows added newest-first)
  uint32_t cur_oldest;   // smallest epoch seen in `cur` so far (its start row)
  bool     open;
} sleep_rev_cursor;

/* Begin a backward read. Returns false (and leaves cursor->open=false) if there's no file. */
static bool sleep_rev_begin(sleep_rev_cursor *c) {
  memset(c, 0, sizeof(*c));
  if (!store_available() || !store_fs().exists(SLEEP_CSV_PATH)) return false;
  c->f = store_fs().open(SLEEP_CSV_PATH, FILE_READ);
  if (!c->f) return false;
  c->pos  = (uint32_t)c->f.size();
  c->open = true;
  return true;
}

static void sleep_rev_end(sleep_rev_cursor *c) {
  if (c->open) { c->f.close(); c->open = false; }
}

/* Pull the next line (scanning the file from the end toward the start) into `out`.
 * Returns the length, or -1 when the start of the file is reached. Lines are returned
 * bottom-to-top. Handles lines that straddle a block boundary by carrying the leftover
 * head of a block back into the next (earlier) block read. */
static int sleep_rev_next_line(sleep_rev_cursor *c, char *out, int out_sz) {
  // `c->buf[0..c->blen)` holds bytes [c->pos, c->pos+c->blen) not yet emitted; we scan it
  // right-to-left for a '\n' that begins a line. When exhausted, read the earlier block.
  for (;;) {
    // Look for a newline within the current buffer (excluding a trailing one we already cut).
    for (int i = c->blen - 1; i >= 0; i--) {
      if (c->buf[i] == '\n') {
        int start = i + 1;
        int len   = c->blen - start;
        if (len > 0) {
          if (len > out_sz - 1) len = out_sz - 1;
          memcpy(out, c->buf + start, len);
          out[len] = '\0';
          c->blen = i;                 // keep everything up to (and incl.) this '\n' for later
          return len;
        }
        c->blen = i;                   // empty line (trailing newline) — drop it, keep scanning
        continue;
      }
    }
    // No newline left in buf. The remaining buf bytes are the HEAD of a line that continues
    // into the earlier block. Read the previous block and prepend the leftover.
    if (c->pos == 0) {
      // Start of file: whatever's left in buf is the first line (no leading '\n').
      if (c->blen > 0) {
        int len = c->blen;
        if (len > out_sz - 1) len = out_sz - 1;
        memcpy(out, c->buf, len);
        out[len] = '\0';
        c->blen = 0;
        return len;
      }
      return -1;                       // fully consumed
    }
    int leftover = c->blen;
    if (leftover > SLEEP_REV_BLOCK) leftover = SLEEP_REV_BLOCK;   // safety
    uint32_t want = (c->pos >= SLEEP_REV_BLOCK) ? SLEEP_REV_BLOCK : c->pos;
    char tmp[SLEEP_REV_BLOCK];
    c->f.seek(c->pos - want);
    int got = c->f.read((uint8_t *)tmp, want);
    if (got <= 0) { c->pos = 0; continue; }   // treat as EOF-ish; flush leftover next loop
    // Advance pos by exactly what we read (a short read leaves the rest for next time).
    c->pos -= (uint32_t)got;
    // New buffer = [tmp(0..got)] + [old leftover]. Both fit (got<=BLOCK, leftover<=BLOCK).
    if (got + leftover > SLEEP_REV_BLOCK) {
      // Shouldn't happen for our short rows, but clamp the leftover if a line exceeds a block.
      leftover = SLEEP_REV_BLOCK - got;
    }
    memmove(c->buf + got, c->buf, leftover);
    memcpy(c->buf, tmp, got);
    c->blen = got + leftover;
  }
}

/* Advance the backward read by up to `max_nights` completed nights, calling
 * cb(night, ud) for each — NEWEST first. Sets *done=true once the whole file is consumed.
 * Returns the number of nights emitted this call. */
typedef void (*sleep_rev_cb)(const sleep_night_t *n, void *ud);
static int sleep_rev_step(sleep_rev_cursor *c, int max_nights, sleep_rev_cb cb, void *ud) {
  if (!c->open || c->done) { if (c) c->done = true; return 0; }
  int emitted = 0;
  char line[96];

  while (emitted < max_nights) {
    int len = sleep_rev_next_line(c, line, sizeof(line));
    if (len < 0) {                              // reached start of file
      if (c->have_cur) { if (cb) cb(&c->cur, ud); emitted++; c->have_cur = false; }
      c->done = true;
      break;
    }
    uint32_t epoch, accum, peak, events, samples;
    if (!sleep_parse_row(line, &epoch, &accum, &peak, &events, &samples)) continue;

    if (c->have_cur && c->cur_oldest > epoch + SLEEP_NIGHT_GAP_S) {
      // Walking backward, this earlier row is separated by a gap -> the night we were
      // assembling is complete (it's the newer one). Emit it, then start a new night here.
      if (cb) cb(&c->cur, ud);
      emitted++;
      c->have_cur = false;
    }
    if (!c->have_cur) {
      c->cur = (sleep_night_t){};
      c->cur.end_epoch = epoch;                 // first row seen backward = the night's LAST row
      c->cur_oldest    = epoch;
      c->have_cur      = true;
    }
    // Add this (older) row into the current night.
    c->cur.start_epoch = epoch;                 // keeps moving earlier => final = true start
    if (epoch < c->cur_oldest) c->cur_oldest = epoch;
    c->cur.rows++;
    c->cur.sum_events  += events;
    c->cur.sum_samples += samples;
    c->cur.sum_accum   += accum;
    if (peak > c->cur.peak) c->cur.peak = peak;
  }
  return emitted;
}

/* ---- trends load: newest `want` nights' restful% (oldest..newest) + average ----
 * Uses the BACKWARD cursor so it only reads the tail it needs (the newest `want` nights),
 * never the whole file. The cursor yields newest-first, so we collect into a small temp and
 * reverse into s_trend_pct[] (index 0 = oldest in the window) for a left=old chart. */
typedef struct { uint8_t pct[SLEEP_TREND_MAX]; int n; int want; uint32_t sum; } sleep_trend_ctx;
static void sleep_trend_rev_cb(const sleep_night_t *n, void *ud) {
  sleep_trend_ctx *c = (sleep_trend_ctx *)ud;
  if (c->n >= c->want || c->n >= SLEEP_TREND_MAX) return;
  uint8_t pct = sleep_night_restful_pct(n);
  c->pct[c->n++] = pct;                                   // 0 = newest here (reversed below)
  c->sum += pct;
}
/* Fill s_trend_pct / s_trend_n / s_trend_avg for the newest `want` nights. */
static void sleep_trends_load(int want) {
  s_trend_n = 0; s_trend_avg = 0;
  if (want < 1) want = 1;
  if (want > SLEEP_TREND_MAX) want = SLEEP_TREND_MAX;

  sleep_rev_cursor cur;
  if (!sleep_rev_begin(&cur)) return;
  sleep_trend_ctx c = {}; c.want = want;
  while (!cur.done && c.n < want)
    sleep_rev_step(&cur, want - c.n, sleep_trend_rev_cb, &c);
  sleep_rev_end(&cur);

  // Reverse newest-first -> oldest..newest so the chart reads left=old, right=recent.
  for (int i = 0; i < c.n; i++) s_trend_pct[i] = c.pct[c.n - 1 - i];
  s_trend_n = c.n;
  if (c.n > 0) s_trend_avg = (uint8_t)(c.sum / c.n);
}

/* Stream the CSV and run `cb` for each row's MOVEMENT INTENSITY in the night that begins
 * exactly at `start_epoch`. Walks rows in time order: once it hits the row whose epoch ==
 * start_epoch it's "in" the night, and it stays in until a gap > SLEEP_NIGHT_GAP_S ends it.
 *
 * INTENSITY, not raw accum: a row's `accum` is the SUM of per-sample accel deviation over the
 * whole wake span, so it grows with the number of samples (~62 Hz * interval). Two equal-length
 * spans therefore have similar raw accum even if one was restless and one still — feeding raw
 * accum to the chart made every bar peg near the night's max (the "always 0 or 100" symptom).
 * Normalizing by the sample count (accum/samples = AVERAGE deviation per sample) removes the
 * span-length factor so the bars reflect how active each stretch actually was. samples==0 rows
 * (e.g. the start marker) pass 0. */
typedef void (*sleep_accum_cb)(uint32_t intensity, void *ud);
static void sleep_night_for_each(uint32_t start_epoch, sleep_accum_cb cb, void *ud) {
  if (!store_available() || !store_fs().exists(SLEEP_CSV_PATH)) return;
  File f = store_fs().open(SLEEP_CSV_PATH, FILE_READ);
  if (!f) return;
  bool in = false; uint32_t prev = 0;
  char line[96];
  while (f.available()) {
    int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (len <= 0) continue; line[len] = '\0';
    uint32_t e, a, p, ev, s;
    if (!sleep_parse_row(line, &e, &a, &p, &ev, &s)) continue;
    if (!in) {
      // Match the night's first row (which may be the samples==0 START MARKER). Enter the
      // night but DON'T emit a bar for a 0-sample row — it carries no movement, and a forced
      // empty first bar would skew the chart. prev is set so the gap test still works.
      if (e == start_epoch) {
        in = true; prev = e;
        if (s > 0) cb(a / s, ud);
      }
      continue;
    }
    if (e > prev + SLEEP_NIGHT_GAP_S) break;    // gap -> this night ended
    if (s > 0) cb(a / s, ud);                   // average per-sample deviation (skip 0-sample rows)
    prev = e;
  }
  f.close();
}

/* Assemble the SUMMARY of the one night that begins exactly at `start_epoch` into *out.
 * Streams forward from that row until a gap > SLEEP_NIGHT_GAP_S ends the night (same rule
 * as sleep_night_for_each). Returns true if the night was found. Used by the detail screen,
 * which now gets a night by epoch (the list no longer keeps a page array in RAM). */
static bool sleep_night_summary(uint32_t start_epoch, sleep_night_t *out) {
  if (!out || !store_available() || !store_fs().exists(SLEEP_CSV_PATH)) return false;
  File f = store_fs().open(SLEEP_CSV_PATH, FILE_READ);
  if (!f) return false;
  bool in = false; uint32_t prev = 0;
  *out = (sleep_night_t){};
  char line[96];
  while (f.available()) {
    int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (len <= 0) continue; line[len] = '\0';
    uint32_t e, a, p, ev, s;
    if (!sleep_parse_row(line, &e, &a, &p, &ev, &s)) continue;
    if (!in) {
      if (e != start_epoch) continue;           // not our night yet
      in = true; out->start_epoch = e; prev = e;
    } else if (e > prev + SLEEP_NIGHT_GAP_S) {
      break;                                     // gap -> night ended
    }
    out->end_epoch = e;
    out->rows++;
    out->sum_events  += ev;
    out->sum_samples += s;
    out->sum_accum   += a;
    if (p > out->peak) out->peak = p;
    prev = e;
  }
  f.close();
  return in;
}

/* Counting callback (first pass) + bucketing callback (second pass) for the detail graph. */
static void sleep_count_cb(uint32_t accum, void *ud) { (void)accum; (*(uint32_t *)ud)++; }
typedef struct { uint32_t bucket, bsum, bn; } sleep_bucket_t;
static void sleep_bucket_cb(uint32_t accum, void *ud) {
  sleep_bucket_t *b = (sleep_bucket_t *)ud;
  b->bsum += accum; b->bn++;
  if (b->bn >= b->bucket && s_night_pts_n < SLEEP_NIGHT_POINTS) {
    uint32_t avg = b->bsum / b->bn;
    if (avg > 32000u) avg = 32000u;   // clamp to lv_coord_t (int16) range for the chart
    s_night_pts[s_night_pts_n++] = (uint16_t)avg;
    if (avg > s_night_pts_max) s_night_pts_max = avg;
    b->bsum = 0; b->bn = 0;
  }
}

/* Load the downsampled movement timeline for the night starting at `start_epoch` into
 * s_night_pts[] (oldest..newest), bucket-averaging its per-epoch `accum` down to at most
 * SLEEP_NIGHT_POINTS points. Two streamed passes (count, then bucket) — no full load. */
static void sleep_night_load_points(uint32_t start_epoch) {
  s_night_pts_n = 0;
  s_night_pts_max = 1;
  uint32_t rows = 0;
  sleep_night_for_each(start_epoch, sleep_count_cb, &rows);
  if (rows == 0) return;
  sleep_bucket_t b = { (rows + SLEEP_NIGHT_POINTS - 1) / SLEEP_NIGHT_POINTS, 0, 0 };
  if (b.bucket < 1) b.bucket = 1;
  sleep_night_for_each(start_epoch, sleep_bucket_cb, &b);
  if (b.bn > 0 && s_night_pts_n < SLEEP_NIGHT_POINTS) {   // flush partial last bucket
    uint32_t avg = b.bsum / b.bn;
    if (avg > 32000u) avg = 32000u;   // clamp to lv_coord_t (int16) range for the chart
    s_night_pts[s_night_pts_n++] = (uint16_t)avg;
    if (avg > s_night_pts_max) s_night_pts_max = avg;
  }
}

/* ============================================================================
 *  TEST DATA — synthetic /sleep.csv generator (dev only, gated by SLEEP_TEST_DATA).
 *
 *  Flip SLEEP_TEST_DATA to 1 (top of the .ino), flash, and setup() OVERWRITES
 *  /sleep.csv with 30 varied nights so the data screens can be exercised immediately
 *  (without sleeping for real). Flip it back to 0 and flash to resume real logging.
 *  WARNING: this REPLACES the file — any real recorded data is wiped while it's on.
 * ========================================================================== */
#ifndef SLEEP_TEST_DATA
#define SLEEP_TEST_DATA 0
#endif
#if SLEEP_TEST_DATA
/* Tiny deterministic PRNG so the dataset is repeatable across boots (no <random>). */
static uint32_t slp_rng_state = 0x1234567u;
static uint32_t slp_rng(void) {
  slp_rng_state ^= slp_rng_state << 13;
  slp_rng_state ^= slp_rng_state >> 17;
  slp_rng_state ^= slp_rng_state << 5;
  return slp_rng_state;
}
static inline uint32_t slp_rng_range(uint32_t lo, uint32_t hi) {
  return lo + (slp_rng() % (hi - lo + 1));
}

/* Write synthetic nights ending on consecutive recent days. Each night is a run of
 * per-epoch rows spaced ~10 min apart (well under the 90-min gap, so each stays one
 * night), with a quality target sweeping low->high across nights so restful%, the
 * color bands, durations, the graph shape, the trends windows AND list paging all get
 * exercised. NIGHTS > SLEEP_PAGE (100) on purpose so the list's page flip is testable.
 * Movement is calm stretches punctuated by restless bursts. */
static void sleep_test_seed(void) {
  if (!store_available()) { USBSerial.println("[sleep] test seed: no store"); return; }
  slp_rng_state = 0x1234567u;                       // reset for a repeatable dataset
  File f = store_fs().open(SLEEP_CSV_PATH, FILE_WRITE);   // truncate + rewrite
  if (!f) { USBSerial.println("[sleep] test seed: open failed"); return; }
  f.println("# epoch,accum,peak,events,samples   "
            "(accum=summed accel deviation; peak=max; events=restless samples)  [TEST DATA]");

  const uint32_t EPOCH_GAP   = 10 * 60;             // 10 min between rows (epochs)
  const uint32_t SAMPLES_ROW = 60UL * 1000 / 16;    // ~ULP samples in 10 min @ ~62Hz (~37500)
  uint32_t now = (uint32_t)rtc_now_epoch();
  if (now < 1700000000u) now = 1700000000u;         // fallback if clock not set yet
  const int NIGHTS = 130;                           // > SLEEP_PAGE so paging is exercised

  for (int d = NIGHTS - 1; d >= 0; d--) {            // oldest day first (file is time-ordered)
    // Night d days ago: wake near ~07:00, length 5.5-9h -> rows.
    uint32_t wake   = now - (uint32_t)d * 86400u;
    uint32_t hours  = slp_rng_range(5, 9);
    uint32_t rows   = (hours * 3600u) / EPOCH_GAP;   // ~33-54 rows
    uint32_t start  = wake - rows * EPOCH_GAP;
    // Quality target 0..100 sweeping across the 30 nights (so we span red/amber/green),
    // jittered a bit so it isn't a perfect ramp.
    int q = (int)((NIGHTS - 1 - d) * 100 / (NIGHTS - 1));   // 0..100 newest=high
    q += (int)slp_rng_range(0, 20) - 10;
    if (q < 0) q = 0; if (q > 100) q = 100;

    for (uint32_t r = 0; r < rows; r++) {
      uint32_t epoch = start + r * EPOCH_GAP;
      // restless fraction of samples driven by (100-q): low quality -> more events.
      uint32_t restless_pct = (uint32_t)(100 - q);
      // add occasional bursts regardless of quality (a few restless epochs every night).
      if (slp_rng_range(0, 9) == 0) restless_pct += slp_rng_range(10, 40);
      if (restless_pct > 100) restless_pct = 100;
      uint32_t events  = (SAMPLES_ROW * restless_pct) / 100;
      uint32_t accum   = events * slp_rng_range(350, 900) + slp_rng_range(0, 4000);
      uint32_t peak    = slp_rng_range(300, 6000) + (restless_pct * 30);
      f.printf("%lu,%lu,%lu,%lu,%lu\n", (unsigned long)epoch, (unsigned long)accum,
               (unsigned long)peak, (unsigned long)events, (unsigned long)SAMPLES_ROW);
    }
  }
  f.close();
  USBSerial.printf("[sleep] test seed: wrote %d synthetic nights to %s\n",
                   NIGHTS, SLEEP_CSV_PATH);
}
#endif  /* SLEEP_TEST_DATA */

/* Called on each periodic background-check wake (from background_check_has_new) when a
 * session is active. Logs one CSV row of movement metrics for the epoch that just ended.
 *
 *   - S3 (sleep ULP): the ULP sampled the accel CONTINUOUSLY through the whole sleep span
 *     and accumulated the metrics into RTC mem; imu_steps_ulp_collect() (run early in setup,
 *     before this) already folded + zeroed them, so we just read imu_sleep_*() and log. This
 *     is the accurate, full-coverage path — far better than the old quarter-second peek.
 *   - C6 / no-ULP fallback: there's no sleep ULP yet, so take a short HP-side accel burst
 *     here (decoupled imu_read_accel_dev — does NOT touch the step count) and derive the same
 *     metrics from it. Only sees this wake window, but keeps the feature working cross-board.
 *
 * Caller does NOT hold i2c_lock.
 *
 * IMPORTANT: this runs on the SCREEN-LESS background/timer wake, where the full-boot
 * imu_steps_begin() has NOT run yet — so s_imu_ok (imu_steps_available()) is FALSE here.
 * We must NOT gate the whole function on it: the ULP branch reads RTC-folded metrics that
 * imu_steps_ulp_collect() already snapshotted (no live chip needed), and only the HP-burst
 * fallback actually touches the live IMU. Gating on imu_steps_available() up front is what
 * silently dropped every night's row (nothing was ever written — FFat empty). */
static void sleep_track_log_wake(uint32_t epoch) {
  if (!s_sleep_mode) return;

  // Prefer the ULP-folded metrics (samples > 0 means the sleep ULP actually ran this span).
  // These came from RTC via imu_steps_ulp_collect() — valid even though the live IMU driver
  // isn't initialized on this wake path.
  if (imu_sleep_samples() > 0) {
    sleep_csv_append(epoch, imu_sleep_accum(), imu_sleep_peak(),
                     imu_sleep_events(), imu_sleep_samples());
    USBSerial.printf("[sleep] wake logged (ulp): epoch=%lu accum=%lu peak=%lu events=%lu n=%lu\n",
                     (unsigned long)epoch, (unsigned long)imu_sleep_accum(),
                     (unsigned long)imu_sleep_peak(), (unsigned long)imu_sleep_events(),
                     (unsigned long)imu_sleep_samples());
    return;
  }

  // Fallback HP burst (C6 / no sleep ULP): derive accum/peak/events from a short window.
  if (!imu_steps_available()) return;
  uint32_t accum = 0, peak = 0, events = 0, got = 0;
  for (int i = 0; i < SLEEP_BURST_SAMPLES; i++) {
    i2c_lock();
    int dev = imu_read_accel_dev();      // decoupled deviation — does NOT bump the step count
    i2c_unlock();
    if (dev >= 0) {
      accum += (uint32_t)dev;
      if ((uint32_t)dev > peak) peak = (uint32_t)dev;
      if (dev > SLEEP_BURST_EVENT_TH) events++;
      got++;
    }
    delay(SLEEP_BURST_GAP_MS);
  }
  sleep_csv_append(epoch, accum, peak, events, got);
  USBSerial.printf("[sleep] wake logged (burst): epoch=%lu accum=%lu peak=%lu events=%lu n=%lu\n",
                   (unsigned long)epoch, (unsigned long)accum, (unsigned long)peak,
                   (unsigned long)events, (unsigned long)got);
}
