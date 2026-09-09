/* dsi_pll_12nm.c — re-lock the 12nm DSI PLL the bootloader left configured.
 *
 * Ported from the REAL hoki kernel (fossil-sdm429w-devs/android_kernel_
 * fossil_sdm429w, drivers/clk/qcom/mdss/mdss-dsi-pll-12nm-util.c):
 *   pll_vco_prepare_12nm() -> the continuous-splash branch:
 *       "3.) Boot up with cont splash enabled where PHY is programmed in LK
 *        Execute the Re-lock sequence to enable the DSI PLL."
 *   dsi_pll_relock() + pll_is_pll_locked_12nm() + pll_vco_enable_12nm()
 *
 * Why relock and not a full bring-up: display3 proved the MDSS GDSC was never
 * collapsed after aboot's splash (the MDP pipe config survived intact), so the
 * PLL's frequency configuration — programmed by aboot for this exact panel
 * (308.65536 Mbps, 1 lane) — is still sitting in the registers. aboot's
 * disable path only strips the output gates (CLK_SEL/GP_CLK_EN) and asserts
 * the power-down override; relock undoes exactly that. SYS_CTRL bit7 tells us
 * whether that story is true (the kernel keys the same decision off it).
 *
 * All offsets are from the PHY/PLL block base 0x1a94400 (DT: dsi_phy +
 * qcom,mdss_dsi_pll_12nm share one 0x400 region).
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#define DSIPHY_PLL_POWERUP_CTRL   0x034u
#define DSIPHY_SSC0               0x394u
#define DSIPHY_STAT0              0x3E0u
#define DSIPHY_SYS_CTRL           0x3F0u
#define DSIPHY_PLL_CTRL           0x3F8u

/* 12nm PHY registers (mdss_dsi_phy_12nm.c) */
#define PHY_T_TA_GO               0x014u
#define PHY_T_TA_SURE             0x018u
#define PHY_HSTX_DRV_CLKLANE      0x0C0u
#define PHY_HSTX_DATAREV_CLKLANE  0x0D4u
#define PHY_HSTX_DRV_LANE0        0x100u
#define PHY_HSTX_READY_DLY_LANE0  0x114u
#define PHY_HSTX_DRV_LANE1        0x140u
#define PHY_HSTX_READY_DLY_LANE1  0x154u
#define PHY_CLKLANE_REQSTATE      0x180u
#define PHY_CLKLANE_HS0STATE      0x188u
#define PHY_CLKLANE_TRAILSTATE    0x18Cu
#define PHY_CLKLANE_EXITSTATE     0x190u
#define PHY_CLKLANE_CLKPOSTSTATE  0x194u
#define PHY_DATALANE_REQSTATE     0x1C0u
#define PHY_DATALANE_HS0STATE     0x1C8u
#define PHY_DATALANE_TRAILSTATE   0x1CCu
#define PHY_DATALANE_EXITSTATE    0x1D0u
#define PHY_HSTX_DRV_LANE2        0x200u
#define PHY_HSTX_READY_DLY_LANE2  0x214u
#define PHY_HSTX_DRV_LANE3        0x240u
#define PHY_HSTX_READY_DLY_LANE3  0x254u
#define PHY_CTRL0                 0x3E8u  /* bit0 = CFG_CLK_EN */
#define PHY_REQ_DLY               0x3FCu

#define PLL_R(off)    mmio_read(PLAT_DSI_PHY_BASE + (off))
#define PLL_W(off, v) mmio_write(PLAT_DSI_PHY_BASE + (off), (v))

