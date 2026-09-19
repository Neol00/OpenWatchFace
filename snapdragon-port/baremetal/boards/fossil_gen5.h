/* fossil_gen5.h — Fossil Gen 5 "Carlyle HR" (triggerfish), Snapdragon Wear 3100.
 *
 * EVERY VALUE HERE IS FROM THIS WATCH'S OWN DTB, dumped 2026-09-10 from the
 * stock boot partition of serial C3F9453E4746 and kept at
 * ../../dtbs/triggerfish-stock.dts. The recovery partition carries a
 * byte-identical copy of that tree, which is what makes it trustworthy rather
 * than merely plausible.
 *
 * WHAT THIS BOARD ACTUALLY IS. The Wear 3100 is the Wear 2100 AP (APQ8009W)
 * plus a QCC1110 always-on co-processor. The DTB's own model string says it:
 *     "Fossil Group, Inc. based on APQ8009W-PM660 BG Alpha SDW3100"
 * So the SoC half is firefish and the PMIC half is hoki. Concretely:
 *
 *   IDENTICAL to the Gen 4 (byte-for-byte nodes): usb@78d9000 + phy_csr
 *   0x6c000, sdhci@7824900/7824000, smem@87d00000 with the same
 *   smem_targ_info_reg and APCS IPC, pronto@a21b000, tz-log@8600720,
 *   i2c@78b9000 carrying a raydium@39 touch controller, wcnss gpios 40..44.
 *
 *   FROM THE GEN 6 INSTEAD: a PM660 (not PM8916) on SPMI, so haptics are the
 *   qpnp-haptics WAVEFORM PLAYER at 0xc000, not the PM8916 single-enable-bit
 *   vibrator, and the WCNSS rails hang off PM660 regulators.
 *
 *   ITS OWN: a 416x416 panel (the Gen 4 is 454x454), an SMB1357 charger on
 *   I2C, no PixArt crown, and the bgapp/bgappbak partitions that hold the
 *   co-processor image.
 *
 * STATUS: board data only. Nothing here has been RUN on the watch yet — this
 * header is the transcription of a device tree, and every "CONFIRMED" below
 * means "confirmed in the DTB", never "observed working". */
#pragma once

#define PLAT_NAME           "fossil-triggerfish"   /* Gen 5, Carlyle HR, 44mm */
/* This board has a rotating crown on the co-processor's RSB_CTRL channel. The
 * bring-up is opt-in (bgcom.c): a board that does not declare this never runs
 * it. boards/fossil_darter.h inherits it by including this header; the Gen 5E
 * includes this header too but has only a pusher, so it sets
 * PLAT_NO_ROTARY_CROWN, which overrides this. */
#define PLAT_HAS_ROTARY_CROWN 1

/* DDR: 1 GB (MemTotal 948404 kB from the running watch; the DTB's memory node
 * is <0 0 0 0>, filled in by the bootloader, so the device is the source). */
#define PLAT_DDR_BASE       0x80000000u
#define PLAT_DDR_SIZE       (1024u * 1024u * 1024u)
#define PLAT_LINK_BASE      0x80008000u
/* First reserved region: external_image__region@0 reg <0x87a00000 0x600000>.
 * Same address as the Gen 4, so the same ceiling applies. */
#define PLAT_DDR_SAFE_END   0x87A00000u

/* GIC-400 and the 19.2 MHz XO architected timer: msm8909, unchanged. */
#define PLAT_GICD_BASE      0x0B000000u
#define PLAT_GICC_BASE      0x0B002000u
#define PLAT_TIMER_HZ       19200000u

/* Debug UART left DISABLED for the same reason as the Gen 4: nothing has
 * proven aboot leaves the BLSP1 UART clocks running, and touching a
 * clock-gated MSM peripheral does not fail politely — the AHB transaction
 * never completes and the boot dies looking like "the image never ran".
 * The ramlog keeps every con_puts(). */
#define PLAT_UART_TYPE_MSM  1
#define PLAT_UART_BASE      0x078AF000u
#define PLAT_UART_DISABLED  1

/* PMIC RTC WRITES DISABLED. On the Gen 6 the first-ever write to the PM660's
 * RTC_CTRL was an instant hard reset: on production units that peripheral is
 * owned by the secure world and a non-secure SPMI write is an ownership
 * violation. This watch has the SAME PM660, so unlike the Gen 4 (where the
 * rule was inherited caution about a different PMIC) here it is the exact
 * silicon that produced the reset. Reads stay on. */
