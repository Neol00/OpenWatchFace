/* usb_phy_msm.c — ULPI PHY bring-up for the Gen 6 HS USB controller.
 *
 * Step 2 of the USB device-mode bring-up (step 1 = gcc_usb.c clocks).
 *
 * EVERYTHING HERE IS FROM THE DEVICE, NOT FROM MEMORY. The values were read
 * out of the merged /proc/device-tree pulled off this watch on 2026-08-06
 * (dumps/gen6-44mm-20260806/device-tree/soc/usb@78db000):
 *
 *   compatible                  = "qcom,hsusb-otg"
 *   reg                         = <0x078db000 0x400>, <0x0006c000 0x200>
 *   reg-names                   = "core", "phy_csr"
 *   qcom,hsusb-otg-phy-type     = <3>          (QUSB_ULPI_PHY)
 *   qcom,hsusb-otg-mode         = <1>          (peripheral)
 *   qcom,hsusb-otg-otg-control  = <2>          (PMIC)
 *   qcom,hsusb-otg-phy-init-seq = <0x73 0x80  0x3f 0x81  0x7f 0x82>
 *
 * NOTE the init sequence is NOT the one the old gcc_usb.c header comment
 * claimed (<0x43 0x80 0x06 0x82>). That came from the Gen 4 firefish board.
 * This watch wants 0x73->0x80, 0x3f->0x81, 0x7f->0x82. The pairs are
 * (value, address), matching msm_otg's loop:
 *     for (i = 0; seq[i] >= 0; i += 2) ulpi_write(otg, seq[i], seq[i + 1]);
 *
 * REGISTER MODEL, cross-checked rather than recalled. msm_otg uses absolute
 * offsets from the core base: USBCMD 0x140, USBINTR 0x148, ULPI_VIEWPORT
 * 0x170, PORTSC 0x184. EHCI puts USBCMD at op+0x00 and PORTSC at op+0x44, so
 * op = 0x140 and 0x140 + 0x44 = 0x184 checks out. The capability block
 * therefore starts at core+0x100 with CAPLENGTH = 0x40. Consistent on all
 * three landmarks, so the map is right.
 */
#include "platform.h"
#if defined(PLAT_HAVE_USB_CDC)

#define ULPI_VIEWPORT      (PLAT_USB_BASE + 0x170u)

#define ULPI_RUN           (1u << 30)
#define ULPI_WRITE         (1u << 29)
#define ULPI_ADDR(a)       (((a) & 0xFFu) << 16)
#define ULPI_DATA(d)       ((d) & 0xFFu)
#define ULPI_DATA_READ(v)  (((v) >> 8) & 0xFFu)

/* The viewport is a request/complete register: write the request with RUN
 * set, the hardware clears RUN when the ULPI transaction has finished. A PHY
 * with no clock never clears it, which is exactly the failure we want bounded
 * rather than a hang. */
static int ulpi_wait(void)
{
    uint32_t t0 = timer_ms();
    while (mmio_read(ULPI_VIEWPORT) & ULPI_RUN) {
        if ((uint32_t)(timer_ms() - t0) > 10u) return -1;
    }
    return 0;
}

int usb_ulpi_write(uint8_t val, uint8_t reg)
{
    mmio_write(ULPI_VIEWPORT, ULPI_RUN | ULPI_WRITE | ULPI_ADDR(reg) | ULPI_DATA(val));
    return ulpi_wait();
}

int usb_ulpi_read(uint8_t reg, uint8_t *out)
{
    mmio_write(ULPI_VIEWPORT, ULPI_RUN | ULPI_ADDR(reg));
    if (ulpi_wait() < 0) return -1;
    *out = (uint8_t)ULPI_DATA_READ(mmio_read(ULPI_VIEWPORT));
    return 0;
}

/* WHY THERE IS NO ULPI INIT SEQ ANY MORE (2026-08-06, from the REAL hoki
 * kernel, phy-msm-usb.c pulled locally): for phy-type 3 = QUSB_ULPI_PHY,
 * ulpi_read/ulpi_write REFUSE any register above 0x3F:
 *     if (motg->pdata->phy_type == QUSB_ULPI_PHY && reg > 0x3F) { skip }
 * so the DT's qcom,hsusb-otg-phy-init-seq registers (0x80..0x82) are NEVER
 * written through the viewport by the stock kernel; neither are ULPI_MISC_A
 * (0x96) nor the OTG comparator reg (0x88). What the kernel DOES do for this
 * PHY is a power-cycle through the PHY CSR window (msm_usb_phy_reset,
 * QUSB_ULPI_PHY case) — implemented below. Our previous viewport writes were
 * writes the stock stack never performs on this PHY. */

