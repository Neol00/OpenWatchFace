/* touch_raydium.c — Raydium RM_TS touch controller over BLSP QUP I2C.
 *
 * Ported from the vendor 3.18 kernel (firefish branch; see ../../HARDWARE.md):
 *   drivers/input/touchscreen/raydium_i2c_ts.c  (PDA2 access, report parsing)
 *   drivers/input/touchscreen/raydium_i2c_ts.h  (addresses, page constants)
 * The defconfig selects CONFIG_TOUCHSCREEN_RM_TS for this watch (it7260 and
 * synaptics are disabled), so this is the right controller.
 *
 * Access model (PDA2): the controller exposes a paged register window. Set
 * the page via the PAGE register, then read/write the small page-0 registers:
 *   0x00 TCH_RPT_STATUS  — [0]=seq, [1]=touch count
 *   0x01 TCH_RPT         — touch report, 11 bytes per point
 *   0x02 HOST_CMD        — host command (sleep, mode, calibration)
 *
 * Scope: report reading only. The kernel driver is ~148 KB, but the bulk of it
 * is firmware update, factory test, sysfs and gesture paths that a watch
 * firmware does not need. Not ported: FW update over I2C, calibration flows,
 * PDA (v1) 4-byte-address mode, gesture wake.
 *
 * Interrupt: the controller has an INT line (falling edge on new data). This
 * port POLLS from the LVGL input read callback instead, which is simpler and
 * matches LVGL's pull model. Wiring the TLMM GPIO IRQ is a later refinement.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN4)

#include <string.h>

int i2c_init(void);
int i2c_write(uint8_t addr, const uint8_t *buf, uint32_t len);
int i2c_write_read(uint8_t addr, const uint8_t *wbuf, uint32_t wlen,
                   uint8_t *rbuf, uint32_t rlen);

/* raydium_i2c_ts.h */
#define RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR  0x00
#define RAYDIUM_PDA2_TCH_RPT_ADDR         0x01
#define RAYDIUM_PDA2_HOST_CMD_ADDR        0x02
#define RAYDIUM_PDA2_PAGE_ADDR            0x0B
#define RAYDIUM_PDA2_PAGE_0               0x00

#define RAYDIUM_HOST_CMD_PWR_SLEEP        0x30
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

#define MAX_TOUCH_NUM    2

static int s_ready;

/* PDA2 page select — must precede page-0 register access. */
static int raydium_set_page(uint8_t page)
{
    uint8_t buf[2] = { RAYDIUM_PDA2_PAGE_ADDR, page };
    return i2c_write(PLAT_TOUCH_I2C_ADDR, buf, 2);
}

static int raydium_pda2_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
    if (raydium_set_page(RAYDIUM_PDA2_PAGE_0) < 0) return -1;
    return i2c_write_read(PLAT_TOUCH_I2C_ADDR, &reg, 1, buf, len);
}

static int raydium_pda2_write(uint8_t reg, const uint8_t *data, uint32_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof buf) return -1;
    if (raydium_set_page(RAYDIUM_PDA2_PAGE_0) < 0) return -1;
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_write(PLAT_TOUCH_I2C_ADDR, buf, len + 1);
}

int touch_init(void)
{
    uint8_t status[4];

    if (i2c_init() < 0) return -1;

    /* A status read is the cheapest liveness probe: if the controller is
     * present and clocked it ACKs and returns a sequence/count pair. */
    if (raydium_pda2_read(RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR, status, 2) < 0) {
        con_puts("touch: raydium not responding at 0x");
        con_puthex(PLAT_TOUCH_I2C_ADDR); con_puts("\n");
        return -1;
    }

    con_puts("touch: raydium up, seq="); con_puthex(status[POS_SEQ]);
    con_puts(" pts="); con_putdec(status[POS_PT_AMOUNT]); con_puts("\n");
    s_ready = 1;
    return 0;
}

/* Read the current touch state. Returns 1 if a finger is down (and fills the
 * x and y out-params), 0 if not, negative on I2C error. */
int touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t status[4];
    uint8_t rpt[MAX_TOUCH_NUM * LENGTH_PT];
    uint8_t npts;

    if (!s_ready) return -1;

    if (raydium_pda2_read(RAYDIUM_PDA2_TCH_RPT_STATUS_ADDR, status, 2) < 0)
        return -1;

    npts = status[POS_PT_AMOUNT];
    if (npts == 0 || npts > MAX_TOUCH_NUM) return 0;

    if (raydium_pda2_read(RAYDIUM_PDA2_TCH_RPT_ADDR, rpt, npts * LENGTH_PT) < 0)
        return -1;

    /* Report only the first contact: the watch UI is single-touch. */
    *x = (uint16_t)(rpt[POS_X_L] | ((uint16_t)rpt[POS_X_H] << 8));
    *y = (uint16_t)(rpt[POS_Y_L] | ((uint16_t)rpt[POS_Y_H] << 8));
    return 1;
}

/* Host command passthrough — used for the sleep/active transitions the
 * firmware's power model will need (Phase 6). */
int touch_set_sleep(int sleep)
{
    uint8_t cmd = sleep ? RAYDIUM_HOST_CMD_PWR_SLEEP : RAYDIUM_HOST_CMD_TP_MODE;
    return raydium_pda2_write(RAYDIUM_PDA2_HOST_CMD_ADDR, &cmd, 1);
}

#endif /* PLAT_BOARD_FOSSIL_GEN4 */
