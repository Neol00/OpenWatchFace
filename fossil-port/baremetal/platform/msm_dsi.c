/* msm_dsi.c — MDSS MIPI-DSI host + 28nm PHY bring-up, ported from the vendor
 * 3.18 kernel (firefish branch; see ../../HARDWARE.md).
 *
 * Sources ported:
 *   drivers/video/msm/mdss/msm_mdss_io_8974.c
 *       mdss_dsi_28nm_phy_regulator_enable()  -> dsi_phy_regulator_enable()
 *       mdss_dsi_28nm_phy_config()            -> dsi_phy_config()
 *   drivers/video/msm/mdss/mdss_dsi_host.c
 *       mdss_dsi_host_init()                  -> dsi_host_init()
 *       mdss_dsi_cmd_dma_tx()                 -> dsi_cmd_dma_tx()
 *
 * What the kernel does that we deliberately do NOT do (and why it is safe):
 *   - regulator/GDSC enable, clock enable: on this port aboot has already lit
 *     the panel for its splash, so MDSS rails and clocks are ON when we take
 *     over. dsi_takeover_from_aboot() below asserts that assumption instead of
 *     re-deriving it. Owning GCC/SPMI is Phase 6 work.
 *   - the PHY software reset dance is kept, because the panel command tables
 *     must be replayed from a known state.
 *
 * The per-board data values (timing/lanecfg/strength/regulator/bist) are NOT
 * invented here: they are the DT properties qcom,platform-* from
 * msm8909-mdss.dtsi and qcom,mdss-dsi-panel-timings from the panel DTSI.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN4)

#include "msm_dsi_regs.h"
#include <string.h>

#define DSI_R(off)      mmio_read(PLAT_DSI_CTRL_BASE + (off))
#define DSI_W(off, v)   mmio_write(PLAT_DSI_CTRL_BASE + (off), (v))
#define PHY_W(off, v)   mmio_write(PLAT_DSI_PHY_BASE + (off), (v))
#define PHYREG_W(off, v) mmio_write(PLAT_DSI_PHY_REG_BASE + (off), (v))

static inline void dsi_wmb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* --- board PHY data, verbatim from msm8909-mdss.dtsi mdss_dsi_ctrl0 ------- */

/* qcom,platform-regulator-settings = [00 01 01 00 20 07 00] */
static const uint8_t phy_regulator[7] = { 0x00, 0x01, 0x01, 0x00, 0x20, 0x07, 0x00 };
/* qcom,platform-strength-ctrl = [ff 06] */
static const uint8_t phy_strength[2]  = { 0xff, 0x06 };
/* qcom,platform-bist-ctrl = [00 00 b1 ff 00 00] */
static const uint8_t phy_bist[6]      = { 0x00, 0x00, 0xb1, 0xff, 0x00, 0x00 };
/* qcom,platform-lane-config = 5 lanes x 9 bytes */
static const uint8_t phy_lanecfg[DSIPHY_NUM_LANES * DSIPHY_LANE_NREGS] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x97,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x97,
    0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x01, 0x97,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x01, 0x97,
    0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xbb,
};
/* qcom,mdss-dsi-panel-timings from the panel DTSI (AUO 400p cmd template;
 * replace from the stock DTB once dumped — HARDWARE.md open question #1). */
static const uint8_t phy_timing[12] = {
    0x5F, 0x12, 0x0A, 0x00, 0x32, 0x34, 0x10, 0x16, 0x0F, 0x03, 0x04, 0x00
};

/* qcom,regulator-ldo-mode is present in the DT for this controller. */
#define PHY_LDO_MODE 1

/* --- PHY ----------------------------------------------------------------- */

/* Port of mdss_dsi_28nm_phy_regulator_enable() (LDO-mode branch). */
static void dsi_phy_regulator_enable(void)
{
    PHYREG_W(DSIPHY_REG_CTRL_0, 0x00);
    PHYREG_W(DSIPHY_REG_CAL_PWR_CFG, phy_regulator[6]);
    dsi_wmb();
    timer_delay_ms(1);                      /* kernel: udelay(1000) */
    PHYREG_W(DSIPHY_REG_TEST,   phy_regulator[5]);
    PHYREG_W(DSIPHY_REG_CTRL_3, phy_regulator[3]);
    PHYREG_W(DSIPHY_REG_CTRL_2, phy_regulator[2]);
    PHYREG_W(DSIPHY_REG_CTRL_1, phy_regulator[1]);
    PHYREG_W(DSIPHY_REG_CTRL_4, phy_regulator[4]);
    PHY_W(DSIPHY_LDO_CTRL, 0x0d);           /* hw_rev < 103_1 on msm8909 */
    dsi_wmb();
}

