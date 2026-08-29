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

#if BOARD_HAS_GAUGE_BQ27220
#include "gauge_bq27220.h"
#endif
#if BOARD_HAS_CHARGER_BQ25896
#include "charger_bq25896.h"
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
  power.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

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
  power.setSysPowerDownVoltage(3000);
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
 * Only peripheral outputs are candidates — never DCDC1 (ESP32 VCC3V3) or the
 * RTC supply. The first six are the classic peripheral LDOs (loads verified on
 * this board). The AUX tail is every REMAINING AXP2101 output: per the 2.06
 * schematic none of them has a visible load (DCDC5 is marked NC, DCDC2/3/4 have
 * no power inductor placed, DLDO1/2 + CPUSLDO show no net), so they only cost
 * converter/LDO quiescent current if the PMU's defaults left them enabled.
 * They're listed so the Power app can experiment with cutting them in sleep —
 * but they are NEVER force-enabled at boot (see RAIL_AUX_MASK below). */
enum { RAIL_ALDO1, RAIL_ALDO2, RAIL_ALDO3, RAIL_ALDO4, RAIL_BLDO1, RAIL_BLDO2,
       RAIL_DC2, RAIL_DC3, RAIL_DC4, RAIL_DC5, RAIL_DLDO1, RAIL_DLDO2, RAIL_CPUSLDO,
       RAIL_COUNT };

static const char *rail_name(uint8_t i) {
  static const char *N[RAIL_COUNT] = { "ALDO1","ALDO2","ALDO3","ALDO4","BLDO1","BLDO2",
                                       "DC2","DC3","DC4","DC5","DLDO1","DLDO2","CPULDO" };
  return (i < RAIL_COUNT) ? N[i] : "?";
}
static void rail_set(uint8_t i, bool on) {
  switch (i) {
    case RAIL_ALDO1:   on ? power.enableALDO1()   : power.disableALDO1();   break;
    case RAIL_ALDO2:   on ? power.enableALDO2()   : power.disableALDO2();   break;
    case RAIL_ALDO3:   on ? power.enableALDO3()   : power.disableALDO3();   break;
    case RAIL_ALDO4:   on ? power.enableALDO4()   : power.disableALDO4();   break;
    case RAIL_BLDO1:   on ? power.enableBLDO1()   : power.disableBLDO1();   break;
    case RAIL_BLDO2:   on ? power.enableBLDO2()   : power.disableBLDO2();   break;
    case RAIL_DC2:     on ? power.enableDC2()     : power.disableDC2();     break;
    case RAIL_DC3:     on ? power.enableDC3()     : power.disableDC3();     break;
    case RAIL_DC4:     on ? power.enableDC4()     : power.disableDC4();     break;
    case RAIL_DC5:     on ? power.enableDC5()     : power.disableDC5();     break;
    case RAIL_DLDO1:   on ? power.enableDLDO1()   : power.disableDLDO1();   break;
    case RAIL_DLDO2:   on ? power.enableDLDO2()   : power.disableDLDO2();   break;
    case RAIL_CPUSLDO: on ? power.enableCPUSLDO() : power.disableCPUSLDO(); break;
  }
}
static bool rail_is_on(uint8_t i) {
  switch (i) {
    case RAIL_ALDO1:   return power.isEnableALDO1();
    case RAIL_ALDO2:   return power.isEnableALDO2();
    case RAIL_ALDO3:   return power.isEnableALDO3();
    case RAIL_ALDO4:   return power.isEnableALDO4();
    case RAIL_BLDO1:   return power.isEnableBLDO1();
    case RAIL_BLDO2:   return power.isEnableBLDO2();
    case RAIL_DC2:     return power.isEnableDC2();
    case RAIL_DC3:     return power.isEnableDC3();
    case RAIL_DC4:     return power.isEnableDC4();
    case RAIL_DC5:     return power.isEnableDC5();
    case RAIL_DLDO1:   return power.isEnableDLDO1();
    case RAIL_DLDO2:   return power.isEnableDLDO2();
    case RAIL_CPUSLDO: return power.isEnableCPUSLDO();
  }
  return false;
}

