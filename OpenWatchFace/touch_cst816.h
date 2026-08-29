/* ============================================================================
 *  touch_cst816.h — CST816D/S capacitive touch controller over I2C
 *
 *  Used by the Waveshare ESP32-S3-Touch-LCD-2 (BOARD_TOUCH_CST816). Self-contained
 *  (raw Wire register reads) rather than pulling in Waveshare's bsp_cst816 library,
 *  for three reasons:
 *    - that library hard-codes its reset pin and its own Serial prints,
 *    - it keeps a private `TwoWire*` and does its own bus talking, bypassing this
 *      firmware's i2c_lock() that serialises the touch/IMU bus, and
 *    - it is one more library the user must hand-install per board. The whole
 *      protocol is five register reads, so it lives in-tree like display_jd9853.h.
 *
 *  THE CHIP. The CST816 family reports ONE touch point. Register map (8-bit
 *  addresses, MSB-first 12-bit coordinates):
 *    0xA7 CHIP_ID    — 0xB6 on the CST816D fitted here (identity check only)
 *    0x02 FINGER_NUM — live finger count; 0 = released
 *    0x03/0x04       — X high nibble / X low byte
 *    0x05/0x06       — Y high nibble / Y low byte
 *  Coordinates come out in the PANEL's native orientation, which for this board's
 *  240x320 portrait ST7789 (rotation 0) is already LVGL's orientation — so no
 *  swap/flip is applied. If a future board runs this controller rotated, rotate
 *  here (like the vendor bsp does) rather than in the caller.
 *
 *  POLLED, NOT INTERRUPT-DRIVEN. The board breaks out no INT line (TP_INT = -1 ->
 *  BOARD_HAS_TOUCH_INT = 0), so my_touchpad_read() calls cst816_read() on every
 *  LVGL indev poll. That is exactly what the vendor demo does. Cost: the ISR-driven
 *  s_touch_activity flag never fires from an edge, so idle-sleep learns about a
 *  touch only via the poll — quick taps are still caught because the poll runs at
 *  LVGL's indev period, but this is the documented tradeoff (see board.h).
 *
 *  IMPORTANT — a finger-count of 0 must be trusted. Like the FT3168, this chip does
 *  NOT clear its X/Y registers on release: they latch the last touch position
 *  forever. So the caller must gate the reported press POSITION on fingers > 0, or
 *  it hands LVGL stale coordinates and the previously-tapped button fires again.
 *  cst816_read() enforces that by returning false (and leaving x/y untouched)
 *  whenever no finger is down.
 *
 *  THREADING: the caller holds i2c_lock() across cst816_read() — the QMI8658 IMU
 *  shares this bus. Nothing here locks on its own.
 * ========================================================================== */
#pragma once
#if BOARD_TOUCH_CST816

#include <Wire.h>

#define CST816_ADDR            0x15
#define CST816_REG_CHIP_ID     0xA7
#define CST816_REG_FINGER_NUM  0x02
#define CST816_REG_XH          0x03   /* XH, XL, YH, YL are consecutive: 0x03..0x06 */

/* Read `len` bytes starting at `reg`. Returns false on any I2C error (NAK / short
 * read), so a missing or wedged controller degrades to "no touch" rather than
 * feeding uninitialised bytes to LVGL as coordinates. */
static bool cst816_reg_read(uint8_t reg, uint8_t *out, uint8_t len) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;      // NAK: chip absent/asleep
  if (Wire.requestFrom((uint8_t)CST816_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) out[i] = Wire.read();
  return true;
}

/* Probe the controller. Returns true when the chip answers with a plausible ID.
 * The CST816D here reports 0xB6, but siblings in the family (CST816S/T, CST820)
 * use other IDs — so a SUCCESSFUL READ is treated as present and the ID is only
 * logged. Rejecting on an unexpected-but-working ID would break touch on a board
 * that shipped with a different die, which is a worse failure than a wrong log. */
static bool cst816_begin(uint8_t *out_id = nullptr) {
  uint8_t id = 0;
  if (!cst816_reg_read(CST816_REG_CHIP_ID, &id, 1)) return false;
  if (out_id) *out_id = id;
  return true;
}

/* Poll for a live touch. Returns true (and fills x/y) ONLY while a finger is
 * actually down — see the stale-coordinate note in the header comment. */
static bool cst816_read(int32_t *x, int32_t *y) {
  uint8_t fingers = 0;
  if (!cst816_reg_read(CST816_REG_FINGER_NUM, &fingers, 1)) return false;
  if (fingers == 0) return false;         // released: X/Y are stale, never report them

  uint8_t raw[4];                          // XH, XL, YH, YL — one burst read
  if (!cst816_reg_read(CST816_REG_XH, raw, 4)) return false;

  /* 12-bit coordinates: the high register's low nibble is the top 4 bits. */
  *x = (int32_t)(((uint16_t)(raw[0] & 0x0F) << 8) | raw[1]);
  *y = (int32_t)(((uint16_t)(raw[2] & 0x0F) << 8) | raw[3]);
  return true;
}

/* ---- SECOND touch point (for pinch) --------------------------------------
 * The CST816 family's documented map defines ONE point: finger-count at 0x02 and
 * a single X/Y pair at 0x03..0x06. The FT-style layout these parts derive from
 * puts a second point 6 registers further on (0x09..0x0C), and some dies in the
 * family populate it even though the datasheet only advertises one. Whether THIS
 * die does cannot be settled from the register map alone, so the code asks the
 * hardware instead of assuming either way.
 *
 * cst816_read2() returns true only when the controller reports 2+ fingers AND the
 * second slot holds a plausible coordinate. Callers must treat false as "this
 * panel is single-touch" and fall back — never as a transient miss. */
#define CST816_REG_XH2  0x09   /* second point: XH, XL, YH, YL at 0x09..0x0C */

static bool cst816_read2(int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2) {
  uint8_t fingers = 0;
  if (!cst816_reg_read(CST816_REG_FINGER_NUM, &fingers, 1)) return false;
  if ((fingers & 0x0F) < 2) return false;        // low nibble = live finger count

  uint8_t raw[4];
  if (!cst816_reg_read(CST816_REG_XH, raw, 4)) return false;
  *x1 = (int32_t)(((uint16_t)(raw[0] & 0x0F) << 8) | raw[1]);
  *y1 = (int32_t)(((uint16_t)(raw[2] & 0x0F) << 8) | raw[3]);

  if (!cst816_reg_read(CST816_REG_XH2, raw, 4)) return false;
  int32_t sx = (int32_t)(((uint16_t)(raw[0] & 0x0F) << 8) | raw[1]);
  int32_t sy = (int32_t)(((uint16_t)(raw[2] & 0x0F) << 8) | raw[3]);

  /* Reject an unpopulated slot: a die that doesn't implement point 2 leaves these
   * registers at 0x00 (or mirrors point 1). Either would make a pinch handler
   * compute nonsense, so only accept a distinct, in-range coordinate. */
  if (sx <= 0 && sy <= 0) return false;
  if (sx == *x1 && sy == *y1) return false;
  if (sx >= LCD_WIDTH * 2 || sy >= LCD_HEIGHT * 2) return false;

  *x2 = sx; *y2 = sy;
  return true;
}

/* True once a genuine 2-finger report has EVER been seen this session. Latched,
 * because pinch UI needs a stable answer: a momentary 1-finger poll in the middle
 * of a pinch must not make the app decide the panel is single-touch. */
static bool s_cst_multitouch_seen = false;
static inline bool cst816_has_multitouch(void) { return s_cst_multitouch_seen; }

#endif  /* BOARD_TOUCH_CST816 */
