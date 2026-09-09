/* cpu_pc8909.c — CPU power collapse on the msm8909w (Gen 4 / C2): rung 2 of
 * the sleep ladder. The core really switches off; L2, DDR, the XO and the
 * rails stay up (those are rungs 3 and 4: L2 power collapse with the RPM
 * handshake, and the RPM sleep set).
 *
 * How Linux does it on this SoC (drivers/power/qcom/msm-pm.c, pm-boot.c,
 * firefish 3.18 branch; qcom,pc-mode = "tz_l2_int", no PSCI):
 *   boot:   scm_set_boot_addr(msm_pm_boot_entry, WARMBOOT_CPU0)
 *   sleep:  msm_spm_set_low_power_mode(PC) ; __cpu_suspend(msm_pm_collapse)
 *           msm_pm_collapse = flush caches ; scm_call_atomic1(BOOT, TERMINATE_PC, l2flag)
 *   wake:   TZ warm-boots cpu0 at the registered address, MMU off; cpu_resume
 *           restores the context saved by __cpu_suspend.
 * Here: scm_set_warmboot_addr(cpu_pc_resume) once, spm_cpu0_mode(1) (the
 * standalone-pc sequence, no RPM notify), cpu_pc_suspend_scm(0) from
 * cpu_suspend.S (context save + L1 clean + the SCM call), cpu_pc_resume is the
 * warm-boot entry and returns 0 to the caller as if nothing happened.
 *
 * WAKE SOURCES while the core is off. The per-core architected timer dies
 * with the core, so the wake timer is the memory-mapped QTimer frame 1
 * (timer@b020000/frame@b023000, "arm,armv7-timer-mem", the kernel's
 * broadcast timer on this SoC): it runs from the same 19.2 MHz system counter
 * and raises GIC SPI 9 (irq 41), which the always-on GIC distributor delivers
 * once the core is back. The PMIC button line (SPI 190) is the other one.
 * The GIC distributor keeps its state through a cpu-only collapse; only the
 * CPU interface (gic_cpu_resume) and the tick (tick_rearm) are re-done. */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)
#include "FreeRTOS.h"
#include "task.h"

#define QT_CTL_BASE   0x0B020000u
#define QT_FRAME1     0x0B023000u
#define QT_CNTPCT_LO  0x00u
#define QT_CNTPCT_HI  0x04u
#define QT_CVAL_LO    0x20u
#define QT_CVAL_HI    0x24u
#define QT_CTL        0x2Cu
#define QT_IRQ        41u              /* SPI 9 */
#define QT_CNTNSAR    0x04u
#define QT_CNTACR1    0x44u

extern int  cpu_pc_suspend_scm(uint32_t l2_flag);   /* cpu_suspend.S */
extern void cpu_pc_resume(void);
extern volatile uint32_t g_smp_beat;                 /* smp_8909.c */
int cpu_volt_mv(void);                               /* cpu_volt_a7.c */
uint32_t g_cpu_pc_resumes;          /* bumped by cpu_pc_resume (assembly) */
uint32_t g_cpu_pc_l2_off;
uint32_t g_gicd_restored;
uint32_t g_cpu_pc_slp_cmd;          /* sys_pc8909.c stage 4: CPU0 SAW mode 2 = pc + SLP_CMD (RPM handshake from the CPU SAW) */           /* times the distributor came back reset and was restored */           /* sys_pc8909.c: next collapse takes the cluster down (TZ flag 1) */
uint32_t g_cpu_pc_attempts, g_cpu_pc_declined, g_cpu_pc_ok, g_cpu_pc_last_ms;
int32_t  g_cpu_pc_last_rc;
static int s_ready, s_qt_irqs;
static int64_t s_last_voff_delta;
extern char __ramlog_end[];
/* Same DDR slot cpu_suspend.S stamps; the C side continues the trail. */
/* Text breadcrumb ON FLASH for the first two attempts of a boot (eMMC write,
 * needs the scheduler: only call where interrupts are enabled). */
void cpu_pc8909_trace(uint32_t v)
{
#if !defined(PC_TRACE)
    (void)v; return;                  /* bring-up only: each one is an eMMC write on the wake path */
#endif
    if (g_cpu_pc_attempts > 2u) return;
    con_puts("cpu-pc: mark "); con_puthex(v); con_puts("\n");
    con_flush(); blackbox_sync();
}
#define PC_IMEM_MARK  0x08600820u      /* mirrored breadcrumb, survives the warm reset + a Wear OS boot */
#define PC_IMEM_MAGIC 0x504B4D43u      /* "CMKP" */
static void pc_mark(uint32_t v)
{
    *(volatile uint32_t *)(__ramlog_end - 16) = v;
    mmio_write(PC_IMEM_MARK, v); mmio_write(PC_IMEM_MARK + 4u, PC_IMEM_MAGIC);
    __asm__ volatile("dsb sy" ::: "memory");
}
void cpu_pc8909_mark(uint32_t v) { pc_mark(v); }

