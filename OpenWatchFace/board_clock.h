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

#define BOARD_RTC_I2C_ADDR  0x51   // PCF85063 (sleep_power.h's bus-alive probe)

#if BOARD_PLATFORM_TUYA
/* ---- Direct PCF85063 driver over Wire (NO SensorLib) -----------------------
 * SensorLib can't be used on the Tuya build: pulling in any SensorLib header makes
 * the Arduino builder compile the WHOLE src/ tree, including Bosch drivers that
 * #include <Stream.h> (absent as a top-level header on the Tuya core) -> build fails.
 * The PCF85063 is a trivial BCD-register I2C RTC, so we talk to it directly over the
 * (real, tkl_i2c-backed) Wire. Registers: 0x04 sec, 05 min, 06 hour, 07 day, 08 weekday,
 * 09 month, 0A year (00-99 = 2000-2099). 0x00 Control_1 (bit5 STOP). Times are BCD; the
 * seconds register's bit7 is the oscillator-stop / low-voltage flag (time invalid if set).
 *
 * Minimal RTC_DateTime (same shape SensorLib provides: y/mo/d/h/mi/s + getWeek()). The
 * watch face + epoch helpers only use the getters, so this is a drop-in. */
#include <Wire.h>
class RTC_DateTime {
  uint16_t y; uint8_t mo, d, h, mi, s;
public:
  RTC_DateTime() : y(1970), mo(1), d(1), h(0), mi(0), s(0) {}
  RTC_DateTime(uint16_t year, uint8_t mon, uint8_t day,
               uint8_t hour, uint8_t minute, uint8_t second)
      : y(year), mo(mon), d(day), h(hour), mi(minute), s(second) {}
  uint16_t getYear()   const { return y;  }
  uint8_t  getMonth()  const { return mo; }
  uint8_t  getDay()    const { return d;  }
  uint8_t  getHour()   const { return h;  }
  uint8_t  getMinute() const { return mi; }
  uint8_t  getSecond() const { return s;  }
  uint8_t  getWeek() const {                       // 0=Sun..6=Sat (Sakamoto)
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = y - (mo < 3 ? 1 : 0);
    return (uint8_t)((yy + yy/4 - yy/100 + yy/400 + t[mo - 1] + d) % 7);
  }
};

static inline uint8_t owf_bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t owf_dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static inline bool board_clock_persists(void) { return true; }

/* Distinguish "the I2C read FAILED" from "the chip answered and says its time is unset":
 * both come back as the 1970 sentinel from board_clock_now(), but only the second may be
 * repaired by stamping a default — stamping after a mere read failure DESTROYS the good
 * time the chip kept (e.g. through a deep sleep, where the rail never drops). Set by every
 * board_clock_now() call; consumed by the boot-time clock block in the .ino. */
static bool s_owf_rtc_read_failed = false;
static inline bool board_clock_read_failed(void) { return s_owf_rtc_read_failed; }

/* Bus recovery: the deep-sleep entry's PM pad shutdown tri-states SCL/SDA at an arbitrary
 * bit boundary, which can leave a slave on the shared bus (touch/IMU/RTC) mid-transaction
 * holding SDA low — every transfer then fails. Classic fix: clock SCL until the slave
 * releases SDA (<=9 pulses), issue a manual STOP, then re-take the pins as I2C. */
