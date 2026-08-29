/* fb_splash.c — Fossil Gen 6 display path: take over aboot's MDP5 pipe.
 *
 * STRATEGY (rev 3, 2026-08-03). Two hardware facts forced this shape:
 *
 *   1. MDSS IS CLOCK-GATED AT HANDOFF. The first pipe probe wedged the AHB
 *      bus — the classic Gen-6 "touch an unclocked MSM block" hang. A lit
 *      panel proves nothing: the AUO AMOLED is command-mode and self-refreshes
 *      aboot's logo from its own RAM. Fix: gcc_mdss_up() (GDSC + every MDSS
 *      branch clock) runs before the first MDP register access, ever.
 *
 *   2. THE SPLASH BUFFER IS NOT OURS. aboot scans out of splash_region
 *      (0x90000000), which is XPU-protected against the CPU — a single write
 *      there hangs the bus (proven; mmu.c carves it out as Device-XN so it
 *      now faults visibly instead). So "paint aboot's buffer" can NEVER work
 *      on this watch. Instead we render into our OWN buffer in .bss and
 *      repoint the pipe's SRC0_ADDR at it. The MDP is a bus master reading
 *      non-secure DDR — our memory is exactly as scannable as aboot's.
 *
 * The pipe/mixer/CTL config aboot programmed is otherwise reused untouched;
 * fb_kick() latches it with CTL_FLUSH and fires CTL_START (mandatory on a
 * command-mode panel — the MDP only sends a frame when told to).
 *
 * Offsets verbatim from the hoki DTB (qcom,mdss_mdp@1a00000) + vendor hwio:
 *   qcom,mdss-ctl-off        = 0x2000 0x2200 0x2400   (CTL0 = 0x1a02000)
 *   qcom,mdss-pipe-vig-off   = 0x5000
 *   qcom,mdss-pipe-rgb-off   = 0x15000 0x17000
 *   qcom,mdss-pipe-dma-off   = 0x25000
 *   CTL: FLUSH = +0x18, START = +0x1C
 *   SSPP: SRC_SIZE +0x00, SRC0_ADDR +0x14, SRC_YSTRIDE +0x24, SRC_FORMAT +0x30
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#include <stddef.h>     /* NULL */

#define MDP_CTL0_OFF        0x2000u
#define MDP_CTL_FLUSH       0x018u
#define MDP_CTL_START       0x01Cu

/* CTL_FLUSH bits (mdss_mdp_ctl.c flush-bit construction) */
#define FLUSH_VIG0          (1u << 0)
#define FLUSH_RGB0          (1u << 3)
#define FLUSH_RGB1          (1u << 4)
#define FLUSH_LM0           (1u << 6)
#define FLUSH_DMA0          (1u << 11)
#define FLUSH_CTL           (1u << 17)

/* MDP pingpong 0 block (dump: mdss-pingpong-off) — used from fb_init and the
 * tear-check/autorefresh code below, so defined up here. */
#define PP0_OFF              0x71000u
#define PP_TEAR_CHECK_EN     0x000u
#define PP_SYNC_CONFIG_VSYNC 0x004u
#define PP_SYNC_CONFIG_HEIGHT 0x008u
#define PP_SYNC_WRCOUNT      0x00Cu
#define PP_VSYNC_INIT_VAL    0x010u
#define PP_SYNC_THRESH       0x018u
#define PP_START_POS         0x01Cu
#define PP_RD_PTR_IRQ        0x020u
#define PP_WR_PTR_IRQ        0x024u
#define PP_AUTOREFRESH_CFG   0x030u   /* MDSS_MDP_REG_PP_AUTOREFRESH_CONFIG */

/* SSPP (source pipe) registers */
#define MDP_PIPE_SRC_SIZE    0x0000u  /* (h << 16) | w */
#define MDP_PIPE_SRC0_ADDR   0x0014u  /* scanout buffer physical address */
#define MDP_PIPE_SRC_YSTRIDE 0x0024u  /* bytes per line, plane 0 in [15:0] */
#define MDP_PIPE_SRC_FORMAT  0x0030u  /* [10:9] = bytes-per-pixel - 1 */

static inline uintptr_t mdp_reg(uint32_t off)
{
    return (uintptr_t)PLAT_MDP_BASE + off;
}

/* Candidate source pipes + their CTL_FLUSH bits, most-likely first (LK uses
 * DMA0 or RGB0 for the splash on these targets). */
static const struct { uint32_t off, flush; } k_pipes[] = {
    { 0x25000u, FLUSH_DMA0 },
    { 0x15000u, FLUSH_RGB0 },
    { 0x17000u, FLUSH_RGB1 },
    { 0x05000u, FLUSH_VIG0 },
};

#define FB_MIN_DIM   64u
#define FB_MAX_DIM   1024u

/* Our scanout buffer. Static: the MDP streams from it forever after the
 * repoint, so it can never live on a task stack or the heap. Sized for the
 * panel at 4 bytes/px; a probed geometry that does not fit is rejected. */
static uint8_t s_owned_fb[PLAT_PANEL_W * PLAT_PANEL_H * 4]
    __attribute__((aligned(64)));

static void mdp_tearcheck_setup(void);
static void mdp_tearcheck_disable(void);
static void mdp_autorefresh_disable(void);

static uint32_t  s_w, s_h, s_stride, s_bpp = 4;
static uintptr_t s_fb;
static uint32_t  s_flush_bits;   /* 0 = no live pipe: render-only, no kick */
static uintptr_t s_pipe;         /* the SSPP we stole, for register readback */

static int dim_ok(uint32_t w, uint32_t h)
{
    return w >= FB_MIN_DIM && w <= FB_MAX_DIM && h >= FB_MIN_DIM && h <= FB_MAX_DIM;
}

static void fb_own_buffer_fallback(const char *why)
{
    s_fb = (uintptr_t)s_owned_fb;
    s_w = PLAT_PANEL_W; s_h = PLAT_PANEL_H;
    s_bpp = 4; s_stride = s_w * s_bpp;
    s_flush_bits = 0;
    con_puts("fb: no MDP takeover ("); con_puts(why);
    con_puts("); render-only buffer @"); con_puthex((uint32_t)s_fb);
    con_puts("\n");
}

/* Find the pipe aboot used and STEAL it: keep its geometry/format, point its
 * SRC0_ADDR at our own buffer. Requires MDSS clocks up (caller checks). */
static int splash_takeover(void)
{
    for (unsigned i = 0; i < sizeof k_pipes / sizeof k_pipes[0]; i++) {
        uintptr_t pipe = mdp_reg(k_pipes[i].off);
        uint32_t addr = mmio_read(pipe + MDP_PIPE_SRC0_ADDR);
        uint32_t size = mmio_read(pipe + MDP_PIPE_SRC_SIZE);
        uint32_t w = size & 0xFFFFu, h = (size >> 16) & 0xFFFFu;

        if (addr < PLAT_DDR_BASE || addr >= PLAT_DDR_BASE + PLAT_DDR_SIZE)
            continue;                       /* pipe not pointing at DDR */
        if (!dim_ok(w, h))
            continue;

        uint32_t fmt    = mmio_read(pipe + MDP_PIPE_SRC_FORMAT);
        uint32_t bpp    = ((fmt >> 9) & 3u) + 1u;
        uint32_t stride = mmio_read(pipe + MDP_PIPE_SRC_YSTRIDE) & 0xFFFFu;

        if (bpp < 2u) continue;             /* 1-byte formats: not a splash fb */
        if (stride < w * bpp || stride > FB_MAX_DIM * 8u) stride = w * bpp;
        if (stride * h > sizeof s_owned_fb) continue;   /* won't fit our buffer */

        s_w = w; s_h = h; s_stride = stride; s_bpp = bpp;
        s_pipe = pipe;
        s_fb = (uintptr_t)s_owned_fb;
        s_flush_bits = k_pipes[i].flush | FLUSH_LM0 | FLUSH_CTL;

        /* Repoint the scanout at our buffer. Everything else aboot set up
         * (mixer, CTL topology, DSI, panel) is reused as-is. */
        mmio_write(pipe + MDP_PIPE_SRC0_ADDR, (uint32_t)s_fb);
        __asm__ volatile("dsb sy" ::: "memory");

        bdiag_puts("fb: took pipe @+0x"); bdiag_puthex(k_pipes[i].off);
        bdiag_puts(" (aboot buf=");       bdiag_puthex(addr);
        bdiag_puts(") ");                 bdiag_putdec(w);
        bdiag_puts("x");                  bdiag_putdec(h);
        bdiag_puts(" bpp=");              bdiag_putdec(bpp);
        bdiag_puts(" stride=");           bdiag_putdec(stride);
        bdiag_puts("\n");
        return 0;
    }
    return -1;
}

