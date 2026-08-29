/* ============================================================================
 *  keyboard_tca8418.h — TCA8418 I2C keypad scanner (T-Deck Pro QWERTY)
 *
 *  The T-Deck Pro carries a BlackBerry-style physical keyboard wired as a 4x10
 *  matrix into a TI TCA8418 scanner. The chip debounces and scans the matrix in
 *  hardware and exposes a FIFO of key events over I2C, so this driver is small:
 *  configure which rows/cols are in the matrix, then drain the FIFO.
 *
 *  WHY THIS EXISTS. On every previous board the only input is a touch panel and
 *  LVGL gets one pointer indev. Here there is also a real keyboard, so this
 *  registers a SECOND LVGL indev of type KEYPAD.
 *
 *  IT DOES NOT REPLACE TOUCH. This is a BlackBerry-style TEXT keyboard: letters,
 *  space, shift, symbol, backspace, enter, and two hardware-function keys. There
 *  are NO arrow keys and no navigation cluster, so the only LVGL key codes it can
 *  actually produce are LV_KEY_ENTER and LV_KEY_BACKSPACE — not enough to move
 *  focus around a UI. Touch remains how you navigate; this is for text entry.
 *  See BOARD_KB_KEYMAP in the board header for exactly what exists.
 *
 *  Written from LilyGo's own peri_keypad.cpp + the Adafruit_TCA8418 library that
 *  file drives, rather than pulling Adafruit_TCA8418 in as a dependency: the
 *  whole protocol is three register reads, and an in-tree driver goes through
 *  this firmware's i2c_lock() (the touch controller, PMU, fuel gauge and IMU all
 *  share this bus) instead of keeping its own private TwoWire like the library
 *  does. Same reasoning as touch_cst816.h / touch_cst328.h.
 *
 *  ============ THE EVENT ENCODING — READ BEFORE EDITING ============
 *  Two details here are the opposite of what the register map suggests, and both
 *  were verified against Adafruit_TCA8418.cpp and LilyGo's peri_keypad.cpp:
 *
 *  1. BIT 7 IS SET ON *PRESS*. The TCA8418 event byte is 0x81..0xD0 for a key
 *     PRESS and 0x01..0x50 for a key RELEASE — bit 7 SET means pressed. LilyGo's
 *     constants confirm it in decimal: KEYPAD_PRESS_VAL_MIN/MAX is 129..163
 *     (0x81..0xA3) and their release window is 1..35; Adafruit's examples test
 *     `event & 0x80` for pressed the same way. Getting this backwards inverts
 *     every key event, which manifests ON-DEVICE as keys that REPEAT FOREVER
 *     after being released (the release registers as a press that never ends)
 *     and held-modifier state that latches permanently.
 *
 *  2. THE COLUMN ORDER IS REVERSED. The scanner numbers keys row-major, but this
 *     keyboard's columns are wired backwards relative to that numbering, so the
 *     column index must be flipped:
 *         col = (COLS - 1) - (code % COLS)
 *     Without the flip, row 0 reads as "poiuytrewq" — each row individually
 *     mirrored, which is easy to mistake for a wrong keymap table and "fix" in
 *     the wrong place. The flip is a property of THIS keyboard PCB, which is why
 *     it lives here next to the keymap rather than in the generic decode.
 *
 *  THREADING: the caller holds i2c_lock() across these calls. Nothing here locks.
 * ========================================================================== */
#pragma once
#if BOARD_HAS_KEYBOARD_TCA8418

#include <Wire.h>

#define TCA8418_ADDR            0x34

/* Register map (only what this driver touches). */
#define TCA8418_REG_CFG          0x01   /* global config: interrupt enables */
#define TCA8418_REG_INT_STAT     0x02   /* interrupt status (write 1 to clear) */
#define TCA8418_REG_KEY_LCK_EC   0x03   /* low nibble = queued event count */
#define TCA8418_REG_KEY_EVENT_A  0x04   /* pop one event from the FIFO */
#define TCA8418_REG_KP_GPIO_1    0x1D   /* which rows 0-7 are in the matrix */
#define TCA8418_REG_KP_GPIO_2    0x1E   /* which cols 0-7 are in the matrix */
#define TCA8418_REG_KP_GPIO_3    0x1F   /* which cols 8-9 are in the matrix */

#define TCA8418_CFG_KE_IEN       0x01   /* raise INT when the key FIFO is non-empty */

static bool tca8418_wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA8418_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

static bool tca8418_rd(uint8_t reg, uint8_t *val) {
  Wire.beginTransmission(TCA8418_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)TCA8418_ADDR, (uint8_t)1) != 1) return false;
  *val = Wire.read();
  return true;
}

