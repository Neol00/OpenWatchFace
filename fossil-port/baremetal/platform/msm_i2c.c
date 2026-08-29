/* msm_i2c.c — BLSP QUP v2 I2C master (polled, FIFO mode).
 *
 * Ported from the vendor 3.18 kernel (firefish branch; see ../../HARDWARE.md):
 *   drivers/i2c/busses/i2c-msm-v2.c   (i2c_msm_qup_init, FIFO xfer path)
 *   include/linux/i2c/i2c-msm-v2.h    (register map, QUP v2 tag values)
 *
 * Scope: FIFO mode only, polled, one transfer at a time. The kernel driver
 * also implements BAM/DMA mode and block mode for large transfers; the touch
 * controller's reports are tens of bytes, well inside the 16-entry FIFO, so
 * FIFO mode is all this port needs. Transfers larger than the FIFO are
 * rejected rather than silently truncated.
 *
 * Deliberately omitted (Phase 6 / not needed here):
 *   - clock enabling: BLSP QUP apps/iface clocks come from GCC. aboot leaves
 *     BLSP1 clocked (it uses UART1 on the same BLSP for its console), so the
 *     QUP core is reachable. If a QUP read returns all-ones on hardware, an
 *     unclocked block is the first thing to suspect.
 *   - runtime PM, bus recovery (9-clock SCL toggle), interrupt mode.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#include <string.h>

/* --- QUP registers (i2c-msm-v2.h) ---------------------------------------- */
#define QUP_CONFIG              0x000
#define QUP_STATE               0x004
#define QUP_IO_MODES            0x008
#define QUP_SW_RESET            0x00C
#define QUP_OPERATIONAL         0x018
#define QUP_ERROR_FLAGS         0x01C
#define QUP_ERROR_FLAGS_EN      0x020
#define QUP_OPERATIONAL_MASK    0x028
#define QUP_HW_VERSION          0x030
#define QUP_MX_OUTPUT_COUNT     0x100
#define QUP_OUT_FIFO_BASE       0x110
#define QUP_MX_WRITE_COUNT      0x150
#define QUP_MX_INPUT_COUNT      0x200
#define QUP_MX_READ_COUNT       0x208
#define QUP_IN_FIFO_BASE        0x218
#define QUP_I2C_MASTER_CLK_CTL  0x400
#define QUP_I2C_STATUS          0x404
#define QUP_I2C_MASTER_CONFIG   0x408

/* QUP_STATE */
#define QUP_STATE_RESET         0u
#define QUP_STATE_RUN           1u
#define QUP_STATE_PAUSE         3u
#define QUP_STATE_MASK          3u
#define QUP_STATE_VALID         (1u << 2)
#define QUP_I2C_FLUSH           (1u << 6)

/* QUP_CONFIG */
#define QUP_N_VAL               0x7
#define QUP_MINI_CORE_I2C_VAL   0x200
#define QUP_NO_OUTPUT           (1u << 6)
#define QUP_NO_INPUT            (1u << 7)

/* QUP_OPERATIONAL */
#define QUP_INPUT_FIFO_NOT_EMPTY (1u << 5)
#define QUP_OUTPUT_SERVICE_FLAG  (1u << 8)
#define QUP_INPUT_SERVICE_FLAG   (1u << 9)
#define QUP_MAX_OUTPUT_DONE_FLAG (1u << 10)
#define QUP_MAX_INPUT_DONE_FLAG  (1u << 11)

/* QUP_OPERATIONAL_MASK */
#define QUP_INPUT_SERVICE_MASK  (1u << 9)
#define QUP_OUTPUT_SERVICE_MASK (1u << 8)

/* QUP_ERROR_FLAGS_EN */
#define QUP_OUTPUT_OVER_RUN_ERR_EN  (1u << 5)
#define QUP_INPUT_UNDER_RUN_ERR_EN  (1u << 4)
#define QUP_OUTPUT_UNDER_RUN_ERR_EN (1u << 3)
#define QUP_INPUT_OVER_RUN_ERR_EN   (1u << 2)

/* QUP_I2C_STATUS error bits */
#define QUP_PACKET_NACKED       (1u << 3)
#define QUP_MSTR_STTS_ERR_MASK  0x380003Cu
/* i2c-msm-v2.h: I2C_STATUS_BUS_ACTIVE — set while the mini-core still owns
 * the wire. Only when it clears has the STOP condition actually gone out. */
#define QUP_I2C_BUS_ACTIVE      (1u << 8)