void *fb_init(uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    bdiag_puts("fb: gen6 MDP5 pipe-takeover path\n");
#if defined(WDOG_TRACE) && defined(PLAT_SOC_MSM)
    wdog_stage(24);          /* fb_init entered */
#endif

#if defined(FB_NO_MDP)
    /* Display hardware access compiled out (bring-up safety flag): the whole
     * firmware runs, LVGL renders into the buffer, nothing reaches the glass. */
    fb_own_buffer_fallback("FB_NO_MDP");
#else
    uint32_t st = gcc_mdss_up();
    if ((st & GCC_MDSS_ST_CORE_CLKS) && !(st & GCC_MDSS_ST_DSI_CLKS)) {
        /* Campaign history (2026-08-03): display3 — aboot kills the DSI PLL
         * at handoff. display4 — relock locks the VCO but outputs stay dead.
         * display5 — PHY config replay alone still not enough: aboot's
         * shutdown PHY-RESETS the block, so the retained-config assumption
         * relock rests on is void. Therefore: full PHY config + full PLL
         * programming from scratch, both kernel-verbatim. */
        dsi_host_12nm_sw_reset();     /* clean lane/engine state FIRST */
        dsi_phy_12nm_config();
        if (dsi_pll_12nm_program() & DSI_PLL_ST_LOCKED)
            st = gcc_mdss_dsi_clks_retry();
        if (st & GCC_MDSS_ST_LINK_CLKS)
            dsi_host_12nm_reenable();
    }
    if (!(st & GCC_MDSS_ST_GDSC_ON) || !(st & GCC_MDSS_ST_CORE_CLKS)) {
        /* Domain or core clocks refused: MDP registers are untouchable. */
        fb_own_buffer_fallback("mdss clocks down");
    } else if (splash_takeover() == 0) {
        /* TE gating, both halves. mdp_tearcheck_setup() is the MDP/pingpong
         * half; dsi_host_12nm_trigger_setup() is the DSI half (TRIG_CTRL
         * TE_SEL). Only the MDP half was ever done, and dsi_dcs.c was actively
         * clearing the DSI half every frame — see dcs_dma_send(). Both are
         * needed for a command-mode panel with a real TE pin. */
        dsi_host_12nm_trigger_setup();
        /* Kernel order: autorefresh must be dealt with BEFORE the tear-check
         * registers are touched (mdss_mdp_cmd_tearcheck_setup does exactly
         * this first). If aboot left it on, this is the collision source. */
        mdp_autorefresh_disable();
        /* Reproduce the round-4 NOTE arm's REGISTER STATE exactly, which means
         * CONFIGURE the tear check and then DISABLE it — not skip it.
         *
         * dsi28 got this wrong and was much worse than the TE build: it never
         * called mdp_tearcheck_setup(), so the PP registers kept aboot's
         * values, and its disable path was guarded by `if (s_tc_on)` which is
         * 0 from boot — so TEAR_CHECK_EN was never cleared at all. If aboot
         * leaves tear check enabled, that build ran gated by aboot's stale
         * config: the worst of both worlds. "Configure then disable" and
         * "never configure" are different hardware states.
         *
         * So: program every PP register (and mux the TE pin), then clear
         * TEAR_CHECK_EN and TE_SEL UNCONDITIONALLY. */
        mdp_tearcheck_setup();
        /* ===== PANEL Hz: MEASURED, 45.4 (2026-08-06, dsi31) ================
         * The dsi31 experiment sent SET_TEAR_ON with all OUR gating off, and
         * the frame rate locked from ~74 fps (wire ceiling) to fps10=454 with
         * zero underflows and no visible tearing: with TE enabled the DDIC
         * itself flow-controls incoming writes to its scan. So the panel's
         * true refresh is ~45.4 Hz, and TEAR_ON alone is a complete, gate-free
         * tear-sync — remember that if tear-free 45 fps is ever wanted: it is
         * ONE DCS command, no tear-check/TE_SEL config at all.
         * (tehz10 read 0 in that test — the pingpong vsync counter only runs
         * while TEAR_CHECK_EN=1, so the RD_PTR probe never fired. The fps
         * lock-on measurement made it unnecessary.)
         *
         * USER'S CHOICE: maximum frame rate over tear-free — TE stays OFF. */
        g_dsi_te_on = 0u;
        dsi_dcs_set_tear(0);                       /* panel: SET_TEAR_OFF */
        mdp_tearcheck_disable();
        mmio_write(PLAT_DSI_CTRL_BASE + 0x084u,
                   mmio_read(PLAT_DSI_CTRL_BASE + 0x084u) & ~(1u << 31));
        __asm__ volatile("dsb sy" ::: "memory");
    } else {
        /* Clocks fine but no believable pipe config — aboot's state was lost
         * (GDSC power-cycled resets MDSS). Full MDP5+DSI init is the only way
         * to pixels from here; render-only keeps the firmware alive. */
        fb_own_buffer_fallback("no live pipe");
    }
#endif /* FB_NO_MDP */

    if (s_stride != s_w * s_bpp) {
        con_puts("fb: padded stride unsupported by the direct-mode buffer\n");
        return NULL;
    }

#if defined(WDOG_TRACE) && defined(PLAT_SOC_MSM)
    wdog_stage(25);          /* geometry settled; about to touch the buffer */
#endif

    /* Clear OUR buffer to black (never aboot's — see header point 2). */
    volatile uint8_t *fb = (volatile uint8_t *)s_fb;
    for (uint32_t i = 0; i < s_stride * s_h; i++) fb[i] = 0;

#if defined(WDOG_TRACE) && defined(PLAT_SOC_MSM)
    wdog_stage(26);          /* framebuffer cleared successfully */
#endif
    return (void *)s_fb;
}

/* Geometry/format accessors: size LVGL to what was actually detected. */
uint32_t fb_width(void)  { return s_w ? s_w : PLAT_PANEL_W; }
uint32_t fb_height(void) { return s_h ? s_h : PLAT_PANEL_H; }
uint32_t fb_bpp(void)    { return s_bpp; }
void    *fb_ptr(void)    { return (void *)s_fb; }

/* Make CPU writes visible to the MDP's bus master: clean D-cache by MVA.
 * mmu.c maps DDR Normal-cacheable, so skipping this shows a stale frame. */
void fb_flush_region(const void *addr, uint32_t len)
{
    uintptr_t p   = (uintptr_t)addr & ~31u;
    uintptr_t end = ((uintptr_t)addr + len + 31u) & ~31u;
    for (; p < end; p += 32)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory"); /* DCCMVAC */
    __asm__ volatile("dsb sy" ::: "memory");
}

/* MDP5 interrupt block (MDP base = MDSS + 0x1000, mdss_mdp_hwio.h):
 * INTR_STATUS 0x14 / INTR_CLEAR 0x18; PING_PONG_0_DONE = bit 8.
 * DSI controller INT_CTRL @0x110: CMD_MDP_DONE status = bit 8 (W1C). */
#define MDP_INTR_STATUS      0x1014u
#define MDP_INTR_CLEAR       0x1018u
#define MDP_INTR_PP0_DONE    (1u << 8)
#define DSI_INT_CTRL         0x0110u
#define DSI_INTR_CMD_MDP_DONE (1u << 8)

/* Panel-refresh counter: PP0_RD_PTR (INTR bit 12) fires once per real DDIC
 * scan (TE resets the vsync counter, which then crosses PP_RD_PTR_IRQ=1).
 * Polled — no IRQ wiring needed — from the kick wait loops and idle refresh;
 * at 74 kicks/s the poll coverage is near-continuous. W1C on sight. */
#define MDP_INTR_PP0_RD_PTR  (1u << 12)
static uint32_t s_te_count;
static inline void te_poll(void)
{
    if (mmio_read(mdp_reg(MDP_INTR_STATUS)) & MDP_INTR_PP0_RD_PTR) {
        mmio_write(mdp_reg(MDP_INTR_CLEAR), MDP_INTR_PP0_RD_PTR);
        s_te_count++;
    }
}

static uint32_t s_kick_err;     /* 0 ok, 1 = pingpong timeout, 2 = DSI timeout */
static uint32_t s_err_classes;  /* b0 ack-err, b1 timeout, b2 phy, b3 fifo */
static uint32_t s_recover_count; /* real FIFO-error recoveries (both-half reset) */
/* Last census sample, reported once per second by the DSI line (see below). */
static uint32_t s_fifo_raw;      /* FIFO_STATUS as read */
static uint32_t s_fifo_masked;   /* & kernel 4-lane error mask */
static uint32_t s_fifo_live;     /* masked, minus this link's at-rest baseline */
static uint32_t s_cls_win;       /* error classes seen in the current 1 s window */
static uint32_t s_tmo_win;       /* OR of TIMEOUT_STATUS over the window        */
static uint32_t s_telc_min = 0xFFFFFFFFu, s_telc_max;  /* PP0 line-count range */

/* ---- WHERE DOES THE TIME GO? (round 6) -----------------------------------
 * The frame rate DECAYS from ~45-60 fps to ~20 and then snaps back. Something
 * inside fb_kick grows. Rather than guess a fifth time, time each phase with
 * the arch timer and report per-kick averages plus the worst single kick, so
 * the decay is attributable to a specific wait. */
static uint64_t s_us_repin, s_us_pp0, s_us_mdp, s_us_recov;
static uint32_t s_us_worst;

