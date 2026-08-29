/* ticwatch_c2.h — Mobvoi TicWatch C2 / C2+ (skipjack), model WG12036.
 * Snapdragon Wear 2100, APQ8009W (msm8909w) — the SAME SoC as the Fossil Gen 4.
 *
 * That equivalence is the whole reason this port is short. Everything at SoC
 * tier — GIC-400, the 19.2 MHz architected timer, MDP3 + the 28nm DSI host,
 * GCC, SPMI, the A7 clock RCG and its CPR/SMPS voltage path — is the same
 * silicon at the same addresses, and is already written and proven on the
 * Gen 4. Those drivers guard on PLAT_SOC_MSM8909 (see platform.h) and pick this
 * watch up unchanged. What is genuinely NEW here is the parts list: a different
 * panel, an ITE touch controller instead of Raydium, and an external I2C fuel
 * gauge instead of the PM8916's internal VM-BMS.
 *
 * SOURCES, and how far to trust each:
 *   - sailfish-wearable/android_kernel_mobvoi_skipjack, branch
 *     android-msm-skipjack-3.18-pie-wear-dr — the vendor 3.18 wear kernel, the
 *     exact analogue of firefish's. arch/arm/boot/dts/msm8909w-skipjack/ is a
 *     skipjack-specific DT directory, and it builds exactly ONE board:
 *     apq8009w-skipjack-wtp.dtb.
 *   - Values marked CONFIRMED-SOC are msm8909w facts, already verified on the
 *     Gen 4 against a real dumped DTB. They are safe.
 *   - Values marked FROM-DTB come from the watch's ACTUAL device tree. The C2
 *     is a 3.18-era appended-DTB device (no dtbo partition, no A/B — confirmed
 *     from its own by-name listing), so there is no blob to pull off the eMMC
 *     even with root. Instead the kernel tree's single skipjack board file was
 *     preprocessed and compiled here with dtc, and the result decompiled to a
 *     merged tree: fossil-port/dumps/c2-skipjack-fromsource/skipjack.dts.
 *     `ro.hardware`/`ro.product.board` read `skipjack` off the real watch and
 *     `ro.boot.bootdevice` reads `7824900.sdhci`, so the tree that was built
 *     is the tree this watch boots. Reading the MERGED output matters: several
 *     values below contradict the individual .dtsi files, because the board
 *     overlay disables what the reference dtsi enables.
 *   - Values marked FROM-DT come from a .dtsi in the kernel tree but were NOT
 *     confirmed in the merged output. The Gen 4 taught this
 *     lesson expensively: its panel was "390x390" in every community source
 *     and the shipped DTB said 454x454. Treat anything FROM-DT as a hypothesis
 *     until the device's own DTB confirms it.
 *   - Values marked TODO(dtb) are not known at all yet.
 *
 * Fleet note: the TicWatch S2/E2 are codename `tunny`, NOT skipjack — a
 * separate kernel tree and a separate board header when that watch arrives.
 * They are the same Wear 2100 family, so they will inherit PLAT_SOC_MSM8909
 * the same way this one does. */
#pragma once

#define PLAT_NAME           "ticwatch-c2"     /* skipjack, WG12036 */

/* ---- DDR ------------------------------------------------------------------
 * CONFIRMED-SOC base. 512 MB on this watch. apq8009w-skipjack-memory.dtsi puts
 * its lowest reserved carve-out at 0x87b00000, so the 0x87A00000 ceiling the
 * Gen 4 uses is valid here too and leaves a 1 MB margin below the first
 * reservation. Keeping the two watches on the same number is deliberate: one
 * less thing to differ while the port is unproven. */
#define PLAT_DDR_BASE       0x80000000u
#define PLAT_DDR_SIZE       (512u * 1024u * 1024u)
#define PLAT_LINK_BASE      0x80008000u
#define PLAT_DDR_SAFE_END   0x87A00000u

/* ---- Core SoC blocks — all CONFIRMED-SOC (identical to the Gen 4) --------- */
#define PLAT_GICD_BASE      0x0B000000u
#define PLAT_GICC_BASE      0x0B002000u
#define PLAT_TIMER_HZ       19200000u
#define PLAT_GCC_BASE       0x01800000u

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

/* ---- Debug UART: BLSP1 UART2, serial@78b0000 ------------------------------
 * DIFFERENT FROM THE GEN 4, and possibly better. firefish's DT marks its UART
 * (uart1 @0x78af000) `status = "disabled"`, which is why PLAT_UART_DISABLED
 * exists there — touching a clock-gated MSM peripheral does not error, it hangs
 * the AHB transaction and kills the boot. skipjack's apq8009w-skipjack-wtp.dts
 * instead marks blsp1_uart2 `status = "ok"` with a console pinctrl, so this
 * watch may actually have a usable serial console.
 *
 * It stays DISABLED anyway for first boot. "The DT enables it" is not the same
 * claim as "aboot leaves its clocks running when it hands off to us", and the
 * Gen 6 already paid for that exact conflation once. Bring it up deliberately
 * later — clear this flag, and if the watch then dies before first pixel, the
 * UART clock is the answer. The ramlog holds every con_puts() until then. */