/* QUP_I2C_MASTER_CONFIG */
#define QUP_EN_VERSION_TWO_TAG  1u

/* QUP v2 tags */
#define QUP_TAG2_START             0x81u
#define QUP_TAG2_DATA_WRITE        0x82u
#define QUP_TAG2_DATA_WRITE_N_STOP 0x83u
#define QUP_TAG2_DATA_READ         0x85u
#define QUP_TAG2_DATA_READ_N_STOP  0x87u
#define QUP_TAG2_START_STOP        0x8Au
#define QUP_TAG2_INPUT_EOT         0x93u

/* All entry points take the QUP block's base address, so one driver serves
 * every BLSP QUP on the SoC (Gen 4 touch bus, Gen 6 sensor buses...). */
#define I2C_W(off, v)  mmio_write(base + (off), (v))
#define I2C_R(off)     mmio_read(base + (off))

static inline void i2c_wmb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* See the WRITE DRAIN note in i2c_bus_xfer(). */
uint32_t g_i2c_wr_total, g_i2c_wr_early, g_i2c_wr_busy;

/* QUP v2 FIFO is 16 entries x 4 bytes. Keep transfers well inside it. */
#define QUP_FIFO_BYTES 64

static int qup_state_set(uintptr_t base, uint32_t state)
{
    uint32_t t0 = timer_ms();
    /* Wait for STATE_VALID, then write the requested state. */
    while (!(I2C_R(QUP_STATE) & QUP_STATE_VALID)) {
        if ((uint32_t)(timer_ms() - t0) > 100) return -1;
    }
    I2C_W(QUP_STATE, state);
    i2c_wmb();
    t0 = timer_ms();
    while ((I2C_R(QUP_STATE) & QUP_STATE_MASK) != state) {
        if ((uint32_t)(timer_ms() - t0) > 100) return -1;
    }
    return 0;
}

