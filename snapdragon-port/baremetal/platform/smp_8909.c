/* smp_8909.c — second-core bring-up test for the msm8909w (Gen 4 / C2 / S2).
 *
 * Stage 1 of SMP, 2026-09-04: prove that (a) TZ delivers a cold-booted core
 * to an entry point we choose, (b) the core runs with the MMU on through the
 * shared page table and its writes are visible to CPU0, and (c) the GIC
 * delivers an SGI from CPU0 to CPU1. FreeRTOS SMP is stage 2 and needs all
 * three.
 *
 * Boot recipe = the C2's own 3.18 kernel (drivers/soc/qcom/cpu_ops.c
 * msm_cpu_prepare/msm_cpu_boot + cpu_pwr_ctl.c msm_unclamp_secondary_arm_cpu):
 *   1. scm_set_boot_addr(entry, SCM_FLAG_COLDBOOT_CPUn)   (SVC_BOOT/BOOT_ADDR)
 *   2. L2 already powered (CPU0 lives in it) -> power_on_l2 short-circuits
 *   3. ACC(cpu) CPU_PWR_CTL 0x4 / CPU_PWR_GATE_CTL 0x14 sequence:
 *        0x33 -> gate 0x10000001 -> 0x31 -> 0x39 -> 0x20038 -> 0x20008 -> 0x20088
 *   ACC bases: cpu-sleep-status@b088008/b098008/b0a8008/b0b8008 in the DTB
 *   are ACC+0x8, so ACC(n) = 0xB088000 + n*0x10000 (the msm8916 layout). */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

#define ACC_BASE(n)        (0x0B088000u + 0x10000u * (n))
#define CPU_PWR_CTL        0x04u
#define CPU_PWR_GATE_CTL   0x14u
#define SCM_FLAG_COLDBOOT_CPU1 0x01u

#define GICD(o)  (PLAT_GICD_BASE + (o))
#define GICC(o)  (PLAT_GICC_BASE + (o))
#define GICD_ISENABLER0  GICD(0x100)
#define GICD_IPRIORITYR  GICD(0x400)
#define GICD_SGIR        GICD(0xF00)
#define GICC_CTLR        GICC(0x000)
#define GICC_PMR         GICC(0x004)
#define GICC_IAR         GICC(0x00C)
#define GICC_EOIR        GICC(0x010)

extern void smp_secondary_entry(void);
uint8_t smp_cpu1_stack[8192] __attribute__((aligned(16)));
uint8_t *const smp_cpu1_stack_top = smp_cpu1_stack + sizeof smp_cpu1_stack;

/* Shared with CPU1. Kept in their own cache lines and pushed/pulled by
 * explicit cache maintenance so the test does not DEPEND on SCU coherency
 * (which is one of the things it is meant to find out). */
volatile uint32_t g_smp_landed   __attribute__((aligned(64)));
volatile uint32_t g_smp_beat     __attribute__((aligned(64)));
volatile uint32_t g_smp_sgi_rx   __attribute__((aligned(64)));
volatile uint32_t g_smp_mpidr    __attribute__((aligned(64)));
volatile uint32_t g_smp_actlr    __attribute__((aligned(64)));
volatile uint32_t g_smp_busy_ticks __attribute__((aligned(64)));
/* CPU1 POWER COLLAPSE (2026-09-07): core 0 sets g_smp_pc_req and raises SGI0;
 * CPU1 runs its own SPM standalone-pc + TERMINATE_PC. It has no context to
 * keep -- the warm-boot address is smp_secondary_entry, i.e. it comes back
 * through the cold path on the next SGI0 and re-enters the service loop. */
volatile uint32_t g_smp_pc_req   __attribute__((aligned(64)));
volatile uint32_t g_smp_pc_n     __attribute__((aligned(64)));   /* SMCs issued */
volatile uint32_t g_smp_pc_decl  __attribute__((aligned(64)));   /* TZ declined */
extern int smp_cpu1_collapse(void);                              /* smp_entry.S */
extern void smp_cpu1_wfi_collapse(void);
volatile uint32_t g_smp_pc_mode __attribute__((aligned(64)));  /* 1 = TERMINATE_PC SMC, 2 = SPM + WFI (no TZ) */