#define PLAT_RTC_WRITE_DISABLED  1

/* ---- Display: MDP3 + MIPI-DSI. Bases are msm8909 MDSS, same as the Gen 4. */
#define PLAT_MDP_BASE        0x01A00000u
#define PLAT_MDP_VBIF_BASE   0x01AB0000u
#define PLAT_DSI_CTRL_BASE   0x01AC8000u
#define PLAT_DSI_PHY_BASE    0x01AC8500u
#define PLAT_DSI_PHY_REG_BASE 0x01AC8780u
#define PLAT_MMSS_MISC_BASE  0x0193E000u
#define PLAT_GCC_BASE        0x01800000u
#define PLAT_DSI_IRQ         (32u + 80u)
#define PLAT_MDP_IRQ         (32u + 72u)

/* Panel: qcom,mdss_dsi_auo_416p_amoled_cmd — "AUO 416p AMOLED command mode
 * dsi panel", 416x416 (0x1a0), 45 fps, 24 bpp, rgb_swap_rgb, tight packing.
 * The Gen 4's AUO panel is the same family but 454x454, so the existing
 * panel-auo-416p-amoled-cmd.dts init sequence is the closer starting point.
 * The raydium touch node's display-coords agree at 0x1a0, which is the
 * independent check that this is the fitted panel and not a leftover node. */
#define PLAT_PANEL_W        416u
#define PLAT_PANEL_H        416u
#define PLAT_SCREEN_ROUND   1
/* Panel control lines, FROM-DTB (qcom,mdss_dsi_ctrl0@1ac8000):
 *   platform-te-gpio        TLMM 24 (0x18)  — tear-effect in, command mode
 *   platform-reset-gpio     TLMM 25 (0x19)
 *   platform-enable-gpio    TLMM 59 (0x3b)
 *   platform-tsresout-gpio  TLMM 16 (0x10)  — same pin as the touch reset
 *   platform-avdden-gpio    pin 12 on a DIFFERENT controller (phandle 0x93,
 *                           i.e. the PMIC's gpio block, not TLMM) — so the
 *                           panel's AVDD enable is a PM660 GPIO and cannot be
 *                           driven through the TLMM path the other four use.
 * DSI supplies: vdd/vddio via the dsi_pm660_panel_pwr_supply node. */
#define PLAT_PANEL_TE_GPIO      24u
#define PLAT_PANEL_RESET_GPIO   25u
#define PLAT_PANEL_ENABLE_GPIO  59u
#define PLAT_PANEL_TSRESOUT_GPIO 16u
#define PLAT_PANEL_AVDDEN_PMIC_GPIO 12u

/* ---- USB: node is IDENTICAL to the Gen 4's, field for field. */
#define PLAT_USB_BASE        0x078D9000u
#define PLAT_USB_PHY_CSR     0x0006C000u
#define PLAT_HAVE_USB_CDC    1
/* USB PHY rails, FROM-DTB usb@78d9000 on triggerfish: hsusb_vdd_dig pm660_l5
 * at 1.2 V (qcom,vdd-voltage-level = <0 0x124f80 0x124f80>), HSUSB_1p8
 * pm660_l12, HSUSB_3p3 pm660_l16. The Gen 4 uses 8916_l2/l7/l13 for the same
 * three, which is why these are board data rather than constants in usb_ci.c.
 * Voted before the controller is touched; see the note there. */
#define PLAT_USB_RAIL_DIG_LDO   5u
#define PLAT_USB_RAIL_DIG_UV    1200000u
#define PLAT_USB_RAIL_1P8_LDO   12u
#define PLAT_USB_RAIL_3P3_LDO   16u

/* ---- SMEM / RPM / IPC: identical to the Gen 4. */
#define PLAT_SMEM_BASE       0x87D00000u
#define PLAT_SMEM_SIZE       0x00100000u
#define PLAT_SMEM_AUX_BASE   0x00060000u
#define PLAT_SMEM_AUX_SIZE   0x00008000u
#define PLAT_APCS_IPC        0x0B011008u
#define PLAT_SMEM_TARG_INFO_REG 0x0193D000u