/* Configure the matrix. Mirrors Adafruit_TCA8418::matrix(): a mask of ROWS bits
 * into KP_GPIO_1, COLS bits split across KP_GPIO_2 (cols 0-7) and KP_GPIO_3
 * (cols 8-9). Pins outside the matrix stay GPIOs and are ignored.
 *
 * Returns false if the chip does not ACK, so a unit with no keyboard fitted
 * degrades to "no key events" rather than hanging or reporting garbage. */
static bool tca8418_begin(void) {
  uint8_t probe;
  if (!tca8418_rd(TCA8418_REG_KEY_LCK_EC, &probe)) return false;   /* absent -> bail */

  uint8_t row_mask = (uint8_t)((1u << BOARD_KB_ROWS) - 1u);
  if (!tca8418_wr(TCA8418_REG_KP_GPIO_1, row_mask)) return false;

  uint8_t col_lo = (uint8_t)((BOARD_KB_COLS >= 8)
                             ? 0xFF : ((1u << BOARD_KB_COLS) - 1u));
  tca8418_wr(TCA8418_REG_KP_GPIO_2, col_lo);
  uint8_t col_hi = (uint8_t)((BOARD_KB_COLS > 8)
                             ? ((BOARD_KB_COLS == 9) ? 0x01 : 0x03) : 0x00);
  tca8418_wr(TCA8418_REG_KP_GPIO_3, col_hi);

  /* Drain whatever the chip queued while it was being configured, so the first
   * real keypress isn't preceded by a burst of stale events (LilyGo's flush()). */
  uint8_t ev, guard = 0;
  while (tca8418_rd(TCA8418_REG_KEY_EVENT_A, &ev) && ev != 0 && guard++ < 32) { }

  tca8418_wr(TCA8418_REG_INT_STAT, 0x03);          /* clear latched interrupts */
  tca8418_wr(TCA8418_REG_CFG, TCA8418_CFG_KE_IEN);
  return true;
}

/* Pop ONE event from the FIFO. Returns false when it is empty.
 *   *code    — 0-based matrix index (row * BOARD_KB_COLS + raw_col)
 *   *pressed — true on press, false on release (see the bit-7 note above) */
static bool tca8418_pop(uint8_t *code, bool *pressed) {
  uint8_t cnt = 0;
  if (!tca8418_rd(TCA8418_REG_KEY_LCK_EC, &cnt)) return false;
  if ((cnt & 0x0F) == 0) return false;

  uint8_t ev = 0;
  if (!tca8418_rd(TCA8418_REG_KEY_EVENT_A, &ev)) return false;
  if (ev == 0) return false;                        /* FIFO raced empty */

  /* Bit 7 SET = press, CLEAR = release (see note 1 in the header). The low 7
   * bits are a 1-based key number, so subtract 1 to get the matrix index. */
  *pressed = (ev & 0x80) != 0;
  uint8_t n = (uint8_t)(ev & 0x7F);
  if (n == 0) return false;
  *code = (uint8_t)(n - 1);
  return true;
}

/* ---- Matrix index -> LVGL key --------------------------------------------
 * Recovers row/col from the 0-based index, applying this keyboard's reversed
 * column order (see note 2 in the header). Returns 0 for a position with no
 * mapping, which the caller drops.
 *
 * `sym` selects the symbol layer (BOARD_KB_ALTMAP, the secondary keycap
 * legends); a 0 altmap entry falls back to the base map so enter/backspace/
 * space and the modifiers still work with sym active. `shift` uppercases
 * letters. The MODIFIER STATE itself lives in the caller (my_keypad_read) —
 * this stays a pure lookup. */
static uint32_t tca8418_to_lvgl(uint8_t code, bool sym, bool shift) {
  uint8_t row = (uint8_t)(code / BOARD_KB_COLS);
  uint8_t col = (uint8_t)((BOARD_KB_COLS - 1) - (code % BOARD_KB_COLS));
  if (row >= BOARD_KB_ROWS || col >= BOARD_KB_COLS) return 0;

  static const uint16_t keymap[BOARD_KB_ROWS][BOARD_KB_COLS] = BOARD_KB_KEYMAP;
  uint32_t k = (uint32_t)keymap[row][col];
#ifdef BOARD_KB_ALTMAP
  if (sym) {
    static const uint16_t altmap[BOARD_KB_ROWS][BOARD_KB_COLS] = BOARD_KB_ALTMAP;
    if (altmap[row][col]) k = (uint32_t)altmap[row][col];
  }
#else
  (void)sym;
#endif
  if (shift && k >= 'a' && k <= 'z') k -= 'a' - 'A';
  return k;
}

#endif  /* BOARD_HAS_KEYBOARD_TCA8418 */
