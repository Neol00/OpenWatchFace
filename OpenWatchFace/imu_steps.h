/* ============================================================================
 *  imu_steps.h — step counter on the QMI8658 6-axis IMU (both boards).
 *
 *  RAW-REGISTER driver over the shared Wire bus — NOT SensorLib. SensorLib's begin()
 *  re-inits I2C (broke touch), and even in custom-callback mode its rapid-fire
 *  read-modify-write config storm choked the ESP32 NG I2C driver (ESP_ERR_INVALID_STATE)
 *  so its writes silently never landed on the chip. Direct single-register writes over the
 *  one shared Wire (with a small settle gap) DO land — that's what reads real accel data.
 *
 *  This driver gets the accelerometer reading real motion reliably. The QMI8658 hardware
 *  pedometer engine is configured too; whether it validates steps is still being worked on,
 *  but the accel data path is solid (the p2p debug reacts to movement).
 *
 *  Board-neutral API:
 *    imu_steps_begin()    -> probe + configure (accel always-on from boot). true on success.
 *    imu_steps_available()-> configured OK?
 *    imu_steps_running()  -> engine live?
 *    imu_steps_count()    -> current step count.
 *    imu_steps_reset()    -> zero the counter.
 *    imu_steps_full_reset()-> soft-reset the chip + full reconfigure (Reset button).
 *    imu_steps_start()/stop() -> no-ops (engine is always-on); kept so wiring links.
 *
 *  I2C LOCKING IS THE CALLER'S JOB: begin() is wrapped in i2c_lock() in setup(); the app
 *  wraps count()/reset()/full_reset() in i2c_lock()/i2c_unlock().
 * ========================================================================== */
#pragma once

#if BOARD_HAS_IMU_QMI8658
#include <Wire.h>
#include <math.h>

/* The shared-bus lock lives in the .ino but is forward-declared after this header's include
 * point, so declare it here too (same signature) for imu_steps_tick()'s self-locking. */
static inline void i2c_lock(void);
static inline void i2c_unlock(void);

/* The IMU is ALWAYS reached over the shared touch bus (Wire, hardware I2C) while AWAKE — on
 * both the S3 and the C6. On the C6 the IMU's pads are ALSO bridged to GPIO5/6 (LP domain) so
 * the LP CORE can reach the same bus during deep sleep; but awake, the main CPU uses the
 * normal GPIO18/19 touch bus and leaves GPIO5/6 idle (driving both pins of a bridged pair at
 * once would short them). So there is no awake bit-bang / second bus — GPIO5/6 are sleep-only,
 * handled entirely inside the LP-core program. */
#define IMU_WIRE       Wire
#define IMU_BUS_SHARED 1

/* ---- Deep-sleep step counting via the RISC-V ULP (S3 only) -------------------------
 * The QMI8658 hardware pedometer is dead on this unit, and the main CPU is off in deep
 * sleep, so steps during sleep are counted by a RISC-V ULP program (ulp/main.c, prebuilt
 * blob in ulp_step_blob.h) that bit-bangs I2C to the IMU on GPIO14/15 and accumulates into
 * RTC slow memory. Gated by BOARD_HAS_ULP_STEPS (set in the S3 board header — the C6 uses a
 * different LP-core path, not wired yet). */
#if defined(BOARD_HAS_ULP_STEPS) && BOARD_HAS_ULP_STEPS
#include "ulp_riscv.h"              // ulp_riscv_load_binary/run/timer_stop/halt
#include "ulp_common_defs.h"        // RTC_SLOW_MEM
#include "ulp_common.h"             // ulp_set_wakeup_period
#include "soc/soc.h"                // SET/CLEAR_PERI_REG_MASK
#include "soc/rtc_cntl_reg.h"       // RTC_CNTL_COCPU_CTRL_REG bits (see imu_ulp_clocks_off)
#include "driver/rtc_io.h"          // rtc_gpio_deinit — hand pins back from RTC-IO to GPIO
#include "esp_sleep.h"              // esp_sleep_pd_config / ESP_PD_OPTION_AUTO
#include "ulp_step_blob.h"          // the prebuilt ULP binary + ulp_step_blob_len
#include "ulp_sleep_blob.h"         // sibling SLEEP-movement ULP binary + ulp_sleep_blob_len
// ULP variable offsets in RTC_SLOW_MEM, from the build's ulp_main.map (bytes -> /4 for the
// uint32 index). REBUILD the blob => re-check these in the new .map.
#define ULP_OFF_LAST_DEV     (0x328 / 4)
#define ULP_OFF_SAMPLE_CNT   (0x32c / 4)
#define ULP_OFF_STEP_CNT     (0x330 / 4)
static bool s_ulp_loaded = false;
// SLEEP ULP offsets, from ulp_sleep_build's ulp_main.map (different layout from the step
// blob — only ONE of the two is ever loaded at a time, they're mutually exclusive). REBUILD
// the sleep blob => re-check these in the new .map.
#define SLP_OFF_LAST_DEV     (0x300 / 4)
#define SLP_OFF_SAMPLE_CNT   (0x304 / 4)
#define SLP_OFF_MOVE_EVENTS  (0x308 / 4)
#define SLP_OFF_MOVE_PEAK    (0x30c / 4)
#define SLP_OFF_MOVE_ACCUM   (0x310 / 4)
static bool s_sleep_ulp_loaded = false;

#elif defined(BOARD_HAS_LP_STEPS) && BOARD_HAS_LP_STEPS
/* C6 LP-CORE deep-sleep step counting. The LP core bit-bangs I2C to the IMU on GPIO5/6 (in
 * the LP domain, bridged to the touch bus at the IMU pads) while the HP CPU sleeps. */