/* ---- WCNSS / Pronto (WiFi + BT).
 * The firmware region MOVED and GREW vs the Gen 4:
 *     Gen 4  pheripheral_region@0  0x8A100000 + 0x500000
 *     Gen 5  pheripheral_region@0  0x8A300000 + 0x600000
 * Everything else matches: pas-id 6, pronto@a21b000, wlan gpios 40..44 on
 * TLMM function 1, tz-log at 0x8600720 with the header at that address. */
#define PLAT_WCNSS_FW_BASE    0x8A300000u
#define PLAT_WCNSS_FW_SIZE    0x00600000u
/* reserved-memory `no-map` regions, FROM-DTB (triggerfish-stock.dts). mmu.c
 * maps these Device + XN so nothing is cached and the prefetcher cannot wander
 * into XPU-protected memory. They are NOT the Gen 4's: every one differs, and
 * the WCNSS region in particular extends 3 MB past the Gen 4's table. Sections
 * are 1 MB, inclusive: last = (base + size - 1) >> 20. */

/* Modem (MSS) proxy rails from the triggerfish stock DT mss node (2026-09-15): vdd_cx-supply =
 * pm660_s2_corner (smpa 2), vdd_mx-supply = pm660_s3_corner_ao (smpa 3), vdd_pll-supply =
 * pm660_l12 1.8 V (voted on explicitly; on PM8916 boards l7 is already up). */
#define PLAT_MSS_CX_TYPE 0x61706d73u   /* "smpa" */
#define PLAT_MSS_CX_ID   2u
#define PLAT_MSS_MX_TYPE 0x61706d73u   /* "smpa" */
#define PLAT_MSS_MX_ID   3u
/* v418: the vendor tree labels CX/MX on s2/s3 one way in the mss node and the other way in the
 * WCNSS node (our working WiFi code follows the latter). Until a rail is proven, hold BOTH at the
 * SUPER_TURBO corner (wire 6, legal on both: max corner 7) for the load; both are released to
 * NORMAL together on proxy-unvote. v416 voted s3 down to SVS during the load and died mid-copy. */
/* v424: power test. Every Gen 5 load died at the same point (~13 s in, when the MBA starts hashing
 * seg 12 with the Q6 at full tilt) with no fault record and no watchdog; v416 with lower votes got
 * two segments further. Hold both rails at the NORMAL corner (wire 4) for the load instead of
 * SUPER_TURBO: a PM660 UVLO/OCP reset would move or vanish, an XPU death would not. */
#define PLAT_MSS_MX_WIRE 6u   /* both rails SUPER_TURBO for the load (label ambiguity, see above) */
#define PLAT_MSS_PLL_LDO 12u
#define PLAT_MSS_PLL_UV  1800000u
#define PLAT_MMU_HOLES_8909 \
    { 0x87A, 0x87F },   /* external_image__region  0x87A00000 + 0x600000  */ \
    { 0x880, 0x8A2 },   /* modem_adsp_region       0x88000000 + 0x2300000 */ \
    { 0x8A3, 0x8A8 },   /* pheripheral_region      0x8A300000 + 0x600000  (WCNSS) */
#define PLAT_WCNSS_PAS_ID     6u
#define PLAT_PRONTO_PMU_BASE  0x0A21B000u
#define PLAT_WCNSS_GPIO_FIRST 40u
#define PLAT_WCNSS_GPIO_FUNC  1u
#define PLAT_TZLOG_PTR        0x08600720u
#define PLAT_TZLOG_SIZE       0x1000u
/* RAILS: PM660, and NOT the Gen 6's PM660 map either. Resolved from this
 * tree's own phandles (wcnss.c carries the table and the reasoning):
 *   pronto vddmx pm660_s2_corner_ao  vddcx pm660_s3_corner  vddpx pm660_l13
 *   iris   vddxo pm660_l12 1.8V      vddrfa pm660_l6 1.3V   vdddig pm660_l13
 *   pil proxy vdd_pronto_pll -> pm660_l12
 * Two traps this board sets for anyone porting by analogy: vddrfa is an LDO
 * here (the PM8916 watches put it on SMPS s3), and there is NO vddpa property
 * anywhere in this tree, so the PA rail is not AP-switchable.
 * The Gen 6 is also PM660 but wires vddcx to s1 and vddrfa to l5 — which is
 * why the symbol is board-distinct rather than "PM660". */