/* Owner-validated default cut set for THIS board. ALDO1 MUST stay on — it feeds the
 * PCF85063 RTC VDD / shared-I2C bus, and cutting it in deep sleep leaves the RTC +
 * FT3168 touch unrecoverable on wake (verified: RTC-not-found + touch-init-fail,
 * fixed only by a cold reboot). Every OTHER rail (ALDO2/3/4, BLDO1/2) cuts cleanly
 * and is restored on wake by rails_restore()'s forced, sequenced LDO re-cycle, so
 * cut them all by default for the deep-sleep power saving.
 * The AUX rails (DC2..CPUSLDO) default to NOT cut: they're EXPERIMENTAL — no load
 * on the schematic, so their pre-firmware PMU state is left alone until you opt in
 * from Settings>Power. */
//                       ALDO1 ALDO2 ALDO3 ALDO4 BLDO1 BLDO2  DC2 DC3 DC4 DC5 DLDO1 DLDO2 CPUSLDO
static const uint8_t RAIL_DEFAULT_CUT[RAIL_COUNT] =
                       { 0,    1,    1,    1,    1,    1,     0,  0,  0,  0,  0,    0,    0 };

/* Rails that can NEVER be cut in deep sleep, as a bitmask over the rail enum, no
 * matter the cut set or the Power-app toggles. ALDO1 = the RTC/I2C supply (BIT0).
 * rails_cut_for_sleep() honours this as a hard guard. */
#define RAIL_NEVER_CUT_MASK  (1u << RAIL_ALDO1)

/* EXPERIMENTAL aux rails (bitmask): AXP2101 outputs with NO visible load on the
 * 2.06 schematic (DCDC2/3/4 have no inductor placed, DCDC5 is NC, DLDO1/DC1SW,
 * DLDO2/DC4SW and CPUSLDO show no net). Policy differences vs the classic rails
 * (enforced in sleep_power.h):
 *   - rails_restore() does NOT force-cycle them at boot: an aux rail the PMU's
 *     power-on defaults left OFF must stay off (force-enabling an inductor-less
 *     buck is exactly the kind of thing that could brown out VSYS). Only an aux
 *     rail you've opted IN to cutting is re-enabled on wake (it's "managed":
 *     on awake, off asleep — so the sleep/wake experiment is symmetrical).
 *   - rails_probe() restores an aux rail to its PRE-PROBE state, not forced on. */
#define RAIL_AUX_MASK  ((1u << RAIL_DC2)   | (1u << RAIL_DC3)   | (1u << RAIL_DC4) | \
                        (1u << RAIL_DC5)   | (1u << RAIL_DLDO1) | (1u << RAIL_DLDO2) | \
                        (1u << RAIL_CPUSLDO))

#elif BOARD_HAS_ADC_BATTERY  /* -------- ADC battery (no PMU, e.g. C6-1.47) --- */

/* No PMU chip, but the battery is brought to an ADC pin through a divider. We
 * read the pin in millivolts (the IDF calibration handles the ADC's nonlinear
 * curve), scale back up by the divider ratio, and estimate percent from a
 * LiPo voltage curve. There is no fuel-gauge IC, so percent is an estimate and
 * charge/VBUS state is unknown (returns false). board_power_ok() is TRUE so the
 * watch face + Power app show the live voltage/percent. No I2C lock needed —
 * the ADC is local to the SoC, not on the shared bus. */

#if BOARD_PLATFORM_TUYA  /* ===== T5-E1: TuyaOpen tkl_adc, not ESP-IDF adc_oneshot ===== */

/* The BK7258 reads the battery on GPIO13 -> TUYA_ADC_NUM_0, channel 11 (from the T5
 * Arduino variant's adcPinToChannel). We init the ADC once with that single channel
 * enabled and read millivolts directly via tkl_adc_read_voltage() (no separate cali
 * step needed - the HAL returns calibrated mV). These tkl_adc_* symbols are in
 * libtuyaos_adapter.a (verified present). */
