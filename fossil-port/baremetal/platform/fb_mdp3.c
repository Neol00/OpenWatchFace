/* fb_mdp3.c — Fossil Gen 4 (msm8909w) display path: take over aboot's MDP3
 * DMA_P splash and push our own frames through it.
 *
 * WHY THIS EXISTS (the Gen 6's most expensive lesson, applied up front).
 * The Gen 4's first display attempt was a full, blind MIPI-DSI bring-up:
 * PHY regulators, PHY timings, PLL, host init, then the panel's 18-command
 * power-on table (platform/msm_dsi.c + platform/dsi_panel.c). That stack has
 * no observable failure modes on a watch with no UART — every mistake in it,
 * from a wrong PHY timing to a swapped page-select, produces exactly one
 * symptom: a black screen. The Gen 6 spent its entire display campaign there
 * and only got pixels after giving up and inheriting the bootloader's state.
 *
 * So the Gen 4 starts where the Gen 6 finished:
 *
 *   aboot lights the panel for its splash and drives it from MDP3's DMA_P
 *   engine over DSI in command mode. That is the SAME engine, the SAME DSI
 *   host and the SAME panel state this firmware needs. Instead of
 *   reprogramming any of it, we read DMA_P's registers back, point
 *   DMA_P_IBUF_ADDR at a buffer of our own, and write DMA_P_START once per
 *   rendered frame. Nothing about DSI, the PHY, the PLL or the panel is
 *   touched, so none of it can be got wrong.
 *
 * The kernel sanctions exactly this: mdp3_dmap_config() takes a
 * `splash_screen_active` flag and skips every DMA_P register write when it
 * is set (drivers/video/msm/mdss/mdp3_dma.c).
 *
 * DIFFERENCE FROM THE GEN 6. There, aboot's splash buffer is XPU-protected
 * and a single CPU write to it hangs the bus, so the Gen 6 must own a buffer
 * and repoint the pipe. Here the same shape is used, but for a different
 * reason: firefish's own device tree marks splash_region@83000000 as a plain
 * reservation (no `no-map`, unlike external_image/modem/adsp/pheripheral
 * around it), so aboot's buffer is very likely writable — we still render
 * into our own .bss buffer, because that keeps the MDP reading memory whose
 * lifetime and cache state we control, and it costs nothing.
 *
 * FALLBACK LADDER, in order of how much we have to get right:
 *   1. takeover        — aboot left a believable DMA_P config: repoint it.
 *   2. reconfigure     — MDSS is clocked and DSI is live but DMA_P's config
 *                        is not what we need (e.g. aboot used RGB565):
 *                        reprogram DMA_P only, still no DSI/panel work.
 *   3. blind bring-up  — only with -DGEN4_DSI_INIT: dsi_init() + panel_on().
 *                        Off by default; this is the unobservable path.
 *   4. render-only     — no display hardware touched at all. The firmware
 *                        still boots and runs; nothing reaches the glass.
 * Every step down is reported through con_puts, and the chosen step is
 * readable in the ramlog after the fact.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

#include <stddef.h>

/* Our scanout buffer. Static, in .bss: the MDP is a bus master reading it
 * asynchronously, so it can never live on a stack or in the heap. XRGB8888
 * at the panel's full size — LVGL renders here in direct mode. */
static uint32_t s_owned_fb[PLAT_PANEL_W * PLAT_PANEL_H]
    __attribute__((aligned(4096)));

static uint32_t  s_w, s_h, s_stride, s_bpp = 4;
static uintptr_t s_fb;
static int       s_live;          /* 0 = render-only: never kick the MDP */
static uint32_t  s_kick_err;      /* 0 ok, 1 = DMA_P_DONE timeout        */
static uint32_t  s_kick_count;
static uint32_t  s_last_kick_ms;

/* How the display path was actually brought up — reported once, and the
 * single most useful number in the boot log when the glass stays black. */
enum { FB_PATH_NONE = 0, FB_PATH_TAKEOVER, FB_PATH_RECONFIG,
       FB_PATH_BLIND, FB_PATH_RENDER_ONLY };
static uint32_t s_path;

static void fill_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                      uint32_t xrgb);

static void render_only(const char *why)
{
    s_fb = (uintptr_t)s_owned_fb;
    s_w = PLAT_PANEL_W; s_h = PLAT_PANEL_H;
    s_bpp = 4; s_stride = s_w * s_bpp;
    s_live = 0;
    s_path = FB_PATH_RENDER_ONLY;
    con_puts("fb: render-only ("); con_puts(why);
    con_puts(") buf@"); con_puthex((uint32_t)s_fb); con_puts("\n");
}

