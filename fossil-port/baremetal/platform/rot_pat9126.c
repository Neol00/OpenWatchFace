/* rot_pat9126.c — PixArt PAT9126 optical rotation sensor = the Fossil Gen 4's
 * rotating crown.
 *
 * WHAT THE CROWN ACTUALLY IS. Not a quadrature encoder on two GPIOs, which is
 * what "crown" usually means and what the LVGL encoder indev is written for.
 * Fossil put an OPTICAL MOTION SENSOR under the crown: it watches a patterned
 * surface on the inside of the wheel and reports relative motion, exactly like
 * the sensor in an optical mouse. So the crown is an I2C device, it is read by
 * polling deltas, and there is no absolute position to recover -- only "it
 * moved this much since you last asked".
 *
 * FROM THE DEVICE TREE (firefish-boot.dts:5904, verbatim):
 *   pixart_pat9126@75 {
 *     compatible    = "pixart,pat9126";
 *     reg           = <0x75>;                 I2C address
 *     interrupts    = <0x1b 0x2008>;          TLMM GPIO 27, level low
 *     pixart,irq-gpio = <&tlmm 0x1b 0x00>;
 *     vld-supply    = <&pm8916_l18>;          sensor LED/illumination rail
 *     vddio-supply  = <&pm8916_l16>;          IO rail (SHARED with the touch
 *                                             controller's vcc_i2c -- so this
 *                                             one is definitely up already)
 *     vdd-supply    = <&pm8916_l14>;          sensor core rail
 *   }
 * It is a sibling of the raydium touch node, so it sits on the SAME bus we
 * already drive for touch: i2c@78b9000 = PLAT_I2C_TOUCH_BASE. No new QUP, no
 * new GCC clocks -- the one bus whose clocks gcc_blsp.c already owns.
 *
 * THE ONE THING THAT COULD STOP THIS WORKING, stated up front so a silent
 * failure is not mistaken for a driver bug: l14 and l18 are RPM-SMD
 * regulators. They are owned by the RPM co-processor and turned on by asking
 * it over SMD -- machinery this firmware does not have. If aboot leaves them
 * off, the sensor is unpowered and will not ACK at 0x75, and no amount of
 * register poking here will change that; the fix would be an RPM SMD client,
 * which is a real subproject. crown_init() therefore PROBES and says exactly
 * what it found. l16 (vddio) is shared with touch and is provably up, so a
 * NACK points at l14/l18 rather than at the bus.
 *
 * REGISTER MAP: from the vendor driver drivers/input/misc/pixart_pat9126.c.
 * This one is NOT quoted from a local source -- there is no kernel tree on
 * this machine -- so it is the weakest link in the file and everything is
 * built to fail loudly rather than silently: the product-ID check gates all
 * other access, every transfer is bounded by msm_i2c's own timeouts, and a
 * failed probe latches the driver off for the rest of the boot instead of
 * retrying into a bus that touch is also using.
 */
#include "platform.h"
#if defined(PLAT_CROWN_I2C_ADDR)

#define PAT_PID1        0x00u   /* product id 1, expect 0x31 */
#define PAT_PID2        0x01u   /* product id 2 */
#define PAT_MOTION      0x02u   /* bit7 = motion data available */
#define PAT_DELTA_X_LO  0x03u
#define PAT_DELTA_Y_LO  0x04u
#define PAT_CONFIG      0x06u   /* bit7 = chip reset */
#define PAT_WRITE_PROT  0x09u   /* 0x5A unlocks, 0x00 locks */
#define PAT_DELTA_XY_HI 0x12u   /* [7:4] = X high nibble, [3:0] = Y high nibble */

#define PAT_PID1_VALUE  0x31u
#define PAT_MOTION_BIT  0x80u   /* kept for CROWN_DIAG reporting; not gated on */
#define PAT_UNLOCK      0x5Au

/* Poll interval. The crown shares its bus with touch, and touch is read from
 * the LVGL indev callback on every frame, so the crown deliberately does NOT
 * poll every loop: 15 ms is far faster than a finger can turn a wheel and
 * leaves the bus to touch the rest of the time. */
#define CROWN_POLL_MS   15u

/* WHY THIS FILE DOES NOT USE cdiag_puts(). The first diagnostic image printed
 * NOTHING from here, which looked like the code never ran. It ran fine: the
 * bdiag_* macros are compiled to ((void)0) unless -DBOOT_DIAG is also passed
 * (platform.h), so -DCROWN_DIAG gated the code in while the printing macros
 * stayed gated out. Two independent switches for one diagnostic is a trap, so
 * the crown's own diagnostics go straight to con_* under the ONE flag that is
 * supposed to control them. */
