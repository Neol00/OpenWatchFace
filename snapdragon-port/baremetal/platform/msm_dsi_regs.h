/* msm_dsi_regs.h — MDSS DSI controller + 28nm DSI PHY register map.
 *
 * Transcribed from the vendor 3.18 kernel (the firefish branch named in
 * ../../HARDWARE.md):
 *   drivers/video/msm/mdss/mdss_dsi_host.c   (controller offsets)
 *   drivers/video/msm/mdss/msm_mdss_io_8974.c (mdss_dsi_28nm_phy_config /
 *                                              _regulator_enable)
 * The kernel uses bare hex offsets inline; the names here are from the
 * comments beside those writes and the MDSS register documentation.
 *
 * All offsets are byte offsets from PLAT_DSI_CTRL_BASE / PLAT_DSI_PHY_BASE /
 * PLAT_DSI_PHY_REG_BASE respectively. */
#pragma once

/* ---- DSI controller (base PLAT_DSI_CTRL_BASE) --------------------------- */
#define DSI_HW_VERSION              0x0000
#define DSI_CTRL                    0x0004
#define DSI_STATUS                  0x0008  /* CMD_MODE_DMA_BUSY = BIT(2) */
#define DSI_FIFO_STATUS             0x000c
#define DSI_VIDEO_MODE_CTRL         0x0010
#define DSI_VIDEO_MODE_ACTIVE_H     0x0024
#define DSI_VIDEO_MODE_ACTIVE_V     0x0028
#define DSI_VIDEO_MODE_TOTAL        0x002c
#define DSI_VIDEO_MODE_HSYNC        0x0030
#define DSI_VIDEO_MODE_VSYNC        0x0034
#define DSI_VIDEO_MODE_VSYNC_VPOS   0x0038
#define DSI_CMD_MODE_DMA_CTRL       0x003c
#define DSI_CMD_MODE_MDP_CTRL       0x0040
#define DSI_CMD_MODE_MDP_DCS_CMD_CTRL 0x0044
#define DSI_DMA_CMD_OFFSET          0x0048  /* DMA base address of cmd buffer */
#define DSI_DMA_CMD_LENGTH          0x004c  /* bytes to send */
#define DSI_CMD_MODE_MDP_STREAM0_CTRL 0x0058
#define DSI_CMD_MODE_MDP_STREAM0_TOTAL 0x005c
#define DSI_CMD_MODE_MDP_STREAM1_CTRL 0x0060
#define DSI_CMD_MODE_MDP_STREAM1_TOTAL 0x0064
#define DSI_ACK_ERR_STATUS          0x0068
#define DSI_RDBK_DATA0              0x006c
#define DSI_TRIG_CTRL               0x0084
#define DSI_EXT_MUX                 0x0088
/* THESE TWO WERE SWAPPED (fixed 2026-08-28) and it cost the whole first
 * brightness attempt: every DCS write kicked 0x08c, no DMA ever started, and
 * the completion check of the day reported success anyway.
 *
 * Ground truth is mdss_dsi_host.c's command-DMA path, which writes the buffer
 * address to 0x048, the length to 0x04c, and then triggers at 0x090:
 *     MIPI_OUTP((ctrl->ctrl_base) + 0x048, ctrl->dma_addr);
 *     MIPI_OUTP((ctrl->ctrl_base) + 0x04c, len);
 *     MIPI_OUTP((ctrl->ctrl_base) + 0x090, 0x01);
 * (mdss_dsi_cmd_dma_tx, and again in mdss_dsi_cmd_mdp_busy's sibling path.) */
#define DSI_CMD_MODE_MDP_SW_TRIGGER 0x008c
#define DSI_CMD_MODE_DMA_SW_TRIGGER 0x0090
#define DSI_RESET_SW_TRIGGER        0x0094
#define DSI_MISR_CMD_CTRL           0x009c
#define DSI_LANE_CTRL               0x00ac
#define DSI_LAN_SWAP_CTRL           0x00b0
#define DSI_HS_TIMER_CTRL           0x00bc
#define DSI_CLKOUT_TIMING_CTRL      0x00c4
#define DSI_EOT_PACKET_CTRL         0x00cc
#define DSI_ERR_INT_MASK0           0x010c
#define DSI_INTL_CTRL               0x0110
#define DSI_CLK_CTRL                0x011c
#define DSI_SOFT_RESET              0x0118

/* DSI_CTRL bits (mdss_dsi_host.c: dsi_ctrl = BIT(8)|BIT(2), lanes BIT(4..7)) */
#define DSI_CTRL_ENABLE             (1u << 0)
#define DSI_CTRL_VIDEO_MODE_EN      (1u << 1)
#define DSI_CTRL_CMD_MODE_EN        (1u << 2)
#define DSI_CTRL_DATA_LANE0         (1u << 4)
#define DSI_CTRL_DATA_LANE1         (1u << 5)
#define DSI_CTRL_DATA_LANE2         (1u << 6)
#define DSI_CTRL_DATA_LANE3         (1u << 7)
#define DSI_CTRL_CLK_EN             (1u << 8)
#define DSI_CTRL_ECC_CHECK          (1u << 20)
#define DSI_CTRL_CRC_CHECK          (1u << 24)