static inline uint64_t tk_now(void) { return timer_ticks(); }
static inline uint32_t tk_us(uint64_t d)
{
    uint64_t f = timer_freq_hz();
    return f ? (uint32_t)((d * 1000000u) / f) : 0u;
}
uint32_t fb_last_kick_err(void) { return s_kick_err; }
uint32_t fb_error_classes(void) { return s_err_classes; }

/* Push a frame to the panel — a full command-mode TRANSACTION, not a bare
 * START. display6 lesson (solid white instead of 1 Hz flashing): the first
 * kickoff lands, then the latched PING_PONG_0_DONE / CMD_MDP_DONE status
 * bits make the engines report busy forever and every later START is
 * swallowed. The kernel's kickoff path (mdss_mdp_cmd_kickoff +
 * mdss_dsi_cmd_mdp_busy) waits for and ACKS both after every frame. */
static uint32_t s_kick_count;
static uint32_t s_last_kick_ms;

/* ---- MDP pingpong tear check (2026-08-04) --------------------------------
 * THE PER-KICK DRIFT, third act: with repin removed the UI still slips a
 * small constant per transfer — we fire CTL_START at arbitrary times, but
 * the vendor stack NEVER does: command-mode kickoffs are gated on the
 * panel's TE signal via the pingpong tear-check block (the panel DT demands
 * it: te-using-te-pin, tear-check-frame-rate 4500). A transfer colliding
 * with the DDIC's self-refresh scan is exactly the kind of thing that costs
 * its write pointer a few bytes each time. Setup is kernel-verbatim
 * (mdss_mdp_cmd_tearcheck_cfg): PP0 at MDP+0x71000 (dump: mdss-pingpong-off),
 * vsync counter = 19.2 MHz / (vtotal 420 * 45 fps) = 1015 with refx100=4500
 * cancelling out, TE pin = TLMM gpio24 muxed to mdp_vsync (dump: pmx_mdss_te,
 * PINGROUP(24, mdp_vsync..) = func 1) — aboot leaves it unmuxed. */
static uint32_t s_tc_on;
static uint32_t s_aref_boot;     /* AUTOREFRESH_CONFIG as found at takeover */

/* ===== SPLASH AUTOREFRESH (2026-08-06) =====================================
 * From the hoki kernel itself, mdss_mdp_intf_cmd.c
 * mdss_mdp_cmd_tearcheck_setup():
 *
 *     // Disable auto refresh mode, if enabled in splash to avoid corruption.
 *     if (pingpong_read(PP_AUTOREFRESH_CONFIG) & BIT(31)) { ... }
 *
 * PP_AUTOREFRESH_CONFIG (PP0 + 0x030): bit31 = enable, [15:0] = frame count.
 * When set, the MDP HARDWARE fires command-mode transfers BY ITSELF every N
 * vsync-counter frames — no CTL_START needed. Bootloaders enable it so the
 * splash keeps refreshing a command panel with zero software. The port never
 * read or wrote this register, so if aboot left it on, an autonomous transfer
 * stream has been colliding with every fb_kick on the one lane from day one:
 *   - two interleaved transfers stall the pixel supply mid-line
 *     -> DLN0_HS_FIFO_UNDERFLOW, HS_TX spending >1 frame budget (both measured)
 *   - a truncated line displaces every following line
 *     -> wrapped image, byte-phase-shifted colours, garbage pixels
 *   - the census recovery storm stacks 50/60 ms timeouts -> multi-second freezes
 *   - and the per-frame recovery being "load-bearing" fits too: each CTL/DSI
 *     reset breaks the in-flight collision before the next kick.
 * The kernel's disable sequence, ported: (1) stop the rd-ptr trigger from the
 * external TE, (2) wait out any in-flight transfer via PP0_DONE, (3) clear the
 * config. Called from fb_init BEFORE tear-check setup, kernel order. */
static void mdp_autorefresh_disable(void)
{
    uintptr_t pp = (uintptr_t)PLAT_MDP_BASE + PP0_OFF;
    uint32_t cfg = mmio_read(pp + PP_AUTOREFRESH_CFG);
    s_aref_boot = cfg;
    bdiag_puts("fb: pp0 autorefresh cfg="); bdiag_puthex(cfg);
    bdiag_puts(cfg & (1u << 31) ? "  ** SPLASH AUTOREFRESH ON **\n" : "\n");
    if (!(cfg & (1u << 31)))
        return;

    /* 1. rd-ptr trigger from external TE off (kernel __disable_rd_ptr_from_te) */
    uint32_t sc = mmio_read(pp + PP_SYNC_CONFIG_VSYNC);
    mmio_write(pp + PP_SYNC_CONFIG_VSYNC, sc & ~(1u << 20));
    __asm__ volatile("dsb sy" ::: "memory");

    /* 2. let any in-flight autorefresh transfer retire (PP0_DONE). If none is
     * in flight this just burns the timeout once at boot — harmless. */
    mmio_write(mdp_reg(MDP_INTR_CLEAR), MDP_INTR_PP0_DONE);
    __asm__ volatile("dsb sy" ::: "memory");
    {
        uint32_t t0 = timer_ms();
        while (!(mmio_read(mdp_reg(MDP_INTR_STATUS)) & MDP_INTR_PP0_DONE)) {
            if ((uint32_t)(timer_ms() - t0) > 60u) break;
        }
        mmio_write(mdp_reg(MDP_INTR_CLEAR), MDP_INTR_PP0_DONE);
    }

    /* 3. autorefresh OFF. (The TE rd-ptr bit is left for mdp_tearcheck_setup /
     * mdp_tearcheck_disable to own — they run right after us in fb_init.) */
    mmio_write(pp + PP_AUTOREFRESH_CFG, 0u);
    __asm__ volatile("dsb sy" ::: "memory");
    bdiag_puts("fb: splash autorefresh DISABLED\n");
}

static void mdp_tearcheck_setup(void)
{
#if !defined(NO_TEARCHECK)
    uintptr_t pp = (uintptr_t)PLAT_MDP_BASE + PP0_OFF;
    uint32_t yres = s_h ? s_h : PLAT_PANEL_H;

    tlmm_cfg(24u, 1u, 1u /* pull-down */, 2u, 0);   /* TE -> mdp_vsync */

    mmio_write(pp + PP_SYNC_CONFIG_VSYNC,
               (1u << 19) | (1u << 20) | 1015u);  /* en counter | hw TE | vclks/line */
    mmio_write(pp + PP_SYNC_CONFIG_HEIGHT, 0xFFF0u);
    mmio_write(pp + PP_VSYNC_INIT_VAL, yres);
    mmio_write(pp + PP_RD_PTR_IRQ,  yres + 1u);
    mmio_write(pp + PP_WR_PTR_IRQ,  0u);
    mmio_write(pp + PP_START_POS,   yres);
    mmio_write(pp + PP_SYNC_THRESH, (4u << 16) | 4u);
    mmio_write(pp + PP_SYNC_WRCOUNT, yres + 4u + 1u);
    mmio_write(pp + PP_TEAR_CHECK_EN, 1u);
    __asm__ volatile("dsb sy" ::: "memory");
    s_tc_on = 1;
    bdiag_puts("fb: tear check armed (TE gpio24)\n");
#endif
}

/* ---- UNDERFLOW ARM SWEEP (2026-08-06) ------------------------------------
 * The TE_SEL fix landed (census showed trig=0x80000004) and changed nothing:
 * DLN0 still reports HS_FIFO_UNDERFLOW on ~half the kicks. The log did settle
 * one long-standing question though — the FIFO nibbles are per lane, 4 bits
 * each from bit 16 (b0 EMPTY, b1 FULL, b2 OVERFLOW, b3 UNDERFLOW):
 *
 *   at rest   raw=0x11111000 -> DLN0..3 nibble 0x1 = EMPTY, all four
 *   errored   raw=0x11191080 -> DLN0 nibble 0x9 = EMPTY|UNDERFLOW; DLN1..3
 *                               stay 0x1
 *
 * Only the ONE active lane ever shows the underflow bit, so this is a REAL
 * single-lane underflow, not the 1-lane phantom we chased twice. It also means
 * the recorded "baseline" (0x00080000, sampled on the very first kick) was a
 * real error captured at t=0, which is why `live` prints 0x0 forever and is
 * worthless. Suppressing recovery was always wrong for this reason — the error
 * is genuine, and that is why the panel froze both times we tried.
 *
 * Flashing is the bottleneck, so instead of one guess per image this sweeps
 * the remaining hypotheses automatically: each arm runs for one census window,
 * the window reports kicks/recov for THAT arm, and the cycle repeats so every
 * arm gets repeated measurements. Read the resulting table by arm name. */
