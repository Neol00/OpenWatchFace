/* ============================================================================
 *  board_power.h — battery / PMU abstraction (board-neutral API).
 *
 *  Owns the power-management chip. On the S3-AMOLED-2.06 that's the AXP2101
 *  (XPowersLib): fuel gauge, charger config, peripheral rails, power key. On a
 *  board without a PMU the stub at the bottom keeps the same API: gauge reads
 *  return "unknown", rail control is a no-op, board_power_ok() is false — and
 *  every consumer already handles that (it's the old pmu_ok==false path).
 *
 *  The `power` object and `pmu_ok` flag are PRIVATE to this header now — no
 *  other file may touch them. Consumers use the board_* functions (and the
 *  rail_* primitives, used by sleep_power.h's probe machinery).
 *
 *  I2C LOCKING IS THE CALLER'S JOB: gauge reads share the touch/RTC bus, so
 *  wrap calls in i2c_lock()/i2c_unlock() exactly as before (this module can't
 *  take the lock itself — watch_base.h isn't included yet at this point in
 *  the .ino, and several callers batch multiple reads under one lock).
 * ========================================================================== */
#pragma once

/* Boards that predate the ADC-battery path don't define this flag. */
#ifndef BOARD_HAS_ADC_BATTERY
#define BOARD_HAS_ADC_BATTERY 0
#endif

#if BOARD_HAS_PMU_AXP2101
#include "XPowersLib.h"

static XPowersPMU power;        // module-private — use the accessors below
static bool       pmu_ok = false;

static bool board_power_ok(void) { return pmu_ok; }

/* Chip begin() ONLY — no config. The early-boot path calls this to get the PMU
 * answering ASAP (restore cut rails before the display init); the full config
 * runs later via board_power_full_init(). Safe to call again (re-begin). */
static bool board_power_begin(void) {
  pmu_ok = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  return pmu_ok;
}

/* Full interactive-boot config: fuel gauge measurements, power-key behaviour,
 * IRQ mask. Call after board_power_begin() returns true. */
static void board_power_full_init(void) {
  if (!pmu_ok) return;
  power.enableBattDetection();
  power.enableBattVoltageMeasure();
  power.enableSystemVoltageMeasure();
  power.enableVbusVoltageMeasure();

  // Power-key behaviour:
  //  - ONLEVEL: how long PWR must be held to power the system ON.
  //    512ms (1) is a quick, deliberate press and is known-good. If you want
  //    to experiment with 128ms (0) it MAY be too short for the PMU debounce;
  //    revert to 1 if power-on becomes unreliable.
  power.setOnLevel(1);  // 0:128ms 1:512ms 2:1s 3:2s
  power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  power.clearIrqStatus();
  power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
}

/* Charge cap + low-voltage cutoff — applied on background wakes too, in case a
 * re-begin() reset the PMU registers. */
static void board_power_sleep_charge_cfg(void) {
  if (!pmu_ok) return;
  power.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  power.setSysPowerDownVoltage(2600);
}

/* ---- Fuel gauge (caller holds the I2C lock) ------------------------------- */
static int      board_batt_percent(void)     { return power.getBatteryPercent(); }
static uint16_t board_batt_voltage_mv(void)  { return power.getBattVoltage(); }
static uint16_t board_vbus_voltage_mv(void)  { return power.getVbusVoltage(); }
static bool     board_is_charging(void)      { return power.isCharging(); }
static bool     board_vbus_in(void)          { return pmu_ok && power.isVbusIn(); }
static bool     board_usb_powered(void)      { return board_vbus_in(); }  // PMU VBUS sense

/* PMU die temperature (degC), or -273 if there's no PMU. The Tdie ADC channel
 * is enabled lazily on the first read (µA-scale; shared-ADC mux). Caller holds
 * the I2C lock. */
static float board_pmu_temp_c(void) {
  if (!pmu_ok) return -273.0f;
  static bool s_tdie_en = false;
  if (!s_tdie_en) { power.enableTemperatureMeasure(); s_tdie_en = true; }
  return power.getTemperature();
}

