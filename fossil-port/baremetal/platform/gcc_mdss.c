/* gcc_mdss.c — MDSS power domain + clock bring-up on the Gen 6 (SDA429W).
 *
 * THE fix for the recurring Gen-6 failure mode: every MDSS access so far
 * (fb_splash's MDP pipe probe) wedged the AHB bus, because aboot gates the
 * MDSS clocks before handing off and a read of an unclocked MSM block never
 * completes. A lit panel proves nothing — the AUO AMOLED is command-mode and
 * self-refreshes aboot's logo from its own RAM with MDSS fully dark.
 *
 * Register layout: the msm8916/8917/8937 GCC family, which sdm429w belongs
 * to. Confirmed against the REAL device tree dumped from the watch
 * (2026-08-02): the DSI PLL node's second reg is 0x0184d074+8 — i.e.
 * GCC + 0x4d074, exactly this family's MDSS GDSC block. All MDSS branch
 * clocks (CBCRs) live in the same 0x4d000 block.
 *
 * What this deliberately does NOT do (yet): program the RCG dividers or the
 * 12nm DSI PLL. The RCGs live in the always-on GCC, so aboot's mux/divider
 * config survives handoff; the DSI byte/pixel clocks source from the DSI PLL
 * inside the (possibly collapsed) MDSS domain. If aboot killed that PLL,
 * BYTE0/PCLK0 report stuck below and a full 12nm PLL bring-up is the next
 * phase — the status flags exist precisely to make the watch tell us which
 * world we are in.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)
/* Gen 4 (msm8909w) shares this register map EXACTLY. Verified twice: the
 * vendor kernel's clock-gcc-8909.c defines MDSS_AHB_CBCR 0x4D07C ...
 * MDSS_ESC0_CBCR 0x4D098 and PCLK0/MDP/BYTE0/ESC0_CMD_RCGR at 0x4D000 /
 * 0x4D014 / 0x4D044 / 0x4D05C — identical values to the sdm429w family this
 * file was written against — and firefish's OWN dumped device tree places
 * gdsc_mdss at 0x184d078, i.e. GCC + 0x4D078. Only the mdp_clk_src rate
 * table differs, so that one write stays Gen 6 only. */

/* --- GCC MDSS block (offsets from PLAT_GCC_BASE) ------------------------- */
#define GCC_MDSS_GDSCR       0x4D078u  /* bit0 SW_COLLAPSE, bit31 PWR_ON */
#define GCC_MDSS_AHB_CBCR    0x4D07Cu
#define GCC_MDSS_AXI_CBCR    0x4D080u
#define GCC_MDSS_PCLK0_CBCR  0x4D084u
#define GCC_MDSS_MDP_CBCR    0x4D088u
#define GCC_MDSS_VSYNC_CBCR  0x4D090u
#define GCC_MDSS_BYTE0_CBCR  0x4D094u
#define GCC_MDSS_ESC0_CBCR   0x4D098u

/* RCG roots feeding the DSI branches (same 0x4d000 block; CMD_RCGR at +0) */
#define GCC_MDP_CMD_RCGR     0x4D014u  /* mdp_clk_src (gcc-sdm429w.c) */
#define GCC_PCLK0_CMD_RCGR   0x4D000u
#define GCC_BYTE0_CMD_RCGR   0x4D044u
#define GCC_ESC0_CMD_RCGR    0x4D05Cu

#define GDSC_SW_COLLAPSE     (1u << 0)
#define GDSC_PWR_ON          (1u << 31)
#define CBCR_CLK_ENABLE      (1u << 0)
#define CBCR_CLK_OFF         (1u << 31)
#define RCG_UPDATE           (1u << 0)
#define RCG_ROOT_EN          (1u << 1)
#define RCG_ROOT_OFF         (1u << 31)

#define GCC_R(off)    mmio_read(PLAT_GCC_BASE + (off))
#define GCC_W(off, v) mmio_write(PLAT_GCC_BASE + (off), (v))

static uint32_t s_status;