#include "ulp_lp_core.h"            // ulp_lp_core_load_binary/run(cfg)/stop
#include "driver/rtc_io.h"          // rtc_gpio_init/deinit — hand GPIO5/6 to/from the LP core
#include "esp_sleep.h"              // esp_sleep_pd_config / ESP_PD_OPTION_AUTO
#include "ulp_step_blob_c6.h"       // prebuilt LP-core STEP binary + ulp_step_blob_c6_len
#include "ulp_sleep_blob_c6.h"      // prebuilt LP-core SLEEP-movement binary + ulp_sleep_blob_c6_len
// LP-core shared vars are at ABSOLUTE RTC addresses (from the .map); index into RTC_SLOW_MEM
// = (addr - 0x50000000)/4. step_count=0x50000784 -> 481. REBUILD the blob => re-check .map.
#define LP_RTC_BASE          0x50000000u
#define LP_OFF_STEP_CNT      ((0x50000784u - LP_RTC_BASE) / 4)   // = 481
#define LP_OFF_SAMPLE_CNT    ((0x50000780u - LP_RTC_BASE) / 4)   // = 480
#define LP_OFF_LAST_DEV      ((0x5000077cu - LP_RTC_BASE) / 4)   // = 479
// SLEEP LP blob: SEPARATE .map (different var set/layout) — the step and sleep blobs are
// mutually exclusive (only one is ever loaded), so their RTC addresses are independent.
// From lp_sleep_build_c6/build/.../ulp_main.map. REBUILD the sleep blob => re-check these.
#define LP_SLP_OFF_MOVE_ACCUM   ((0x50000764u - LP_RTC_BASE) / 4)   // = 473
#define LP_SLP_OFF_MOVE_PEAK    ((0x50000760u - LP_RTC_BASE) / 4)   // = 472
#define LP_SLP_OFF_MOVE_EVENTS  ((0x5000075cu - LP_RTC_BASE) / 4)   // = 471
#define LP_SLP_OFF_SAMPLE_CNT   ((0x50000758u - LP_RTC_BASE) / 4)   // = 470
#define LP_SLP_OFF_LAST_DEV     ((0x50000754u - LP_RTC_BASE) / 4)   // = 469
static volatile uint32_t *const LP_RTC_MEM = (volatile uint32_t *)LP_RTC_BASE;
static bool s_lp_loaded       = false;   // step blob loaded?
static bool s_lp_sleep_loaded = false;   // sleep blob loaded?
// Sleep metrics folded from the LP sleep blob on the last wake (mirrors the S3 s_slp_*).
static uint32_t s_slp_accum = 0, s_slp_peak = 0, s_slp_events = 0, s_slp_samples = 0;
#endif

/* ---- QMI8658 register map (subset we need) ---- */
#define QMI_WHO_AM_I     0x00   // =0x05
#define QMI_CTRL1        0x02   // bit6 = ADDR_AI
#define QMI_CTRL2        0x03   // accel: (range<<4)|odr
#define QMI_CTRL5        0x06   // accel/gyro LPF
#define QMI_CTRL7        0x08   // bit0 = accel enable, bit7 = sync-sample mode
#define QMI_CTRL8        0x09   // bit4 = pedometer enable, bit7 = CTRL9 handshake type
#define QMI_CTRL9        0x0A   // command register
#define QMI_CAL1_L       0x0B
#define QMI_CAL1_H       0x0C
#define QMI_CAL2_L       0x0D
#define QMI_CAL2_H       0x0E
#define QMI_CAL3_L       0x0F
#define QMI_CAL3_H       0x10
#define QMI_CAL4_L       0x11
#define QMI_CAL4_H       0x12
#define QMI_STATUS_INT   0x2D   // bit7 = CTRL9 cmd done
#define QMI_STATUS0      0x2E   // bit0 = accel data ready
#define QMI_STATUS1      0x2F   // bit4 = pedometer (step) event
#define QMI_AX_L         0x35   // accel X..Z, 6 bytes LE
#define QMI_RST_RESULT   0x4D   // reads 0x80 once reset done
#define QMI_STEP_CNT_L   0x5A   // step count 3 bytes LE (0x5A..0x5C)
#define QMI_RESET        0x60

#define QMI_WHO_AM_I_VAL 0x05
#define QMI_CMD_ACK              0x00
#define QMI_CMD_CONFIG_PEDOMETER 0x0D
#define QMI_CMD_RESET_PEDOMETER  0x0F

// CTRL2 = (range<<4)|odr. 2g (range=0) @ 62.5Hz (odr=7) => 0x07. The pedometer timing
// params are calibrated to 62.5Hz; 2g matches the working PedometerExample (walking is
// ~1-1.5g, doesn't saturate).
#define QMI_CTRL2_VAL   0x07
#define QMI_CTRL5_VAL   0x00   // accel LPF OFF — full bandwidth so step transients reach the
                               // engine. (LPF on filtered out shaking, only rotation passed.)

static bool s_imu_ok      = false;
// s_imu_running survives deep sleep (RTC_DATA_ATTR): a deep-sleep wake re-runs setup() with
// the static reset to false, which would make the Fitness app think counting had stopped
// (button shows "Start", count shows 0). Persisting it keeps the counting session alive
// across sleep so the UI and the background sampler both see the true state.
RTC_DATA_ATTR static bool s_imu_running = false;
// SLEEP tracking is a SEPARATE owner of the accel from the step counter — they share ONLY the
// physical QMI8658 + I2C helpers + the (single) ULP/LP core, nothing else. s_sleep_running is
// the sleep counterpart of s_imu_running: true while a sleep session owns the accel. The two
// are mutually exclusive (one chip, one ULP) and each has its OWN start/stop, arm, collect,
// wanted-check, blob, and RTC offsets below. RTC_DATA_ATTR so it survives the deep-sleep timer
// wakes a night spans; the durable truth is s_sleep_mode (NVS), re-asserted at boot.
RTC_DATA_ATTR static bool s_sleep_running = false;

/* ---- raw register helpers over IMU_WIRE (the shared touch bus) ----
 * STOP (true) between address and data — the NG I2C driver is flaky with repeated-start
 * here and it left the bus in a state that tripped the touch driver. A small settle gap
 * after each write keeps the NG driver from choking on back-to-back writes. (Awake the IMU is
 * always on the shared Wire — even on the modded C6; GPIO5/6 are used ONLY by the LP core in
 * sleep, never by this awake path.) */
static bool qmi_w(uint8_t reg, uint8_t val) {
  IMU_WIRE.beginTransmission(IMU_QMI8658_ADDR);
  IMU_WIRE.write(reg);
  IMU_WIRE.write(val);
  bool ok = (IMU_WIRE.endTransmission() == 0);
  delayMicroseconds(400);
  return ok;
}
static int qmi_r(uint8_t reg) {
  IMU_WIRE.beginTransmission(IMU_QMI8658_ADDR);
  IMU_WIRE.write(reg);
  if (IMU_WIRE.endTransmission(true) != 0) return -1;   // STOP, not repeated-start
  if (IMU_WIRE.requestFrom((int)IMU_QMI8658_ADDR, 1) != 1) return -1;
  return IMU_WIRE.read();
}

