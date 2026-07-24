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

void timer_delay_ms(uint32_t ms)
{
    uint64_t end = timer_ticks() + (uint64_t)ms * (timer_freq_hz() / 1000u);
    while (timer_ticks() < end) { __asm__ volatile("yield"); }
}