void *fb_init(uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    bdiag_puts("fb: gen4 MDP3 DMA_P takeover path\n");
#if defined(WDOG_TRACE)
    wdog_stage(24);
#endif

#if defined(FB_NO_MDP)
    /* Bring-up safety flag: no display register is touched at all. */
    render_only("FB_NO_MDP");
    return (void *)s_fb;
#else
    /* MDSS clocks FIRST, always. Reading an unclocked MSM block does not
     * return an error — the AHB transaction never completes and the CPU
     * hangs there forever. This is the Gen 6's "touch an unclocked block"
     * failure, and a lit panel does not rule it out: a command-mode AMOLED
     * self-refreshes the bootloader's logo out of its own RAM with MDSS
     * completely dark. gcc_mdss_up() is safe to call either way (the CBCRs
     * live in the always-on GCC). */
    uint32_t st = gcc_mdss_up();
    if (!(st & GCC_MDSS_ST_GDSC_ON) || !(st & GCC_MDSS_ST_CORE_CLKS)) {
        render_only("mdss clocks down");
        return (void *)s_fb;
    }

    struct mdp3_splash_cfg sp;
    int have_splash = (mdp3_splash_probe(&sp) == 0);

    if (have_splash) {
        con_puts("fb: aboot DMA_P buf="); con_puthex(sp.addr);
        con_puts(" "); con_putdec(sp.w); con_puts("x"); con_putdec(sp.h);
        con_puts(" stride="); con_putdec(sp.stride);
        con_puts(" fmt="); con_putdec(sp.format);
        con_puts(" cfg="); con_puthex(sp.config); con_puts("\n");
    } else {
        con_puts("fb: no live DMA_P splash (cfg="); con_puthex(sp.config);
        con_puts(" addr="); con_puthex(sp.addr); con_puts(")\n");
    }

    s_fb = (uintptr_t)s_owned_fb;

    if (have_splash) {
        /* The panel, the DSI host, its PHY and its PLL are all live and stay
         * untouched. We program only the DMA_P block: format, geometry,
         * stride and buffer address.
         *
         * This DELIBERATELY reprograms DMA_P even when aboot's configuration
         * already looks like ours. An earlier version repointed the buffer
         * alone in that case, on the "touch the least" principle — but it
         * made the colour path depend on which of two branches ran, and
         * "which branch ran" is precisely what cannot be observed on a watch
         * with no log. Six register writes to one block, always the same
         * six, is both predictable and still nowhere near DSI territory.
         * (The kernel's own mdp3_dmap_config() skips these writes while the
         * splash is active; it can afford to, because it inherits a
         * configuration it also chose.) */
        /* GEOMETRY COMES FROM THE HARDWARE, NOT THE HEADER.
         *
         * This used to read `s_w = PLAT_PANEL_W`, discarding the geometry the
         * probe had just recovered. On the Gen 4 that was harmless because the
         * header's 454x454 was confirmed correct — the bug was invisible
         * because the two numbers agreed. On the TicWatch C2 they do not: the
         * panel node says 400x400 while the touch node says 360, and forcing
         * the wrong one makes DMA_P walk each row at the wrong stride, which
         * shears the image diagonally across the glass.
         *
         * aboot has already configured this panel correctly — its logo is on
         * the screen, which is proof. So believe DMA_P_SIZE over any
         * compile-time constant. The header value survives only as a fallback
         * for a probe that returned something impossible, and any disagreement
         * is reported rather than silently resolved, because a panel whose DT
         * and bootloader disagree is worth knowing about. */
        if (sp.w >= 64u && sp.w <= 1024u && sp.h >= 64u && sp.h <= 1024u) {
            s_w = sp.w; s_h = sp.h;
            if (sp.w != PLAT_PANEL_W || sp.h != PLAT_PANEL_H) {
                con_puts("fb: NOTE aboot geometry "); con_putdec(sp.w);
                con_puts("x"); con_putdec(sp.h);
                con_puts(" != header "); con_putdec(PLAT_PANEL_W);
                con_puts("x"); con_putdec(PLAT_PANEL_H);
                con_puts(" — using aboot's\n");
            }
        } else {
            s_w = PLAT_PANEL_W; s_h = PLAT_PANEL_H;
            con_puts("fb: probe geometry implausible, falling back to header\n");
        }
        s_bpp = 4; s_stride = s_w * s_bpp;
        mdp3_dma_config((const void *)s_fb, s_w, s_h);
        s_live = 1;
        s_path = FB_PATH_RECONFIG;
        con_puts("fb: RECONFIG (DMA_P reprogrammed, DSI untouched)\n");

#if defined(FB_PROBE_DUMP)
        /* Bring-up aid: put the numbers on the glass, because this watch has
         * no console and "what did aboot actually program?" is the question
         * every display fault here reduces to. Build with -DFB_PROBE_DUMP. */
        {
            static char d[160]; unsigned n = 0;
            const char *hex = "0123456789ABCDEF";
            #define P_STR(t) do { const char *q=(t); while(*q && n<sizeof d-1) d[n++]=*q++; } while(0)
            #define P_DEC(v) do { uint32_t x=(v); char t[12]; int k=0; \
                                  if(!x) t[k++]='0'; while(x){t[k++]=(char)('0'+x%10);x/=10;} \
                                  while(k && n<sizeof d-1) d[n++]=t[--k]; } while(0)
            #define P_HEX(v) do { uint32_t x=(v); for(int k=28;k>=0;k-=4) \
                                  if(n<sizeof d-1) d[n++]=hex[(x>>k)&0xF]; } while(0)
            P_STR("ABOOT "); P_DEC(sp.w); P_STR("x"); P_DEC(sp.h);
            P_STR("\nSTRIDE "); P_DEC(sp.stride);
            P_STR("\nFMT "); P_DEC(sp.format);
            P_STR("\nADDR "); P_HEX(sp.addr);
            P_STR("\nCFG "); P_HEX(sp.config);
            P_STR("\nHDR "); P_DEC(PLAT_PANEL_W); P_STR("x"); P_DEC(PLAT_PANEL_H);
            d[n] = 0;
            fb_text_dump(d);
            fb_flush_all();
            timer_delay_ms(8000u);
        }