#define PLAT_WCNSS_RAILS_PM660_8909 1
/* WLAN bring-up, first attempt on this watch (2026-09-10). Everything the
 * Gen 4 needs was already transcribed -- SMEM, the Pronto PMU, the PAS id, the
 * WCNSS GPIO block and this board's own PM660 rail table -- and the one piece
 * that was genuinely missing, the NV blob, is now generated from this unit's
 * own /persist, as firmware/gen5-<serial>/wcnss_nv.c. The Gen 5 cannot borrow
 * the Gen 4's: the two blobs differ.
 *
 * The remaining unknowns are all things only hardware can answer: whether TZ
 * accepts the image through PAS (the Gen 6 is still stuck at pas_init_image
 * = -13), whether the rails as transcribed actually bring the core up, and
 * whether HAL_START completes with this NV. Turning the flag on is what makes
 * those questions askable -- with the caveat this comment used to carry, that
 * a scan which hangs is worse than a scan that is absent, so this stays a
 * test image until the radio answers once. */
#if !defined(NO_WLAN_APP)   /* chime58: -DNO_WLAN_APP builds without WiFi (audio bisect) */
#define PLAT_WLAN_APP 1
#endif
/* Collect received frames by DESCRIPTOR STATE, not only the raw interrupt bit
 * (2026-09-10, proven on hardware). This watch's Pronto firmware (CNSS.PR.4.0.4,
 * variant SCAQBWZM) fills the DXE RX rings but never sets DXE_INT_SRC_RAW
 * CH1/CH3, so gating the drain on that bit left every frame uncollected (scan:
 * RX frames 0 with 10/16 RX-high descriptors consumed). Also resyncs the ring
 * head when it drifts ahead of the engine (RX-high 15/16 consumed, channel
 * disabled, connects failing). The Gen 4's firmware raises the bit, so it keeps
 * the original poll. The feature-caps / ADD_STA_SELF / CH_SWITCH / SMSM steps
 * this define used to carry are REMOVED to test whether this firmware needs
 * them; if scan or connect regresses after v245, one of them was needed. */
#define PLAT_WCNSS_RX_DESC_POLL 1

/* ---- eMMC: sdhci node identical to the Gen 4's (hc 0x7824900 / core
 * 0x7824000, 8-bit). Pad drive/pull values are the same msm8909 SDC1 field
 * layout; re-confirm from this tree's sdc1_*_on before trusting on hardware. */
/* STORAGE ON (2026-09-10): the user is converting this watch, and
 * BOARD_HAS_FFAT is 1 in board_fossil_gen5.h, as on the Gen 4.
 *
 * The safety no longer comes from leaving FFat off. It comes from the
 * foreign-filesystem guard in storage_gen6.c: if userdata still holds ext4,
 * f2fs, or an encrypted Android volume (FDE footer 0xD0B5B1C4), storage goes
 * READ-ONLY for that boot -- no superblock, no blackbox, no NVS, no f_mkfs --
 * which is the "no storage" every Gen 5 boot so far has logged. eMMC READS are
 * therefore already proven on this watch (the guard reads userdata to decide).
 *
 * Converting is one explicit command, `fastboot erase userdata`; the next boot
 * then creates our superblock and formats the FFat region. Writes on this
 * watch are unproven until that boot. A full verified backup exists at
 * firmware/gen5-C3F9453E4746/. */
#define PLAT_HAVE_EMMC_STORAGE 1
#define PLAT_SDHC_HAS_CQE   0
#define PLAT_SDHC_HAS_ICE   0
#define PLAT_SDC1_PAD_CTL   0x0110A000u
#define PLAT_SDC1_PAD_VAL   (4u | (4u << 3) | (7u << 6) | (3u << 9) | (3u << 11) | (0u << 13))
#define PLAT_SDC1_PAD_MASK  0x7FFFu
/* Card VCC: FROM-DTB sdhc1 vdd-supply -> pm660_l19 at 2.9 V (0x2c4020), and
 * vdd-io -> pm660_l13 at 1.8 V (never touched). PM660 L19 is the SAME rail the
 * Gen 6 uses for its card, so the qpnp address is already derived there:
 * sid 1, base 0x4000 + 18*0x100 = 0x5200, EN_CTL 0x5246. sdhci_msm.c only
 * READS this and enables bit 7 if the handoff left it off — aboot just read
 * our image off this card, so normally it is a read and a log line. */
