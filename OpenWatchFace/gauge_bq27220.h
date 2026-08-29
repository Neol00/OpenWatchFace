/* ============================================================================
 *  gauge_bq27220.h — TI BQ27220 battery fuel gauge over I2C (T-Deck Pro)
 *
 *  A REAL coulomb-counting gauge, not a voltage guess. Every other board in this
 *  firmware either reads an ADC divider (and infers percent from a discharge
 *  curve) or asks an AXP2101 PMU. This chip integrates current over time and
 *  reports actual state-of-charge, remaining mAh, and health — so the readings
 *  here are better than anything the other targets can produce.
 *
 *  It also runs AUTONOMOUSLY: the gauge tracks the cell continuously on its own
 *  power domain, including while the SoC is in deep sleep. There is no
 *  initialisation sequence and no periodic servicing to do — you just read
 *  registers. That is why gauge_begin() only probes for presence.
 *
 *  ============ REGISTER MAP (standard command set, 16-bit little-endian) ======
 *    0x06  Temperature        0.1 K   (NOT Celsius — see the conversion below)
 *    0x08  Voltage            mV
 *    0x0A  BatteryStatus      flags   (bit 0 = DSG: 1 = discharging)
 *    0x0C  Current            mA      SIGNED: negative = discharging
 *    0x10  RemainingCapacity  mAh
 *    0x12  FullChargeCapacity mAh
 *    0x2C  StateOfCharge      %       0..100, the headline number
 *    0x3C  DesignCapacity     mAh
 *
 *  Every value is a 16-bit little-endian word read as two bytes from the command
 *  address. Temperature is in DECIKELVIN: degC = raw/10 - 273.15. Reading it as
 *  Celsius directly gives ~2980 and looks like a broken sensor.
 *
 *  Current is SIGNED (int16). A positive value means charge flowing IN. Treating
 *  it as unsigned makes any discharge read as ~65000 mA.
 *
 *  THREADING: the caller holds i2c_lock() — the touch controller, keyboard,
 *  charger and IMU all share this bus. Nothing here locks on its own.
 * ========================================================================== */
#pragma once
#if BOARD_HAS_GAUGE_BQ27220

#include <Wire.h>

#define BQ27220_ADDR              0x55

#define BQ27220_REG_TEMPERATURE   0x06
#define BQ27220_REG_VOLTAGE       0x08
#define BQ27220_REG_BATT_STATUS   0x0A
#define BQ27220_REG_CURRENT       0x0C
#define BQ27220_REG_REMAIN_CAP    0x10
#define BQ27220_REG_FULL_CAP      0x12
#define BQ27220_REG_SOC           0x2C
#define BQ27220_REG_DESIGN_CAP    0x3C

/* BatteryStatus bit 0: set while the pack is DISCHARGING. */
#define BQ27220_STATUS_DSG        0x0001

/* Read one 16-bit little-endian word. Returns false on any I2C error, so an
 * absent or wedged gauge degrades to "no reading" rather than feeding garbage
 * into the battery UI and the low-battery cutoff. */
static bool bq27220_read_word(uint8_t reg, uint16_t *out) {
  Wire.beginTransmission(BQ27220_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)BQ27220_ADDR, (uint8_t)2) != 2) return false;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  *out = (uint16_t)((uint16_t)hi << 8 | lo);
  return true;
}

/* Probe for the gauge. DesignCapacity is used as the liveness check rather than
 * SoC or voltage: it is a non-zero constant on any configured pack, whereas a
 * freshly reset gauge can legitimately report 0% or 0 mV for a moment and would
 * then look absent. */
static bool bq27220_begin(void) {
  uint16_t v = 0;
  if (!bq27220_read_word(BQ27220_REG_DESIGN_CAP, &v)) return false;
  return v != 0;
}

/* State of charge, 0..100. Returns -1 when unreadable — the sentinel the rest of
 * the firmware already uses for "no battery data" (board_batt_percent). */
static int bq27220_soc(void) {
  uint16_t v = 0;
  if (!bq27220_read_word(BQ27220_REG_SOC, &v)) return -1;
  if (v > 100) return -1;                   /* out of range = not trustworthy */
  return (int)v;
}

/* Pack voltage in mV, 0 when unreadable. */
static uint16_t bq27220_voltage_mv(void) {
  uint16_t v = 0;
  if (!bq27220_read_word(BQ27220_REG_VOLTAGE, &v)) return 0;
  return v;
}

/* Signed pack current in mA: POSITIVE = charging in, NEGATIVE = discharging. */
static int16_t bq27220_current_ma(void) {
  uint16_t v = 0;
  if (!bq27220_read_word(BQ27220_REG_CURRENT, &v)) return 0;
  return (int16_t)v;                        /* reinterpret as signed */
}

/* True while the gauge says the pack is taking charge.
 *
 * Uses the BatteryStatus DSG flag rather than the sign of Current, because
 * current hovers around zero when charge terminates and a sign test then
 * flickers between "charging" and "not" every read. The flag is the gauge's own
 * debounced answer. */
static bool bq27220_is_charging(void) {
  uint16_t st = 0;
  if (!bq27220_read_word(BQ27220_REG_BATT_STATUS, &st)) return false;
  return (st & BQ27220_STATUS_DSG) == 0;    /* DSG clear = not discharging */
}

/* Battery temperature in degrees C. Returns -273.0f when unreadable, matching
 * the "no reading" convention board_pmu_temp_c() already uses.
 * Raw units are 0.1 KELVIN — see the header note. */
static float bq27220_temp_c(void) {
  uint16_t v = 0;
  if (!bq27220_read_word(BQ27220_REG_TEMPERATURE, &v)) return -273.0f;
  return (float)v / 10.0f - 273.15f;
}

/* Remaining / full-charge capacity in mAh (0 when unreadable). Their ratio is
 * effectively SoC; exposed separately because the Power app can show real mAh,
 * which no other board in this firmware can provide. */
static uint16_t bq27220_remaining_mah(void) {
  uint16_t v = 0;
  return bq27220_read_word(BQ27220_REG_REMAIN_CAP, &v) ? v : 0;
}
static uint16_t bq27220_full_mah(void) {
  uint16_t v = 0;
  return bq27220_read_word(BQ27220_REG_FULL_CAP, &v) ? v : 0;
}

/* State of health as a percentage of design capacity: how much the pack has aged.
 * Returns -1 if either capacity is unreadable. */
static int bq27220_health_pct(void) {
  uint16_t full = bq27220_full_mah();
  uint16_t design = 0;
  if (full == 0) return -1;
  if (!bq27220_read_word(BQ27220_REG_DESIGN_CAP, &design) || design == 0) return -1;
  return (int)((uint32_t)full * 100u / design);
}

#endif  /* BOARD_HAS_GAUGE_BQ27220 */