#endif
    } else {
#if defined(GEN4_DSI_INIT)
        /* Step 3: the blind path. Only reachable when explicitly asked for,
         * because a failure here is indistinguishable from every other
         * failure here. */
        s_w = PLAT_PANEL_W; s_h = PLAT_PANEL_H;
        s_bpp = 4; s_stride = s_w * s_bpp;
        if (panel_full_init() == 0) {
            mdp3_dma_config((const void *)s_fb, s_w, s_h);
            s_live = 1;
            s_path = FB_PATH_BLIND;
            con_puts("fb: BLIND bring-up succeeded\n");
        } else {
            render_only("blind bring-up failed");
            return (void *)s_fb;
        }
#else
        render_only("no splash; -DGEN4_DSI_INIT not set");
        return (void *)s_fb;
#endif
    }

#if defined(WDOG_TRACE)
    wdog_stage(25);
#endif
    /* Clear our buffer. Note this leaves aboot's logo on the glass until the
     * first flush — which is useful: the moment the screen goes black is the
     * moment our first kick actually reached the panel. */
    for (uint32_t i = 0; i < s_w * s_h; i++) s_owned_fb[i] = 0;
#if defined(WDOG_TRACE)
    wdog_stage(26);
#endif

#if defined(FB_COLORTEST)
    /* A SELF-DESCRIBING TEST PATTERN, shown for a few seconds before LVGL
     * takes over. Point: "the colours look wrong" has at least four distinct
     * causes on this path — the DMA_P pack pattern (which component goes out
     * first), the source format field, the byte stride, and the panel's
     * column offset — and they are indistinguishable from a photo of a watch
     * face. This pattern separates them in one boot:
     *
     *   4 horizontal bands, top to bottom: RED, GREEN, BLUE, WHITE.
     *     Read off which band shows which colour and the pack pattern is
     *     known exactly (e.g. bands reading blue/green/red = R and B swapped
     *     = flip MDP3_DMA_OUTPUT_PACK_PATTERN between RGB 0x21 and BGR 0x12).
     *   A 4-pixel WHITE BORDER around the whole panel.
     *     A stride error makes it slant or wrap instead of framing squarely;
     *     a column-offset error clips one side.
     *   A WHITE DIAGONAL from top-left to bottom-right.
     *     Straight = stride correct. Sheared = stride wrong by that slope.
     */
    for (uint32_t y = 0; y < s_h; y++) {
        uint32_t band = (y * 4u) / s_h;
        uint32_t c = band == 0 ? 0x00FF0000u    /* red   */
                   : band == 1 ? 0x0000FF00u    /* green */
                   : band == 2 ? 0x000000FFu    /* blue  */
                               : 0x00FFFFFFu;   /* white */
        uint32_t *row = (uint32_t *)(s_fb + (uintptr_t)y * s_stride);
        for (uint32_t x = 0; x < s_w; x++) row[x] = c;
    }
    fill_rect(0, 0, s_w, 4, 0x00FFFFFFu);
    fill_rect(0, s_h - 4u, s_w, 4, 0x00FFFFFFu);
    fill_rect(0, 0, 4, s_h, 0x00FFFFFFu);
    fill_rect(s_w - 4u, 0, 4, s_h, 0x00FFFFFFu);
    for (uint32_t y = 0; y < s_h; y++) {
        uint32_t x = (y * s_w) / s_h;
        if (x < s_w)
            ((uint32_t *)(s_fb + (uintptr_t)y * s_stride))[x] = 0x00FFFFFFu;
    }
    fb_flush_all();
    con_puts("fb: colour test pattern up (5 s)\n");
    timer_delay_ms(5000);
    for (uint32_t i = 0; i < s_w * s_h; i++) s_owned_fb[i] = 0;
    fb_flush_all();
