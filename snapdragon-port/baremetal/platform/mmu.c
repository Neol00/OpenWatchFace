/* mmu.c — flat (identity) MMU map + caches for ARMv7-A.
 *
 * Why this exists: with the MMU off, ARMv7 treats ALL memory as strongly-
 * ordered, and unaligned accesses FAULT. LVGL/newlib rely on ordinary
 * unaligned loads, so the runtime cannot host them until DDR is mapped as
 * Normal memory. Caches ride along for free (essential on real silicon;
 * QEMU's TCG models them as no-ops).
 *
 * Map: 4096 x 1MB sections, VA == PA.
 *   - DDR (PLAT_DDR_BASE..+SIZE): Normal, write-back write-allocate.
 *   - everything else:            Device (peripherals stay strongly ordered).
 */
#include "platform.h"

static uint32_t s_l1[4096] __attribute__((aligned(16384)));
uint32_t g_mmu_ttbr0;   /* TTBR0 value CPU0 uses; secondary cores load the same table (smp_entry.S) */

#define SECT_DEVICE   0x00000C12u  /* section, AP=rw, XN=1, TEX=000 C=0 B=0 (strongly-ordered) */
#define SECT_NORMAL   0x00015C0Eu  /* section, AP=rw, XN=0, S=1, TEX=101 C=1 B=1 (WBWA) */

static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