#define PLAT_EMMC_VDD_SID    1u
#define PLAT_EMMC_VDD_EN_CTL 0x5246u

/* ---- Touch: raydium@39 on i2c@78b9000, same controller and bus as the Gen 4,
 * DIFFERENT GPIOs — reset moved 12 -> 16, IRQ moved 13 -> 98.
 * DTB: raydium,reset-gpio = <tlmm 0x10 0>, raydium,irq-gpio = <tlmm 0x62 ...>,
 * interrupts = <0x62 0x2002>. num-max-touches 2, display-coords 416x416.
 * NOTE the bus runs at clk-freq-out 0x186a0 = 100 kHz in this tree (the two
 * NFC/charger buses are 0x61a80 = 400 kHz), so start at 100 kHz here. */
#define PLAT_I2C_TOUCH_BASE 0x078B9000u
#define PLAT_I2C_TOUCH_IRQ  (32u + 99u)   /* DT interrupts = <0 0x63 0> */
#define PLAT_I2C_CORE_HZ    19200000u
#define PLAT_I2C_BUS_HZ     100000u       /* FROM-DTB clk-freq-out */
#define PLAT_TOUCH_I2C_ADDR 0x39u
#define PLAT_TOUCH_RESET_GPIO 16u
#define PLAT_TOUCH_IRQ_GPIO   98u
/* Gate the touch poll on that INT pin instead of hitting the bus every time.
 * The on-watch load census (2026-09-10) attributed this watch's whole idle CPU
 * to the touch read -- touch 5%, spmi 0%, cache 0% -- and this bus runs at
 * 100 kHz here (FROM-DTB) against the Gen 4's 400 kHz, so each poll costs four
 * times what it does there. See touch_raydium.c for why the gate is only ever
 * a hint. */
#define PLAT_TOUCH_INT_GATED  1

/* ---- NO PIXART CROWN ON THIS WATCH.
 * The Gen 4 has pixart_pat9126@75 on the touch bus and crown_nav.h reads it as
 * an optical encoder. There is no pat9126 node anywhere in this tree. What the
 * running watch DOES expose is /dev/glink_pkt_bg_rsb_ctrl — "RSB" being the
 * Rotary Side Button — alongside glink_pkt_bg_display_ctrl and
 * glink_pkt_bg_sso_ctrl. So on the Wear 3100 the crown is owned by the QCC1110
 * co-processor and reached over GLINK, not by the AP over I2C.
 * Consequence: the crown is NOT available to this port until a GLINK client
 * for the BG exists. Deliberately no PLAT_CROWN_* here — a wrong I2C address
 * would just read a nonexistent device forever. */

/* ---- BG co-processor (QCC1110, "blackghost"), FROM-DTB.
 * qcom,bg-spi under spi@78B8000 (BLSP1 QUP4, qcom,shared_ee: TZ's bgapp uses
 * the same bus to load firmware), 16 MHz, mode 0. spi4_default pins
 * gpio12/13/15 function blsp_spi4, spi4_cs0_active gpio14. The QUP4 I2C node
 * on gpio14/15 (smb1357 charger) is disabled -- these pins belong to the BG.
 * qcom,pil-blackghost: bg2ap-status 97, bg2ap-errfatal 95, ap2bg-status 17,
 * ap2bg-errfatal 23. qcom,bg-daemon reset = PM660 GPIO5 (sid 0, 0xC400).
 * Rails: bg-rsb l11 1.8 V + l15 3.0 V, bg-daemon ssr l3 + l9. Firmware
 * bg-wear.mdt/b00-b02 on the modem FAT partition. */
#define PLAT_HAS_BG_QCC1110      1
#define PLAT_BG_SPI_MOSI         12u
#define PLAT_BG_SPI_MISO         13u
#define PLAT_BG_SPI_CS           14u
#define PLAT_BG_SPI_CLK          15u
#define PLAT_BG_IRQ_GPIO         110u
#define PLAT_BG2AP_STATUS_GPIO   97u
#define PLAT_BG2AP_ERRFATAL_GPIO 95u
#define PLAT_AP2BG_STATUS_GPIO   17u
#define PLAT_AP2BG_ERRFATAL_GPIO 23u
#define PLAT_BG_RESET_PM_GPIO    5u

