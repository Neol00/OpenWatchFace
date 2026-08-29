/* fossil_gen4.h — Fossil Gen 4 (firefish/ray), Snapdragon Wear 2100 (msm8909w/APQ8009w).
 * Addresses are sourced from the firefish kernel branch / its DT, and — as of
 * 2026-07-28 — CONFIRMED against the stock DTB dumped from a real DW6F1
 * (Q Explorist HR, 44mm = firefish). See ../../HARDWARE.md.
 *
 * Fleet note: one header per device. A future Gen 6 (hoki, Wear 4100+, ARM64)
 * gets its own header AND its own startup (AArch64) — only the driver model
 * and the platform/ API are shared.
 *
 * Sub-variant: firefish (44mm) and ray (40mm) are the SAME SoC and share every
 * driver, so PLAT_BOARD_FOSSIL_GEN4 stays the single SoC-level guard used by all
 * platform/*.c. They differ only in per-size board data (panel geometry), which
 * is selected below by PLAT_FOSSIL_VARIANT_*. Default is firefish (the unit whose
 * DTB is confirmed); ray values are TODO until that watch's DTB is dumped. */
#pragma once

/* Pick the size variant. Set by the build (build.sh gen4-firefish|gen4-ray);
 * default to the DTB-confirmed firefish if the build didn't specify. */
#if !defined(PLAT_FOSSIL_VARIANT_FIREFISH) && !defined(PLAT_FOSSIL_VARIANT_RAY)
#define PLAT_FOSSIL_VARIANT_FIREFISH 1
#endif

#if defined(PLAT_FOSSIL_VARIANT_FIREFISH)
#define PLAT_NAME           "fossil-firefish"   /* Gen 4, 44mm */
#elif defined(PLAT_FOSSIL_VARIANT_RAY)
#define PLAT_NAME           "fossil-ray"        /* Gen 4, 40mm */
#endif

/* DDR: msm8909 RAM base. 512 MB on this watch (not the 1gb DT variants). */
#define PLAT_DDR_BASE       0x80000000u
#define PLAT_DDR_SIZE       (512u * 1024u * 1024u)
/* Link/run address: DDR base + 32 KB, mirroring the boot.img kernel_offset
 * convention (base 0x0 + kernel_offset 0x8000 is applied by aboot relative to
 * its own DDR base on msm8909). startup.S self-relocates here from wherever
 * aboot actually dropped us, so this only needs to be a safe final home. */
#define PLAT_LINK_BASE      0x80008000u

/* Reserved regions to NEVER touch (from msm8909 reserved-memory + wear DTs):
 * 0x87A00000..0x88000000 external_image/modem-adjacent region. Stay below. */
#define PLAT_DDR_SAFE_END   0x87A00000u

/* GIC-400 (qcom,msm-qgic2) — msm8909.dtsi interrupt-controller@b000000 */
#define PLAT_GICD_BASE      0x0B000000u
#define PLAT_GICC_BASE      0x0B002000u

/* ARM architected timer: 19.2 MHz XO (msm8909.dtsi arm,armv7-timer) */
#define PLAT_TIMER_HZ       19200000u

/* Debug UART: BLSP1 UART1, UARTDM v1.4 ("qcom,msm-lsuart-v14"), serial@78af000.
 * This is the stock cmdline's ttyHSL0 / earlycon. Physical accessibility on the
 * watch unknown (HARDWARE.md open question #3); ramlog is the primary console.
 * NOTE: its core/iface clocks (gcc_blsp1_uart1_apps_clk) must already be running
 * (true when aboot used the console) — the skeleton does not own GCC yet. */
#define PLAT_UART_TYPE_MSM  1
#define PLAT_UART_BASE      0x078AF000u

/* ...AND IT IS DISABLED, for the reason the Gen 6 paid for first. Touching a
 * clock-gated MSM peripheral does not return an error: the AHB transaction
 * never completes and the boot dies there, which reads from outside as "the
 * image never ran". uart_msm.c's own comment used to assume "aboot leaves the
 * console clocked" — that assumption is exactly what cost the Gen 6 a boot,
 * and nothing here has PROVEN aboot leaves BLSP1 UART1 running on firefish.
 *
 * And we lose nothing by not touching it: HARDWARE.md open question #3 (is the
 * HSL UART bonded out to any reachable pad?) is still open, so even a working
 * UART has no listener. The ramlog keeps every con_puts(). Delete this line
 * once a pad is found AND gcc owns the BLSP1 UART clocks. */
#define PLAT_UART_DISABLED  1

