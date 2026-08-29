/* cpu_pc.c — CPU power-collapse sleep: the C half of warm boot.
 *
 * cpu_suspend.S does the register-level save/restore. This file does the
 * things that must happen AROUND it, in C, where they can be read:
 * composing the state ID, standing the per-CPU hardware back up, putting the
 * scheduler's clock right, and keeping the two watchdogs from shooting a
 * perfectly healthy sleep.
 *
 * WHY THIS IS THE STEP THAT MATTERS. Everything before it — tickless suspend,
 * perf-l2-wfi, interrupt wake — kept the core POWERED. This is the first
 * state where the CPU genuinely switches off, which is where the large power
 * saving lives, and it is the gateway to system-ret and system-pc: those need
 * cpu-pc as their child state (qcom,min-child-idx), so nothing deeper is
 * reachable until this works.
 *
 * THE STATE ID (extended format, confirmed by PSCI_FEATURES=3 on hardware):
 *   cpu "pc" is qcom,psci-cpu-mode = 3 at shift 0 -> StateID 0x003
 *   StateType = POWER-DOWN, bit 30
 * so power_state = 0x40000003. Composition rule is the same one already
 * proven by 0x010 (perf-l2-wfi) being accepted with psci_fail=0.
 *
 * WHAT A POWER-DOWN TAKES WITH IT, and who puts it back:
 *   CPU registers, MMU, L1, VFP ....... cpu_suspend.S
 *   GIC CPU interface ................. gic_cpu_resume()
 *   GIC PPI (per-CPU banked) enables .. tick_rearm()
 *   CNTV_CTL / CNTV_CVAL (the tick) ... tick_rearm()
 *   SPI enables, priorities ........... nothing — the distributor is in the
 *                                       always-on domain and survives
 *   CNTVCT (the counter itself) ....... nothing — always-on system counter,
 *                                       which is exactly why we can measure
 *                                       how long we were gone
 *
 * THE TWO WATCHDOGS. Nothing can wake this core except the PMIC interrupt:
 * the architected timer is gone, so there is no way to wake up periodically
 * and pet anything. Both dogs must therefore stand down for the duration:
 *   - hardware watchdog: wdog_disable() / re-armed on resume. Its window
 *     cannot exceed ~32 s anyway (20-bit at 32765 Hz), so it can never cover
 *     a real sleep.
 *   - the deadman: a FreeRTOS software timer that reboots to fastboot if
 *     un-kicked. It is TICK-driven, so it does not run while we are down —
 *     but xTaskCatchUpTicks() on resume would replay the whole elapsed time
 *     at once and fire it. Disarm before, leave disarmed.
 * The trade is deliberate: during a power-collapsed sleep a hang cannot be
 * distinguished from a healthy sleep by any timer we still own, so the
 * recovery path is the hardware one (hold crown + lower ~10-15 s for
 * fastboot, or hold power to switch off) rather than a watchdog.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#include "FreeRTOS.h"
#include "task.h"

/* cpu "pc": psci-cpu-mode 3 at shift 0, StateType power-down (bit 30). */
#define CPU_PC_POWER_STATE   0x40000003u

extern int cpu_pc_suspend(uint32_t power_state);   /* cpu_suspend.S */
uint32_t g_cpu_pc_resumes;      /* incremented BY THE ASSEMBLY resume path */
uint32_t g_cpu_pc_attempts;
uint32_t g_cpu_pc_declined;     /* PSCI returned instead of powering down */
int32_t  g_cpu_pc_last_rc;
uint32_t g_cpu_pc_last_ms;      /* how long the last collapse actually lasted */

/* 1 once the state has been accepted at least once — suspend_msm.c uses this
 * to decide whether cpu-pc is trustworthy for subsequent sleeps. */
uint32_t g_cpu_pc_ok;

/* Enter cpu-pc and come back. Returns 1 if the core really powered down,
 * 0 if PSCI declined (nothing lost; the caller should fall back). */
