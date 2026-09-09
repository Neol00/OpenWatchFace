/* ============================================================================
 *  hr_store.h — heart-rate measurement history on /hr.csv (store_fs()).
 *
 *  One row per finished measurement, oldest first:
 *      epoch,bpm,rmssd_ms,sdnn_ms,quality_pct
 *  The whole file is small (a row is ~30 bytes; a year of daily readings is
 *  ~10 KB), so the reader simply loads the newest HR_STORE_MAX rows into RAM
 *  for the History and Trends screens.
 * ========================================================================== */
#pragma once
#include <stdint.h>

#define HR_CSV_PATH  "/hr.csv"
#define HR_STORE_MAX 256

typedef struct { uint32_t epoch; uint16_t bpm, rmssd, sdnn; uint8_t quality; } hr_row_t;

static hr_row_t s_hr_rows[HR_STORE_MAX];   // newest first after hr_store_load()
static int      s_hr_rows_n = 0;

static bool hr_store_append(uint32_t epoch, int bpm, int rmssd, int sdnn, int quality) {
  if (!store_available()) { USBSerial.println("[hr] append: no storage"); return false; }
  bool fresh = !store_fs().exists(HR_CSV_PATH);
  File f = store_fs().open(HR_CSV_PATH, FILE_APPEND);
  if (!f) { USBSerial.println("[hr] append failed"); return false; }
  if (fresh) f.println("epoch,bpm,rmssd_ms,sdnn_ms,quality");
  f.printf("%lu,%d,%d,%d,%d\n", (unsigned long)epoch, bpm, rmssd, sdnn, quality);
  f.close();
  USBSerial.printf("[hr] saved: epoch=%lu bpm=%d rmssd=%d sdnn=%d q=%d\n", (unsigned long)epoch, bpm, rmssd, sdnn, quality);
  return true;
}

#ifndef HR_TEST_DATA
#define HR_TEST_DATA 0
#endif
#if HR_TEST_DATA
/* Synthetic history, made in RAM (dev only, gated by HR_TEST_DATA). 100 readings,
 * one to three per day going back ~7 weeks, newest first. Resting bpm drifts slowly
 * (fitness trend) with per-reading scatter; HRV runs inversely to bpm the way it does
 * for real, plus scatter; quality is mostly good with the odd poor reading. Repeatable
 * across boots (fixed PRNG seed) so a screen looks the same every time you open it. */
static uint32_t hr_rng_state = 0x9E3779B9u;
static uint32_t hr_rng(void) {
  hr_rng_state ^= hr_rng_state << 13; hr_rng_state ^= hr_rng_state >> 17; hr_rng_state ^= hr_rng_state << 5;
  return hr_rng_state;
}
static inline int hr_rng_range(int lo, int hi) { return lo + (int)(hr_rng() % (uint32_t)(hi - lo + 1)); }
static int hr_store_test_seed(void) {
  hr_rng_state = 0x9E3779B9u;
  uint32_t now = (uint32_t)rtc_now_epoch();
  if (now < 1700000000u) now = 1700000000u;
  const int N = 100;
  uint32_t t = now - 600u;                    // newest reading ten minutes ago
  for (int i = 0; i < N; i++) {
    // slow trend: resting rate eases from ~82 (oldest) to ~68 (newest) over the set
    int base = 68 + (int)((long)i * 14 / (N - 1));
    int bpm  = base + hr_rng_range(-9, 12);
    if (hr_rng_range(0, 11) == 0) bpm += hr_rng_range(15, 35);     // an after-exercise reading
    int rmssd = 92 - bpm / 2 + hr_rng_range(-10, 12); if (rmssd < 12) rmssd = 12;
    int sdnn  = rmssd + hr_rng_range(5, 25);
    int q     = hr_rng_range(0, 7) == 0 ? hr_rng_range(35, 65) : hr_rng_range(74, 98);
    s_hr_rows[i] = { t, (uint16_t)bpm, (uint16_t)rmssd, (uint16_t)sdnn, (uint8_t)q };
    // next (older) reading: 8-30 h earlier, so most days have one and some have two or three
    t -= (uint32_t)hr_rng_range(8, 30) * 3600u + (uint32_t)hr_rng_range(0, 3599);
  }
  s_hr_rows_n = N;
  return N;
}
#endif /* HR_TEST_DATA */