extern "C" {
#include "tkl_adc.h"
#include "tkl_gpio.h"     // BAT_CHG (GPIO30) charge-status read
}
#define OWF_T5_BATT_ADC_PORT  TUYA_ADC_NUM_0
/* GPIO13 -> ADC channel 15. This is the REAL HAL channel, taken verbatim from Tuya's own
 * production battery driver for this board (TuyaOpen/apps/tuya.ai/your_chat_bot/src/battery/
 * app_battery.c: ADC_BATTERY_ADC_CHANNEL 15). The Arduino variant's adcPinToChannel(13)=11
 * is WRONG - that mismatch is why earlier reads were garbage (1V, 10V jumps). */
#define OWF_T5_BATT_ADC_CH    15
#define OWF_T5_BATT_CHG_PIN   TUYA_GPIO_NUM_30 /* BAT_CHG: LOW = charging (same source) */

/* This whole path mirrors Tuya's own production battery driver for THIS board
 * (TuyaOpen/apps/tuya.ai/your_chat_bot/src/battery/app_battery.c) - that's the authoritative
 * reference and it differs from my earlier guesses in three ways that ALL mattered:
 *   1. channel is 15 (not 11) - the real HAL channel for GPIO13.
 *   2. read via tkl_adc_read_voltage(), which returns MICROVOLTS -> divide by 1000 for mV.
 *   3. the ADC is init'd ONCE and left running. Tuya never deinits it, and their display is
 *      fine - so the GREEN TINT was NOT "ADC on", it was my init/deinit CHURN every read
 *      (each teardown+reinit reconfigures shared analog/clock state, disturbing the panel).
 *      Init once, read forever, never deinit. */
static bool s_badc_ok = false;

static bool board_power_ok(void) { return true; }

static bool board_power_begin(void) {
  if (s_badc_ok) return true;
  TUYA_ADC_BASE_CFG_T cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ch_list.data = (1u << OWF_T5_BATT_ADC_CH);   // channel 15
  cfg.ch_nums  = 1;
  cfg.width    = 12;
  cfg.mode     = TUYA_ADC_CONTINUOUS;
  cfg.type     = TUYA_ADC_INNER_SAMPLE_VOL;
  cfg.conv_cnt = 1;
  OPERATE_RET rt = tkl_adc_init(OWF_T5_BATT_ADC_PORT, &cfg);
  USBSerial.printf("[batt-init] tkl_adc_init(port=%d ch=%d) rt=%d\n",
                   (int)OWF_T5_BATT_ADC_PORT, (int)OWF_T5_BATT_ADC_CH, (int)rt);
  if (rt != OPRT_OK) return false;
  s_badc_ok = true;
  return true;
}

static void board_power_full_init(void)        {}
static void board_power_sleep_charge_cfg(void) {}

static uint16_t board_batt_voltage_mv(void);   // fwd: the USB latch clears on a voltage sag

/* USB-POWERED — BAT_CHG (GPIO30 = ETA6098 STAT) latched, cleared by a voltage sag.
 *
 * The board exposes NO direct VBUS-presence signal to the T5 (verified against the schematic
 * netlist + the ETA6098 and CH342F datasheets): VBUS reaches no T5 pin; the CH342F's only
 * T5-readable output (TXD0) auto-suspends because the host never opens UART0; its ACT# pin
 * isn't bonded out; the BK7258 PMU/SCTRL VBUS bits don't read from the app core. So BAT_CHG
 * (charge status) is the only USB-correlated signal we have.
 *
 * STAT alone is insufficient — per its datasheet it's LOW only while CURRENT is flowing and
 * goes HIGH-Z the moment charging completes, so on a near-full cell it drops within ~5s of
 * plug-in even though the cable is in (the reported bug). Fix = LATCH with HYSTERESIS so brief
 * ADC noise can't permanently release it:
 *   SET (USB on):  STAT LOW (charging/recharge pulse) OR cell voltage >= OWF_T5_USB_SET_MV.
 *   CLEAR (USB off): only when cell voltage < OWF_T5_USB_CLR_MV.
 * While USB is plugged the charger holds the rail at ~4.22V, so voltage sits ABOVE the SET
 * threshold and keeps re-latching even between auto-recharge pulses — a momentary dip below the
 * CLEAR threshold (noise/load spike) immediately re-latches on the next read once it recovers.
 * The cell can only stay below CLEAR once the cable is actually pulled and the running watch
 * drains the pack. The SET>CLEAR gap is the hysteresis band that rejects transient dips.
 *
 * Re-init the pin each read so the live sample survives the CPU light-sleep that froze a
 * once-configured read; callers are throttled (~2 Hz) so the cost is negligible. */
