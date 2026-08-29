/* owf_time.cpp — a settable wall clock on the fossil-port monotonic timer,
 * seeded from (and written back to) the PMIC RTC.
 * newlib (nosys) has no settimeofday and a stub _gettimeofday; provide both so
 * settimeofday()/gettimeofday()/time()/localtime() work.
 *
 * The PMIC RTC is a battery-backed 32-bit seconds counter that the stock OS
 * keeps at UTC Unix time, so on first read it usually IS the wall clock. The
 * RAM offset stays the session's source of truth; the RTC is read once at
 * first use and written best-effort on settimeofday (the write path may be
 * secure-world-locked on production units — see pmic_rtc.c). */
#include <sys/time.h>
#include <cstdint>
extern "C" {
uint32_t timer_ms(void);
int rtc_read_epoch(uint32_t *sec);
int rtc_write_epoch(uint32_t sec);
}

static long long s_offset_us = 0;   /* epoch_us - uptime_us at the moment time was set */
static bool s_seeded = false;       /* tried the RTC yet? (once, lazily) */

/* Sanity window for an RTC seed: 2020-01-01 .. 2100-01-01. A never-programmed
 * RTC counts from 0 (1970) — that must not become the visible clock when the
 * firmware has a saved/synced time of its own to apply later. */
static bool plausible_epoch(uint32_t s) {
    return s >= 1577836800u && s < 4102444800u;
}

static void seed_from_rtc(void) {
    s_seeded = true;
    uint32_t sec;
    if (rtc_read_epoch(&sec) == 0 && plausible_epoch(sec)) {
        long long uptime = (long long)timer_ms() * 1000;
        s_offset_us = (long long)sec * 1000000LL - uptime;
    }
}

extern "C" int settimeofday(const struct timeval *tv, const struct timezone *) {
    if (tv) {
        s_seeded = true;            /* an explicit set beats any RTC seed */
        long long uptime = (long long)timer_ms() * 1000;
        s_offset_us = ((long long)tv->tv_sec * 1000000LL + tv->tv_usec) - uptime;
        if (plausible_epoch((uint32_t)tv->tv_sec))
            rtc_write_epoch((uint32_t)tv->tv_sec);   /* best-effort persist;
                never let an unset 1970 clock clobber the phone-synced RTC */
    }
    return 0;
}
extern "C" int _gettimeofday(struct timeval *tv, void *) {
    if (!s_seeded) seed_from_rtc();
    if (tv) {
        long long now = (long long)timer_ms() * 1000 + s_offset_us;
        tv->tv_sec  = (time_t)(now / 1000000);
        tv->tv_usec = (long)(now % 1000000);
    }
    return 0;
}