/* DSI_STATUS / interrupt bits */
#define DSI_STATUS_CMD_MODE_DMA_BUSY (1u << 2)
#define DSI_INTR_CMD_DMA_DONE        (1u << 0)
#define DSI_INTR_CMD_DMA_DONE_MASK   (1u << 1)
#define DSI_INTR_CMD_MDP_DONE        (1u << 8)
#define DSI_INTR_CMD_MDP_DONE_MASK   (1u << 9)
#define DSI_INTR_ERROR               (1u << 24)
#define DSI_INTR_ERROR_MASK          (1u << 25)

/* ---- 28nm DSI PHY (base PLAT_DSI_PHY_BASE) ------------------------------ */
/* Lane config: 5 blocks (4 data + clk) of 9 registers, stride 0x40, from
 * offset 0x0000 — msm_mdss_io_8974.c mdss_dsi_28nm_phy_config(). */
#define DSIPHY_LANE_CFG_BASE        0x0000
#define DSIPHY_LANE_STRIDE          0x0040
#define DSIPHY_LANE_NREGS           9
#define DSIPHY_NUM_LANES            5

#define DSIPHY_TIMING_CTRL_0        0x0140  /* 12 regs, stride 4 */
#define DSIPHY_CTRL_0               0x0170  /* 0x5b then 0x5f (lanes powered) */
#define DSIPHY_CTRL_1               0x0174
#define DSIPHY_CTRL_2               0x0178
#define DSIPHY_CTRL_3               0x017c
#define DSIPHY_CTRL_4               0x0180  /* = 0x0a */
#define DSIPHY_STRENGTH_CTRL_0      0x0184
#define DSIPHY_STRENGTH_CTRL_1      0x0188
#define DSIPHY_BIST_CTRL_0          0x01b4  /* 6 regs, stride 4 */
#define DSIPHY_GLBL_TEST_CTRL       0x01d4  /* = 0x01 for single-DSI */
#define DSIPHY_LDO_CTRL             0x01dc  /* 0x0d ldo mode / 0x00 dcdc */

/* ---- 28nm PHY regulator block (base PLAT_DSI_PHY_REG_BASE) -------------- */
#define DSIPHY_REG_CTRL_0           0x0000
#define DSIPHY_REG_CTRL_1           0x0004
#define DSIPHY_REG_CTRL_2           0x0008
#define DSIPHY_REG_CTRL_3           0x000c
#define DSIPHY_REG_CTRL_4           0x0010
#define DSIPHY_REG_TEST             0x0014
#define DSIPHY_REG_CAL_PWR_CFG      0x0018

/* ---- MMSS misc (base PLAT_MMSS_MISC_BASE) ------------------------------- */
/* DT: qcom,mmss-ulp-clamp-ctrl-offset / qcom,mmss-phyreset-ctrl-offset */
#define MMSS_MISC_ULP_CLAMP_CTRL    0x0020
#define MMSS_MISC_PHYRESET_CTRL     0x0024

/* ---- MIPI DSI packet data types (DCS / generic) ------------------------- */
#define DTYPE_DCS_WRITE             0x05  /* short, 0 param  */
#define DTYPE_DCS_WRITE1            0x15  /* short, 1 param  */
#define DTYPE_DCS_LWRITE            0x39  /* long            */
#define DTYPE_DCS_READ              0x06
#define DTYPE_GEN_WRITE             0x03
#define DTYPE_GEN_WRITE1            0x13
#define DTYPE_GEN_WRITE2            0x23
#define DTYPE_GEN_LWRITE            0x29
#define DTYPE_EOT                   0x08

/* Standard DCS commands used by the panel on/off tables */
#define DCS_ENTER_SLEEP_MODE        0x10
#define DCS_EXIT_SLEEP_MODE         0x11
#define DCS_SET_DISPLAY_OFF         0x28
#define DCS_SET_DISPLAY_ON          0x29
#define DCS_WRITE_DISPLAY_BRIGHTNESS 0x51  /* bl_ctrl_dcs; 1 byte, max 0xff */
#define DCS_SET_COLUMN_ADDRESS      0x2a
#define DCS_SET_PAGE_ADDRESS        0x2b
#define DCS_WRITE_MEMORY_START      0x2c
#define DCS_WRITE_MEMORY_CONTINUE   0x3c
#define DCS_SET_TEAR_OFF            0x34
#define DCS_SET_TEAR_ON             0x35