static void owf_t5_i2c_bus_recover(void) {
  Wire.end();
  pinMode(IIC_SCL, OUTPUT);
  digitalWrite(IIC_SCL, HIGH);
  pinMode(IIC_SDA, INPUT_PULLUP);                  // observe SDA (external 4.7k pulls exist)
  for (int i = 0; i < 9 && digitalRead(IIC_SDA) == LOW; i++) {
    digitalWrite(IIC_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
  }
  pinMode(IIC_SDA, OUTPUT);                        // manual STOP: SDA low->high, SCL high
  digitalWrite(IIC_SDA, LOW);  delayMicroseconds(5);
  digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
  digitalWrite(IIC_SDA, HIGH); delayMicroseconds(5);
  OWF_WIRE_BEGIN(IIC_SDA, IIC_SCL);
}

/* Probe the chip: read Control_1. Returns true if it ACKs on the bus. */
static bool board_clock_begin(void) {
  Wire.beginTransmission(BOARD_RTC_I2C_ADDR);
  Wire.write(0x00);                                // point at Control_1
  if (Wire.endTransmission() != 0) return false;   // no ACK -> chip absent/bus down
  return true;
}

/* Read date/time from registers 0x04..0x0A. Caller holds the I2C lock. */
static RTC_DateTime board_clock_now(void) {
  s_owf_rtc_read_failed = true;                    // cleared below once the transfer succeeds
  Wire.beginTransmission(BOARD_RTC_I2C_ADDR);
  Wire.write(0x04);                                // point at the seconds register
  if (Wire.endTransmission() != 0) return RTC_DateTime();        // STOP, then a fresh read
  if (Wire.requestFrom((uint8_t)BOARD_RTC_I2C_ADDR, (size_t)7) != 7) return RTC_DateTime();
  s_owf_rtc_read_failed = false;                   // chip answered; the value is authoritative
  uint8_t sec  = (uint8_t)Wire.read();
  uint8_t mint = (uint8_t)Wire.read();
  uint8_t hour = (uint8_t)Wire.read();
  uint8_t day  = (uint8_t)Wire.read();
  (void)Wire.read();                               // weekday (we recompute via getWeek)
  uint8_t mon  = (uint8_t)Wire.read();
  uint8_t yr   = (uint8_t)Wire.read();
  // sec bit7 = OS (oscillator stopped / low-voltage) -> time invalid; report 1970 so the
  // .ino's "year < 2024" check re-initializes / NTP-syncs it. This is the CHIP saying its
  // supply really dropped — log it once per boot to distinguish from a failed I2C read.
  if (sec & 0x80) {
    static bool s_os_logged = false;
    if (!s_os_logged) { s_os_logged = true;
      USBSerial.println("[rtc] PCF85063 OS flag set - chip really lost power (time invalid)"); }
    return RTC_DateTime();
  }
  return RTC_DateTime((uint16_t)2000 + owf_bcd2dec(yr),
                      owf_bcd2dec(mon & 0x1F), owf_bcd2dec(day & 0x3F),
                      owf_bcd2dec(hour & 0x3F), owf_bcd2dec(mint & 0x7F),
                      owf_bcd2dec(sec & 0x7F));
}

/* Write date/time to registers 0x04..0x0A. Caller holds the I2C lock. Writing the
 * seconds register with bit7=0 also CLEARS the oscillator-stop flag (marks time valid). */
static void board_clock_set(uint16_t year, uint8_t mon, uint8_t day,
                            uint8_t hour, uint8_t minute, uint8_t second) {
  if (year < 2000) year = 2000;
  RTC_DateTime tmp(year, mon, day, hour, minute, second);  // for the weekday byte
  Wire.beginTransmission(BOARD_RTC_I2C_ADDR);
  Wire.write(0x04);                                // start at seconds
  Wire.write(owf_dec2bcd(second) & 0x7F);          // bit7=0 -> clears OS flag
  Wire.write(owf_dec2bcd(minute));
  Wire.write(owf_dec2bcd(hour));
  Wire.write(owf_dec2bcd(day));
  Wire.write(tmp.getWeek());                       // weekday 0..6
  Wire.write(owf_dec2bcd(mon));
  Wire.write(owf_dec2bcd((uint8_t)(year - 2000)));
  Wire.endTransmission();
}

static inline void board_clock_persist_save(void) {}
static inline void board_clock_persist_restore(void) {}

#else  /* non-Tuya RTC boards: SensorLib as before --------------------------- */
#include "SensorPCF85063.hpp"

/* Read-validity probe is a T5-only concept (SensorLib reads don't report transport
 * failure); report "never failed" so shared callers compile everywhere. */
static inline bool board_clock_read_failed(void) { return false; }

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

/* This board has a battery-backed RTC chip, so it keeps time through reset/power-off
 * on its own — the NVS persist/restore is a no-op here. */
static inline void board_clock_persist_save(void) {}
static inline void board_clock_persist_restore(void) {}
#endif  /* BOARD_PLATFORM_TUYA */

#else  /* !BOARD_HAS_RTC_PCF85063 ------------------------------------------ */

/* No external RTC chip — back onto the ESP32 internal RTC (libc system clock).
 * We just fill/read RTC_DateTime through time.h. Survives deep sleep, NOT a cold
 * power-off, so the firmware re-syncs from NTP / the phone on a cold boot.
 *
 * Where RTC_DateTime comes from:
 *   - On a board WITH the AXP2101 PMU, the XPowersLib headers already define it
 *     (SensorLib's struct, pulled in via board_power.h) — use that.
 *   - On a board with NEITHER PMU nor RTC chip (e.g. the C6-1.47), SensorLib is
 *     not even installed, so we define a minimal compatible struct here. It only
 *     needs the (y,mo,d,h,mi,s) constructor + getXxx() accessors the firmware uses. */
#if BOARD_HAS_PMU_AXP2101
#include "SensorPCF85063.hpp"   // RTC_DateTime comes from SensorLib (also via XPowersLib)
#else
class RTC_DateTime {
  uint16_t y; uint8_t mo, d, h, mi, s;
public:
  RTC_DateTime() : y(1970), mo(1), d(1), h(0), mi(0), s(0) {}
  RTC_DateTime(uint16_t year, uint8_t mon, uint8_t day,
               uint8_t hour, uint8_t minute, uint8_t second)
      : y(year), mo(mon), d(day), h(hour), mi(minute), s(second) {}
  uint16_t getYear()   const { return y;  }
  uint8_t  getMonth()  const { return mo; }
  uint8_t  getDay()    const { return d;  }
  uint8_t  getHour()   const { return h;  }
  uint8_t  getMinute() const { return mi; }
  uint8_t  getSecond() const { return s;  }
  /* Day of week, 0=Sun..6=Sat (Sakamoto's algorithm) — matches SensorLib's getWeek(). */
  uint8_t  getWeek() const {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = y - (mo < 3 ? 1 : 0);
    return (uint8_t)((yy + yy/4 - yy/100 + yy/400 + t[mo - 1] + d) % 7);
  }
};
#endif

/* No RTC on I2C — the value is unused (the rail-probe bus check that reads it is
 * PMU-gated, and a chip-less board has no rails), but sleep_power.h references
 * the macro, so give it a harmless placeholder. */
#define BOARD_RTC_I2C_ADDR  0x00

static inline bool board_clock_persists(void) { return false; }

/* No I2C RTC — reads can't "fail"; see the T5 branch for what this means there. */
static inline bool board_clock_read_failed(void) { return false; }

static bool board_clock_begin(void) {
  // Set the LOCAL timezone at boot so localtime()/mktime() apply the right offset
  // from the very first clock read — independent of NTP. Without this the TZ env is
  // UTC until an NTP sync runs, so the restored/displayed time was off by the whole
  // UTC offset (e.g. 2h behind for CEST) on every wake. NTP_TZ comes from the .ino
  // (defined before this header is included), so changing it there is honored here too.
#ifdef NTP_TZ
  setenv("TZ", NTP_TZ, 1);
  tzset();
#endif
  return true;
}

static RTC_DateTime board_clock_now(void) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return RTC_DateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec);
}

