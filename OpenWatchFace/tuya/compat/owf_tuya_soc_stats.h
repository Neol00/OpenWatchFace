/* ============================================================================
 *  tuya/compat/owf_tuya_soc_stats.h — live CPU frequency + core voltage on the T5.
 *
 *  For the Power app's "CORE & CLOCK" block. The clean SDK getters are STUBS in this
 *  prebuilt (bk_pm_*_cpu_freq_get() return a constant 0; sys_hal_get_clk_select() is a
 *  "tmp build" return 0), so we read the SoC's clock + analog registers directly — the
 *  SAME registers the SDK's own sys_hal_set_core_freq()/sys_hal_ctrl_vdddig_h_vol()
 *  write. Addresses/bitfields are from the BK7258 register map (sys_reg.h) and the
 *  decode is cross-checked against the SDK's DVFS table in sys_hal_switch_cpu_bus_freq().
 *
 *  CPU frequency — SYS_CPU_CLK_DIV_MODE1 @ 0x44010020:
 *      clkdiv_core = bits[3:0]   (F / (1 + N))
 *      cksel_core  = bits[5:4]   (0=XTAL 26M, 1=DCO, 2=320M PLL, 3=480M PLL)
 *    core_freq = base(cksel_core) / (1 + clkdiv_core).  Default boot = 120 MHz
 *    (CONFIG_CPU_FREQ_HZ), i.e. cksel=3 (480M) / div=3 -> 120 MHz, matching the SDK's
 *    PM_CPU_FRQ_120M entry { sys_hal_core_bus_clock_ctrl(0x3,0x3,...) }.
 *
 *  Core voltage — SYS_ANA_REG9 @ 0x44010024, vcorehsel = bits[19:16] (the active digital-
 *    core LDO select, set by sys_hal_ctrl_vdddig_h_vol). The SDK's DVFS table pins the
 *    code<->mV scale: 0x5=0.725V, 0x6=0.750V, 0x7=0.775V, 0x8=0.800V -> 25 mV/code,
 *    so mV = 600 + code*25. (Confirmed: sys_hal_vdddig_h_vol_get disassembles to
 *    ubfx #16,#4 of reg9 — exactly this field.)
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated by its includer).
 * ========================================================================== */
#pragma once
#include <stdint.h>

/* BK7258 system-control register block (reg_base.h: 0x44010000, SOC_ADDR_OFFSET=0). */
#define OWF_T5_SYS_REG_BASE   0x44010000u
#define OWF_T5_REG_CPU_CLKDIV (*(volatile uint32_t *)(OWF_T5_SYS_REG_BASE + (0x8u << 2)))  /* 0x44010020 CLK_DIV_REG0 */
#define OWF_T5_REG_CPU1_OP    (*(volatile uint32_t *)(OWF_T5_SYS_REG_BASE + (0x5u << 2)))  /* 0x44010014 cpu1 ctrl   */
/* Analog reg9 is at offset 0x49 (SYS_ANA_REG9_ADDR), NOT 0x9 — the earlier 0x9 read the
 * wrong register, which is why the core voltage "never changed". It's plain MMIO
 * (sys_ll_get_analog_reg_value = REG_READ). vcorehsel = bits[19:16]. */
#define OWF_T5_REG_ANA9       (*(volatile uint32_t *)(OWF_T5_SYS_REG_BASE + (0x49u << 2)))  /* 0x44010124 ana reg9 */

/* Raw clock-div register fields (CLK_DIV_MODE1 @ 0x44010020):
 *   cksel_core  = bits[5:4]  base PLL select
 *   clkdiv_core = bits[3:0]  core divider, F/(1+N)
 * The base-clock-per-cksel mapping is taken straight from the T5 SDK's own
 * sys_hal_switch_cpu_bus_freq() / sys_hal_core_bus_clock_ctrl() (bk7258_ap/hal/sys_hal.c)
 * and its register doc (sys_struct.h reg0x08): cksel 0=XTAL 26M, 1=DCO, 2=320M PLL,
 * 3=480M PLL. The firmware runs on CPU1, whose rate IS the core rate base/(1+div).
 * Cross-checked against the SDK's DVFS table, which this decode reproduces exactly:
 *   480/(1+3)=120  480/(1+1)=240  480/(1+5)=80  480/(1+7)=60  26/(1+0)=26. */