#if defined(CROWN_DIAG)
#  define cdiag_puts(s)   con_puts(s)
#  define cdiag_puthex(v) con_puthex(v)
#  define cdiag_putdec(v) con_putdec(v)
#else
#  define cdiag_puts(s)   ((void)0)
#  define cdiag_puthex(v) ((void)0)
#  define cdiag_putdec(v) ((void)0)
#endif

static int      s_ok;           /* probe succeeded; 0 = driver latched off */
static int      s_probed;       /* probe has run (success or not) */
static int32_t  s_accum;        /* raw counts since the last crown_take_delta */
static uint32_t s_last_ms;

static int pat_rd(uint8_t reg, uint8_t *val)
{
    return i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, PLAT_CROWN_I2C_ADDR,
                        &reg, 1u, val, 1u);
}

static int pat_wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, PLAT_CROWN_I2C_ADDR,
                        b, 2u, 0, 0u);
}

/* 12-bit signed delta, split across a low byte and a nibble of the shared high
 * register. Sign-extended by hand rather than by casting through int16_t,
 * because the sign bit is bit 11, not bit 15. */
static int32_t pat_delta(uint8_t lo, uint8_t hi_nib)
{
    int32_t v = (int32_t)(((uint32_t)(hi_nib & 0x0Fu) << 8) | lo);
    if (v & 0x800) v -= 0x1000;
    return v;
}

#if defined(CROWN_DIAG)
/* Everything needed to tell "unpowered" from "wrong register map" apart, in
 * one place, printed once at boot.
 *
 * The bus scan is the part that settles it. Touch lives at 0x39 on this same
 * bus, so its ACK is a positive control: if 0x39 answers and 0x75 does not,
 * the bus, the QUP clocks and this driver's transfer code are all fine and the
 * sensor simply has no power. If NEITHER answers, the problem is the bus and
 * the crown is innocent.
 *
 * The register dump then gives us the sensor's real layout to compare against
 * the vendor driver's map, which is the one piece of this port quoted from
 * memory rather than from a file on this machine. */
static void crown_diag_dump(void)
{
    cdiag_puts("crown: i2c scan on touch bus:");
    for (uint8_t a = 0x08u; a < 0x78u; a++) {
        uint8_t reg = 0x00u, val = 0u;
        if (i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, a, &reg, 1u, &val, 1u) == 0) {
            cdiag_puts(" "); cdiag_puthex(a);
        }
    }
    cdiag_puts("\n");

    cdiag_puts("crown: 0x75 regs 00-1f:");
    for (uint8_t r = 0x00u; r < 0x20u; r++) {
        uint8_t v = 0xFFu;
        int rc = i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, PLAT_CROWN_I2C_ADDR,
                              &r, 1u, &v, 1u);
        cdiag_puts(" ");
        if (rc == 0) cdiag_puthex(v); else cdiag_puts("--");
    }
    cdiag_puts("\n");
}
#endif /* CROWN_DIAG */

int crown_init(void)
{
    if (s_probed) return s_ok ? 0 : -1;
    s_probed = 1;

    /* Touch has usually initialised this bus already; i2c_bus_init just
     * reprograms the divider, so calling it again is safe and makes the crown
     * work even if it is brought up first. */
    (void)i2c_bus_init(PLAT_I2C_TOUCH_BASE);

    uint8_t id1 = 0, id2 = 0;
    int rc = pat_rd(PAT_PID1, &id1);
    (void)pat_rd(PAT_PID2, &id2);

    /* Always printed, not gated: this one line is the whole difference
     * between "unpowered", "wrong address" and "answering fine". */
    con_puts("crown: pat9126 probe rc="); con_putdec((uint32_t)-rc);
    con_puts(" id1=");                    con_puthex(id1);
    con_puts(" id2=");                    con_puthex(id2);
    con_puts("\n");

#if defined(CROWN_DIAG)
    crown_diag_dump();
#endif

    /* TWO DIFFERENT FAILURES, kept apart on purpose, because they have
     * completely different fixes and the first image could not tell them
     * apart:
     *   NACK (rc != 0)      -- nothing is electrically there. On this board
     *                          that means vdd (l14) / vld (l18) are off, and
     *                          the fix is an RPM-SMD client, not this file.
     *   ACK, unexpected ID  -- something IS powered and answering at 0x75, so
     *                          the rails are fine and it is our register map
     *                          that is wrong. Carry on anyway: the motion
     *                          registers may well be right even if the ID
     *                          offset is not, reads cannot hurt, and a crown
     *                          that half-works produces far better evidence
     *                          than one that refuses to start. */
    if (rc != 0) {
        con_puts("crown: pat9126 NACK at 0x75 - sensor unpowered? crown disabled\n");
        return -1;
    }
    if (id1 != PAT_PID1_VALUE) {
        con_puts("crown: 0x75 answers but product id is unexpected - "
                 "continuing anyway, check the register map\n");
    }

    /* Deliberately minimal configuration. The vendor driver replays a long
     * table of tuning registers; every one of those is a value this port
     * cannot verify, and the sensor reports motion perfectly well in its
     * power-on defaults. Unlock, leave the defaults, lock again -- and read
     * the motion registers once to clear any stale delta latched from before
     * we arrived, so the first turn is not preceded by a phantom jump. */
    (void)pat_wr(PAT_WRITE_PROT, PAT_UNLOCK);
    (void)pat_wr(PAT_WRITE_PROT, 0x00u);

    uint8_t st = 0, dx = 0, dy = 0, hi = 0;
    (void)pat_rd(PAT_MOTION, &st);
    (void)pat_rd(PAT_DELTA_X_LO, &dx);
    (void)pat_rd(PAT_DELTA_Y_LO, &dy);
    (void)pat_rd(PAT_DELTA_XY_HI, &hi);

    s_accum = 0;
    s_ok = 1;
    con_puts("crown: pat9126 ready\n");
    return 0;
}