/* Port of i2c_msm_qup_init(). */
int i2c_bus_init(uintptr_t base)
{
    uint32_t fs_div, hs_div, clk_ctl;

#if defined(PLAT_I2C_DISABLED)
    /* HARDWARE ACCESS DISABLED (2026-08-03) — the third instance of the same
     * trap that took out uart_init() and the MDP probe: touching a CLOCK-GATED
     * MSM block wedges the bus. setup() calls Wire.begin() long before anything
     * owns the GCC clocks, and the boot hangs exactly there.
     *
     * Returning an error is the CORRECT behaviour, not a fudge: Wire.begin()
     * reports failure, board_power_begin() on this board reads the fuel gauge
     * over SPMI (which genuinely works) and never needs I2C, and touch_init()
     * degrades to display-only. The firmware boots.
     *
     * Undo this only together with a GCC driver that enables the BLSP QUP
     * apps/iface clocks first. See boards/fossil_gen6.h. */
    (void)base; (void)fs_div; (void)hs_div; (void)clk_ctl;
    bdiag_puts("i2c: DISABLED (BLSP clocks not owned yet)\n");
    return -1;
#else

#if defined(PLAT_SOC_MSM)
    /* Clock-gate fence (2026-08-03): only the ONE touch QUP has its GCC
     * clocks owned (gcc_blsp.c). Any other QUP is still gated, and one
     * register read of a gated QUP is an instant hard reset on the recovery
     * path (measured: TOUCHTEST died ~2 s) — refuse other bases outright so
     * the touch probe's multi-bus fallback can never wander into one.
     * The msm8909w watches need this fence just as much: Wire.begin() runs
     * before anything owns a clock, and on both of them the touch bus is QUP5
     * with another device sharing it (the Gen 4's PixArt crown), so there is
     * no second bus worth trying anyway. Expressing the rule as "the touch
     * bus, whatever the board says it is" makes it board-agnostic. */
#if defined(PLAT_SOC_MSM8909)
    if (base != PLAT_I2C_TOUCH_BASE) {
#else
    if (base != PLAT_I2C_BLSP1_QUP4) {
#endif
        con_puts("i2c: bus refused (no GCC clocks owned for it)\n");
        return -1;
    }
    if (gcc_blsp_qup4_up() < 0) return -1;
#endif

    I2C_W(QUP_SW_RESET, 1);
    i2c_wmb();
    timer_delay_ms(1);

    if (qup_state_set(base, QUP_STATE_RESET) < 0) {
        con_puts("i2c: stuck out of reset\n");
        return -1;
    }

    I2C_W(QUP_CONFIG, QUP_N_VAL | QUP_MINI_CORE_I2C_VAL);
    I2C_W(QUP_ERROR_FLAGS_EN, QUP_OUTPUT_OVER_RUN_ERR_EN |
                              QUP_INPUT_UNDER_RUN_ERR_EN |
                              QUP_OUTPUT_UNDER_RUN_ERR_EN |
                              QUP_INPUT_OVER_RUN_ERR_EN);
    /* FIFO mode: kernel leaves the service flags UNMASKED (mask = 0; masking
     * is only for its DMA path). The flags still latch in QUP_OPERATIONAL
     * either way; match the kernel exactly. */
    I2C_W(QUP_OPERATIONAL_MASK, 0);
    I2C_W(QUP_I2C_MASTER_CONFIG, QUP_EN_VERSION_TWO_TAG);

    /* Clock divider, i2c-msm-v2 rev 2 (2026-08-04): rev 1 was DOUBLY wrong —
     * the high-time divider lives at bits [23:16] of CLK_CTL (we had put a
     * bogus "hs_div" at [15:8] and left the real field 0 = malformed SCL),
     * and the kernel does NOT compute fs_div: it uses a hand-tuned table
     * "as per HW Designers" (i2c_msm_clk_div_map, 19.2 MHz core):
     *   100 kHz -> fs 124, ht 62;  400 kHz -> fs 28, ht 14. */
    fs_div = (PLAT_I2C_BUS_HZ >= 400000u) ? 28u : 124u;
    hs_div = (PLAT_I2C_BUS_HZ >= 400000u) ? 14u : 62u;   /* high-time div */
    clk_ctl = ((hs_div & 0xffu) << 16) | (fs_div & 0xffu);
    I2C_W(QUP_I2C_MASTER_CLK_CTL, clk_ctl);
    i2c_wmb();

    /* FIFO mode: OUTPUT_MODE/INPUT_MODE = 0, with pack/unpack enabled so the
     * FIFO words carry 4 bytes each. */
    I2C_W(QUP_IO_MODES, (1u << 14) | (1u << 15));   /* UNPACK_EN | PACK_EN */
    i2c_wmb();

    bdiag_puts("i2c: QUP hw_version="); bdiag_puthex(I2C_R(QUP_HW_VERSION));
    bdiag_puts(" clk_ctl="); bdiag_puthex(clk_ctl); bdiag_puts("\n");
    return 0;
#endif /* PLAT_I2C_DISABLED */
}

static int qup_check_error(uintptr_t base)
{
    uint32_t st = I2C_R(QUP_I2C_STATUS);
    if (st & QUP_MSTR_STTS_ERR_MASK) {
        /* Clear by writing back; NACK is the common, expected-ish case. */
        I2C_W(QUP_I2C_STATUS, 0);
        return (st & QUP_PACKET_NACKED) ? -2 : -1;
    }
    return 0;
}

/* Push one 4-byte word into the output FIFO. */
static void qup_out_word(uintptr_t base, uint32_t w) { I2C_W(QUP_OUT_FIFO_BASE, w); }

/* Write `len` bytes to `addr`, then optionally repeated-START and read
 * `rlen` bytes. Either half may be zero. Returns 0, -1 error, -2 NACK. */
int i2c_bus_xfer(uintptr_t base, uint8_t addr, const uint8_t *wbuf, uint32_t len,
                 uint8_t *rbuf, uint32_t rlen)
{
    uint8_t out[QUP_FIFO_BYTES];
    uint32_t n = 0, i, t0;

#if defined(PLAT_I2C_DISABLED)
    /* See i2c_bus_init(): no QUP register may be touched until the BLSP clocks
     * are owned. Without this the touch bus probe would hang the boot too. */
    (void)base; (void)addr; (void)wbuf; (void)len; (void)rbuf; (void)rlen;
    (void)out; (void)n; (void)i; (void)t0;
    return -1;
#endif

    if (len > QUP_FIFO_BYTES - 8 || rlen > QUP_FIFO_BYTES - 8) return -1;

    /* FIFO-mode byte accounting (i2c_msm_qup_xfer_init_reset_state): the MX
     * counts must hold the TRUE tag+data byte totals — rev 1 wrote 0 to all
     * four, which starves the QUP of its transfer size (never validated on
     * hardware until the Gen 6 touch campaign; likely THE mute-chip cause).
     * Output: 4 tag bytes (START,addr,DATA*,len) per phase + write payload.
     * Input:  2 tag bytes precede the read data in the input stream. */
    {
        uint32_t tx_cnt = (len ? 4u + len : 0u) + (rlen ? 4u : 0u);
        uint32_t rx_cnt = rlen ? rlen + 2u : 0u;

        if (qup_state_set(base, QUP_STATE_RESET) < 0) return -1;

        I2C_W(QUP_MX_OUTPUT_COUNT, 0);          /* block/DMA modes only */
        I2C_W(QUP_MX_INPUT_COUNT, 0);
        I2C_W(QUP_MX_WRITE_COUNT, tx_cnt);      /* FIFO-mode true counts */
        I2C_W(QUP_MX_READ_COUNT, rx_cnt);
        I2C_W(QUP_CONFIG, QUP_N_VAL | QUP_MINI_CORE_I2C_VAL |
                          (rx_cnt ? 0u : QUP_NO_INPUT) |
                          (tx_cnt ? 0u : QUP_NO_OUTPUT));
        I2C_W(QUP_I2C_STATUS, 0);               /* clear stale status */
        i2c_wmb();
    }

    if (qup_state_set(base, QUP_STATE_RUN) < 0) return -1;
    /* kernel: CLK_CTL is (re)written in the run state, then FIFO loading
     * happens in PAUSE to avoid racing the wire */
    if (qup_state_set(base, QUP_STATE_PAUSE) < 0) return -1;

    /* --- build the tag stream --------------------------------------------
     * Write phase: [START | addr<<1|0] then [DATA_WRITE(_N_STOP) | len] data.
     * Read phase:  [START | addr<<1|1] then [DATA_READ_N_STOP | rlen].
     * QUP v2 tags are (tag, value) byte pairs in the output FIFO. */
    if (len) {
        out[n++] = QUP_TAG2_START;
        out[n++] = (uint8_t)(addr << 1);              /* write */
        out[n++] = rlen ? QUP_TAG2_DATA_WRITE : QUP_TAG2_DATA_WRITE_N_STOP;
        out[n++] = (uint8_t)len;
        for (i = 0; i < len; i++) out[n++] = wbuf[i];
    }
    if (rlen) {
        out[n++] = QUP_TAG2_START;
        out[n++] = (uint8_t)((addr << 1) | 1u);       /* read */
        out[n++] = QUP_TAG2_DATA_READ_N_STOP;
        out[n++] = (uint8_t)rlen;
    }
    while (n & 3) out[n++] = 0;                       /* pad to a whole word */

    for (i = 0; i < n; i += 4) {
        qup_out_word(base, (uint32_t)out[i] | ((uint32_t)out[i+1] << 8) |
                     ((uint32_t)out[i+2] << 16) | ((uint32_t)out[i+3] << 24));
    }
    i2c_wmb();

    /* FIFO loaded while PAUSED (kernel: "load fifo while in pause state to
     * avoid race conditions") — RUN starts the wire transfer. */
    if (qup_state_set(base, QUP_STATE_RUN) < 0) return -1;

    /* Completion (kernel ISR semantics): a transfer ending in a read is done
     * on MAX_INPUT_DONE; a pure write is done on OUTPUT_SERVICE (the kernel
     * notes MAX_OUTPUT_DONE lags and uses the service flag instead). */
    {
        uint32_t done_flag = rlen ? QUP_MAX_INPUT_DONE_FLAG
                                  : QUP_OUTPUT_SERVICE_FLAG;
        t0 = timer_ms();
        while (!(I2C_R(QUP_OPERATIONAL) & done_flag)) {
            if ((uint32_t)(timer_ms() - t0) > 100) { qup_state_set(base, QUP_STATE_RESET); return -1; }
            if (qup_check_error(base) < 0) { qup_state_set(base, QUP_STATE_RESET); return -2; }
        }
    }
    if (qup_check_error(base) < 0) { qup_state_set(base, QUP_STATE_RESET); return -2; }

    /* --- WRITE DRAIN (2026-08-06, the silent-write bug) --------------------
     * OUTPUT_SERVICE_FLAG means "the mini-core has emptied the output FIFO",
     * i.e. the bytes left the FIFO — NOT that they were shifted out on SCL and
     * the STOP condition was issued. For a pure write that flag was our only
     * completion test, and the very next statement drops the QUP into
     * QUP_STATE_RESET, which ABORTS whatever is still on the wire. Reads were
     * never affected (MAX_INPUT_DONE genuinely means the data came back), so
     * every read in this port worked while every pure write — PDA2 page
     * select, HOST_CMD, and the Raydium seq ACK — was truncated at random.
     * A chip whose report ACK never lands stops queueing reports, which is
     * exactly the "one report per press, then silence" touch behaviour.
     *
     * Wait for MAX_OUTPUT_DONE and then for the bus to go idle before the
     * reset. Both waits are advisory: if a flag never appears we carry on
     * rather than fail a transfer that previously "succeeded". */
    if (!rlen) {
        /* Proof instrumentation: how many pure writes would have been reset
         * mid-flight by the old code (out-done not yet latched, or the wire
         * still busy) — if these are non-zero the truncation was real. */
        g_i2c_wr_total++;
        if (!(I2C_R(QUP_OPERATIONAL) & QUP_MAX_OUTPUT_DONE_FLAG)) g_i2c_wr_early++;
        if (I2C_R(QUP_I2C_STATUS) & QUP_I2C_BUS_ACTIVE)           g_i2c_wr_busy++;
        t0 = timer_ms();
        while (!(I2C_R(QUP_OPERATIONAL) & QUP_MAX_OUTPUT_DONE_FLAG)) {
            if ((uint32_t)(timer_ms() - t0) > 10) break;
            if (qup_check_error(base) < 0) { qup_state_set(base, QUP_STATE_RESET); return -2; }
        }
        t0 = timer_ms();
        while (I2C_R(QUP_I2C_STATUS) & QUP_I2C_BUS_ACTIVE) {
            if ((uint32_t)(timer_ms() - t0) > 10) break;
        }
    }

    /* --- drain the input FIFO ---------------------------------------------
     * The input stream begins with 2 tag bytes (QUP_BUF_OVERHD_BC) BEFORE
     * the read data — the kernel strips them (i2c_msm_fifo_read_xfer_buf);
     * rev 1 returned them as the first two "data" bytes. */
    if (rlen) {
        uint32_t got = 0, skip = 2, total = rlen + 2;
        t0 = timer_ms();
        while (got < total) {
            if (!(I2C_R(QUP_OPERATIONAL) & QUP_INPUT_FIFO_NOT_EMPTY)) {
                if ((uint32_t)(timer_ms() - t0) > 100) {
                    qup_state_set(base, QUP_STATE_RESET);
                    return -1;
                }
                if (qup_check_error(base) < 0) { qup_state_set(base, QUP_STATE_RESET); return -2; }
                continue;
            }
            uint32_t w = I2C_R(QUP_IN_FIFO_BASE);
            for (i = 0; i < 4 && got < total; i++, got++) {
                uint8_t byte = (uint8_t)((w >> (i * 8)) & 0xff);
                if (got >= skip) rbuf[got - skip] = byte;
            }
        }
    }

    /* Ack every latched completion flag (W1C) so the next transfer starts
     * from clean OPERATIONAL state, then park the mini-core. */
    I2C_W(QUP_OPERATIONAL, QUP_OUTPUT_SERVICE_FLAG | QUP_INPUT_SERVICE_FLAG |
                           QUP_MAX_OUTPUT_DONE_FLAG | QUP_MAX_INPUT_DONE_FLAG);
    qup_state_set(base, QUP_STATE_RESET);
    return 0;
}

/* ---- legacy single-bus API: the board's designated touch bus -------------
 * Kept so touch_raydium.c (Gen 4) is untouched; boards without a known touch
 * bus simply do not define PLAT_I2C_TOUCH_BASE and get only the *_bus_* API. */
#if defined(PLAT_I2C_TOUCH_BASE)
int i2c_init(void)
{
    return i2c_bus_init(PLAT_I2C_TOUCH_BASE);
}

int i2c_write(uint8_t addr, const uint8_t *buf, uint32_t len)
{
    return i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, addr, buf, len, 0, 0);
}

int i2c_read(uint8_t addr, uint8_t *buf, uint32_t len)
{
    return i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, addr, 0, 0, buf, len);
}

/* Register read: write the register index, repeated-START, then read. */
int i2c_write_read(uint8_t addr, const uint8_t *wbuf, uint32_t wlen,
                   uint8_t *rbuf, uint32_t rlen)
{
    return i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, addr, wbuf, wlen, rbuf, rlen);
}

/* Probe: zero-length-ish write to see whether anything ACKs the address. */
int i2c_probe(uint8_t addr)
{
    uint8_t b = 0;
    return i2c_bus_xfer(PLAT_I2C_TOUCH_BASE, addr, &b, 1, 0, 0);
}
#endif /* PLAT_I2C_TOUCH_BASE */

#endif /* PLAT_SOC_MSM */
