/* ============================================================================
 *  core_voltage.h — EXPERIMENTAL real core undervolt (ESP32-S3 digital LDO).
 *
 *  This is the REAL knob, not the AXP2101 rail trick in settings_store.h. It
 *  lowers the chip's ACTUAL digital-core voltage by overriding the `dig_dbias`
 *  trim that ESP-IDF programs into the on-die LDO. Power ~ V^2, so dropping the
 *  core from 1.25V to 1.15V at 240 MHz cuts core dynamic power ~15%. The biggest
 *  wins are at LOW clocks: IDF leaves 80/160 MHz at 1.25V too (when flash is
 *  80/160 MHz, which this board uses), so they're badly over-volted by default.
 *
 *  ---------------------------------------------------------------------------
 *  HOW TO TUNE: edit CORE_UV_MV[] below — just type the core voltage you want
 *  for each CPU speed, in MILLIVOLTS. You don't need exact values or to know the
 *  register: the code snaps whatever you write to the nearest voltage the chip
 *  actually supports (50 mV grid) and clamps it to a safe floor. So "1100" means
 *  "give 80 MHz roughly 1.1 V". Lower number = more undervolt = more savings and
 *  more risk. That's the whole knob.
 *  ---------------------------------------------------------------------------
 *
 *  MECHANISM: the active digital-core voltage is the I2C_DIG_REG_EXT_DIG_DREG
 *  field of the analog "regi2c" block (I2C_DIG_REG = 0x6D). setCpuFrequencyMhz()
 *  rewrites it for the new frequency, so we MUST re-apply our override AFTER each
 *  frequency change (see settings_apply_cpu_mhz()).
 *
 *  RISK MODEL — fundamentally different from the rail. Too-low core voltage does
 *  NOT brown out cleanly; it causes SILENT logic/RAM/flash corruption: wrong
 *  arithmetic, random crashes, LVGL glitches, possibly bad flash writes. So the
 *  canary here is a COMPUTE+MEMORY self-test (below), NOT the BROWNOUT banner.
 *  It is NOT a brick risk: dig_dbias resets to the IDF default on every reboot,
 *  and the target values live only in this source table -> reflash to change.
 *
 *  The boot path captures a "golden" self-test result at the stock voltage,
 *  applies the table, then re-checks; on mismatch it AUTO-REVERTS to 1.25V and
 *  flags it, so a too-aggressive value can't brick a boot. Tune ONE step at a
 *  time, flash, then run it hard (max brightness + WiFi fetch + busy CPU) for a
 *  long while. Expect silicon lottery — your chip likely does 240 MHz at ~1.15V
 *  and 80 MHz far lower.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "board.h"                     // BOARD_* selection (S3 vs C6 voltage mechanism)

/* --- Per-board core-voltage MECHANISM ---------------------------------------
 * The two chips set the active digital-core voltage through DIFFERENT hardware:
 *
 *   S3-2.06 (ESP32-S3): a static analog "regi2c" trim — write the
 *     I2C_DIG_REG_EXT_DIG_DREG field once and it holds. (Original path.)
 *
 *   C6-1.47 (ESP32-C6): the PMU owns core voltage. The active voltage is the
 *     PMU HP_ACTIVE regulator dbias field (PMU.hp_sys[HP_ACTIVE].regulator0.dbias),
 *     set via pmu_ll_hp_set_regulator_dbias(). A regi2c write would be the wrong
 *     register and get clobbered by the PMU on the next HP-active entry, so the C6
 *     MUST go through the PMU. Stock HP-active dbias = HP_CALI_DBIAS_DEFAULT (25).
 *
 * Both are 5-bit dbias codes (0..31), so the canary + auto-revert logic is shared;
 * only the setter/getter, the code<->mV grid, and the "stock" code differ. */
#if defined(BOARD_WS_S3_TOUCH_AMOLED_206)
  #define CORE_UV_VIA_REGI2C 1
  #include "esp_private/regi2c_ctrl.h" // REGI2C_WRITE_MASK / READ_MASK (private SDK API)
  #include "soc/regi2c_dig_reg.h"      // I2C_DIG_REG, I2C_DIG_REG_EXT_DIG_DREG