/* SET high (re-latch) on charging OR a held-up rail; CLEAR (unplugged) only on a cell that
 * stays WELL below the charge floor. CLR was 4150 — but a charge-terminated cell ON USB
 * legitimately sags toward the ~4.1V hardware ceiling between the charger's recharge pulses,
 * dipping below 4150 for seconds at a time. That flickered the latch off (observed usb 1->0->1
 * while plugged), which un-blocked auto-dim -> "dims on USB". A real UNPLUG, with the running
 * watch draining the pack, falls much further. So CLR is low (4000mV) AND debounced: the cell
 * must read below CLR for OWF_T5_USB_CLR_STREAK consecutive polls before we declare unplugged.
 * Any charging pulse or a recovered voltage resets the streak and re-latches. */
#define OWF_T5_USB_SET_MV     4150   /* cell mV >= this => rail held up by USB -> (re)latch ON */
#define OWF_T5_USB_CLR_MV     3900   /* cell mV <  this (sustained) => unplugged + draining */
#define OWF_T5_USB_CLR_STREAK 5      /* consecutive sub-CLR polls before clearing (~debounce) */
static bool board_usb_powered(void) {
  static bool s_usb_latched = false;
  static uint8_t s_low_streak = 0;

  TUYA_GPIO_BASE_CFG_T in = {};
  in.mode   = TUYA_GPIO_PULLUP;
  in.direct = TUYA_GPIO_INPUT;
  tkl_gpio_init(OWF_T5_BATT_CHG_PIN, &in);
  TUYA_GPIO_LEVEL_E lvl = TUYA_GPIO_LEVEL_HIGH;
  tkl_gpio_read(OWF_T5_BATT_CHG_PIN, &lvl);
  bool charging = (lvl == TUYA_GPIO_LEVEL_LOW);
  uint16_t mv = board_batt_voltage_mv();

  if (charging || mv >= OWF_T5_USB_SET_MV) {
    s_usb_latched = true;                       // charging, or rail held up -> on USB
    s_low_streak  = 0;                          // any sign of USB resets the unplug streak
  } else if (s_usb_latched) {
    if (mv < OWF_T5_USB_CLR_MV) {
      if (s_low_streak < 255) s_low_streak++;
      if (s_low_streak >= OWF_T5_USB_CLR_STREAK) s_usb_latched = false;  // sustained low -> unplugged
    } else {
      s_low_streak = 0;                         // recovered above CLR -> not unplugged
    }
  }
  return s_usb_latched;
}

/* Battery millivolts. tkl_adc_read_voltage() returns MICROVOLTS at the pin; /1000 -> mV,
 * then x4 for the divider (Tuya's exact ratio for the 2M/510K network - the HAL's calibrated
 * read already folds in the ADC reference, so the effective multiplier is 4, not 2510/510).
 * The board header's MUL/CAL provide a fine-trim on top (default 4 via CAL, see board.h). */
