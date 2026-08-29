/* gcc_usb.c — GCC-side USB HS clock ownership (Gen 6 SDA429W, and the
 * msm8909w watches: Fossil Gen 4 + TicWatch C2).
 *
 * SHARED ACROSS BOTH SoCs, deliberately. The USB block's GCC window is at the
 * same offsets in gcc-msm8917.c (quoted below) and gcc-msm8916.c, which is the
 * driver family the msm8909w belongs to: USB_HS_BCR 0x41000, system CBCR
 * 0x41004, AHB CBCR 0x41008, system RCG 0x41010, phy sleep 0x4102c, phy cfg
 * AHB 0x41030, QUSB2_PHY_BCR 0x4103c. That is a strong claim to make from a
 * register map rather than from the silicon in hand, so NOTHING here trusts
 * it: every branch_enable() is bounded by a 10 ms timeout on CLK_OFF, the RCG
 * update is bounded the same way, and usb_dev_init() refuses to write a single
 * controller register unless CAPLENGTH reads back plausibly. A wrong offset
 * therefore costs the USB console and nothing else.
 *
 *
 * Step 1 of the USB device-mode bring-up. The controller is `qcom,hsusb-otg`
 * at 0x78db000 (DTS usb@78db000; asteroid dmesg: "msm_otg 78db000.usb",
 * "msm_hsusb [ci13xxx_start] hw_ep_max = 32"), i.e. a ChipIdea HS UDC.
 *
 * WHY THIS FILE EXISTS FIRST: on this SoC every single block we have brought
 * up died the same way when its GCC clocks were left gated by aboot — display
 * needed gcc_mdss.c, touch needed gcc_blsp.c, eMMC needed gcc_sdcc.c, and in
 * each case driving the block into a dead clock domain hard-reset the SoC a
 * few seconds later. USB gets its clocks turned on before a single controller
 * register is touched.
 *
 * OFFSETS ARE VERIFIED, NOT REMEMBERED. Every value below was quoted verbatim
 * out of mainline drivers/clk/qcom/gcc-msm8917.c (SDA429W is in the msm8917 /
 * sdm439 family; the SDCC offsets we already run were taken from the same
 * place and are proven on hardware):
 *
 *   usb_hs_system_clk_src       .cmd_rcgr = 0x41010, .hid_width = 5,
 *                               .parent_map = gcc_xo_gpll0_map (XO=0, GPLL0=1)
 *   ftbl_usb_hs_system_clk_src  F(80000000,  P_GPLL0, 10,  0, 0)
 *                               F(100000000, P_GPLL0,  8,  0, 0)
 *                               F(133330000, P_GPLL0,  6,  0, 0)
 *                               F(177780000, P_GPLL0,  4.5,0, 0)
 *   gcc_usb_hs_system_clk       .halt_reg/.enable_reg = 0x41004, BIT(0)
 *   gcc_usb_hs_ahb_clk          .halt_reg/.enable_reg = 0x41008, BIT(0)
 *   gcc_usb2a_phy_sleep_clk     .halt_reg/.enable_reg = 0x4102c, BIT(0)
 *   gcc_usb_hs_phy_cfg_ahb_clk  .halt_reg/.enable_reg = 0x41030, BIT(0)
 *
 * BLOCK RESETS, also verified (gcc_msm8917_resets[]):
 *   [GCC_USB_HS_BCR]            = { 0x41000 }   <- DTS "core_reset"
 *   [GCC_USB2_HS_PHY_ONLY_BCR]  = { 0x41034 }
 *   [GCC_QUSB2_PHY_BCR]         = { 0x4103c }
 * Only the CORE reset is applied here. The two PHY resets are defined but NOT
 * asserted: resetting the PHY obliges us to replay its init sequence (the DTS
 * carries qcom,hsusb-otg-phy-init-seq = <0x43 0x80 0x06 0x82 -1>, ULPI
 * value/address pairs written through the viewport), and half a PHY bring-up
 * is worse than none. That belongs in usb_phy_msm.c, next step.
 */
#include "platform.h"
#if defined(PLAT_HAVE_USB_CDC)

#define USB_HS_SYSTEM_CBCR    0x41004u
#define USB_HS_AHB_CBCR       0x41008u
#define USB_HS_SYSTEM_CMD_RCGR 0x41010u
#define USB2A_PHY_SLEEP_CBCR  0x4102Cu
#define USB_HS_PHY_CFG_AHB_CBCR 0x41030u