#endif

    return (void *)s_fb;
#endif /* FB_NO_MDP */
}

uint32_t fb_width(void)  { return s_w ? s_w : PLAT_PANEL_W; }
uint32_t fb_height(void) { return s_h ? s_h : PLAT_PANEL_H; }
uint32_t fb_bpp(void)    { return s_bpp; }
void    *fb_ptr(void)    { return (void *)s_fb; }

/* Make CPU writes visible to the MDP's bus master. mmu.c maps DDR as Normal
 * write-back, so without this the engine streams a stale frame. */
void fb_flush_region(const void *addr, uint32_t len)
{
    uintptr_t p   = (uintptr_t)addr & ~31u;
    uintptr_t end = ((uintptr_t)addr + len + 31u) & ~31u;
    for (; p < end; p += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory"); /* DCCMVAC */
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Push the whole framebuffer to the panel. Command mode: one explicit
 * transfer per frame, no continuous scanout. mdp3_flush() does the cache
 * clean, the IBUF repoint and the DMA_P_START, then waits for DMA_P_DONE. */
void fb_flush_all(void)
{
    if (!s_live) return;
    s_kick_err = (mdp3_flush((const void *)s_fb) == 0) ? 0u : 1u;
    s_kick_count++;
    s_last_kick_ms = timer_ms();
}

/* The panel holds the last frame it was given, so there is nothing to
 * refresh when the UI is idle — unlike the Gen 6, whose DSI link drifts.
 * Kept so the shared runtime can call it unconditionally. */
void fb_idle_refresh(uint32_t max_age_ms)
{
    (void)max_age_ms;
}

uint32_t fb_last_kick_err(void) { return s_kick_err; }
uint32_t fb_error_classes(void) { return 0u; }

/* --- on-glass debug primitives ------------------------------------------
 * With no UART bonded out on this watch, the panel IS the console. These
 * mirror the Gen 6's helpers so shared code (gfx_text.c, the bring-up
 * traces in main.c) links and behaves the same on both watches. */
static void fill_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                      uint32_t xrgb)
{
    if (!s_fb) return;
    for (uint32_t y = y0; y < y0 + h && y < s_h; y++) {
        uint32_t *row = (uint32_t *)(s_fb + (uintptr_t)y * s_stride);
        for (uint32_t x = x0; x < x0 + w && x < s_w; x++) row[x] = xrgb;
    }
}

void fb_trace(uint32_t xrgb)
{
    fill_rect(0, 0, s_w, s_h, xrgb);
    fb_flush_all();
}

void fb_dbg_mark(uint32_t idx, uint32_t xrgb)
{
    uint32_t side = s_w / 6u;
    fill_rect(s_w / 2u - side / 2u,
              (s_h / 4u) + idx * side, side, side, xrgb);
    fb_flush_all();
}

void fb_dbg_byte(uint32_t row, uint32_t val)
{
    uint32_t cell = s_w / 10u;
    uint32_t y = s_h / 2u + row * cell;
    for (uint32_t b = 0; b < 8; b++)
        fill_rect(cell + b * cell, y, cell - 2u, cell - 2u,
                  (val & (1u << (7u - b))) ? 0x00FFFFFFu : 0x00202020u);
    fb_flush_all();
}

void fb_perf_loop_tick(uint32_t body_ms) { (void)body_ms; }

#endif /* PLAT_SOC_MSM8909 */
