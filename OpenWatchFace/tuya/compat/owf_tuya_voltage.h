/* ============================================================================
 *  tuya/compat/owf_tuya_voltage.h — core-voltage control on the T5 (BK7258).
 *
 *  TWO STACKED CORE REGULATORS (both in analog reg9, confirmed in the BK7258 SDK
 *  source middleware/soc/bk7258/hal/sys_hal.c):
 *
 *   - vcorehsel  reg9[19:16]  4-bit  FINE digital-core LDO trim.
 *       SDK anchors (sys_types.h pm_vdddig_value_e): 0xB=0.875 0xC=0.9 0xD=0.925
 *       0xE=0.95 V  ->  mV = 600 + code*25.  Ceiling 0xF = 0.975 V.  This is the
 *       knob owf_tuya_cpu_freq.h uses per-frequency; stock 480 MHz = 0xE (0.95 V).
 *
 *   - vdighsel   reg9[28:26]  3-bit  COARSE VDDD digital rail — the path ABOVE 0.975 V.
 *       Written by the SDK's sys_hal_ctrl_vddd_h_vol(), with ONE documented anchor:
 *       PM_VDDD_H_VOL_1V = 0x6 = 1.0 V. Its sibling 3-bit rails (LP vol, pm.h) step at
 *       100 mV/code, so we MODEL vdighsel as 100 mV/code about that anchor:
 *           code 0x3=0.7 0x4=0.8 0x5=0.9 0x6=1.0 0x7=1.1 V
 *       >>> The per-code scale ABOVE 1.0 V is NOT documented in the SDK — VERIFY on the
 *           bench / datasheet before trusting the mV. The raw setter is exposed for that.
 *       NOTE: in the STOCK build CONFIG_RX_OPTIMIZE=n, so the SDK never writes vdighsel
 *       (sys_hal_ctrl_vddd_h_vol is #if'd out). We write it directly here, which is the
 *       whole point — it unlocks the >0.975 V rail the stock firmware leaves disabled.
 *
 *  Writes go through the SDK's generated LL setters (sys_ll_set_ana_reg9_*), i.e. the
 *  real analog SPI-shadow commit path (REG_WRITE + status poll). Each rail write is
 *  framed by spi_latch1v=1..0, exactly as sys_hal_ctrl_vdddig_h_vol/vddd_h_vol do.
 *
 *  NO CLAMPS BY DESIGN — you are deliberately probing limits. Values are masked to field
 *  width so a write can't corrupt neighbouring bits, but no voltage ceiling is enforced.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build.
 * ========================================================================== */
#pragma once
#include <stdint.h>

/* SDK low-level register layer: vcorehsel/vdighsel/vcorelsel setters + spi_latch1v,
 * and the correct analog-commit poll for THIS build (self-guarded, extern "C"). */
#include "sys_ll.h"

/* bk_delay_us: same busy-wait microsecond delay used in owf_tuya_psram_freq.h for the
 * PSRAM LDO settle (exported in libbk_system.a, header not on the sketch include path).
 *
 * Stock Tuya firmware boots at 240 MHz and lets its own LVGL/power-management stack
 * dynamically step the core up to 480 MHz at runtime; the vendor's rail setters
 * (sys_hal_ctrl_vddd_h_vol / sys_hal_ctrl_vdddig_h_vol in sys_hal.c) add a settle delay
 * after each vcorehsel/vdighsel write specifically to cover that dynamic step-up, but only
 * on the CONFIG_RX_OPTIMIZE path -- which is compiled out in the stock build we're on, and
 * which we don't use anyway. We ship our OWN LVGL stack, never dynamically rescale, and
 * instead force one fixed operating point at boot via owf_tuya_set_cpu_mhz() -- so the
 * vendor's dynamic-step delay logic is simply absent from our path.
 *
 * We still need a settle delay for the same underlying reason (the rail needs time to
 * physically stabilize after the write), just applied to OUR fixed-frequency boot instead
 * of their dynamic scaling. owf_tuya_set_cpu_mhz() writes the CPU clock-div register right
 * after this voltage write; without a settle gap here, that clock switch can land before
 * the new rail has stabilized -- fine once the chip is warm and already running for hours,
 * but a source of intermittent failures right at cold-boot / OC-apply time. */
extern "C" void bk_delay_us(uint32_t us);
#define OWF_T5_VOLTAGE_SETTLE_US 20u

/* ---- fine rail: vcorehsel (reg9[19:16], <=0.975 V) -------------------------------- */
static inline void owf_tuya_set_vcorehsel(uint8_t code) {
  sys_ll_set_ana_reg9_spi_latch1v(1);
  sys_ll_set_ana_reg9_vcorehsel(code & 0xF);
  sys_ll_set_ana_reg9_spi_latch1v(0);
  bk_delay_us(OWF_T5_VOLTAGE_SETTLE_US);
}
static inline uint8_t owf_tuya_get_vcorehsel(void) {
  return (uint8_t)(sys_ll_get_ana_reg9_vcorehsel() & 0xF);
}
static inline uint16_t owf_tuya_vcorehsel_mv(uint8_t code) {
  return (uint16_t)(600 + (code & 0xF) * 25);            /* 0xE=950, 0xF=975 */
}

