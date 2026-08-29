/* fossil_gen6.h — Fossil Gen 6 (hoki), Snapdragon Wear 4100+ (SDA429W).
 *
 * Every address below is VERBATIM from the stock device tree, which AsteroidOS
 * recovered from a real watch and published as a kernel patch. A decompiled
 * copy is checked in at ../../sda429-hoki-decompiled.dts (fully flattened —
 * every node inline, so a grep for a node name finds the real values).
 * See ../../HARDWARE-GEN6.md for provenance and the carries-over matrix.
 *
 * WHY THIS IS THE PREFERRED DEV TARGET (vs the Gen 4):
 * the Gen 6 exposes a full 4-pin USB connection on the charger pads. No
 * soldering to the board, no fragile hand-made pogo cable that tears off — so
 * `fastboot boot` iteration is repeatable instead of a one-shot gamble.
 *
 * CRITICAL — THIS IS A 32-BIT ARM TARGET, NOT AArch64.
 * The A53 is ARMv8, but this watch's vendor kernel puts its device tree in
 * arch/arm/boot/dts/ and AsteroidOS tunes it armv7vehf-neon — i.e. the stock
 * bootloader hands off in AArch32. ARMv8's AArch32 state is ARMv7-A compatible,
 * so the existing startup.S / short-descriptor MMU / GICv2 / FreeRTOS ARM_CA9
 * port all apply unchanged. Do NOT "fix" this into an ARM64 port.
 */
#pragma once

#define PLAT_NAME           "fossil-hoki"       /* Gen 6 */

/* ---------------------------------------------------------------------------
 * DDR. Same base as the Gen 4 (0x80000000), but the reserved-memory map is
 * different and much tighter at the bottom:
 *   other_ext_region  0x85B00000 + 0x00D00000
 *   modem_region      0x86800000 + 0x01F00000
 *   adsp_fw_region    0x88700000 + 0x01500000
 *   wcnss_fw_region   0x89C00000 + 0x00700000
 *   splash_region     0x90000000 + 0x01400000   (see PLAT_SPLASH_FB_BASE)
 * The first no-map region starts at 0x85B00000, so everything we own must live
 * below that. That is ~92 MB from the link base, which is plenty. */
#define PLAT_DDR_BASE       0x80000000u
#define PLAT_DDR_SIZE       (1024u * 1024u * 1024u)   /* 1 GB on hoki */
#define PLAT_DDR_SAFE_END   0x85B00000u               /* first no-map region */

/* Link/run address: DDR base + 32 KB, matching the stock boot.img's
 * kernel_addr (base 0x80000000 + kernel_offset 0x8000). startup.S
 * self-relocates here from wherever aboot actually dropped us. */
#define PLAT_LINK_BASE      0x80008000u

/* ---------------------------------------------------------------------------
 * GIC-400 (qcom,msm-qgic2) — interrupt-controller@b000000.
 * reg = <0xb000000 0x1000  0xb002000 0x1000> — IDENTICAL to the Gen 4, so the
 * existing gic.c needs no change at all. */
#define PLAT_GICD_BASE      0x0B000000u
#define PLAT_GICC_BASE      0x0B002000u

/* ARM architected timer: 19.2 MHz XO (DT clock-frequency = 0x124f800).
 * Same rate as the Gen 4. Note the DT compatible is arm,armv8-timer, but the
 * AArch32 CNTV interface (CNTFRQ/CNTVCT/CNTV_CVAL) is the same one timer.c
 * already drives via cp15. */
#define PLAT_TIMER_HZ       19200000u

/* ---------------------------------------------------------------------------
 * Debug UART: BLSP1 UART2, UARTDM v1.4 — serial@78b0000, alias blsp1_uart2.
 * This is the stock cmdline's ttyMSM0 AND its earlycon
 * (earlycon=msm_serial_dm,0x78b0000), so aboot leaves its clocks running —
 * the same precondition uart_msm.c already relies on for the Gen 4.
 * (Gen 4's was 0x78af000; only the base differs, the IP is identical.) */
