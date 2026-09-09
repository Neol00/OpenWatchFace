/* fb_ramfb.c — QEMU ramfb framebuffer via fw_cfg (QEMU-only display path).
 *
 * ramfb displays a plain guest-RAM buffer: perfect stand-in for the watch,
 * whose MDP3/DSI driver will expose the same fb_init() contract on hardware.
 * Requires `-device ramfb` on the QEMU command line.
 *
 * fw_cfg MMIO on -M virt (DT node fw-cfg@9020000):
 *   +0x00 data (sequential stream reads/writes)
 *   +0x08 selector (16-bit, big-endian)
 *   +0x10 DMA address (64-bit big-endian; writing the low half triggers)
 * Config writes require the DMA interface (the data port is read-only-ish for
 * modern QEMU); everything in fw_cfg is big-endian.
 */
#include "platform.h"
#if defined(PLAT_BOARD_QEMU_VIRT)

#define FWCFG_BASE   0x09020000u
#define FWCFG_DATA   (FWCFG_BASE + 0x00)
#define FWCFG_SEL    (FWCFG_BASE + 0x08)
#define FWCFG_DMA_HI (FWCFG_BASE + 0x10)
#define FWCFG_DMA_LO (FWCFG_BASE + 0x14)

#define FW_CFG_FILE_DIR   0x0019
#define DMA_CTL_WRITE     0x10
#define DMA_CTL_SELECT    0x08

#define FOURCC_XR24  0x34325258u   /* DRM_FORMAT_XRGB8888 */

static inline void fwcfg_select(uint16_t key)
{
    *(volatile uint16_t *)FWCFG_SEL = __builtin_bswap16(key);
}
static inline uint8_t fwcfg_read8(void)
{
    return *(volatile uint8_t *)FWCFG_DATA;
}
static uint32_t fwcfg_read32_be(void)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | fwcfg_read8();
    return v;
}

struct __attribute__((packed)) fwcfg_dma {
    uint32_t control_be;
    uint32_t length_be;
    uint64_t address_be;
};

static void fwcfg_dma_write(uint16_t key, const void *buf, uint32_t len)
{
    static volatile struct fwcfg_dma dma __attribute__((aligned(16)));
    dma.control_be = __builtin_bswap32(((uint32_t)key << 16) | DMA_CTL_SELECT | DMA_CTL_WRITE);
    dma.length_be  = __builtin_bswap32(len);
    dma.address_be = __builtin_bswap64((uint32_t)(uintptr_t)buf);
    __asm__ volatile("dsb sy" ::: "memory");
    uint64_t dma_addr = (uint32_t)(uintptr_t)&dma;
    mmio_write(FWCFG_DMA_HI, __builtin_bswap32((uint32_t)(dma_addr >> 32)));
    mmio_write(FWCFG_DMA_LO, __builtin_bswap32((uint32_t)dma_addr));
    while (__builtin_bswap32(dma.control_be) & ~1u) { }   /* wait, ignore error bit pos */
}

struct __attribute__((packed)) ramfb_cfg {
    uint64_t addr_be;
    uint32_t fourcc_be;
    uint32_t flags_be;
    uint32_t width_be;
    uint32_t height_be;
    uint32_t stride_be;
};

/* Framebuffer: XRGB8888. Sized for the largest fleet panel (Gen 6 is 416x416;
 * Gen 4 firefish 390x390) — actual size set at fb_init. */
#define FB_MAX_W 480
#define FB_MAX_H 480
static uint32_t s_fb[FB_MAX_W * FB_MAX_H] __attribute__((aligned(4096)));
static uint32_t s_w_ramfb, s_h_ramfb;

void *fb_init(uint32_t w, uint32_t h)
{
    if (w > FB_MAX_W || h > FB_MAX_H) return 0;

    /* find "etc/ramfb" in the fw_cfg file directory */
    fwcfg_select(FW_CFG_FILE_DIR);
    uint32_t count = fwcfg_read32_be();
    uint16_t key = 0;
    for (uint32_t i = 0; i < count && !key; i++) {
        uint32_t size = fwcfg_read32_be(); (void)size;
        uint16_t sel = (uint16_t)((fwcfg_read8() << 8) | fwcfg_read8());
        fwcfg_read8(); fwcfg_read8();                     /* reserved */
        char name[57];
        for (int c = 0; c < 56; c++) name[c] = (char)fwcfg_read8();
        name[56] = 0;
        const char *want = "etc/ramfb";
        int match = 1;
        for (int c = 0; want[c] || name[c]; c++)
            if (want[c] != name[c]) { match = 0; break; }
        if (match) key = sel;
    }
    if (!key) { con_puts("ramfb: not found (add -device ramfb)\n"); return 0; }

    static struct ramfb_cfg cfg;
    cfg.addr_be   = __builtin_bswap64((uint32_t)(uintptr_t)s_fb);
    cfg.fourcc_be = __builtin_bswap32(FOURCC_XR24);
    cfg.flags_be  = 0;
    cfg.width_be  = __builtin_bswap32(w);
    cfg.height_be = __builtin_bswap32(h);
    cfg.stride_be = __builtin_bswap32(w * 4);
    fwcfg_dma_write(key, &cfg, sizeof cfg);

    s_w_ramfb = w; s_h_ramfb = h;
    bdiag_puts("ramfb: "); bdiag_putdec(w); bdiag_puts("x"); bdiag_putdec(h);
    bdiag_puts(" @ "); bdiag_puthex((uint32_t)(uintptr_t)s_fb); bdiag_puts("\n");
    return s_fb;
}

/* Accessors so board-neutral code (gfx_text.c) can render here too. */
uint32_t fb_width(void)  { return s_w_ramfb; }
uint32_t fb_height(void) { return s_h_ramfb; }
uint32_t fb_bpp(void)    { return 4; }
void    *fb_ptr(void)    { return s_fb; }
void     fb_flush_all(void) { }          /* ramfb scans DDR continuously */

#endif /* PLAT_BOARD_QEMU_VIRT */