int cpu_pc_sleep(void)
{
    uint32_t before = g_cpu_pc_resumes;
    uint64_t t0     = timer_ticks();
    uint32_t hz     = timer_freq_hz();

    g_cpu_pc_attempts++;

    /* ARM THE ALWAYS-ON WAKE FIRST — this is what the first attempt lacked.
     * The GIC cannot bring back a core that is not powered; the MPM can, and
     * pin 62 is the SPMI arbiter line carrying every PMIC wake source we use.
     * If arming does not read back, DO NOT collapse: a core that goes down
     * with no wake source configured stays down until a hardware power-on,
     * which is precisely the failure this replaces. */
    if (!mpm_arm_pmic_wake()) {
        g_cpu_pc_declined++;
        con_puts("cpu-pc: MPM arm FAILED (pin 62 did not latch) -"
                 " refusing to power down\n");
        con_flush();
        return 0;
    }

    /* Stand both dogs down — see the header comment. */
    deadman_disarm();
    wdog_disable();

    /* LAST WORDS. Everything after this may be unrecoverable, so flush a full
     * picture of what we are collapsing WITH: the armed MPM state, and whether
     * the PMIC wake line is already pending at the distributor. If the watch
     * dies, this line is the whole post-mortem. */
    mpm_dump("armed");
    con_puts("cpu-pc: gic222_pending="); con_putdec((uint32_t)gic_is_pending(222u));
    con_puts(" state=");                 con_puthex(CPU_PC_POWER_STATE);
    con_puts(" -> collapsing now\n");
    con_flush();

    /* PSCI PROTOCOL: CPU_SUSPEND must be called with interrupts masked at
     * PSTATE (Linux's cpu_suspend() does exactly this). We were calling it
     * with IRQs enabled and the 1 kHz tick still armed, so the tick — and
     * potentially the very interrupt meant to wake us — could be taken inside
     * the power-down sequence. Silence the tick, mask IRQs, collapse; the
     * wake source stays enabled at the GIC and the MPM, which is what the
     * power controller watches. */
    tick_stop();
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("cpsid if" ::: "memory");

    int32_t rc = cpu_pc_suspend(CPU_PC_POWER_STATE);

    if (!(cpsr & 0x80u)) __asm__ volatile("cpsie i" ::: "memory");

    /* --- we are either back from the dead or PSCI never went ------------- */
    int collapsed = (g_cpu_pc_resumes != before);

    if (collapsed) {
        /* Per-CPU hardware first: without the GIC CPU interface no interrupt
         * is ever presented again, and without the tick nothing schedules. */
        gic_cpu_resume();
        tick_rearm();
    }

    mpm_disarm_pmic_wake();
    wdog_extend(30u);

    /* CNTVCT ran the whole time, so this is real elapsed time, not a guess. */
    uint64_t elapsed = timer_ticks() - t0;
    uint32_t ms      = (uint32_t)(elapsed / (hz / 1000u));
    g_cpu_pc_last_ms = ms;
    g_cpu_pc_last_rc = rc;

    if (collapsed) {
        g_cpu_pc_ok = 1u;
        /* Give the scheduler back the time it missed. Same contract as
         * tick_unpark(), but the tick hardware was gone rather than parked, so
         * the count comes from the always-on counter. */
        uint32_t period  = hz / configTICK_RATE_HZ;
        uint32_t missed  = (uint32_t)(elapsed / period);
        if (missed) xTaskCatchUpTicks((TickType_t)missed);
    } else {
        g_cpu_pc_declined++;
    }
    return collapsed;
}

/* ---------------------------------------------------------------------------
 * PSCI_SYSTEM_SUSPEND — the other suspend-to-RAM door.
 *
 * CPU_SUSPEND makes US responsible for composing a platform StateID and for
 * everything the platform needs done around the collapse. SYSTEM_SUSPEND takes
 * no state at all: "put the whole system in its deepest state, re-enter me
 * here". If TZ implements it, the firmware owns the part we have been guessing
 * at for three attempts — and it is the ONLY call in the spec that is defined
 * to reach system-pc, which the DTB otherwise gates behind cpu-pc.
 *
 * The warm-boot contract is identical to CPU_SUSPEND's, so cpu_pc_resume is
 * the entry point unchanged and the save/restore path — already proven sound
 * by the selftest — is reused as-is. Only the SMC in the middle differs, which
 * is exactly why this is worth a flash cycle.
 * -------------------------------------------------------------------------- */
extern int cpu_sys_suspend(uint32_t entry);   /* cpu_suspend.S */