/* PMIC RTC WRITES DISABLED — inherited, unproven-here, and cheap.
 * On the Gen 6 the first-ever write to the PM660's RTC_CTRL was an instant
 * hard reset into Wear OS: on production units that peripheral is owned by the
 * secure world, and a non-secure SPMI write to it is an arbiter ownership
 * violation. The .ino reaches that write on any boot where the RTC reads back
 * unset, which is every boot here. The Gen 4's PM8916 is a different PMIC, so
 * this is NOT proven on this watch — but the failure it prevents is
 * unrecoverable-looking and the cost of keeping it is one wrong wall clock.
 * READS stay on (observer path, fails clean). Remove after proving ownership
 * from firefish's own DT. */
#define PLAT_RTC_WRITE_DISABLED  1

/* ---------------------------------------------------------------------------
 * Display: MDP3 + MIPI-DSI (MDSS). Bases from msm8909-mdss.dtsi (see
 * HARDWARE.md); the four DSI ranges are exactly the DT's `ranges` property.
 *   mdp_phys          0x1a00000 len 0x100000
 *   vbif_phys         0x1ab0000 len 0x3000
 *   dsi_ctrl          0x1ac8000 len 0x25c
 *   dsi_phy           0x1ac8500 len 0x280
 *   dsi_phy_regulator 0x1ac8780 len 0x30
 *   mmss_misc_phys    0x193e000 len 0x30
 * MDSS is a 28nm DSI PHY (mdss_dsi_28nm_phy_config in the vendor kernel). */
#define PLAT_MDP_BASE        0x01A00000u
#define PLAT_MDP_VBIF_BASE   0x01AB0000u
#define PLAT_DSI_CTRL_BASE   0x01AC8000u
#define PLAT_DSI_PHY_BASE    0x01AC8500u
#define PLAT_DSI_PHY_REG_BASE 0x01AC8780u
#define PLAT_MMSS_MISC_BASE  0x0193E000u
/* GCC (clock controller) — msm8909.dtsi clock-controller@1800000 */
#define PLAT_GCC_BASE        0x01800000u

/* ---- HS USB device controller ("qcom,hsusb-otg") --------------------------
 * FROM-DT, quoted verbatim from this watch's own device tree:
 *   usb@78d9000  reg = <0x78d9000 0x400  0x6c000 0x200>
 *                reg-names = "core", "phy_csr"
 *                qcom,hsusb-otg-phy-type = <3>   (QUSB_ULPI_PHY)
 *                qcom,hsusb-otg-mode     = <1>   (peripheral)
 *                qcom,hsusb-otg-otg-control = <2> (PMIC)
 *                qcom,dp-manual-pullup
 * That node is IDENTICAL to the Gen 6's usb@78db000 in every field that the
 * driver reads -- same PHY type, same PHY CSR window at 0x6c000, same PMIC
 * OTG control, same manual D+ pullup. Only the core base differs (0x78d9000
 * here vs 0x78db000 on the SDA429W), so usb_ci.c / usb_phy_msm.c / gcc_usb.c
 * are shared verbatim and parameterised on these two addresses.
 *
 * (The old gen4_stubs.c comment claimed the msm8909w had a plain ULPI SNPS
 * PHY and that the Gen 6 stack was therefore a port rather than a recompile.
 * The device tree says otherwise: phy-type 3 and a phy_csr reg + phy_csr_clk
 * are exactly the QUSB2 signature. The stub was a guess; this is the DT.)
 *
 * The capability block sits at core+0x100 and the operational block at +0x140
 * (CAPLENGTH 0x40) -- usb_ci.c reads CAPLENGTH at runtime rather than trusting
 * that, and refuses to touch the controller if it does not read back sanely. */
#define PLAT_USB_BASE        0x078D9000u
#define PLAT_USB_PHY_CSR     0x0006C000u
#define PLAT_HAVE_USB_CDC    1
/* DSI controller IRQ: DT `interrupts = <0 80 0>` → SPI 80 → GIC ID 32+80 */
#define PLAT_DSI_IRQ         (32u + 80u)
/* MDP IRQ: DT `interrupts = <0 72 0>` */
#define PLAT_MDP_IRQ         (32u + 72u)

/* Panel geometry — CONFIRMED from the stock DTB (2026-07-28).
 * The active panel is an AUO AMOLED command-mode DSI (pref-prim-pan phandle
 * 0x9b), and the Raydium touch node reports display-coords 0x1c6 = 454, so the
 * firefish panel is 454x454 round — NOT the 390x390 earlier notes assumed.
 * ray (40mm) shares the SoC but may use a smaller/different panel: dump that
 * unit's DTB before trusting these numbers on it. */
