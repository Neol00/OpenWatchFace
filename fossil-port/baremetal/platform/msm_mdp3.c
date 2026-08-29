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
#if defined(PLAT_SOC_MSM8909)

/* PWR census counters (storage in gen4_stubs.c). */
extern volatile uint32_t g_fb_wait_us, g_fb_cache_us;

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
#define MDP3_REG_DMA_P_FETCH_CFG        0x90074

/* interrupt bits (mdp3_hwio.h) */
#define MDP3_INTR_DMA_P_DONE_BIT        (1u << 14)
#define MDP3_INTR_SYNC_PRIMARY_LINE_BIT (1u << 8)

/* enum ordinals (mdp3_dma.h) — these are plain enum positions in the kernel */
#define MDP3_DMA_OUTPUT_SEL_DSI_CMD     1
#define MDP3_DMA_IBUF_FORMAT_XRGB8888   2
/* Output pack pattern (mdp3_dma.h): 2 bits per component, packed [c2 c1 c0].
 * RGB = (R<<4)|(G<<2)|(B<<0) = (2<<4)|(1<<2)|0 = 0x21.
 * BGR = 0x12 (mdp3_dma.h's enum, VERBATIM — an earlier hand-derivation of
 * this value produced 0x06, which is that enum's GBR: green and blue would
 * have swapped instead of red and blue).
 * The AUO h139 DT declares color-order "rgb_swap_rgb" (R and B swapped), so the
 * Fossil panel wants BGR pack order. VERIFY ON HARDWARE: if reds and blues are
 * inverted on the first boot, this is the line — flip between 0x21 and 0x06. */
#define MDP3_DMA_OUTPUT_PACK_PATTERN_RGB 0x21
#define MDP3_DMA_OUTPUT_PACK_PATTERN_BGR 0x12
/* Which of the two the panel actually wants is a ONE-BIT QUESTION that only
 * hardware can answer, so it is a build flag rather than a source edit:
 * pass -DMDP3_PACK_RGB to send R first, -DMDP3_PACK_BGR to send B first.
 * ANSWERED ON HARDWARE (first pixels, 2026-08-28): the default is RGB.
 * BGR was the reasoned guess — the panel node declares color-order
 * "rgb_swap_rgb", and mdp3_ctrl.c picks BGR for its 8888 formats — and it was
 * wrong: red rendered as blue and a bright blue as orange, with black and
 * white correct, which is R and B swapped and nothing else. The DT's
 * "rgb_swap_rgb" evidently describes a swap the panel already performs, so
 * the MDP must NOT perform it a second time.
 * Build with -DFB_COLORTEST to see the four-band test pattern that names the
 * right answer outright instead of guessing from a photo of a watch face. */
#if defined(MDP3_PACK_BGR)
#define MDP3_DMA_OUTPUT_PACK_PATTERN     MDP3_DMA_OUTPUT_PACK_PATTERN_BGR
#else
#define MDP3_DMA_OUTPUT_PACK_PATTERN     MDP3_DMA_OUTPUT_PACK_PATTERN_RGB
#endif
#define MDP3_DMA_OUTPUT_PACK_ALIGN_LSB  0
#define MDP3_DMA_OUTPUT_COMP_BITS_8     3
/* color_comp_out_bits is THREE 2-bit fields (one per component), not one:
 * mdp3_ctrl.c writes (BITS_8 << 4) | (BITS_8 << 2) | BITS_8 = 0x3F. Writing
 * the bare 3 sets only component 0 to 8 bits and leaves the other two at
 * 4 bits — the kernel value is the only correct one. */