/* Block Control Resets (BCR): bit0 = assert, self-held until cleared. */
#define USB_HS_BCR            0x41000u   /* GCC_USB_HS_BCR ("core_reset") */
#define USB2_HS_PHY_ONLY_BCR  0x41034u   /* defined, NOT asserted — see header */
#define QUSB2_PHY_BCR         0x4103Cu   /* defined, NOT asserted — see header */

#define CBCR_CLK_ENABLE       (1u << 0)
#define CBCR_CLK_OFF          (1u << 31)
#define RCG_UPDATE            (1u << 0)
#define RCG_ROOT_EN           (1u << 1)

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

/* usb_hs_system_clk_src -> GPLL0 / 10 = 80 MHz (the lowest entry in the
 * kernel's table; USB HS needs far less than the controller's max and a
 * conservative root is the right default for first light).
 *
 * RCG2 without MND (the table entries all have m=0,n=0): CFG_RCGR is just
 * (src << CFG_SRC_SEL_SHIFT) | pre_div, where clk-rcg2's F() macro stores
 * pre_div already doubled-and-decremented — F(..., 10, ...) => 2*10-1 = 19.
 * Same encoding gcc_sdcc.c uses and that is proven on this watch. */
static int usb_hs_system_set_rate(void)
{
    uint32_t cfg = (1u << 8) | 19u;          /* src = GPLL0, div = 10 */

    GCC_W(USB_HS_SYSTEM_CMD_RCGR + 0x04u, cfg);
    GCC_W(USB_HS_SYSTEM_CMD_RCGR,
          GCC_R(USB_HS_SYSTEM_CMD_RCGR) | RCG_ROOT_EN | RCG_UPDATE);
    uint32_t t0 = timer_ms();
    while (GCC_R(USB_HS_SYSTEM_CMD_RCGR) & RCG_UPDATE) {
        if ((uint32_t)(timer_ms() - t0) > 10u) return -1;
    }
    return 0;
}

/* Core block reset. Deliberately does NOT touch the PHY BCRs (see header):
 * a PHY reset without replaying qcom,hsusb-otg-phy-init-seq leaves the link
 * dead, so the PHY is usb_phy_msm.c's job. */
static void usb_hs_core_reset(void)
{
    GCC_W(USB_HS_BCR, 1u);
    timer_delay_ms(1);
    GCC_W(USB_HS_BCR, 0u);
    timer_delay_ms(1);
}

/* QUSB2 PHY block reset pulse — the "phy_reset" reset_control the kernel's
 * msm_usb_phy_reset() asserts for QUSB_ULPI_PHY. 10 us assert per the HPG;
 * timer granularity gives us 1 ms, which only errs on the safe side. */
void gcc_usb_qusb2_phy_reset(void)
{
    GCC_W(QUSB2_PHY_BCR, 1u);
    timer_delay_ms(1);
    GCC_W(QUSB2_PHY_BCR, 0u);
    timer_delay_ms(1);
}

/* Idempotent; call before the first USB controller register access.
 * Returns 0 when the AHB + system clocks both run (sleep/phy_cfg are
 * best-effort: a halt there is informative, not fatal). */
int gcc_usb_hs_up(void)
{
    static int s_done;
    if (s_done) return 0;

    int rc = 0;
    rc |= branch_enable(USB_HS_AHB_CBCR);      /* register file reachable */
    usb_hs_core_reset();                       /* clean state, clocks up */
    rc |= usb_hs_system_set_rate();            /* root before the branch */
    rc |= branch_enable(USB_HS_SYSTEM_CBCR);   /* core clock */

    int aux = 0;
    aux |= branch_enable(USB_HS_PHY_CFG_AHB_CBCR);
    aux |= branch_enable(USB2A_PHY_SLEEP_CBCR);

    bdiag_puts("gcc-usb: ahb/system rc="); bdiag_putdec((uint32_t)-rc);
    bdiag_puts(" phy_aux=");               bdiag_putdec((uint32_t)-aux);
    bdiag_puts(" cfg=");                   bdiag_puthex(GCC_R(USB_HS_SYSTEM_CMD_RCGR + 0x04u));
    bdiag_puts("\n");

    if (rc == 0) s_done = 1;
    return rc;
}

#endif /* PLAT_HAVE_USB_CDC */
