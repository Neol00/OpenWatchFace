/* ============================================================================
 *  tuya/compat/owf_tuya_cpu_measure.h — MEASURE the real CPU clock on the T5.
 *
 *  WHY THIS EXISTS (and why every register/counter shortcut failed):
 *    - Decoding the clock-div register (CLK_DIV_MODE1 @ 0x44010020) just returns the
 *      setpoint WE WROTE — it can't tell you what the PLL actually produced.
 *    - The SDK's bk_pm_*_cpu_freq_get() are stubs returning 0/the vote in this build.
 *    - DWT->CYCCNT (0xE0001004) reads a CONSTANT here: the Beken Star-MC1 core
 *      (ARMv8-M, components/cmsis/.../core_star.h — an M33-class core, NOT RISC-V)
 *      does not run CYCCNT off the live core clock, and the SDK's own run-time-stats
 *      use bk_get_tick(), never CYCCNT. So CYCCNT is useless for this.
 *
 *  WHAT THIS DOES INSTEAD — times real CPU work against a physical reference:
 *    Execute a hand-written assembly loop with a FIXED, known cycle count per
 *    iteration, and measure how long a fixed number of iterations takes using
 *    bk_aon_rtc_get_us(). The AON-RTC is driven by the 32768 Hz crystal — a physical
 *    constant, completely independent of the CPU PLL — so this is an ABSOLUTE reading
 *    anchored to wall-clock time, NOT to any assumed CPU setpoint. It reports what the
 *    register actually produced.
 *
 *        core_Hz = (iterations * CYCLES_PER_ITER) / seconds_elapsed
 *
 *    The loop body is inline asm (`subs; bne`) so the compiler cannot change its
 *    instruction count. On Cortex-M33 / Star-MC1 a taken `bne` is 2 cycles and `subs`
 *    is 1 -> 3 cycles/iteration when the branch is taken (the dominant case). This is
 *    the one constant the measurement depends on; see OWF_T5_CYCLES_PER_ITER below.
 *
 *  ACCURACY: the AON-RTC has ~+/-30us jitter; with a ~30 ms window that's ~0.1%.
 *  CAVEAT: this is a BLOCKING busy-loop (~30 ms) AND assumes the loop runs
 *  uninterrupted — an ISR firing mid-window inflates the time and reads LOW. Call it
 *  from a settings/diagnostics screen, take the of a few runs (we keep the fastest =
 *  least-interrupted), and cache the result. Not for the render loop.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build.
 * ========================================================================== */
#pragma once
#include <stdint.h>
#include "owf_tuya_soc_stats.h"   // s_owf_tuya_meas_mhz cache + decode fallback

extern "C" {
/* AON-RTC microsecond counter — 32768 Hz-crystal domain, independent of the core PLL. */
uint64_t bk_aon_rtc_get_us(void);
}

/* Cycles per iteration of the asm loop below. On ARMv8-M (Cortex-M33 / Beken Star-MC1)
 * the loop is `subs rN,rN,#1` (1 cyc) + `bne` (2 cyc when taken) = 3 cycles. The branch
 * is taken on every iteration except the last, so 3 is exact to within one iteration
 * over millions — negligible. If a future measurement reads consistently high/low by a
 * fixed ratio, this constant is where to correct it. */
#define OWF_T5_CYCLES_PER_ITER 3u

/* Busy-loop `iters` times in fixed-cost assembly. `volatile`/clobber stop the compiler
 * from optimising it away or hoisting it. Returns nothing; timed by the caller. */
static inline void owf_tuya_spin(uint32_t iters) {
  __asm__ volatile(
    "1:\n"
    "   subs %0, %0, #1\n"
    "   bne  1b\n"
    : "+r"(iters)   /* in/out: counted down to 0 */
    :
    : "cc");
}

/* One raw measurement: time OWF_T5_SPIN_ITERS loop iterations against the AON-RTC and
 * convert to core MHz using the known cycles-per-iteration. */
#define OWF_T5_SPIN_ITERS 2000000u   /* ~12.5 ms at 480 MHz, ~50 ms at 120 MHz */
static inline uint32_t owf_tuya_measure_cpu_mhz_once(void) {
  uint64_t t0 = bk_aon_rtc_get_us();
  owf_tuya_spin(OWF_T5_SPIN_ITERS);
  uint64_t t1 = bk_aon_rtc_get_us();
  uint64_t dus = t1 - t0;
  if (dus == 0) return 0;
  /* cycles = iters * cyc/iter ; MHz = cycles / microseconds. */
  uint64_t cycles = (uint64_t)OWF_T5_SPIN_ITERS * OWF_T5_CYCLES_PER_ITER;
  return (uint32_t)((cycles + dus / 2) / dus);
}

/* Measure the real CPU clock in MHz. Runs the loop a few times and keeps the FASTEST
 * (highest MHz) result: an ISR stealing cycles only ever makes a run look slower, so
 * the max is the least-interrupted, most-accurate sample. Updates the live cache that
 * the cheap getters (getCpuFrequencyMhz / Power app) read. Blocks ~40-150 ms total. */
static inline uint32_t owf_tuya_measure_cpu_mhz(void) {
  uint32_t best = 0;
  for (int i = 0; i < 3; i++) {
    uint32_t m = owf_tuya_measure_cpu_mhz_once();
    if (m > best) best = m;
  }
  if (best) s_owf_tuya_meas_mhz = (uint16_t)best;
  return best;
}
