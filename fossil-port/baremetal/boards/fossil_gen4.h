/* fossil_gen4.h — Fossil Gen 4 (firefish/ray), Snapdragon Wear 2100 (msm8909w/APQ8009w).
 * Every address here is sourced from the firefish kernel branch / its DT
 * (see ../../HARDWARE.md). Values marked TODO await the stock-DTB dump.
 *
 * Fleet note: one header per device. A future Gen 6 (hoki, Wear 4100+, ARM64)
 * gets its own header AND its own startup (AArch64) — only the driver model
 * and the platform/ API are shared. */
#pragma once

#define PLAT_NAME           "fossil-gen4"

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
/* DSI controller IRQ: DT `interrupts = <0 80 0>` → SPI 80 → GIC ID 32+80 */
#define PLAT_DSI_IRQ         (32u + 80u)
/* MDP IRQ: DT `interrupts = <0 72 0>` */
#define PLAT_MDP_IRQ         (32u + 72u)

/* Panel geometry. 390x390 round (firefish). The exact controller + init table
 * come from the stock DTB (HARDWARE.md open question #1); until then the
 * AUO 400p command-mode panel is the template driving dsi_panel.c. */
#define PLAT_PANEL_W        390u
#define PLAT_PANEL_H        390u
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
#define PLAT_I2C_TOUCH_BASE 0x078B9000u
/* QUP core clock: BLSP QUP apps clk runs at 19.2 MHz on msm8909 for I2C. */
#define PLAT_I2C_CORE_HZ    19200000u
#define PLAT_I2C_BUS_HZ     400000u      /* fast mode; raydium supports it */

/* Raydium RM_TS touch controller (CONFIG_TOUCHSCREEN_RM_TS).
 * 7-bit I2C address 0x39 is the raydium_i2c_ts default; confirm from DTB. */
#define PLAT_TOUCH_I2C_ADDR 0x39u
