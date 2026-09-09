/* gcc_blsp.c — BLSP1 touch-I2C QUP clock bring-up (both watches).
 *
 * Same disease, same cure as gcc_mdss.c: aboot gates the BLSP clocks at
 * handoff, and the first register access to the unclocked QUP is an instant
 * hard reset on the recovery boot path (measured 2026-08-03: the TOUCHTEST
 * image with I2C enabled but no clocks died ~2 s into boot, before the dial).
 * Enable the block's clocks in the always-on GCC FIRST, then the QUP is an
 * ordinary peripheral.
 *
 * Register recipe, kernel-verbatim from the hoki tree
 * (drivers/clk/qcom/gcc-sdm429w.c, lineage-22.1):
 *   gcc_blsp1_ahb_clk          VOTED branch: APCS_CLOCK_BRANCH_ENA_VOTE
 *                              0x45004 bit 10; halt status CBCR 0x1008.
 *   blsp1_qup4_i2c_apps_clk_src RCG2 @ cmd_rcgr 0x5000, hid_width 5, no MND;
 *                              19.2 MHz row = { src P_BI_TCXO (sel 0), div 1 }
 *                              -> CFG = 0 (matches the DT's qcom,clk-freq-in
 *                              of 19200000 for i2c@78b8000).
 *   gcc_blsp1_qup4_i2c_apps_clk branch CBCR 0x5020 bit 0, CLK_OFF bit 31.
 *
 * The i2c-msm-v2 hardware also has a BAM DMA pipe (blsp1_bam) — our polled
 * i2c_msm.c driver never touches it, so its clock stays off.
 */
/* The Gen 4 (msm8909w) needs the identical sequence against a DIFFERENT QUP:
 * its touch controller hangs off i2c@78b9000, which its own device tree names
 * i2c5 — BLSP1 QUP5, not QUP4. Every register value below is confirmed in the
 * vendor clock-gcc-8909.c: APCS_CLOCK_BRANCH_ENA_VOTE 0x45004 with
 * gcc_blsp1_ahb_clk's en_mask BIT(10), BLSP1_AHB_CBCR 0x01008,
 * BLSP1_QUP5_I2C_APPS_CMD_RCGR 0x06000 and BLSP1_QUP5_I2C_APPS_CBCR 0x06020
 * (QUP4's are 0x05000/0x05020 — one 0x1000 block lower, which is exactly the
 * kind of off-by-one-peripheral mistake that shows up as "touch is mute"). */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#define GCC_APCS_BRANCH_ENA_VOTE  0x45004u
#define   BLSP1_AHB_VOTE_BIT      (1u << 10)
#define GCC_BLSP1_AHB_CBCR        0x01008u   /* halt status only (voted clk) */
/* Which QUP carries touch is a BOARD fact, but both msm8909w watches here put
 * it on QUP5: the Gen 4's raydium@39 and the C2's focaltech@38 both hang off
 * i2c@78b9000. The Gen 6 uses QUP4. */
#if defined(PLAT_SOC_MSM8909)
#define GCC_QUP4_I2C_CMD_RCGR     0x06000u   /* BLSP1 QUP5: +0 CMD, +4 CFG */
#define GCC_QUP4_I2C_CBCR         0x06020u
#define GCC_QUP_NAME              "QUP5"
#else
#define GCC_QUP4_I2C_CMD_RCGR     0x05000u   /* BLSP1 QUP4: +0 CMD, +4 CFG */
#define GCC_QUP4_I2C_CBCR         0x05020u
#define GCC_QUP_NAME              "QUP4"
#endif

#define CBCR_CLK_ENABLE   (1u << 0)
#define CBCR_CLK_OFF      (1u << 31)
#define RCG_UPDATE        (1u << 0)
#define RCG_ROOT_EN       (1u << 1)
#define RCG_CFG_XO_DIV1   0u               /* src_sel 0 (TCXO) << 8 | div-1 0 */

#define GCC_R(off)    mmio_read(PLAT_GCC_BASE + (off))
#define GCC_W(off, v) mmio_write(PLAT_GCC_BASE + (off), (v))

/* Any BLSP1 QUP I2C clock pair, by GCC offsets (msm8909/8916 layout:
 * QUP1 CMD 0x0200C/CBCR 0x02008, QUP2 0x03000/0x03010, QUP3 0x04000/0x04020,
 * QUP4 0x05000/0x05020, QUP5 0x06000/0x06020, QUP6 0x07000/0x07020). Added
 * 2026-09-03 for the modem-owned sensor buses (QUP1/QUP2). */