static uint64_t qt_now(void)
{
    uint32_t hi, lo, hi2;
    do { hi = mmio_read(QT_FRAME1 + QT_CNTPCT_HI); lo = mmio_read(QT_FRAME1 + QT_CNTPCT_LO); hi2 = mmio_read(QT_FRAME1 + QT_CNTPCT_HI); } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}
static void qt_irq(void *arg)
{
    (void)arg;
    mmio_write(QT_FRAME1 + QT_CTL, 0);                 /* one-shot: disable + clear */
    s_qt_irqs++;
}
static void qt_arm(uint64_t deadline)
{
    mmio_write(QT_FRAME1 + QT_CTL, 0);
    mmio_write(QT_FRAME1 + QT_CVAL_LO, (uint32_t)deadline);
    mmio_write(QT_FRAME1 + QT_CVAL_HI, (uint32_t)(deadline >> 32));
    mmio_write(QT_FRAME1 + QT_CTL, 1);                 /* enable, unmasked */
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Boot-time: SPM sequences, warm-boot address, wake-timer frame. Prints one
 * line per step so a refusal is visible. */
void cpu_pc8909_init(void)
{
    spm_init();
    if (!spm_ready()) { con_puts("cpu-pc: no SPM, power collapse disabled\n"); return; }
    int rc = scm_set_warmboot_addr((uint32_t)(uintptr_t)cpu_pc_resume);
    con_puts("cpu-pc: warm-boot address "); con_puthex((uint32_t)(uintptr_t)cpu_pc_resume);
    con_puts(" -> scm rc "); con_putdec((uint32_t)rc); con_puts("\n");
    if (rc != 0) { con_puts("cpu-pc: TZ refused the warm-boot address -> power collapse DISABLED, WFI suspend only\n"); return; }
    /* v160: ALSO the multi-cluster form, the one the vendor kernel actually
     * uses on this TZ, with one dispatching entry for both cores. */
    if (scm_mc_boot_available()) {
        int rm = scm_set_warmboot_addr_mc_all((uint32_t)(uintptr_t)pc_warm_entry);
        con_puts("cpu-pc: BOOT_ADDR_MC warm(all) -> "); con_puthex((uint32_t)(uintptr_t)pc_warm_entry);
        con_puts(" rc "); con_putdec((uint32_t)(rm < 0 ? -rm : rm)); con_puts(rm ? " (neg)\n" : "\n");
    } else con_puts("cpu-pc: BOOT_ADDR_MC not offered by TZ\n");
    { uint32_t actlr; __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr)); con_puts("cpu-pc: ACTLR at boot "); con_puthex(actlr); con_puts("\n"); }
    con_puts("cpu-pc: qtimer frame1 CNTNSAR="); con_puthex(mmio_read(QT_CTL_BASE + QT_CNTNSAR));
    con_puts(" CNTACR1="); con_puthex(mmio_read(QT_CTL_BASE + QT_CNTACR1));
    con_puts(" cntpct="); con_puthex((uint32_t)qt_now()); con_puts(" cntvct="); con_puthex((uint32_t)timer_ticks()); con_puts("\n");
    irq_register(QT_IRQ, qt_irq, 0);
    gic_enable_irq(QT_IRQ, 0xA0u);
    pon_ps_hold_hard();                 /* v121: warm broke USB enumeration; flash breadcrumbs instead */
    s_ready = 1;
#if defined(SYS_PC_8909)
    (void)sys_pc8909_init();
#endif
}

int cpu_pc8909_ready(void) { return s_ready; }

/* What the PREVIOUS life left behind: the IMEM breadcrumb cpu_suspend.S stamps
 * at each step (0x12 = SCM issued, never re-entered; 0x20..0x23 = TZ woke us
 * and the restore faulted at that step; 0x24 = restore done, died in C) and
 * the PM8916 PON block's reset reasons (SID 0: 0x808 PON_REASON1, 0x80A
 * WARM_RESET_REASON1/2, 0x80C ON_REASON, 0x80E POFF_REASON1/2,
 * 0x810 SOFT_RESET_REASON1/2). Call once at boot, before any collapse. */