#elif defined(BOARD_WS_C6_TOUCH_LCD_147)
  #define CORE_UV_VIA_PMU 1
  // Direct MMIO to the PMU HP_ACTIVE regulator register — NOT the pmu_ll inline +
  // &PMU struct symbol, which crash-looped (instruction-access fault) when called
  // from app flash. The PMU is an always-on peripheral, so a plain REG_SET_FIELD
  // is safe with the cache on and can't jump anywhere.
  #include "soc/pmu_reg.h"             // PMU_HP_ACTIVE_HP_REGULATOR0_REG + DBIAS field
  #include "soc/soc.h"                 // REG_SET_FIELD / REG_GET_FIELD
#else
  #define CORE_UV_UNSUPPORTED 1        // unknown board -> compile to safe no-ops
#endif

/* === the only things most people need to touch ============================ */
#define UNDERVOLT_CORE_ENABLE 0     // 0 = leave IDF stock voltages; 1 = apply CORE_UV_MV[]
#define CORE_UV_FLOOR_MV      900   // hard floor; values below this are clamped up

typedef struct { uint16_t mhz; uint16_t mv; } core_uv_entry_t;
#if CORE_UV_VIA_PMU
/* C6-1.47 (ESP32-C6): stock HP-active core ≈ 1.10 V (dbias 25). The C6 commonly
 * runs at 160 MHz (boot default) / 80 MHz. CONSERVATIVE first sweep — drop a step
 * at a time against a USB meter, watching the [uv] canary line. Lower = more
 * savings + more risk of SILENT corruption (canary auto-reverts a bad boot). */
static const core_uv_entry_t CORE_UV_MV[] = {
  { 160, 1100 },   // stock
  {  80, 1100 },   // over-volted by default -> good headroom
};
#else
/* Target core voltage (mV) per CPU speed, same order as the Power menu offers.
 * DEFAULT = 1250 everywhere = STOCK / no-op. Lower a number to undervolt that
 * speed. Values interpolate to the nearest ~20 mV hardware code and clamp to
 * CORE_UV_FLOOR_MV.
 *
 *   Conservative first try:   240->1200  160->1150   80->1100
 *   Aggressive (validate!):   240->1150  160->1100   80->1050
 * S3-2.06 (ESP32-S3): stock 1.25 V; IDF leaves 80/160 over-volted -> big headroom. */
static const core_uv_entry_t CORE_UV_MV[] = {
  { 240, 1250 },   // stock
  { 160, 1250 },   // over-volted by default -> good headroom
  {  80, 1250 },   // most over-volted -> lots of room to drop
};
#endif
static const uint8_t CORE_UV_COUNT = sizeof(CORE_UV_MV) / sizeof(CORE_UV_MV[0]);

/* === internals: mV <-> dbias code ========================================
 * Anchors are the NAMED voltages from soc/rtc.h (900-1300 mV). The codes BETWEEN
 * anchors (e.g. 19, 21, 22, 24...) are valid hardware steps too — ~15-25 mV each
 * — so the converters below INTERPOLATE between anchors instead of snapping to
 * the named points. That yields ~20 mV tuning resolution (e.g. 1020 -> code 19,
 * distinct from 1050 -> code 20). Interpolated mV labels are estimates; only the
 * anchor rows are exact. Rows below 900 mV are EXTRAPOLATED guesses. */
typedef struct { uint16_t mv; uint8_t code; } core_dbias_step_t;

#if CORE_UV_VIA_PMU
/* C6-1.47 (ESP32-C6 PMU dbias). Anchored on two values from the C6 IDF
 * (pmu_param.h): code 1 = 0.6 V (PMU_HP_DBIAS_LIGHTSLEEP_0V6_DEFAULT) and the
 * stock HP-active code 25 (HP_CALI_DBIAS_DEFAULT) ≈ 1.10 V. That's ~21 mV/code,
 * a straight line used to interpolate the intermediate codes. The high anchor
 * (~1.15 V) is the IDF max; rows above stock are not used for undervolting. */