static uint16_t board_batt_voltage_mv(void) {
  static uint16_t s_last_mv = 0;
  if (!s_badc_ok && !board_power_begin()) return s_last_mv;

  // CACHE: every UI surface (watch face, Power-app graph, the number under it, the detail
  // block) calls board_batt_percent()/board_batt_voltage_mv() INDEPENDENTLY. The ADC drifts a
  // few mV between reads, and near the top of the percent curve that swings the % (this is why
  // the three readouts disagreed - 90/98/100). So we read the ADC at most once per
  // OWF_BATT_CACHE_MS and return the SAME value to all callers in between -> they always agree.
  // millis() is the Arduino tick; cheap and monotonic. The battery moves far slower than this.
  #define OWF_BATT_CACHE_MS 3000
  static uint32_t s_last_read_ms = 0;
  uint32_t now_ms = millis();
  if (s_last_mv != 0 && (now_ms - s_last_read_ms) < OWF_BATT_CACHE_MS)
    return s_last_mv;                              // serve cached -> all callers see one value

  // tkl_adc_read_voltage on THIS core (Arduino-TuyaOpen) returns MILLIVOLTS at the pin
  // directly - NOT microvolts. (The native SDK app divides by 1000 because that build's HAL
  // returns uV; this one does not. Confirmed live: uv=870 with raw=2190 -> 870 IS the pin mV.)
  int32_t pin_mv_raw = 0;
  OPERATE_RET rt = tkl_adc_read_voltage(OWF_T5_BATT_ADC_PORT, &pin_mv_raw, 1);
  if (rt != OPRT_OK) return s_last_mv;             // read failed -> hold last
  int pin_mv = (int)pin_mv_raw;
  s_last_read_ms = now_ms;

  long batt = (long)pin_mv * BOARD_BATT_ADC_MUL;
  batt = batt * BOARD_BATT_CAL_NUM / BOARD_BATT_CAL_DEN;

  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    USBSerial.printf("[batt] pin=%dmV -> cell=%ldmV (x%d*%d/%d)\n",
                     pin_mv, batt, (int)BOARD_BATT_ADC_MUL,
                     (int)BOARD_BATT_CAL_NUM, (int)BOARD_BATT_CAL_DEN);
  }
  s_last_mv = (uint16_t)batt;
  return s_last_mv;
}

#else  /* ===== ESP-IDF boards (C6-1.47, S3-1.47): adc_oneshot ===== */

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "hal/adc_types.h"
#include "soc/adc_channel.h"

static adc_oneshot_unit_handle_t s_badc      = nullptr;
static adc_cali_handle_t          s_bcali     = nullptr;
static bool                       s_badc_ok   = false;
static adc_unit_t                 s_bunit     = ADC_UNIT_1;
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
  // Resolve the battery GPIO to its ADC unit+channel. Don't assume "channel ==
  // GPIO": that holds on the C6's ADC1 (GPIO0->CH0...) but NOT on the S3, where
  // the battery pin (GPIO12) is on ADC2_CH1. adc_oneshot_io_to_channel() reads the
  // SoC's pad map and returns the correct (unit, channel) for whatever board this
  // is — so the same code serves the C6 (ADC1) and the S3 (ADC2).
  if (adc_oneshot_io_to_channel(BOARD_BATT_ADC_GPIO, &s_bunit, &s_bchan) != ESP_OK)
    return false;

  adc_oneshot_unit_init_cfg_t ucfg = {};
  ucfg.unit_id = s_bunit;
  if (adc_oneshot_new_unit(&ucfg, &s_badc) != ESP_OK) return false;

  adc_oneshot_chan_cfg_t ccfg = {};
  ccfg.atten    = ADC_ATTEN_DB_12;   // full-scale ~0..3.1 V at the pin (post-divider)
  ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_oneshot_config_channel(s_badc, s_bchan, &ccfg) != ESP_OK) return false;

  // Curve-fitting calibration so reads come back in real millivolts.
  adc_cali_curve_fitting_config_t cal = {};
  cal.unit_id  = s_bunit;
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
  // Last good reading. On the S3 the battery pin is on ADC2, which the WiFi driver
  // owns while the radio is active -> adc_oneshot_read() returns ESP_ERR_TIMEOUT/
  // _NOT_FOUND for the duration. The battery moves slowly and is polled rarely, so
  // when a whole sample set collides with WiFi we just return the previous value
  // instead of 0 (which would read as a dead/uninitialized cell and could trip the
  // low-battery cutoff). On the C6 (ADC1) reads never collide, so this is a no-op.
  static uint16_t s_last_mv = 0;
  int s[9]; int n = 0;
  for (int i = 0; i < 9; i++) {
    int raw = 0;
    if (adc_oneshot_read(s_badc, s_bchan, &raw) != ESP_OK) continue;
    int mv = raw;
    if (s_bcali) adc_cali_raw_to_voltage(s_bcali, raw, &mv);
    s[n++] = mv;
  }
  if (!n) return s_last_mv;   // every read failed (e.g. ADC2 busy with WiFi) -> hold last
  // Insertion sort (tiny n) then take the middle element = median pin mV.
  for (int i = 1; i < n; i++) {
    int v = s[i], j = i - 1;
    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
    s[j + 1] = v;
  }
  int pin_mv = s[n / 2];
  long batt = (long)pin_mv * BOARD_BATT_ADC_MUL;          // undo divider
  batt = batt * BOARD_BATT_CAL_NUM / BOARD_BATT_CAL_DEN;  // per-unit trim
  s_last_mv = (uint16_t)batt;
  return s_last_mv;
}

