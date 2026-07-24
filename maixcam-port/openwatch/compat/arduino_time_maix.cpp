/* ============================================================================
 *  compat/arduino_time_maix.cpp — the Arduino time functions, backed by MaixCDK.
 *
 *  This is the ONLY compat TU that includes MaixCDK headers. It deliberately does
 *  NOT include compat/Arduino.h: MaixCDK headers pull in `using namespace std`
 *  (so std::map is visible unqualified), and Arduino.h's free `long map(...)`
 *  declaration then misparses as a std::map deduction guide on the device's older
 *  GCC. Keeping the two apart sidesteps that entirely. The signatures here match
 *  the declarations in Arduino.h exactly (same global C++ linkage).
 * ========================================================================== */
#include <cstdint>
#include "maix_basic.hpp"   // maix::time
#include "maix_lvgl.hpp"    // maix::maix_display (set by lvgl_init)
#include "maix_pmu.hpp"     // maix::ext_dev::pmu::PMU (on-board AXP2101)
#include "owf_maix_hooks.h"

using namespace maix;

/* Panel brightness bridge: the firmware (board_display_set_brightness) calls this
 * with 0..100; route it to the MaixCDK display backlight. */
void owf_maix_set_backlight(int pct_0_100)
{
    if (!maix_display) return;
    if (pct_0_100 < 0)   pct_0_100 = 0;
    if (pct_0_100 > 100) pct_0_100 = 100;
    maix_display->set_backlight((float)pct_0_100);
}

/* ---- AXP2101 PMU bridge -------------------------------------------------- */
static ext_dev::pmu::PMU *s_pmu = nullptr;
static bool               s_pmu_tried = false;

static ext_dev::pmu::PMU *owf_pmu()
{
    if (!s_pmu && !s_pmu_tried) {
        s_pmu_tried = true;
        try { s_pmu = new ext_dev::pmu::PMU(); }   // defaults: axp2101, on-board i2c, 0x34
        catch (...) { s_pmu = nullptr; }
    }
    return s_pmu;
}

int owf_maix_pmu_begin(void)   { return owf_pmu() ? 1 : 0; }
int owf_maix_pmu_ok(void)      { return s_pmu ? 1 : 0; }
int owf_maix_bat_percent(void) { auto p = owf_pmu(); return p ? p->get_bat_percent() : -1; }
int owf_maix_bat_mv(void)      { auto p = owf_pmu(); return p ? (int)p->get_bat_vol() : 0; }
int owf_maix_is_charging(void) { auto p = owf_pmu(); return (p && p->is_charging()) ? 1 : 0; }
int owf_maix_vbus_in(void)     { auto p = owf_pmu(); return (p && p->is_vbus_in()) ? 1 : 0; }

uint32_t millis(void)                    { return (uint32_t)time::ticks_ms(); }
uint32_t micros(void)                    { return (uint32_t)time::ticks_us(); }
void     delay(uint32_t ms)              { time::sleep_ms(ms); }
void     delayMicroseconds(uint32_t us)  { time::sleep_us(us); }
void     yield(void)                     { time::sleep_us(0); }