void cpu_pc8909_prev_report(void)
{
    extern char __ramlog_end[];
#if defined(SYS_PC_WARM_RESET_DIAG)
    con_puts("tz-log tail (previous life):\n"); tz_log_tail(600u); con_puts("\n");
    tz_boot_counters("previous life");
#endif
#if defined(SYS_PC_8909)
    pon_crumb_report();
#if !defined(SYS_PC_WARM_RESET_DIAG)
    tz_boot_counters("previous life");   /* IMEM survives the PMIC reset (v158): did TZ start the warm boot? */
#endif
#endif
    {   /* v192: CPU fault record from the previous life (startup.S fault_stub) */
        volatile uint32_t *f = (volatile uint32_t *)(__ramlog_end - 32);
        if ((f[0] & 0xFF000000u) == 0xFA000000u) {
            uint32_t cls = f[0] & 0xFFu;
            con_puts("!! FAULT in previous life: "); con_puts(cls == 1u ? "UNDEF" : cls == 3u ? "PREFETCH ABORT" : cls == 4u ? "DATA ABORT" : cls == 5u ? "FIQ" : cls == 6u ? "MALLOC FAILED (FreeRTOS heap)" : cls == 7u ? "STACK OVERFLOW (task tag in far)" : cls == 8u ? "DEAD-MAN TIMEOUT (30 s without a loop kick)" : "?");
            con_puts(" lr="); con_puthex(f[1]); con_puts(" (pc ~ lr-8 for a data abort, lr-4 otherwise)");
            con_puts(" fsr="); con_puthex(f[2]); con_puts(" far="); con_puthex(f[3]);
            con_puts("  -> arm-none-eabi-addr2line -e build/<board>/owf.elf 0x"); con_puthex(f[1] - (cls == 4u ? 8u : 4u)); con_puts("\n");
            f[0] = 0u;
        }
    }
    volatile uint32_t *slot = (volatile uint32_t *)(__ramlog_end - 16);
    uint32_t mark = *slot; const char *src = "DDR";
    if (mmio_read(PC_IMEM_MARK + 4u) == PC_IMEM_MAGIC) {
        mark = mmio_read(PC_IMEM_MARK); src = "IMEM";
        mmio_write(PC_IMEM_MARK + 4u, 0u);            /* consume: report each life once */
    }
    con_puts("cpu-pc: "); con_puts(src); con_puts(" mark from previous life = "); con_puthex(mark); con_puts(" (");
    switch (mark) {
    case 0x10u: case 0x11u: con_puts("died before the SCM call"); break;
    case 0x12u: con_puts("SCM issued, never re-entered: TZ did not warm-boot our address"); break;
    case 0x13u: con_puts("SCM returned without collapsing"); break;
    case 0x20u: case 0x21u: case 0x22u: case 0x23u: con_puts("TZ re-entered us, restore faulted at this step"); break;
    case 0x24u: con_puts("restore completed, died before gic_cpu_resume"); break;
    case 0x30u: con_puts("died in gic_cpu_resume"); break;
    case 0x31u: con_puts("died in tick_rearm"); break;
    case 0x32u: con_puts("died clearing the qtimer frame"); break;
    case 0x33u: con_puts("died on cpsie / first interrupt"); break;
    case 0x34u: con_puts("died in spm_cpu0_mode(0)"); break;
    case 0x35u: con_puts("died in wdog_extend"); break;
    case 0x36u: con_puts("died in xTaskCatchUpTicks"); break;
    case 0x37u: con_puts("cpu-pc returned to the suspend loop, died there"); break;
    case 0x38u: con_puts("died after cpu_pc8909_report"); break;
    case 0x39u: con_puts("died in the chunk housekeeping"); break;
    case 0x3Au: con_puts("suspend loop ended, died on the way back to the app"); break;
    case 0x3Bu: con_puts("report + blackbox commit done, died before housekeeping (wdog_pet/deadman_kick)"); break;
    case 0x3Cu: con_puts("died in usb_poll() after a resume"); break;
    case 0x3Du: con_puts("died in con_flush()/wake checks after a resume"); break;
    case 0x3Eu: con_puts("next chunk started, died before/inside cpu_pc8909_sleep entry (timer reads)"); break;
    case 0x3Fu: con_puts("died in qt_arm() of the next attempt"); break;
    case 0x40u: con_puts("qtimer armed for the next attempt, died before/in the pre-SMC print or blackbox"); break;
    default: con_puts("no collapse attempt, or IMEM not retained"); break;
    }
    con_puts(")\n");
    *slot = 0u;
    static const uint16_t regs[] = { 0x808, 0x809, 0x80A, 0x80B, 0x80C, 0x80E, 0x80F, 0x810, 0x811 };
    con_puts("pon: reasons");
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        uint8_t v = 0;
        con_puts(" "); con_puthex(regs[i]); con_puts("=");
        if (spmi_read8(0, regs[i], &v) == 0) con_puthex(v); else con_puts("--");
    }
    con_puts("\n");
    ramlog_prev_tail(1500u);
}