/* FRAME-PUSH MAILBOX (2026-09-06). Core 0 renders; CPU1 pushes frames to the
 * panel. Core 0 cleans the framebuffer out of ITS OWN D-cache (this cluster
 * runs without SMP coherency broadcast, so CPU1 could not do that for it),
 * then posts the address here and raises SGI0. CPU1 kicks DMA_P and spins on
 * DMA_P_DONE - the transfer time the UI loop used to burn on core 0. Each
 * word sits on its own cache line and is cleaned/invalidated explicitly, the
 * same discipline as the counters above. */
volatile uint32_t g_smp_flush_addr __attribute__((aligned(64)));
volatile uint32_t g_smp_flush_req  __attribute__((aligned(64)));   /* seq, written by core 0 */
volatile uint32_t g_smp_flush_ack  __attribute__((aligned(64)));   /* seq, written by CPU1   */
volatile uint32_t g_smp_flush_err  __attribute__((aligned(64)));
static uint32_t   s_flush_seq;                                     /* core 0 side */

static inline void dc_clean(volatile void *p)  { __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p)); __asm__ volatile("dsb sy" ::: "memory"); }
static inline void dc_inval(volatile void *p)  { __asm__ volatile("mcr p15, 0, %0, c7, c6, 1"  :: "r"(p)); __asm__ volatile("dsb sy" ::: "memory"); }
static inline uint32_t rd_cp(void) { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(v)); return v; }

/* Runs on CPU1, MMU on, IRQs masked at the CPSR. WFI still wakes on a
 * pending IRQ, so the SGI is taken by polling GICC_IAR instead of a vector. */
void smp_secondary_main(void)
{
    uint32_t actlr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
    g_smp_mpidr = rd_cp(); dc_clean(&g_smp_mpidr);
    g_smp_actlr = actlr;   dc_clean(&g_smp_actlr);
    g_smp_landed = 1u;     dc_clean(&g_smp_landed);

    /* This core's GIC CPU interface + its banked SGI0 enable/priority. */
    mmio_write(GICC_PMR, 0xFFu);
    mmio_write(GICC_CTLR, 1u);
    mmio_write(GICD_IPRIORITYR + 0u, 0xC0u);       /* SGI0 priority (banked, byte 0) */
    mmio_write(GICD_ISENABLER0, 1u << 0);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Idle accounting for the Power app: ticks spent awake (outside WFI). */
    uint64_t t_wake = timer_ticks();
    for (;;) {
        g_smp_beat++; dc_clean(&g_smp_beat);
        g_smp_busy_ticks += (uint32_t)(timer_ticks() - t_wake); dc_clean(&g_smp_busy_ticks);
        __asm__ volatile("wfi");
        t_wake = timer_ticks();
        uint32_t iar = mmio_read(GICC_IAR);
        uint32_t id = iar & 0x3FFu;
        if (id < 1020u) {
            if (id == 0u) { g_smp_sgi_rx++; dc_clean(&g_smp_sgi_rx); }
            mmio_write(GICC_EOIR, iar);
        }
        dc_inval(&g_smp_pc_req);
        if (g_smp_pc_req) {
            g_smp_pc_req = 0; dc_clean(&g_smp_pc_req);
            g_smp_pc_n++;     dc_clean(&g_smp_pc_n);
            g_smp_busy_ticks += (uint32_t)(timer_ticks() - t_wake); dc_clean(&g_smp_busy_ticks);
            spm_cpu1_mode(1);
            dc_inval(&g_smp_pc_mode);
            if (g_smp_pc_mode == 2u) smp_cpu1_wfi_collapse();
            else (void)smp_cpu1_collapse();     /* returns only when declined */
            spm_cpu1_mode(0);
            g_smp_pc_decl++;  dc_clean(&g_smp_pc_decl);
            t_wake = timer_ticks();
        }
        /* frame push service: one outstanding request at a time */
        dc_inval(&g_smp_flush_req); dc_inval(&g_smp_flush_addr);
        uint32_t req = g_smp_flush_req;
        if (req != g_smp_flush_ack) {
            mdp3_kick((const void *)(uintptr_t)g_smp_flush_addr);
            int rc = mdp3_wait(100);
            g_smp_flush_err = (rc == 0) ? 0u : 1u; dc_clean(&g_smp_flush_err);
            g_smp_flush_ack = req;                 dc_clean(&g_smp_flush_ack);
        }
    }
}

