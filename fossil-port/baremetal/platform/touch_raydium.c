/* touch_raydium.c — Raydium touch controller over BLSP QUP I2C (both watches).
 *
 * Gen 4 (firefish, 3.18 kernel): CONFIG_TOUCHSCREEN_RM_TS
 *   drivers/input/touchscreen/raydium_i2c_ts.c — PDA2, PAGE reg 0x0B.
 * Gen 6 (hoki, 4.14 kernel): CONFIG_TOUCHSCREEN_RAYDIUM_U128BLA03_CHIPSET —
 *   IDENTIFIED FROM THE ASTEROIDOS DEFCONFIG, no device dump needed (their
 *   kernel drives this panel, so the driver their config enables IS the part).
 *   drivers/input/touchscreen/raydium_wt030/raydium_i2c_ts.{c,h} — same PDA2
 *   register model with two deltas, both taken verbatim from that source:
 *     - PAGE register is 0x0A (RAYDIUM_PDA2_PAGE_ADDR), not 0x0B
 *     - the status byte is written back (seq ack) after each report read
 *   Slave addresses from the header: 0x39 normal (NID), 0x5A engineering (EID).
 *
 * WHAT IS STILL UNKNOWN ON THE GEN 6: which of the 8 QUP buses the controller
 * sits on (that lives in the dtbo overlay we do not have). Instead of guessing,
 * touch_init() PROBES every bus for a Raydium at 0x39/0x5A. Every probe is
 * bounded (an unclocked QUP times out in ms and moves on), so the worst case
 * is a couple of seconds once at boot, and a wrong guess is impossible — the
 * controller is wherever it actually answers a status read.
 *
 * Access model (PDA2): set page 0 via the PAGE register, then use the small
 * page-0 registers:
 *   0x00 TCH_RPT_STATUS — [0]=seq, [1]=points, ([2]=gesture, [3]=fw state)
 *   0x01 TCH_RPT        — touch report, 11 bytes per point
 *   0x02 HOST_CMD       — host command (sleep, mode, calibration)
 *
 * Scope: report reading only (no FW update / factory / gesture paths). Polled
 * from the LVGL input read callback; INT-GPIO wiring is a later refinement.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN4) || defined(PLAT_BOARD_FOSSIL_GEN6)

#include <string.h>

/* raydium register model (identical across both kernel drivers) */
#define RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR  0x00
#define RAYDIUM_PDA2_TCH_RPT_ADDR         0x01
#define RAYDIUM_PDA2_HOST_CMD_ADDR        0x02
#define RAYDIUM_PDA2_GESTURE_RPT_ADDR     0x04
#define RAYDIUM_PDA2_PALM_STATUS_ADDR     0x05
#define RAYDIUM_PDA2_FW_VERSION_ADDR      0x06
#define RAYDIUM_PDA2_PANEL_VERSION_ADDR   0x07
#define RAYDIUM_PDA2_DISPLAY_MODE_ADDR    0x08
#define RAYDIUM_PDA2_PAGE_0               0x00

#define RAYDIUM_HOST_CMD_PWR_SLEEP        0x30
#define RAYDIUM_HOST_CMD_DISPLAY_MODE     0x33
#define RAYDIUM_HOST_CMD_CALIBRATION      0x5C
#define RAYDIUM_HOST_CMD_TP_MODE          0x60

/* Report layout: 11 bytes per point (enum raydium_pt_report_idx). */
#define LENGTH_PT        11
#define POS_PT_ID        0
#define POS_X_L          1
#define POS_X_H          2
#define POS_Y_L          3
#define POS_Y_H          4
#define POS_PRESSURE_L   5
#define POS_PRESSURE_H   6

/* Status register: [0] = sequence, [1] = number of points */
#define POS_SEQ          0
#define POS_PT_AMOUNT    1
#define POS_GESTURE_STATUS 2
#define POS_FW_STATE     3

#define MAX_TOUCH_NUM    2
#define MAX_TCH_STATUS_PAKAGE_SIZE 2   /* kernel name kept for greppability */

#if defined(PLAT_BOARD_FOSSIL_GEN4)
/* 3.18 raydium_i2c_ts: known bus + address.
 * RAD_ACK_SEQ selects the Gen 6's queue-DRAINING reader, not "does this chip
 * need acking" — both do; see raydium_ack_report() and the note on touch_read
 * below. The Gen 4 keeps the kernel's simpler one-report-per-poll cycle.
 * PAGE reg is 0x0A, NOT 0x0B (corrected 2026-08-28). firefish's own
 * drivers/input/touchscreen/raydium_i2c_ts.h says
 *   #define RAYDIUM_PDA2_PAGE_ADDR 0x0A
 * exactly as the newer wt030 header does; 0x0B is not a defined register in
 * either. It went unnoticed because page 0 is the power-on default, so every
 * page-0 access worked anyway — but it meant the page write itself landed
 * nowhere, which matters as soon as anything depends on the page actually
 * being selected. */
#define RAD_PAGE_ADDR    0x0A
#define RAD_ACK_SEQ      0
#else
/* 4.14 raydium_wt030 (U128BLA03): PAGE reg 0x0A, seq is acked back, bus
 * unknown until probed. */
#define RAD_PAGE_ADDR    0x0A
#define RAD_ACK_SEQ      1
#endif

static int       s_ready;
static uintptr_t s_bus;
static uint8_t   s_addr;

/* Diagnostic: last HOST_CMD readback after the ACTIVE_MODE notify.
 * 0x00 = chip consumed the command, 0x33 = stuck/ignored, 0xEE = read
 * failed, 0xFF = never attempted. Shown on the GLASS_DIAG screen. */
uint8_t g_touch_hostcmd_rb = 0xFFu;

/* Chip-reported scan mode from PDA2 reg 0x08 (0=ACTIVE, 1=AMBIENT, 2=SLEEP,
 * 0xFF = never read). This is the register that decides whether the
 * "chip stays in ambient scan" theory is true; nothing before this build ever
 * looked at it. */
uint8_t g_touch_display_mode = 0xFFu;

/* GLASS_DIAG counters, cheap and always maintained: how many finger-down
 * reports the chip delivered during the LAST tap, and how many times the
 * silence watchdog (not the chip) had to end a press. Together they separate
 * "chip streams pressed for 2 s after liftoff" (RPTS large, WDOG 0 — a
 * baseline problem in the chip) from "driver lost the release" (WDOG>0). */
uint32_t g_touch_last_rpts;
uint32_t g_touch_wdog_cnt;
static uint32_t s_cur_rpts;

/* PDA2 page select — must precede page-0 register access. */
static int raydium_set_page(uint8_t page)
{
    uint8_t buf[2] = { RAD_PAGE_ADDR, page };
    return i2c_bus_xfer(s_bus, s_addr, buf, 2, 0, 0);
}

