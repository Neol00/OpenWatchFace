/* ============================================================================
 *  charger_bq25896.h — TI BQ25896 battery charger over I2C (T-Deck Pro)
 *
 *  Companion to gauge_bq27220.h. The gauge knows about the CELL; this chip knows
 *  about the INPUT — whether USB is actually supplying power, and what the
 *  charge state machine is doing. Those are different questions, and the
 *  firmware needs both:
 *      board_usb_powered() / board_vbus_in()  -> this file (VBUS present?)
 *      board_batt_percent() / voltage / temp  -> the gauge
 *
 *  WHY VBUS MUST COME FROM HERE. Without a real VBUS signal the firmware has no
 *  way to know it is plugged in, and two behaviours break: the idle-sleep guard
 *  (which must not sleep while tethered) and the low-battery cutoff (which must
 *  never trip on USB). The previous state of this board — no charger driver —
 *  meant board_usb_powered() returned a hard-coded false and the watch face fell
 *  back to printing "USB" as a placeholder for "no battery data at all", which
 *  read exactly like a stuck USB detection. It was neither: it was nothing.
 *
 *  Note the RED CHARGE LED on the board is driven by the BQ25896 in HARDWARE and
 *  is lit whenever VBUS is present. It is not firmware-controlled and its being
 *  on says nothing about what this driver reports.
 *
 *  ============ REGISTER MAP (8-bit registers) ================================
 *    0x0B  STATUS   bit 2   : PG_STAT   1 = power good (VBUS valid)
 *                  bits 4-3 : CHRG_STAT 00 not charging, 01 pre-charge,
 *                                       10 fast charging, 11 charge done
 *                  bits 7-5 : VBUS_STAT (input source type)
 *    0x0C  FAULT    bit 3   : BAT_FAULT, bits 5-4 CHRG_FAULT
 *    0x0E  BATV     bit 7   : THERM_STAT, bits 6-0 battery voltage ADC
 *    0x11  VBUSV    bits 6-0: VBUS voltage ADC (offset 2600 mV, 100 mV/LSB)
 *
 *  The ADC in this chip is ONE-SHOT by default and must be kicked (REG02 bit 7,
 *  CONV_START) before VBUSV/BATV hold fresh values. Presence detection uses the
 *  PG_STAT status bit instead, which is always live and needs no conversion —
 *  so the common path costs one register read and never has to wait.
 *
 *  THREADING: the caller holds i2c_lock(). Nothing here locks on its own.
 * ========================================================================== */
#pragma once
#if BOARD_HAS_CHARGER_BQ25896

#include <Wire.h>

#define BQ25896_ADDR            0x6B

#define BQ25896_REG_ADC_CTRL    0x02   /* bit 7 CONV_START, bit 6 CONV_RATE */
#define BQ25896_REG_MISC_CTRL   0x07   /* bit 6 STAT_DIS (1 = STAT pin/LED off) */
#define BQ25896_REG_STATUS      0x0B
#define BQ25896_REG_FAULT       0x0C
#define BQ25896_REG_BATV        0x0E
#define BQ25896_REG_VBUSV       0x11

#define BQ25896_PG_STAT         0x04   /* REG0B bit 2: power good */
#define BQ25896_CHRG_STAT_MASK  0x18   /* REG0B bits 4-3 */
#define BQ25896_CHRG_STAT_SHIFT 3

static bool bq25896_read_reg(uint8_t reg, uint8_t *out) {
  Wire.beginTransmission(BQ25896_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)BQ25896_ADDR, (uint8_t)1) != 1) return false;
  *out = Wire.read();
  return true;
}