/* CTRL9 command handshake: write cmd, wait STATUS_INT bit7, ACK, wait it clears. */
static bool qmi_ctrl9(uint8_t cmd, uint32_t timeout_ms = 300) {
  if (!qmi_w(QMI_CTRL9, cmd)) return false;
  uint32_t t0 = millis();
  while (!(qmi_r(QMI_STATUS_INT) & 0x80)) {
    if (millis() - t0 > timeout_ms) return false;
    delay(1);
  }
  if (!qmi_w(QMI_CTRL9, QMI_CMD_ACK)) return false;
  t0 = millis();
  while (qmi_r(QMI_STATUS_INT) & 0x80) {
    if (millis() - t0 > timeout_ms) return false;
    delay(1);
  }
  return true;
}


/* ---- SOFTWARE step detection on the accel magnitude --------------------------------
 * The hardware pedometer does not work on this unit, so we count steps ourselves from the
 * accelerometer. A step is a peak in the gravity-removed acceleration magnitude that rises
 * above a threshold, falls below a lower hysteresis threshold, and is spaced >= STEP_MIN_MS
 * from the last (rejects vibration/double-counts). */
#define STEP_HI_THRESH   3000    // |a|-gravity must exceed this (raw counts @2g; ~185mg) to arm
                                 // (raised slightly from 2600 to reduce over-sensitivity)
#define STEP_LO_THRESH   1400    // ...then drop below this to complete a step (hysteresis)
#define STEP_MIN_MS      260     // min ms between steps (~3.8 steps/s max — covers running)

RTC_DATA_ATTR static uint32_t s_step_total = 0;   // survives deep/light sleep
static bool     s_step_armed   = false;           // currently above the high threshold?
static uint32_t s_last_step_ms = 0;
static float    s_grav_mag     = 16384.0f;        // slow-tracked gravity magnitude (1g @2g)

/* PROBE + configure the QMI8658 at boot. Parks the accel off unless a counting session
 * survived sleep (then re-enables it). Caller holds i2c_lock; assumes Wire.begin() already
 * ran and runs AFTER touch init (shared-bus order). */
static bool imu_steps_begin(void) {
  if (s_imu_ok) return true;

  int who = qmi_r(QMI_WHO_AM_I);
  if (who != QMI_WHO_AM_I_VAL) {
    USBSerial.printf("[imu] WHO_AM_I=0x%02X (expected 0x05) - not a QMI8658\n", who);
    return false;
  }

  // Soft reset -> wait for completion (RST_RESULT reads 0x80). Leaves the accel OFF.
  qmi_w(QMI_RESET, 0xB0);
  uint32_t t0 = millis();
  while (qmi_r(QMI_RST_RESULT) != 0x80) {
    if (millis() - t0 > 500) { USBSerial.println("[imu] reset timeout"); break; }
    delay(10);
  }
  s_imu_ok = true;

  // If a counting session was in progress before sleep (s_imu_running survived in RTC mem),
  // RE-ENABLE the accel so awake background sampling resumes seamlessly — don't park it off.
  // Otherwise park OFF: the IMU draws ~nothing and won't count until Fitness > Start.
  if (s_imu_running) {
    qmi_w(QMI_CTRL2, QMI_CTRL2_VAL);   // 2g @ 62.5Hz
    qmi_w(QMI_CTRL5, QMI_CTRL5_VAL);   // accel LPF off
    qmi_w(QMI_CTRL7, 0x01);            // accel ON
    uint32_t dr = millis();
    while (!(qmi_r(QMI_STATUS0) & 0x01)) { if (millis() - dr > 300) break; delay(5); }
    s_last_step_ms = millis();         // reset detector timing for the resumed session
    s_step_armed   = false;
    s_grav_mag     = 16384.0f;
    USBSerial.println("[imu] QMI8658 present, counting session resumed (accel ON)");
  } else {
    qmi_w(QMI_CTRL7, 0x00);            // park OFF
    USBSerial.println("[imu] QMI8658 present, parked off (counts on Fitness > Start)");
  }
  return true;
}


/* Power the accel ON and (re)start counting. RESUMES the accumulated total — it does NOT
 * zero s_step_total, so Start after a Stop continues from where it left off (only the Reset
 * button clears the count, via imu_steps_full_reset). Only the software DETECTOR state is
 * reset so it starts clean on fresh data. Caller holds i2c_lock. Returns true once the accel
 * is sampling. */
static bool imu_steps_start(void) {
  if (!s_imu_ok) return false;

  qmi_w(QMI_CTRL2, QMI_CTRL2_VAL);   // 2g @ 62.5Hz
  qmi_w(QMI_CTRL5, QMI_CTRL5_VAL);   // accel LPF off (full bandwidth)
  qmi_w(QMI_CTRL7, 0x01);            // accel ON, async

  // Wait for first sample so the detector starts on real data.
  uint32_t dr = millis();
  while (!(qmi_r(QMI_STATUS0) & 0x01)) { if (millis() - dr > 300) break; delay(5); }

  s_step_armed   = false;            // reset detector state only — keep s_step_total
  s_last_step_ms = millis();
  s_grav_mag     = 16384.0f;
  s_imu_running  = true;
  USBSerial.println("[imu] step start: accel ON, counting started");
  return true;
}

/* Power the accel OFF and stop STEP counting. After this the IMU draws nothing and normal
 * deep-sleep can resume. Caller holds i2c_lock. */
static void imu_steps_stop(void) {
  if (!s_imu_ok) return;
  qmi_w(QMI_CTRL7, 0x00);            // accel OFF
  s_imu_running = false;
  USBSerial.println("[imu] step stop: accel OFF, counting stopped");
}

/* ---- SLEEP session accel power (separate owner from the step counter) --------------
 * Same physical accel + the same CTRL2/5/7 config (2g@62.5Hz, LPF off) as steps, but tracked
 * by s_sleep_running and used only by the sleep-movement ULP/LP path. Mutually exclusive with
 * step counting (one chip). Caller holds i2c_lock. */