/* Port of mdss_dsi_28nm_phy_config(). */
static void dsi_phy_config(void)
{
    int i, ln, off;

    /* Strength ctrl 0 (28nm, hw_rev <= 104_2) */
    PHY_W(DSIPHY_CTRL_0, 0x5b);
    PHY_W(DSIPHY_STRENGTH_CTRL_0, phy_strength[0]);
    dsi_wmb();

    /* phy timing ctrl 0..11 */
    off = DSIPHY_TIMING_CTRL_0;
    for (i = 0; i < 12; i++) { PHY_W(off, phy_timing[i]); dsi_wmb(); off += 4; }

    /* 4 data lanes + clk lane: 9 regs each, 0x40 stride */
    for (ln = 0; ln < DSIPHY_NUM_LANES; ln++) {
        off = ln * DSIPHY_LANE_STRIDE;
        for (i = 0; i < DSIPHY_LANE_NREGS; i++) {
            PHY_W(off, phy_lanecfg[i + ln * DSIPHY_LANE_NREGS]);
            dsi_wmb();
            off += 4;
        }
    }

    PHY_W(DSIPHY_CTRL_4, 0x0a);
    dsi_wmb();
    PHY_W(DSIPHY_GLBL_TEST_CTRL, 0x01);     /* single_dsi */
    dsi_wmb();
    PHY_W(DSIPHY_CTRL_0, 0x5f);             /* lanes powered on */
    dsi_wmb();

    off = DSIPHY_BIST_CTRL_0;
    for (i = 0; i < 6; i++) { PHY_W(off, phy_bist[i]); off += 4; }

    /* Strength ctrl 1 — LP Rx + contention detection (mdss_dsi_lp_cd_rx) */
    PHY_W(DSIPHY_STRENGTH_CTRL_1, phy_strength[1]);
    dsi_wmb();
}

/* --- host ---------------------------------------------------------------- */

/* Port of mdss_dsi_host_init() for a COMMAND-mode panel, 4 lanes, 24bpp.
 * Command mode is what this panel is (dsi_cmd_mode in the panel DTSI), so the
 * video-mode timing registers are intentionally not programmed. */
static void dsi_host_init(void)
{
    uint32_t dsi_ctrl, data;

    /* DCS command control for the MDP path: packet type + virtual channel.
     * (kernel writes 0x0044 with the DCS cmd ctrl for cmd mode) */
    DSI_W(DSI_CMD_MODE_MDP_DCS_CMD_CTRL, 0x40);

    dsi_ctrl = DSI_CTRL_CLK_EN | DSI_CTRL_CMD_MODE_EN;
    dsi_ctrl |= DSI_CTRL_DATA_LANE0 | DSI_CTRL_DATA_LANE1 |
                DSI_CTRL_DATA_LANE2 | DSI_CTRL_DATA_LANE3;

    /* DSI_TRIG_CTRL: sw dma trigger (panel: dma-trigger = trigger_sw = 4),
     * mdp trigger none (0), stream 0, te_sel set (panel uses a TE pin). */
    data  = (1u << 31);          /* te_sel */
    data |= (0u << 4);           /* mdp_trigger = none */
    data |= 0x4;                 /* dma_trigger = trigger_sw */
    DSI_W(DSI_TRIG_CTRL, data);

    /* lane_map_0123 → no swap */
    DSI_W(DSI_LAN_SWAP_CTRL, 0x0);

    /* clkout timing: t_clk_post 0x05, t_clk_pre 0x11 (panel DTSI) */
    data = ((0x05u & 0x3f) << 8) | (0x11u & 0x3f);
    DSI_W(DSI_CLKOUT_TIMING_CTRL, data);

    /* EOT packet: append TX EOT (kernel default when tx_eot_append) */
    DSI_W(DSI_EOT_PACKET_CTRL, 0x1);

    /* HS TX timeout, kernel constant */
    DSI_W(DSI_HS_TIMER_CTRL, 0x3fd08);

    /* only ack-err-status generates an interrupt */
    DSI_W(DSI_ERR_INT_MASK0, 0x03f03fc0);

    /* We poll rather than take the DSI IRQ during bring-up: masks stay clear
     * so a stray DSI interrupt cannot fire before irq.c knows about it. */
    DSI_W(DSI_INTL_CTRL, 0x0);

    /* turn esc, byte, dsi, pclk, sclk, hclk on */
    DSI_W(DSI_CLK_CTRL, 0x23f);

    DSI_W(DSI_LANE_CTRL, 0x0);

    dsi_ctrl |= DSI_CTRL_ENABLE;
    DSI_W(DSI_CTRL, dsi_ctrl);
    dsi_wmb();
}

/* --- command TX ---------------------------------------------------------- */

