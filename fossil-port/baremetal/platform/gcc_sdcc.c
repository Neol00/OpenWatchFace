/* gcc_sdcc.c — GCC-side SDCC1 (eMMC) clock ownership on the Gen 6.
 *
 * THE STORAGE2 4-6 s DEATH, third instance of the port's oldest failure
 * class: aboot gates a block's GCC clocks at handoff, and driving the block
 * anyway is an instant hard reset. Display needed gcc_mdss.c, touch needed
 * gcc_blsp.c — eMMC needs this. STORAGE1 proved the SDHCI *register file*
 * stays reachable (AHB clock on: the present-state read returned cleanly),
 * so the gated one is the CORE clock (gcc_sdcc1_apps): commands "run" into
 * a dead clock domain.
 *
 * Registers verbatim from the hoki kernel's gcc-sdm429w.c:
 *   sdcc1_apps_clk_src  RCG   CMD_RCGR 0x42004  (divider config retained;
 *                                       root may be parked off by aboot)
 *   gcc_sdcc1_ahb_clk   CBCR  0x4201C
 *   gcc_sdcc1_apps_clk  CBCR  0x42018
 *   sdcc1_ice_core      RCG 0x5D000 / CBCR 0x5D014 (inline crypto engine —
 *                                       enabled tolerantly, some cores gate
 *                                       internal paths on it)
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)
/* Gen 4 (msm8909w) is the same GCC family and needs no changes: the vendor
 * clock-gcc-8909.c defines SDCC1_APPS_CMD_RCGR 0x42004, SDCC1_APPS_CBCR
 * 0x42018 and SDCC1_AHB_CBCR 0x4201C, and its ftbl_gcc_sdcc1_2_apps_clk
 * carries the identical two recipes used below (400 kHz = XO/12 M1/N4,
 * 25 MHz = GPLL0/16 M1/N2). The 8909 has no inline crypto engine, so the
 * ICE block is the one Gen-6-only part. */

#define SDCC1_APPS_CMD_RCGR  0x42004u
#define SDCC1_APPS_CBCR      0x42018u
#define SDCC1_AHB_CBCR       0x4201Cu
#define SDCC1_ICE_CMD_RCGR   0x5D000u
#define SDCC1_ICE_CBCR       0x5D014u

#define CBCR_CLK_ENABLE      (1u << 0)
#define CBCR_CLK_OFF         (1u << 31)
#define RCG_UPDATE           (1u << 0)
#define RCG_ROOT_EN          (1u << 1)

#define GCC_R(off)    mmio_read(PLAT_GCC_BASE + (off))
#define GCC_W(off, v) mmio_write(PLAT_GCC_BASE + (off), (v))

static int branch_enable(uint32_t off)
{
    GCC_W(off, GCC_R(off) | CBCR_CLK_ENABLE);
    uint32_t t0 = timer_ms();
    while (GCC_R(off) & CBCR_CLK_OFF) {
        if ((uint32_t)(timer_ms() - t0) > 10u) return -1;
    }
    return 0;
}

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

/* Idempotent; call before the first SDHCI access. Returns 0 when the AHB +
 * core clocks both run (ICE is best-effort). */
int gcc_sdcc1_up(void)
{
    static int s_done;
    if (s_done) return 0;

    int rc = 0;
    rc |= branch_enable(SDCC1_AHB_CBCR);
    /* STORAGE15, the magenta root (finally): aboot runs the eMMC off GPLL4
     * (384 MHz HS mode) and shuts GPLL4 down at handoff — the RCG points at
     * a DEAD PLL, so the apps branch can never un-halt and gcc_sdcc1_up
     * failed... which emmc_init reported with no error latched = the exact
     * magenta class, BEFORE any of the later fixes could even run. Repoint
     * the RCG to always-alive GPLL0 (25 MHz recipe) FIRST. */
    rc |= gcc_sdcc1_set_rate(0);
    rc |= rcg_root_enable(SDCC1_APPS_CMD_RCGR);
    rc |= branch_enable(SDCC1_APPS_CBCR);

#if defined(PLAT_BOARD_FOSSIL_GEN6)
    /* best-effort ICE (not all paths need it; a halt here is not fatal) */
    rcg_root_enable(SDCC1_ICE_CMD_RCGR);
    branch_enable(SDCC1_ICE_CBCR);
#endif

    bdiag_puts("gcc-sdcc: ahb/apps up rc="); bdiag_putdec((uint32_t)-rc);
    bdiag_puts(" rcg="); bdiag_puthex(GCC_R(SDCC1_APPS_CMD_RCGR + 4u)); bdiag_puts("\n");
    if (rc == 0) s_done = 1;
    return rc;
}

/* Card-clock RATE control (2026-08-06, STORAGE11). sdhci-msm does NOT use
 * the standard SDHCI internal divider — the card clock IS this RCG (the
 * kernel's sdhci-msm changes rates via clk_set_rate on sdcc1_apps and keeps
 * the divider bypassed). STORAGE10 programmed the internal divider instead:
 * dead card clock, every command silently never completed (magenta class).
 *
 * Recipes verbatim from ftbl_sdcc1_apps_clk_src (parent map 18: XO=0,
 * GPLL0=1):  400 kHz = XO/12 * (1/4)   (identification, spec-mandated)
 *            25 MHz  = GPLL0/16 * (1/2) (conservative transfer)
 * RCG2+MND programming: M=m, N=~(n-m), D=~n, CFG=(src<<8)|div|(dual-edge),
 * then pulse UPDATE. Glitch-free switching is an RCG hardware property. */
int gcc_sdcc1_set_rate(int ident)
{
    uint32_t cfg, m, n;
    if (ident) { cfg = (0u << 8) | 23u | (2u << 12); m = 1u; n = 4u; }
    else       { cfg = (1u << 8) | 31u | (2u << 12); m = 1u; n = 2u; }

    GCC_W(SDCC1_APPS_CMD_RCGR + 0x08u, m);
    GCC_W(SDCC1_APPS_CMD_RCGR + 0x0Cu, (~(n - m)) & 0xFFu);
    GCC_W(SDCC1_APPS_CMD_RCGR + 0x10u, (~n) & 0xFFu);
    GCC_W(SDCC1_APPS_CMD_RCGR + 0x04u, cfg);
    GCC_W(SDCC1_APPS_CMD_RCGR, GCC_R(SDCC1_APPS_CMD_RCGR) | RCG_ROOT_EN | RCG_UPDATE);
    uint32_t t0 = timer_ms();
    while (GCC_R(SDCC1_APPS_CMD_RCGR) & RCG_UPDATE) {
        if ((uint32_t)(timer_ms() - t0) > 10u) return -1;
    }
    bdiag_puts(ident ? "gcc-sdcc: 400kHz (ident)\n" : "gcc-sdcc: 25MHz (transfer)\n");
    return 0;
}

#endif /* PLAT_SOC_MSM */