/* ---- Buttons: gpio_keys has ONE entry, stem_1 on TLMM gpio 90 (0x5a),
 * active low, wakeup-capable, keycode 0x109. The other two stems are not in
 * this tree, which fits the RSB/co-processor story above. */
#define PLAT_KEY_STEM1_GPIO  90u

/* ---- PMIC: PM660 on SPMI, arbiter qcom,spmi@200f000 with chnls 0x2400000
 * (identical arbiter layout to the Gen 6). Two slave ids as usual:
 * qcom,pm660@0 (revid, pon, gpio, rtc, adc) and qcom,pm660@1 (regulators,
 * haptics). */
#define PLAT_SPMI_CHNLS_BASE 0x02400000u
#define PLAT_PMIC_SID        1u

/* ---- Haptics: qcom,pm660-haptics @ 0xc000 on sid 1 — the Gen 6's block, NOT
 * the Gen 4's. FROM-DTB: actuator-type "erm", vmax-mv 0xc80 = 3200,
 * ilim-ma 0x190 = 400, play-rate-us 0x2710. Same values the Gen 6 header
 * carries, so pmic_vib.c's PM660 branch applies unchanged. */
#define PLAT_VIB_PM660_HAPTICS 1
#define PLAT_HAP_BASE        0xC000u   /* qcom,haptics@c000, reg <0xc000 0x100> */
/* DRIVE LEVEL. Tuned on hardware to 3300 mV (2026-09-12, v297), down from the
 * block maximum of 3596 that v296 ran at: with braking on (pmic_vib.c) the full
 * 3596 was firmer than wanted, and the brake is what gives the buzz its crisp
 * edge, not the amplitude.
 *
 * QUANTIZED: the block's step is 116 mV, so this does not program 3300. The
 * encoder rounds to the nearest step, as the vendor driver does, and 3300 lands
 * on 28 steps = 3248 mV. The neighbouring settings, if this needs another
 * nudge, are 27 = 3132 mV and 29 = 3364 mV -- anything between 3190 and 3305
 * gives the same 3248, so move by at least a step to change anything.
 *
 * For reference: 0xc80 = 3200 mV is the node's qcom,vmax-mv default, and the
 * effects under the same node carry qcom,wf-vmax-mv = 0xe10 = 3600 (clamped to
 * 3596), so the usable band is bracketed by the vendor's own two values and
 * this sits inside it. The other feel knobs are PLAT_HAP_BRAKE_EN and
 * HAPTICS_CLICK_MS (OpenWatchFace/board_fossil_gen5.h). */
#define PLAT_HAP_VMAX_MV     3300u

/* ---- Charger: smb1357-charger@57 on i2c@78b8000 (BLSP1 QUP3, 400 kHz),
 * float-voltage-mv 0x1068 = 4200, charging-timeout 0x600, thermal-mitigation
 * <1500 700 600 0> mA. It is marked status = "disabled" in this tree while
 * qcom,qpnp-smb2 (the PM660's own charger block, over SPMI) is present — so
 * the SMB1357 is fitted but the STOCK SOFTWARE CHARGES OVER THE PMIC, not
 * over I2C. Do not assume the C2's SMB231 driver ports here; the register map
 * is a different part and the active path is probably not even I2C.
 * Recorded, not enabled. */
#define PLAT_I2C_CHG_BASE    0x078B8000u
#define PLAT_CHG_SMB1357_ADDR 0x57u

/* ---- NFC exists on this watch (nq@28 on i2c@78b5000, status okay) — Carlyle
 * HR ships Google Pay. Nothing in this port uses it; noted so nobody is
 * surprised by an active NXP part on that bus. */

