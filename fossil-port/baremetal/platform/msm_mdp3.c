/* msm_mdp3.c — MDP3 DMA_P frame push to a command-mode DSI panel.
 *
 * Ported from the vendor 3.18 kernel (firefish branch; see ../../HARDWARE.md):
 *   drivers/video/msm/mdss/mdp3_dma.c
 *       mdp3_dmap_config()  -> mdp3_dma_config()
 *       mdp3_dmap_update()  -> mdp3_flush()
 *   drivers/video/msm/mdss/mdp3_hwio.h  (register offsets, interrupt bits)
 *   drivers/video/msm/mdss/mdp3_dma.h   (format / output-select enums)
 *
 * Model: the panel is COMMAND mode, so there is no continuous scanout. Each
 * frame is an explicit push — point DMA_P at the framebuffer, write
 * DMA_P_START, and the MDP streams one frame out over DSI. This is exactly
 * what mdp3_dmap_update() does on the MDP3_DMA_OUTPUT_SEL_DSI_CMD path.
 *
 * Deliberately omitted vs the kernel (all Phase 6 / not needed to show pixels):
 *   - color-correction (CCS), LUT, histogram, cursor blocks
 *   - the vsync/DMA-done *interrupt* path: we poll INTR_STATUS instead, so no
 *     MDP IRQ can fire before irq.c is told about it (same choice as msm_dsi.c)
 *   - clock/GDSC/bus-bandwidth voting: aboot left MDSS powered for its splash
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN4)

#include "msm_dsi_regs.h"

/* --- MDP3 registers (mdp3_hwio.h), offsets from PLAT_MDP_BASE ------------ */
#define MDP3_REG_INTR_ENABLE            0x0020
#define MDP3_REG_INTR_STATUS            0x0024
#define MDP3_REG_INTR_CLEAR             0x0028
#define MDP3_REG_DMA_P_START            0x0044
#define MDP3_REG_DISPLAY_STATUS         0x0038

#define MDP3_REG_DMA_P_CONFIG           0x90000
#define MDP3_REG_DMA_P_SIZE             0x90004
#define MDP3_REG_DMA_P_IBUF_ADDR        0x90008
#define MDP3_REG_DMA_P_IBUF_Y_STRIDE    0x9000C
#define MDP3_REG_DMA_P_OUT_XY           0x90010
#define MDP3_REG_DMA_P_FETCH_CFG        0x90048

/* interrupt bits (mdp3_hwio.h) */
#define MDP3_INTR_DMA_P_DONE_BIT        (1u << 14)
#define MDP3_INTR_SYNC_PRIMARY_LINE_BIT (1u << 8)

/* enum ordinals (mdp3_dma.h) — these are plain enum positions in the kernel */
#define MDP3_DMA_OUTPUT_SEL_DSI_CMD     1
#define MDP3_DMA_IBUF_FORMAT_XRGB8888   2
#define MDP3_DMA_OUTPUT_PACK_PATTERN_RGB 0x21
#define MDP3_DMA_OUTPUT_PACK_ALIGN_LSB  0
#define MDP3_DMA_OUTPUT_COMP_BITS_8     3

#define MDP_R(off)      mmio_read(PLAT_MDP_BASE + (off))
#define MDP_W(off, v)   mmio_write(PLAT_MDP_BASE + (off), (v))

static inline void mdp_wmb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint32_t s_w, s_h, s_stride;
static const void *s_buf;

/* Clean the framebuffer out of D-cache so the MDP's bus master sees the
 * pixels LVGL just wrote. mmu.c maps DDR as Normal cacheable, so this is
 * mandatory before every flush — see HARDWARE.md perf note #2. */