static const core_dbias_step_t CORE_DBIAS_GRID[] = {
  {  600, 1  }, {  700, 6  }, {  800, 10 }, {  900, 15 },
  { 1000, 20 }, { 1050, 22 }, { 1100, 25 }, { 1150, 27 },
};
static const uint8_t CORE_DBIAS_GRID_N = sizeof(CORE_DBIAS_GRID) / sizeof(CORE_DBIAS_GRID[0]);
#define CORE_DBIAS_STOCK 25          // C6 stock HP-active (HP_CALI_DBIAS_DEFAULT) — safe fallback
#else
/* S3-2.06 (ESP32-S3 regi2c dig_dbias). Anchors are the NAMED voltages from
 * soc/rtc.h (900-1300 mV); in-between codes are valid ~15-25 mV steps too. */
static const core_dbias_step_t CORE_DBIAS_GRID[] = {
  {  750, 4  }, {  800, 7  }, {  850, 10 }, {  900, 13 },
  {  950, 16 }, { 1000, 18 }, { 1050, 20 }, { 1100, 23 },
  { 1150, 25 }, { 1200, 28 }, { 1250, 30 }, { 1300, 31 },
};
static const uint8_t CORE_DBIAS_GRID_N = sizeof(CORE_DBIAS_GRID) / sizeof(CORE_DBIAS_GRID[0]);
#define CORE_DBIAS_STOCK 30          // 1.25 V — IDF's default; the safe fallback
#endif

/* Requested mV -> nearest hardware dbias code via piecewise-linear interpolation
 * between anchors (clamped to the grid ends + the user floor). Reaches the
 * in-between codes the named points skip, for ~20 mV resolution. */
static inline uint8_t core_mv_to_dbias(uint16_t mv) {
  if (mv < CORE_UV_FLOOR_MV) mv = CORE_UV_FLOOR_MV;
  const uint8_t last = CORE_DBIAS_GRID_N - 1;
  if (mv <= CORE_DBIAS_GRID[0].mv)    return CORE_DBIAS_GRID[0].code;
  if (mv >= CORE_DBIAS_GRID[last].mv) return CORE_DBIAS_GRID[last].code;
  for (uint8_t i = 1; i < CORE_DBIAS_GRID_N; i++) {
    if (mv <= CORE_DBIAS_GRID[i].mv) {
      int dmv  = CORE_DBIAS_GRID[i].mv   - CORE_DBIAS_GRID[i-1].mv;
      int dcod = CORE_DBIAS_GRID[i].code - CORE_DBIAS_GRID[i-1].code;
      int off  = (int)mv - CORE_DBIAS_GRID[i-1].mv;
      return (uint8_t)(CORE_DBIAS_GRID[i-1].code + (off * dcod + dmv / 2) / dmv);  // rounded
    }
  }
  return CORE_DBIAS_STOCK;  // unreachable
}
/* Reverse map for display/logging: dbias code -> approx mV (same interpolation). */
static inline uint16_t core_dbias_to_mv(uint8_t code) {
  const uint8_t last = CORE_DBIAS_GRID_N - 1;
  if (code <= CORE_DBIAS_GRID[0].code)    return CORE_DBIAS_GRID[0].mv;
  if (code >= CORE_DBIAS_GRID[last].code) return CORE_DBIAS_GRID[last].mv;
  for (uint8_t i = 1; i < CORE_DBIAS_GRID_N; i++) {
    if (code <= CORE_DBIAS_GRID[i].code) {
      int dcod = CORE_DBIAS_GRID[i].code - CORE_DBIAS_GRID[i-1].code;
      int dmv  = CORE_DBIAS_GRID[i].mv   - CORE_DBIAS_GRID[i-1].mv;
      int off  = (int)code - CORE_DBIAS_GRID[i-1].code;
      return (uint16_t)(CORE_DBIAS_GRID[i-1].mv + (off * dmv + dcod / 2) / dcod);  // rounded
    }
  }
  return 0;  // unreachable
}