/* THERE IS NO SOFTWARE ROUTE TO FASTBOOT ON THIS WATCH.
 *
 * Established by disassembling this unit's own aboot (a plain ARM ELF in the
 * aboot partition, firmware/gen5-C3F9453E4746/emmc/gen5-aboot.img):
 *
 *   0x77665500 / 0x77665501 / 0x77665502   0 occurrences
 *   0x0860065C  (IMEM restart_reason)      0 references
 *   0x0193D100  (TCSR boot-misc-detect)    0 references
 *
 * The LK restart-reason cookie that reboot_to_bootloader() sends -- and that
 * works on the Gen 4, the TicWatches and the Gen 6 -- simply does not exist in
 * triggerfish's bootloader. No value written to those registers can steer it,
 * which is why removing the TCSR/EDL write changed nothing.
 *
 * What this aboot DOES have is a Fossil-added entry path through the touch
 * controller and the stem key (target/msm8909/raydium_i2c_ts.c is compiled
 * into it, and it logs "[Raydium] Power key was pressed." and "[Raydium]
 * switch to fastboot mode via touch"). So fastboot is reached by HOLDING THE
 * STEM / TOUCHING THE GLASS during boot -- a hardware gesture, not a cookie.
 *
 * PLAT_REBOOT_USE_BCB is therefore kept only to CLEAR the misc BCB on the way
 * out, so a "boot-recovery" left behind by a power-cycled recovery session does
 * not keep hijacking every boot. It no longer tries to request fastboot. */
#define PLAT_REBOOT_USE_BCB 1

/* qcom,use-legacy-hard-reset-offset: triggerfish-stock.dts:4676 carries qcom,use-legacy-hard-reset-offset
 * (and the PON reads subtype 0x04 = GEN2_PRIMARY, so without this the reason
 * would be written one bit position low and aboot would never see it).
 * Selects where qpnp_pon_set_restart_reason() puts the restart reason in
 * SOFT_RB_SPARE -- see the long note in platform/reboot_msm.c. */
#define PLAT_PON_LEGACY_HARD_RESET_OFFSET 1

/* ---- Battery: QPNP FG-GEN3 fuel gauge + SMB2 charger on the PM660 ---------
 * FROM-DTB (triggerfish): qcom,fg-gen3 on pm660@0 with
 *   qcom,fg-batt-soc@4000   MONOTONIC_SOC at 0x4009 (shadow 0x400A)
 *   qcom,fg-batt-info@4100  BATT_TEMP 0x4150, VBATT 0x41A0, IBATT 0x41A2
 *   qcom,fg-memif@4400      (SRAM access, not used by this driver)
 * and qcom,qpnp-smb2 with qcom,chgr@1000 (BATTERY_CHARGER_STATUS_1 at 0x1006)
 * and qcom,usb-chgpth@1300 (USB INT_RT_STS at 0x1310, USBIN_PLUGIN bit 4).
 * All on SPMI slave id 0. See platform/pmic_fg.c. */
#define PLAT_FG_GEN3 1


/* ---- CPU core rail (VDD_APC) ---------------------------------------------
 * FROM-DTB: vdd-apc-supply -> pm660_s1, spm-regulator@1400 on qcom,pm660@1,
 * range 1052000..1352000 uV. NOT the Gen 4's 8916_s2 at 0x1700 -- writing that
 * address on a PM660 lands on an unrelated peripheral.
 *
 * REGULATOR GENERATION, confirmed by reading the part on the watch (2026-09-10):
 * TYPE 0x1C SUBTYPE 0x0A = FTS426, not the Gen 4's ULT HF buck. Its setpoint is
 * 12 bits across 0x40 (LSB) / 0x41 (MSB) and is expressed in MILLIVOLTS, where
 * the PM8916's is one byte of 12.5 mV steps from 375 mV. The live read settled
 * it: 0x0468 = 1128, which lands inside this rail's DT range as millivolts and
 * is 4.8 V under the other reading. Every DT bound here (1052, 1228, 1156 and
 * 1352 mV) is a multiple of the node's own qcom,cpr-apc-volt-step of 4000 uV,
 * which is exactly the FTS426's granularity.
 *
 * CPR TABLE, from this watch's own regulator@b018000. The fuse layout is
 * identical to the Gen 4's (row 0x1a, 6-bit fields at bits 0/18/36, 10 mV per
 * step, qcom,cpr-init-voltage-as-ceiling) -- only the ceilings and floors
 * differ, so those are all that the board has to supply. Note corner 1's floor
 * EQUALS its ceiling: at 200 and 400 MHz the voltage is fixed at 1052000 uV no
 * matter what the die's fuse says, which is the safest possible target. */