int gcc_blsp_qup_i2c_up(uint32_t cmd_rcgr, uint32_t cbcr, const char *name)
{
    uint32_t t0;
    GCC_W(GCC_APCS_BRANCH_ENA_VOTE, GCC_R(GCC_APCS_BRANCH_ENA_VOTE) | BLSP1_AHB_VOTE_BIT);
    t0 = timer_ms();
    while (GCC_R(GCC_BLSP1_AHB_CBCR) & CBCR_CLK_OFF)
        if ((uint32_t)(timer_ms() - t0) > 10u) { con_puts("gcc-blsp: AHB clk stuck\n"); return -1; }
    GCC_W(cmd_rcgr + 4u, RCG_CFG_XO_DIV1);
    GCC_W(cmd_rcgr, GCC_R(cmd_rcgr) | RCG_ROOT_EN | RCG_UPDATE);
    t0 = timer_ms();
    while (GCC_R(cmd_rcgr) & RCG_UPDATE)
        if ((uint32_t)(timer_ms() - t0) > 10u) { con_puts("gcc-blsp: RCG update stuck\n"); return -1; }
    GCC_W(cbcr, GCC_R(cbcr) | CBCR_CLK_ENABLE);
    t0 = timer_ms();
    while (GCC_R(cbcr) & CBCR_CLK_OFF)
        if ((uint32_t)(timer_ms() - t0) > 10u) { con_puts("gcc-blsp: core clk stuck\n"); return -1; }
    timer_delay_us(200u);   /* v84 settle, see gcc_blsp_qup4_up */
    con_puts("gcc-blsp: "); con_puts(name); con_puts(" iface+core clocks up\n");
    return 0;
}

/* Bring up the touch-I2C QUP clocks. Idempotent; bounded; never touches the
 * QUP itself. Returns 0 when both iface and core clocks report running. */
int gcc_blsp_qup4_up(void)
{
    uint32_t t0;

    /* 1) BLSP1 AHB (iface) — voted branch: SET our vote bit, never clear
     * others (RPM/aboot votes share this register). */
    GCC_W(GCC_APCS_BRANCH_ENA_VOTE,
          GCC_R(GCC_APCS_BRANCH_ENA_VOTE) | BLSP1_AHB_VOTE_BIT);
    t0 = timer_ms();
    while (GCC_R(GCC_BLSP1_AHB_CBCR) & CBCR_CLK_OFF) {
        if ((uint32_t)(timer_ms() - t0) > 10u) {
            con_puts("gcc-blsp: AHB clk stuck\n");
            return -1;
        }
    }

    /* 2) RCG root: XO / 1 = 19.2 MHz (the rate the vendor DT programs), then
     * latch with UPDATE and keep the root force-enabled like gcc_mdss does. */
    GCC_W(GCC_QUP4_I2C_CMD_RCGR + 4u, RCG_CFG_XO_DIV1);
    GCC_W(GCC_QUP4_I2C_CMD_RCGR, GCC_R(GCC_QUP4_I2C_CMD_RCGR) | RCG_ROOT_EN | RCG_UPDATE);
    t0 = timer_ms();
    while (GCC_R(GCC_QUP4_I2C_CMD_RCGR) & RCG_UPDATE) {
        if ((uint32_t)(timer_ms() - t0) > 10u) {
            con_puts("gcc-blsp: RCG update stuck\n");
            return -1;
        }
    }

    /* 3) QUP4 I2C core branch. */
    GCC_W(GCC_QUP4_I2C_CBCR, GCC_R(GCC_QUP4_I2C_CBCR) | CBCR_CLK_ENABLE);
    t0 = timer_ms();
    while (GCC_R(GCC_QUP4_I2C_CBCR) & CBCR_CLK_OFF) {
        if ((uint32_t)(timer_ms() - t0) > 10u) {
            con_puts("gcc-blsp: core clk stuck\n");
            return -1;
        }
    }

    /* SETTLE (v84, 2026-09-06). Every image that ever booted printed a log line
     * right here, between the last clock enable and the first access into the
     * newly clocked block. The quiet build (v79+) removed those lines and
     * stopped booting on both 8909w watches; v82, the same code with the log
     * lines back, boots. The status bits above say the clock runs, but the
     * bus behind it evidently needs a moment longer, so give it one on
     * purpose instead of by accident. */
    timer_delay_us(200u);
    bdiag_puts("gcc-blsp: " GCC_QUP_NAME " iface+core clocks up\n");
    return 0;
}

/* SLEEP (2026-09-07): gate whichever QUP core clocks are running (touch on
 * QUP5, charger bus QUP4, sensor buses QUP1/2). The QUP registers keep their
 * state (AHB stays); wake re-enables the same set. */
static uint32_t s_qup_sleep_mask;
static const uint32_t k_qup_cbcr[] = { 0x02008u, 0x03010u, 0x04020u, 0x05020u, 0x06020u, 0x07020u };
void gcc_blsp_sleep(int on)
{
    unsigned n = sizeof k_qup_cbcr / sizeof k_qup_cbcr[0];
    if (on) {
        s_qup_sleep_mask = 0;
        for (unsigned i = 0; i < n; i++)
            if (GCC_R(k_qup_cbcr[i]) & CBCR_CLK_ENABLE) { s_qup_sleep_mask |= 1u << i; GCC_W(k_qup_cbcr[i], GCC_R(k_qup_cbcr[i]) & ~CBCR_CLK_ENABLE); }
        __asm__ volatile("dsb sy" ::: "memory");
    } else {
        for (unsigned i = 0; i < n; i++) if (s_qup_sleep_mask & (1u << i)) {
            GCC_W(k_qup_cbcr[i], GCC_R(k_qup_cbcr[i]) | CBCR_CLK_ENABLE);
            uint32_t t0 = timer_ms();
            while (GCC_R(k_qup_cbcr[i]) & CBCR_CLK_OFF) if ((uint32_t)(timer_ms() - t0) > 10u) break;
        }
        timer_delay_us(200u);
        s_qup_sleep_mask = 0;
    }
}

#endif /* PLAT_SOC_MSM */