/* ULPI register numbers. Linux encodes read-modify-write as address aliases:
 * ULPI_SET(a) = a + 1 sets the given bits, ULPI_CLR(a) = a + 2 clears them.
 *
 * ULPI_MISC_A is the Qualcomm PHY's external-VBUS-validity control, and it is
 * the reason the first USB image never enumerated. This DT node carries
 *   qcom,dp-manual-pullup;
 *   qcom,hsusb-otg-otg-control = <2>   (PMIC)
 * i.e. the PHY does NOT sense VBUS itself -- session validity is delivered by
 * the PMIC through a driver we do not have. msm_otg covers exactly this case:
 *
 *   if (motg->pdata->dp_manual_pullup)
 *       ulpi_write(otg, ULPI_MISC_A_VBUSVLDEXT | ULPI_MISC_A_VBUSVLDEXTSEL,
 *                  ULPI_SET(ULPI_MISC_A));
 *
 * VBUSVLDEXTSEL switches the PHY to the EXTERNAL vbus-valid input, VBUSVLDEXT
 * drives that input true. Without both, the PHY believes no cable is present,
 * never pulls up D+, and the host sees nothing at all -- no lsusb entry, no
 * attach, which is precisely what happened. */
#define ULPI_MISC_A               0x96u
#define ULPI_SET(a)               ((a) + 1u)
#define ULPI_CLR(a)               ((a) + 2u)
#define ULPI_MISC_A_VBUSVLDEXT    (1u << 0)
#define ULPI_MISC_A_VBUSVLDEXTSEL (1u << 1)

/* Kept for compatibility with older call sites; on the QUSB PHY this write
 * is one the stock kernel cannot perform (reg 0x97 > 0x3F). No-op. */
int usb_phy_force_vbus_valid(void)
{
    return 0;
}

/* QUSB2 PHY power-on-reset, kernel-verbatim (msm_usb_phy_reset, QUSB case):
 *   1. assert + deassert GCC_QUSB2_PHY_BCR      (reset_control phy_reset)
 *   2. CSR + 0xB4 (QUSB2PHY_PORT_POWERDOWN) = 0x23   power down
 *   3. CSR + 0xC4 (QUSB2PHY_PORT_UTMI_CTRL2) = 0x00
 *   4. CSR + 0xB4 = 0x22                             power back up
 * The kernel would also replay pdata->phy_init_seq into the CSR window here,
 * but this device's seq offsets (0x73/0x3f/0x7f) are not 32-bit aligned so a
 * writel to them can never have executed on stock either — skipped. */
#define QUSB2PHY_PORT_POWERDOWN   0xB4u
#define QUSB2PHY_PORT_UTMI_CTRL2  0xC4u

void usb_phy_qusb_por(void)
{
    gcc_usb_qusb2_phy_reset();
    mmio_write(PLAT_USB_PHY_CSR + QUSB2PHY_PORT_POWERDOWN, 0x23u);
    mmio_write(PLAT_USB_PHY_CSR + QUSB2PHY_PORT_UTMI_CTRL2, 0x00u);
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(PLAT_USB_PHY_CSR + QUSB2PHY_PORT_POWERDOWN, 0x22u);
    timer_delay_ms(1);
}

/* Post-reset PHY liveness check only (regs 0x00/0x01 are legal for this PHY:
 * both are <= 0x3F). 0xFFFF or a timeout = PHY not talking. */
int usb_phy_init_seq(void)
{
    uint8_t vid_lo = 0xFFu, vid_hi = 0xFFu;
    int rc = usb_ulpi_read(0x00u, &vid_lo);
    (void)usb_ulpi_read(0x01u, &vid_hi);
    bdiag_puts("usb-phy: qusb por done, ulpi vid=");
    bdiag_puthex(((uint32_t)vid_hi << 8) | vid_lo);
    bdiag_puts(" rc="); bdiag_putdec((uint32_t)-rc);
    bdiag_puts("\n");
    return rc;
}

#endif /* PLAT_HAVE_USB_CDC */