int cpu_pc_system_suspend(void)
{
    if (!g_psci_sys_suspend_ok) return 0;

    uint32_t before = g_cpu_pc_resumes;
    uint64_t t0     = timer_ticks();
    uint32_t hz     = timer_freq_hz();

    g_cpu_pc_attempts++;

    if (!mpm_arm_pmic_wake()) {
        g_cpu_pc_declined++;
        con_puts("sys-suspend: MPM arm FAILED - refusing\n"); con_flush();
        return 0;
    }

    deadman_disarm();
    wdog_disable();

    mpm_dump("armed");
    con_puts("sys-suspend: gic222_pending=");
    con_putdec((uint32_t)gic_is_pending(222u));
    con_puts(" -> PSCI_SYSTEM_SUSPEND now\n");
    con_flush();

    tick_stop();
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("cpsid if" ::: "memory");

    int32_t rc = cpu_sys_suspend(0u);

    if (!(cpsr & 0x80u)) __asm__ volatile("cpsie i" ::: "memory");

    int collapsed = (g_cpu_pc_resumes != before);
    if (collapsed) { gic_cpu_resume(); tick_rearm(); }

    mpm_disarm_pmic_wake();
    wdog_extend(30u);

    uint64_t elapsed = timer_ticks() - t0;
    g_cpu_pc_last_ms = (uint32_t)(elapsed / (hz / 1000u));
    g_cpu_pc_last_rc = rc;

    if (collapsed) {
        g_cpu_pc_ok = 1u;
        uint32_t missed = (uint32_t)(elapsed / (hz / configTICK_RATE_HZ));
        if (missed) xTaskCatchUpTicks((TickType_t)missed);
    } else {
        g_cpu_pc_declined++;
        con_puts("sys-suspend: PSCI returned WITHOUT suspending, rc=");
        con_putdec((uint32_t)rc); con_puts("\n"); con_flush();
    }
    return collapsed;
}

extern int cpu_pc_selftest(void);          /* cpu_suspend.S */
uint32_t g_cpu_pc_selftest_ok;

/* Exercise the whole save/restore path with the core still POWERED.
 *
 * The real thing already proved the power-down works — the watch went dead and
 * only a cold boot brought it back. What that cannot tell us is whether the
 * core was never woken, or woke and faulted in our restore code. This does:
 * it saves context, drops the MMU and caches by hand (exactly the state PSCI
 * re-enters us in) and runs cpu_pc_resume for real. Everything is tested
 * except the power-down and the wake.
 *
 * The "about to" line is flushed FIRST, so if the restore path dies the log
 * ends on it and names the failure precisely. */
void cpu_pc_selftest_run(void)
{
    diag_puts("cpu-pc: selftest — running the resume path with the core "
             "powered (MMU/caches off, then restore)\n");
    diag_flush();

    uint32_t before = g_cpu_pc_resumes;
    cpu_pc_selftest();

    /* Getting here at all means the restore path returned through its pop. */
    g_cpu_pc_selftest_ok = 1u;
    diag_puts("cpu-pc: selftest SURVIVED — save/restore is sound "
             "(resume-path counter ");
    diag_putdec(g_cpu_pc_resumes - before);
    diag_puts("). Remaining unknown is the WAKE, not the restore.\n");
    diag_flush();
}

/* ---------------------------------------------------------------------------
 * POST-MORTEM OF THE PREVIOUS LIFE.
 *
 * Three cpu-pc attempts have ended in a cold boot, and the log always stops at
 * the same place, so nothing in the log can say WHERE it died. IMEM can: it is
 * on-die SRAM that keeps its contents across a warm reset, and cpu_suspend.S
 * stamps it at every step. Read it at boot, BEFORE anything overwrites it, and
 * the last value names the last step the dead core reached:
 *
 *   (nothing/garbage) IMEM was cleared -> the reset was a full power-on, not a
 *                     warm reset; the SoC really did drop its rails
 *   0x12  reached the SMC and never came back to any of our code. The core
 *         powered down and the wake did NOT deliver control to cpu_pc_resume:
 *         the fault is in TZ's wake path, not in our restore code
 *   0x20-0x23  the core WOKE and our restore path faulted at that step
 *   0x24  restore completed; whatever killed us is downstream, in C
 *
 * That single word decides which half of the problem is real, and costs one
 * read. -------------------------------------------------------------------- */
#define CPU_PC_MARK_ADDR  0x08600840u
static uint32_t s_prev_mark;

void cpu_pc_prev_mark_report(void)
{
    s_prev_mark = mmio_read(CPU_PC_MARK_ADDR);
    diag_puts("cpu-pc: IMEM mark from previous life = "); diag_puthex(s_prev_mark);
    diag_puts("  (");
    switch (s_prev_mark) {
    case 0x12u: diag_puts("reached the SMC, never returned -> TZ did not deliver"
                         " the wake to our entry point"); break;
    case 0x20u: case 0x21u: case 0x22u: case 0x23u:
                diag_puts("WOKE, then faulted inside our restore path"); break;
    case 0x24u: diag_puts("restore completed — died later, in C"); break;
    case 0x40u: diag_puts("a CPU_ON secondary landed"); break;
    case 0x00u: case 0xFFFFFFFFu:
                diag_puts("cleared — full power-on reset, or IMEM not retained");
                break;
    default:    diag_puts("no cpu-pc attempt, or an unrelated value"); break;
    }
    diag_puts(")\n");
    diag_flush();
}