static bool imu_sleep_start(void) {
  if (!s_imu_ok) return false;
  qmi_w(QMI_CTRL2, QMI_CTRL2_VAL);   // 2g @ 62.5Hz
  qmi_w(QMI_CTRL5, QMI_CTRL5_VAL);   // accel LPF off
  qmi_w(QMI_CTRL7, 0x01);            // accel ON, async
  uint32_t dr = millis();
  while (!(qmi_r(QMI_STATUS0) & 0x01)) { if (millis() - dr > 300) break; delay(5); }
  s_sleep_running = true;
  USBSerial.println("[imu] sleep start: accel ON, movement tracking started");
  return true;
}

/* Power the accel OFF and stop the SLEEP session. Caller holds i2c_lock. */
static void imu_sleep_stop(void) {
  if (!s_imu_ok) return;
  qmi_w(QMI_CTRL7, 0x00);            // accel OFF
  s_sleep_running = false;
  USBSerial.println("[imu] sleep stop: accel OFF, movement tracking stopped");
}

static inline bool imu_steps_available(void) { return s_imu_ok; }
/* "Is the STEP counter running?" — pure step state. Sleep tracking is a separate owner
 * (s_sleep_running) and never sets s_imu_running, so no exclusion is needed here. */
static inline bool imu_steps_running(void)   { return s_imu_running; }
/* "Is a SLEEP-tracking session running?" — the sleep counterpart, fully independent. */
static inline bool imu_sleep_running(void)   { return s_sleep_running; }
static inline uint32_t imu_steps_count(void) { return s_step_total; }
static inline void imu_steps_reset(void)     { s_step_total = 0; }

/* ---- NVS persistence hooks --------------------------------------------------------
 * The count lives in RTC memory (survives deep-sleep TIMER wakes) but is WIPED by a full
 * power-off — and on the C6, by the RST-button wake (a chip reset). So we mirror it to NVS.
 * The actual flash read/write lives in the .ino (which owns the shared `prefs` handle); these
 * helpers just expose/accept the value and rate-limit saves to spare flash wear.
 *   - imu_steps_set_total(): called once at boot with the NVS-loaded value (RTC mem may be
 *     stale/zero after a reset; trust NVS as the floor — take the larger so we never go
 *     backwards if RTC happened to survive with a higher count).
 *   - imu_steps_save_due(): true if the count changed AND enough time passed since the last
 *     save (the .ino calls imu_steps_mark_saved() after it writes). */
static uint32_t s_nvs_last_saved = 0;       // last value we persisted
static uint32_t s_nvs_last_save_ms = 0;
#define STEP_NVS_MIN_SAVE_MS  60000UL       // at most one NVS write/minute while counting

static inline void imu_steps_set_total(uint32_t nvs_val) {
  if (nvs_val > s_step_total) s_step_total = nvs_val;   // never regress below the saved floor
  s_nvs_last_saved = s_step_total;
}

/* Restore the "was counting" state from NVS and, if it was, RESUME counting (re-enable the
 * accel). Needed because s_imu_running is RTC_DATA_ATTR and is WIPED by a full power-off or a
 * C6 RST-button wake — without this the watch would silently stop counting every cold boot,
 * forcing the user to re-tap Start. Call AFTER imu_steps_begin(). Caller holds i2c_lock. */
static void imu_steps_restore_running(bool was_running) {
  if (!s_imu_ok) return;
  if (was_running && !s_imu_running) {
    // RTC said not-running (wiped by reset) but NVS says we were — resume the session.
    s_imu_running = true;
    qmi_w(QMI_CTRL2, QMI_CTRL2_VAL);
    qmi_w(QMI_CTRL5, QMI_CTRL5_VAL);
    qmi_w(QMI_CTRL7, 0x01);            // accel ON
    uint32_t dr = millis();
    while (!(qmi_r(QMI_STATUS0) & 0x01)) { if (millis() - dr > 300) break; delay(5); }
    s_last_step_ms = millis();
    s_step_armed   = false;
    s_grav_mag     = 16384.0f;
    USBSerial.println("[imu] step counting RESUMED from NVS state after cold boot");
  }
}

/* SLEEP counterpart of imu_steps_restore_running: resume a sleep session's accel if it was
 * active before a power-off / C6 RST wake (s_sleep_running is RTC and gets wiped). Driven by
 * s_sleep_mode (NVS, the durable truth). Call AFTER imu_steps_begin(). Caller holds i2c_lock. */
static void imu_sleep_restore_running(bool was_running) {
  if (!s_imu_ok) return;
  if (was_running && !s_sleep_running) {
    s_sleep_running = true;
    qmi_w(QMI_CTRL2, QMI_CTRL2_VAL);
    qmi_w(QMI_CTRL5, QMI_CTRL5_VAL);
    qmi_w(QMI_CTRL7, 0x01);            // accel ON
    uint32_t dr = millis();
    while (!(qmi_r(QMI_STATUS0) & 0x01)) { if (millis() - dr > 300) break; delay(5); }
    USBSerial.println("[imu] sleep tracking RESUMED from NVS state after cold boot");
  }
}
static inline bool imu_steps_save_due(uint32_t now_ms) {
  if (s_step_total == s_nvs_last_saved) return false;            // nothing changed
  if (now_ms - s_nvs_last_save_ms < STEP_NVS_MIN_SAVE_MS) return false;
  return true;
}
static inline void imu_steps_mark_saved(uint32_t now_ms) {
  s_nvs_last_saved   = s_step_total;
  s_nvs_last_save_ms = now_ms;
}

/* Read one accel sample and update the step detector. Call this repeatedly while counting
 * (e.g. from the Fitness refresh timer, and later from each light-sleep wake). Caller holds
 * i2c_lock. Returns the magnitude deviation for debugging. */
static int imu_steps_sample(void) {
  if (!s_imu_ok || !s_imu_running) return 0;
  int xl = qmi_r(QMI_AX_L), xh = qmi_r(0x36);
  int yl = qmi_r(0x37),     yh = qmi_r(0x38);
  int zl = qmi_r(0x39),     zh = qmi_r(0x3A);
  int16_t ax = (int16_t)((xh << 8) | (xl & 0xFF));
  int16_t ay = (int16_t)((yh << 8) | (yl & 0xFF));
  int16_t az = (int16_t)((zh << 8) | (zl & 0xFF));
  float mag = sqrtf((float)ax*ax + (float)ay*ay + (float)az*az);

  // Slow-track gravity (low-pass) so we measure DYNAMIC accel (mag - gravity).
  s_grav_mag += (mag - s_grav_mag) * 0.05f;
  int dev = (int)fabsf(mag - s_grav_mag);

  uint32_t now = millis();
  if (!s_step_armed) {
    if (dev > STEP_HI_THRESH) s_step_armed = true;       // rising edge over high threshold
  } else {
    if (dev < STEP_LO_THRESH) {                          // fell back below low threshold
      s_step_armed = false;
      if (now - s_last_step_ms >= STEP_MIN_MS) {         // spaced far enough = a real step
        s_step_total++;
        s_last_step_ms = now;
      }
    }
  }
  return dev;
}