/* Hard power-off via the PMU: cuts the system rail entirely (deeper than deep
 * sleep). Used by the low-battery protective cutoff. Returns true if it issued
 * the shutdown (caller should then fall back to deep sleep only if this is
 * false, e.g. no PMU). On the AXP2101 this does not return power until the user
 * presses PWR (or USB is plugged). */
static bool board_power_off(void) {
  if (!pmu_ok) return false;
  power.shutdown();
  return true;
}

/* Raw DCDC1 (ESP32 main 3.3V rail) voltage set — the EXPERIMENTAL undervolt.
 * Clamping/snapping policy stays in settings_store.h; this is just the write. */
static void board_set_core_rail_mv(uint16_t mv) {
  if (!pmu_ok) return;
  power.setDC1Voltage(mv);
}

/* Does the PMU still answer on I2C? (sleep_power.h's rail-probe bus check.) */
static bool board_power_acks(void) {
  Wire.beginTransmission(AXP2101_SLAVE_ADDRESS);
  return Wire.endTransmission() == 0;
}

/* ---- Peripheral rails (sleep_power.h probe/cut machinery) ------------------
 * Only peripheral LDOs are candidates — never DCDC1 (ESP32) or the RTC supply. */
enum { RAIL_ALDO1, RAIL_ALDO2, RAIL_ALDO3, RAIL_ALDO4, RAIL_BLDO1, RAIL_BLDO2, RAIL_COUNT };

static const char *rail_name(uint8_t i) {
  static const char *N[RAIL_COUNT] = { "ALDO1","ALDO2","ALDO3","ALDO4","BLDO1","BLDO2" };
  return (i < RAIL_COUNT) ? N[i] : "?";
}
static void rail_set(uint8_t i, bool on) {
  switch (i) {
    case RAIL_ALDO1: on ? power.enableALDO1() : power.disableALDO1(); break;
    case RAIL_ALDO2: on ? power.enableALDO2() : power.disableALDO2(); break;
    case RAIL_ALDO3: on ? power.enableALDO3() : power.disableALDO3(); break;
    case RAIL_ALDO4: on ? power.enableALDO4() : power.disableALDO4(); break;
    case RAIL_BLDO1: on ? power.enableBLDO1() : power.disableBLDO1(); break;
    case RAIL_BLDO2: on ? power.enableBLDO2() : power.disableBLDO2(); break;
  }
}
static bool rail_is_on(uint8_t i) {
  switch (i) {
    case RAIL_ALDO1: return power.isEnableALDO1();
    case RAIL_ALDO2: return power.isEnableALDO2();
    case RAIL_ALDO3: return power.isEnableALDO3();
    case RAIL_ALDO4: return power.isEnableALDO4();
    case RAIL_BLDO1: return power.isEnableBLDO1();
    case RAIL_BLDO2: return power.isEnableBLDO2();
  }
  return false;
}

/* Owner-validated default cut set for THIS board. ALDO1 MUST stay on — it feeds the
 * PCF85063 RTC VDD / shared-I2C bus, and cutting it in deep sleep leaves the RTC +
 * FT3168 touch unrecoverable on wake (verified: RTC-not-found + touch-init-fail,
 * fixed only by a cold reboot). Every OTHER rail (ALDO2/3/4, BLDO1/2) cuts cleanly
 * and is restored on wake by rails_restore()'s forced, sequenced LDO re-cycle, so
 * cut them all by default for the deep-sleep power saving. */
//                                          ALDO1 ALDO2 ALDO3 ALDO4 BLDO1 BLDO2
static const uint8_t RAIL_DEFAULT_CUT[RAIL_COUNT] = { 0,    1,    1,    1,    1,    1 };

/* Rails that can NEVER be cut in deep sleep, as a bitmask over the rail enum, no
 * matter the cut set or the Power-app toggles. ALDO1 = the RTC/I2C supply (BIT0).
 * rails_cut_for_sleep() honours this as a hard guard. */
#define RAIL_NEVER_CUT_MASK  (1u << RAIL_ALDO1)

#elif BOARD_HAS_ADC_BATTERY  /* -------- ADC battery (no PMU, e.g. C6-1.47) --- */