#define PLAT_UART_TYPE_MSM  1
#define PLAT_UART_BASE      0x078B0000u

/* ...BUT THE UART IS DISABLED ON THIS WATCH (2026-08-03, measured).
 *
 * The staircase test rebooted at the "MMU up, GIC not yet up" step, and the
 * only three calls in that window are uart_init(), ramlog_init() and
 * gic_init(). Neither of the latter two can block (no loops, DDR only), and
 * uart_putc is iteration-bounded — so the failure is a BUS HANG, and on MSM
 * parts touching a CLOCK-GATED peripheral does exactly that. The comment in
 * uart_msm.c assumed "aboot leaves the console clocked"; that assumption is
 * the thing that just cost us a boot.
 *
 * And we lose NOTHING by dropping it: this watch has no exposed UART pad
 * (HARDWARE-GEN6.md), so nothing was ever readable from it. The ramlog keeps
 * every con_puts() message. Define PLAT_UART_ENABLE to put it back if a pad is
 * ever found and the GCC clock driver lands. */
#define PLAT_UART_DISABLED  1

/* PMIC RTC WRITE path disabled (2026-08-03, THE 3-SECOND RECOVERY CRASH).
 * VISUAL_TRACE pinned the instant TZ reset to the setup() clock block: the
 * first-ever sid-0 SPMI READ (rtc_read_epoch) comes back failed/implausible,
 * the .ino declares the RTC unset and calls board_clock_set -> settimeofday ->
 * rtc_write_epoch, whose first op WRITES RTC_CTRL. On production units the
 * RTC write path is owned by the secure world (the kernel driver needs an
 * explicit DT allow-flag for a reason) — an NS write to a TZ-owned SPMI
 * peripheral is an arb ownership violation = instant hard reset into Wear OS.
 * READS stay enabled (observer-path, fail clean). Remove only with a
 * validated write path (or after proving ownership from the stock DT). */
#define PLAT_RTC_WRITE_DISABLED  1

/* ---------------------------------------------------------------------------
 * Display. NOTE THE DIVERGENCE FROM THE GEN 4: this is MDP5, not MDP3, and a
 * 12nm DSI PHY, not 28nm. msm_mdp3.c does NOT apply here and the PHY init is a
 * different block. The bases below are from the DTB; the pixel path itself is
 * deliberately NOT claimed yet (see PLAT_HAS_DSI_PANEL below).
 *   qcom,mdss_mdp@1a00000            (mdp reg offset 0x1000)
 *   qcom,mdss_dsi_ctrl0@1a94000      reg = ctrl 0x1a94000+0x300,
 *                                          phy  0x1a94400+0x400,
 *                                          misc 0x193e000+0x30
 *   qcom,mdss_dsi_pll@1a94400        compatible qcom,mdss_dsi_pll_12nm
 *   gcc-mdss                         qcom,gcc-mdss@1800000 -> GCC @0x1800000 */
#define PLAT_MDP_BASE        0x01A00000u
#define PLAT_DSI_CTRL_BASE   0x01A94000u
#define PLAT_DSI_PHY_BASE    0x01A94400u
#define PLAT_MMSS_MISC_BASE  0x0193E000u
#define PLAT_GCC_BASE        0x01800000u

/* Continuous-splash framebuffer: splash_region 0x90000000 len 0x1400000.
 * aboot draws its boot logo here and leaves the panel LIT and scanning. That
 * makes it the cheapest possible display bring-up on this watch: write pixels
 * straight into this buffer and they appear, with NO DSI/PHY/panel code of our
 * own. That is exactly what fb_splash.c does. */
#define PLAT_SPLASH_FB_BASE  0x90000000u
#define PLAT_SPLASH_FB_SIZE  0x01400000u

