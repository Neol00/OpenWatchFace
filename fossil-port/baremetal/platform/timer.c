/* timer.c — ARMv7 architected timer, polled reads (CNTVCT).
 * Gen 4: 19.2 MHz XO. QEMU virt: 62.5 MHz. Always trust CNTFRQ when nonzero. */
#include "platform.h"

uint32_t timer_freq_hz(void)
{
    uint32_t f;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f)); /* CNTFRQ */
    return f ? f : PLAT_TIMER_HZ;
}

uint64_t timer_ticks(void)
{
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi)); /* CNTVCT */
    return ((uint64_t)hi << 32) | lo;
}

uint32_t timer_ms(void)
{
    return (uint32_t)(timer_ticks() / (timer_freq_hz() / 1000u));
}

/* Free-running microseconds, wrapping at 2^32 (~71 min). Deltas are what the
 * census counters use, and those are always far shorter than the wrap. */
uint32_t timer_us32(void)
{
    uint32_t per_us = timer_freq_hz() / 1000000u;
    return per_us ? (uint32_t)(timer_ticks() / per_us) : 0u;
}

void timer_delay_ms(uint32_t ms)
{
    uint64_t end = timer_ticks() + (uint64_t)ms * (timer_freq_hz() / 1000u);
    /* WFI, not a spin (the 100%-duty bug — see irq.c): once the 1 kHz tick is
     * live an interrupt arrives within 1 ms, so sleeping here costs at most
     * one tick of overshoot on a millisecond-granularity delay. Before the
     * tick is armed (early boot, IRQs masked) WFI could sleep forever — spin
     * with yield exactly as before. */
    while (timer_ticks() < end) {
        if (g_tick_armed) __asm__ volatile("wfi");
        else              __asm__ volatile("yield");
    }
}