/* No PMU chip, but the battery is brought to an ADC pin through a divider. We
 * read the pin in millivolts (the IDF calibration handles the ADC's nonlinear
 * curve), scale back up by the divider ratio, and estimate percent from a
 * LiPo voltage curve. There is no fuel-gauge IC, so percent is an estimate and
 * charge/VBUS state is unknown (returns false). board_power_ok() is TRUE so the
 * watch face + Power app show the live voltage/percent. No I2C lock needed —
 * the ADC is local to the SoC, not on the shared bus. */
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "hal/adc_types.h"
#include "soc/adc_channel.h"

static adc_oneshot_unit_handle_t s_badc      = nullptr;
static adc_cali_handle_t          s_bcali     = nullptr;
static bool                       s_badc_ok   = false;
static adc_channel_t              s_bchan     = ADC_CHANNEL_0;

static bool board_power_ok(void) { return s_badc_ok; }

/* Is the board powered/talking over USB right now? The C6 has no PMU VBUS sense,
 * but it IS being powered (and possibly flashed) over its built-in USB
 * Serial/JTAG when a host is attached — detect that so the low-battery cutoff
 * does NOT fire while plugged in (which would fight the flasher and power the
 * watch off on USB). usb_serial_jtag_is_connected() reports an attached host. */
#include "driver/usb_serial_jtag.h"
static bool board_usb_powered(void) {
  return usb_serial_jtag_is_connected();
}

static bool board_power_begin(void) {
  if (s_badc_ok) return true;
  // Resolve the GPIO to its ADC1 unit+channel. The SOC header maps each pad to
  // ADC1_CHANNEL_n via ADC1_GPIOxx_CHANNEL; the channel number equals the pad on
  // the C6's ADC1 block (GPIO0->CH0 ...), which is what we configure.
  adc_oneshot_unit_init_cfg_t ucfg = {};
  ucfg.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&ucfg, &s_badc) != ESP_OK) return false;

  s_bchan = (adc_channel_t)BOARD_BATT_ADC_GPIO;   // ADC1: channel == GPIO on C6
  adc_oneshot_chan_cfg_t ccfg = {};
  ccfg.atten    = ADC_ATTEN_DB_12;   // full-scale ~0..3.1 V at the pin (post-divider)
  ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_oneshot_config_channel(s_badc, s_bchan, &ccfg) != ESP_OK) return false;

  // Curve-fitting calibration (C6 scheme) so reads come back in real millivolts.
  adc_cali_curve_fitting_config_t cal = {};
  cal.unit_id  = ADC_UNIT_1;
  cal.chan     = s_bchan;
  cal.atten    = ADC_ATTEN_DB_12;
  cal.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_cali_create_scheme_curve_fitting(&cal, &s_bcali) != ESP_OK) s_bcali = nullptr;

  s_badc_ok = true;
  return true;
}

static void board_power_full_init(void)        {}
static void board_power_sleep_charge_cfg(void) {}

/* Per-board calibration trim defaults (no-op if the board header didn't set one). */
#ifndef BOARD_BATT_CAL_NUM
#define BOARD_BATT_CAL_NUM 1
#define BOARD_BATT_CAL_DEN 1
#endif

/* Battery millivolts: MEDIAN of several pin samples (rejects outliers so the
 * reading doesn't bounce), scaled up by the divider ratio, then trimmed by the
 * per-unit calibration factor. */
static uint16_t board_batt_voltage_mv(void) {
  if (!s_badc_ok) return 0;
  int s[9]; int n = 0;
  for (int i = 0; i < 9; i++) {
    int raw = 0;
    if (adc_oneshot_read(s_badc, s_bchan, &raw) != ESP_OK) continue;
    int mv = raw;
    if (s_bcali) adc_cali_raw_to_voltage(s_bcali, raw, &mv);
    s[n++] = mv;
  }
  if (!n) return 0;
  // Insertion sort (tiny n) then take the middle element = median pin mV.
  for (int i = 1; i < n; i++) {
    int v = s[i], j = i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
    s[j + 1] = v;
  }
  int pin_mv = s[n / 2];
  long batt = (long)pin_mv * BOARD_BATT_ADC_MUL;          // undo divider
  batt = batt * BOARD_BATT_CAL_NUM / BOARD_BATT_CAL_DEN;  // per-unit trim
  return (uint16_t)batt;
}