#define PLAT_APC_SID            1u
#define PLAT_APC_SPMI_BASE      0x1400u
#define PLAT_APC_FTS426         1
/* indexed by CPR fuse corner 1..3; index 0 unused (kernel is 1-based) */
#define PLAT_CPR_CEIL_UV        { 0, 1052000, 1228000, 1352000 }
#define PLAT_CPR_FLOOR_UV       { 0, 1052000, 1052000, 1156000 }

/* ---- Deep sleep (2026-09-10) ----------------------------------------------
 * The CPU0 SAW and the L2 gdhs/ret sequences are byte-identical to the Gen 4's,
 * and so are the MPM gic/gpio maps (triggerfish renames the node wake-gic@601d0
 * but the maps match firefish's). What differs is everything PMIC-facing:
 *
 * PLAT_SPM_L2_PM660: the L2 SAW pc sequence without the pmic-data 4/5 commands,
 * no data4/5 writes, and the FTS426 two-byte SAW voltage handshake
 * (qcom,vctl-port 0 + qcom,vctl-port-ub 1). See platform/spm_8909.c. */
#define PLAT_SPM_L2_PM660 1

/* ---- Heart rate: PixArt PAH8011 (2026-09-11) ------------------------------
 * FROM THIS WATCH'S OWN persist/sensors/registry (config_list.txt loads the
 * 8909w_pah_8011_0_* set; the pah_8131 files beside it are NOT listed):
 *   bus_type 0 (I2C), bus_instance 0, slave_config 21 = 0x15, 400 kHz,
 *   dri_irq_num 34 (TLMM gpio34), vddio "/pmic/client/sensor_vddio".
 * Same part and address as the Gen 4 / C2 (hr_pah8011.c), different INT pin.
 * The registry's bus_instance does not name pins, so the driver tries the I2C
 * pairs from triggerfish-stock.dts in order and keeps the one that answers:
 *   gpio6/7    blsp_i2c1 (i2c@78b5000) -- the Gen 4's HR bus; NFC nq@28 is also here
 *   gpio29/30  blsp_i2c3 (i2c@78b7000, DT-disabled, camera: not fitted)
 *   gpio14/15  blsp_i2c4 (i2c@78b8000, DT-disabled, smb1357: not used)
 * NOT tried: gpio19/18 blsp_i2c5 = the live touch bus. */
#define PLAT_HAS_HR_PAH8011   1
#define PLAT_HR_PAH8011_INT   34u
/* NOT gpio14/15: those are the BG co-processor's SPI CS/CLK (spi@78B8000,
 * qcom,bg-spi) -- bit-banging I2C there would clock garbage into the QCC1110. */
#define PLAT_HR_PAH8011_PINS  { 6u, 7u }, { 29u, 30u }
/* RPM SLEEP-SET rails, FROM-DTB (triggerfish-stock.dts consumers, init
 * voltages). Every rail with an AP-side consumer that must survive Vdd-min:
 *   l5  1.2 V  mdss_dsi vdda, usb hsusb_vdd_dig
 *   l11 1.8 V  mdss_dsi_ctrl0 vddio
 *   l12 1.8 V  mdss_dsi vddio + dsi pll, usb HSUSB_1p8, iris xo, pronto pll
 *   l13 1.8 V  sdhci@7824000 vdd-io (eMMC I/O), raydium vcc_i2c, pronto vddpx
 *   l16 3.1 V  usb HSUSB_3p3
 *   l18 3.3 V  mdss_dsi_ctrl0 vdd (panel)
 *   l19 2.9 V  sdhci@7824000 vdd (eMMC)
 * NOT voted, deliberately: l3 / l9 / l15 feed the QCC1110 co-processor
 * (bg-daemon, bg-rsb) and belong to it; l6 is the radio's alone. */
#define PLAT_SYS_PC_SLEEP_LDOS \
    { "l5 1.2V (dsi vdda/usb dig)",  5u,  1200000u }, \
    { "l11 1.8V (dsi ctrl vddio)",   11u, 1800000u }, \
    { "l12 1.8V (dsi/pll/usb/xo)",   12u, 1800000u }, \
    { "l13 1.8V (emmc io/touch)",    13u, 1800000u }, \
    { "l16 3.1V (usb 3p3)",          16u, 3100000u }, \
    { "l18 3.3V (panel)",            18u, 3300000u }, \
    { "l19 2.9V (emmc)",             19u, 2900000u },