/* ---- digital rail: vdighsel (reg9[28:26]) — the SEPARATE vdddig LDO ----------------
 * VERIFIED scale from the BK7258 SDK SYS_PM_DIGITAL_HIGH_VOLT_MAP (sys_pm_hal_ctrl.h):
 *   code 0..7 -> 700,750,800,850,900,950,1000,1050 mV  (50 mV/step, base 700 @ code 0).
 * This is a DIFFERENT regulator from vcorehsel — it powers vdddig, default 1.0 V (code 6).
 * It reaches up to 1.05 V; vcorehsel (the core LDO) tops at 0.975 V. */
#define OWF_T5_VDIGHSEL_BASE_MV      700u                /* code 0 */
#define OWF_T5_VDIGHSEL_MV_PER_CODE  50u
#define OWF_T5_VDIGHSEL_MAX_CODE     7u                  /* 3-bit -> 1050 mV ceiling */

static inline void owf_tuya_set_vdighsel(uint8_t code) {
  sys_ll_set_ana_reg9_spi_latch1v(1);
  sys_ll_set_ana_reg9_vdighsel(code & 0x7);
  sys_ll_set_ana_reg9_spi_latch1v(0);
  bk_delay_us(OWF_T5_VOLTAGE_SETTLE_US);
}
static inline uint8_t owf_tuya_get_vdighsel(void) {
  return (uint8_t)(sys_ll_get_ana_reg9_vdighsel() & 0x7);
}
static inline uint16_t owf_tuya_vdighsel_mv(uint8_t code) {
  return (uint16_t)(OWF_T5_VDIGHSEL_BASE_MV + (code & 0x7) * OWF_T5_VDIGHSEL_MV_PER_CODE);
}
/* Nearest vdighsel code for a target mV (verified scale). Clamped to the 3-bit field. */
static inline uint8_t owf_tuya_vdighsel_code_for(uint16_t mv) {
  if (mv < OWF_T5_VDIGHSEL_BASE_MV) return 0;
  int code = ((int)mv - (int)OWF_T5_VDIGHSEL_BASE_MV + (int)OWF_T5_VDIGHSEL_MV_PER_CODE / 2)
             / (int)OWF_T5_VDIGHSEL_MV_PER_CODE;
  if (code > (int)OWF_T5_VDIGHSEL_MAX_CODE) code = OWF_T5_VDIGHSEL_MAX_CODE;
  return (uint8_t)code;
}

/* ====================================================================================
 *  mV-BASED CORE VOLTAGE — set an absolute target in millivolts.
 *
 *  IMPORTANT (learned on-device): vcorehsel and vdighsel are TWO SEPARATE regulators.
 *  vcorehsel = CORE LDO (hard ceiling 0.975 V); vdighsel = DIGITAL/vdddig LDO (to 1.05 V).
 *  Which rail your overclock actually depends on is part-specific — so for any target we
 *  drive BOTH: vcorehsel to min(mv,975), and vdighsel to the same mv (clamped to its
 *  700..1050 range). That way >975 mV requests genuinely raise the rail that CAN exceed
 *  975 (vdighsel) instead of silently pinning at the core-LDO ceiling. Below 700 mV the
 *  digital rail stays at its 700 mV floor.
 *
 *  Order vs the clock (raise V before speed-up, lower after) is the caller's job —
 *  owf_tuya_set_cpu_mhz already does that via owf_tuya_get_core_mv().
 * ==================================================================================== */
static inline void owf_tuya_set_core_mv(uint16_t mv) {
  /* core LDO: fine, <=975 mV */
  uint16_t vsel_mv = mv > 975 ? 975 : mv;
  if (vsel_mv < 600) vsel_mv = 600;
  uint8_t vcode = (uint8_t)((vsel_mv - 600 + 12) / 25);   /* round to nearest 25 mV */
  if (vcode > 0xF) vcode = 0xF;
  owf_tuya_set_vcorehsel(vcode);

  /* digital LDO: drive it to the same target (this is the rail that reaches >975 mV). */
  owf_tuya_set_vdighsel(owf_tuya_vdighsel_code_for(mv));
}

/* Live core-voltage readback in mV = the HIGHER of the two rails (whichever the part's
 * core is actually riding dominates; reporting the max reflects the effective headroom
 * and stays equal to a >975 request that vdighsel carried). */
static inline uint16_t owf_tuya_get_core_mv(void) {
  uint16_t core = owf_tuya_vcorehsel_mv(owf_tuya_get_vcorehsel());
  uint16_t dig  = owf_tuya_vdighsel_mv(owf_tuya_get_vdighsel());
  return dig > core ? dig : core;
}
/* Separate per-rail reads, for the Power app to show both (so you can see which one your
 * overclock depends on). */
static inline uint16_t owf_tuya_get_core_ldo_mv(void) { return owf_tuya_vcorehsel_mv(owf_tuya_get_vcorehsel()); }
static inline uint16_t owf_tuya_get_dig_ldo_mv(void)  { return owf_tuya_vdighsel_mv(owf_tuya_get_vdighsel()); }
