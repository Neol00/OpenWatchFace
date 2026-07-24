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
#if defined(PLAT_BOARD_FOSSIL_GEN4)

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

#define I2C_W(off, v)  mmio_write(PLAT_I2C_TOUCH_BASE + (off), (v))
#define I2C_R(off)     mmio_read(PLAT_I2C_TOUCH_BASE + (off))

static inline void i2c_wmb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* QUP v2 FIFO is 16 entries x 4 bytes. Keep transfers well inside it. */
#define QUP_FIFO_BYTES 64

static int qup_state_set(uint32_t state)
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
int i2c_init(void)
{
    uint32_t fs_div, hs_div, clk_ctl;

    I2C_W(QUP_SW_RESET, 1);
    i2c_wmb();
    timer_delay_ms(1);

    if (qup_state_set(QUP_STATE_RESET) < 0) {
        con_puts("i2c: stuck out of reset\n");
        return -1;
    }

    I2C_W(QUP_CONFIG, QUP_N_VAL | QUP_MINI_CORE_I2C_VAL);
    I2C_W(QUP_ERROR_FLAGS_EN, QUP_OUTPUT_OVER_RUN_ERR_EN |
                              QUP_INPUT_UNDER_RUN_ERR_EN |
                              QUP_OUTPUT_UNDER_RUN_ERR_EN |
                              QUP_INPUT_OVER_RUN_ERR_EN);
    /* Mask the service flags: we poll QUP_OPERATIONAL, no interrupts. */
    I2C_W(QUP_OPERATIONAL_MASK, QUP_INPUT_SERVICE_MASK | QUP_OUTPUT_SERVICE_MASK);
    I2C_W(QUP_I2C_MASTER_CONFIG, QUP_EN_VERSION_TWO_TAG);

    /* Clock divider: the kernel computes fs_div = core_clk/(bus_clk*2) - 3.
     * hs_div is the high-speed divider, unused in fast mode (kernel uses 3). */
    fs_div = (PLAT_I2C_CORE_HZ / (PLAT_I2C_BUS_HZ * 2u)) - 3u;
    hs_div = 3u;
    clk_ctl = ((hs_div & 0xff) << 8) | (fs_div & 0xff);
    I2C_W(QUP_I2C_MASTER_CLK_CTL, clk_ctl);
    i2c_wmb();

    /* FIFO mode: OUTPUT_MODE/INPUT_MODE = 0, with pack/unpack enabled so the
     * FIFO words carry 4 bytes each. */
    I2C_W(QUP_IO_MODES, (1u << 14) | (1u << 15));   /* UNPACK_EN | PACK_EN */
    i2c_wmb();

    con_puts("i2c: QUP hw_version="); con_puthex(I2C_R(QUP_HW_VERSION));
    con_puts(" clk_ctl="); con_puthex(clk_ctl); con_puts("\n");
    return 0;
}

static int qup_check_error(void)
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
static void qup_out_word(uint32_t w) { I2C_W(QUP_OUT_FIFO_BASE, w); }

/* Write `len` bytes to `addr`, then optionally repeated-START and read
 * `rlen` bytes. Either half may be zero. Returns 0, -1 error, -2 NACK. */
static int i2c_xfer(uint8_t addr, const uint8_t *wbuf, uint32_t len,
                    uint8_t *rbuf, uint32_t rlen)
{
    uint8_t out[QUP_FIFO_BYTES];
    uint32_t n = 0, i, t0;

    if (len > QUP_FIFO_BYTES - 8 || rlen > QUP_FIFO_BYTES) return -1;

    if (qup_state_set(QUP_STATE_RESET) < 0) return -1;

    /* Program expected counts. In FIFO mode these are the tag+data byte
     * counts the QUP will move. */
    I2C_W(QUP_MX_WRITE_COUNT, 0);
    I2C_W(QUP_MX_READ_COUNT, 0);
    I2C_W(QUP_MX_OUTPUT_COUNT, 0);
    I2C_W(QUP_MX_INPUT_COUNT, 0);
    I2C_W(QUP_I2C_STATUS, 0);          /* clear stale status */
    i2c_wmb();

    if (qup_state_set(QUP_STATE_RUN) < 0) return -1;

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
        qup_out_word((uint32_t)out[i] | ((uint32_t)out[i+1] << 8) |
                     ((uint32_t)out[i+2] << 16) | ((uint32_t)out[i+3] << 24));
    }
    i2c_wmb();

    /* Wait for the output FIFO to drain (transfer executed on the wire). */
    t0 = timer_ms();
    while (!(I2C_R(QUP_OPERATIONAL) & QUP_OUTPUT_SERVICE_FLAG)) {
        if ((uint32_t)(timer_ms() - t0) > 50) { qup_state_set(QUP_STATE_RESET); return -1; }
        if (qup_check_error() < 0) { qup_state_set(QUP_STATE_RESET); return -2; }
    }
    I2C_W(QUP_OPERATIONAL, QUP_OUTPUT_SERVICE_FLAG);  /* ack */

    if (qup_check_error() < 0) { qup_state_set(QUP_STATE_RESET); return -2; }

    /* --- drain the input FIFO -------------------------------------------- */
    if (rlen) {
        uint32_t got = 0;
        t0 = timer_ms();
        while (got < rlen) {
            if (!(I2C_R(QUP_OPERATIONAL) & QUP_INPUT_FIFO_NOT_EMPTY)) {
                if ((uint32_t)(timer_ms() - t0) > 50) {
                    qup_state_set(QUP_STATE_RESET);
                    return -1;
                }
                if (qup_check_error() < 0) { qup_state_set(QUP_STATE_RESET); return -2; }
                continue;
            }
            uint32_t w = I2C_R(QUP_IN_FIFO_BASE);
            for (i = 0; i < 4 && got < rlen; i++)
                rbuf[got++] = (uint8_t)((w >> (i * 8)) & 0xff);
        }
        I2C_W(QUP_OPERATIONAL, QUP_INPUT_SERVICE_FLAG);
    }

    qup_state_set(QUP_STATE_RESET);
    return 0;
}

int i2c_write(uint8_t addr, const uint8_t *buf, uint32_t len)
{
    return i2c_xfer(addr, buf, len, 0, 0);
}

int i2c_read(uint8_t addr, uint8_t *buf, uint32_t len)
{
    return i2c_xfer(addr, 0, 0, buf, len);
}

/* Register read: write the register index, repeated-START, then read. */
int i2c_write_read(uint8_t addr, const uint8_t *wbuf, uint32_t wlen,
                   uint8_t *rbuf, uint32_t rlen)
{
    return i2c_xfer(addr, wbuf, wlen, rbuf, rlen);
}

/* Probe: zero-length-ish write to see whether anything ACKs the address. */
int i2c_probe(uint8_t addr)
{
    uint8_t b = 0;
    return i2c_xfer(addr, &b, 1, 0, 0);
}

#endif /* PLAT_BOARD_FOSSIL_GEN4 */
