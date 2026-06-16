/* ============================================================================
 *  board_clock.h — wall-clock timekeeping abstraction (board-neutral API).
 *
 *  Owns the real-time clock. On the S3-2.06 that's a battery-backed PCF85063
 *  (SensorLib) that keeps perfect time across reboots, deep sleep, and PMU
 *  power-off. On a board with NO external RTC chip the stub backs onto the
 *  ESP32's internal RTC (the libc system clock), which survives deep sleep but
 *  NOT a true power-off — so such a board must re-sync from NTP / the phone on
 *  every cold boot (board_clock_persists() reports which kind you have).
 *
 *  Interchange type stays RTC_DateTime (a plain y/mo/d/h/mi/s field struct from
 *  SensorLib) so watchface_update() and the epoch helpers are unchanged. Every
 *  consumer goes through board_clock_now() / board_clock_set() — no file else
 *  touches the `rtc` object.
 *
 *  I2C LOCKING IS THE CALLER'S JOB (same rule as board_power.h): the PCF85063
 *  shares the touch/PMU bus, so callers wrap board_clock_now()/_set() in
 *  i2c_lock()/i2c_unlock() exactly as they wrapped rtc.getDateTime() before.
 *  (On a chip-less board the calls don't touch I2C, but callers still lock —
 *  harmless.)
 * ========================================================================== */
#pragma once
#include "time.h"

#if BOARD_HAS_RTC_PCF85063
#include "SensorPCF85063.hpp"

#define BOARD_RTC_I2C_ADDR  0x51   // PCF85063 (sleep_power.h's bus-alive probe)

static SensorPCF85063 rtc;        // module-private — use the accessors below

/* External battery-backed RTC: time is kept across a true power-off. */
static inline bool board_clock_persists(void) { return true; }

/* Bring the RTC up. Fatal-on-failure is the CALLER's policy (the .ino still
 * halts if this returns false), so just report it here. */
static bool board_clock_begin(void) {
  return rtc.begin(Wire, IIC_SDA, IIC_SCL);
}

/* Read the current date/time. Caller holds the I2C lock. */
static RTC_DateTime board_clock_now(void) {
  return rtc.getDateTime();
}

/* Write the wall-clock time. Caller holds the I2C lock. */
static void board_clock_set(uint16_t year, uint8_t mon, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second) {
  rtc.setDateTime(year, mon, day, hour, minute, second);
}

#else  /* !BOARD_HAS_RTC_PCF85063 ------------------------------------------ */

/* No external RTC chip — back onto the ESP32 internal RTC (libc system clock).
 * RTC_DateTime is still the SensorLib field struct (it comes in via the PMU
 * headers); we just fill/read it through time.h. Survives deep sleep, NOT a
 * cold power-off, so the firmware re-syncs from NTP / the phone on a cold boot. */
#include "SensorPCF85063.hpp"   // for the RTC_DateTime struct definition only

/* No RTC on I2C — the value is unused (the rail-probe bus check that reads it is
 * PMU-gated, and a chip-less board has no rails), but sleep_power.h references
 * the macro, so give it a harmless placeholder. */
#define BOARD_RTC_I2C_ADDR  0x00

static inline bool board_clock_persists(void) { return false; }
static bool board_clock_begin(void) { return true; }   // nothing to init

static RTC_DateTime board_clock_now(void) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return RTC_DateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec);
}

static void board_clock_set(uint16_t year, uint8_t mon, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second) {
  struct tm t = {};
  t.tm_year = year - 1900; t.tm_mon = mon - 1; t.tm_mday = day;
  t.tm_hour = hour; t.tm_min = minute; t.tm_sec = second;
  t.tm_isdst = -1;
  time_t e = mktime(&t);
  struct timeval tv;
  tv.tv_sec = e;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

#endif  /* BOARD_HAS_RTC_PCF85063 */