/* Load the newest HR_STORE_MAX rows (newest first). Returns the count. */
static int hr_store_load(void) {
#if HR_TEST_DATA
  return hr_store_test_seed();
#endif
  s_hr_rows_n = 0;
  if (!store_available() || !store_fs().exists(HR_CSV_PATH)) return 0;
  File f = store_fs().open(HR_CSV_PATH, FILE_READ);
  if (!f) return 0;
  // Ring of the last HR_STORE_MAX rows in file order, then reverse.
  static hr_row_t ring[HR_STORE_MAX];
  int head = 0, count = 0;
  char line[64];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof line - 1);
    line[n] = 0;
    if (n == 0 || line[0] < '0' || line[0] > '9') continue;
    unsigned long e; int b, r, s, q;
    if (sscanf(line, "%lu,%d,%d,%d,%d", &e, &b, &r, &s, &q) != 5) continue;
    ring[head] = { (uint32_t)e, (uint16_t)b, (uint16_t)r, (uint16_t)s, (uint8_t)q };
    head = (head + 1) % HR_STORE_MAX;
    if (count < HR_STORE_MAX) count++;
  }
  f.close();
  for (int i = 0; i < count; i++) s_hr_rows[i] = ring[(head - 1 - i + HR_STORE_MAX) % HR_STORE_MAX];
  s_hr_rows_n = count;
  return count;
}

/* Delete ONE measurement — the row with this epoch (the first match, so two
 * readings that landed in the same second lose only one). Streams the CSV into
 * a temp file, drops the matching line and swaps the files, the same shape as
 * the notification archive's delete: the whole history never has to fit in RAM,
 * and rows older than the HR_STORE_MAX window in s_hr_rows[] survive untouched.
 * Callers refresh the screen with hr_store_load() afterwards. */
#define HR_CSV_TMP_PATH "/hr.tmp"
static bool hr_store_delete(uint32_t epoch) {
#if HR_TEST_DATA
  /* Synthetic history lives only in RAM — drop the row there instead. */
  for (int i = 0; i < s_hr_rows_n; i++) {
    if (s_hr_rows[i].epoch != epoch) continue;
    for (int j = i; j + 1 < s_hr_rows_n; j++) s_hr_rows[j] = s_hr_rows[j + 1];
    s_hr_rows_n--;
    return true;
  }
  return false;
#else
  if (!store_available() || !store_fs().exists(HR_CSV_PATH)) return false;
  File in = store_fs().open(HR_CSV_PATH, FILE_READ);
  if (!in) return false;
  store_fs().remove(HR_CSV_TMP_PATH);
  File out = store_fs().open(HR_CSV_TMP_PATH, FILE_WRITE);
  if (!out) { in.close(); return false; }
  bool removed = false;
  char line[64];
  while (in.available()) {
    int n = in.readBytesUntil('\n', line, sizeof line - 1);
    if (n <= 0) continue;
    line[n] = 0;
    if (!removed && line[0] >= '0' && line[0] <= '9' &&
        (uint32_t)strtoul(line, nullptr, 10) == epoch) { removed = true; continue; }
    out.write((const uint8_t *)line, n);
    out.write((uint8_t)'\n');
  }
  in.close();
  out.close();
  if (!removed) { store_fs().remove(HR_CSV_TMP_PATH); return false; }
  store_fs().remove(HR_CSV_PATH);
  store_fs().rename(HR_CSV_TMP_PATH, HR_CSV_PATH);
  USBSerial.printf("[hr] deleted reading epoch=%lu\n", (unsigned long)epoch);
  return true;
#endif
}
