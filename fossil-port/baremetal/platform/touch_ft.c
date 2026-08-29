/* ============================================================================
 *  touch_ft.c — FocalTech FTS capacitive touch, I2C. TicWatch C2 (skipjack).
 *
 *  WHY THIS FILE EXISTS. The C2's QUP5 bus carries three touch nodes in its
 *  device tree and only one is live — synaptics@20 and it7260@46 are both
 *  status="disabled", while focaltech@38 carries no status property and is
 *  therefore enabled. (See dumps/c2-skipjack-fromsource/skipjack.dts.) So the
 *  Gen 4's Raydium driver does not apply here at all, and this is the one
 *  genuinely new driver the C2 port needs.
 *
 *  IT IS ALSO MUCH SIMPLER THAN THE RAYDIUM. The Raydium needed the PDA2
 *  paging protocol and an explicit per-report acknowledge (without which the
 *  Gen 4 held every touch down — a full day to find). FocalTech has none of
 *  that: the controller keeps a fixed register block at address 0, a read of
 *  which returns the current state. There is no report queue and so nothing to
 *  acknowledge; reading is idempotent and a missed read simply misses a frame.
 *
 *  REGISTER MAP (common to the FT5x06/FT6x36 family this part belongs to):
 *      0x00  DEVICE_MODE
 *      0x01  GEST_ID          gesture, unused here
 *      0x02  TD_STATUS        low 4 bits = number of active points
 *      0x03  P1_XH            [7:6] event flag, [3:0] X high nibble
 *      0x04  P1_XL            X low byte
 *      0x05  P1_YH            [7:4] touch id, [3:0] Y high nibble
 *      0x06  P1_YL            Y low byte
 *      0x07  P1_WEIGHT
 *      0x08  P1_MISC
 *      0x09.. point 2, same six-byte layout
 *  Event flag: 0 = press down, 1 = lift up, 2 = contact (still down),
 *  3 = no event. A point counts as touched for 0 and 2; 1 means released.
 *
 *  COORDINATE SCALING — which turned out to be a non-issue, but is kept because
 *  it is what makes that true. The DT reports focaltech,display-coords 360x360
 *  while the panel node claims 400x400, so this driver was written to scale
 *  between them. The panel is ACTUALLY 360x360 (confirmed on hardware), so the
 *  scale is now identity. It is computed against fb_width()/fb_height() at
 *  RUNTIME rather than a compile-time constant, for the same reason fb_mdp3.c
 *  takes its geometry from the hardware: the display size this firmware runs
 *  at is whatever aboot programmed, and a touch driver that disagrees with the
 *  framebuffer puts every press in the wrong place.
 * ========================================================================== */
#include "platform.h"
#include <stddef.h>

#if defined(PLAT_BOARD_TICWATCH_C2)

#define FT_REG_DEVICE_MODE  0x00u
#define FT_REG_TD_STATUS    0x02u
#define FT_REG_P1_XH        0x03u
/* One read covers TD_STATUS plus both points: 0x02..0x0e = 13 bytes. */
#define FT_READ_BASE        FT_REG_TD_STATUS
#define FT_READ_LEN         13u

#define FT_EVT_DOWN         0u
#define FT_EVT_UP           1u
#define FT_EVT_CONTACT      2u

/* Chip identity registers, read once at init purely to prove the part is
 * answering. A FocalTech that is present but mute is a different problem from
 * one that is absent, and only a probe distinguishes them. */
#define FT_REG_ID_G_LIB_VER_H 0xA1u
#define FT_REG_ID_G_CIPHER    0xA3u
#define FT_REG_ID_G_FOCALTECH 0xA8u   /* vendor id, expected 0x11 on FTS */

static int      s_ready;
static uint16_t s_last_x, s_last_y;
static int      s_last_down;

/* Census, mirroring the Raydium driver's, so the on-glass diagnostics that
 * already exist keep working on this watch. */
static uint32_t s_reads, s_read_err, s_points;

static int ft_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
    return i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, PLAT_TOUCH_I2C_ADDR,
                        &reg, 1u, buf, len);
}