static int s_cpu1_up;
static void unclamp(unsigned cpu);

/* ---- PARK-ONLY CORES (v180, 2026-09-09) ---------------------------------
 * TZ tracks four cores (tzdbg), the stock kernel has all four in TZ power
 * collapse (idle or hotplugged) before any cluster level, and every one of
 * our cluster collapses with CPU2/CPU3 never booted ended with TZ not
 * warm-booting CPU0. So: cold-boot CPU2 and CPU3 through TZ exactly like
 * CPU1, and have each immediately TERMINATE_PC itself with L2 flag 0. If TZ
 * ever warm-boots one of them (after a cluster wake) it lands here again and
 * parks again. They never run anything else. */
uint8_t smp_park_stacks[2048] __attribute__((aligned(16)));
volatile uint32_t g_smp_park_landed __attribute__((aligned(64)));   /* bit per core */
volatile uint32_t g_smp_park_smc    __attribute__((aligned(64)));   /* TERMINATE_PC calls issued by parked cores */
volatile uint32_t g_smp_park_decl   __attribute__((aligned(64)));
void smp_park_main(void)
{
    unsigned cpu = rd_cp() & 0xFFu;
    g_smp_park_landed |= 1u << cpu; dc_clean(&g_smp_park_landed);
    mmio_write(GICC_PMR, 0xFFu);
    mmio_write(GICC_CTLR, 1u);
    __asm__ volatile("dsb sy" ::: "memory");
    for (;;) {
        spm_cpu_mode_n(cpu, 1);                       /* standalone pc on the WFI inside TZ */
        g_smp_park_smc++; dc_clean(&g_smp_park_smc);
        (void)smp_cpu1_collapse();                    /* L1 clean + TERMINATE_PC(0); returns only if declined */
        g_smp_park_decl++; dc_clean(&g_smp_park_decl);
        timer_delay_ms(10);
    }
}
static const uint32_t k_cold_flag[4] = { 0u, 0x01u, 0x08u, 0x20u };  /* scm-boot.h SCM_FLAG_COLDBOOT_CPUn */
static const uint32_t k_warm_flag[4] = { 0x04u, 0x02u, 0x10u, 0x40u };
int smp_park_extra_cores(void)
{
    int ok = 1;
    for (unsigned cpu = 2; cpu <= 3; cpu++) {
        if (!spm_cpu_ready_n(cpu)) { con_puts("smp: cpu"); con_putdec(cpu); con_puts(" has no SAW, not parked\n"); ok = 0; continue; }
        /* v182: print BEFORE touching the core (v181 reset the C2 at this point),
         * and never unclamp a core TZ did not accept a cold-boot address for. */
        con_puts("smp: cpu"); con_putdec(cpu); con_puts(" park: acc pwr_ctl="); con_puthex(mmio_read(ACC_BASE(cpu) + CPU_PWR_CTL));
        con_puts(" sleep-status="); con_puthex(mmio_read(ACC_BASE(cpu) + 0x8u)); con_puts(" ..."); con_flush(); usb_poll();
        int rc = scm_set_boot_addr((uint32_t)(uintptr_t)smp_secondary_entry, k_cold_flag[cpu]);
        int rw = scm_set_boot_addr((uint32_t)(uintptr_t)smp_secondary_entry, k_warm_flag[cpu]);
        con_puts(" scm cold/warm rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts("/"); con_putdec((uint32_t)(rw < 0 ? -rw : rw)); con_puts("\n"); con_flush(); usb_poll();
        if (rc != 0) { con_puts("smp: TZ refused the cold-boot address -> cpu not released\n"); ok = 0; continue; }
        dc_inval(&g_smp_park_smc); uint32_t n0 = g_smp_park_smc;
        pon_crumb_write(0x50u + (uint8_t)cpu);        /* PMIC crumb: 0x52/0x53 = died releasing that core */
        unclamp(cpu);
        uint32_t t0 = timer_ms();
        while ((uint32_t)(timer_ms() - t0) < 50u) { dc_inval(&g_smp_park_smc); if (g_smp_park_smc != n0) break; }
        timer_delay_ms(3);
        dc_inval(&g_smp_park_landed); dc_inval(&g_smp_park_smc); dc_inval(&g_smp_park_decl);
        con_puts("smp: cpu"); con_putdec(cpu); con_puts(" park: cold/warm scm rc "); con_putdec((uint32_t)rc); con_puts("/"); con_putdec((uint32_t)rw);
        con_puts(" landed="); con_puthex(g_smp_park_landed); con_puts(" smc="); con_putdec(g_smp_park_smc - n0);
        con_puts(" declined="); con_putdec(g_smp_park_decl);
        con_puts(" sleep-status="); con_puthex(mmio_read(ACC_BASE(cpu) + 0x8u)); con_puts("\n");
        if (!(g_smp_park_landed & (1u << cpu))) ok = 0;
        pon_crumb_write(0x60u + (uint8_t)cpu);        /* survived releasing it */
#if defined(SMP_PARK_CPU2_ONLY)
        break;
#endif
    }
    tz_boot_counters("after cpu2/3 park");
    return ok;
}
int smp_flush_available(void) { return s_cpu1_up; }

/* Ask CPU1 to power-collapse itself (see g_smp_pc_req). Returns 1 when the
 * ACC sleep-status shows the core down within 20 ms, 0 otherwise. */
int smp_cpu1_pc_request_mode(uint32_t mode)
{
    g_smp_pc_mode = mode; dc_clean(&g_smp_pc_mode);
    return smp_cpu1_pc_request();
}
int smp_cpu1_pc_request(void)
{
    static int warm_set = 0;
    if (!s_cpu1_up || !spm_cpu1_ready()) return 0;
    if (!warm_set) {
        int rc = scm_set_warmboot_addr_cpu1((uint32_t)(uintptr_t)smp_secondary_entry);
        con_puts("smp: cpu1 warm-boot addr -> scm rc "); con_putdec((uint32_t)rc); con_puts("\n");
        if (rc != 0) return 0;
        warm_set = 1;
    }
    dc_inval(&g_smp_pc_n); uint32_t n0 = g_smp_pc_n;
    g_smp_pc_req = 1u; dc_clean(&g_smp_pc_req);
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(GICD_SGIR, (1u << 17) | 0u);
    uint32_t t0 = timer_ms();
    while ((uint32_t)(timer_ms() - t0) < 20u) { dc_inval(&g_smp_pc_n); if (g_smp_pc_n != n0) break; }
    timer_delay_ms(2);
    dc_inval(&g_smp_pc_decl);
    uint32_t sts = mmio_read(ACC_BASE(1) + 0x8u);
    con_puts("smp: cpu1 collapse: smc "); con_putdec(g_smp_pc_n - n0);
    con_puts(" declined "); con_putdec(g_smp_pc_decl);
    con_puts(" sleep-status "); con_puthex(sts); con_puts("\n");
    return (g_smp_pc_n != n0);
}

void smp_flush_post(const void *buf)
{
    g_smp_flush_addr = (uint32_t)(uintptr_t)buf; dc_clean(&g_smp_flush_addr);
    g_smp_flush_req  = ++s_flush_seq;            dc_clean(&g_smp_flush_req);
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(GICD_SGIR, (1u << 17) | 0u);      /* SGI0 -> CPU1: wake and serve */
    __asm__ volatile("dsb sy" ::: "memory");
}

int smp_flush_wait(void)
{
    uint32_t t0 = timer_ms();
    for (;;) {
        dc_inval(&g_smp_flush_ack);
        if (g_smp_flush_ack == s_flush_seq) { dc_inval(&g_smp_flush_err); return g_smp_flush_err ? -1 : 0; }
        if ((uint32_t)(timer_ms() - t0) > 200u) {
            /* CPU1 stopped answering: stop trusting it, core 0 flushes itself from here on. */
            s_cpu1_up = 0;
            con_puts("smp: cpu1 flush service timed out - back to sync flushes\n");
            return -2;
        }
    }
}

/* Bring CPU1 back after a sleep. First the cheap way: SGI0 should make the
 * SPM power the core up and TZ re-enter smp_secondary_entry. The first live
 * test (v149, 2026-09-07) showed the core staying down after that SGI
 * (flush service timed out), so if the beat does not move within 20 ms the
 * proven cold-boot recipe (BOOT_ADDR + ACC unclamp) is run instead. */
int smp_cpu1_wake(void)
{
    static int warm_failed;          /* v151 on the C2: TZ never warm-boots CPU1 -> cold boot directly */
    if (!s_cpu1_up) return 0;
    dc_inval(&g_smp_beat); uint32_t b0 = g_smp_beat, t0 = timer_ms();
    if (!warm_failed) {
        mmio_write(GICD_SGIR, (1u << 17) | 0u);
        __asm__ volatile("dsb sy" ::: "memory");
        while ((uint32_t)(timer_ms() - t0) < 20u) { dc_inval(&g_smp_beat); if (g_smp_beat != b0) break; }
        if (g_smp_beat != b0) { con_puts("smp: cpu1 back via SGI (warm boot)\n"); return 1; }
        warm_failed = 1;
        con_puts("smp: cpu1 did not warm-boot, sleep-status "); con_puthex(mmio_read(ACC_BASE(1) + 0x8u));
        con_puts(" - cold-booting it (and from now on)\n");
    }
    dc_inval(&g_smp_landed); g_smp_landed = 0; dc_clean(&g_smp_landed);
    (void)scm_set_boot_addr((uint32_t)(uintptr_t)smp_secondary_entry, SCM_FLAG_COLDBOOT_CPU1);
    unclamp(1);
    t0 = timer_ms();
    while ((uint32_t)(timer_ms() - t0) < 50u) { dc_inval(&g_smp_landed); if (g_smp_landed) break; }
    dc_inval(&g_smp_landed);
    if (g_smp_landed) { con_puts("smp: cpu1 back via cold boot\n"); return 1; }
    con_puts("smp: cpu1 LOST - sync flushes from now on\n");
    s_cpu1_up = 0;
    return 0;
}

/* CPU1 usage, 0..100, over ~2 s windows (same cadence as the app's poll).
 * -1 until the core is up. */
int pwr_cpu1_pct(void)
{
    static uint64_t t0; static uint32_t b0, pct;
    dc_inval(&g_smp_landed); dc_inval(&g_smp_busy_ticks);
    if (!g_smp_landed) return -1;
    uint64_t now = timer_ticks();
    if (t0 == 0) { t0 = now; b0 = g_smp_busy_ticks; return 0; }
    uint64_t win = now - t0;
    if (win >= 2ull * timer_freq_hz()) {
        uint32_t busy = g_smp_busy_ticks - b0;
        pct = (uint32_t)((uint64_t)busy * 100u / win); if (pct > 100u) pct = 100u;
        t0 = now; b0 = g_smp_busy_ticks;
    }
    return (int)pct;
}

/* ---- CPU0 side --------------------------------------------------------- */
static uint32_t s_t0, s_phase, s_sent;

static void acc_dump(unsigned cpu, const char *tag)
{
    con_dbg("smp: acc"); con_dbg_dec(cpu); con_dbg(" "); con_dbg(tag);
    con_dbg(" pwr_ctl="); con_dbg_hex(mmio_read(ACC_BASE(cpu) + CPU_PWR_CTL));
    con_dbg(" gate="); con_dbg_hex(mmio_read(ACC_BASE(cpu) + CPU_PWR_GATE_CTL));
    con_dbg("\n");
}

static void unclamp(unsigned cpu)
{
    uint32_t b = ACC_BASE(cpu);
    mmio_write(b + CPU_PWR_CTL, 0x00000033u); __asm__ volatile("dsb sy");      /* assert reset */
    mmio_write(b + CPU_PWR_GATE_CTL, 0x10000001u); __asm__ volatile("dsb sy"); /* skew 16 XO cycles */
    timer_delay_us(2);
    mmio_write(b + CPU_PWR_CTL, 0x00000031u); __asm__ volatile("dsb sy");      /* coremem clamp off */
    mmio_write(b + CPU_PWR_CTL, 0x00000039u); __asm__ volatile("dsb sy");      /* close coremem gdhs */
    timer_delay_us(2);
    mmio_write(b + CPU_PWR_CTL, 0x00020038u); __asm__ volatile("dsb sy");      /* cpu clamp off */
    timer_delay_us(2);
    mmio_write(b + CPU_PWR_CTL, 0x00020008u); __asm__ volatile("dsb sy");      /* reset off */
    mmio_write(b + CPU_PWR_CTL, 0x00020088u); __asm__ volatile("dsb sy");      /* PWRDUP */
}

static void report(const char *tag)
{
    dc_inval(&g_smp_landed); dc_inval(&g_smp_beat); dc_inval(&g_smp_sgi_rx);
    dc_inval(&g_smp_mpidr);  dc_inval(&g_smp_actlr);
    con_dbg("smp: "); con_dbg(tag);
    con_dbg(" landed="); con_dbg_dec(g_smp_landed);
    con_dbg(" beat="); con_dbg_dec(g_smp_beat);
    con_dbg(" sgi tx/rx="); con_dbg_dec(s_sent); con_dbg("/"); con_dbg_dec(g_smp_sgi_rx);
    con_dbg(" mpidr="); con_dbg_hex(g_smp_mpidr);
    con_dbg(" actlr="); con_dbg_hex(g_smp_actlr);
    con_dbg("\n");
}

/* Call from the loop; runs the whole test on its own clock once started. */
void smp_cpu1_test_poll(void)
{
    uint32_t now = timer_ms();
    if (s_phase == 0u) {
        s_phase = 1u; s_t0 = now;
        uint32_t a0; __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(a0));
        con_dbg("smp: cpu0 mpidr="); con_dbg_hex(rd_cp()); con_dbg(" actlr="); con_dbg_hex(a0);
        con_dbg(" entry="); con_dbg_hex((uint32_t)(uintptr_t)smp_secondary_entry); con_dbg("\n");
        acc_dump(0, "cpu0"); acc_dump(1, "before");
        dc_clean(&g_smp_landed);
        int rc = scm_set_boot_addr((uint32_t)(uintptr_t)smp_secondary_entry, SCM_FLAG_COLDBOOT_CPU1);
        con_puts("smp: scm BOOT_ADDR(cold cpu1) rc="); con_putdec((uint32_t)rc); con_puts("\n");
        con_flush();
        if (rc != 0) { s_phase = 9u; return; }
        unclamp(1);
        acc_dump(1, "after");
        return;
    }
    if (s_phase == 1u && now - s_t0 >= 300u) { s_phase = 2u; s_t0 = now; report("300 ms"); return; }
    if (s_phase >= 2u && s_phase < 7u && now - s_t0 >= 1000u) {
        s_t0 = now;
        mmio_write(GICD_SGIR, (1u << 17) | 0u);   /* target list = CPU1, SGI 0 */
        __asm__ volatile("dsb sy");
        s_sent++;
        timer_delay_us(200);
        report("tick");
        s_phase++;
        if (s_phase == 7u) {
            con_puts(g_smp_landed ? "smp: cpu1 up (frame push service + idle accounting)\n"
                                  : "smp: cpu1 did NOT come up\n");
            if (g_smp_landed) s_cpu1_up = 1;
            acc_dump(1, "final");
#if defined(SMP_PARK_CPU23)
            if (g_smp_landed) (void)smp_park_extra_cores();
#endif
        }
    }
}

#endif /* PLAT_SOC_MSM8909 */