/* Enable one branch clock; wait for CLK_OFF to deassert. Returns 0 on ok.
 * A branch whose upstream source is dead just leaves CLK_OFF latched — it
 * cannot hang the bus (the CBCR itself is in the always-clocked GCC). */
static int branch_enable(uint32_t off)
{
    GCC_W(off, GCC_R(off) | CBCR_CLK_ENABLE);
    uint32_t t0 = timer_ms();
    while (GCC_R(off) & CBCR_CLK_OFF) {
        if ((uint32_t)(timer_ms() - t0) > 10u) return -1;
    }
    return 0;
}

/* Power the MDSS GDSC + enable every MDSS clock branch. Idempotent.
 * Returns the status flag word (also cached for gcc_mdss_status()). */
uint32_t gcc_mdss_up(void)
{
    uint32_t v = GCC_R(GCC_MDSS_GDSCR);
    if (v & GDSC_PWR_ON) {
        /* aboot left the domain powered: its MDP/DSI register state (pipes,
         * mixers, PHY, PLL) is still intact. Best case. */
        s_status |= GCC_MDSS_ST_GDSC_WAS_ON;
    } else {
        GCC_W(GCC_MDSS_GDSCR, v & ~GDSC_SW_COLLAPSE);
        uint32_t t0 = timer_ms();
        while (!(GCC_R(GCC_MDSS_GDSCR) & GDSC_PWR_ON)) {
            if ((uint32_t)(timer_ms() - t0) > 20u) {
                s_status &= ~GCC_MDSS_ST_GDSC_ON;
                return s_status;        /* domain will not power: stop here */
            }
        }
        /* settle, per the kernel gdsc driver (udelay(100)) */
        timer_delay_ms(1);
    }
    s_status |= GCC_MDSS_ST_GDSC_ON;

    /* MDP CORE CLOCK RATE (2026-08-04, THE FIFO-UNDERFLOW FIX). We always
     * enabled the MDP branch but never programmed its RCG — inheriting
     * whatever lazy rate aboot chose for one splash push. If the MDP
     * composes lines barely faster than the DSI drains them, the stream
     * FIFO underflows mid-frame, truncated lines land displaced, and the
     * "walking UI" is born (REG_BARS proved intra-frame displacement).
     * Kernel table ftbl_mdp_clk_src: 160 MHz = GPLL0 (sel 1) / 5 (hid div
     * field 2*5-1 = 9) — the VDD_LOW-safe rate, 12x the pixel rate. */
#if defined(PLAT_BOARD_FOSSIL_GEN6)
    {
        uint32_t old = GCC_R(GCC_MDP_CMD_RCGR + 4u);
        GCC_W(GCC_MDP_CMD_RCGR + 4u, (1u << 8) | 9u);
        GCC_W(GCC_MDP_CMD_RCGR, GCC_R(GCC_MDP_CMD_RCGR) | RCG_ROOT_EN | RCG_UPDATE);
        uint32_t t0 = timer_ms();
        while (GCC_R(GCC_MDP_CMD_RCGR) & RCG_UPDATE) {
            if ((uint32_t)(timer_ms() - t0) > 10u) break;
        }
        con_puts("gcc-mdss: mdp_clk_src cfg "); con_puthex(old);
        con_puts(" -> "); con_puthex(GCC_R(GCC_MDP_CMD_RCGR + 4u)); con_puts("\n");
    }
#else
    /* Gen 4: leave mdp_clk_src exactly as aboot set it. Its splash is driven
     * by the same MDP at that rate, so the rate is known-sufficient for the
     * traffic we are about to generate; the 8909 frequency table is a
     * different one and re-picking a rate blind risks the very DSI FIFO
     * underflow the Gen 6 change was made to cure. */
#endif

    int core = 0, dsi = 0;
    core |= branch_enable(GCC_MDSS_AHB_CBCR);
    core |= branch_enable(GCC_MDSS_AXI_CBCR);
    core |= branch_enable(GCC_MDSS_MDP_CBCR);
    core |= branch_enable(GCC_MDSS_VSYNC_CBCR);
    dsi  |= branch_enable(GCC_MDSS_ESC0_CBCR);
    dsi  |= branch_enable(GCC_MDSS_BYTE0_CBCR);
    dsi  |= branch_enable(GCC_MDSS_PCLK0_CBCR);

    if (core == 0) s_status |= GCC_MDSS_ST_CORE_CLKS;
    if (dsi  == 0) s_status |= GCC_MDSS_ST_DSI_CLKS;

    con_puts("gcc-mdss: status="); con_puthex(s_status);
    con_puts(" gdscr="); con_puthex(GCC_R(GCC_MDSS_GDSCR)); con_puts("\n");
    return s_status;
}