static inline uint8_t owf_tuya_cksel_core(void) { return (uint8_t)((OWF_T5_REG_CPU_CLKDIV >> 4) & 0x3); }
static inline uint8_t owf_tuya_clkdiv_core(void){ return (uint8_t)( OWF_T5_REG_CPU_CLKDIV       & 0xF); }

/* Base clock (MHz) feeding the core divider, selected by cksel_core. */
static inline uint16_t owf_tuya_cksel_base_mhz(uint8_t cksel) {
  switch (cksel) {
    case 0: return 26;    /* XTAL 26M    */
    case 1: return 120;   /* DCO (~120M on this part; unused by our DVFS) */
    case 2: return 320;   /* 320M PLL    */
    default: return 480;  /* cksel==3, 480M PLL */
  }
}

/* CPU (CPU1/core) frequency in MHz as DECODED FROM THE CLOCK-DIV REGISTER. This is
 * the divider/select the chip was PROGRAMMED with — NOT a measurement. If the PLL
 * didn't actually land where it was told, this still reports the programmed value.
 * For the real running frequency, use the cycle-count measurement in
 * owf_tuya_cpu_measure.h (cached below as the live value the UI/getCpuFrequencyMhz show). */
static inline uint16_t owf_tuya_cpu_mhz_decoded(void) {
  uint16_t base = owf_tuya_cksel_base_mhz(owf_tuya_cksel_core());
  return (uint16_t)(base / (1u + owf_tuya_clkdiv_core()));
}

/* The last CPU freq (MHz) we COMMANDED via owf_tuya_set_cpu_mhz() (owf_tuya_cpu_freq.h). */
static uint16_t s_owf_tuya_cmd_mhz = 0;
static inline uint16_t owf_tuya_voted_mhz(void) { return s_owf_tuya_cmd_mhz ? s_owf_tuya_cmd_mhz : 480; }

/* Cache of the last MEASURED CPU frequency (MHz), updated by owf_tuya_measure_cpu_mhz()
 * (owf_tuya_cpu_measure.h) — e.g. each time the Power panel refreshes. 0 until first
 * measured. Cheap getters (getCpuFrequencyMhz) read this instead of blocking on a
 * fresh measurement. Lives here because this header is included first. */
static uint16_t s_owf_tuya_meas_mhz = 0;

/* Live CPU frequency for cheap, non-blocking callers: the last measured value if we
 * have one, else fall back to the register decode (still better than the setpoint). */
static inline uint16_t owf_tuya_cpu_mhz(void)  { return s_owf_tuya_meas_mhz ? s_owf_tuya_meas_mhz : owf_tuya_cpu_mhz_decoded(); }
static inline uint16_t owf_tuya_cpu1_mhz(void) { return owf_tuya_cpu_mhz(); }
static inline uint16_t owf_tuya_core_mhz(void) { return owf_tuya_cpu_mhz(); }

/* Live digital-core (CPU) LDO voltage in millivolts, from reg9 vcorehsel. */
static inline uint16_t owf_tuya_core_mv(void) {
  uint32_t code = (OWF_T5_REG_ANA9 >> 16) & 0xF;   /* vcorehsel, bits[19:16] */
  return (uint16_t)(600 + code * 25);              /* 25 mV/code (SDK DVFS scale) */
}

/* Raw vcorehsel code (0..15) — handy when (later) stepping it for undervolt. */
static inline uint8_t owf_tuya_core_vsel(void) {
  return (uint8_t)((OWF_T5_REG_ANA9 >> 16) & 0xF);
}