/* Rough LiPo state-of-charge from resting voltage (no coulomb counter). A
 * piecewise-linear fit of a single-cell discharge curve, ~3.05 V (0%) to the
 * board's full-charge ceiling 4.10 V (100%) — this board's HARDWARE charger tops
 * out at ~4.1 V (never the usual 4.2 V), so 4.10 V is "full" here. The 0% anchor
 * sits just above the firmware cutoff (BATT_CUTOFF_MV ~3.00 V) so the readout
 * reaches ~0% right as the watch protectively powers off. */
static int board_batt_percent(void) {
  uint16_t mv = board_batt_voltage_mv();
  if (mv == 0) return -1;
  static const struct { uint16_t mv; uint8_t pct; } C[] = {
    {3050,0},{3300,5},{3500,12},{3650,25},{3750,45},{3850,65},{3950,82},{4050,95},{4100,100}
  };
  if (mv <= C[0].mv) return 0;
  const int N = sizeof(C) / sizeof(C[0]);
  if (mv >= C[N-1].mv) return 100;
  for (int i = 1; i < N; i++) {
    if (mv < C[i].mv) {
      int span = C[i].mv - C[i-1].mv, into = mv - C[i-1].mv;
      int dp = C[i].pct - C[i-1].pct;
      return C[i-1].pct + (into * dp) / span;
    }
  }
  return 100;
}

/* No charger/VBUS sensing without a PMU. */
static uint16_t board_vbus_voltage_mv(void)      { return 0; }
static bool     board_is_charging(void)          { return false; }
static bool     board_vbus_in(void)              { return false; }
static float    board_pmu_temp_c(void)           { return -273.0f; }
static void     board_set_core_rail_mv(uint16_t) {}
static bool     board_power_acks(void)           { return s_badc_ok; }
static bool     board_power_off(void)            { return false; }  // no PMU -> caller deep-sleeps

/* No peripheral rails to probe (no PMU). Keep the same placeholder surface as
 * the bare stub so sleep_power.h compiles. */
enum { RAIL_COUNT = 1 };
static const char *rail_name(uint8_t)      { return "?"; }
static void        rail_set(uint8_t, bool) {}
static bool        rail_is_on(uint8_t)     { return false; }
static const uint8_t RAIL_DEFAULT_CUT[RAIL_COUNT] = { 0 };

#else  /* !BOARD_HAS_PMU_AXP2101 && !BOARD_HAS_ADC_BATTERY ----------------- */

/* No PMU: gauge unknown, rails don't exist, all writes are no-ops. Consumers
 * already degrade on board_power_ok()==false (the old pmu_ok==false path). */
static bool     board_power_ok(void)             { return false; }
static bool     board_power_begin(void)          { return false; }
static void     board_power_full_init(void)      {}
static void     board_power_sleep_charge_cfg(void) {}
static int      board_batt_percent(void)         { return -1; }
static uint16_t board_batt_voltage_mv(void)      { return 0; }
static uint16_t board_vbus_voltage_mv(void)      { return 0; }
static bool     board_is_charging(void)          { return false; }
static bool     board_vbus_in(void)              { return false; }
static bool     board_usb_powered(void)          { return false; }
static float    board_pmu_temp_c(void)           { return -273.0f; }
static void     board_set_core_rail_mv(uint16_t) {}
static bool     board_power_acks(void)           { return false; }
static bool     board_power_off(void)            { return false; }

/* Placeholder rail surface so sleep_power.h compiles until its rail machinery
 * is gated for PMU-less boards (next extraction step). RAIL_COUNT stays >=1
 * because s_rail_state[RAIL_COUNT] arrays exist; nothing iterates usefully
 * since every rail op is a no-op and board_power_ok() is false. */
enum { RAIL_COUNT = 1 };
static const char *rail_name(uint8_t)      { return "?"; }
static void        rail_set(uint8_t, bool) {}
static bool        rail_is_on(uint8_t)     { return false; }
static const uint8_t RAIL_DEFAULT_CUT[RAIL_COUNT] = { 0 };

#endif  /* BOARD_HAS_PMU_AXP2101 */