/* Sweep round 1 verdict (measured on glass, underflows per 1000 kicks):
 *     BASE 592 | NOREPIN 805,900 | MDP320 769 | MDP400 438 | DRAIN 261 |
 *     NOBURST 272
 * NOREPIN was the WORST arm twice over, so the per-frame re-pin is genuinely
 * load-bearing and the "DCS re-pin is the problem" theory is dead. The MDP
 * clock arms were non-monotonic (320 worse than both 160 and 400) = noise, not
 * a bandwidth ceiling. DRAIN more than halved the rate and is principled, so
 * it is now ON BY DEFAULT.
 *
 * Round 2 is a clean 2x2 factorial over the two surviving effects, so each is
 * measured with and without the other instead of one-at-a-time against a
 * drifting control:
 *     HS timer  = kernel 0x3fd08  vs  aboot 0x0000ffff
 *     DCS drain = on              vs  off
 * Everything else is held fixed (mdp 160 MHz, burst on, re-pin on).
 * Expectation if the HS_TX timeout really is the root cause: both HSTMR arms
 * collapse toward ~0 regardless of drain, and BOTHOFF reproduces the old ~600. */
/* Round 2 was VOID as an experiment — every arm read back hstmr=0x3fd08
 * because the recovery path re-runs dsi_host_12nm_trigger_setup() on each
 * underflow and overwrote the arm's value. It still taught us two things:
 * with the correct one-frame HS timeout permanently installed the underflow
 * rate stayed at 350-870/1000, and HS_TX_TIMEOUT (tmo bit0) STILL fires. A
 * frame is therefore genuinely taking longer than 13.46 ms of HS time: the
 * timeout is a symptom of a stall in the pixel supply, not its cause.
 *
 * What is left. gcc-mdss reported status=0xf (DSI_CLKS up), so the
 * `if (CORE && !DSI)` branch in fb_init never ran: dsi_pll_12nm_program() and
 * dsi_host_12nm_reenable() are NOT executed on this boot and the PLL/pclk
 * config in use is aboot's own, untouched — the clock-mismatch theories are
 * dead too. The remaining difference between aboot's working splash and us is
 * not configuration at all: aboot pushed ONE frame with an idle CPU, while we
 * push continuously with LVGL, USB and FreeRTOS all hammering DDR. An MDP
 * starved of DDR bandwidth underflows exactly like this, and it is the only
 * theory that also explains the wild window-to-window variance (348 -> 900)
 * that no static misconfiguration can.
 *
 * ROUND 3 tests that directly, and needs no kernel source:
 *   QUIET    control
 *   DDRLOAD  hammer DDR with a memcpy burst INSIDE the PP0_DONE wait, i.e.
 *            precisely while the MDP is fetching the frame. If contention is
 *            the mechanism the rate must jump hard against QUIET.
 *   NOTEAR   tear check off on both sides. telc has never advanced past its
 *            init value, so TE may be gating us onto a signal that is not
 *            arriving and stretching every transfer.
 *   HSTMRLO  aboot's 0xffff — now that g_dsi_hstmr is honoured this finally
 *            tests what round 2 could not, as a sanity check on the harness. */
/* Round 3 verdict (underflows per 1000 kicks, two passes each):
 *     QUIET 385,841 | DDRLOAD 539,315 | NOTEAR 238,230 | HSTMRLO 454,809
 * DDRLOAD did NOT raise the rate (315 on the second pass, below QUIET's own
 * range) => DDR contention is DEAD, and with it the MDP-QoS theory.
 * NOTEAR produced the two lowest numbers in the run and was the only arm with
 * low variance => the tear check was actively HURTING.
 *
 * The cause turned out to be a missing panel command, not a register: the
 * vendor on-command table sends DCS 0x35 SET_TEAR_ON (param 0x02) and we never
 * did, so the DDIC was never driving TE at all. See dsi_dcs_set_tear(). That
 * is why `telc` has never advanced in ANY run, and why gating on it hurt.
 *
 * ROUND 4 is the 2x2 that settles it: panel TE on/off x tear check on/off.
 *   TEON     TE + tear check   <- the candidate fix; should be LOW and, unlike
 *                                 NOTEAR, actually synchronised
 *   TEON_NT  TE, no tear check
 *   NOTE     no TE, no tear check  <- round 3's best (230-238), the control
 *   OLD      no TE + tear check    <- the broken configuration we shipped
 * The decisive readout is not the rate but `telcmin/telcmax`: the pingpong
 * line counter MUST sweep a real range in the TE arms. If it still does not
 * move after 0x35, the TE wire itself is the problem, not the command. */
/* ---- ROUND 5: are we simply OVER-DRIVING the link? -----------------------
 * DCS 0x35 (round 4) made the panel drive TE and the frame rate jumped, but
 * the glass now shows a clear LIMIT CYCLE: smooth and fast -> progressively
 * laggier -> flicker, drift, freezes, worsening -> sudden snap back to fast.
 * That is the signature of a saturated feedback loop, not a static bug.
 *
 * The arithmetic says we are asking for far more than the link can carry:
 *   frame        = 416*416*3      = 519168 bytes
 *   link         = 308.65536/8    =  38.58 MB/s  (1 lane)
 *   => one frame = 13.46 ms of PURE DATA, i.e. ~74 fps at 100%% duty
 *   LV_DEF_REFR_PERIOD = 8 ms     = 125 Hz requested
 * So LVGL asks for ~1.7x what the wire can deliver and the link never gets an
 * idle moment. Worse, at the ~24 fps we actually achieve each push takes ~40 ms
 * to cross the screen while the DDIC re-scans its RAM every 22.2 ms (45 Hz):
 * the read pointer laps the write pointer about twice per frame, so the glass
 * shows a walking mix of old and new content. The vendor runs 13.46 ms
 * transfers at 45 fps — about 60%% duty — and never hits this.
 *
 * Sweep a floor on the inter-kick interval. If the limit cycle is over-drive,
 * there is a rate at which ufl/1000 collapses AND the glass stays stable. */
#define ARM_RATE_FREE 0u   /* no floor — current behaviour                   */
#define ARM_RATE22    1u   /* 45 fps = the panel's own framerate (DT 0x2d)   */
#define ARM_RATE33    2u   /* 30 fps                                         */
#define ARM_RATE50    3u   /* 20 fps — deliberately conservative             */

#define ARM_COUNT    4u

volatile uint32_t g_fb_wait_us;   /* monotonic: us spent blocked in fb_kick
                                   * waits — read (as deltas) by pwr_diag.c */
volatile uint32_t g_dsi_no_repin;
volatile uint32_t g_dsi_dcs_drain = 1u;      /* round 1: halved the rate */
volatile uint32_t g_dsi_hstmr     = 0x3FD08u; /* kernel one-frame budget  */
volatile uint32_t g_dsi_te_on     = 1u;      /* DCS 0x35 sent / to re-assert */

/* ROUND 6 arms. The rate floor is gone (harmful). What is left that could
 * GROW over time and then release:
 *   BASE      everything as shipped in round 4 (the ~45-60 fps image) + timing
 *   NODRAIN   the DCS drain I added caps at 10 ms and repin_window sends FOUR
 *             packets per frame; if the lane stays busy that is 40 ms/frame,
 *             which alone would explain 50 fps -> 20 fps
 *   NOFULL    stop the periodic repin_full every 8 kicks (7 extra DCS packets)
 *   NOPTL     keep repin_full but drop DCS 0x12 PTLON from it — the vendor
 *             sends partial-mode-on ONCE at init, never to a running panel */
/* ===== ROUND 7: SWEEP OVER, CONFIGURATION FROZEN (2026-08-06) ==============
 *
 * The round-4 log contained the only perfectly clean window ever recorded on
 * this watch, and it went unnoticed at the time:
 *
 *   arm=NOTE kicks=372 recov=0 ufl/1000=0 cls=0x0 tmo=0x0  te=0 tc=0
 *   vs TEON  kicks=181            ufl=403
 *   vs TEON_NT kicks=85           ufl=258
 *
 * ZERO underflows, zero recoveries, zero error classes, and more than double
 * the frame rate of any other arm. The winning configuration is panel TE OFF
 * and no tear-check gating on either side.
 *
 * That is counter-intuitive but the data is unambiguous, and `telc=0..416` in
 * the TE arms proves the panel genuinely IS driving TE after DCS 0x35 — so
 * this is not a dead-wire artefact. Gating on TE is what produces the errors:
 * our tear-check parameters (start_pos/threshold/vclks_line in
 * mdp_tearcheck_setup) must fire the transfer at the worst possible phase of
 * the DDIC's scan, and round 6 measured the cost directly — pp0 = 19400-23300
 * us per kick, i.e. a 13.46 ms transfer padded out to ~20 ms by the TE wait.
 *
 * It also explains the user-reported "limit cycle" in the round-4/5 images:
 * smooth-and-fast then progressive decay then a sudden snap back to perfect
 * was the SWEEP CYCLING THROUGH ITS ARMS every 40 s, with NOTE as the good
 * window. There was never a hardware oscillation to chase.
 *
 * So: freeze the configuration, drop the cycling, keep the census.
 * Everything here is the exact NOTE arm, not a recombination — drain stays ON
 * because that is what NOTE ran with, even though round 6's NODRAIN arm looked
 * better under TE gating (260/143 vs 478/645). Do not mix results across arms
 * that differ in TE state; that is how round 2 went wrong. */
#define ARM_STABLE   0u

static const char *const k_arm_name[ARM_COUNT] = {
    "STABLE", "STABLE", "STABLE", "STABLE"
};
static uint32_t s_arm;