#if defined(PLAT_FOSSIL_VARIANT_FIREFISH)
#define PLAT_PANEL_W        454u
#define PLAT_PANEL_H        454u
#elif defined(PLAT_FOSSIL_VARIANT_RAY)
/* TODO(ray/40mm): confirm from a ray DTB dump. Provisionally mirror firefish so
 * the build links; the Raydium display-coords in the ray DT is the source of
 * truth once available. */
#define PLAT_PANEL_W        454u
#define PLAT_PANEL_H        454u
#endif
#define PLAT_SCREEN_ROUND   1

/* ---------------------------------------------------------------------------
 * I2C: BLSP1 QUP masters ("qcom,i2c-msm-v2"). Bases from msm8909.dtsi; the
 * touch controller sits on QUP5 per the wear reference DT (apq8009w-swoctp).
 * CONFIRM Fossil's actual QUP index + touch address from the stock DTB
 * (HARDWARE.md open question #2) — the reference board is only a template.
 *   BLSP1 QUP0 0x78b5000  QUP1 0x78b6000 (NFC on refs)  QUP2 0x78b7000
 *   BLSP1 QUP3 0x78b8000  QUP4 0x78b9000 (touch on refs) QUP5 0x78ba000
 * NOTE the DT names them i2c_N by QUP index; HARDWARE.md records the touch bus
 * as "QUP5 @ 0x78b9000" from the reference dtsi — the ADDRESS is the reliable
 * half of that, so it is what we use. */
/* CONFIRMED from stock DTB (2026-07-28): raydium touch sits under i2c@78b9000
 * (reg = <0x78b9000 0x1000>, qup_irq SPI 0x63=99). The reference-board guess
 * above happened to be exactly right, so this address is now VERIFIED. */
#define PLAT_I2C_TOUCH_BASE 0x078B9000u
#define PLAT_I2C_TOUCH_IRQ  (32u + 99u)  /* DT interrupts = <0 0x63 0> */
/* QUP core clock: BLSP QUP apps clk runs at 19.2 MHz on msm8909 for I2C.
 * DTB confirms clk-freq-out 0x186a0=100000, clk-freq-in 0x124f800=19.2M. */
#define PLAT_I2C_CORE_HZ    19200000u
#define PLAT_I2C_BUS_HZ     400000u      /* fast mode; raydium supports it */

/* Raydium RM_TS touch controller (CONFIG_TOUCHSCREEN_RM_TS).
 * CONFIRMED from stock DTB: raydium@39, reg = <0x39>. GPIOs on TLMM (phandle
 * 0xa0): reset = GPIO 12 (0x0c), IRQ = GPIO 13 (0x0d). num-max-touches 2. */
#define PLAT_TOUCH_I2C_ADDR 0x39u
#define PLAT_TOUCH_RESET_GPIO 12u
#define PLAT_TOUCH_IRQ_GPIO   13u

/* PixArt PAT9126 optical rotation sensor = the CROWN, on the SAME i2c@78b9000
 * bus at 0x75 (DTB: pixart_pat9126@75, IRQ GPIO 27=0x1b). This is an OPTICAL
 * motion sensor read over I2C, NOT a quadrature GPIO encoder, so the Phase 4
 * crown->LVGL-encoder mapping needs a pat9126 driver. Not yet ported. */
#define PLAT_CROWN_I2C_ADDR   0x75u
#define PLAT_CROWN_IRQ_GPIO   27u
/* AXIS: X, MEASURED -- not the vendor driver's answer.
 *
 * pixart_pat9126.c reports the wheel from the Y delta, so Y was the default,
 * and on this watch that is simply wrong. Captured over the USB console on
 * 2026-08-29 while rolling the crown down at speed (CROWN_DIAG build):
 *   crown: st=0x84 x=1 y=0 hi=0x00   [x38 suppressed]
 *   crown: st=0x84 x=2 y=0 hi=0x00
 *   crown: st=0x84 x=0 y=1 hi=0x00
 * X moves on every motion event; Y is occasional noise off the same surface.
 * Idle reads st=0x04 with both deltas 0, so there is nothing to filter out.
 *
 * The same capture fixes the sign: rolling DOWN produced POSITIVE X, which is
 * the direction crown_nav.h already treats as "down" (pull the shade down,
 * scroll the list downward). Hence no PLAT_CROWN_INVERT here.
 *
 * Magnitude, for whoever tunes CROWN_SCROLL_PX_PER_CNT next: registers 0x0D/
 * 0x0E (CPI X/Y) read 0x14 = 20 on this part, which is a LOW resolution
 * setting -- hence 1-2 counts per event rather than tens. A fast roll produced
 * roughly 40 counts. */
#define PLAT_CROWN_AXIS_X     1
