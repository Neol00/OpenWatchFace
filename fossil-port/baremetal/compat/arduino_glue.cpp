/* arduino_glue.cpp — implement the Arduino core wiring functions (declared by
 * ArduinoCore-API's Common.h) on top of the fossil-port bare-metal runtime.
 *
 * Timing binds to the runtime's arch-timer (timer.c). GPIO/analog are safe
 * no-op stubs for now: the Fossil has no exposed Arduino-style GPIO, and every
 * board feature that would drive a pin (BACKLIGHT_PWM, AUDIO_PWM, nav buttons,
 * power latch) is OFF in board_fossil_gen6.h, so these are only here to satisfy
 * the linker for any guarded call that slips through. Real pin control, where a
 * peripheral needs it, goes through the runtime's own drivers, not this shim.
 */
#include "Arduino.h"

/* fossil-port runtime (platform.h is C) */
extern "C" {
    uint32_t timer_ms(void);
    uint32_t timer_us32(void);   /* real elapsed sleep accounting, see delay() */
    uint64_t timer_ticks(void);
    uint32_t timer_freq_hz(void);
    void     timer_delay_ms(uint32_t ms);
    void     vib_set(int on);
    int      pon_kpdpwr_pressed(void);
    int      pon_resin_pressed(void);
}

/* Virtual pins routed to PMIC drivers — must match the definitions in
 * OpenWatchFace/board_fossil_gen6.h. haptics.h drives the motor exclusively
 * through digitalWrite(HAPTICS_MOTOR_GPIO, ...), and the .ino polls its BOOT
 * button with digitalRead(BOOT_BTN_GPIO) (active-LOW), so these hooks give the
 * firmware real vibration + a real button with zero changes to its logic. */
#define FOSSIL_PIN_VIB     200
#define FOSSIL_PIN_KPDPWR  201   /* power/crown button, reads LOW when held */
#define FOSSIL_PIN_RESIN   202   /* second pusher, reads LOW when held */

extern "C" unsigned long millis(void) { return timer_ms(); }

extern "C" unsigned long micros(void)
{
    /* ticks -> microseconds, full-precision (freq is 19.2 MHz, not a µs multiple) */
    return (unsigned long)((timer_ticks() * 1000000ULL) / timer_freq_hz());
}

/* Sleep-time accounting for the PWR census: time spent in delay() inside the
 * loop body is idle, not CPU work — pwr_diag.c subtracts it from the load. */
extern "C" { volatile uint32_t g_pwr_sleep_ms; }
/* CREDIT WHAT WE ACTUALLY SLEPT, NOT WHAT WAS ASKED FOR (2026-08-07).
 *
 * This booked the REQUESTED ms and then slept longer than that, and the whole
 * difference was reported as CPU load. timer_delay_ms() WFIs until its
 * deadline, but with the 1 kHz tick the core only wakes on tick boundaries, so
 * the real block is uniform in [ms, ms+1) — mean ms+0.5. pwr_diag computes
 * cpu = body - slp - fbw from a body measured in REAL time, so every call
 * donated ~0.5 ms to "compute". At the measured 138 loops/s that is ~7% of
 * wall time of pure phantom load, which is most of the steady 11-12% the
 * census reported at idle and never varied with UI activity.
 *
 * Measure in microseconds with a carried remainder: rounding each call to
 * whole ms would reintroduce a systematic bias in the same direction. */
extern "C" void delay(unsigned long ms)
{
    static uint32_t frac_us;
    uint32_t t0 = timer_us32();
    timer_delay_ms((uint32_t)ms);
    frac_us += timer_us32() - t0;
    g_pwr_sleep_ms += frac_us / 1000u;
    frac_us %= 1000u;
}

extern "C" void delayMicroseconds(unsigned int us)
{
    uint32_t f = timer_freq_hz();
    uint64_t start = timer_ticks();
    uint64_t want  = ((uint64_t)us * f) / 1000000ULL;
    while ((timer_ticks() - start) < want) { /* busy-wait */ }
}

extern "C" void yield(void) { /* cooperative yield lands here once tasks drive it */ }

/* ---- GPIO / analog: no Arduino-style GPIO on this SoC yet -> no-op stubs ---- */
extern "C" void      pinMode(pin_size_t, PinMode)        { }
extern "C" void      digitalWrite(pin_size_t pin, PinStatus val)
{
    if (pin == FOSSIL_PIN_VIB) vib_set(val == HIGH);
}
extern "C" PinStatus digitalRead(pin_size_t pin)
{
    /* Buttons present active-LOW (the .ino treats LOW as pressed, matching an
     * ESP BOOT pin with a pull-up). SPMI errors read as released (HIGH). */
    if (pin == FOSSIL_PIN_KPDPWR) return pon_kpdpwr_pressed() == 1 ? LOW : HIGH;
    if (pin == FOSSIL_PIN_RESIN)  return pon_resin_pressed()  == 1 ? LOW : HIGH;
    return LOW;
}
extern "C" int       analogRead(pin_size_t)              { return 0; }
extern "C" void      analogReference(uint8_t)            { }
extern "C" void      analogWrite(pin_size_t, int)        { }

extern "C" void init(void)        { }
extern "C" void initVariant(void) { }

/* ---- WMath: map()/makeWord() come from ArduinoCore-API's Common.cpp; only
 * random()/randomSeed() need providing (Common.cpp omits them). ------------- */
long random(long howbig)             { return howbig ? 0 : 0; }   /* deterministic stub */
long random(long howsmall, long)     { return howsmall; }
void randomSeed(unsigned long)       { }