/* ---- NVS time persistence (boards with no battery-backed RTC, e.g. the C6) ----
 * The internal RTC dies on the RST button — which is the ONLY way the C6 wakes from
 * deep sleep — so without this the clock resets to 1970 on every wake. We snapshot
 * the wall-clock epoch to NVS (flash, survives reset/power-off) and restore it on
 * boot. NVS can't tick while asleep, so the restored time is STALE by the sleep
 * duration; NTP (WiFi) or the phone (Gadgetbridge) then corrects it within seconds
 * of waking. Uses its OWN Preferences handle so it works regardless of include order
 * (board_clock.h is included before settings_store.h, which owns the shared `prefs`).
 *
 * To narrow the staleness we ALSO add the elapsed sleep time when we can: the RTC
 * microsecond counter (esp_timer / gettimeofday's monotonic base) is reset by RST,
 * so we instead bump the saved epoch by the known sleep interval at save time isn't
 * possible — so restore is best-effort "as of last save", corrected by NTP. */
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/Preferences.h"
#else
#include <Preferences.h>
#endif
static void board_clock_persist_save(void) {
  time_t now = time(nullptr);
  if (now < 1700000000) return;            // clock not valid yet (pre-2023) -> don't save garbage
  Preferences p;
  if (!p.begin("clk", false)) return;
  p.putULong64("epoch", (uint64_t)now);
  p.end();
}
static void board_clock_persist_restore(void) {
  // IMPORTANT: only restore the (stale) snapshot when the LIVE clock is invalid.
  // On a deep-sleep TIMER wake the RTC kept counting, so the kernel clock is already
  // correct AND has advanced through sleep — overwriting it with the older NVS value
  // would move time BACKWARD (the "woke after 30 min but still shows the old time"
  // bug). Only an RST/power-on reset wipes the clock to 1970; restore the snapshot
  // there (stale by the sleep duration — unavoidable, NTP/phone then corrects it).
  time_t live = time(nullptr);
  if (live >= 1700000000) return;          // live clock already valid -> keep it (timer wake)

  Preferences p;
  if (!p.begin("clk", true)) return;       // read-only
  uint64_t saved = p.getULong64("epoch", 0);
  p.end();
  if (saved < 1700000000ULL) return;       // nothing valid saved
  struct timeval tv = { (time_t)saved, 0 };
  settimeofday(&tv, nullptr);              // restore "time as of last save"; NTP/phone refines it
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