int touch_init(void)
{
    uint8_t v = 0;

    s_ready = 0; s_last_down = 0;
    s_reads = s_read_err = s_points = 0;

    if (i2c_bus_init(PLAT_I2C_TOUCH_BASE) < 0) {
        con_puts("ft: i2c bus init failed\n");
        return -1;
    }

    /* Probe. The vendor id is the cheapest read that proves the right chip is
     * on the right address; a NACK here means nothing is at 0x38. */
    if (ft_read(FT_REG_ID_G_FOCALTECH, &v, 1u) < 0) {
        /* Only now try a reset. Same reasoning the Gen 4 arrived at the hard
         * way: the controller self-boots at power-on and keeps running across
         * OS boots, so an unconditional reset is a way to make a working chip
         * stop working. Reset is a fallback, never the opening move. */
        con_puts("ft: no answer at 0x38, pulsing reset\n");
        tlmm_touch_reset_pulse();
        if (ft_read(FT_REG_ID_G_FOCALTECH, &v, 1u) < 0) {
            con_puts("ft: still mute after reset\n");
            return -1;
        }
    }

    con_puts("ft: vendor id 0x"); con_puthex(v);
    /* 0x11 is the documented FocalTech vendor id. Report a mismatch but do
     * NOT refuse: the id byte varies across the family, and a working touch
     * with an unexpected id is worth far more than a strict check. */
    con_puts(v == 0x11u ? " (FocalTech)\n" : " (unexpected — continuing)\n");

    /* Normal operating mode. The chip powers up in it; writing it is cheap
     * insurance against whatever the bootloader may have left behind. */
    {
        uint8_t wr[2] = { FT_REG_DEVICE_MODE, 0x00u };
        (void)i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, PLAT_TOUCH_I2C_ADDR,
                           wr, 2u, NULL, 0u);
    }

    s_ready = 1;
    return 0;
}

/* Returns 1 with *x/*y set while a finger is down, 0 when nothing is touched,
 * -1 on a bus error. Same contract as touch_raydium.c's touch_read(). */
int touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t b[FT_READ_LEN];
    uint32_t n;
    uint8_t evt;
    uint16_t rx, ry;

    if (!s_ready) return -1;

    /* DELIBERATELY NOT INT-GATED. The Gen 6's Raydium declares its interrupt
     * IRQ_TYPE_LEVEL_LOW, so the line stays low for as long as a report is
     * pending and polling the pin is a valid, cheap substitute for polling the
     * bus. This FocalTech node declares EDGE_FALLING (interrupts = <13
     * 0x2002>): the controller PULSES the line when something changes rather
     * than holding it. Sampling an edge-triggered line from a poll loop misses
     * the pulse almost every time, which would look exactly like dead touch.
     *
     * So read the register block every time. It is 13 bytes at 100 kHz —
     * about 1.3 ms, a few percent of the bus at LVGL's poll rate — and the
     * read is idempotent with no report to acknowledge, so a missed poll costs
     * one frame and nothing else. Cheap and correct beats clever and racy;
     * the Gen 4 lost a day to exactly the opposite trade. */

    s_reads++;
    if (ft_read(FT_READ_BASE, b, FT_READ_LEN) < 0) { s_read_err++; return -1; }

    n = (uint32_t)(b[0] & 0x0Fu);          /* TD_STATUS */
    if (n == 0u || n > PLAT_TOUCH_MAX_PTS) { s_last_down = 0; return 0; }

    /* First point only — the firmware's input model is single-touch. b[1] is
     * P1_XH because the read started at TD_STATUS. */
    evt = (uint8_t)(b[1] >> 6);
    if (evt == FT_EVT_UP) { s_last_down = 0; return 0; }

    rx = (uint16_t)(((uint16_t)(b[1] & 0x0Fu) << 8) | b[2]);
    ry = (uint16_t)(((uint16_t)(b[3] & 0x0Fu) << 8) | b[4]);

    /* Guard against the garbage a half-initialised controller emits: a point
     * outside its own declared coordinate space is not a touch. */
    if (rx >= PLAT_TOUCH_COORD_W || ry >= PLAT_TOUCH_COORD_H) return 0;

    /* Map the controller's space onto whatever the framebuffer actually is
     * (identity on this watch: both are 360). Runtime, not compile-time —
     * see the header note. */
    {
        uint32_t fw = fb_width(), fh = fb_height();
        if (fw && fw != PLAT_TOUCH_COORD_W)
            rx = (uint16_t)(((uint32_t)rx * fw) / PLAT_TOUCH_COORD_W);
        if (fh && fh != PLAT_TOUCH_COORD_H)
            ry = (uint16_t)(((uint32_t)ry * fh) / PLAT_TOUCH_COORD_H);
    }

    s_points++;
    s_last_x = rx; s_last_y = ry; s_last_down = 1;
    *x = rx; *y = ry;
    return 1;
}

/* The FTS family's power modes live in register 0xA5: 0 = active,
 * 1 = monitor, 3 = hibernate. Hibernate needs a reset pulse to leave, which
 * tlmm_touch_reset_pulse() provides, so the wake path stays honest. */
#define FT_REG_PMODE      0xA5u
#define FT_PMODE_ACTIVE   0x00u
#define FT_PMODE_HIBERNATE 0x03u

int touch_set_sleep(int sleep)
{
    uint8_t wr[2] = { FT_REG_PMODE, sleep ? FT_PMODE_HIBERNATE : FT_PMODE_ACTIVE };
    if (!s_ready) return -1;
    if (!sleep) tlmm_touch_reset_pulse();   /* hibernate exits only via reset */
    return i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, PLAT_TOUCH_I2C_ADDR,
                        wr, 2u, NULL, 0u);
}

#endif /* PLAT_BOARD_TICWATCH_C2 */