/* Read one accel sample and return ONLY the gravity-removed deviation — WITHOUT running
 * the step detector or touching s_step_total / s_grav_mag. This is the decoupled read for
 * SLEEP-movement sampling: it must not pollute the step count (which imu_steps_sample()
 * would, since any movement crossing the walking thresholds bumps s_step_total). Uses its
 * own slow-tracked gravity so the two paths never interfere. Caller holds i2c_lock.
 * Returns the deviation (same units as imu_steps_sample's dev), or 0 if the IMU is off. */
static float s_dev_grav_mag = 16384.0f;   // private gravity tracker for imu_read_accel_dev
static int imu_read_accel_dev(void) {
  if (!s_imu_ok) return 0;
  int xl = qmi_r(QMI_AX_L), xh = qmi_r(0x36);
  int yl = qmi_r(0x37),     yh = qmi_r(0x38);
  int zl = qmi_r(0x39),     zh = qmi_r(0x3A);
  int16_t ax = (int16_t)((xh << 8) | (xl & 0xFF));
  int16_t ay = (int16_t)((yh << 8) | (yl & 0xFF));
  int16_t az = (int16_t)((zh << 8) | (zl & 0xFF));
  float mag = sqrtf((float)ax*ax + (float)ay*ay + (float)az*az);
  s_dev_grav_mag += (mag - s_dev_grav_mag) * 0.05f;
  return (int)fabsf(mag - s_dev_grav_mag);
}

/* Background sampler — call from the main loop EVERY iteration. Self-rate-limited to ~50ms
 * and self-locking (takes i2c_lock), so step counting continues regardless of which screen
 * is open (or none). No-ops unless a counting session is active. This is what keeps the
 * counter running in the background after you leave the Fitness app while awake. */
static void imu_steps_tick(uint32_t now_ms) {
  static uint32_t s_last_tick = 0;
  // Pure STEP gate. Sleep tracking is a separate owner (s_sleep_running) and never sets
  // s_imu_running, so a sleep session can't trip this — no sleep-session check needed.
  if (!s_imu_ok || !s_imu_running) return;
  if (now_ms - s_last_tick < 50) return;
  s_last_tick = now_ms;
#if IMU_BUS_SHARED
  i2c_lock();                 // shared with touch -> must lock
  imu_steps_sample();
  i2c_unlock();
#else
  imu_steps_sample();         // IMU on its own Wire1 -> no lock needed
#endif
}

/* Reset button: zero the count + restart the detector (re-establish accel if needed). */
static void imu_steps_full_reset(void) {
  if (!s_imu_ok) return;
  s_step_total   = 0;
  s_step_armed   = false;
  s_last_step_ms = millis();
  if (s_imu_running) { qmi_w(QMI_CTRL7, 0x01); }   // ensure accel still on
}

/* Diagnostic: sample once + print the detector state + count. Caller holds i2c_lock. */
static void imu_steps_debug(void) {
  if (!s_imu_ok) return;
  int dev = imu_steps_sample();
  USBSerial.printf("[imu dbg] steps=%lu dev=%d armed=%d run=%d\n",
                   (unsigned long)s_step_total, dev, s_step_armed, s_imu_running);
}

/* ============================================================================
 *  ULP deep-sleep handoff (S3). When the watch is about to deep-sleep WHILE counting,
 *  the main CPU hands the IMU to the ULP: the accel stays powered (main CPU left it on in
 *  imu_steps_start), the ULP bit-bangs I2C on GPIO14/15 and counts into RTC_SLOW_MEM. On
 *  wake the main CPU folds the ULP's count into s_step_total. When NOT counting, none of
 *  this runs and normal deep sleep proceeds with the IMU off.
 * ========================================================================== */
#if defined(BOARD_HAS_ULP_STEPS) && BOARD_HAS_ULP_STEPS

/* THE deep-sleep drain root cause (S3): ulp_riscv_run() force-enables the COCPU clock —
 * RTC_CNTL_COCPU_CLK_FO + RTC_CNTL_COCPU_CLKGATE_EN in RTC_CNTL_COCPU_CTRL_REG — and IDF's
 * ulp_riscv_halt() NEVER clears them (verified against IDF v5.5 ulp_riscv.c: halt() only
 * stops the timer, sets COCPU_DONE and holds the core in reset). These bits live in the
 * ALWAYS-ON RTC domain: they persist across deep sleep and every reboot, and only a full
 * power loss resets them. On a soldered-battery unit that means a SINGLE ulp_riscv_run()
 * (one step/sleep session, ever) leaves the coprocessor clock forced on through every
 * later deep sleep — the "2 weeks -> 1.5 days on the same battery after switching to the
 * RISC-V ULP" regression. Call this after EVERY halt so no sleep ever inherits the state. */
static void imu_ulp_clocks_off(void) {
  CLEAR_PERI_REG_MASK(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_CLKGATE_EN);
  CLEAR_PERI_REG_MASK(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_CLK_FO);
  CLEAR_PERI_REG_MASK(RTC_CNTL_COCPU_CTRL_REG, RTC_CNTL_COCPU_DONE_FORCE);
}

/* Release the bit-bang I2C pins (GPIO14/15) from RTC-IO back to normal GPIO + drop the
 * RTC_PERIPH hold. The ULP (step OR sleep) held them; until released, the Arduino I2C driver
 * can't drive them and touch+battery (same bus) come up dead. Shared by both collect paths. */
static void imu_ulp_release_pins(void) {
  rtc_gpio_deinit((gpio_num_t)IIC_SDA);   // GPIO15
  rtc_gpio_deinit((gpio_num_t)IIC_SCL);   // GPIO14
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
}

/* ======================= STEP ULP (S3) — pure step counting ======================= */

/* True if the ULP STEP counter should run during the upcoming deep sleep. Requires the live
 * driver (s_imu_ok) because a step session is only ever (re)armed from a full boot. */
static inline bool imu_steps_ulp_wanted(void) { return s_imu_ok && s_imu_running; }