#define PLAT_UART_TYPE_MSM  1
#define PLAT_UART_BASE      0x078B0000u   /* FROM-DT: blsp1_uart2 */
#define PLAT_UART_DISABLED  1

/* Inherited from the Gen 4 for the same reason it exists there: on the Gen 6 a
 * first write to the PMIC's RTC_CTRL was an instant hard reset, because that
 * peripheral belongs to the secure world on production units. The C2 carries a
 * PM8916, same as the Gen 4. Reads stay on; they fail clean. */
#define PLAT_RTC_WRITE_DISABLED  1

/* ---- Display: MDP3 + MIPI-DSI. CONFIRMED-SOC bases ------------------------ */
#define PLAT_MDP_BASE        0x01A00000u
#define PLAT_MDP_VBIF_BASE   0x01AB0000u
#define PLAT_DSI_CTRL_BASE   0x01AC8000u
#define PLAT_DSI_PHY_BASE    0x01AC8500u
#define PLAT_DSI_PHY_REG_BASE 0x01AC8780u
#define PLAT_MMSS_MISC_BASE  0x0193E000u
#define PLAT_DSI_IRQ         (32u + 80u)
#define PLAT_MDP_IRQ         (32u + 72u)

/* ---- Panel: EDO E1392AMC AMOLED, command mode, 400x400 -------------------
 * FROM-DT, and no longer a guess. msm8909w-skipjack.dtsi:112 selects it
 * outright:
 *     &mdss_dsi0 { qcom,dsi-pref-prim-pan = <&dsi_edo_e1392amc_amoled_cmd>; }
 * and skipjack-dsi-panel-edo-e1392amc-amoled-cmd.dtsi gives:
 *     panel-name    "EDO E1392AMC AMOLED command mode dsi panel"
 *     panel-type    dsi_cmd_mode        framerate 60
 *     width 400     height 400          bpp 24
 *     one data lane (only mdss-dsi-lane-0-state is present)
 *     TE pin used   (te-using-te-pin, te-pin-select 1)
 *
 * 400, NOT the 360x360 every C2 spec sheet prints. This is the same trap the
 * Gen 4 set — its panel was "390x390" in every community source and 454x454 in
 * the shipped DTB — and it is why the placeholder that used to sit here was
 * marked as one rather than trusted. 360 is presumably the marketing figure
 * for the visible circle inside a 400x400 addressable panel.
 *
 * Still worth confirming against this watch's own DTB when TWRP is running:
 * this comes from the kernel tree, and a vendor is free to ship a different
 * panel than its own default. But fb_mdp3.c auto-detects geometry from what
 * aboot programmed into DMA_P, so a wrong value here costs nothing at boot. */
/* CONFIRMED ON HARDWARE 2026-08-29: 360x360, which is what aboot programs into
 * DMA_P and what the glass actually is. The panel node above says 400x400 and
 * is WRONG for this unit — either Mobvoi fitted a different EDO part than the
 * dtsi describes, or the node is a family default nobody corrected. Both the
 * spec sheets and the touch controller said 360 all along.
 *
 * The lesson is the one fb_mdp3.c now encodes: the bootloader has a working
 * configuration in front of it and a device tree is only a description of one.
 * Where they disagree, believe the hardware. These constants are a fallback
 * for an implausible probe, nothing more. */
#define PLAT_PANEL_W        360u
#define PLAT_PANEL_H        360u
#define PLAT_SCREEN_ROUND   1

/* Brightness: "bl_ctrl_dcs", min level 1, max 255 — the SAME scheme as the
 * Gen 4's AUO h139. dsi_dcs_set_brightness() (DCS 0x51 over the msm8909w DSI
 * host) therefore applies unchanged, including the auto-dim path. One of the
 * larger pieces of the Gen 4 bring-up that this watch simply inherits. */
#define PLAT_BL_MIN_LEVEL   1u
#define PLAT_BL_MAX_LEVEL   255u

/* ---- Touch: FocalTech FTS at 0x38 — NOT the ITE, and not Raydium ---------
 * FROM-DTB, and this is where reading the merged tree paid for itself. The
 * QUP5 bus carries THREE candidate touch nodes, and only one of them is live:
 *
 *     synaptics@20   status = "disabled"
 *     it7260@46      status = "disabled"
 *     focaltech@38   NO status property  ->  enabled by default
 *
 * An earlier pass here read the reference .dts, saw the ITE node, and recorded
 * "ITE IT7260 @ 0x46" as the strong hypothesis. It was wrong. A node's absence
 * of `status` is what enables it, so the disabled ones are the loud ones and
 * the live one is silent — which is exactly the sort of thing a merged tree
 * settles and a per-file read does not.
 *
 * FocalTech FTS is a well-known controller family with a simple I2C register
 * interface (a touch-data block starting at register 0x00, one byte of touch
 * count, then six bytes per point), and it is far simpler than the Raydium
 * PDA2 protocol the Gen 4 needed. It reports up to 2 points.
 *
 * COORDINATE SPACE — the one thing to get right. The touch node reports
 * 360x360 while the panel is 400x400 (see above). Those disagree, so the
 * driver must scale (x * 400 / 360) rather than pass raw counts through, or
 * every touch lands short of where the user pressed and the error grows toward
 * the edge of the dial. Confirm the direction of the scaling on hardware
 * before trusting it; 360 may instead be the usable circle inside a 400 panel,
 * in which case the mapping is an offset rather than a scale. */
