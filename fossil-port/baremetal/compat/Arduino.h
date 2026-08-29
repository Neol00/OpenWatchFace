/* Arduino.h — platform Arduino header for the Fossil bare-metal port.
 *
 * The firmware is written against the Arduino API. Rather than hand-roll it, we
 * reuse the official platform-neutral ArduinoCore-API (String/Print/Stream/...
 * in namespace arduino, plus the core wiring-function DECLARATIONS in Common.h)
 * — the same base the Tuya T5 port uses. This header just:
 *   1. pulls in ArduinoCore-API (which also includes the C libc headers), and
 *   2. lifts the arduino:: classes into the global namespace, as every Arduino
 *      sketch expects (`String`, `Print`, `IPAddress`, ...).
 *
 * The core wiring functions declared in Common.h (millis/delay/pinMode/...) are
 * IMPLEMENTED in arduino_glue.cpp, bound to the fossil-port runtime.
 */
#pragma once

#include "ArduinoAPI.h"   /* api classes + <stdlib/string/math.h> + Common.h decls */
#include "plat_soc_tier.h"   /* PLAT_SOC_* — derived, not -D'd; see that header */

#ifdef __cplusplus
using namespace arduino;
#endif

/* ESP storage-class attributes the firmware annotates statics with. No deep-sleep
 * retention yet on this port, so RTC_* are plain (state resets on reboot). */
#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif

/* ESP32-hal CPU frequency + POSIX time the firmware expects from the core. */
#include <sys/time.h>
#ifdef __cplusplus
static inline bool     setCpuFrequencyMhz(uint32_t) { return true; }
static inline uint32_t getCpuFrequencyMhz(void)     { return 1200; }   /* A53 nominal */
static inline uint32_t getCpuFreqMHz(void)          { return 1200; }
#endif

/* ESP-Arduino globals the firmware references directly. */
#include <time.h>
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 3
#define ESP_ARDUINO_VERSION_MINOR 3
#define ESP_ARDUINO_VERSION_PATCH 0
#endif
#ifdef __cplusplus
extern "C" { void reboot_now(void); }
extern "C" int pwr_cpu_mhz(void);

class EspClass {
public:
    uint32_t getFreeHeap()       { return 8u*1024u*1024u; }
    uint32_t getMinFreeHeap()    { return 8u*1024u*1024u; }
    uint32_t getPsramSize()      { return 0; }
    uint32_t getFreePsram()      { return 0; }
    uint32_t getHeapSize()       { return 16u*1024u*1024u; }
    /* Per watch. SDA429W is the Gen 6's part; the Gen 4 is an msm8909w /
     * APQ8009w (Snapdragon Wear 2100), and reporting the wrong SoC on the
     * About screen is exactly the kind of thing that later gets quoted back
     * as fact. */
/* SoC tier, not board tier: the TicWatch C2 is the same APQ8009W as the
     * Gen 4, and while this tested the BOARD it reported the C2 as an SDA429W
     * — the Gen 6's part — on its own About screen. Exactly the "later gets
     * quoted back as fact" failure the comment above warns about. */
#if defined(PLAT_SOC_MSM8909)
    const char* getChipModel()   { return "APQ8009W"; }
#else
    const char* getChipModel()   { return "SDA429W"; }
#endif
    uint32_t getFlashChipSize()  { return 0; }
    uint32_t getFlashChipSpeed() { return 0; }
#if defined(PLAT_SOC_MSM8909)
    /* msm8909w A7 cluster. This USED to return the part's rated 1.2 GHz top
     * speed, with a comment explaining that we did not own the clocks — that
     * has not been true since cpu_clk_a7.c landed, and reporting a rate the
     * watch never runs at is worse than reporting none. Ask the clock driver
     * for the LIVE rate and fall back to the boot rate only if it cannot say.
     * (1.2 GHz is not even offered by the ladder: it needs the A7 PLL and a
     * higher voltage corner. See platform/cpu_clk_a7.c.) */
    uint32_t getCpuFreqMHz()     { int m = pwr_cpu_mhz(); return m > 0 ? (uint32_t)m : 800u; }
#else
    uint32_t getCpuFreqMHz()     { return 1200; }
#endif
    uint64_t getEfuseMac()       { return 0x0600574F0002ULL; }
    void restart()               { reboot_now(); }
};
static EspClass ESP;
/* SoC die temperature — real on the Gen 6 (hottest TSENS channel, validated
 * against the PWR census 2026-08-07); the old fake 25.0 elsewhere. */
extern "C" int pwr_soc_temp_dc(void);
static inline float temperatureRead(void) {
    int dc = pwr_soc_temp_dc();
    return (dc == -9999) ? 25.0f : (float)dc / 10.0f;
}
static inline bool getLocalTime(struct tm *info, uint32_t = 5000) {
    if (!info) return false;
    time_t now = time(nullptr);
    localtime_r(&now, info);
    return now > 1000000000;   /* "valid" once the clock is set past 2001 */
}
static inline void configTzTime(const char*, const char*, const char* = nullptr, const char* = nullptr) {}
static inline void configTime(long, int, const char*, const char* = nullptr, const char* = nullptr) {}
#endif