/* === low-level: set / read the active digital-core dbias trim ===
 * Board-specific register: S3 = regi2c EXT_DIG_DREG; C6 = PMU HP_ACTIVE regulator
 * dbias (the PMU's own state config, so it persists across HP-active entries). */
#if CORE_UV_VIA_REGI2C
static inline void core_set_dig_dbias(uint8_t code) {
  REGI2C_WRITE_MASK(I2C_DIG_REG, I2C_DIG_REG_EXT_DIG_DREG, code);
}
static inline uint8_t core_get_dig_dbias(void) {
  return (uint8_t)REGI2C_READ_MASK(I2C_DIG_REG, I2C_DIG_REG_EXT_DIG_DREG);
}
#elif CORE_UV_VIA_PMU
/* C6 core voltage = the HP_ACTIVE regulator's dbias field, written by DIRECT MMIO.
 * PMU_HP_ACTIVE_HP_REGULATOR_DBIAS lives in bits [31:27] of
 * PMU_HP_ACTIVE_HP_REGULATOR0_REG (R/W, HW default 24; cal sets ~25). This is a
 * memory-mapped PMU register (always-on domain), so REG_SET_FIELD is a plain store
 * — no function call, no flash fetch, cache-safe. (The earlier pmu_ll inline +
 * &PMU symbol jumped to garbage from app flash; this avoids both.) */
static inline void core_set_dig_dbias(uint8_t code) {
  REG_SET_FIELD(PMU_HP_ACTIVE_HP_REGULATOR0_REG, PMU_HP_ACTIVE_HP_REGULATOR_DBIAS, code);
}
static inline uint8_t core_get_dig_dbias(void) {
  return (uint8_t)REG_GET_FIELD(PMU_HP_ACTIVE_HP_REGULATOR0_REG, PMU_HP_ACTIVE_HP_REGULATOR_DBIAS);
}
#else  /* unsupported board */
static inline void core_set_dig_dbias(uint8_t code) { (void)code; }
static inline uint8_t core_get_dig_dbias(void) { return CORE_DBIAS_STOCK; }
#endif

/* Apply the table's voltage for `mhz`. No-op unless enabled. MUST be called
 * AFTER setCpuFrequencyMhz(), which resets dbias for the new frequency. */
static inline void core_apply_for_mhz(uint16_t mhz) {
#if UNDERVOLT_CORE_ENABLE
  for (uint8_t i = 0; i < CORE_UV_COUNT; i++)
    if (CORE_UV_MV[i].mhz == mhz) { core_set_dig_dbias(core_mv_to_dbias(CORE_UV_MV[i].mv)); return; }
#else
  (void)mhz;
#endif
}

/* === stability self-test (the canary for silent corruption) ===============
 * Captures a "golden" result of a fixed compute+memory workload at the current
 * (trusted, stock) voltage, then re-runs it after undervolting and compares.
 * A mismatch means the lowered voltage made the core miscompute. Fast (<~5ms). */
static volatile uint32_t s_core_golden = 0;
static bool s_core_golden_valid = false;
static bool s_core_unstable = false;     // set if a post-undervolt check failed

static uint32_t core_selftest_compute(void) {
  // Mix arithmetic (ALU) with a strided RAM read/write so both logic and SRAM
  // are exercised. Deterministic: same input -> same output on a healthy core.
  static volatile uint32_t buf[256];
  uint32_t acc = 2166136261u;
  for (uint32_t i = 0; i < 256; i++) buf[i] = i * 2654435761u;
  for (uint32_t pass = 0; pass < 1200; pass++) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = buf[(i * 131u) & 255u];
      acc ^= v; acc *= 16777619u; acc ^= acc >> 15;
      buf[i] = acc;
    }
  }
  return acc;
}
static void core_selftest_capture_golden(void) {
  s_core_golden = core_selftest_compute();
  s_core_golden_valid = true;
}
static bool core_selftest_ok(void) {
  return s_core_golden_valid && (core_selftest_compute() == s_core_golden);
}
