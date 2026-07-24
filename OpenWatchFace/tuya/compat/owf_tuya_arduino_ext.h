/* ============================================================================
 *  tuya/compat/owf_tuya_arduino_ext.h - ESP32-Arduino extras the firmware uses that
 *  the TuyaOpen core's <Arduino.h> doesn't provide.
 *
 *  On the Maix port these live in a REPLACEMENT <Arduino.h>; on the Tuya Arduino-IDE
 *  build <Arduino.h> resolves to the real TuyaOpen core, so we supply the extras in
 *  this supplemental header included ONCE early in the .ino (after board.h, before any
 *  firmware header that uses them).
 *
 *    RTC_DATA_ATTR      - ESP attribute placing a var in deep-sleep-retained RTC RAM.
 *                         The T5 wakes through reset with no retained RAM, so this is
 *                         empty: the vars become plain statics (their deep-sleep
 *                         persistence is lost - acceptable for now; values that must
 *                         survive deep sleep go through NVS/Preferences instead).
 *    settimeofday()     - from newlib <sys/time.h> (board_clock.h sets the system clock).
 *    temperatureRead()  - ESP-S3 die temp sensor; no T5 equivalent here -> returns NAN.
 *    setCpuFrequencyMhz / getCpuFrequencyMhz - ESP DVFS; T5 freq isn't tuned here.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_TUYA

#include <cstdint>
#include <cmath>        // NAN
#include <cstdlib>      // setenv
#include <ctime>        // localtime_r, tzset
#include <sys/time.h>   // settimeofday / gettimeofday (newlib)
#include "owf_tuya_soc_stats.h"   // owf_tuya_cpu1_mhz() — live clock for getCpuFrequencyMhz()

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_memory.h"   // free-heap / psram sizes
#include "tkl_system.h"   // tkl_system_reset
}

#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR   /* no deep-sleep-retained RAM on the T5 (wakes through reset) */
#endif

/* ESP-S3 built-in die temperature sensor. No equivalent exposed on the T5 here; the
 * Power app guards on isnan(), so NAN reads as "unavailable" rather than a bogus value. */
static inline float temperatureRead(void) { return NAN; }

/* CPU frequency. getCpuFrequencyMhz() reports the LIVE cpu1 (fast app core) clock read
 * from the SoC clock register (owf_tuya_soc_stats.h) — the real current rate, so the
 * firmware's "did the clock change?" checks work. The SETTER stays a no-op here: on the
 * T5 the clock is changed by a PM vote (owf_tuya_set_cpu_mhz in owf_tuya_cpu_freq.h),
 * which settings_clock_hw() calls directly — NOT through this ESP-style entry point. */
static inline uint32_t getCpuFrequencyMhz(void)     { return owf_tuya_cpu1_mhz(); }
static inline bool     setCpuFrequencyMhz(uint32_t) { return false; }

/* ---- ESP global object (Settings > About reads chip/heap/flash info) ------
 * Backed by real T5 values where we have them (16 MB PSRAM, tkl heap), plausible
 * constants elsewhere. */
#define ESP_ARDUINO_VERSION_MAJOR 3
#define ESP_ARDUINO_VERSION_MINOR 3
#define ESP_ARDUINO_VERSION_PATCH 11

class EspClass {
public:
    const char *getChipModel()     { return "BK7258"; }
    uint8_t     getChipRevision()  { return 1; }
    uint8_t     getChipCores()     { return 3; }       /* app SMP domain */
    uint32_t    getCpuFreqMHz()    { return 480; }
    uint32_t    getFreeHeap()      { return (uint32_t)tkl_system_get_free_heap_size(); }
    uint32_t    getHeapSize()      { return 0; }
    uint32_t    getMinFreeHeap()   { return (uint32_t)tkl_system_get_free_heap_size(); }
    uint32_t    getMaxAllocHeap()  { return (uint32_t)tkl_system_get_free_heap_size(); }
    uint32_t    getFreePsram()     { return (uint32_t)tkl_system_psram_get_free_heap_size(); }
    uint32_t    getPsramSize()     { return 16u * 1024 * 1024; }   /* 16 MB PSRAM */
    uint32_t    getFlashChipSize() { return 8u * 1024 * 1024; }    /* 8 MB on the T5-E1 (Waveshare 1.75) */
    uint32_t    getFlashChipSpeed(){ return 80u * 1000 * 1000; }
    uint64_t    getEfuseMac()      { return 0ULL; }    /* real per-unit MAC via esp_mac.h */
    void        restart()          { tkl_system_reset(); }
};
static EspClass ESP;

/* ---- NTP helpers (notif_net.h time sync) ---------------------------------
 * configTzTime sets the POSIX TZ; getLocalTime reads the current local time. The
 * ACTUAL SNTP sync over the network is a T5 SDK concern wired later - for now
 * getLocalTime returns whatever the system clock holds (set by board_clock /
 * settimeofday), so the firmware's "year >= 2024" sanity check governs validity. */
static inline void configTzTime(const char *tz, const char * = nullptr,
                                const char * = nullptr, const char * = nullptr) {
    if (tz) { setenv("TZ", tz, 1); tzset(); }
}
static inline bool getLocalTime(struct tm *info, uint32_t = 5000) {
    if (!info) return false;
    /* Read via gettimeofday - the platform backs it with the AON RTC, the SAME clock
     * our settimeofday() writes (owf_tuya_time.cpp), so reads and writes are consistent
     * with board_clock_now() which also uses time(). */
    struct timeval tv; gettimeofday(&tv, nullptr);
    time_t t = tv.tv_sec;
    localtime_r(&t, info);
    return (info->tm_year + 1900) >= 2024;   // false until the clock has been set
}

#endif /* BOARD_PLATFORM_TUYA */