/* Arm the STEP ULP just before esp_deep_sleep_start(). Caller should NOT hold i2c_lock. */
static bool imu_steps_ulp_arm(void) {
  if (!imu_steps_ulp_wanted()) return false;
  if (!s_ulp_loaded) {
    if (ulp_riscv_load_binary(ulp_step_blob, ulp_step_blob_len) != ESP_OK) {
      USBSerial.println("[imu ulp] step load_binary failed");
      return false;
    }
    s_ulp_loaded = true;
  }
  RTC_SLOW_MEM[ULP_OFF_STEP_CNT]   = 0;
  RTC_SLOW_MEM[ULP_OFF_SAMPLE_CNT] = 0;
  RTC_SLOW_MEM[ULP_OFF_LAST_DEV]   = 0;
  ulp_set_wakeup_period(0, 16000);    // ~62 Hz, matching the accel ODR
  if (ulp_riscv_run() != ESP_OK) { USBSerial.println("[imu ulp] step run failed"); return false; }
  USBSerial.println("[imu ulp] armed for deep sleep (counting continues)");
  return true;
}

/* On wake: fold the ULP's step count into the total, halt the ULP, release the pins. Safe
 * before imu_steps_begin(); only call after a DEEPSLEEP wake. */
static void imu_steps_ulp_collect(void) {
  ulp_riscv_timer_stop();
  ulp_riscv_halt();
  imu_ulp_clocks_off();            // halt() leaves the COCPU clock forced ON — clear it
  uint32_t ulp_steps = RTC_SLOW_MEM[ULP_OFF_STEP_CNT];
  if (ulp_steps) {
    s_step_total += ulp_steps;
    RTC_SLOW_MEM[ULP_OFF_STEP_CNT] = 0;
    USBSerial.printf("[imu ulp] collected %lu steps from sleep (total now %lu)\n",
                     (unsigned long)ulp_steps, (unsigned long)s_step_total);
  }
  imu_ulp_release_pins();
  s_ulp_loaded = false;
}

/* ================== SLEEP ULP (S3) — separate sleep-movement path ================== */

/* Sleep-movement metrics folded from the sleep ULP on the last wake. sleep_track.h reads
 * these (imu_sleep_*) to log a CSV row, then they're overwritten next wake. */
static uint32_t s_slp_accum = 0, s_slp_peak = 0, s_slp_events = 0, s_slp_samples = 0;
static inline uint32_t imu_sleep_accum(void)   { return s_slp_accum; }
static inline uint32_t imu_sleep_peak(void)    { return s_slp_peak; }
static inline uint32_t imu_sleep_events(void)  { return s_slp_events; }
static inline uint32_t imu_sleep_samples(void) { return s_slp_samples; }

/* True if the SLEEP-movement ULP should run during the upcoming deep sleep. Gated on the
 * DURABLE s_sleep_running (RTC, survives the screen-less timer wakes), NOT s_imu_ok — so the
 * sleep blob re-arms every span of the night. */
static inline bool imu_sleep_ulp_wanted(void) { return s_sleep_running; }

/* Arm the SLEEP ULP just before esp_deep_sleep_start(). Caller should NOT hold i2c_lock. */
static bool imu_sleep_ulp_arm(void) {
  if (!imu_sleep_ulp_wanted()) return false;
  if (!s_sleep_ulp_loaded) {
    if (ulp_riscv_load_binary(ulp_sleep_blob, ulp_sleep_blob_len) != ESP_OK) {
      USBSerial.println("[imu ulp] sleep load_binary failed");
      return false;
    }
    s_sleep_ulp_loaded = true;
  }
  RTC_SLOW_MEM[SLP_OFF_MOVE_ACCUM]  = 0;
  RTC_SLOW_MEM[SLP_OFF_MOVE_PEAK]   = 0;
  RTC_SLOW_MEM[SLP_OFF_MOVE_EVENTS] = 0;
  RTC_SLOW_MEM[SLP_OFF_SAMPLE_CNT]  = 0;
  RTC_SLOW_MEM[SLP_OFF_LAST_DEV]    = 0;
  ulp_set_wakeup_period(0, 16000);    // ~62 Hz, matching the accel ODR
  if (ulp_riscv_run() != ESP_OK) { USBSerial.println("[imu ulp] sleep run failed"); return false; }
  USBSerial.println("[imu ulp] armed for deep sleep (sleep movement logging)");
  return true;
}

/* On wake: snapshot+zero the sleep movement metrics, halt the ULP, release the pins. Safe
 * before imu_steps_begin(); only call after a DEEPSLEEP wake. */
static void imu_sleep_ulp_collect(void) {
  ulp_riscv_timer_stop();
  ulp_riscv_halt();
  imu_ulp_clocks_off();            // halt() leaves the COCPU clock forced ON — clear it
  s_slp_accum   = RTC_SLOW_MEM[SLP_OFF_MOVE_ACCUM];
  s_slp_peak    = RTC_SLOW_MEM[SLP_OFF_MOVE_PEAK];
  s_slp_events  = RTC_SLOW_MEM[SLP_OFF_MOVE_EVENTS];
  s_slp_samples = RTC_SLOW_MEM[SLP_OFF_SAMPLE_CNT];
  RTC_SLOW_MEM[SLP_OFF_MOVE_ACCUM]  = 0;
  RTC_SLOW_MEM[SLP_OFF_MOVE_PEAK]   = 0;
  RTC_SLOW_MEM[SLP_OFF_MOVE_EVENTS] = 0;
  RTC_SLOW_MEM[SLP_OFF_SAMPLE_CNT]  = 0;
  USBSerial.printf("[imu ulp] sleep movement: accum=%lu peak=%lu events=%lu n=%lu\n",
                   (unsigned long)s_slp_accum, (unsigned long)s_slp_peak,
                   (unsigned long)s_slp_events, (unsigned long)s_slp_samples);
  imu_ulp_release_pins();
  s_sleep_ulp_loaded = false;
}

