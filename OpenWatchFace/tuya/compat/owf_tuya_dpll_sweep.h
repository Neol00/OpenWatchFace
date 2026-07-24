/* ============================================================================
 *  tuya/compat/owf_tuya_dpll_sweep.h — DISCOVER >480 MHz DPLL bands on the T5 (BK7258).
 *
 *  WHY: the core clock above 480 MHz can only come from retuning the DPLL, which is a
 *  BAND-CALIBRATED PLL (no N-multiplier). The frequency knob is ana_reg0.band[24:22] +
 *  bandmanual[25]; the SDK normally auto-calibrates it to ~480 and never forces a band.
 *  There is NO band->MHz table anywhere, so the only way to know what a band yields is
 *  to FORCE it, re-lock, and MEASURE. This routine does exactly that and logs the map.
 *
 *  CRITICAL SAFETY — the CPU running this code is clocked BY the DPLL we're retuning, and
 *  320M/480M are both taps off the SAME single DPLL (confirmed: only one en_dpll, no
 *  separate PLL). So we CANNOT 'park' on 320. We park on the 26 MHz XTAL (cksel_core=0,
 *  en_xtall — the one source independent of the DPLL) while the DPLL is retuned, then
 *  switch back to the DPLL tap to measure. The AON-RTC timer used by the measurement is
 *  also XTAL/32k-domain, so timing stays valid throughout.
 *
 *  REVERSIBLE: on exit we clear bandmanual (back to auto-cal 480) and restore the
 *  original cksel/div, so even a band that fails to lock leaves the device recoverable.
 *
 *  STILL RISKY: a forced band may not lock, or may lock at a rate the core can't run at
 *  the current voltage -> hang. RAISE CORE VOLTAGE FIRST (owf_tuya_set_core_mv, e.g.
 *  1100 mV) for headroom, and expect that some sweeps need a power-cycle. Run from a
 *  diagnostics screen over USB serial; this is a bring-up tool, not a normal code path.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build.
 * ========================================================================== */
#pragma once
#include <stdint.h>
#include "sys_ll.h"                  // ana_reg0 band setters, cksel_core, spitrig/spideten
#include "owf_tuya_voltage.h"        // owf_tuya_set_core_mv() — matched voltage for the band
#include "owf_tuya_soc_stats.h"      // OWF_T5_REG_CPU_CLKDIV + decode helpers
#include "owf_tuya_cpu_measure.h"    // owf_tuya_measure_cpu_mhz() — crystal-anchored, real

/* The SDK's sys_hal_delay() is `static` (file-local, unlinkable) and is just an empty
 * busy-loop `while(times--)`. We replicate it so the DPLL re-lock timing matches the
 * SDK's cali sequence by iteration count. NOTE: during the sweep the CPU is parked on
 * 26 MHz, so each iteration is ~18x longer than at 480 MHz — a LONGER PLL settle, which
 * only helps the lock. `volatile` stops the loop being optimised away. */
static inline void owf_tuya_busy_delay(volatile uint32_t times) { while (times--) { } }

/* cksel_core source codes (reg CLK_DIV_MODE1[5:4]). */
#define OWF_T5_CKSEL_XTAL   0u   /* 26 MHz crystal — independent of the DPLL */
#define OWF_T5_CKSEL_320    2u
#define OWF_T5_CKSEL_480    3u

/* Park the CPU on the 26 MHz XTAL (div=0): cksel_core=0. Returns the previous raw
 * clk-div register so the exact prior state can be restored. */
static inline uint32_t owf_tuya_park_xtal(void) {
  uint32_t prev = OWF_T5_REG_CPU_CLKDIV;
  uint32_t r = (prev & ~0x3Fu) | (OWF_T5_CKSEL_XTAL << 4) | 0u;  /* cksel=0, div=0 */
  OWF_T5_REG_CPU_CLKDIV = r;
  return prev;
}
static inline void owf_tuya_restore_clkdiv(uint32_t prev) {
  OWF_T5_REG_CPU_CLKDIV = prev;
}

/* Run the DPLL re-lock trigger sequence (mirrors sys_hal_cali_dpll(param=0) timing).
 * Must be called with the CPU PARKED OFF the DPLL — it transiently unlocks the PLL. */
static inline void owf_tuya_dpll_relock(void) {
  sys_ll_set_ana_reg0_spitrig(0);
  owf_tuya_busy_delay(120);
  sys_ll_set_ana_reg0_spitrig(1);
  sys_ll_set_ana_reg0_spideten(0);
  owf_tuya_busy_delay(3400);
  sys_ll_set_ana_reg0_spideten(1);
  owf_tuya_busy_delay(3400);
}

/* Force the DPLL VCO band manually (bandmanual=1 + band code), then re-lock.
 * `band` is the 3-bit ana_reg0.band field (0..7). CPU must be parked first. */
static inline void owf_tuya_dpll_force_band(uint8_t band) {
  sys_ll_set_ana_reg0_bandmanual(1);
  sys_ll_set_ana_reg0_band(band & 0x7);
  owf_tuya_dpll_relock();
}

/* Return the DPLL to its stock auto-calibrated ~480 MHz (bandmanual=0 + re-lock). */
static inline void owf_tuya_dpll_restore_auto(void) {
  sys_ll_set_ana_reg0_bandmanual(0);
  owf_tuya_dpll_relock();
}