/* Panel geometry.
 * UNCONFIRMED — the published DTB does NOT contain the panel node (it is in a
 * dtbo overlay we do not have yet), so there is no authoritative width/height.
 * The only in-DTB hint is qcom,mdss_wb_panel's 640x640 writeback buffer, and
 * the Gen 6 is widely reported as a 1.28" 416x416 AMOLED. fb_splash.c does not
 * trust either: it AUTO-DETECTS the real geometry from what aboot programmed
 * into the MDP, and only falls back to this value if that read fails. */
#define PLAT_PANEL_W        416u
#define PLAT_PANEL_H        416u
#define PLAT_SCREEN_ROUND   1

/* Capability flags. The Gen 6 starts with the splash framebuffer only; DSI,
 * touch and the crown arrive once the dtbo dump gives us their real nodes.
 * Guarding on these keeps the shared platform/ code honest about what is
 * actually implemented per board instead of silently linking Gen 4 drivers. */
#define PLAT_HAS_SPLASH_FB   1
#define PLAT_HAS_DSI_PANEL   0   /* MDP5 + 12nm PHY: not ported yet */
#define PLAT_HAS_TOUCH       1   /* Raydium U128BLA03 (from the AsteroidOS hoki
                                    defconfig); bus auto-probed at touch_init —
                                    see touch_raydium.c */

/* ---------------------------------------------------------------------------
 * SPMI PMIC arbiter — qcom,spmi@200f000, chnls 0x2400000, EE 0, channel 0.
 * IDENTICAL addresses to the Gen 4, so the arbiter half of pmic_vib.c is
 * shared. The PMIC itself is NOT the same: PM660, and its haptics block is a
 * different peripheral from PM8916's simple vibrator (see below). */
#define PLAT_SPMI_CHNLS_BASE 0x02400000u

/* ---------------------------------------------------------------------------
 * I2C — 8x qcom,i2c-msm-v2 BLSP QUP masters, same QUP v2 IP as the Gen 4, so
 * msm_i2c.c drives them with only these bases. Verbatim from the DTB:
 *   BLSP1: i2c@78b5000 (QUP1) 78b6000 (QUP2) 78b7000 (QUP3) 78b8000 (QUP4)
 *   BLSP2: i2c@7af5000 (QUP5) 7af6000 (QUP6) 7af7000 (QUP7) 7af8000 (QUP8)
 * The ONLY populated I2C child in the published DTB is the NFC controller,
 * nq@28 on i2c@7af5000 — every sensor (touch, IMU...) hides in the dtbo we do
 * not have yet. NOTE aboot only guarantees clocks for BLSP1 (its own console
 * UART is there); an unclocked QUP reads all-ones and the driver's bounded
 * polls turn that into a clean error, never a hang.
 * No PLAT_I2C_TOUCH_BASE on purpose: the touch bus is unknown until the dtbo
 * dump, so only the explicit-base API (i2c_bus_*) exists on this board. */
#define PLAT_I2C_BLSP1_QUP1  0x078B5000u
#define PLAT_I2C_BLSP1_QUP2  0x078B6000u
#define PLAT_I2C_BLSP1_QUP3  0x078B7000u
#define PLAT_I2C_BLSP1_QUP4  0x078B8000u
#define PLAT_I2C_BLSP2_QUP1  0x07AF5000u   /* NFC nq@28 lives here */
#define PLAT_I2C_BLSP2_QUP2  0x07AF6000u
#define PLAT_I2C_BLSP2_QUP3  0x07AF7000u
#define PLAT_I2C_BLSP2_QUP4  0x07AF8000u
/* CONFIRMED FROM THE DEVICE (2026-08-02 /sys dump, AsteroidOS root shell):
 *   /sys/bus/i2c/devices/4-0039 -> .../78b8000.i2c/i2c-4/4-0039   = TOUCH
 *   /sys/bus/i2c/devices/2-0028, 2-0034 -> .../78b6000.i2c/i2c-2
 *   /sys/bus/i2c/devices/i2c-5 -> .../7af5000.i2c                 = NFC bus
 * So the Raydium is on BLSP1 QUP4 at 0x39 — the touch driver now uses these
 * directly and only falls back to probing if it does not answer. */