/* ---------------------------------------------------------------------------
 * CPU_ON PLUMBING TEST — see psci_secondary_entry in cpu_suspend.S for why.
 * Read-only as far as our core is concerned: worst case PSCI returns an error.
 * -------------------------------------------------------------------------- */
extern void psci_secondary_entry(void);
uint32_t g_psci_cpu_on_landed;

void cpu_pc_cpu_on_test(void)
{
    if (!g_psci_cpu_on_ok) {
        diag_puts("cpu-on: FEATURES says CPU_ON is absent — no PSCI path can"
                 " start a core at an address we choose\n");
        diag_flush();
        return;
    }

    /* TARGET IS AN MPIDR, NOT AN INDEX. The DTB names the cores cpu@100..103,
     * so Aff1=1 and Aff0=0..3: cpu1 is 0x101. The first version of this test
     * passed 1, which is a CPU that does not exist — TZ answered -6
     * INTERNAL_FAILURE rather than NOT_PRESENT, and that read as a plumbing
     * failure when it was only a bad argument. Print our own MPIDR alongside
     * it so the assumption is visible in the log instead of implied. */
    uint32_t mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    uint32_t target = (mpidr & 0x00FFFF00u) | 0x01u;   /* same cluster, core 1 */

    mmio_write(CPU_PC_MARK_ADDR, 0xA5A5A5A5u);   /* sentinel: see above */
    __asm__ volatile("dsb sy" ::: "memory");

    diag_puts("cpu-on: self_mpidr="); diag_puthex(mpidr);
    diag_puts(" target=");            diag_puthex(target);
    diag_puts(" entry=");             diag_puthex((uint32_t)(uintptr_t)psci_secondary_entry);
    diag_puts("\n");
    diag_flush();

    int32_t rc = psci_cpu_on(target, (uint32_t)(uintptr_t)psci_secondary_entry, 0u);

    /* HOW WE KNOW IT LANDED — and why the obvious way does not work.
     *
     * cpu1 lands with the MMU and caches OFF, so its store to
     * g_psci_cpu_on_landed goes straight to DRAM while our copy of that line
     * sits in CPU0's cache. The first version of this cleaned-and-invalidated
     * (DCCIMVAC, c7 c14 1) before reading — and the CLEAN half wrote our stale
     * zero back OVER cpu1's value, so a successful landing read as a failure.
     * Invalidate ONLY (DCIMVAC, c7 c6 1): discard our copy, take DRAM's.
     *
     * The IMEM mark is the ground truth regardless, because it is device
     * memory and never cached by anyone. Stamp a sentinel first so a stale
     * 0x40 left in IMEM by an EARLIER boot cannot be mistaken for this one. */
    timer_delay_ms(50u);
    void *p = (void *)&g_psci_cpu_on_landed;
    __asm__ volatile("mcr p15, 0, %0, c7, c6, 1\n\tdsb\n\tisb"
                     :: "r"(p) : "memory");
    uint32_t mark = mmio_read(CPU_PC_MARK_ADDR);
    if (mark == 0x40u) g_psci_cpu_on_landed = 1u;

    diag_puts("cpu-on: rc=");        diag_putdec((uint32_t)rc);
    diag_puts(" landed=");           diag_putdec(g_psci_cpu_on_landed);
    diag_puts(" affinity=");         diag_putdec((uint32_t)psci_affinity_info(target, 0u));
    diag_puts(" mark=");             diag_puthex(mark);
    diag_puts(rc == 0 && g_psci_cpu_on_landed
             ? "  -> TZ CAN deliver control to our entry point; the cpu-pc"
               " failure is in the collapse/wake, not in entry delivery\n"
             : "  -> entry delivery did NOT happen; no PSCI warm-boot path is"
               " open to this firmware\n");
    diag_flush();
}

/* One-shot report, printed after the first attempt so the outcome is legible
 * whichever way it went. */
void cpu_pc_report(void)
{
    diag_puts("cpu-pc: attempts=");   diag_putdec(g_cpu_pc_attempts);
    diag_puts(" resumes=");           diag_putdec(g_cpu_pc_resumes);
    diag_puts(" declined=");          diag_putdec(g_cpu_pc_declined);
    diag_puts(" last_rc=");           diag_putdec((uint32_t)g_cpu_pc_last_rc);
    diag_puts(" last_ms=");           diag_putdec(g_cpu_pc_last_ms);
    diag_puts(" apss_shutdowns=");    diag_putdec(rpm_apss_shutdowns());
    diag_puts(" mpm_irq=");           diag_putdec(g_mpm_irq_n);
    diag_puts(" mpm_sts=");           diag_puthex(g_mpm_last_sts1);
    diag_puts(":");                   diag_puthex(g_mpm_last_sts0);
    diag_puts("\n");
    diag_flush();
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