/* ===================== NO-SESSION teardown (the deep-sleep drain fix) =====================
 * Force the FULLY powered-down state before a deep sleep in which NO step/sleep ULP session is
 * wanted. Belt-and-suspenders, because two pieces of state PERSIST across deep sleep and were
 * only ever cleared on the session-collect path — so if a session had run earlier, they could
 * stay "on" through later session-less sleeps and burn ~tens of mA every cycle (the observed
 * bimodal 10 vs 50 mA average):
 *
 *   1) The QMI8658's CTRL7 (accel enable) is in the CHIP's registers, and the chip sits on the
 *      always-on I2C/RTC rail (ALDO1, never cut) — so deep sleep does NOT reset it. If the accel
 *      was left ON, it keeps sampling at 62.5 Hz the entire sleep. Write CTRL7=0 to be sure.
 *   2) esp_sleep_pd_config(RTC_PERIPH, ON) is latched in RTC config registers that SURVIVE deep
 *      sleep (that's their job). An armed session set it ON to keep the ULP alive; nothing in the
 *      session-less path put it back to AUTO, so the whole RTC peripheral domain could stay
 *      powered every sleep. Force it back to AUTO so the domain powers down.
 *   Also halt the ULP cores + release the RTC-IO pins in case a stale program is still clocking.
 *
 * Caller (arm_wakes_and_sleep) does NOT hold i2c_lock and is single-threaded at this point
 * (BLE already torn down, about to deep-sleep), so the direct qmi_w() is safe. Idempotent and
 * cheap, so it's run on EVERY session-less sleep. */
static void imu_ensure_off_for_sleep(void) {
  // Accel OFF in the chip — UNCONDITIONAL, not gated on s_imu_ok. The enable bit lives in
  // the QMI8658 on the always-on rail, so it persists across sleep; but s_imu_ok is false on
  // screen-less wakes (imu_steps_begin never ran there), which used to SKIP this write and
  // leave a stale accel sampling at 62.5 Hz through every following sleep. The chip is always
  // powered/reachable here (Wire is up on every path into a sleep); if it's absent the write
  // just NAKs harmlessly.
  qmi_w(QMI_CTRL7, 0x00);
  // Halt either ULP program if one is still loaded/running, then hand the RTC-IO pins back and
  // drop RTC_PERIPH to AUTO so the domain isn't kept powered through sleep.
  ulp_riscv_timer_stop();
  ulp_riscv_halt();
  imu_ulp_clocks_off();   // THE drain fix: halt() never clears COCPU_CLK_FO/CLKGATE_EN, and the
                          // forced-on COCPU clock persists (RTC domain) through every later sleep
  s_ulp_loaded = false;
  s_sleep_ulp_loaded = false;
  imu_ulp_release_pins();                 // rtc_gpio_deinit + esp_sleep_pd_config(RTC_PERIPH, AUTO)
}

#elif defined(BOARD_HAS_LP_STEPS) && BOARD_HAS_LP_STEPS
/* ---- C6 LP-core variant: SEPARATE step + sleep families (same split as the S3 ULP) ---- */

/* Hand GPIO5/6 to the LP/RTC domain so the LP core can drive them in sleep. (Bridged to the
 * touch bus GPIO18/19 at the IMU pads; the HP touch bus is off in sleep, so no contention.) */
static void imu_lp_take_pins(void) {
  rtc_gpio_init((gpio_num_t)IMU_LP_SDA_GPIO);
  rtc_gpio_init((gpio_num_t)IMU_LP_SCL_GPIO);
}
/* Release GPIO5/6 back to normal GPIO + drop the RTC_PERIPH hold. Must happen before Wire is
 * used on wake, or the bridged pins fight. Shared by both collect paths. */
static void imu_lp_release_pins(void) {
  rtc_gpio_deinit((gpio_num_t)IMU_LP_SDA_GPIO);
  rtc_gpio_deinit((gpio_num_t)IMU_LP_SCL_GPIO);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
}

/* ======================= STEP LP core (C6) — pure step counting ======================= */

static inline bool imu_steps_ulp_wanted(void) { return s_imu_ok && s_imu_running; }

static bool imu_steps_ulp_arm(void) {
  if (!imu_steps_ulp_wanted()) return false;
  imu_lp_take_pins();
  if (!s_lp_loaded) {
    if (ulp_lp_core_load_binary(ulp_step_blob_c6, ulp_step_blob_c6_len) != ESP_OK) {
      USBSerial.println("[imu lp] step load_binary failed");
      return false;
    }
    s_lp_loaded = true;
  }
  LP_RTC_MEM[LP_OFF_STEP_CNT]   = 0;
  LP_RTC_MEM[LP_OFF_SAMPLE_CNT] = 0;
  LP_RTC_MEM[LP_OFF_LAST_DEV]   = 0;
  ulp_lp_core_cfg_t cfg = {
    .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER,
    .lp_timer_sleep_duration_us = 16000,    // ~62 Hz, matching the accel ODR
  };
  if (ulp_lp_core_run(&cfg) != ESP_OK) { USBSerial.println("[imu lp] step run failed"); return false; }
  USBSerial.println("[imu lp] armed for deep sleep (counting continues)");
  return true;
}

static void imu_steps_ulp_collect(void) {
  ulp_lp_core_stop();
  uint32_t lp_steps = LP_RTC_MEM[LP_OFF_STEP_CNT];
  if (lp_steps) {
    s_step_total += lp_steps;
    LP_RTC_MEM[LP_OFF_STEP_CNT] = 0;
    USBSerial.printf("[imu lp] collected %lu steps from sleep (total now %lu)\n",
                     (unsigned long)lp_steps, (unsigned long)s_step_total);
  }
  imu_lp_release_pins();
  s_lp_loaded = false;
}

/* ================== SLEEP LP core (C6) — separate sleep-movement path ==================
 * C6 now HAS a dedicated LP-core sleep blob (ulp_sleep_blob_c6, built in lp_sleep_build_c6),
 * so a sleep session runs the LP core with the sleep-movement program — same as the S3 sleep
 * ULP. Gated on the DURABLE s_sleep_running (RTC) so it re-arms every span of the night. */
static inline uint32_t imu_sleep_accum(void)   { return s_slp_accum; }
static inline uint32_t imu_sleep_peak(void)    { return s_slp_peak; }
static inline uint32_t imu_sleep_events(void)  { return s_slp_events; }
static inline uint32_t imu_sleep_samples(void) { return s_slp_samples; }
static inline bool     imu_sleep_ulp_wanted(void) { return s_sleep_running; }