static uint32_t s_min_kick_ms;   /* retired (round 5): kept at 0 */
static uint32_t s_skipped;

volatile uint32_t g_dsi_drain_timeouts;
volatile uint32_t g_dsi_repin_full_n = 8u;
volatile uint32_t g_dsi_partial_on   = 1u;

/* Put the display into the named arm's configuration. Every arm sets EVERY
 * knob, so an arm never inherits the previous one's state.
 *
 * Everything proven in rounds 1-4 is now HELD FIXED and no longer swept:
 * re-pin on, DCS drain on, kernel HS timer, panel TE on (DCS 0x35), tear check
 * on. The only variable is the frame-rate floor. */
static void arm_apply(uint32_t a)
{
    (void)a;                        /* single frozen configuration now */

    g_dsi_no_repin     = 0u;        /* per-frame re-pin ON  (round 1)  */
    g_dsi_dcs_drain    = 1u;        /* as NOTE ran it                  */
    g_dsi_repin_full_n = 8u;        /* as NOTE ran it                  */
    g_dsi_partial_on   = 1u;        /* as NOTE ran it                  */
    s_min_kick_ms      = 0u;        /* never rate-limit (round 5)      */

    g_dsi_hstmr = 0x3FD08u;
    mmio_write(PLAT_DSI_CTRL_BASE + 0x0BCu, g_dsi_hstmr);

    /* No TE GATING on either side (both disables UNCONDITIONAL — dsi28's
     * `if (s_tc_on)` guard meant TEAR_CHECK_EN was never actually cleared).
     * The panel's TE OUTPUT itself stays ON now purely for the Hz census;
     * gating remains off, so the transfer path is the fast configuration. */
    mdp_tearcheck_disable();
    mmio_write(PLAT_DSI_CTRL_BASE + 0x084u,
               mmio_read(PLAT_DSI_CTRL_BASE + 0x084u) & ~(1u << 31));

    gcc_mdss_set_mdp_cfg(0x109u);
    mmio_write(PLAT_DSI_CTRL_BASE + 0x1B8u,
               mmio_read(PLAT_DSI_CTRL_BASE + 0x1B8u) | (1u << 16)); /* burst on */
    /* Autorefresh must STAY off — unconditional, same rule as the tear-check
     * disable above (a guarded write is how dsi28 regressed). */
    mmio_write((uintptr_t)PLAT_MDP_BASE + PP0_OFF + PP_AUTOREFRESH_CFG, 0u);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Clear TEAR_CHECK_EN unconditionally. Separate from the s_tc_on bookkeeping
 * on purpose: dsi28 guarded the register write with `if (s_tc_on)`, which is 0
 * from boot, so it never cleared aboot's tear-check enable at all. */
static void mdp_tearcheck_disable(void)
{
    mmio_write((uintptr_t)PLAT_MDP_BASE + PP0_OFF + PP_TEAR_CHECK_EN, 0u);
    __asm__ volatile("dsb sy" ::: "memory");
    s_tc_on = 0;
}

static void fb_kick(void)
{
    s_last_kick_ms = timer_ms();
    if (!s_flush_bits) { s_kick_err = 3; return; }  /* render-only: no pipe */
    uint64_t k_t0 = tk_now(), k_ph;

    /* display9 lesson: re-running dsi_host_12nm_reenable() before EVERY kick
     * made things WORSE (1 frame vs display8's 3) — rewriting DSI_CTRL and
     * the stream registers between transfers disturbs the link. Host setup
     * is once-only (fb_init); per-frame we only handle completion status. */

    /* THE DRIFT FIX rev 2 (2026-08-04): re-pin the DDIC RAM window before
     * EVERY frame, exactly like the kernel's cmd-mode partial-update path
     * sends the column/page addresses per kickoff. Rev 1's "every 64 kicks"
     * missed that LVGL only kicks on dirty content (a clock face can kick
     * once a MINUTE) — the drift stepped far faster than the repin. Per-frame
     * costs two short DMA packets on an idle link (~µs) and guarantees every
     * frame we push lands in the canonical window no matter what the DDIC's
     * state has drifted to. */
#if !defined(NO_REPIN)
    k_ph = tk_now();
    if (!g_dsi_no_repin) dsi_dcs_repin_window();
    s_us_repin += tk_now() - k_ph;
#endif

    /* drop any stale completion status from the previous pass (or aboot) */
    mmio_write(mdp_reg(MDP_INTR_CLEAR), MDP_INTR_PP0_DONE);
    uint32_t ic = mmio_read(PLAT_DSI_CTRL_BASE + DSI_INT_CTRL);
    mmio_write(PLAT_DSI_CTRL_BASE + DSI_INT_CTRL, ic | DSI_INTR_CMD_MDP_DONE);
    __asm__ volatile("dsb sy" ::: "memory");

    mmio_write(mdp_reg(MDP_CTL0_OFF + MDP_CTL_FLUSH), s_flush_bits);
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(mdp_reg(MDP_CTL0_OFF + MDP_CTL_START), 1);
    __asm__ volatile("dsb sy" ::: "memory");

    /* wait for the MDP to finish pushing the frame into the panel RAM ... */
    uint32_t t0 = timer_ms();
    k_ph = tk_now();
    s_kick_err = 0;
    while (!(mmio_read(mdp_reg(MDP_INTR_STATUS)) & MDP_INTR_PP0_DONE)) {
        /* Sleep between polls instead of hammering the AHB (the 100%-duty
         * bug, irq.c): the transfer takes ~13 ms and the tick re-wakes us
         * every 1 ms, so completion is seen at most ~1 ms late (~0.5 ms
         * average — worth ~3 fps off the 74 fps ceiling for a core that is
         * asleep instead of spinning ~13 ms per frame). -DFB_SPIN_WAIT
         * restores the pure spin if peak fps ever matters more than heat. */
#if !defined(FB_SPIN_WAIT)
        if (g_tick_armed) __asm__ volatile("wfi");
#endif
        te_poll();
        /* Sample the pingpong line counter WHILE the transfer is in flight —
         * a single sample at window end can't distinguish "parked" from
         * "happened to be read at the same phase". */
        {
            uint32_t lc = mmio_read((uintptr_t)PLAT_MDP_BASE + PP0_OFF + 0x02Cu);
            if (lc < s_telc_min) s_telc_min = lc;
            if (lc > s_telc_max) s_telc_max = lc;
        }
        if ((uint32_t)(timer_ms() - t0) > 60u) {
            if (s_tc_on) {
                /* TE never gated us through (pin unrouted on this unit?) —
                 * drop back to unsynced kicks rather than freeze. */
                mmio_write((uintptr_t)PLAT_MDP_BASE + PP0_OFF + PP_TEAR_CHECK_EN, 0u);
                s_tc_on = 0;
                /* Drop the DSI half of the TE gating too, or we would trade a
                 * stalled pingpong for a stalled DSI stream. TE_SEL is only
                 * safe while the TE pin is proven to be arriving; if it is
                 * not, this returns the link to exactly its pre-fix
                 * behaviour rather than freezing. */
                mmio_write(PLAT_DSI_CTRL_BASE + 0x084u,
                           mmio_read(PLAT_DSI_CTRL_BASE + 0x084u) & ~(1u << 31));
                __asm__ volatile("dsb sy" ::: "memory");
                con_puts("fb: tear check TIMED OUT - disabled, unsynced kicks"
                         " (DSI TE_SEL dropped too)\n");
                mmio_write(mdp_reg(MDP_CTL0_OFF + MDP_CTL_START), 1);
                __asm__ volatile("dsb sy" ::: "memory");
                t0 = timer_ms();
                continue;
            }
            s_kick_err = 1; return;
        }
    }
    mmio_write(mdp_reg(MDP_INTR_CLEAR), MDP_INTR_PP0_DONE);
    s_us_pp0 += tk_now() - k_ph;
    g_fb_wait_us += tk_us(tk_now() - k_ph);   /* PWR census: display wait */

    /* ... and the DSI to retire the MDP-stream transfer */
    t0 = timer_ms();
    k_ph = tk_now();
    while (!(mmio_read(PLAT_DSI_CTRL_BASE + DSI_INT_CTRL) & DSI_INTR_CMD_MDP_DONE)) {
#if !defined(FB_SPIN_WAIT)
        if (g_tick_armed) __asm__ volatile("wfi");   /* see the PP0 wait */
#endif
        te_poll();
        if ((uint32_t)(timer_ms() - t0) > 60u) { s_kick_err = 2; return; }
    }
    s_us_mdp += tk_now() - k_ph;
    g_fb_wait_us += tk_us(tk_now() - k_ph);   /* PWR census: display wait */
    ic = mmio_read(PLAT_DSI_CTRL_BASE + DSI_INT_CTRL);
    mmio_write(PLAT_DSI_CTRL_BASE + DSI_INT_CTRL, ic | DSI_INTR_CMD_MDP_DONE);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Error census, rev 2 (2026-08-04): THE WALKING-UI ROOT CAUSE lived in
     * rev 1. Its FIFO "error" mask 0x133D00 included bit 20 — which in
     * FIFO_STATUS is DLN1_FIFO_EMPTY, and on this 1-LANE link lanes 1-3 are
     * permanently empty, so bit 20 is ALWAYS SET. Every frame was declared
     * errored and took the recovery path: a DSI-controller sw_reset with NO
     * matching MDP-side reset. The kernel never does that — for lane-FIFO
     * errors it resets BOTH halves (mdss_dsi_sw_reset + the intf_recovery
     * hook -> mdss_mdp_ctl_reset), because a DSI restarted from packet-
     * boundary zero against an un-reset mid-phase MDP interface skews the
     * frame framing by a constant: frames land displaced by a stable offset
     * that re-rolls on the next reset. One reset per kick = one step per
     * kick = the walking UI, immune to every downstream fix we tried.
     *
     * Rev 2 is kernel-verbatim: per-register significance masks (TIMEOUT
     * 0x0111, PHY 0x011111, FIFO 0xCCCC4409 — EMPTY bits excluded), ACK
     * cleared with the kernel's extra-0 quirk, and a controller reset ONLY
     * for real FIFO errors — now resetting BOTH the DSI and the MDP CTL. */
    {
        uint32_t v, now = 0, fifo_err;
        v = mmio_read(PLAT_DSI_CTRL_BASE + 0x068u);   /* ACK_ERR_STATUS  */
        if (v) { now |= 1u; mmio_write(PLAT_DSI_CTRL_BASE + 0x068u, v);
                 mmio_write(PLAT_DSI_CTRL_BASE + 0x068u, 0u); /* kernel quirk */ }
        v = mmio_read(PLAT_DSI_CTRL_BASE + 0x0C0u);   /* TIMEOUT_STATUS  */
        /* Accumulate: the census used to re-read this register at the end of
         * the window, i.e. AFTER this handler had already W1C-cleared it, so
         * it always printed 0x0 and hid which timeout was firing. Keep the OR
         * of everything seen during the window instead. */
        s_tmo_win |= v;
        if (v & 0x0111u) { now |= 2u; mmio_write(PLAT_DSI_CTRL_BASE + 0x0C0u, v); }
        v = mmio_read(PLAT_DSI_CTRL_BASE + 0x0B4u);   /* DLN0_PHY_ERR    */
        if (v & 0x011111u) { now |= 4u; mmio_write(PLAT_DSI_CTRL_BASE + 0x0B4u, v); }
        v = mmio_read(PLAT_DSI_CTRL_BASE + 0x00Cu);   /* FIFO_STATUS     */
        /* SELF-CALIBRATING ERROR MASK (2026-08-06) — rev 3, and the second
         * time this exact trap has cost us weeks.
         *
         * Rev 1 fired recovery every frame because its mask included bit 20
         * (DLN1_FIFO_EMPTY), which is permanently set on this ONE-LANE link.
         * Rev 2 switched to the kernel's 0xCCCC4409, but that mask is written
         * for a FOUR-lane link: bits 18/19, 22/23, 26/27, 30/31 are the upper
         * two status bits of the DLN0..DLN3 FIFOs, and on a 1-lane panel the
         * DLN1-3 FIFOs are never fed, so their bits can sit asserted forever
         * — the same failure wearing different bits. The evidence it was
         * still happening: dsi_host_12nm_reenable() is reachable ONLY from
         * this recovery branch, and the first log we ever pulled off the
         * watch was that function's print repeating without end. A full DSI
         * reset + MDP CTL reset + DCS re-pin on EVERY frame is also a
         * complete explanation for the ~0.5 fps.
         *
         * Rather than hand-pick lane bits a third time, BASELINE the register:
         * the first time through, whatever "error" bits are already asserted
         * are recorded as this link's permanent background and excluded from
         * then on. Wrong-by-construction bits can no longer trigger recovery,
         * whichever ones they turn out to be, and the baseline is logged so
         * the real answer is visible instead of inferred. */
        {
            /* MEASURE-ONLY (2026-08-06, rev 3b) — RESTORED after rev 4 made
             * the SAME regression rev 3a did. Rev 4 applied the baseline on
             * the argument that recovery cannot be needed after a frame that
             * signalled PP0_DONE and CMD_MDP_DONE. On hardware the result was
             * rev 3a's exactly: permanently wrapped image, panel never
             * updating again. That argument is therefore FALSE, and the
             * completion flags do not mean what it assumed.
             *
             * Twice now, suppressing this branch has stopped the display
             * dead, and it is not the re-pin: rev 4 left dsi_dcs_repin_window()
             * running per frame and dsi_dcs_repin_full() every 8 kicks, and
             * the panel still froze. So the per-frame DSI sw_reset + MDP CTL
             * reset + dsi_host_12nm_reenable() is itself load-bearing — this
             * link apparently only keeps delivering frames because it is
             * re-established every frame. Understanding WHY is the open
             * question; until it is answered, this branch must not be
             * throttled, masked, or made conditional.
             *
             * Behaviour here is EXACTLY rev 2. The baseline is RECORDED and
             * reported, never applied. */
            static uint32_t s_fifo_base; static int s_fifo_based;
            uint32_t raw_err = v & 0xCCCC4409u;       /* kernel 4-lane mask */
            if (!s_fifo_based) {
                s_fifo_based = 1;
                s_fifo_base = raw_err;                /* at-rest background */
                bdiag_puts("dsi-census: fifo_baseline="); bdiag_puthex(s_fifo_base);
                bdiag_puts(" raw="); bdiag_puthex(v); bdiag_puts("\n");
            }
            fifo_err = raw_err;                       /* rev 2 behaviour */
            s_fifo_raw = v; s_fifo_masked = raw_err;
            s_fifo_live = raw_err & ~s_fifo_base;     /* REPORTED ONLY */
        }
        if (fifo_err) { now |= 8u; mmio_write(PLAT_DSI_CTRL_BASE + 0x00Cu, v); }
        __asm__ volatile("dsb sy" ::: "memory");
        s_err_classes |= now;
        s_cls_win |= now;      /* per-second copy for the census; s_err_classes
                                * stays sticky for fb_error_classes()/diag */

        /* NO RATE LIMIT — see the census note above: throttling this branch
         * has twice stopped the panel updating altogether. */
        if (fifo_err) {
            uint64_t r0 = tk_now();
            s_recover_count++;
            dsi_host_12nm_sw_reset();
            /* MDP-side half of the kernel's underflow recovery:
             * mdss_mdp_ctl_reset — CTL_SW_RESET @ CTL+0x030, self-clearing. */
            mmio_write(mdp_reg(MDP_CTL0_OFF + 0x030u), 1u);
            {
                uint32_t tr = timer_ms();
                while (mmio_read(mdp_reg(MDP_CTL0_OFF + 0x030u)) & 1u) {
                    if ((uint32_t)(timer_ms() - tr) > 20u) break;
                }
            }
            dsi_host_12nm_reenable();
            /* After a controller reset the DDIC may have lost more than the
             * window — restore the FULL positional state (window + partial
             * area + partial mode, the vendor on-command values). */
#if !defined(NO_REPIN)
            dsi_dcs_repin_full();
#endif
            s_us_recov += tk_now() - r0;
        }

        /* ONE census line per second — never per frame (that is what wrecked
         * the first log). Shows whether recovery is still storming and which
         * FIFO bits are responsible, so the mask question is answered by
         * measurement rather than by another guess. */
        {
            static uint32_t s_win, s_kicks, s_recov0, s_started;
            uint32_t t = timer_ms();
            s_kicks++;
            { uint32_t kus = tk_us(tk_now() - k_t0);
              if (kus > s_us_worst) s_us_worst = kus; }
            if (!s_started) { s_started = 1; s_win = t; s_recov0 = s_recover_count;
                              arm_apply(s_arm); }
            if ((uint32_t)(t - s_win) >= 10000u) {   /* 10 s: keep the log readable on-glass */
                uint32_t rec = s_recover_count - s_recov0;
                /* THIS LINE IS NOW CONDITIONAL. It was the instrument that
                 * settled the display work — arm comparison, underflow rate,
                 * where the microseconds go — and the configuration it was
                 * measuring is frozen (ARM_STABLE, recov=0, ufl=0). Printing
                 * ~400 characters every 10 s for a steady answer buried
                 * everything else in the log.
                 *
                 * It still prints ON ITS OWN whenever the numbers stop being
                 * boring: any recovery, any DCS drain timeout, or any CLS/
                 * timeout error latched this window. So a display regression
                 * announces itself; only the healthy case is silent. Force it
                 * always-on with -DDSI_DIAG. */
                int dsi_report = (rec || g_dsi_drain_timeouts
                                  || s_cls_win || s_tmo_win);
#if defined(DSI_DIAG)
                dsi_report = 1;
#endif
                if (dsi_report) {
                con_puts("DSI arm=");      con_puts(k_arm_name[s_arm]);
                con_puts(" t=");           con_putdec(t);
                con_puts(" kicks=");       con_putdec(s_kicks);
                con_puts(" recov=");       con_putdec(rec);
                /* underflow rate in per-mille of kicks — the ONE number to
                 * compare between arms. */
                con_puts(" ufl/1000=");
                con_putdec(s_kicks ? (rec * 1000u) / s_kicks : 0u);
                con_puts(" fifo_raw=");    con_puthex(s_fifo_raw);
                con_puts(" cls=");         con_puthex(s_cls_win);
                /* Raw error registers, not just class bits: cls=0xa says a
                 * TIMEOUT is firing every window too and we have never seen
                 * WHICH one (0x0C0: b0 HS_TX, b4 LP_RX, b8 BTA). hstmr is the
                 * HS transmit timeout limit — gen 6 never programs it (gen 4
                 * writes 0x3fd08), so if aboot left it short the controller is
                 * aborting our 13 ms frame mid-transfer, which would truncate
                 * lines exactly the way the glass shows. */
                /* tmo = OR over the window (b0 HS_TX, b4 LP_RX, b8 BTA).
                 * If the HS_TX bit disappears on the kernel-timer arms, the
                 * frame-abort diagnosis is confirmed outright. */
                con_puts(" tmo=");         con_puthex(s_tmo_win);
                con_puts(" phy=");
                con_puthex(mmio_read(PLAT_DSI_CTRL_BASE + 0x0B4u));
                con_puts(" hstmr=");
                con_puthex(mmio_read(PLAT_DSI_CTRL_BASE + 0x0BCu));
                con_puts(" trig=");
                con_puthex(mmio_read(PLAT_DSI_CTRL_BASE + 0x084u));
                /* THE decisive readout for round 4: sampled across the whole
                 * window while transfers were in flight. A real TE makes this
                 * sweep a wide range; parked min==max means the DDIC still is
                 * not driving the line even after DCS 0x35. */
                con_puts(" telc=");
                con_putdec(s_telc_min == 0xFFFFFFFFu ? 0u : s_telc_min);
                con_puts("..");            con_putdec(s_telc_max);
                con_puts(" te=");          con_putdec(g_dsi_te_on);
                con_puts(" tc=");          con_putdec(s_tc_on);
                /* aref = AUTOREFRESH_CONFIG as found at boot / live now.
                 * boot bit31 set = aboot's splash autorefresh WAS running
                 * behind every earlier image — the collision source. live
                 * must read 0x0 from now on. */
                /* fps10 = frames pushed this 10 s window (=> /10 = fps).
                 * tehz10 = REAL panel scans this window (=> /10 = panel Hz):
                 * PP0_RD_PTR events, one per TE = one per DDIC refresh. */
                con_puts(" fps10=");       con_putdec(s_kicks);
                con_puts(" tehz10=");      con_putdec(s_te_count);
                con_puts(" aref=");        con_puthex(s_aref_boot);
                con_puts("/");
                con_puthex(mmio_read((uintptr_t)PLAT_MDP_BASE + PP0_OFF
                                     + PP_AUTOREFRESH_CFG));
                /* WHERE THE TIME GOES, microseconds AVERAGED PER KICK, plus
                 * the worst single kick. As the frame rate decays one of
                 * these must grow — that names the culprit outright:
                 *   repin  the 4 DCS packets we send before every frame
                 *   pp0    waiting for the MDP to finish the transfer
                 *   mdp    waiting for the DSI to retire it
                 *   recov  the sw_reset + CTL reset + full re-pin path
                 *   drto   how often the DCS drain hit its 10 ms cap */
                con_puts(" us/kick repin=");
                con_putdec(s_kicks ? tk_us(s_us_repin) / s_kicks : 0u);
                con_puts(" pp0=");
                con_putdec(s_kicks ? tk_us(s_us_pp0) / s_kicks : 0u);
                con_puts(" mdp=");
                con_putdec(s_kicks ? tk_us(s_us_mdp) / s_kicks : 0u);
                con_puts(" recov=");
                con_putdec(s_kicks ? tk_us(s_us_recov) / s_kicks : 0u);
                con_puts(" worst=");       con_putdec(s_us_worst);
                con_puts(" drto=");        con_putdec(g_dsi_drain_timeouts);
                /* Did our CTL_SW_RESET in the recovery path knock the pipe
                 * back to aboot's buffer or drop its geometry? */
                con_puts(" src0=");
                con_puthex(s_pipe ? mmio_read(s_pipe + MDP_PIPE_SRC0_ADDR) : 0u);
                con_puts("\n");
                }

                s_win = t; s_kicks = 0; s_te_count = 0; s_recov0 = s_recover_count;
                s_cls_win = 0; s_tmo_win = 0;
                s_telc_min = 0xFFFFFFFFu; s_telc_max = 0; s_skipped = 0;
                s_us_repin = s_us_pp0 = s_us_mdp = s_us_recov = 0;
                s_us_worst = 0; g_dsi_drain_timeouts = 0;
                /* No arm cycling any more — the configuration is frozen (see
                 * the ROUND 7 note). Re-assert it so nothing that the recovery
                 * path touched can drift back. */
                arm_apply(ARM_STABLE);
            }
        }

        /* Periodic deep re-pin from this safe point (transfer retired, link
         * idle) — insurance against DDIC state decay our error census never
         * sees. The per-frame window re-pin above handles the fast path. */
#if !defined(NO_REPIN)
        if (g_dsi_repin_full_n &&
            (++s_kick_count % g_dsi_repin_full_n) == 0u) dsi_dcs_repin_full();
#else
        (void)s_kick_count;
#endif
    }
}

/* ---- PERF_BARS (2026-08-04): eyeball-readable performance overlay ---------
 * The UI runs at ~0.5 fps and there are four distinct suspects with four
 * different fixes. This overlay separates them in ONE hardware test — four
 * rows of chunky blocks at the top-left, redrawn into the framebuffer on
 * every flush (each block is easily countable on a photo or by eye):
 *   row 0 WHITE : loop() iterations in the last second   (1 block each)
 *   row 1 GREEN : frames pushed in the last second       (1 block each)
 *   row 2 RED   : last fb_kick duration, 1 block = 5 ms
 *   row 3 YELLOW: last loop() body duration, 1 block = 5 ms
 * Readings:
 *   row0 low + row3 short -> loop starves between bodies = tick/scheduler
 *   row3 long             -> the work is inside loop() (next: bisect there)
 *   row2 long             -> our display push is the cost (repin/TE waits)
 *   row0 high + row1 low  -> LVGL isn't flushing (refresh-timer side)     */
static uint32_t s_pb_loops, s_pb_loops_disp;
static uint32_t s_pb_frames, s_pb_frames_disp;
static uint32_t s_pb_win_start;
static uint32_t s_pb_kick_ms, s_pb_body_ms;

void fb_perf_loop_tick(uint32_t body_ms)
{
    s_pb_loops++;
    s_pb_body_ms = body_ms;
    /* Keep the panel-Hz census counting even when LVGL has nothing dirty and
     * fb_kick is idle. Guarded: render-only mode means MDP may be unclocked. */
    if (s_flush_bits) te_poll();
}

#if defined(PERF_BARS)
static void fb_perf_overlay(void)
{
    if (!s_fb || s_bpp < 2) return;

    uint32_t now = timer_ms();
    if ((uint32_t)(now - s_pb_win_start) >= 1000u) {
        s_pb_win_start   = now;
        s_pb_loops_disp  = s_pb_loops;  s_pb_loops  = 0;
        s_pb_frames_disp = s_pb_frames; s_pb_frames = 0;
    }

    uint32_t rows[4];
    rows[0] = s_pb_loops_disp;
    rows[1] = s_pb_frames_disp;
    rows[2] = (s_pb_kick_ms + 4u) / 5u;
    rows[3] = (s_pb_body_ms + 4u) / 5u;
    static const uint8_t k_rgb[4][3] = {
        { 0xFF, 0xFF, 0xFF }, { 0x00, 0xFF, 0x00 },
        { 0xFF, 0x20, 0x20 }, { 0xFF, 0xFF, 0x00 },
    };

    uint32_t cw = 9u, gap = 3u, ch = 9u;
    for (uint32_t r = 0; r < 4; r++) {
        uint32_t nblk = rows[r] > 32u ? 32u : rows[r];
        for (uint32_t b = 0; b < 32u; b++) {
            uint32_t x0 = 40u + b * (cw + gap);
            uint32_t y0 = 40u + r * (ch + 3u);
            if (x0 + cw >= s_w) break;
            for (uint32_t y = 0; y < ch; y++) {
                volatile uint8_t *px = (volatile uint8_t *)
                    (s_fb + (y0 + y) * s_stride + x0 * s_bpp);
                for (uint32_t x = 0; x < cw; x++) {
                    /* lit block = row color; unlit = dim gray baseline so
                     * "zero" is distinguishable from "overdrawn" */
                    uint8_t lit = (b < nblk);
                    px[0] = lit ? k_rgb[r][2] : 0x20;    /* B */
                    px[1] = lit ? k_rgb[r][1] : 0x20;    /* G */
                    px[2] = lit ? k_rgb[r][0] : 0x20;    /* R */
                    if (s_bpp == 4) { px[3] = 0xFF; px += 4; } else px += 3;
                }
            }
        }
    }
}
#endif /* PERF_BARS */

/* Debug block: three LARGE 60x60 live-state blocks across the VERTICAL
 * CENTER of the glass (rev 2 — rev 1 was a 6px strip at y=2, invisible on
 * a round panel whose top/bottom/sides are physically clipped). idx 0..2,
 * left to right, all inside the circle at mid-height. No flush — repainted
 * every touch poll, visible with the next kicked frame. */
void fb_dbg_mark(uint32_t idx, uint32_t xrgb)
{
    if (!s_fb || s_bpp < 2) return;
    uint32_t x0 = 40u + idx * 90u;                /* 40,130,220 (+60 wide) */
    uint32_t y0 = (s_h / 2u) - 30u;
    uint8_t r = (uint8_t)(xrgb >> 16), g = (uint8_t)(xrgb >> 8), b = (uint8_t)xrgb;
    for (uint32_t y = 0; y < 60; y++) {
        volatile uint8_t *px = (volatile uint8_t *)
            (s_fb + (y0 + y) * s_stride + x0 * s_bpp);
        for (uint32_t x = 0; x < 60; x++) {
            px[0] = b; px[1] = g; px[2] = r;
            if (s_bpp == 4) { px[3] = 0xFF; px += 4; } else px += 3;
        }
    }
}

/* Debug byte row: 8 bit-blocks (20x20, MSB leftmost), rows stacked in the
 * center band (row 0..3 at y=120..230, x=112..300 — all inside the round
 * panel's visible circle). White block = bit set, dark = clear. */
void fb_dbg_byte(uint32_t row, uint32_t val)
{
    if (!s_fb || s_bpp < 2) return;
    uint32_t y0 = 120u + row * 30u;
    for (uint32_t b = 0; b < 8; b++) {
        uint32_t x0 = 112u + b * 24u;
        uint8_t v = ((val >> (7u - b)) & 1u) ? 0xFF : 0x28;
        for (uint32_t y = 0; y < 20; y++) {
            volatile uint8_t *px = (volatile uint8_t *)
                (s_fb + (y0 + y) * s_stride + x0 * s_bpp);
            for (uint32_t x = 0; x < 20; x++) {
                px[0] = v; px[1] = v; px[2] = v;
                if (s_bpp == 4) { px[3] = 0xFF; px += 4; } else px += 3;
            }
        }
    }
}

/* Whole-frame flush, for LVGL's direct-mode callback. */
void fb_flush_all(void)
{
    if (!s_fb) return;
    /* ROUND 5 RATE FLOOR: REMOVED, it was actively harmful (measured).
     * Throttling to 45/30/20 fps made the frame rate WORSE and turned the
     * corruption from intermittent into permanent. The reason the over-drive
     * theory was wrong: the round-4 image reached ~45-60 fps while looking
     * smooth and clean, i.e. the link already runs near its ~74 fps ceiling
     * when healthy. Fewer pushes just means fewer re-pins, so the DDIC's
     * autonomous drift accumulates instead of being corrected every frame.
     * MORE frames is better on this panel, not fewer. */
#if defined(PERF_BARS)
    fb_perf_overlay();
#endif
    /* SPLIT FOR THE CENSUS (2026-08-07): the LV line's flush_ms conflated two
     * very different costs — the D-cache clean of the whole 519 KB frame
     * (real CPU work) and fb_kick's blocking wait on the DSI transfer (~13 ms,
     * not CPU work at all, and already counted in disp%). Time them apart or
     * the number cannot be acted on. */
    {
        uint32_t c0 = timer_us32();
        fb_flush_region((const void *)s_fb, s_stride * s_h);
        g_fb_cache_us += timer_us32() - c0;
    }
    {
        uint32_t t0 = timer_ms();
        fb_kick();
        s_pb_kick_ms = timer_ms() - t0;
    }
    s_pb_frames++;
}

#if defined(REG_BARS)
/* REGISTER BARS (2026-08-04): live hardware state ON THE GLASS as binary
 * bars — the port's first real register-readback channel (ramlog unreadable,
 * no UART). Six rows, 32 bits each, bit31 leftmost, white=1/dark=0, drawn
 * straight into the framebuffer (LVGL repaints over them eventually; they
 * re-draw every few seconds). Rows:
 *   0  DSI FIFO_STATUS  (0x00C)  raw at sample time
 *   1  err_classes [3:0] | recover_count [7:4] | kick_count<<8
 *   2  DSI DCS_CMD_CTRL (0x044)  readback — is insert-2C bit16 really set?
 *   3  DSI STREAM0_CTRL (0x058)  readback
 *   4  GCC mdp RCG CFG (0x4D018) readback — 0x109 = 160 MHz fix landed
 *   5  PP0 LINE_COUNT   (+0x2C)  — nonzero/changing proves TE counts     */
static void fb_diag_bars(void)
{
    if (!s_fb || s_bpp < 2) return;
    uint32_t vals[6];
    vals[0] = mmio_read(PLAT_DSI_CTRL_BASE + 0x00Cu);
    vals[1] = s_err_classes | ((s_recover_count & 0xFu) << 4) | (s_kick_count << 8);
    vals[2] = mmio_read(PLAT_DSI_CTRL_BASE + 0x044u);
    vals[3] = mmio_read(PLAT_DSI_CTRL_BASE + 0x058u);
    vals[4] = mmio_read((uintptr_t)PLAT_GCC_BASE + 0x4D018u); /* mdp RCG CFG:
                            expect 0x109 = GPLL0/5 = 160 MHz (underflow fix) */
    vals[5] = mmio_read((uintptr_t)PLAT_MDP_BASE + PP0_OFF + 0x02Cu);

    uint32_t rows = 6, ch = 12u, cw = 12u;
    uint32_t y0 = s_h - rows * (ch + 2u) - 4u;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t b = 0; b < 32; b++) {
            uint32_t set = (vals[r] >> (31u - b)) & 1u;
            uint32_t x0 = 4u + b * cw;
            for (uint32_t y = 0; y < ch; y++) {
                volatile uint8_t *px = (volatile uint8_t *)
                    (s_fb + (y0 + r * (ch + 2u) + y) * s_stride + x0 * s_bpp);
                for (uint32_t x = 0; x < cw - 2u; x++) {
                    uint8_t v = set ? 0xFF : 0x18;
                    /* red tint every 8th bit as a reading ruler */
                    px[0] = v; px[1] = v;
                    px[2] = ((b & 7u) == 0u) ? 0xFF : v;
                    if (s_bpp == 4) { px[3] = 0xFF; px += 4; } else px += 3;
                }
            }
        }
    }
}
#endif /* REG_BARS */