/* Page-less variants. KERNEL-EXACT PAGE HANDLING (2026-08-06):
 * raydium_read_touchdata() sets the page ONCE, then does status-read,
 * report-read and the seq ack with no further page writes. We set it before
 * every single transfer, tripling the transactions per poll and interleaving a
 * write to register 0x0A between the status read and its ack — the exact
 * window in which the chip decides whether to load the next report. Keep the
 * paged helpers for one-shot init work; the hot path below sets the page once
 * per drain iteration and then uses these. */
static int raydium_reg_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
    return i2c_bus_xfer(s_bus, s_addr, &reg, 1, buf, len);
}

static int raydium_reg_write(uint8_t reg, const uint8_t *data, uint32_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof buf) return -1;
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_bus_xfer(s_bus, s_addr, buf, len + 1, 0, 0);
}

static int raydium_pda2_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
    if (raydium_set_page(RAYDIUM_PDA2_PAGE_0) < 0) return -1;
    return raydium_reg_read(reg, buf, len);
}

static int raydium_pda2_write(uint8_t reg, const uint8_t *data, uint32_t len)
{
    if (raydium_set_page(RAYDIUM_PDA2_PAGE_0) < 0) return -1;
    return raydium_reg_write(reg, data, len);
}

/* Liveness probe on (s_bus, s_addr): a status read that ACKs. */
static int raydium_probe_here(void)
{
    uint8_t status[4];
    return raydium_pda2_read(RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR, status, 2);
}

/* THE SLOW-TOUCH FIX (2026-08-04, TOUCH7; extended to the Gen 4 2026-08-28). Post-reset the chip sits in its
 * slow ambient/idle scan: finger-DOWN is reported instantly (wake event) but
 * everything after — including the RELEASE — waits for the ~3 s idle scan
 * cadence, which read as "every tap is a 3 s long-press". The wt030 kernel
 * driver's fb notifier tells the chip the display state on every unblank
 * (HOST_NOTIFY_EN, raydium_notify_function): HOST_CMD 0x33 DISPLAY_MODE with
 * a 16-bit mode — ACTIVE_MODE 0x00 = full-rate scan. Wear OS sends it at
 * every screen-on; we never did. Payload layout kernel-verbatim:
 * {0x33, 0x00, mode_lo, mode_hi} written at HOST_CMD (0x02), page 0.
 *
 * WHY THIS ALSO APPLIES TO THE GEN 4 (2026-08-28). Its first touch-enabled
 * boot reproduced the Gen 6's symptom exactly: presses register, but each one
 * takes seconds to appear and stays held for seconds after the finger lifts,
 * so swiping is impossible. Note that firefish's OWN 3.18 driver never sends
 * this command — it defines DISPLAY_MODE_ADDR and the ACTIVE/AMBIENT/SLEEP
 * constants and then uses none of them, and its suspend/resume handlers only
 * toggle the IRQ. So the command is not copied from that driver; it is the
 * same PDA2 chip protocol, and the identical symptom on the identical vendor
 * part is the reason to expect the identical cure. If the chip firmware here
 * does not implement 0x33 the write is simply dequeued with no effect, which
 * the DISPLAY_MODE readback below will show. */
#define RAYDIUM_DISPLAY_ACTIVE_MODE   0x0000u
static int raydium_notify_display_mode(uint16_t mode)
{
    uint8_t buf[4];
    buf[0] = RAYDIUM_HOST_CMD_DISPLAY_MODE;
    buf[1] = 0x00;
    buf[2] = (uint8_t)(mode & 0xFFu);
    buf[3] = (uint8_t)(mode >> 8);
    return raydium_pda2_write(RAYDIUM_PDA2_HOST_CMD_ADDR, buf, 4);
}

#if defined(PLAT_BOARD_FOSSIL_GEN6)
/* Forced baseline CALIBRATION (2026-08-04, TOUCH9 — last protocol-side shot).
 * Symptom fit: press detection is instant but release lags by a WILDLY
 * VARYING 1-5 s — the signature of a capacitance baseline zeroed under the
 * wrong electrical conditions (our rail/display bring-up differs from
 * stock), leaving the release threshold marginal. Kernel sysfs calibration
 * handler: write HOST_CMD 0x5C, then poll HOST_CMD until the chip clears it
 * to NO_OP 0x00 (seconds). Must run with NOTHING touching the glass — boot
 * time is exactly that. */
static void touch_gen6_calibrate(void)
{
    uint8_t cmd = RAYDIUM_HOST_CMD_CALIBRATION;
    if (raydium_pda2_write(RAYDIUM_PDA2_HOST_CMD_ADDR, &cmd, 1) < 0) {
        con_puts("touch: calibration cmd FAILED\n");
        return;
    }
    uint32_t t0 = timer_ms();
    while ((uint32_t)(timer_ms() - t0) < 5000u) {
        timer_delay_ms(100);
        uint8_t v = 0xFF;
        if (raydium_pda2_read(RAYDIUM_PDA2_HOST_CMD_ADDR, &v, 1) == 0 &&
            v == 0x00u) {
            bdiag_puts("touch: calibration done\n");
            return;
        }
    }
    con_puts("touch: calibration timeout (continuing)\n");
}
#else
#define touch_gen6_calibrate() ((void)0)
#endif