#endif  /* BOARD_PLATFORM_TUYA ADC vs ESP-IDF ADC ---------------------------- */

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
    {3050,0},{3300,15},{3500,32},{3650,55},{3750,65},{3850,82},{3900,90},{4000,99},{4200,100}
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

/* No PMU charger IC to query, but we DO know when USB is attached
 * (usb_serial_jtag host present = board_usb_powered). On this hardware USB is the
 * charge source, so treat "USB attached" as both VBUS-in and charging: there's no
 * charge-complete signal, so we can't distinguish "topping off" from "full", but
 * reporting charging while plugged in is what drives the UI's USB/charging state
 * (purple battery + "USB" readout). VBUS voltage is unmeasured -> report nominal
 * 5 V when present so the Power menu can show "USB power: yes (5.00 V)". */
static bool     board_vbus_in(void)              { return board_usb_powered(); }
static bool     board_is_charging(void)          { return board_usb_powered(); }
static uint16_t board_vbus_voltage_mv(void)      { return board_usb_powered() ? 5000 : 0; }
static float    board_pmu_temp_c(void)           { return -273.0f; }
static void     board_set_core_rail_mv(uint16_t) {}
#if BOARD_PLATFORM_TUYA
static bool     board_power_acks(void)           { return true; }   // ADC opened per-read
#else
static bool     board_power_acks(void)           { return s_badc_ok; }
#endif
static bool     board_power_off(void)            { return false; }  // no PMU -> caller deep-sleeps

/* No peripheral rails to probe (no PMU). Keep the same placeholder surface as
 * the bare stub so sleep_power.h compiles. */
enum { RAIL_COUNT = 1 };
static const char *rail_name(uint8_t)      { return "?"; }
static void        rail_set(uint8_t, bool) {}
static bool        rail_is_on(uint8_t)     { return false; }
static const uint8_t RAIL_DEFAULT_CUT[RAIL_COUNT] = { 0 };

#else  /* !BOARD_HAS_PMU_AXP2101 && !BOARD_HAS_ADC_BATTERY ----------------- */

/* No XPowersLib/ADC path. On the Maix platform the on-board AXP2101 is read via
 * MaixCDK's PMU (owf_maix_* bridge in owf_maix_hooks.h); otherwise this is the
 * bare no-PMU stub (gauge unknown, all writes no-ops). */
#if BOARD_PLATFORM_MAIX
static bool     board_power_ok(void)             { return owf_maix_pmu_ok() != 0; }
static bool     board_power_begin(void)          { return owf_maix_pmu_begin() != 0; }
static void     board_power_full_init(void)      {}
static void     board_power_sleep_charge_cfg(void) {}
static int      board_batt_percent(void)         { return owf_maix_bat_percent(); }
static uint16_t board_batt_voltage_mv(void)      { return (uint16_t)owf_maix_bat_mv(); }
static uint16_t board_vbus_voltage_mv(void)      { return owf_maix_vbus_in() ? 5000 : 0; }
static bool     board_is_charging(void)          { return owf_maix_is_charging() != 0; }
static bool     board_vbus_in(void)              { return owf_maix_vbus_in() != 0; }
static bool     board_usb_powered(void)          { return board_vbus_in(); }
static float    board_pmu_temp_c(void)           { return -273.0f; }
#elif BOARD_PLATFORM_FOSSIL
/* Fossil: the PMIC fuel gauge / charger, read over SPMI by the bare-metal
 * runtime (fossil-port pmic_fg.c — real on the Gen 6's PM660 FG-GEN3, honest
 * -1 stubs on the Gen 4). The gauge runs autonomously with the profile the
 * stock OS programmed, so there is no begin()/config step: power_ok is simply
 * "does a SoC read answer sanely". No I2C involved — no bus lock needed. */