/* Poll for motion. Cheap and rate-limited; safe to call every loop. */
void crown_poll(void)
{
    if (!s_ok) return;

    uint32_t now = timer_ms();
    if ((uint32_t)(now - s_last_ms) < CROWN_POLL_MS) return;
    s_last_ms = now;

    /* NO LONGER GATED ON THE MOTION BIT. The first version returned early
     * unless PAT_MOTION bit 7 was set, which made the whole crown depend on
     * one bit of a register map quoted from memory: if that bit is at a
     * different offset, or is latched differently on this part, the deltas are
     * never even read and the crown looks completely dead -- which is exactly
     * what the first image did. Reading the deltas directly is one extra byte
     * on a bus that is idle anyway, and it is self-validating: the sensor
     * clears its accumulators on read, so "both deltas zero" IS "nothing
     * moved". The status byte is still read, but only so CROWN_DIAG can show
     * whether the bit tracks motion. */
    uint8_t st = 0, dx = 0, dy = 0, hi = 0;
    (void)pat_rd(PAT_MOTION, &st);
    if (pat_rd(PAT_DELTA_X_LO, &dx) != 0) return;
    if (pat_rd(PAT_DELTA_Y_LO, &dy) != 0) return;
    if (pat_rd(PAT_DELTA_XY_HI, &hi) != 0) return;

    int32_t x = pat_delta(dx, (uint8_t)(hi >> 4));
    int32_t y = pat_delta(dy, hi);

    /* WHICH AXIS IS THE CROWN: the wheel only turns one way, so only one of
     * these carries signal and the other is noise from the same surface. The
     * vendor driver reports REL_WHEEL from the Y delta, so Y is the default --
     * but that is a claim about a driver, not a measurement of this watch, so
     * the board header can flip it (PLAT_CROWN_AXIS_X) and invert it
     * (PLAT_CROWN_INVERT) without touching this file. Build with
     * -DCROWN_DIAG to see both axes and settle it from the log. */
#if defined(PLAT_CROWN_AXIS_X)
    int32_t d = x;
#else
    int32_t d = y;
#endif
#if defined(PLAT_CROWN_INVERT)
    d = -d;
#endif

#if defined(CROWN_DIAG)
    /* Heartbeat: prove the poll is RUNNING even when every byte reads zero.
     * Without it, silence is ambiguous -- a crown that is never polled and a
     * crown whose deltas are always zero look identical in the log, and that
     * ambiguity is what cost us the last round trip. */
    {
        static uint32_t s_hb;
        if ((uint32_t)(now - s_hb) >= 2000u) {
            s_hb = now;
            con_puts("crown: alive st="); con_puthex(st);
            con_puts(" dx=");             con_puthex(dx);
            con_puts(" dy=");             con_puthex(dy);
            con_puts(" hi=");             con_puthex(hi);
            con_puts("\n");
        }
    }
    if (st || dx || dy || hi) {
        cdiag_puts("crown: st="); cdiag_puthex(st);
        cdiag_puts(" x=");        cdiag_putdec((uint32_t)x);
        cdiag_puts(" y=");        cdiag_putdec((uint32_t)y);
        cdiag_puts(" hi=");       cdiag_puthex(hi);
        cdiag_puts("\n");
    }
#endif

    s_accum += d;
}

/* Hand over everything accumulated since the last call and reset. Signed:
 * positive = one direction, negative = the other. Raw sensor counts, NOT
 * detents -- the UI layer decides how many counts make a scroll step, which
 * keeps the tuning where it can be felt rather than buried in a driver. */
int crown_take_delta(void)
{
    int d = (int)s_accum;
    s_accum = 0;
    return d;
}

int crown_present(void) { return s_ok; }

#else  /* no crown on this board */

int  crown_init(void)       { return -1; }
void crown_poll(void)       { }
int  crown_take_delta(void) { return 0; }
int  crown_present(void)    { return 0; }

#endif