/* Collapse cpu0 until `wake_ticks` (system-counter value) or an interrupt.
 * Returns 1 if the core really went down and came back, 0 if TZ declined
 * (nothing lost; caller falls back to WFI for this chunk). */
int cpu_pc8909_sleep(uint64_t wake_ticks)
{
    if (!s_ready) return 0;
    uint32_t before = g_cpu_pc_resumes;
    uint64_t t0 = qt_now();                             /* always-on physical counter */
    uint64_t v0 = timer_ticks();
    uint32_t hz = timer_freq_hz();
    g_cpu_pc_attempts++;
    pc_mark(0x3F); cpu_pc8909_trace(0x3F);              /* cpu_pc8909_sleep entered, before qt_arm */

    /* Wake timer on the always-on frame. CNTPCT and CNTVCT are the same
     * counter here (no hypervisor offset), verified by the boot line. */
    qt_arm(wake_ticks);
    pc_mark(0x40); cpu_pc8909_trace(0x40);              /* qtimer armed */
    if (g_cpu_pc_attempts == 1u) {   /* one line per sleep, no flash commit (bring-up: -DPC_TRACE) */
        /* The first few collapses on a boot each leave a line ON FLASH before
         * the SMC (and the resume report after, see suspend_msm.c), so a resume
         * that never lands is pinned to its attempt number. Attempt 1 resumed
         * and attempt 2 died on the C2 (2026-09-06); attempt 1 alone was logged. */
        con_puts("cpu-pc: attempt "); con_putdec(g_cpu_pc_attempts);
        con_puts(": chunk "); con_putdec((uint32_t)((wake_ticks - v0) / (hz / 1000u)));
        con_puts(" ms, issuing terminate-pc SMC\n");
        con_flush(); usb_poll();
#if defined(PC_TRACE)
        timer_delay_ms(20); usb_poll(); blackbox_sync();
#endif
    }

    deadman_disarm();
    /* The APPS watchdog runs on the sleep clock and keeps counting through a
     * CPU collapse. Leave it ARMED at its 31 s ceiling: every sleep chunk is
     * <= 15 s so a successful resume always pets it in time, and a resume that
     * never lands reboots the watch (ramlog breadcrumbs survive the warm reset
     * and cpu_pc8909_prev_report() names the step) instead of hanging it. */
#if !defined(SLEEP_NO_WDOG)
    wdog_extend(31u);
#endif
    tick_stop();
    spm_cpu0_mode(g_cpu_pc_slp_cmd ? 2 : 1);           /* 1 = standalone pc, 2 = pc + SLP_CMD (kernel suspend) */

    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("cpsid if" ::: "memory");
    if (g_cpu_pc_l2_off) { pon_crumb_write(0x11u); ramlog_clean_to_ddr(); }
    int32_t rc = cpu_pc_suspend_scm(g_cpu_pc_l2_off == 3u ? 3u : (g_cpu_pc_l2_off ? 1u : 0u));   /* 1 = L2_OFF, 3 = L2_GDHS */
    int collapsed = (g_cpu_pc_resumes != before);
    if (g_cpu_pc_l2_off && collapsed) pon_crumb_write(0x20u);   /* SPMI arbiter: always-on, no cache */
    pc_mark(0x30);
    if (collapsed) {
        /* v156: restore the distributor ONLY if the cluster power-down actually
         * reset it (CTLR reads 0). v154 stage 0 proved a blind restore breaks
         * the wake when the distributor survived. */
        if (g_cpu_pc_l2_off && mmio_read(PLAT_GICD_BASE) == 0u) { gicd_restore(); g_gicd_restored++; }
        gic_cpu_resume(); pc_mark(0x31); tick_rearm(); pc_mark(0x32);
        if (g_cpu_pc_l2_off) pon_crumb_write(0x21u);
    }
    mmio_write(QT_FRAME1 + QT_CTL, 0);                 /* drop the level IRQ before unmasking */
    pc_mark(0x33);
    if (!(cpsr & 0x80u)) __asm__ volatile("cpsie i" ::: "memory");
    pc_mark(0x34);
    spm_cpu0_mode(0);
    pc_mark(0x35);
#if !defined(SLEEP_NO_WDOG)
    wdog_extend(30u);
#endif
    pc_mark(0x36);

    uint64_t elapsed = qt_now() - t0;
    if (elapsed > (uint64_t)hz * 3600u) elapsed = (uint64_t)hz * 3600u;   /* never trust more than an hour */
    g_cpu_pc_last_ms = (uint32_t)(elapsed / (hz / 1000u));
    g_cpu_pc_last_rc = rc;
    /* CNTVOFF check: if TZ's warm boot moved the virtual counter, every
     * timer_ticks() consumer would be confused; report the delta. */
    s_last_voff_delta = (int64_t)(timer_ticks() - v0) - (int64_t)elapsed;
    if (collapsed) {
        g_cpu_pc_ok = 1u;
        uint32_t period = hz / configTICK_RATE_HZ;
        uint32_t missed = (uint32_t)(elapsed / period);
        if (missed > 60u * configTICK_RATE_HZ) missed = 60u * configTICK_RATE_HZ;
        /* v64: plain tick step instead of xTaskCatchUpTicks (the v63 hang
         * was inside the latter). Due tasks unblock on the next real tick. */
        /* No scheduler catch-up at all for this experiment: task delays
         * simply do not see the slept time (the app clocks itself from the
         * RTC epoch, so nothing user-visible depends on it). */
        (void)missed;
        pc_mark(0x37);
    } else {
        g_cpu_pc_declined++;
        tick_rearm();
    }
    return collapsed;
}