static void touch_activate(void)
{
    /* FW VERSION CHECK — SETTLED 2026-08-06, THEORY DEAD.
     * The suspicion was that AsteroidOS's PRAM firmware upload (IC 0x2120003
     * -> image 0x2120004) is RAM-resident, so our port always runs the old,
     * ambient-only firmware. It does not: our decode was simply wrong.
     * raydium_fw_update_check() builds the version as
     *     (b[0]<<24) | (b[1]<<16) | (b[3]<<8) | b[2]
     * -- bytes 2 and 3 SWAPPED, not straight big-endian. Our raw bytes
     * 02 12 04 00 therefore read 0x02120004, i.e. exactly the image version
     * AsteroidOS upgrades TO. The chip already carries the latest firmware and
     * no upload is needed. Decode kernel-verbatim so the number stays
     * comparable to their dmesg. */
    {
        uint8_t v[4] = { 0, 0, 0, 0 };
        if (raydium_pda2_read(RAYDIUM_PDA2_FW_VERSION_ADDR, v, 4) == 0) {
            bdiag_puts("touch: IC FW version 0x");
            bdiag_puthex(((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                       ((uint32_t)v[3] << 8) | v[2]);
            bdiag_puts(" (raw ");
            bdiag_puthex(v[0]); bdiag_puts(" "); bdiag_puthex(v[1]); bdiag_puts(" ");
            bdiag_puthex(v[2]); bdiag_puts(" "); bdiag_puthex(v[3]);
            bdiag_puts(")\n");
        } else {
            con_puts("touch: IC FW version read FAILED\n");
        }
    }
    if (raydium_notify_display_mode(RAYDIUM_DISPLAY_ACTIVE_MODE) == 0) {
        /* WRITE-LANDED PROOF (2026-08-06). "hostcmd readback=0x00" has been
         * read as "the chip consumed the command", but 0x00 is ALSO what an
         * untouched HOST_CMD register reads — the two are indistinguishable
         * after a settle delay, and the QUP truncated our pure writes, so the
         * command may never have arrived at all. Read the register with NO
         * delay first: a write that landed is briefly visible as 0x33 before
         * the firmware dequeues it. hcnow=0x33 => the write reaches the chip;
         * hcnow=0x00 both times => it never did. */
        uint8_t hcnow = 0xFFu;
        if (raydium_pda2_read(RAYDIUM_PDA2_HOST_CMD_ADDR, &hcnow, 1) != 0)
            hcnow = 0xEEu;
        bdiag_puts("touch: hostcmd immediate readback="); bdiag_puthex(hcnow);
        bdiag_puts(" (0x33 = the write reached the chip)\n");
        timer_delay_ms(20);
        uint8_t hc = 0xFF;
        int hrc = raydium_pda2_read(RAYDIUM_PDA2_HOST_CMD_ADDR, &hc, 1);
        g_touch_hostcmd_rb = (hrc == 0) ? hc : 0xEEu;
        bdiag_puts("touch: display-mode ACTIVE sent, hostcmd readback=");
        bdiag_puthex(g_touch_hostcmd_rb); bdiag_puts("\n");
        /* HOST_CMD clearing to NO_OP only proves the command was DEQUEUED, not
         * that the scan mode changed — which is why "readback=0x00" has been
         * consistent with a chip that still reports one event per press. The
         * header carries a dedicated status register for this,
         * RAYDIUM_PDA2_DISPLAY_MODE_ADDR (0x08, page 0). The stock driver never
         * reads it, so we have never once observed the chip's ACTUAL mode.
         * Expect 0x00 = ACTIVE, 0x01 = AMBIENT, 0x02 = SLEEP. */
        uint8_t dm[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        if (raydium_pda2_read(RAYDIUM_PDA2_DISPLAY_MODE_ADDR, dm, 4) == 0) {
            g_touch_display_mode = dm[0];
            bdiag_puts("touch: display-mode readback ");
            bdiag_puthex(dm[0]); bdiag_puts(" "); bdiag_puthex(dm[1]); bdiag_puts(" ");
            bdiag_puthex(dm[2]); bdiag_puts(" "); bdiag_puthex(dm[3]); bdiag_puts("\n");
        } else
            con_puts("touch: display-mode readback FAILED\n");
    } else
        con_puts("touch: display-mode notify FAILED\n");
#if defined(TOUCH_CALIBRATE) && defined(PLAT_BOARD_FOSSIL_GEN6)
    /* OPT-IN ONLY (2026-08-06). RAYDIUM_HOST_CMD_CALIBRATION appears in the
     * kernel tree exclusively in raydium_sysfs.c -- a manual debug knob. The
     * driver NEVER calibrates at probe, on resume, or on unblank. We invented
     * a boot-time calibration, which both blocks ~5 s and forces a capacitance
     * baseline under our own rail/display conditions rather than the ones the
     * factory baseline was taken under. Prime suspect for leaving the chip in
     * a degraded scan state, so it is off unless explicitly asked for. */
    touch_gen6_calibrate();
#endif
}

int touch_init(void)
{
#if defined(PLAT_BOARD_FOSSIL_GEN4)
    s_bus  = PLAT_I2C_TOUCH_BASE;
    s_addr = PLAT_TOUCH_I2C_ADDR;
    /* Route SDA/SCL to blsp_i2c5 and give the reset and INT lines a defined
     * state before the first transfer. The bus happens to work without this
     * (aboot leaves the pins usable), but "happens to work" is not a state to
     * build on, and the reset line needs to be driven HIGH rather than left
     * floating. tlmm_touch_setup() deliberately does NOT pulse reset — see the
     * note there; the chip self-boots at power-on and an unnecessary reset is
     * what leaves it in a degraded scan state. */
    tlmm_touch_setup();
    if (i2c_bus_init(s_bus) < 0) return -1;
    if (raydium_probe_here() < 0) {
        con_puts("touch: raydium not responding at 0x");
        bdiag_puthex(s_addr); bdiag_puts("\n");
        return -1;
    }
    /* Tell the chip the display is ON and it should scan at full rate. */
    touch_activate();
#else
    /* Gen 6: try the CONFIRMED location first — the device's own /sys tree
     * (dumped 2026-08-02) shows the controller as 4-0039 on 78b8000.i2c, i.e.
     * BLSP1 QUP4 at address 0x39. The full probe below stays as a fallback in
     * case a unit differs or the bus is not clocked yet. */
    /* 2026-08-04: clocks alone were not enough — aboot never routes the I2C
     * pins to the QUP (SDA/SCL sat in reset-default GPIO mode, chip mute on a
     * healthy bus). Mux gpio14/15 -> blsp_i2c4 and give the Raydium its
     * reset pulse (TLMM 64) before the first probe. */
    tlmm_touch_setup();
    s_bus = PLAT_I2C_TOUCH_BASE;
    s_addr = PLAT_TOUCH_I2C_ADDR;
    if (i2c_bus_init(s_bus) == 0) {
        /* NO-RESET-FIRST (2026-08-06): stock never resets the chip on a
         * normal boot — it inherits the self-booted, fully-scanning chip.
         * Our every-boot hard reset left it in an ambient-only state that no
         * stock I2C command recovers. So: probe the chip AS-IS first; only
         * if it stays mute, pulse reset (fallback) and retry across the
         * chip's ~360 ms boot window. */
        for (unsigned pass = 0; pass < 2; pass++) {
            for (unsigned tries = 0; tries < 6; tries++) {
                if (raydium_probe_here() == 0) {
                    bdiag_puts(pass ? "touch: raydium answered AFTER reset pulse\n"
                                  : "touch: raydium answered WITHOUT reset\n");
                    touch_activate();  /* full-rate scan notify */
                    s_ready = 1;
                    return 0;
                }
                timer_delay_ms(80u);
            }
            if (pass == 0) tlmm_touch_reset_pulse();
        }
    }
    con_puts("touch: not at the known bus; probing all QUPs\n");

    static const uintptr_t buses[] = {
        PLAT_I2C_BLSP1_QUP1, PLAT_I2C_BLSP1_QUP2,
        PLAT_I2C_BLSP1_QUP3, PLAT_I2C_BLSP1_QUP4,
        PLAT_I2C_BLSP2_QUP2, PLAT_I2C_BLSP2_QUP3,
        PLAT_I2C_BLSP2_QUP4, PLAT_I2C_BLSP2_QUP1,   /* QUP5 last: NFC's bus */
    };
    static const uint8_t addrs[] = { 0x39, 0x5A };
    unsigned b, a;

    s_ready = 0;
    for (b = 0; b < sizeof buses / sizeof buses[0]; b++) {
        if (i2c_bus_init(buses[b]) < 0) continue;      /* unclocked QUP */
        for (a = 0; a < sizeof addrs / sizeof addrs[0]; a++) {
            s_bus = buses[b]; s_addr = addrs[a];
            if (raydium_probe_here() == 0) goto found;
        }
    }
    con_puts("touch: no raydium found on any QUP\n");
#if defined(TOUCH_DIAG)
    /* Failure-class report ON THE GLASS (the ramlog is unreadable and the
     * display provably works — 2026-08-04). One color, ~1 s, then boot
     * continues:
     *   MAGENTA = i2c_bus_init failed (QUP/clock level — before any chip)
     *   RED     = bus fine, chip mute, and PM660 L13 (vcc_i2c, rpm-smd
     *             regulator, dump phandle 0x97) reads DISABLED -> the rail
     *             is the blocker (needs an RPM/owner-safe enable)
     *   GREEN   = bus fine, chip mute, L13 commanded ON -> rail fine; suspect
     *             pins/reset polarity/QUP protocol instead
     *   YELLOW  = L13 status unreadable over SPMI (observer) — no verdict
     * L13 = sid 0, qpnp LDO base 0x4C00, EN_CTL 0x46 bit 7. READ ONLY —
     * regulators are RPM-owned; a direct write is the TZ-reset class. */
    {
        /* 2026-08-04: sid 0 read came back YELLOW (unreadable) on hardware —
         * PM660 splits its peripherals across two slave ids and the haptics
         * already live on sid 1, so the regulator blocks most likely do too.
         * Try sid 1 first, keep sid 0 as fallback. */
        uint8_t en = 0;
        if (i2c_bus_init(PLAT_I2C_TOUCH_BASE) < 0) {
            fb_trace(0xFF00FFu);                        /* magenta */
        } else {
            /* QUP driver rev 2 (2026-08-04) distinguishes failure classes;
             * flash the PROBE verdict first — it decides the next campaign:
             *   BLUE = NACK: the QUP ran the transfer and the wire works
             *          electrically, but nothing ACKed 0x39 (chip-side:
             *          power timing / reset / wrong address)
             *   CYAN = timeout: the QUP never completed — protocol engine
             *          still wrong, chip verdict unknown */
            /* direct xfer so the -2 NACK / -1 timeout class survives (the
             * pda2 helpers flatten everything to -1) */
            uint8_t pg[2] = { RAD_PAGE_ADDR, RAYDIUM_PDA2_PAGE_0 };
            int pr = i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, PLAT_TOUCH_I2C_ADDR,
                                  pg, 2, 0, 0);
            if (pr == 0) {
                /* late ACK — the chip finished booting after the retry
                 * window: WHITE, then just use it. */
                fb_trace(0xFFFFFFu);
                s_bus = PLAT_I2C_TOUCH_BASE; s_addr = PLAT_TOUCH_I2C_ADDR;
                touch_activate();
                s_ready = 1;
                return 0;
            }
            fb_trace((pr == -2) ? 0x0000FFu : 0x00FFFFu);   /* blue : cyan */
            timer_delay_ms(400);
            /* then the rail state, as before */
            if (spmi_read8(1, 0x4C46u, &en) == 0 ||
                spmi_read8(0, 0x4C46u, &en) == 0) {
                fb_trace((en & 0x80u) ? 0x00FF00u : 0xFF0000u); /* green : red */
            } else {
                fb_trace(0xFFFF00u);                    /* yellow */
            }
        }
        timer_delay_ms(600);
    }
#endif
    return -1;
found:
    bdiag_puts("touch: raydium at bus 0x"); bdiag_puthex((uint32_t)s_bus);
    bdiag_puts(" addr 0x"); bdiag_puthex(s_addr); bdiag_puts("\n");
    touch_activate();
#endif

    s_ready = 1;
    return 0;
}

/* Read the current touch state. Returns 1 if a finger is down (and fills the
 * x and y out-params), 0 if not, negative on I2C error.
 *
 * Gen 6 STICKY-TOUCH FIX (2026-08-04): the first working-touch build held
 * every press 2-3 s too long. Root cause: the wt030 contract is a QUEUE of
 * sequence-numbered reports — the kernel (raydium_read_touchdata) reads the
 * status, SKIPS if the seq number is unchanged ("report not updated"), and
 * acks EVERY fresh report including the pts=0 finger-up one. Our first cut
 * ignored seq, never acked pts=0 reports, and consumed at most ONE report
 * per LVGL poll (~30 Hz) while a touched chip queues them faster — so after
 * lift-off we replayed the stale pressed backlog for seconds. Now each poll
 * drains the whole backlog (bounded) and keeps the LAST report as truth. */
/* Clear the sequence byte of TCH_RPT_STATUS: "inform IC to prepare next
 * report" (raydium_read_touchdata). One byte, page 0. Both watches need it —
 * without it the controller never publishes another report and the last one
 * stays latched, which shows up as a finger that will not lift. */
static int raydium_ack_report(void)
{
    uint8_t zero = 0;
    return raydium_pda2_write(RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR, &zero, 1);
}

/* Shared by both read paths. */
static int      s_down;
static uint16_t s_last_x, s_last_y;

#if RAD_ACK_SEQ
static uint8_t  s_seq;                    /* kernel: static u8_seq_no */
static uint8_t  s_st[4];                  /* last status successfully read */
static uint32_t s_int_seen, s_int_t0;
static int      s_int_ok, s_int_fallback;
static uint32_t s_last_pts_t;             /* ms of the last points>0 report */
static uint32_t s_last_rpt_t;             /* ms of the last consumed report */
static uint8_t  s_fw_warned = 0x1Au;      /* last abnormal fw_state announced */
static uint32_t s_esd_resets;             /* bounded ESD hw-reset rescues */

/* GHOST-HOLD RELEASE WATCHDOG (2026-08-06) — the fix, not just a measurement.
 *
 * Hardware measurement: EVERY tap reports held=2067..2068 ms no matter how
 * briefly the glass is touched, with only 2 consumed reports across the whole
 * hold. And note the caller's contract (owf_fossil_lvgl.h): it treats a return
 * of 1 as PRESSED and everything else, -1 included, as RELEASED. So for LVGL
 * to have held the press for 2 s, our status reads must have been SUCCEEDING
 * and status[POS_SEQ] must genuinely not have changed -- the chip went quiet,
 * exactly the slow ambient-scan behaviour the TOUCH7 note describes.
 *
 * Whatever keeps the chip in that scan mode, reporting "still pressed" because
 * the chip stopped talking is wrong on its face: a finger that is really down
 * produces a continuous report stream, so silence means "no longer known to be
 * touched". Release on silence. A drag refreshes s_last_pts_t on every report
 * and is unaffected; only the phantom tail is cut. */
/* Measured 2026-08-06: TIDLE dseq=20 per 5 s => the chip publishes only ~4
 * reports/s in ambient scan, i.e. one every ~250 ms. The first watchdog used
 * 180 ms, BELOW that cadence, so it released before the chip could ever
 * confirm the touch -- which is exactly why taps mostly did nothing and only
 * landed when a finger happened to fall just after a scan boundary. The
 * timeout has to exceed the slowest cadence we expect to survive; once
 * ACTIVE_MODE holds, reports stream at 60-120 Hz and this never fires. */
#ifndef TOUCH_HOLD_MS
#define TOUCH_HOLD_MS 350u
#endif

#if defined(TOUCH_LOG)
/* PER-TAP EVENT TRACE. The previous revision logged only CONSUMED reports,
 * which made the 2 s gap a black hole -- a silent chip and a dead bus looked
 * identical. This records what happened on EVERY poll of a press, including
 * the two status bytes we have never once examined:
 *
 *   ges  status[POS_GES_STATUS] -- gesture/palm state
 *   fw   status[POS_FW_STATE]   -- the kernel gates on this being 0x1A or
 *        0xAA and treats anything else as an abnormal IRQ worth a hw reset.
 *        If the chip drops to some other state on press, this byte says so.
 *
 * Quiet polls coalesce into one entry with a repeat count, so a 2 s silence
 * costs one line instead of a hundred. Kinds:
 *   q  quiet   read OK, seq unchanged -- the chip had nothing to say
 *   r  report  a fresh report consumed and acked
 *   e  err     the status read FAILED (rc = the i2c class)
 *   a  ackerr  the seq ack write failed
 *   w  wdog    the release watchdog fired
 */
#define TAP_EV_MAX 40
struct tap_ev {
    uint32_t t;        /* ms since press */
    uint16_t rep;      /* consecutive identical polls collapsed into this */
    uint8_t  kind, seq, npts, ges, fw;
    int8_t   rc;
    uint16_t x, y;
};
static struct tap_ev s_ev[TAP_EV_MAX];
static unsigned  s_ev_n, s_ev_drop;
static uint32_t  s_tap_press_t, s_tap_rpts, s_tap_pgt0, s_tap_lastp_t;
static uint32_t  s_tap_err, s_tap_same, s_tap_erc, s_tap_ackerr;
static int       s_tap_prev_down, s_tap_wdog;
static uint8_t   s_tap_ackrb = 0xFFu, s_tap_ackrb_seen;

static void ev_push(uint8_t kind, int rc, const uint8_t *st, uint16_t x, uint16_t y)
{
    uint8_t seq = st ? st[POS_SEQ] : 0, np = st ? st[POS_PT_AMOUNT] : 0;
    uint8_t ge = st ? st[2] : 0,        fw = st ? st[3] : 0;
    if (s_ev_n) {
        struct tap_ev *l = &s_ev[s_ev_n - 1];
        /* reports are always distinct; quiet/error runs collapse */
        if (kind != 'r' && l->kind == kind && l->rc == (int8_t)rc &&
            l->seq == seq && l->npts == np && l->ges == ge && l->fw == fw) {
            if (l->rep < 0xFFFFu) l->rep++;
            return;
        }
    }
    if (s_ev_n >= TAP_EV_MAX) { s_ev_drop++; return; }
    struct tap_ev *e = &s_ev[s_ev_n++];
    e->t = timer_ms() - s_tap_press_t; e->rep = 1; e->kind = kind;
    e->rc = (int8_t)rc; e->seq = seq; e->npts = np; e->ges = ge; e->fw = fw;
    e->x = x; e->y = y;
}

static void ev_dump(void)
{
    for (unsigned i = 0; i < s_ev_n; i++) {
        struct tap_ev *e = &s_ev[i];
        char k[2]; k[0] = (char)e->kind; k[1] = 0;
        bdiag_puts("TAPEV ");   bdiag_putdec(i);
        bdiag_puts(" dt=");     bdiag_putdec(e->t);
        bdiag_puts(" ");        bdiag_puts(k);
        bdiag_puts(" n=");      bdiag_putdec(e->rep);
        bdiag_puts(" rc=");     bdiag_putdec((uint32_t)(e->rc < 0 ? -e->rc : e->rc));
        bdiag_puts(" seq=");    bdiag_puthex(e->seq);
        bdiag_puts(" np=");     bdiag_putdec(e->npts);
        bdiag_puts(" ges=");    bdiag_puthex(e->ges);
        bdiag_puts(" fw=");     bdiag_puthex(e->fw);
        if (e->kind == 'r' && e->npts) {
            bdiag_puts(" x="); bdiag_putdec(e->x);
            bdiag_puts(" y="); bdiag_putdec(e->y);
        }
        bdiag_puts("\n");
    }
    if (s_ev_drop) { bdiag_puts("TAPEV +"); bdiag_putdec(s_ev_drop); bdiag_puts(" dropped\n"); }
}
#endif /* TOUCH_LOG */

int touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t status[4];
    uint8_t rpt[MAX_TOUCH_NUM * LENGTH_PT];
#if defined(TOUCH_TRACE)
    static uint32_t s_last_fresh = 0x404040u;
    static uint32_t s_beat;
#endif

    if (!s_ready) return -1;

    /* QUEUE-DRAIN rev 2 (2026-08-04). After an ack the chip needs a moment to
     * publish the NEXT queued report, so an instant re-read sees "seq
     * unchanged" and quits after consuming ONE report per poll while a touched
     * chip queues them faster. When we HAVE consumed something this poll,
     * treat "seq unchanged" as possibly-stale and re-read up to twice with a
     * 1 ms settle, draining the backlog within one or two polls. */
    /* INT GATE (2026-08-06). raydium,irq-gpio = TLMM 65, active low. The
     * kernel is purely interrupt-driven: INT falls when a report is ready, the
     * handler reads it and acks, and the chip then loads the next one. We
     * polled blindly and acked whenever we felt like it, racing the chip's
     * report-load -- which is why a whole finger-drag produced one report and
     * scrolling was impossible. Poll the PIN instead of the bus: identical
     * semantics to the kernel, no interrupt plumbing required.
     *
     * Fallback: if INT never asserts (wrong polarity, pin not routed on this
     * unit), fall back to the seq model permanently rather than leaving touch
     * dead, and say so once in the log. */
    /* FALLBACK LATCH BUG (fixed 2026-08-06). The 3 s "INT never asserted"
     * timer starts at the FIRST POLL, i.e. during boot, when nobody is
     * touching the watch — so INT is correctly high and the fallback ALWAYS
     * armed. It then latched forever: the log shows the line going on to
     * assert 1223 times while every TIDLE still printed imode=0. The whole
     * session ran blind on the seq model with the interrupt path switched off.
     *
     * A line that asserts is a line that works, whenever it proves it. Make
     * the fallback a REVOCABLE state, not a latch: the first real assertion
     * clears it and promotes us to INT mode for good. */
    int intr = (tlmm_in(PLAT_TOUCH_INT_GPIO) == 0);   /* active low */
    if (intr) {
        s_int_seen++;
        if (!s_int_ok || s_int_fallback) {
            s_int_ok = 1;
            s_int_fallback = 0;
            bdiag_puts("touch: INT gpio asserted -- interrupt mode live\n");
        }
    }
    if (!s_int_ok) {
        /* Not yet proven working: run the seq model meanwhile, and say so once
         * so a genuinely dead INT line is visible in the log. */
        if (!s_int_t0) s_int_t0 = timer_ms();
        else if (!s_int_fallback && (uint32_t)(timer_ms() - s_int_t0) > 3000u) {
            s_int_fallback = 1;
            bdiag_puts("touch: INT gpio not asserted yet in 3 s -- seq model "
                     "until it does\n");
        }
    }
    unsigned drained = 0, settle = 0;
    /* With a working INT, a de-asserted line means "no report" — but ONLY if
     * the chip holds INT low until the report is read (level semantics). The
     * kernel requests IRQF_TRIGGER_FALLING, i.e. the line may just PULSE per
     * report; polled at 15 ms we would miss most pulses and the gate would
     * starve us of reports — missed taps AND stuck presses. So the gate is a
     * fast-path hint only: it never skips the bus while a press is in flight
     * (a missed release is the exact ghost-hold bug), and it forces a real
     * status read at least every 100 ms so a missed pulse costs one poll
     * interval, not the whole tap. */
    static uint32_t s_last_bus_t;
    if (s_int_ok && !s_int_fallback && !intr && !s_down &&
        (uint32_t)(timer_ms() - s_last_bus_t) < 100u) {
#if defined(TOUCH_LOG)
        s_tap_same++;
        ev_push('i', 0, s_st, 0, 0);      /* i = INT idle, bus untouched */
#endif
        goto after_drain;
    }
    for (;;) {
        if (drained >= 12u) break;
        s_last_bus_t = timer_ms();
        /* Page ONCE per iteration, kernel-exact; every access below is
         * page-less so no write to reg 0x0A lands between the status read and
         * its ack. */
        int rrc = raydium_set_page(RAYDIUM_PDA2_PAGE_0);
        if (rrc == 0)
            rrc = raydium_reg_read(RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR, status, 4);
        if (rrc < 0) {
#if defined(TOUCH_LOG)
            s_tap_err++; s_tap_erc = (uint32_t)(-rrc);
            ev_push('e', rrc, 0, 0, 0);
#endif
#if defined(TOUCH_TRACE)
            s_last_fresh = 0xFFFF00u;
            fb_dbg_mark(0, s_last_fresh);
#endif
            /* Do NOT return early: the watchdog below still has to run, and
             * bailing here is what previously made an error path invisible. */
            break;
        }
        s_st[0] = status[0]; s_st[1] = status[1];
        s_st[2] = status[2]; s_st[3] = status[3];

        /* ESD GATE, kernel-verbatim condition (raydium_read_touchdata /
         * raydium_esd_check): FW state must be 0x1A (or 0xAA in the esd path);
         * anything else is "abnormal irq" and the driver hard-resets the chip.
         * We have never checked it. Report rather than reset — a hard reset is
         * exactly the thing the no-reset-first note says leaves this chip in a
         * degraded state — but a state other than 0x1A/0xAA showing up here
         * would immediately explain a chip that stops publishing reports. */
        if (status[POS_FW_STATE] != 0x1Au && status[POS_FW_STATE] != 0xAAu) {
            if (status[POS_FW_STATE] != s_fw_warned) {
                s_fw_warned = status[POS_FW_STATE];
                con_puts("touch: ABNORMAL fw_state 0x");
                bdiag_puthex(status[POS_FW_STATE]);
                bdiag_puts("\n");
            }
            /* Kernel contract (raydium_read_touchdata): an abnormal FW state
             * is not a report, it is a wedged chip, and the driver hw-resets
             * and starts over. We only ever printed it and then carried on
             * parsing the bytes as if they were a touch. Reset — but bounded,
             * because the no-reset-first note is right that a reset storm is
             * its own failure mode: a couple of rescues per session, then we
             * live with it and keep reporting. */
            if (s_esd_resets < 2u) {
                s_esd_resets++;
                con_puts("touch: hw reset (ESD rescue "); con_putdec(s_esd_resets);
                bdiag_puts("/2)\n");
                tlmm_touch_reset_pulse();
                timer_delay_ms(100);
                s_seq = 0; s_down = 0; s_cur_rpts = 0;
                raydium_notify_display_mode(RAYDIUM_DISPLAY_ACTIVE_MODE);
            }
            break;
        }

        /* KERNEL-EXACT SEQ TEST (2026-08-06, the 2 s ghost-hold root cause).
         * Measured RPTS:1188 over a 2.1 s phantom = ~570 consumes/s, several
         * times any real scan rate -- we were re-consuming the SAME report.
         * The earlier model assumed the status register keeps reading 0 after
         * our ack; it only reads 0 transiently, then the chip's (unchanged)
         * seq is back, and "seq != 0" counted that as fresh on every poll.
         * Each false consume also RE-ACKED the status register (~500/s),
         * which the kernel never does -- it acks only genuinely new reports
         * (raydium_read_touchdata: skip when seq == u8_seq_no). Match it:
         * s_seq holds the LAST CONSUMED seq; equal seq = no news, and seq 0
         * = our own ack transient. Neither is consumed, neither is acked. */
        if (status[POS_SEQ] == s_seq || status[POS_SEQ] == 0u) {
            if (drained == 0 || settle >= 2u) {
#if defined(TOUCH_LOG)
                if (drained == 0) { s_tap_same++; ev_push('q', 0, status, 0, 0); }
#endif
                break;                     /* truly no new report: state holds */
            }
            settle++;
            timer_delay_ms(1);             /* chip publish latency after ack */
            continue;
        }
        settle = 0;
        drained++;
        s_last_rpt_t = timer_ms();

        uint8_t npts = status[POS_PT_AMOUNT];
        if (npts <= MAX_TOUCH_NUM && npts != 0) {
            if (raydium_reg_read(RAYDIUM_PDA2_TCH_RPT_ADDR, rpt,
                                 npts * LENGTH_PT) < 0) {
#if defined(TOUCH_LOG)
                s_tap_err++; ev_push('e', -9, status, 0, 0);
#endif
                break;
            }
        }

        /* Consume + ack, kernel-verbatim: remember the consumed seq, write a
         * single 0 byte to the status register. The seq-0 guard above absorbs
         * the post-ack readback transient that the old model mishandled. */
        s_seq = status[POS_SEQ];
        {
            uint8_t zero = 0;
            int arc = raydium_reg_write(RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR, &zero, 1);
            if (arc < 0) {
#if defined(TOUCH_LOG)
                s_tap_ackerr++; ev_push('a', arc, status, 0, 0);
#endif
            }
#if defined(TOUCH_LOG)
            /* THE DISCRIMINATOR (2026-08-06). Across every logged tap the
             * status register read back seq=0x01 on all 21 quiet polls and
             * NEVER ONCE read 0 — yet ackerr=0, so the write itself succeeded
             * on the bus. Two incompatible stories fit that:
             *   (a) the ack lands, the chip re-publishes the same seq because
             *       it has nothing new (genuine slow/ambient scan), or
             *   (b) the ack never reaches the register, so the chip is stalled
             *       waiting for the host to clear a report it already queued.
             * One byte read immediately after the write separates them: 0x00
             * means the ack landed (-> (a), a scan-rate problem), anything
             * else means it did not (-> (b), a protocol problem). Nothing in
             * the driver has ever looked at this. */
            uint8_t rb = 0xFEu;
            if (raydium_reg_read(RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR, &rb, 1) < 0)
                rb = 0xEEu;
            s_tap_ackrb = rb;
            s_tap_ackrb_seen = 1;
#endif
        }

        uint16_t px = 0, py = 0;
        if (npts && npts <= MAX_TOUCH_NUM) {
            px = (uint16_t)(rpt[POS_X_L] | ((uint16_t)rpt[POS_X_H] << 8));
            py = (uint16_t)(rpt[POS_Y_L] | ((uint16_t)rpt[POS_Y_H] << 8));
        }
#if defined(TOUCH_LOG)
        s_tap_rpts++;
        ev_push('r', 0, status, px, py);
#endif
        if (npts == 0 || npts > MAX_TOUCH_NUM) {
            if (s_down) g_touch_last_rpts = s_cur_rpts;
            s_cur_rpts = 0;
            s_down = 0;                    /* finger up (or corrupt: release) */
#if defined(TOUCH_TRACE)
            s_last_fresh = 0xFF0000u;
#endif
        } else {
            s_cur_rpts++;
            s_down = 1;                    /* first contact only: single-touch UI */
            s_last_x = px; s_last_y = py;
            s_last_pts_t = timer_ms();
#if defined(TOUCH_LOG)
            s_tap_pgt0++; s_tap_lastp_t = s_last_pts_t;
#endif
#if defined(TOUCH_TRACE)
            s_last_fresh = 0x00FF00u;
#endif
        }
#if defined(TOUCH_TRACE)
        s_beat++;
#endif
    }

after_drain:
    /* SCAN-MODE UPKEEP, rev 2 (2026-08-06). The previous revision fired
     * ACTIVE_MODE unconditionally every 2 s. That is not what the kernel does
     * — raydium_notify_function() runs only on an actual FB blank transition —
     * and it means a HOST_CMD write lands in the middle of a chip we are
     * polling at 66 Hz, roughly 30 times per idle minute, for no evidence it
     * was ever needed. Poll the chip's OWN mode register instead and only
     * command it when it disagrees. Same protection, ~1 write per session
     * instead of 30 per minute, and dmode= in the TAP line now records what
     * the chip actually thinks its scan mode is. */
    /* SCAN-MODE UPKEEP, rev 3 (2026-08-06). Rev 2 polled PDA2 reg 0x08 and
     * re-sent DISPLAY_MODE whenever it did not read 0x00. On hardware that
     * register reads 0xA0 — it is not a mode readback on this firmware, 4-byte
     * reads give 0xA0 0x01 0xA0 0x01 — so the condition was ALWAYS true and we
     * fired a HOST_CMD at the chip every 2 s forever, ~30 times an idle
     * minute, which is exactly what rev 2 set out to stop doing. Keep reg 0x08
     * as a logged diagnostic only.
     *
     * The only re-notify that is justified is a rescue: the chip is asserting
     * INT (it wants to say something) yet has published nothing for 10 s. That
     * is a wedged chip, and one command every 10 s at most is not spam. */
    {
        static uint32_t s_act_win, s_dm_win;
        uint32_t now2 = timer_ms();
        if (!s_down && (uint32_t)(now2 - s_dm_win) >= 5000u) {
            uint8_t dm[4] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu };
            s_dm_win = now2;
            if (raydium_pda2_read(RAYDIUM_PDA2_DISPLAY_MODE_ADDR, dm, 4) == 0)
                g_touch_display_mode = dm[0];
        }
        if (!s_down && intr && s_last_rpt_t &&
            (uint32_t)(now2 - s_last_rpt_t) >= 10000u &&
            (uint32_t)(now2 - s_act_win) >= 10000u) {
            s_act_win = now2;
            con_puts("touch: INT asserted but silent 10 s -- re-notify ACTIVE\n");
            raydium_notify_display_mode(RAYDIUM_DISPLAY_ACTIVE_MODE);
        }
    }

    /* The watchdog: pressed, but the chip has said nothing for TOUCH_HOLD_MS. */
    {
        uint32_t now = timer_ms();
        if (s_down && s_last_pts_t &&
            (uint32_t)(now - s_last_pts_t) > TOUCH_HOLD_MS) {
            g_touch_wdog_cnt++;
            g_touch_last_rpts = s_cur_rpts;
            s_cur_rpts = 0;
            s_down = 0;
#if defined(TOUCH_LOG)
            s_tap_wdog = 1;
            ev_push('w', 0, s_st, 0, 0);
#endif
        }
    }

#if defined(TOUCH_TRACE)
    (void)s_last_fresh; (void)s_beat;
    fb_dbg_byte(0, status[POS_SEQ]);
    fb_dbg_byte(1, status[POS_PT_AMOUNT]);
    fb_dbg_byte(2, status[3]);
    fb_dbg_byte(3, s_down ? 0xFFu : 0x00u);
#endif

#if defined(TOUCH_LOG)
    {
        uint32_t t = timer_ms();
        if (s_down && !s_tap_prev_down) {          /* finger down: start */
            s_tap_press_t = t; s_tap_rpts = 0; s_tap_pgt0 = 0; s_tap_lastp_t = 0;
            s_tap_err = 0; s_tap_same = 0; s_tap_erc = 0; s_tap_ackerr = 0;
            s_tap_wdog = 0; s_ev_n = 0; s_ev_drop = 0;
            ev_push('r', 0, s_st, s_last_x, s_last_y);   /* the press itself */
        } else if (!s_down && s_tap_prev_down) {   /* finger up: the verdict */
            bdiag_puts("TAP held=");  bdiag_putdec(t - s_tap_press_t);
            bdiag_puts(" rpts=");     bdiag_putdec(s_tap_rpts);
            bdiag_puts(" pgt0=");     bdiag_putdec(s_tap_pgt0);
            bdiag_puts(" lastp=");    bdiag_putdec(s_tap_lastp_t ? (t - s_tap_lastp_t) : 0u);
            bdiag_puts(" err=");      bdiag_putdec(s_tap_err);
            bdiag_puts(" erc=");      bdiag_putdec(s_tap_erc);
            bdiag_puts(" ackerr=");   bdiag_putdec(s_tap_ackerr);
            bdiag_puts(" same=");     bdiag_putdec(s_tap_same);
            bdiag_puts(" wdog=");     bdiag_putdec((uint32_t)s_tap_wdog);
            bdiag_puts(" evs=");      bdiag_putdec(s_ev_n);
            bdiag_puts(" ints=");     bdiag_putdec(s_int_seen);
            bdiag_puts(" ackrb=");    bdiag_puthex(s_tap_ackrb_seen ? s_tap_ackrb : 0xFFu);
            bdiag_puts(" dmode=");    bdiag_puthex(g_touch_display_mode);
            bdiag_puts(" wr=");       bdiag_putdec(g_i2c_wr_total);
            bdiag_puts(" wrcut=");    bdiag_putdec(g_i2c_wr_early);
            bdiag_puts(" wrbusy=");   bdiag_putdec(g_i2c_wr_busy);
            bdiag_puts("\n");
            ev_dump();
        }
        s_tap_prev_down = s_down;

        /* IDLE SAMPLER: with no finger down, show the chip's natural cadence
         * and its fw/gesture state. If seq advances here the chip is scanning
         * and publishing on its own; if it is frozen the chip only speaks when
         * poked, which is the whole ghost-hold story. */
        if (!s_down) {
            static uint32_t s_idle_win, s_idle_seq0, s_idle_first;
            if (!s_idle_win) { s_idle_win = t; s_idle_seq0 = s_seq; s_idle_first = 1; }
            if ((uint32_t)(t - s_idle_win) >= 5000u) {
                bdiag_puts("TIDLE t=");   bdiag_putdec(t);
                bdiag_puts(" dseq=");     bdiag_putdec((uint32_t)((uint8_t)(s_seq - (uint8_t)s_idle_seq0)));
                bdiag_puts(" seq=");      bdiag_puthex(s_st[POS_SEQ]);
                bdiag_puts(" np=");       bdiag_putdec(s_st[POS_PT_AMOUNT]);
                bdiag_puts(" ges=");      bdiag_puthex(s_st[2]);
                bdiag_puts(" fw=");       bdiag_puthex(s_st[3]);
                bdiag_puts(" int=");      bdiag_putdec((uint32_t)tlmm_in(PLAT_TOUCH_INT_GPIO));
                bdiag_puts(" ints=");     bdiag_putdec(s_int_seen);
                bdiag_puts(" imode=");    bdiag_putdec((uint32_t)(s_int_ok && !s_int_fallback));
                bdiag_puts(" dmode=");    bdiag_puthex(g_touch_display_mode);
                bdiag_puts(" wr=");       bdiag_putdec(g_i2c_wr_total);
                bdiag_puts(" wrcut=");    bdiag_putdec(g_i2c_wr_early);
                bdiag_puts("\n");
                s_idle_win = t; s_idle_seq0 = s_seq; (void)s_idle_first;
            }
        }
    }
#endif
    *x = s_last_x;
    *y = s_last_y;
    return s_down;
}
#else  /* Gen 4: the firefish 3.18 driver's report cycle, acked */
/* THE STUCK-RELEASE FIX (2026-08-28). The symptom on hardware was the Gen 6's
 * old one: a press registers instantly, then the finger stays "down" for
 * seconds after lift-off, so nothing can be swiped and most taps act as
 * long-presses.
 *
 * The cause was this function, and specifically the premise written at the top
 * of this file that the 3.18 driver has "no report ack". It does. From
 * firefish's own drivers/input/touchscreen/raydium_i2c_ts.c,
 * raydium_read_touchdata():
 *
 *     u8_seq_no = tp_status[POS_SEQ];
 *     tp_status[POS_SEQ] = 0;
 *     // inform IC to prepare next report
 *     raydium_i2c_pda2_write(client, RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR,
 *                            tp_status, 1);
 *
 * Until that byte is cleared the controller does not publish the next report,
 * so the release — which is just a report with zero points — never arrives.
 *
 * The old code had a second, compounding bug: it returned early on
 * `npts == 0` WITHOUT acking. A zero-point report is not "nothing to do", it
 * is the finger-up event, and skipping its ack is precisely how the chip ends
 * up wedged holding the last pressed report.
 *
 * Kernel quirks reproduced deliberately:
 *   - `u8_seq_no` is a LOCAL in the kernel, so its "report not updated" test
 *     always compares against 0. Sequence number 0 therefore means "nothing
 *     new"; any other value is a fresh report. Mirrored here.
 *   - a point count above MAX_TOUCH_NUM is discarded, but still acked, so one
 *     corrupt status cannot stall the stream. */
int touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t status[MAX_TCH_STATUS_PAKAGE_SIZE];
    uint8_t rpt[MAX_TOUCH_NUM * LENGTH_PT];
    uint8_t npts;

    if (!s_ready) return -1;

    if (raydium_pda2_read(RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR, status,
                          MAX_TCH_STATUS_PAKAGE_SIZE) < 0)
        return -1;

    /* Nothing new: hold the last known state rather than inventing a release,
     * exactly as the kernel leaves the input device untouched here. */
    if (status[POS_SEQ] == 0) {
        *x = s_last_x; *y = s_last_y;
        return s_down;
    }

    npts = status[POS_PT_AMOUNT];
    if (npts > MAX_TOUCH_NUM) {
        raydium_ack_report();               /* discard, but keep the stream alive */
        *x = s_last_x; *y = s_last_y;
        return s_down;
    }

    if (npts != 0 &&
        raydium_pda2_read(RAYDIUM_PDA2_TCH_RPT_ADDR, rpt, npts * LENGTH_PT) < 0)
        return -1;

    /* Ack BEFORE reporting, and on every path: this is what lets the next
     * report — including the release — be produced. */
    raydium_ack_report();

    if (npts == 0) {
        s_down = 0;
        *x = s_last_x; *y = s_last_y;       /* LVGL wants the last position */
        return 0;
    }

    s_last_x = (uint16_t)(rpt[POS_X_L] | ((uint16_t)rpt[POS_X_H] << 8));
    s_last_y = (uint16_t)(rpt[POS_Y_L] | ((uint16_t)rpt[POS_Y_H] << 8));
    s_down = 1;
    *x = s_last_x;
    *y = s_last_y;
    return 1;
}
#endif /* RAD_ACK_SEQ */

/* Host command passthrough — used for the sleep/active transitions the
 * firmware's power model will need (Phase 6). */
int touch_set_sleep(int sleep)
{
    uint8_t cmd = sleep ? RAYDIUM_HOST_CMD_PWR_SLEEP : RAYDIUM_HOST_CMD_TP_MODE;
    return raydium_pda2_write(RAYDIUM_PDA2_HOST_CMD_ADDR, &cmd, 1);
}

#endif /* PLAT_BOARD_FOSSIL_GEN4 || PLAT_BOARD_FOSSIL_GEN6 */
