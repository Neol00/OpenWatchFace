/* ============================================================================
 *  touch_cst328.h — CST328 / CST226SE capacitive touch controller over I2C
 *
 *  Used by the LilyGo T-Deck Pro (BOARD_TOUCH_CST328). Despite the similar name
 *  this is NOT the CST816 of touch_cst816.h — different I2C address (0x1A vs
 *  0x15) and a COMPLETELY different protocol. The CST816 exposes a handful of
 *  single-byte registers; this chip returns one 28-byte status block that must be
 *  validated against several magic bytes before any coordinate in it can be
 *  trusted. Do not try to unify the two drivers.
 *
 *  Written from lewisxhe's SensorLib TouchDrvCST226.cpp (the driver LilyGo's own
 *  examples use via TouchDrvCSTXXX) rather than depending on SensorLib, for the
 *  same reasons as touch_cst816.h: SensorLib keeps its own bus handle and would
 *  bypass this firmware's i2c_lock(), and it is one more library to hand-install.
 *
 *  ============ THE STATUS BLOCK ============
 *  One burst read of 28 bytes from register 0x00:
 *
 *    buffer[0]      first point's id (high nibble) + event (low nibble)
 *    buffer[1]      X high 8 bits
 *    buffer[2]      Y high 8 bits
 *    buffer[3]      X low nibble (high half) + Y low nibble (low half)
 *    buffer[4]      pressure
 *    buffer[5]      live finger count in the low 7 bits
 *    buffer[6]      MUST be 0xAB — the block's validity marker
 *
 *  Coordinates are 12-BIT, split across three bytes:
 *      x = (buffer[1] << 4) | ((buffer[3] >> 4) & 0x0F)
 *      y = (buffer[2] << 4) |  (buffer[3]       & 0x0F)
 *
 *  Subsequent points follow at +7 for the second and +5 for each after that —
 *  an irregular stride, which is why point 2 is read explicitly below rather
 *  than in a loop with a fixed step.
 *
 *  ============ THE REJECTION CASES — ALL FOUR ARE LOAD-BEARING ============
 *  A block is only real if ALL of these hold. Each rejects a genuine state the
 *  chip produces, and dropping any one of them feeds LVGL phantom touches:
 *    - buffer[6] == 0xAB      : block marker absent -> the read was torn/garbage
 *    - buffer[0] != 0xAB      : chip is reporting "idle", not a point
 *    - buffer[0] != 0x00      : uninitialised block
 *    - buffer[5] != 0x80      : the 0x80 flag means "no live finger"
 *  and the finger count (buffer[5] & 0x7F) must be in 1..MAX. When the count is
 *  out of range the vendor driver WRITES 0xAB back to register 0x00 to reset the
 *  chip's reporting state — we do the same, since skipping it leaves the
 *  controller wedged in that bad state and touch dies until a power cycle.
 *
 *  There is also a hardware-button block (0x83 0x17 ... 0x80) which this panel
 *  does not use; it is rejected by the checks above and needs no special case.
 *
 *  THREADING: the caller holds i2c_lock() across these calls — the keyboard, PMU,
 *  fuel gauge and IMU all share this bus. Nothing here locks on its own.
 * ========================================================================== */
#pragma once
#if BOARD_TOUCH_CST328

#include <Wire.h>

#define CST328_ADDR            0x1A
#define CST328_STATUS_REG      0x00
#define CST328_BLOCK_LEN       28
#define CST328_VALID_MARKER    0xAB
#define CST328_MAX_FINGERS     5

/* Burst-read the status block. Returns false on any I2C error so a missing or
 * wedged controller degrades to "no touch" rather than feeding uninitialised
 * bytes to LVGL as coordinates. */
static bool cst328_read_block(uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(CST328_ADDR);
  Wire.write(CST328_STATUS_REG);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)CST328_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static bool cst328_write_reg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(CST328_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

/* Probe the controller. A successful status read is treated as "present": the
 * chip-ID handshake (the 0xD1/0xD2 register window) requires entering a debug
 * mode, and getting that wrong on a working panel is a worse failure than not
 * knowing the exact die. Same philosophy as cst816_begin(). */
static bool cst328_begin(void) {
  uint8_t buf[CST328_BLOCK_LEN];
  return cst328_read_block(buf, CST328_BLOCK_LEN);
}

/* Poll for a live touch. Returns true (and fills x/y) ONLY when the block is
 * valid AND a finger is genuinely down — see the rejection cases above. */
static bool cst328_read(int32_t *x, int32_t *y) {
  uint8_t b[CST328_BLOCK_LEN];
  if (!cst328_read_block(b, CST328_BLOCK_LEN)) return false;

  if (b[6] != CST328_VALID_MARKER) return false;   /* torn / invalid block */
  if (b[0] == CST328_VALID_MARKER) return false;   /* idle report */
  if (b[0] == 0x00) return false;                  /* uninitialised */
  if (b[5] == 0x80) return false;                  /* explicit "no finger" flag */

  uint8_t n = (uint8_t)(b[5] & 0x7F);
  if (n == 0 || n > CST328_MAX_FINGERS) {
    /* Out-of-range count: the chip is in a bad reporting state. Writing the
     * marker back to the status register resets it — WITHOUT this the controller
     * stays wedged and touch never recovers. */
    cst328_write_reg(CST328_STATUS_REG, CST328_VALID_MARKER);
    return false;
  }

  *x = (int32_t)(((uint16_t)b[1] << 4) | ((b[3] >> 4) & 0x0F));
  *y = (int32_t)(((uint16_t)b[2] << 4) |  (b[3]       & 0x0F));
  return true;
}

/* ---- SECOND touch point (for pinch) --------------------------------------
 * This controller genuinely reports up to 5 points. The second point sits at
 * offset +7 from the first (later points step by 5 — see the header note on the
 * irregular stride). Returns false when fewer than 2 fingers are down, which
 * callers must treat as "not a pinch right now" rather than "single-touch
 * panel". */
static bool cst328_read2(int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2) {
  uint8_t b[CST328_BLOCK_LEN];
  if (!cst328_read_block(b, CST328_BLOCK_LEN)) return false;

  if (b[6] != CST328_VALID_MARKER) return false;
  if (b[0] == CST328_VALID_MARKER || b[0] == 0x00) return false;
  if (b[5] == 0x80) return false;

  uint8_t n = (uint8_t)(b[5] & 0x7F);
  if (n < 2 || n > CST328_MAX_FINGERS) return false;

  *x1 = (int32_t)(((uint16_t)b[1] << 4) | ((b[3] >> 4) & 0x0F));
  *y1 = (int32_t)(((uint16_t)b[2] << 4) |  (b[3]       & 0x0F));

  const uint8_t i = 7;   /* second point's base offset */
  *x2 = (int32_t)(((uint16_t)b[i + 1] << 4) | ((b[i + 3] >> 4) & 0x0F));
  *y2 = (int32_t)(((uint16_t)b[i + 2] << 4) |  (b[i + 3]       & 0x0F));
  return true;
}

/* True once a genuine 2-finger report has EVER been seen this session. Latched,
 * for the same reason as the CST816's: a momentary 1-finger poll mid-pinch must
 * not make the UI decide the panel is single-touch. */
static bool s_cst328_multitouch_seen = false;
static inline bool cst328_has_multitouch(void) { return s_cst328_multitouch_seen; }

/* Put the controller to sleep before the SoC sleeps (vendor command 0xD1 0x05). */
static inline void cst328_sleep(void) { cst328_write_reg(0xD1, 0x05); }

#endif  /* BOARD_TOUCH_CST328 */