/* Apply a KNOWN-GOOD overclock band as a stable operating point (not a probe):
 *   - park the CPU on the 26 MHz XTAL (so retuning the DPLL can't hang the running core),
 *   - force the band (bandmanual=1) and trigger the lock, but leave spi-detect DISABLED
 *     so the auto-detect can't drag the VCO back toward the 480 band,
 *   - switch the CPU onto the DPLL 480-tap (cksel=3, div=0) to ride the retuned PLL,
 *   - set the matched core voltage.
 * `band` is the confirmed band (e.g. 0 = ~544 MHz on this part). The caller picks mv. */

/* The lock sequence WITHOUT re-enabling detect — for a forced band that must STICK. */
static inline void owf_tuya_dpll_relock_manual(void) {
  sys_ll_set_ana_reg0_spitrig(0);
  owf_tuya_busy_delay(120);
  sys_ll_set_ana_reg0_spitrig(1);
  sys_ll_set_ana_reg0_spideten(0);   /* keep detect OFF so the manual band holds */
  owf_tuya_busy_delay(3400);
}

/* Snapshot of the DPLL band state, for the read-only baseline. */
typedef struct {
  uint8_t bandmanual, band, band0, band1, cksel;   /* ana_reg0 fields */
  uint8_t cksel_core, clkdiv_core;                 /* CPU clk-div */
} owf_t5_dpll_state_t;

static inline owf_t5_dpll_state_t owf_tuya_dpll_read(void) {
  owf_t5_dpll_state_t s;
  s.bandmanual  = (uint8_t)sys_ll_get_ana_reg0_bandmanual();
  s.band        = (uint8_t)sys_ll_get_ana_reg0_band();
  s.band0       = (uint8_t)sys_ll_get_ana_reg0_band0();
  s.band1       = (uint8_t)sys_ll_get_ana_reg0_band1();
  s.cksel       = (uint8_t)sys_ll_get_ana_reg0_cksel();
  s.cksel_core  = owf_tuya_cksel_core();
  s.clkdiv_core = owf_tuya_clkdiv_core();
  return s;
}

/* ---- the sweep --------------------------------------------------------------------
 * For each band in [band_lo..band_hi]: park on XTAL, force the band + re-lock, switch
 * back to the DPLL 480-tap (cksel=3,div=0), measure the real MHz, store it. Restores
 * the DPLL to auto + the original CPU clock at the end. Results[i] = measured MHz for
 * band (band_lo+i), or 0 if it didn't yield a usable clock. Returns count measured.
 *
 * Caller MUST have raised core voltage first. `out` needs (band_hi-band_lo+1) slots. */
static inline int owf_tuya_dpll_sweep(uint8_t band_lo, uint8_t band_hi, uint16_t *out) {
  if (band_hi > 7) band_hi = 7;
  uint8_t  orig_band   = (uint8_t)sys_ll_get_ana_reg0_band();
  uint8_t  orig_manual = (uint8_t)sys_ll_get_ana_reg0_bandmanual();
  uint32_t orig_clk    = OWF_T5_REG_CPU_CLKDIV;
  int n = 0;

  for (uint8_t b = band_lo; b <= band_hi; b++, n++) {
    uint32_t saved = owf_tuya_park_xtal();          /* CPU now on 26 MHz, DPLL free */
    owf_tuya_dpll_force_band(b);                     /* retune + re-lock the DPLL    */
    /* switch CPU to the DPLL 480-tap (cksel=3, div=0) to sample the new PLL rate */
    OWF_T5_REG_CPU_CLKDIV = (saved & ~0x3Fu) | (OWF_T5_CKSEL_480 << 4) | 0u;
    out[n] = (uint16_t)owf_tuya_measure_cpu_mhz();   /* crystal-anchored real MHz    */
  }

  /* restore: park, auto-cal the DPLL back to 480, then the original CPU clock. */
  owf_tuya_park_xtal();
  if (orig_manual) { sys_ll_set_ana_reg0_bandmanual(1); sys_ll_set_ana_reg0_band(orig_band); }
  else             { sys_ll_set_ana_reg0_bandmanual(0); }
  owf_tuya_dpll_relock();
  owf_tuya_restore_clkdiv(orig_clk);
  return n;
}

/* ---- apply a known-good overclock band as a STABLE operating point ------------------
 * Set core voltage first (so the faster clock has its rail), park on XTAL, force the
 * band with detect OFF so it holds (the manual relock), then ride the DPLL 480-tap
 * (cksel=3, div=0). On this part band 0 = ~544 MHz. Returns the measured MHz.
 * To leave OC, call owf_tuya_set_cpu_mhz(480) (auto-cal restore is in band_step's '-'). */
static inline uint16_t owf_tuya_dpll_apply_band(uint8_t band, uint16_t mv) {
  owf_tuya_set_core_mv(mv);                 /* voltage BEFORE the speed-up */
  uint32_t saved = owf_tuya_park_xtal();    /* CPU on 26 MHz while the DPLL retunes */
  sys_ll_set_ana_reg0_bandmanual(1);
  sys_ll_set_ana_reg0_band(band & 0x7);
  owf_tuya_dpll_relock_manual();            /* lock, detect stays OFF so band holds */
  OWF_T5_REG_CPU_CLKDIV = (saved & ~0x3Fu) | (OWF_T5_CKSEL_480 << 4) | 0u;  /* ride it */
  return (uint16_t)owf_tuya_measure_cpu_mhz();
}