static bool imu_sleep_ulp_arm(void) {
  if (!imu_sleep_ulp_wanted()) return false;
  imu_lp_take_pins();
  if (!s_lp_sleep_loaded) {
    if (ulp_lp_core_load_binary(ulp_sleep_blob_c6, ulp_sleep_blob_c6_len) != ESP_OK) {
      USBSerial.println("[imu lp] sleep load_binary failed");
      return false;
    }
    s_lp_sleep_loaded = true;
  }
  LP_RTC_MEM[LP_SLP_OFF_MOVE_ACCUM]  = 0;
  LP_RTC_MEM[LP_SLP_OFF_MOVE_PEAK]   = 0;
  LP_RTC_MEM[LP_SLP_OFF_MOVE_EVENTS] = 0;
  LP_RTC_MEM[LP_SLP_OFF_SAMPLE_CNT]  = 0;
  LP_RTC_MEM[LP_SLP_OFF_LAST_DEV]    = 0;
  ulp_lp_core_cfg_t cfg = {
    .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER,
    .lp_timer_sleep_duration_us = 16000,    // ~62 Hz, matching the accel ODR
  };
  if (ulp_lp_core_run(&cfg) != ESP_OK) { USBSerial.println("[imu lp] sleep run failed"); return false; }
  USBSerial.println("[imu lp] armed for deep sleep (sleep movement logging)");
  return true;
}

static void imu_sleep_ulp_collect(void) {
  ulp_lp_core_stop();
  s_slp_accum   = LP_RTC_MEM[LP_SLP_OFF_MOVE_ACCUM];
  s_slp_peak    = LP_RTC_MEM[LP_SLP_OFF_MOVE_PEAK];
  s_slp_events  = LP_RTC_MEM[LP_SLP_OFF_MOVE_EVENTS];
  s_slp_samples = LP_RTC_MEM[LP_SLP_OFF_SAMPLE_CNT];
  LP_RTC_MEM[LP_SLP_OFF_MOVE_ACCUM]  = 0;
  LP_RTC_MEM[LP_SLP_OFF_MOVE_PEAK]   = 0;
  LP_RTC_MEM[LP_SLP_OFF_MOVE_EVENTS] = 0;
  LP_RTC_MEM[LP_SLP_OFF_SAMPLE_CNT]  = 0;
  USBSerial.printf("[imu lp] sleep movement: accum=%lu peak=%lu events=%lu n=%lu\n",
                   (unsigned long)s_slp_accum, (unsigned long)s_slp_peak,
                   (unsigned long)s_slp_events, (unsigned long)s_slp_samples);
  imu_lp_release_pins();
  s_lp_sleep_loaded = false;
}

/* NO-SESSION teardown (C6 LP-core analog of the S3 version above; same deep-sleep drain fix).
 * Force the accel OFF in the chip and put the LP core + RTC_PERIPH back to their powered-down
 * state, in case a prior session left them on (both persist across deep sleep). See the S3
 * version's comment for the full rationale. Caller does NOT hold i2c_lock; single-threaded here. */
static void imu_ensure_off_for_sleep(void) {
  qmi_w(QMI_CTRL7, 0x00);   // accel OFF in the chip — unconditional; see the S3 version's comment
                            // (s_imu_ok is false on screen-less wakes, but the bit persists in
                            // the always-powered chip and a NAK on an absent chip is harmless)
  ulp_lp_core_stop();
  s_lp_loaded = false;
  s_lp_sleep_loaded = false;
  imu_lp_release_pins();                   // rtc_gpio_deinit(5/6) + esp_sleep_pd_config(RTC_PERIPH, AUTO)
}

#else  /* IMU present but no ULP/LP step path — no-op arm/collect so callers link */
static inline bool imu_steps_ulp_wanted(void)  { return false; }
static inline bool imu_sleep_ulp_wanted(void)  { return false; }
static inline bool imu_steps_ulp_arm(void)     { return false; }
static inline bool imu_sleep_ulp_arm(void)     { return false; }
static inline void imu_steps_ulp_collect(void) {}
static inline void imu_sleep_ulp_collect(void) {}
/* Even without a ULP path, force the accel OFF before a session-less sleep (the chip is on the
 * always-on rail, so CTRL7 persists across deep sleep). Unconditional — see the S3 version. */
static inline void imu_ensure_off_for_sleep(void) { qmi_w(QMI_CTRL7, 0x00); }
static inline uint32_t imu_sleep_accum(void)   { return 0; }
static inline uint32_t imu_sleep_peak(void)    { return 0; }
static inline uint32_t imu_sleep_events(void)  { return 0; }
static inline uint32_t imu_sleep_samples(void) { return 0; }
#endif

#else  /* no IMU on this board — harmless stubs */

static inline bool     imu_steps_begin(void)      { return false; }
static inline bool     imu_steps_available(void)  { return false; }
static inline bool     imu_steps_running(void)    { return false; }
static inline bool     imu_sleep_running(void)     { return false; }
static inline uint32_t imu_steps_count(void)      { return 0; }
static inline void     imu_steps_reset(void)      {}
static inline void     imu_steps_full_reset(void) {}
static inline bool     imu_steps_start(void)      { return false; }
static inline void     imu_steps_stop(void)       {}
static inline bool     imu_sleep_start(void)       { return false; }
static inline void     imu_sleep_stop(void)        {}
static inline int      imu_steps_sample(void)     { return 0; }
static inline int      imu_read_accel_dev(void)   { return 0; }
static inline void     imu_steps_tick(uint32_t)   {}
static inline void     imu_steps_set_total(uint32_t) {}
static inline void     imu_steps_restore_running(bool) {}
static inline void     imu_sleep_restore_running(bool)  {}
static inline bool     imu_steps_save_due(uint32_t)  { return false; }
static inline void     imu_steps_mark_saved(uint32_t){}
static inline bool     imu_steps_ulp_wanted(void) { return false; }
static inline bool     imu_sleep_ulp_wanted(void) { return false; }
static inline bool     imu_steps_ulp_arm(void)    { return false; }
static inline bool     imu_sleep_ulp_arm(void)    { return false; }
static inline void     imu_steps_ulp_collect(void){}
static inline void     imu_sleep_ulp_collect(void){}
static inline void     imu_ensure_off_for_sleep(void) {}   // no IMU -> nothing to power down
static inline uint32_t imu_sleep_accum(void)   { return 0; }
static inline uint32_t imu_sleep_peak(void)    { return 0; }
static inline uint32_t imu_sleep_events(void)  { return 0; }
static inline uint32_t imu_sleep_samples(void) { return 0; }

#endif  /* BOARD_HAS_IMU_QMI8658 */