void mmu_enable_flat(void)
{
    uint32_t ddr_first = PLAT_DDR_BASE >> 20;
    uint32_t ddr_last  = (PLAT_DDR_BASE + PLAT_DDR_SIZE - 1) >> 20;

    for (uint32_t i = 0; i < 4096; i++) {
        uint32_t base = i << 20;
        s_l1[i] = base | ((i >= ddr_first && i <= ddr_last) ? SECT_NORMAL : SECT_DEVICE);
    }

#if defined(PLAT_SOC_MSM8909)
    /* Same carve-out reasoning as the Gen 6 block below, with this SoC's own
     * numbers, read from the reserved-memory node of the DTB dumped off a
     * Fossil Gen 4 (snapdragon-port/dtbs/firefish-stock.dts) — and then found to be
     * BYTE-IDENTICAL in the TicWatch C2's tree
     * (dtbs/skipjack-fromsource.dts:9109-9132), all four ranges
     * including ramoops. They describe where the modem, ADSP and TrustZone
     * images live on msm8909w, which is a property of the SoC's memory map
     * rather than of either watch, so both get them:
     *
     *   external_image__region  0x87B00000 + 0x500000   removed-dma-pool, no-map
     *   modem_adsp_region       0x88000000 + 0x2100000  removed-dma-pool, no-map
     *   pheripheral_region      0x8A100000 + 0x500000   removed-dma-pool, no-map
     *   ramoops_region          0x9FF00000 + 0x100000   removed-dma-pool, no-map
     *
     * `no-map` means the stock kernel never creates a mapping for these at
     * all — they belong to the modem/ADSP images and to TrustZone, and are
     * XPU-protected against the application cores. Mapped Normal and
     * executable, the A7's prefetcher can wander into one at any moment and
     * take an XPU violation that arrives as an unexplained reset seconds
     * after boot, with nothing in the log. Device+XN stops speculation dead;
     * a genuine explicit access still faults visibly, which is what we want.
     *
     * splash_region@83000000 (+0xC00000) is deliberately NOT carved out: it
     * is a plain reservation with no `no-map`, it is where aboot's own
     * framebuffer lives, and the display path may legitimately read it. It
     * does sit inside the range our heap could reach, which is the reason
     * PLAT_DDR_SAFE_END exists — keep allocations below it. */
    /* CORRECTION (2026-09-10): "a property of the SoC's memory map rather than
     * of either watch" is WRONG. It held for the Gen 4 and the C2 because their
     * trees happen to match; the Fossil Gen 5's own tree (triggerfish-stock.dts)
     * puts every region somewhere else -- external_image 0x87A00000 + 6 MB,
     * modem_adsp 0x88000000 + 0x2300000, pheripheral 0x8A300000 + 6 MB, and no
     * ramoops. With this table the Gen 5's WCNSS region top (0x8A6-0x8A8) was
     * mapped NORMAL: cached, executable. The firmware loader wrote it through
     * the D-cache and TrustZone hashed physical DDR, so pas_auth_and_reset
     * failed whenever those lines had not been flushed -- 4 attempts in 5 on
     * hardware. A board whose tree differs supplies PLAT_MMU_HOLES_8909. */
    static const struct { uint32_t first_mb, last_mb; } k_msm8909_holes[] = {
#if defined(PLAT_MMU_HOLES_8909)
        PLAT_MMU_HOLES_8909
#else
        { 0x87B, 0x87F },   /* external_image  */
        { 0x880, 0x8A0 },   /* modem + adsp    */
        { 0x8A1, 0x8A5 },   /* pheripheral     */
        { 0x9FF, 0x9FF },   /* ramoops         */
#endif
    };
    for (uint32_t h = 0; h < sizeof k_msm8909_holes / sizeof k_msm8909_holes[0]; h++)
        for (uint32_t i = k_msm8909_holes[h].first_mb; i <= k_msm8909_holes[h].last_mb; i++)
            s_l1[i] = (i << 20) | SECT_DEVICE;
#endif
#if defined(PLAT_BOARD_FOSSIL_GEN6)
    /* CARVE OUT TrustZone's DDR (2026-08-03). These ranges sit INSIDE DDR but
     * are XPU-protected against us; the stock kernel NEVER maps them
     * (reserved-memory/no-map in the REAL device tree, dumped from hardware).
     * Mapped Normal-executable, the A7's prefetcher/branch predictor may
     * fetch from them SPECULATIVELY at any moment -> XPU violation -> the
     * layout-dependent instant resets / stray prefetch aborts ~1 s after the
     * scheduler spread execution across enough pages to get the predictors
     * wandering. Device-XN stops all speculative access; explicit accesses
     * still fault visibly (fault stubs report them). */
    static const struct { uint32_t first_mb, last_mb; } k_tz_holes[] = {
        { 0x85B, 0x867 },   /* other_ext: TZ apps / QSEE / smem            */
        { 0x868, 0x885 },   /* modem firmware                              */
        { 0x886, 0x8A5 },   /* adsp firmware                               */
        { 0x8A6, 0x8AF },   /* wcnss firmware                              */
        { 0x900, 0x913 },   /* splash framebuffer (proven XPU: write hang) */
    };
    for (uint32_t h = 0; h < sizeof k_tz_holes / sizeof k_tz_holes[0]; h++)
        for (uint32_t i = k_tz_holes[h].first_mb; i <= k_tz_holes[h].last_mb; i++)
            s_l1[i] = (i << 20) | SECT_DEVICE;
#endif
#if defined(PLAT_SMEM_TARG_INFO_REG)
    /* MAP SMEM WHEREVER IT ACTUALLY IS (2026-09-13).
     *
     * PLAT_DDR_SIZE is a compile-time 512 MB on the C2 board header, so the
     * loop above maps 0x80000000..0xA0000000 and nothing more. The TicWatch C2+
     * is a 1 GB part (its aboot DTB reports "ddr: 1024 MB") and its SBL can
     * place SMEM above that line -- outside the map altogether. Every SMD
     * channel lookup then fails and the RPM request channel never opens, which
     * is the "sys-pc: no RPM channel" on that unit: no RPM sleep set, no XO
     * shutdown, ~37 mA instead of ~6 mA. ddr_size_detect() cannot help, it is
     * reporting-only by design (platform.h:910) and runs long after this.
     *
     * smem_targ_info is an MMIO register, readable with no MMU and no SMEM, so
     * the one thing we need before mapping is available before mapping. Map
     * SMEM's own window as Device: uncached is correct for shared memory the
     * RPM writes behind our back, and it is what the 0x87B-0x87F carve-out
     * already gives SMEM on the 512 MB parts. */
    {
        uint32_t id   = *(volatile uint32_t *)(PLAT_SMEM_TARG_INFO_REG);
        uint32_t size = *(volatile uint32_t *)(PLAT_SMEM_TARG_INFO_REG + 4u);
        uint32_t base = *(volatile uint32_t *)(PLAT_SMEM_TARG_INFO_REG + 8u);
        if (id == 0x49494953u && size >= 0x1000u && size <= 0x00400000u) {
            uint32_t f = base >> 20, l = (base + size - 1u) >> 20;
            for (uint32_t i = f; i <= l && i < 4096u; i++)
                s_l1[i] = (i << 20) | SECT_DEVICE;
        }
    }
#endif
    dsb();
#if defined(WDOG_TRACE) && defined(PLAT_SOC_MSM)
    wdog_stage(2);   /* L1 table built; next risk is the enable itself */
#endif

    /* invalidate stale state: I-cache, branch predictor, TLB, L1 D-cache */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0));   /* ICIALLU  */
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" :: "r"(0));   /* BPIALL   */
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0));   /* TLBIALL  */
    /* L1 D-cache geometry READ FROM THE CORE (CCSIDR), not assumed.
     * (2026-08-03: this loop hardcoded "A7: 4-way, 128 sets" — the SoC is
     * actually a Cortex-A53 (Wear 4100, user-corrected) whose L1 happens to
     * share that geometry, so it worked by coincidence. Ask the hardware.) */
    {
        uint32_t ccsidr, csselr = 0;                 /* level 1, data */
        __asm__ volatile("mcr p15, 2, %0, c0, c0, 0" :: "r"(csselr));
        __asm__ volatile("isb");
        __asm__ volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(ccsidr));
        uint32_t line_shift = (ccsidr & 0x7u) + 4u;          /* log2(bytes)  */
        uint32_t ways = ((ccsidr >> 3) & 0x3FFu) + 1u;
        uint32_t sets = ((ccsidr >> 13) & 0x7FFFu) + 1u;
        uint32_t way_shift = 32u - __builtin_clz(ways - 1u ? ways - 1u : 1u);
        for (uint32_t way = 0; way < ways; way++)
            for (uint32_t set = 0; set < sets; set++) {
                uint32_t v = (way << (32u - way_shift)) | (set << line_shift);
                __asm__ volatile("mcr p15, 0, %0, c7, c6, 2" :: "r"(v)); /* DCISW */
            }
    }
    dsb();

    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(0x1));      /* DACR: dom0 client */
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" :: "r"(0));        /* TTBCR: TTBR0 only */
    g_mmu_ttbr0 = (uint32_t)(uintptr_t)s_l1 | 0x48u;
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"(g_mmu_ttbr0)); /* TTBR0: WBWA walk */
    isb();

    uint32_t sctlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    sctlr |= (1u << 0)    /* M: MMU */
           | (1u << 2)    /* C: D-cache */
           | (1u << 11)   /* Z: branch prediction */
           | (1u << 12);  /* I: I-cache */
    sctlr &= ~(1u << 1);  /* A: no strict alignment (Normal memory allows unaligned) */
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(sctlr));
    isb();