/* ANTI-DRIFT IDLE REFRESH (2026-08-04). The DDIC shifts its addressing
 * autonomously (burn-in prevention); the vendor's own idle-off command
 * re-sends the whole positional block ("TODO: will remove set colum/row
 * address" in their dtsi) and stock WearOS never stops pushing frames
 * (45 fps active / 15 fps idle), which is why the shift is invisible there.
 * Our LVGL only kicks on dirty content, so static screens sat shifted for
 * up to a minute. Cure: if nothing has been kicked for max_age_ms, re-push
 * the current framebuffer (each kick re-pins the window first). Called from
 * the app loop; cheap (one 519 KB DMA at 2 Hz worst case). */
void fb_idle_refresh(uint32_t max_age_ms)
{
    if (!s_fb || !s_flush_bits) return;
#if defined(REG_BARS)
    static uint32_t s_last_bars;
    if ((uint32_t)(timer_ms() - s_last_bars) >= 3000u) {
        s_last_bars = timer_ms();
        fb_diag_bars();
        fb_flush_all();
        return;
    }
#endif
    if ((uint32_t)(timer_ms() - s_last_kick_ms) >= max_age_ms)
        fb_flush_all();
}

/* VISUAL_TRACE (2026-08-03): boot-progress reporting on the GLASS. The wdog
 * stopwatch hit its resolution floor (an instant TZ-class reset ignores every
 * armed timeout), but the display path is PROVEN from recovery (display11).
 * So each boot milestone paints a distinct solid color and synchronously
 * kicks it; the last color visible when the watch dies names the last
 * milestone survived. The ~400 ms hold makes the sequence humanly readable.
 * (R/B may be swapped by the pipe format — the chosen colors stay mutually
 * distinct either way.) */
void fb_trace(uint32_t xrgb)
{
    if (!s_fb) return;
    uint8_t r = (uint8_t)(xrgb >> 16), g = (uint8_t)(xrgb >> 8), b = (uint8_t)xrgb;
    volatile uint8_t *p = (volatile uint8_t *)s_fb;
    uint16_t c565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    for (uint32_t y = 0; y < s_h; y++) {
        volatile uint8_t *row = p + y * s_stride;
        for (uint32_t x = 0; x < s_w; x++) {
            if (s_bpp == 2) {
                row[0] = (uint8_t)c565; row[1] = (uint8_t)(c565 >> 8); row += 2;
            } else {
                row[0] = b; row[1] = g; row[2] = r;
                if (s_bpp == 4) { row[3] = 0xFF; row += 4; } else { row += 3; }
            }
        }
    }
    fb_flush_all();
    timer_delay_ms(400);
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
