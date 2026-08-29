/* ============================================================================
 *  tuya/compat/owf_tuya_cpu_freq.h — user-selectable CPU clock on the T5 (BK7258).
 *
 *  HOW WE SET THE CLOCK (and the dead ends we ruled out on-device):
 *   - bk_pm_module_vote_cpu_freq(DEFAULT, code): a mailbox VOTE to the CP1 power
 *     coprocessor. On this build CP1 does NOT act on it at runtime — the clock register
 *     never changed. Useless for forcing a clock.
 *   - sys_drv_switch_cpu_bus_freq(code): the SDK's stepping switch. It RAMPS through
 *     intermediate enum levels, and the 320M level programs only 0.8V at 320 MHz (its
 *     own comment admits 320M+ needs 0.9V) -> brownout. Stepping toward 480 hit that and
 *     PANICKED / hung the boot. Also it can reprogram the bus clock. Rejected.
 *   - WHAT WE USE: set the CORE divider + cpu1 doubler + matched core voltage DIRECTLY:
 *       sys_drv_core_bus_clock_ctrl(cksel=3, div, bus=0, cpu0=0, cpu1)  // clock
 *       sys_hal_ctrl_vdddig_h_vol(vcode)                                 // voltage
 *     KEEPING cksel=3 (the 480 MHz PLL already running) and bus=0 means the PLL source
 *     and the bus-clock divider — the domain clocking QSPI + PSRAM — are NEVER touched,
 *     so the display/PSRAM don't wedge. Only the CPU core rate scales. Voltage is set
 *     BEFORE a speed-up and AFTER a slow-down so the core is never fast on a low rail.
 *     This is exactly why the earlier "stuck at 675 mV / 480 MHz" state crashed: a high
 *     clock on an unmatched low voltage. The OWF_T5_DVFS table below pairs each speed
 *     with its correct voltage.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build.
 * ========================================================================== */
#pragma once
#include <stdint.h>
#include "owf_tuya_soc_stats.h"   // shared s_owf_tuya_cmd_mhz + OWF_T5_REG_CPU_CLKDIV regs
#include "owf_tuya_voltage.h"     // owf_tuya_set_core_mv() — mV-based core voltage (vcorehsel + vdighsel)

/* The clock dividers are written DIRECTLY to CLK_DIV_MODE1 (OWF_T5_REG_CPU_CLKDIV),
 * NOT via sys_drv_core_bus_clock_ctrl — that wrapper rejects the 480MHz/div-0 setpoint
 * (guard: cksel==480M && div==0 -> BK_FAIL), which silently breaks setting 480 MHz.
 * Core voltage is set in MILLIVOLTS via owf_tuya_set_core_mv() (owf_tuya_voltage.h):
 * the fine vcorehsel rail up to 0.975 V, then the coarse vdighsel rail above it, so the
 * table can ask for the higher rails an overclock needs (e.g. 1100 mV). */

/* DVFS operating points. `mv` is the ABSOLUTE core target in millivolts.
 *   <=975 mV  -> vcorehsel (fine);  >975 mV -> vcorehsel pinned 0xF + vdighsel (coarse).
 * NOTE: cksel is a 2-bit field (0=XTAL,1=DCO,2=320PLL,3=480PLL). cksel=4 CANNOT be
 * written (masks to 0) — a real >480 MHz point needs the 480 PLL retuned, not cksel=4.
 * The 640 row is a PLACEHOLDER until that PLL work lands; it will NOT give 640 MHz yet. */
typedef struct { uint16_t mhz; uint8_t cksel; uint8_t div; uint8_t cpu1_dbl; uint16_t mv; } owf_t5_dvfs_t;
static const owf_t5_dvfs_t OWF_T5_DVFS[] = {
  /* mhz  cksel div cpu1   mv    -> rate / voltage */
  { 240,   3,   1,   1,    775 },   /* 480/2 = 240 MHz @ 800 mV  */
  { 320,   2,   0,   1,    825 },   /* 320/1 = 320 MHz @ 875 mV  */
  { 480,   3,   0,   1,    875 },   /* 480/1 = 480 MHz @ 950 mV  */
};
static const int OWF_T5_DVFS_N = (int)(sizeof(OWF_T5_DVFS) / sizeof(OWF_T5_DVFS[0]));

/* Find the operating point for a target MHz (exact match; else the highest point <= mhz,
 * so an in-between value like 320 maps to 240). Clamps up to the lowest if below range. */
static inline const owf_t5_dvfs_t *owf_tuya_dvfs_for(uint16_t mhz) {
  const owf_t5_dvfs_t *best = nullptr;
  for (int i = 0; i < OWF_T5_DVFS_N; i++) {
    if (OWF_T5_DVFS[i].mhz == mhz) return &OWF_T5_DVFS[i];
    if (OWF_T5_DVFS[i].mhz <= mhz) best = &OWF_T5_DVFS[i];  /* table is ascending */
  }
  return best ? best : &OWF_T5_DVFS[0];   /* below lowest -> clamp up to the lowest */
}

/* Enable the actual hardware switch. Set to 0 to fall back to record-only (no HW change)
 * if a board ever misbehaves. */
#define OWF_T5_CPU_FREQ_APPLY 1

/* Set the CPU clock to `mhz`. Picks the matching DVFS point and applies clock + voltage
 * directly, with the matched core voltage set BEFORE a speed-up and AFTER a slow-down so
 * the core is never fast on a low rail. Records the commanded MHz for display. */
static inline bool owf_tuya_set_cpu_mhz(uint16_t mhz) {
  const owf_t5_dvfs_t *p = owf_tuya_dvfs_for(mhz);
  if (!p) return false;
#if OWF_T5_CPU_FREQ_APPLY
  uint16_t cur_mv = owf_tuya_get_core_mv();          /* current core target (mV) */
  if (p->mv > cur_mv)                                /* speeding up: RAISE voltage FIRST */
    owf_tuya_set_core_mv(p->mv);
  /* Clock: write the core clk-div register DIRECTLY, NOT via sys_drv_core_bus_clock_ctrl.
   * That SDK wrapper has a guard `if (cksel==480M && div==0) return BK_FAIL;` which
   * refuses the 480 MHz / div-0 setpoint outright — so going through it silently leaves
   * the clock unchanged (the regression: 480 stops applying). We've confirmed on-device
   * that cksel=3/div=0 = 480 MHz runs fine, so write cksel (bits[5:4]) + div (bits[3:0])
   * straight to CLK_DIV_MODE1, preserving the bus bit and everything else. */
  {
    uint32_t r = OWF_T5_REG_CPU_CLKDIV;
    r = (r & ~0x3Fu)                                /* clear div[3:0] + cksel[5:4] */
        | ((uint32_t)(p->div & 0xF))                /* div  -> bits[3:0] */
        | ((uint32_t)(p->cksel & 0x3) << 4);        /* cksel-> bits[5:4] */
    OWF_T5_REG_CPU_CLKDIV = r;
  }
  if (p->mv < cur_mv)                                /* slowing down: LOWER voltage AFTER */
    owf_tuya_set_core_mv(p->mv);
#endif
  s_owf_tuya_cmd_mhz = p->mhz;
  return true;
}

/* owf_tuya_voted_mhz() (the commanded setpoint) lives in owf_tuya_soc_stats.h.
 * The clock reported by the Power app + getCpuFrequencyMhz is the MEASURED frequency
 * (owf_tuya_measure_cpu_mhz() in owf_tuya_cpu_measure.h — cycle-count vs the AON RTC),
 * NOT this setpoint and NOT the clock-div register decode. */