#define MDP3_DMA_COMP_OUT_BITS_888      ((MDP3_DMA_OUTPUT_COMP_BITS_8 << 4) | \
                                         (MDP3_DMA_OUTPUT_COMP_BITS_8 << 2) | \
                                          MDP3_DMA_OUTPUT_COMP_BITS_8)

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
    cfg |= (uint32_t)MDP3_DMA_OUTPUT_PACK_PATTERN << 8;
    cfg |= (uint32_t)MDP3_DMA_OUTPUT_PACK_ALIGN_LSB << 7;
    cfg |= (uint32_t)MDP3_DMA_COMP_OUT_BITS_888;

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

    /* Cache maintenance IS CPU work and stays counted as busy — it is only
     * measured here so the Power app can attribute it. On a 360x360 XRGB8888
     * frame this cleans ~518 KB per flush, which is genuinely expensive and
     * worth seeing rather than hiding. */
    {
        uint32_t c0 = timer_us32();
        cache_clean_range(buf, s_stride * s_h);
        g_fb_cache_us += timer_us32() - c0;
    }

    MDP_W(MDP3_REG_DMA_P_IBUF_ADDR, (uint32_t)(uintptr_t)buf);
    mdp_wmb();
    MDP_W(MDP3_REG_DMA_P_START, 1);
    mdp_wmb();

    {
        /* PWR census: the time spent spinning here is the panel's transfer
         * time, not the CPU's work. Booking it as busy is what made an idle
         * watch report ~86%. The Gen 6 keeps the same counter in fb_splash.c;
         * storage for the msm8909w watches lives in gen4_stubs.c. */
        uint32_t w0 = timer_us32();
        int rc = mdp3_wait_dma_done(100);
        g_fb_wait_us += timer_us32() - w0;
        return rc;
    }
}

/* --- aboot splash takeover helpers --------------------------------------
 * The Gen 6 taught this port that a blind display bring-up is unobservable:
 * every failure looks identical ("nothing on the glass"), so the port that
 * REUSES what the bootloader already programmed wins. On msm8909 aboot's
 * continuous splash is exactly this DMA_P engine driving the same
 * command-mode DSI panel we want, so the whole DSI/PHY/PLL/panel-init stack
 * can stay untouched and only the buffer pointer changes hands.
 *
 * The kernel does the same thing: mdp3_dmap_config() takes a
 * `splash_screen_active` flag and, when set, writes NONE of the DMA_P
 * registers — it inherits the bootloader's configuration verbatim.
 */
int mdp3_splash_probe(struct mdp3_splash_cfg *out)
{
    uint32_t cfg    = MDP_R(MDP3_REG_DMA_P_CONFIG);
    uint32_t size   = MDP_R(MDP3_REG_DMA_P_SIZE);
    uint32_t addr   = MDP_R(MDP3_REG_DMA_P_IBUF_ADDR);
    uint32_t stride = MDP_R(MDP3_REG_DMA_P_IBUF_Y_STRIDE);

    out->config = cfg;
    out->addr   = addr;
    out->w      = size & 0xFFFFu;
    out->h      = (size >> 16) & 0xFFFFu;
    out->stride = stride;
    out->format = (cfg >> 25) & 3u;
    out->out_sel = (cfg >> 19) & 3u;

    /* Believability: a live splash points DMA_P at DDR with panel-sized
     * geometry and a stride consistent with the format. Anything else means
     * aboot shut the display down (or never lit it) and the registers hold
     * reset/garbage values. */
    if (addr < PLAT_DDR_BASE || addr >= PLAT_DDR_BASE + PLAT_DDR_SIZE) return -1;
    if (out->w < 64u || out->w > 1024u || out->h < 64u || out->h > 1024u) return -1;
    if (out->stride < out->w) return -1;
    if (out->out_sel != MDP3_DMA_OUTPUT_SEL_DSI_CMD) return -1;
    return 0;
}

/* Repoint the engine at a new buffer without touching format/size/stride.
 * Used when our framebuffer geometry already matches aboot's. */
void mdp3_dma_repoint(const void *buf)
{
    s_buf = buf;
    MDP_W(MDP3_REG_DMA_P_IBUF_ADDR, (uint32_t)(uintptr_t)buf);
    mdp_wmb();
}

uint32_t mdp3_dma_stride(void) { return s_stride; }

#endif /* PLAT_SOC_MSM8909 */