static inline void pll_wmb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* ~microsecond spin off the arch timer (same trick as msm_wdog's wd_sync). */
static void pll_udelay(uint32_t us)
{
    uint64_t f = timer_freq_hz();
    uint64_t ticks = (f * us) / 1000000u + 1u;
    uint64_t t0 = timer_ticks();
    while ((uint64_t)(timer_ticks() - t0) < ticks) { }
}

/* STAT0 bit1 = PLL lock. Kernel polls up to 1000 reads / 500 us; give it
 * a generous 5 ms of wall time. */
static int pll_locked(void)
{
    uint32_t t0 = timer_ms();
    while (!(PLL_R(DSIPHY_STAT0) & (1u << 1))) {
        if ((uint32_t)(timer_ms() - t0) > 5u) return 0;
    }
    return 1;
}

static uint32_t s_pll_st;

int dsi_pll_12nm_status_locked(void) { return (s_pll_st & DSI_PLL_ST_LOCKED) != 0; }

/* Full 12nm PHY configuration replay — port of mdss_dsi_12nm_phy_config() +
 * mdss_dsi_12nm_phy_hstx_drv_ctrl(true) from the hoki kernel.
 *
 * Needed because display4 showed the PLL VCO relocks but its byte/pixel
 * outputs stay dead: aboot's shutdown path (SYS_CTRL=0x09 + PHY reset)
 * leaves the PHY side — including CFG_CLK_EN, the gate the PLL output
 * dividers run behind — disabled and possibly wiped.
 *
 * The 8 timing bytes are NOT computed: they are qcom,mdss-dsi-panel-timings-
 * phy-12nm from the REAL panel node dumped from this watch (2026-08-02):
 * [06 05 01 0a 00 03 01 0f]. The OR-in bits are the kernel's, verbatim. */
void dsi_phy_12nm_config(void)
{
    static const uint8_t t[8] = { 0x06, 0x05, 0x01, 0x0a, 0x00, 0x03, 0x01, 0x0f };

    PLL_W(PHY_CTRL0, 0x01);                       /* CFG_CLK_EN */

    PLL_W(PHY_CLKLANE_HS0STATE,     t[0] | 0x80u);
    PLL_W(PHY_CLKLANE_TRAILSTATE,   t[1] | 0x40u);
    PLL_W(PHY_CLKLANE_CLKPOSTSTATE, t[2] | 0x40u);
    PLL_W(PHY_CLKLANE_REQSTATE,     t[3]);
    PLL_W(PHY_CLKLANE_EXITSTATE,    t[7] | 0x40u | 0x80u);

    PLL_W(PHY_DATALANE_HS0STATE,    t[4] | 0x80u);
    PLL_W(PHY_DATALANE_TRAILSTATE,  t[5] | 0x40u);
    PLL_W(PHY_DATALANE_REQSTATE,    t[6]);
    PLL_W(PHY_DATALANE_EXITSTATE,   t[7] | 0x40u | 0x80u);

    PLL_W(PHY_T_TA_GO,   0x03);
    PLL_W(PHY_T_TA_SURE, 0x01);
    PLL_W(PHY_REQ_DLY,   0x85);

    PLL_W(PHY_HSTX_READY_DLY_LANE0, 0x00);
    PLL_W(PHY_HSTX_READY_DLY_LANE1, 0x00);
    PLL_W(PHY_HSTX_READY_DLY_LANE2, 0x00);
    PLL_W(PHY_HSTX_READY_DLY_LANE3, 0x00);
    PLL_W(PHY_HSTX_DATAREV_CLKLANE, 0x00);
    pll_wmb();

    /* hstx_drv_ctrl(enable): BIT(2)|BIT(3) on clock lane + all data lanes */
    PLL_W(PHY_HSTX_DRV_CLKLANE, 0x0C);
    PLL_W(PHY_HSTX_DRV_LANE0,   0x0C);
    PLL_W(PHY_HSTX_DRV_LANE1,   0x0C);
    PLL_W(PHY_HSTX_DRV_LANE2,   0x0C);
    PLL_W(PHY_HSTX_DRV_LANE3,   0x0C);
    pll_wmb();

    con_puts("dsi-phy: 12nm config replayed\n");
}

/* Full 12nm PLL programming for the AUO panel link — port of
 * pll_vco_set_rate_12nm() -> pll_db_commit_12nm() plus the three mux/divider
 * setters (set_post_div_mux_sel / set_gp_mux_sel / pixel_div_set_div), then
 * the first-enable sequence (dsi_pll_enable_seq_12nm).
 *
 * Why: display5 proved relock-with-retained-config cannot work — aboot's
 * shutdown PHY-RESETS the block, so the divider/mux config the relock path
 * trusts is gone. This writes every register from scratch.
 *
 * Values computed for the panel's 308.65536 Mbps 1-lane link exactly the way
 * the kernel computes them (VCO legal range 1.0-2.0 GHz per the driver):
 *   post_div = 8  -> VCO = 8 * 154.32768 MHz = 1234.62 MHz
 *   m_div    = round(VCO * 4 / 19.2 MHz) = 257  -> real VCO 1233.6 MHz
 *                (0.08% below nominal; DSI cmd mode is source-synchronous)
 *   byte_clk = VCO/8/4 = 38.55 MHz;  hsfreqrange(308 Mbps) = 0x14
 *   pixel:  gp_mux = VCO/8, then /(11+1) -> dsi_pclk 12.85 MHz (= bitclk/24)
 *   osc_freq_target = 1315 (target <= 1 GHz), fsm_ovr = BIT(6) (<= 1.5 Gbps)
 *   vco_cntrl: p_div 8 -> 0x20, band 136..178 MHz -> |2 => 0x22; cpbias = 0
 */
uint32_t dsi_pll_12nm_program(void)
{
    uint32_t st = 0;

    if (PLL_R(DSIPHY_SYS_CTRL) & (1u << 7))
        st |= DSI_PLL_ST_LK_PROGRAMMED;

    /* pll_db_commit_12nm(), verbatim order */
    PLL_W(PHY_CTRL0, 0x01);                 /* CFG_CLK_EN                    */
    PLL_W(DSIPHY_PLL_CTRL, 0x05);           /* CLK_SEL, gp mux placeholder   */
    PLL_W(0x28C, 0x01);                     /* SLEWRATE_DDL_LOOP_CTRL        */
    PLL_W(0x110, 0x14 | 0x80);              /* HS_FREQ_RAN_SEL | ovr enable  */
    PLL_W(0x290, 1315u & 0x7F);             /* osc target [6:0]  = 0x23      */
    PLL_W(0x328, (1315u & 0xF80) >> 7);     /* osc target [11:7] = 0x0A      */
    PLL_W(0x064, 0x30);                     /* INPUT_LOOP_DIV_RAT_CTRL       */
    PLL_W(0x060, 257u & 0x3F);              /* LOOP_DIV_RATIO_0 = 0x01       */
    PLL_W(0x2E8, (257u & 0xFC0) >> 6);      /* LOOP_DIV_RATIO_1 = 0x04       */
    PLL_W(0x05C, 0x60);                     /* INPUT_DIV_PLL_OVR             */
    PLL_W(0x038, 0x05);                     /* PROP_CHRG_PUMP_CTRL           */
    PLL_W(0x03C, 0x00);                     /* INTEG_CHRG_PUMP_CTRL          */
    PLL_W(0x04C, 0x1u << 4);                /* GMP_CTRL_DIG_TST              */
    PLL_W(0x07C, 0x03);                     /* ANA_PROG_CTRL                 */
    PLL_W(0x044, 0x50);                     /* ANA_TST_LOCK_ST_OVR_CTRL      */
    PLL_W(0x280, 1u << 6);                  /* SLEWRATE_FSM_OVR_CTRL         */
    PLL_W(0x050, 0x01);                     /* PLL_PHA_ERR_CTRL_0            */
    PLL_W(0x2E4, 0x00);                     /* PLL_PHA_ERR_CTRL_1            */
    PLL_W(0x054, 0xFF);                     /* PLL_LOCK_FILTER               */
    PLL_W(0x058, 0x03);                     /* PLL_UNLOCK_FILTER             */
    PLL_W(0x06C, 0x0C);                     /* PLL_PRO_DLY_RELOCK            */
    PLL_W(0x074, 0x02);                     /* PLL_LOCK_DET_MODE_SEL         */

    /* the three "programmed during vco_prepare" registers */
    PLL_W(0x048, 0x22 | (1u << 6));         /* VCO_CTRL: post_div 8, band 2  */
    PLL_W(0x070, (0u << 6) | (1u << 4));    /* CHAR_PUMP_BIAS: cpbias 0      */
    PLL_W(DSIPHY_PLL_CTRL, ((3u & 7u) << 5) | 0x5u);  /* gp mux = /8 -> 0x65 */
    PLL_W(0x3B8, 11u & 0x7F);               /* SSC9: pixel div (11+1)=12     */
    pll_wmb();

    /* clear any power-down override aboot left, then the enable sequence */
    uint32_t v = PLL_R(DSIPHY_PLL_POWERUP_CTRL);
    PLL_W(DSIPHY_PLL_POWERUP_CTRL, v & ~0x3u);
    pll_udelay(1);

    PLL_W(DSIPHY_SYS_CTRL, 0x49);
    pll_wmb();
    pll_udelay(5);
    PLL_W(DSIPHY_SYS_CTRL, 0xc9);
    pll_wmb();
    pll_udelay(50);

    if (!pll_locked()) {
        con_puts("dsi-pll: full-program lock FAILED, stat0=");
        con_puthex(PLL_R(DSIPHY_STAT0)); con_puts("\n");
        s_pll_st = st;
        return st;
    }
    st |= DSI_PLL_ST_LOCKED;
    pll_udelay(1);

    /* GP (pixel) clock output on */
    PLL_W(DSIPHY_SSC0, PLL_R(DSIPHY_SSC0) | (1u << 6));
    pll_wmb();

    con_puts("dsi-pll: full program LOCKED\n");
    s_pll_st = st;
    return st;
}

/* DSI controller software reset — port of mdss_dsi_sw_reset(). The kernel
 * pulses this at init AND as error recovery (stuck busy engines). display10's
 * census showed DLN0 PHY errors latching during transfers: the lane state
 * machine never got a clean restart after aboot's shutdown (whose last act is
 * a PHY reset, parking the lanes in a state our reconfig alone cannot clear). */
void dsi_host_12nm_sw_reset(void)
{
    uint32_t ctrl = mmio_read(PLAT_DSI_CTRL_BASE + 0x004u);
    mmio_write(PLAT_DSI_CTRL_BASE + 0x004u, ctrl & ~1u);  /* disable first */
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(PLAT_DSI_CTRL_BASE + 0x11Cu, 0x23Fu);      /* clocks MUST run */
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(PLAT_DSI_CTRL_BASE + 0x118u, 0x01u);       /* DSI_RESET */
    __asm__ volatile("dsb sy" ::: "memory");
    pll_udelay(10);
    mmio_write(PLAT_DSI_CTRL_BASE + 0x118u, 0x00u);
    __asm__ volatile("dsb sy" ::: "memory");
    /* NOT re-enabled here: dsi_host_12nm_reenable() restores enable + config */
}

/* TRIG_CTRL @0x084 (2026-08-06). Never programmed on gen 6 until now — the
 * port inherited whatever aboot left, and then dsi_dcs.c's dcs_dma_send()
 * overwrote the whole register with a bare 0x04 on every frame (it runs from
 * fb_kick via dsi_dcs_repin_window), permanently clearing TE_SEL. With TE_SEL
 * clear the command-mode MDP stream is not gated on the panel's TE at all, so
 * every frame is pushed into DDIC RAM regardless of where the DDIC's scan-out
 * pointer is — the collision stalls the link mid-packet and underflows the
 * lane FIFO. See the long note in dcs_dma_send().
 *
 * The panel DT names all three fields: qcom,mdss-dsi-mdp-trigger = "none" (0),
 * qcom,mdss-dsi-dma-trigger = "trigger_sw" (4), qcom,mdss-dsi-te-using-te-pin
 * + te-pin-select 1 => TE_SEL. Bit layout is mdss_dsi_host_init's, whose gen-4
 * port sits in msm_dsi.c and builds exactly this word.
 *
 * Separate from dsi_host_12nm_reenable() because that function is only reached
 * when the DSI clocks were found down at boot (and from error recovery); on a
 * clean handoff nothing would have set the trigger config at all. */
void dsi_host_12nm_trigger_setup(void)
{
    mmio_write(PLAT_DSI_CTRL_BASE + 0x084u,
               (1u << 31)        /* TE_SEL: gate the MDP stream on panel TE */
             | (0u << 4)         /* MDP_TRIGGER_SEL = none                  */
             | 0x4u);            /* DMA_TRIGGER_SEL = sw                    */

    /* ===== DSI_HS_TIMER_CTRL @0x0BC — THE FRAME-ABORT BUG (2026-08-06) =====
     *
     * Gen 6 never programmed this register at all. The live log caught it:
     * hstmr=0x0000ffff, i.e. whatever aboot left behind.
     *
     * The register is [15:0] HS_TX_TO count and [19:16] TIMER_RESOLUTION, and
     * the timeout expires after count * 2^resolution byte-clocks. Do the
     * arithmetic for this link (1 lane, 308.65536 Mbps => byte clock 38.58
     * MHz, frame = 416*416*3 = 519168 bytes = 13.46 ms):
     *
     *   aboot  0x0000ffff -> 65535 * 2^0 =  65535 byte-clocks =  1.70 ms
     *                        = 12.6%% of ONE FRAME
     *   kernel 0x0003fd08 -> 64776 * 2^3 = 518208 byte-clocks = 13.44 ms
     *                        = one frame, by construction
     *
     * So the controller was aborting the HS transmission roughly EIGHT TIMES
     * PER FRAME. An aborted HS transmit starves the lane FIFO (the
     * DLN0_HS_FIFO_UNDERFLOW we have been chasing), truncates the line in
     * flight, and leaves every following line displaced with its byte phase
     * shifted — which is the freeze / flicker / wrapped image / wrong colours
     * cluster, all four from this one register. The error census corroborates
     * it directly: cls has bit1 (TIMEOUT class) set in every single window.
     *
     * 0x3fd08 is the kernel's own constant from mdss_dsi_host_init; the gen-4
     * port in msm_dsi.c has always written it (see DSI_HS_TIMER_CTRL there).
     * Gen 6 simply never had a host_init of its own to carry it over.
     *
     * MEASURED 2026-08-06: installing it does NOT fix the underflow, and
     * HS_TX_TIMEOUT (tmo bit0) still fires with the full one-frame budget —
     * so a frame really is taking >13.4 ms of HS time and this timeout is a
     * SYMPTOM of the stall, not its cause. Keep the correct value anyway (it
     * is what the kernel programs), but the root cause is upstream of here.
     *
     * Sourced from g_dsi_hstmr so the arm sweep can actually vary it: this
     * function is re-run by the recovery path on every underflow, which
     * silently reverted round 2's writes and voided that experiment. */
    mmio_write(PLAT_DSI_CTRL_BASE + 0x0BCu, g_dsi_hstmr);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Re-assert the DSI controller's own enable/clock gates, in case aboot's
 * shutdown cleared them (register CONTENT survives — the domain never lost
 * power — but the enable bits are exactly what a shutdown strips).
 * 1 data lane per the panel DT (qcom,mdss-dsi-lane-0-state only).
 *
 * ALSO reprogram the command-mode MDP-stream registers (mdss_dsi_host.c
 * mdss_dsi_mode_setup, cmd branch). THE REPEAT-FRAME FIX lives here:
 * DCS_CMD_CTRL bit16 = "insert the DCS write command into every frame".
 * aboot pushes its logo exactly ONCE, so it works without that bit — but
 * without it every frame after the first streams into a panel write pointer
 * parked at end-of-RAM: the transfer completes (PP0/CMD_MDP_DONE fire) and
 * the glass never changes. display7's solid-white, explained. */
void dsi_host_12nm_reenable(void)
{
    /* DSI_CLK_CTRL @0x11c: ahbs/ahbm/pclk/esc/byte/dsi/... all on (kernel
     * writes 0x23f when enabling the host). */
    mmio_write(PLAT_DSI_CTRL_BASE + 0x11Cu, 0x23Fu);
    __asm__ volatile("dsb sy" ::: "memory");

    /* DCS_CMD_CTRL @0x44: wr_mem_continue 0x3C << 8 | wr_mem_start 0x2C,
     * bit16 = insert DCS command per frame (the fix). */
    mmio_write(PLAT_DSI_CTRL_BASE + 0x044u,
               (1u << 16) | (0x3Cu << 8) | 0x2Cu);

    dsi_host_12nm_trigger_setup();

    /* BURST MODE @0x1B8 bit16 (2026-08-04, THE MISSED LINE). The kernel's
     * mdss_dsi_mode_setup calls mdss_dsi_set_burst_mode() right before the
     * stream registers we ported — and we skipped it. The panel DT demands
     * burst traffic (qcom,mdss-dsi-traffic-mode "burst_mode"); without the
     * bit the DSI paces HS transmission assuming gapless pixel supply, so
     * any per-line supply hiccup starves the LANE fifo MID-PACKET
     * (DLN0_HS_FIFO_UNDERFLOW, bit 19 — measured on glass via REG_BARS) and
     * every line after the stall lands displaced: the "walking UI". */
    mmio_write(PLAT_DSI_CTRL_BASE + 0x1B8u,
               mmio_read(PLAT_DSI_CTRL_BASE + 0x1B8u) | (1u << 16));

    /* MDP stream 0+1 ctrl/total (mdss_dsi_mode_setup): full 416x416 frame,
     * RGB888 on the wire, VC 0, wrapped as DTYPE_DCS_LWRITE (0x39).
     * ystride = width*3 + 1 (the +1 is the DCS opcode byte). */
    {
        uint32_t w = PLAT_PANEL_W, h = PLAT_PANEL_H;
        uint32_t stream_ctrl  = ((w * 3u + 1u) << 16) | (0u << 8) | 0x39u;
        uint32_t stream_total = (h << 16) | w;
        mmio_write(PLAT_DSI_CTRL_BASE + 0x058u, stream_ctrl);
        mmio_write(PLAT_DSI_CTRL_BASE + 0x060u, stream_ctrl);
        mmio_write(PLAT_DSI_CTRL_BASE + 0x05Cu, stream_total);
        mmio_write(PLAT_DSI_CTRL_BASE + 0x064u, stream_total);
    }
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t ctrl = mmio_read(PLAT_DSI_CTRL_BASE + 0x004u);
    ctrl |= (1u << 0)     /* ENABLE       */
          | (1u << 2)     /* CMD_MODE_EN  */
          | (1u << 4)     /* DATA_LANE0   */
          | (1u << 8);    /* CLK_EN       */
    mmio_write(PLAT_DSI_CTRL_BASE + 0x004u, ctrl);
    __asm__ volatile("dsb sy" ::: "memory");

    /* 2026-08-06: this used to print unconditionally — and because this
     * function is ONLY reached from the DSI FIFO-error recovery path, a
     * recovery storm turned it into a per-frame print that wrapped the 64 KB
     * ramlog faster than logfile_flush() could drain it. The first real log
     * off this watch contained nothing but this line. Log only when the value
     * CHANGES (plus the first call); the recovery RATE is reported once per
     * second by the census in fb_splash.c instead. */
    {
        static uint32_t s_last_ctrl; static int s_seen;
        if (!s_seen || ctrl != s_last_ctrl) {
            s_seen = 1; s_last_ctrl = ctrl;
            con_puts("dsi-host: ctrl="); con_puthex(ctrl); con_puts("\n");
        }
    }
}

/* Returns a DSI_PLL_ST_* bitmask. Requires gcc_mdss_up() first (the PLL block
 * sits behind the MDSS AHB — unclocked access hangs the bus). */
uint32_t dsi_pll_12nm_relock(void)
{
    uint32_t st = 0;

    uint32_t sys = PLL_R(DSIPHY_SYS_CTRL);
    if (sys & (1u << 7))
        st |= DSI_PLL_ST_LK_PROGRAMMED;   /* aboot left the PHY programmed */

    /* dsi_pll_relock(), verbatim: drop the power-down override, pulse the
     * enable through SYS_CTRL, wait for lock. */
    uint32_t v = PLL_R(DSIPHY_PLL_POWERUP_CTRL);
    v &= ~(1u << 1);                      /* ONPLL_OVR_EN off */
    v &= ~(1u << 0);                      /* ONPLL_OVR off    */
    PLL_W(DSIPHY_PLL_POWERUP_CTRL, v);
    pll_udelay(1);                        /* kernel: ndelay(500) */

    PLL_W(DSIPHY_SYS_CTRL, 0x49);
    pll_wmb();
    pll_udelay(5);
    PLL_W(DSIPHY_SYS_CTRL, 0xc9);
    pll_wmb();
    pll_udelay(50);

    if (!pll_locked()) {
        con_puts("dsi-pll: relock FAILED, stat0=");
        con_puthex(PLL_R(DSIPHY_STAT0));
        con_puts(" sys_ctrl="); con_puthex(sys); con_puts("\n");
        s_pll_st = st;
        return st;
    }
    st |= DSI_PLL_ST_LOCKED;
    pll_udelay(1);                        /* kernel: ndelay(50) */

    /* route the PLL to the byte/pixel outputs (CLK_SEL) ... */
    PLL_W(DSIPHY_PLL_CTRL, PLL_R(DSIPHY_PLL_CTRL) | 0x01u);
    pll_udelay(1);                        /* kernel: ndelay(500) */
    pll_wmb();

    /* ... and the GP clock output (pll_vco_enable_12nm) */
    PLL_W(DSIPHY_SSC0, PLL_R(DSIPHY_SSC0) | (1u << 6));
    pll_wmb();

    con_puts("dsi-pll: LOCKED, sys_ctrl was "); con_puthex(sys); con_puts("\n");
    s_pll_st = st;
    return st;
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