extern "C" int fg_batt_percent(void);
extern "C" int fg_batt_mv(void);
extern "C" int fg_batt_ma(void);      /* + = discharging; -32768 on error */
extern "C" int fg_batt_temp_dc(void);
extern "C" int chg_usb_present(void);
extern "C" int chg_charging(void);
/* FG/CHARGER RE-ENABLED (2026-08-07) — VALIDATED ON HARDWARE. The 2026-08-03
 * disable ("plausible junk" + suspected TZ/XPU reset) predated the SPMI
 * arbiter v2 APID-table fix (fossil-port spmi_arb.c): reads through channel 0
 * hit whatever peripheral was first in the arb table, which is exactly where
 * the junk came from. The pwr1 census re-test through the fixed arbiter read
 * soc=87 vb=4190 ib=-3 chg=0x45 usb=0x10 tbat=433 — every value plausible and
 * mutually consistent (near-full cell trickling on USB, charger in terminate)
 * — and the deferred-first-read canary proved no XPU reset. Gen 6 only; the
 * Gen 4's PM8916 VM-BMS still returns the honest -1 stubs.
 *
 * FAIL-SAFES KEPT (learned 2026-08-03, they stay forever): the low-battery
 * cutoff protects a KNOWN-dying, UNPLUGGED cell, so implausible readings must
 * never power the watch off:
 *   - a voltage below any boot-capable level (< 2500 mV) reports 0 (INVALID),
 *     which the cutoff logic already ignores;
 *   - an ERRORED usb-present read (-1) counts as PRESENT, never absent. */
static int      board_batt_percent(void)
{
    /* Full-clamp: this cell sits at/above 4.15 V only when charge-terminated,
     * but the FG's learned capacity can lag and report 9x% there. Anything
     * >= 4150 mV is a full battery — report 100. */
    int pct = fg_batt_percent();
    if (pct >= 0 && fg_batt_mv() >= 4150) return 100;
    return pct;
}
static bool     board_power_ok(void)             { return fg_batt_percent() >= 0; }
static bool     board_power_begin(void)          { return board_power_ok(); }
static void     board_power_full_init(void)      {}   /* gauge runs autonomously */
static void     board_power_sleep_charge_cfg(void) {}
static uint16_t board_batt_voltage_mv(void)
{
    int mv = fg_batt_mv();
    return (mv < 2500) ? 0 : (uint16_t)mv;           /* fail-safe: junk = 0 */
}
static bool     board_vbus_in(void)
{
    return chg_usb_present() != 0;                   /* fail-safe: err = present */
}
static bool     board_usb_powered(void)          { return board_vbus_in(); }
static uint16_t board_vbus_voltage_mv(void)
{
    /* No validated VBUS ADC channel yet — report nominal USB when present. */
    return board_vbus_in() ? 5000 : 0;
}
static bool     board_is_charging(void)          { return chg_charging() == 1; }
static float    board_pmu_temp_c(void)
{
    /* Closest analog to the AXP2101 "PMU die" reading: the FG's battery
     * thermistor (charge-path heat shows up here the same way). */
    int dc = fg_batt_temp_dc();
    return (dc == -9999) ? -273.0f : (float)dc / 10.0f;
}
#elif BOARD_HAS_GAUGE_BQ27220 || BOARD_HAS_CHARGER_BQ25896
/* ===== BQ27220 fuel gauge + BQ25896 charger (T-Deck Pro) ====================
 * Two chips, two jobs, both on the shared I2C bus:
 *   - the GAUGE answers questions about the CELL (percent, mV, temp, health).
 *     It coulomb-counts autonomously, including through deep sleep, so there is
 *     nothing to initialise and nothing to service — reads are always current.
 *   - the CHARGER answers questions about the INPUT (VBUS present, charge state).
 *
 * Presence is probed ONCE at boot and cached: these helpers are called from the
 * loop (the VBUS poll runs twice a second) and re-probing a missing chip on
 * every call would put a doomed I2C transaction on the shared bus each time.
 * A chip that fails its probe reports the standard "no data" sentinels, so the
 * UI degrades exactly as it does on a board with no battery hardware at all. */