#if defined(WDOG_TRACE) && defined(PLAT_SOC_MSM)
    /* Surviving to here means the MMU + caches are ON and we are executing
     * through the new translation — the single riskiest instant in the boot. */
    wdog_stage(3);
#endif
}

/* 2026-09-18 (gen5-modem-36): .rodata is rewritten at runtime, repeatedly, in one 4 KB page.
 * Mark every 1 MB section that holds only code and read-only data as privileged READ-ONLY
 * (APX=1, AP=01). A CPU writer then takes a data abort: startup.S records lr/DFSR/DFAR and
 * the next boot prints "!! FAULT in previous life: DATA ABORT ... far=". A bus master (DMA)
 * is not subject to these bits, so a page that still changes with no fault names hardware. */
extern char __rodata_end[];
void mmu_protect_code_ro(void)
{
    uint32_t start = (0x80008000u + 0xFFFFFu) & ~0xFFFFFu;             /* first whole section past _start */
    uint32_t end   = (uint32_t)(uintptr_t)__rodata_end & ~0xFFFFFu;     /* last whole section before .data */
    uint32_t ro    = (SECT_NORMAL & ~(3u << 10)) | (1u << 10) | (1u << 15);
    unsigned n = 0;
    for (uint32_t a = start; a < end; a += 0x100000u) { s_l1[a >> 20] = a | ro; n++; }
    for (uint32_t a = start; a < end; a += 0x100000u) { const void *e = &s_l1[a >> 20]; __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(e)); }
    __asm__ volatile("dsb sy; mcr p15, 0, %0, c8, c7, 0; dsb sy; isb" :: "r"(0) : "memory");   /* TLBIALL */
    con_puts("mmu: code+rodata read-only: "); con_puthex(start); con_puts(".."); con_puthex(end); con_puts(" ("); con_putdec(n); con_puts(" MB)\n");
}