#define PLAT_I2C_TOUCH_BASE 0x078B9000u  /* FROM-DTB: BLSP1 QUP5, qup_irq 0x63 */
#define PLAT_I2C_TOUCH_IRQ  (32u + 99u)
#define PLAT_I2C_CORE_HZ    19200000u    /* clk-freq-in  0x124f800 */
#define PLAT_I2C_BUS_HZ     100000u      /* clk-freq-out 0x186a0 = 100 kHz.
                                          * NOTE: slower than the Gen 4's
                                          * 400 kHz. Believe the DTB. */
#define PLAT_TOUCH_I2C_ADDR   0x38u      /* FROM-DTB: focaltech@38 */
#define PLAT_TOUCH_RESET_GPIO 12u        /* FROM-DTB: focaltech,reset-gpio */
#define PLAT_TOUCH_IRQ_GPIO   13u        /* FROM-DTB: focaltech,irq-gpio */
#define PLAT_TOUCH_MAX_PTS    2u         /* focaltech,num-max-touches */
#define PLAT_TOUCH_COORD_W    360u       /* focaltech,display-coords 0x168 */
#define PLAT_TOUCH_COORD_H    360u
/* focaltech,hard-reset-delay-ms 20, soft-reset-delay-ms 300 */
#define PLAT_TOUCH_RESET_MS   20u
#define PLAT_TOUCH_BOOT_MS    300u

/* ---- Battery: PM8916 VM-BMS — the SAME driver the Gen 4 already has -------
 * FROM-DTB, and the second correction the merged tree forced. The generic MTP
 * dtsi carries an ST STC3117 coulomb gauge and an SMB231 charger on QUP3, and
 * an earlier pass here recorded those as this watch's battery path. They are
 * not enabled. What is live is:
 *
 *     qcom,vmbms { compatible = "qcom,qpnp-vm-bms"; status = "ok"; }
 *     qcom,vm-bms@4000
 *
 * That is voltage-mode BMS on the PM8916 — bit for bit the same block
 * pmic_fg.c already drives on the Fossil Gen 4, including the VBATT_MUL_FACTOR
 * of 3 that cost a debugging session to find there. So the battery path is
 * inherited whole rather than written. Worse data than a coulomb counter (no
 * current sense, so state-of-charge comes from an OCV lookup) but zero new
 * code, which for a watch whose display is not yet lit is the better trade. */
#define PLAT_BATT_CUTOFF_UV   3400000u   /* qcom,v-cutoff-uv    0x33e140 */
#define PLAT_BATT_MAX_UV      4200000u   /* qcom,max-voltage-uv 0x401640 */

/* ---- Buttons: exactly one ------------------------------------------------
 * FROM-DTB: gpio_keys/stem_1, label "STEM_1", gpios = <&tlmm 91 GPIO_ACTIVE_LOW>,
 * wakeup-capable, 15 ms debounce. A single pusher — no second button and no
 * rotating crown, which settles the question left open below.
 * digitalRead() of the virtual BOOT_BTN_GPIO maps here. */
#define PLAT_BTN_STEM1_GPIO   91u
#define PLAT_BTN_ACTIVE_LOW   1

/* ---- Crown / rotation: NONE ----------------------------------------------
 * RESOLVED, FROM-DTB. The QTI wear reference boards put a PixArt optical
 * rotation sensor at 0x75 on the touch bus (the Gen 4 has a PAT9126 there),
 * and this watch does not: no pixart node survives into the merged tree, and
 * gpio_keys carries a single non-rotating pusher. So there is no encoder to
 * map to LVGL and nothing to port. The C2's bezel is decorative.
 *
 * ---- Vibrator -------------------------------------------------------------
 * FROM-DTB: qcom,vibrator@c000, "qcom,qpnp-vibrator", status okay,
 * qcom,vib-vtg-level-mV = 0xc1c = 3100 — IDENTICAL to the Gen 4, so
 * pmic_vib.c's default PLAT_VIB_VTG_MV of 3100 is right and no override is
 * needed here.
 *
 * ---- aboot's splash framebuffer -------------------------------------------
 * FROM-DTB: splash_region@83000000, reg = <0x83000000 0xc00000> (12 MB).
 * This is where the bootloader's framebuffer lives, and fb_mdp3.c's takeover
 * path reads DMA_P_IBUF_ADDR expecting to find a pointer INTO this region.
 * Two consequences: the address is a sanity check on whether the probe read a
 * believable config, and the heap must not be allowed to allocate over it
 * while the takeover framebuffer is still the live scanout. */
#define PLAT_SPLASH_BASE    0x83000000u
#define PLAT_SPLASH_SIZE    0x00C00000u
