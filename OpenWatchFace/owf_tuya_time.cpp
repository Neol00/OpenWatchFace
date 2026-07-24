/* ============================================================================
 *  owf_tuya_time.cpp - provide settimeofday() on the Tuya T5.
 *
 *  The firmware sets the wall clock via settimeofday() (board_clock.h). newlib
 *  DECLARES it (<sys/time.h>) but the T5 platform only implements the READ side
 *  (gettimeofday / time(), backed by the BK7258 AON RTC); the std-named
 *  settimeofday() is an undefined reference at link.
 *
 *  CRITICAL: the firmware READS time via time()/gettimeofday() (board_clock_now),
 *  which the platform backs with the AON RTC (bk_rtc_gettimeofday). So our
 *  settimeofday() MUST write that SAME AON RTC - otherwise reads and writes hit
 *  different clocks and the displayed time never matches what we set. The platform
 *  exposes the AON-RTC writer as bk_rtc_settimeofday(struct timeval*, ...). We
 *  forward to it (declared here to avoid pulling the deep t5_os driver headers; the
 *  symbol resolves from the platform lib at link). The AON RTC is persistent, so the
 *  clock survives reboots / deep-sleep-through-reset.
 *
 *  A .cpp (not a header) so this emits a single linkable definition. Gated on
 *  BOARD_PLATFORM_TUYA so other targets are untouched.
 * ========================================================================== */
#include "board.h"

#if BOARD_PLATFORM_TUYA

#include <sys/time.h>

/* The BK7258 AON-RTC writer (same clock gettimeofday/time() read). Declared here
 * rather than via <driver/aon_rtc.h> (deep in t5_os, off the sketch include path);
 * the definition links from the platform library. bk_err_t is an int. */
extern "C" int bk_rtc_settimeofday(const struct timeval *tv, const struct timezone *tz);

extern "C" int settimeofday(const struct timeval *tv, const struct timezone *tz) {
    if (!tv) return -1;
    return bk_rtc_settimeofday(tv, tz) == 0 ? 0 : -1;
}

#endif /* BOARD_PLATFORM_TUYA */