/* DMA command buffer. The DSI DMA engine reads this over the bus, so it must
 * be physically contiguous and cache-clean. mmu.c maps DDR as Normal cacheable,
 * hence the explicit clean before handing the address to the controller.
 * 4-byte aligned + padded: the controller transfers whole words. */
static uint8_t s_cmd_buf[256] __attribute__((aligned(64)));

static void cache_clean(const void *addr, uint32_t len)
{
    uintptr_t p   = (uintptr_t)addr & ~31u;
    uintptr_t end = ((uintptr_t)addr + len + 31u) & ~31u;
    for (; p < end; p += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory"); /* DCCMVAC */
    dsi_wmb();
}

static int dsi_wait_dma_done(uint32_t timeout_ms)
{
    uint32_t t0 = timer_ms();
    /* CMD_MODE_DMA_BUSY in DSI_STATUS clears when the transfer completes. */
    while (DSI_R(DSI_STATUS) & DSI_STATUS_CMD_MODE_DMA_BUSY) {
        if ((uint32_t)(timer_ms() - t0) > timeout_ms) return -1;
    }
    /* Ack the DMA-done latch so the next transfer starts clean. */
    DSI_W(DSI_INTL_CTRL, DSI_R(DSI_INTL_CTRL) | DSI_INTR_CMD_DMA_DONE);
    return 0;
}

/* Build a MIPI DSI packet into s_cmd_buf and fire it via the DMA engine.
 * Short packets: 4 bytes (dtype, data0, data1, ecc-placeholder).
 * Long packets:  4-byte header + payload + 2-byte checksum placeholder.
 * The controller computes ECC/checksum itself, so those bytes are padding. */
static int dsi_cmd_tx(uint8_t dtype, const uint8_t *payload, uint32_t len)
{
    uint32_t n = 0;

    if (len <= 2) {                       /* short packet */
        s_cmd_buf[0] = dtype;
        s_cmd_buf[1] = len > 0 ? payload[0] : 0;
        s_cmd_buf[2] = len > 1 ? payload[1] : 0;
        s_cmd_buf[3] = 0;                 /* ECC filled by HW */
        n = 4;
    } else {                              /* long packet */
        if (len + 6 > sizeof s_cmd_buf) return -1;
        s_cmd_buf[0] = dtype;
        s_cmd_buf[1] = (uint8_t)(len & 0xff);
        s_cmd_buf[2] = (uint8_t)((len >> 8) & 0xff);
        s_cmd_buf[3] = 0;                 /* ECC filled by HW */
        memcpy(&s_cmd_buf[4], payload, len);
        n = 4 + len;
        s_cmd_buf[n++] = 0;               /* checksum filled by HW */
        s_cmd_buf[n++] = 0;
    }
    while (n & 3) s_cmd_buf[n++] = 0;     /* word-align the length */

    cache_clean(s_cmd_buf, n);

    DSI_W(DSI_DMA_CMD_OFFSET, (uint32_t)(uintptr_t)s_cmd_buf);
    DSI_W(DSI_DMA_CMD_LENGTH, n);
    dsi_wmb();
    DSI_W(DSI_CMD_MODE_DMA_SW_TRIGGER, 0x1);
    dsi_wmb();

    return dsi_wait_dma_done(50);
}

/* Public: send one DCS command with `len` parameters. */
int dsi_dcs_write(uint8_t cmd, const uint8_t *params, uint32_t len)
{
    uint8_t buf[64];

    if (len == 0)
        return dsi_cmd_tx(DTYPE_DCS_WRITE, &cmd, 1);
    if (len == 1) {
        uint8_t sp[2] = { cmd, params[0] };
        return dsi_cmd_tx(DTYPE_DCS_WRITE1, sp, 2);
    }
    if (len + 1 > sizeof buf) return -1;
    buf[0] = cmd;
    memcpy(&buf[1], params, len);
    return dsi_cmd_tx(DTYPE_DCS_LWRITE, buf, len + 1);
}

/* --- init ---------------------------------------------------------------- */

/* Full DSI bring-up. Assumes MDSS clocks/regulators are already on (aboot
 * splash left them running) — see the file header. */
void dsi_init(void)
{
    con_puts("dsi: hw_version="); con_puthex(DSI_R(DSI_HW_VERSION)); con_puts("\n");

    /* Quiesce the controller before touching the PHY, so aboot's in-flight
     * state cannot race our reprogramming. */
    DSI_W(DSI_CTRL, 0x0);
    dsi_wmb();
    timer_delay_ms(1);

    dsi_phy_regulator_enable();
    dsi_phy_config();
    dsi_host_init();

    con_puts("dsi: host up, ctrl="); con_puthex(DSI_R(DSI_CTRL)); con_puts("\n");
}

#endif /* PLAT_BOARD_FOSSIL_GEN4 */