#define PLAT_I2C_TOUCH_BASE   PLAT_I2C_BLSP1_QUP4   /* 0x078B8000, i2c-4 */
#define PLAT_TOUCH_I2C_ADDR   0x39u                 /* Raydium NID */

/* Touch GPIOs, read verbatim out of the merged /proc/device-tree pulled from
 * the device on 2026-08-06 (dumps/gen6-44mm-20260806, node
 * soc/i2c@78b8000/raydium@39):
 *   raydium,reset-gpio     = <&tlmm 0x40 0x0>     -> 64   (already driven)
 *   raydium,irq-gpio       = <&tlmm 0x41 0x2002>  -> 65, ACTIVE LOW
 *   raydium,display-coords = <0 0 0x1a0 0x1a0>    -> 416 x 416
 *   raydium,num-max-touches = 2
 * Reset 64 matching the pin we already pulse is what confirms this is the
 * right node, so 65 is the INT line the kernel's IRQ handler runs on. */
/* HS USB controller ("qcom,hsusb-otg"), DT usb@78db000 reg <0x078db000 0x400>.
 * Capability block sits at +0x100, operational at +0x140 (CAPLENGTH 0x40) --
 * usb_ci.c reads CAPLENGTH at runtime rather than trusting that. */
#define PLAT_USB_BASE         0x078DB000u
/* PHY CSR block, DT reg-names "phy_csr" <0x0006c000 0x200>. For the QUSB
 * ULPI PHY (qcom,hsusb-otg-phy-type = <3>) the kernel configures the PHY
 * through THIS window, not the ULPI viewport (viewport regs > 0x3F are
 * explicitly unsupported for this PHY type — phy-msm-usb.c ulpi_write). */
#define PLAT_USB_PHY_CSR      0x0006C000u
#define PLAT_HAVE_USB_CDC     1

#define PLAT_TOUCH_RESET_GPIO 64u
#define PLAT_TOUCH_INT_GPIO   65u
#define PLAT_I2C_DEFAULT_BASE PLAT_I2C_BLSP1_QUP4

/* I2C HARDWARE ACCESS IS DISABLED FOR NOW (2026-08-03, measured).
 * setup() calls Wire.begin() before anything owns the GCC clocks, and touching
 * a clock-gated BLSP QUP wedges the bus — the same failure that took out
 * uart_init() and the MDP probe. Every QUP entry point returns an error
 * instead, which the firmware handles gracefully (Wire reports failure, the
 * fuel gauge is on SPMI and unaffected, touch degrades to display-only).
 * REMOVE THIS once a GCC driver enables the BLSP QUP apps/iface clocks.
 *
 * 2026-08-03 UPDATE (post-first-light): the hang measurement above came from
 * the poisoned-instrument era (pre wdog-latch-sync, misdecoded staircases) —
 * the parallel "MDP registers hang" conclusion from the same era turned out
 * to be environment-dependent (display works fine from recovery). So the
 * I2C hang is UNVERIFIED on the recovery path with fixed instruments.
 * -DPLAT_I2C_RETEST re-enables the QUP for exactly that test; if it hangs,
 * the wdog stages + deadman still recover the watch. */
#if !defined(PLAT_I2C_RETEST)
#define PLAT_I2C_DISABLED   1
#endif
#define PLAT_I2C_CORE_HZ     19200000u     /* BLSP QUP apps clk for I2C */
#define PLAT_I2C_BUS_HZ      400000u       /* fast mode */

/* PM660 haptics — qcom,haptic@c000 on qcom,pm660@1 (SPMI slave id 1, NOT 0).
 *   qcom,actuator-type = "erm"
 *   qcom,vmax-mv       = 0xc80 = 3200 mV
 *   qcom,play-rate-us  = 0x2710
 * This is the qpnp-haptics block, which needs an explicit play/enable rather
 * than PM8916's single enable bit. Driven by pmic_vib.c's gen6 path. */
#define PLAT_PMIC_SID        1u
#define PLAT_HAP_BASE        0xC000u
#define PLAT_HAP_VMAX_MV     3200u