uint32_t gcc_mdss_status(void) { return s_status; }

/* Retune mdp_clk_src at runtime. cfg = (src_sel << 8) | hid_div, where the
 * real divider is (hid_div + 1) / 2 and src_sel 1 = GPLL0 (800 MHz):
 *   0x109 = /5   = 160 MHz   (what gcc_mdss_up programs)
 *   0x104 = /2.5 = 320 MHz   (what ABOOT left — see the log line
 *                             "mdp_clk_src cfg 0x104 -> 0x109": we HALVE it)
 *   0x103 = /2   = 400 MHz
 * Exists for the underflow arm sweep in fb_splash.c: if DLN0_HS_FIFO_UNDERFLOW
 * is an MDP-side supply problem its rate must move with this number. */
void gcc_mdss_set_mdp_cfg(uint32_t cfg)
{
    GCC_W(GCC_MDP_CMD_RCGR + 4u, cfg);
    GCC_W(GCC_MDP_CMD_RCGR, GCC_R(GCC_MDP_CMD_RCGR) | RCG_ROOT_EN | RCG_UPDATE);
    uint32_t t0 = timer_ms();
    while (GCC_R(GCC_MDP_CMD_RCGR) & RCG_UPDATE) {
        if ((uint32_t)(timer_ms() - t0) > 10u) break;
    }
}

/* Kick one RCG root awake: force ROOT_EN, pulse UPDATE, wait for the update
 * to latch. aboot turns these roots off when it disables the display clocks;
 * the M/N/D + source mux config itself is retained (GCC never loses power). */
static int rcg_root_enable(uint32_t off)
{
    GCC_W(off, GCC_R(off) | RCG_ROOT_EN);
    GCC_W(off, GCC_R(off) | RCG_UPDATE);
    uint32_t t0 = timer_ms();
    while (GCC_R(off) & RCG_UPDATE) {
        if ((uint32_t)(timer_ms() - t0) > 10u) return -1;
    }
    return 0;
}

/* Second stage, called AFTER dsi_pll_12nm_relock() succeeds: the byte/pixel
 * branches could not come up in gcc_mdss_up() because their source (the DSI
 * PLL) was dead. Re-enable roots, then retry the branches. */
uint32_t gcc_mdss_dsi_clks_retry(void)
{
    int rc = 0;
    rc |= rcg_root_enable(GCC_ESC0_CMD_RCGR);
    rc |= rcg_root_enable(GCC_BYTE0_CMD_RCGR);
    rc |= rcg_root_enable(GCC_PCLK0_CMD_RCGR);

    int esc  = branch_enable(GCC_MDSS_ESC0_CBCR);
    int link = branch_enable(GCC_MDSS_BYTE0_CBCR);
    link    |= branch_enable(GCC_MDSS_PCLK0_CBCR);

    s_status &= ~(GCC_MDSS_ST_DSI_CLKS | GCC_MDSS_ST_ESC0_CLK |
                  GCC_MDSS_ST_LINK_CLKS);
    if (esc  == 0) s_status |= GCC_MDSS_ST_ESC0_CLK;
    if (link == 0) s_status |= GCC_MDSS_ST_LINK_CLKS;
    if (esc == 0 && link == 0) s_status |= GCC_MDSS_ST_DSI_CLKS;

    con_puts("gcc-mdss: dsi retry rcg="); con_putdec((uint32_t)-rc);
    con_puts(" status="); con_puthex(s_status); con_puts("\n");
    return s_status;
}

#endif /* PLAT_SOC_MSM */