static void cache_clean_range(const void *addr, uint32_t len)
{
    uintptr_t p   = (uintptr_t)addr & ~31u;
    uintptr_t end = ((uintptr_t)addr + len + 31u) & ~31u;
    for (; p < end; p += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory"); /* DCCMVAC */
    mdp_wmb();
}

/* Port of mdp3_dmap_config() for: XRGB8888 source, DSI-command output,
 * 8 bits/component, RGB pack order, LSB align, no dither.
 * The bit positions are the kernel's, verbatim. */
void mdp3_dma_config(const void *buf, uint32_t w, uint32_t h)
{
    uint32_t cfg;

    s_buf = buf; s_w = w; s_h = h;
    s_stride = w * 4;                      /* XRGB8888 */

    cfg  = (uint32_t)MDP3_DMA_IBUF_FORMAT_XRGB8888 << 25;
    /* dither_en = 0  -> BIT(24) clear */
    cfg |= (uint32_t)MDP3_DMA_OUTPUT_SEL_DSI_CMD << 19;
    /* bit_mask_polarity = 0 -> BIT(18) clear */
    /* color_components_flip = 0 -> bits 14.. clear */
    cfg |= (uint32_t)MDP3_DMA_OUTPUT_PACK_PATTERN_RGB << 8;
    cfg |= (uint32_t)MDP3_DMA_OUTPUT_PACK_ALIGN_LSB << 7;
    cfg |= (uint32_t)MDP3_DMA_OUTPUT_COMP_BITS_8;

    MDP_W(MDP3_REG_DMA_P_CONFIG, cfg);
    MDP_W(MDP3_REG_DMA_P_SIZE, w | (h << 16));
    MDP_W(MDP3_REG_DMA_P_IBUF_ADDR, (uint32_t)(uintptr_t)buf);
    MDP_W(MDP3_REG_DMA_P_IBUF_Y_STRIDE, s_stride);
    MDP_W(MDP3_REG_DMA_P_OUT_XY, 0);       /* x=0, y=0 */
    MDP_W(MDP3_REG_DMA_P_FETCH_CFG, 0x40); /* kernel constant */
    mdp_wmb();

    /* Clear any stale latched status; we poll these, so leave INTR_ENABLE off. */
    MDP_W(MDP3_REG_INTR_CLEAR, MDP3_INTR_DMA_P_DONE_BIT);
    MDP_W(MDP3_REG_INTR_ENABLE, 0);
    mdp_wmb();

    con_puts("mdp3: DMA_P cfg="); con_puthex(cfg);
    con_puts(" size="); con_putdec(w); con_puts("x"); con_putdec(h); con_puts("\n");
}

/* Wait for the previous frame's DMA to retire. Command mode: DMA_P_DONE. */
static int mdp3_wait_dma_done(uint32_t timeout_ms)
{
    uint32_t t0 = timer_ms();
    while (!(MDP_R(MDP3_REG_INTR_STATUS) & MDP3_INTR_DMA_P_DONE_BIT)) {
        if ((uint32_t)(timer_ms() - t0) > timeout_ms) return -1;
    }
    MDP_W(MDP3_REG_INTR_CLEAR, MDP3_INTR_DMA_P_DONE_BIT);
    mdp_wmb();
    return 0;
}

/* Push one full frame. Port of the DSI_CMD branch of mdp3_dmap_update():
 * set IBUF_ADDR, kick DMA_P_START, then wait for DMA_P_DONE.
 *
 * NOTE this is a BLOCKING flush. HARDWARE.md perf item #1 calls the async
 * (overlap transfer with next-frame compute) version the single biggest
 * lever; that is a follow-up once pixels are confirmed on glass. */
int mdp3_flush(const void *buf)
{
    /* Retire the previous push before repointing the DMA engine. */
    if (MDP_R(MDP3_REG_INTR_STATUS) & MDP3_INTR_DMA_P_DONE_BIT)
        MDP_W(MDP3_REG_INTR_CLEAR, MDP3_INTR_DMA_P_DONE_BIT);

    cache_clean_range(buf, s_stride * s_h);

    MDP_W(MDP3_REG_DMA_P_IBUF_ADDR, (uint32_t)(uintptr_t)buf);
    mdp_wmb();
    MDP_W(MDP3_REG_DMA_P_START, 1);
    mdp_wmb();

    return mdp3_wait_dma_done(100);
}

#endif /* PLAT_BOARD_FOSSIL_GEN4 */