static bool bq25896_write_reg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(BQ25896_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

/* ---- STAT pin / charge LED ------------------------------------------------
 * The red LED next to the USB port is wired to the charger's STAT output. By
 * DEFAULT the BQ25896 drives it from its own state machine, which lights it
 * whenever an input is present — including when charging has terminated, and
 * including after the firmware has "powered off", because the charger keeps
 * running on VBUS regardless of what the SoC is doing. That is why it appeared
 * to be stuck on: nothing was controlling it.
 *
 * REG07 bit 6 (STAT_DIS) gates the pin: SET = LED forced off, CLEAR = LED driven
 * by the charge state machine. So "only lit while actually charging" is:
 *      charging -> clear the bit (hardware drives it, and the state IS charging)
 *      otherwise -> set the bit  (forced off)
 * Read-modify-write, so the other bits in REG07 (watchdog, timers) are kept. */
static bool bq25896_set_led(bool on) {
  uint8_t v = 0;
  if (!bq25896_read_reg(BQ25896_REG_MISC_CTRL, &v)) return false;
  uint8_t nv = on ? (uint8_t)(v & ~0x40) : (uint8_t)(v | 0x40);
  if (nv == v) return true;                    /* already in the wanted state */
  return bq25896_write_reg(BQ25896_REG_MISC_CTRL, nv);
}

/* Probe, and DISABLE THE I2C WATCHDOG.
 *
 * THE WATCHDOG IS WHY THE LED WOULD NOT STAY OFF. REG07 bits 5:4 hold an I2C
 * watchdog timer, enabled by default at 40 s. Its whole purpose is to protect a
 * charger whose host has crashed: when it expires the chip RESETS ITS REGISTERS
 * TO DEFAULTS and resumes safe standalone charging. That reset also clears
 * STAT_DIS (bit 6 of the very same register), so any "LED off" we write is undone
 * ~40 s later and the red LED comes back on by itself.
 *
 * Because a firmware that never writes the charger is exactly what the watchdog
 * is watching for, and we only write it when something changes, the LED would
 * relight and stay lit. Disabling the watchdog is the correct fix: this host does
 * not crash-and-leave-the-charger-misconfigured, and we want our register writes
 * to be durable.
 *
 * Bits 5:4 = 00 disables it. Read-modify-write to preserve the rest of REG07. */
static bool bq25896_begin(void) {
  uint8_t v = 0;
  if (!bq25896_read_reg(BQ25896_REG_STATUS, &v)) return false;

  uint8_t misc = 0;
  if (bq25896_read_reg(BQ25896_REG_MISC_CTRL, &misc)) {
    uint8_t nv = (uint8_t)(misc & ~0x30);        /* WATCHDOG bits 5:4 -> 00 = off */
    if (nv != misc) bq25896_write_reg(BQ25896_REG_MISC_CTRL, nv);
  }
  return true;
}

/* Is USB (VBUS) present and valid?
 *
 * PG_STAT means the input source is good enough to run from — exactly the
 * question the idle-sleep guard and the low-battery cutoff are asking. Returns
 * false if the charger cannot be read, which is the SAFE direction for sleep
 * (a watch that sleeps when it should not is recoverable; one that never sleeps
 * silently drains the cell). */
static bool bq25896_vbus_present(void) {
  uint8_t st = 0;
  if (!bq25896_read_reg(BQ25896_REG_STATUS, &st)) return false;
  return (st & BQ25896_PG_STAT) != 0;
}

/* Charge-state machine: 0 = not charging, 1 = pre-charge, 2 = fast charging,
 * 3 = charge terminated/done. Returns -1 if unreadable.
 *
 * Distinct from the gauge's is-charging flag: this reports what the CHARGER is
 * doing, so state 3 (done, sitting on USB at 100%) is correctly not "charging",
 * while the gauge would just say current is ~0. */
static int bq25896_charge_state(void) {
  uint8_t st = 0;
  if (!bq25896_read_reg(BQ25896_REG_STATUS, &st)) return -1;
  return (int)((st & BQ25896_CHRG_STAT_MASK) >> BQ25896_CHRG_STAT_SHIFT);
}

/* True while the charger is actively pushing current into the cell (pre-charge
 * or fast charge). Charge-done is deliberately NOT "charging". */
static bool bq25896_is_charging(void) {
  int s = bq25896_charge_state();
  return s == 1 || s == 2;
}

/* Keep the STAT LED matching "actually charging". Call periodically (the VBUS
 * poll in loop() is the natural place — twice a second is plenty).
 *
 * DELIBERATELY NOT CACHED. An earlier version remembered the last state it wrote
 * and skipped the I2C write when unchanged. That is the obvious optimisation and
 * it is WRONG here: the charger can reset REG07 to defaults behind our back (the
 * I2C watchdog does exactly that — see bq25896_begin), which relights the LED
 * while our cache still says "off", so it is never corrected and stays lit
 * forever. bq25896_set_led() reads the register and only writes when the bit
 * actually differs, so this already costs one read and usually no write —
 * cheap enough, and self-healing against any external reset. */
static void bq25896_service_led(void) {
  bq25896_set_led(bq25896_is_charging());
}

/* Force the LED off and remember that, so the next service() call re-evaluates
 * from a known state. Used on the power-off path: the charger keeps running on
 * USB after the SoC stops, so without this the LED stays lit on a "powered off"
 * watch — exactly the behaviour being fixed. */
static void bq25896_led_off(void) {
  bq25896_set_led(false);
}

/* VBUS voltage in mV, 0 if unreadable or absent.
 *
 * COSTS A CONVERSION: the ADC is one-shot, so this kicks CONV_START and waits
 * for the result. Do NOT call it on a hot path — bq25896_vbus_present() answers
 * the presence question from a status bit with no conversion at all. This exists
 * for the Power app's diagnostic readout, which is refreshed at human speed. */
static uint16_t bq25896_vbus_mv(void) {
  uint8_t ctrl = 0;
  if (!bq25896_read_reg(BQ25896_REG_ADC_CTRL, &ctrl)) return 0;

  Wire.beginTransmission(BQ25896_ADDR);            /* kick a one-shot conversion */
  Wire.write(BQ25896_REG_ADC_CTRL);
  Wire.write((uint8_t)(ctrl | 0x80));
  if (Wire.endTransmission(true) != 0) return 0;

  /* The conversion takes ~1 ms; poll the start bit clearing rather than a blind
   * delay, with a bounded retry so a wedged chip cannot hang the caller. */
  for (uint8_t i = 0; i < 20; i++) {
    delay(1);
    uint8_t c = 0;
    if (!bq25896_read_reg(BQ25896_REG_ADC_CTRL, &c)) return 0;
    if ((c & 0x80) == 0) break;
  }

  uint8_t v = 0;
  if (!bq25896_read_reg(BQ25896_REG_VBUSV, &v)) return 0;
  uint8_t steps = (uint8_t)(v & 0x7F);
  if (steps == 0) return 0;                        /* no input attached */
  return (uint16_t)(2600 + (uint16_t)steps * 100); /* offset 2600 mV, 100 mV/LSB */
}

#endif  /* BOARD_HAS_CHARGER_BQ25896 */