/* WHAT DOES A COLLAPSE LEAVE BEHIND? (2026-09-07) The collapse path drains
 * the battery FASTER than the plain WFI suspend it replaced, and the core
 * itself is provably off (attempts == resumes, one QTimer wake per chunk).
 * So something around the core is in a different state after a TZ warm boot
 * than before the first collapse. This line is the same set of registers
 * before the first attempt, after each reported one, and at sleep exit;
 * whichever field differs is the suspect. All reads, no writes. */
void cpu_pc8909_state_line(const char *tag)
{
    con_puts("cpu-pc state["); con_puts(tag); con_puts("]:");
    con_puts(" a7cmd=");  con_puthex(mmio_read(0x0B011050u));
    con_puts(" a7cfg=");  con_puthex(mmio_read(0x0B011054u));      /* mux sel [10:8], div [4:0] */
    con_puts(" a7pll=");  con_puthex(mmio_read(0x0B016000u));      /* PLL MODE: bit0 outctl, 1 bypass, 2 reset, 31 lock */
    con_puts(" apc_mv="); con_putdec((uint32_t)cpu_volt_mv());
    con_puts(" spm0ctl="); con_puthex(mmio_read(0x0B089030u));
    con_puts(" spm0sts="); con_puthex(spm_cpu0_sts());
    con_puts(" l2ctl=");  con_puthex(mmio_read(0x0B012030u));
    con_puts(" l2sts=");  con_puthex(spm_l2_sts());
    con_puts(" acc0=");   con_puthex(mmio_read(0x0B088008u));      /* cpu-sleep-status */
    con_puts(" acc1=");   con_puthex(mmio_read(0x0B098008u));
    con_puts(" acc0pwr="); con_puthex(mmio_read(0x0B088004u));     /* CPU_PWR_CTL */
    con_puts(" mdss_gdsc="); con_puthex(mmio_read(PLAT_GCC_BASE + 0x4D078u));
    con_puts(" cpu1_beat="); con_putdec(g_smp_beat);
    con_puts("\n");
}

void cpu_pc8909_report(void)
{
    con_puts("cpu-pc: attempts "); con_putdec(g_cpu_pc_attempts);
    con_puts(" resumes "); con_putdec(g_cpu_pc_resumes);
    con_puts(" declined "); con_putdec(g_cpu_pc_declined);
    con_puts(" last "); con_putdec(g_cpu_pc_last_ms); con_puts(" ms rc "); con_putdec((uint32_t)g_cpu_pc_last_rc);
    con_puts(" qt-irqs "); con_putdec((uint32_t)s_qt_irqs);
    con_puts(" spm-sts "); con_puthex(spm_cpu0_sts());
    con_puts(" vct-pct-delta "); con_putdec((uint32_t)(s_last_voff_delta < 0 ? -s_last_voff_delta : s_last_voff_delta));
    { uint32_t actlr, cntfrq; __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr)); __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(cntfrq));
      con_puts(" actlr "); con_puthex(actlr); con_puts(" cntfrq "); con_putdec(cntfrq); }
    con_puts("\n");
    cpu_pc8909_state_line("post-resume");
}

#endif /* PLAT_SOC_MSM8909 */

/* Referenced by cpu_suspend.S's PSCI CPU_ON landing pad (Gen 6 only); unused here. */
uint32_t g_psci_cpu_on_landed;