static bool s_bq_gauge_ok = false;
static bool s_bq_chg_ok   = false;

static bool     board_power_begin(void)
{
#if BOARD_HAS_GAUGE_BQ27220
  s_bq_gauge_ok = bq27220_begin();
#endif
#if BOARD_HAS_CHARGER_BQ25896
  s_bq_chg_ok   = bq25896_begin();
#endif
  return s_bq_gauge_ok || s_bq_chg_ok;
}
/* "Power subsystem usable" gates the Power app's graphs and the battery UI. The
 * GAUGE is what those need; a charger alone gives no percentage to plot. */
static bool     board_power_ok(void)             { return s_bq_gauge_ok; }
static void     board_power_full_init(void)      {}   /* both run autonomously */
static void     board_power_sleep_charge_cfg(void) {}

#if BOARD_HAS_GAUGE_BQ27220
static int      board_batt_percent(void)         { return s_bq_gauge_ok ? bq27220_soc() : -1; }
static uint16_t board_batt_voltage_mv(void)      { return s_bq_gauge_ok ? bq27220_voltage_mv() : 0; }
static float    board_pmu_temp_c(void)           { return s_bq_gauge_ok ? bq27220_temp_c() : -273.0f; }
#else
static int      board_batt_percent(void)         { return -1; }
static uint16_t board_batt_voltage_mv(void)      { return 0; }
static float    board_pmu_temp_c(void)           { return -273.0f; }
#endif

#if BOARD_HAS_CHARGER_BQ25896
/* VBUS presence comes from the CHARGER's power-good bit, not from the gauge's
 * current sign: "is the watch plugged in" must stay true when the battery is
 * full and current has fallen to zero. */
static bool     board_vbus_in(void)              { return s_bq_chg_ok && bq25896_vbus_present(); }
static bool     board_is_charging(void)          { return s_bq_chg_ok && bq25896_is_charging(); }
static uint16_t board_vbus_voltage_mv(void)      { return s_bq_chg_ok ? bq25896_vbus_mv() : 0; }
#elif BOARD_HAS_GAUGE_BQ27220
/* Gauge only: fall back to its charging flag as a proxy for "on power". Weaker —
 * it goes false once charge terminates even though USB is still attached. */
static bool     board_vbus_in(void)              { return s_bq_gauge_ok && bq27220_is_charging(); }
static bool     board_is_charging(void)          { return board_vbus_in(); }
static uint16_t board_vbus_voltage_mv(void)      { return board_vbus_in() ? 5000 : 0; }
#endif
static bool     board_usb_powered(void)          { return board_vbus_in(); }

#else
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
#endif
static void     board_set_core_rail_mv(uint16_t) {}
static bool     board_power_acks(void)           { return false; }
#if BOARD_PLATFORM_FOSSIL
/* Fossil Gen 4 has no accessible PMU power-off path yet (PM8916 over SPMI is a
 * later driver) AND no button force-reset, so the useful "power off" action on
 * this watch is a warm reboot back to the stock boot chain: the screen goes
 * dark, the user is out of our firmware, and the watch comes back to Wear OS
 * (from which it can be re-launched or re-flashed). reboot_now() is the
 * bare-metal msm8909 reset (platform/reboot_msm.c); it does not return.
 * Returns true because it DID issue the shutdown — the caller must NOT then
 * fall through to a deep-sleep path. */
extern "C" void reboot_now(void);
static bool     board_power_off(void)            { reboot_now(); return true; }
#else
static bool     board_power_off(void)            { return false; }
#endif

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
