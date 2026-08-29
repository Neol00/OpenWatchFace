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

    bdiag_puts("gcc-blsp: " GCC_QUP_NAME " iface+core clocks up\n");
    return 0;
}

#endif /* PLAT_SOC_MSM */
