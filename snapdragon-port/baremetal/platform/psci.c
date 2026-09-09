/* psci.c — PSCI over SMC (rung 3 of the deep-sleep ladder).
 *
 * The device's own DTB says the whole low-power ladder is PSCI-driven:
 *   psci { compatible = "arm,psci-1.0"; method = "smc"; }   (line 10987)
 *   qcom,lpm-levels { qcom,use-psci; ... }                  (line 1250)
 * and the CPUs use enable-method = "psci". So every state below plain WFI is
 * reached by asking the secure world, not by poking hardware ourselves.
 *
 * WHY THIS FILE IS VALIDATION-FIRST: we have never executed a single SMC in
 * this port, and the secure world on this unit has already answered one
 * mistake with an instant reset (the RTC counter write, pmic_rtc.c). So the
 * first image does the two calls that CANNOT have side effects —
 * PSCI_VERSION and PSCI_FEATURES — and prints them. If SMC itself is
 * unavailable or trapped, that shows up as a dead log after a known marker
 * rather than as a mystery reset later, and everything downstream (retention
 * states, MPM, system-pc, and SMP if it is ever wanted) is unblocked or
 * blocked by one legible line.
 *
 * THE RESET/RETENTION SPLIT (the thing that decides how hard each rung is).
 * In the DTB only three levels carry qcom,is-reset: cpu "pc", "perf-l2-pc"
 * and "system-pc". Those are POWER-DOWN states: the core loses context and
 * the secure world re-enters us at an entry point with MMU and caches off, so
 * adopting them needs real warm-boot code (restore translation tables, stack,
 * GIC CPU interface, timer). Every other level — cpu-wfi, perf-l2-wfi,
 * perf-l2-gdhs, system-wfi, system-ret — is standby/retention and RETURNS TO
 * THE CALLER exactly like a WFI. Those are nearly free to adopt, which is why
 * this rung targets them and leaves warm-boot for the system-pc work.
 *
 * power_state encoding — MEASURED, NOT ASSUMED. The first draft of this file
 * documented the PSCI "original" format (StateID[15:0], StateType[16],
 * PowerLevel[25:24]). Hardware said otherwise: PSCI_FEATURES(CPU_SUSPEND)
 * returned 3 on this unit, and that return is a bitmap —
 *     bit0 = 1 -> EXTENDED StateID format
 *     bit1 = 1 -> OS-initiated mode also supported (we stay platform-
 *                 coordinated, which is the simpler and default mode)
 * so the layout is the extended one:
 *     bits [27:0]  StateID   — composed from the DTB's qcom,psci-mode values,
 *                              cpu at shift 0, perf cluster at 4, system at 8
 *     bit  [30]    StateType — 0 = standby/retention, 1 = power-down
 * Getting this wrong would have meant passing a well-formed value that means
 * something entirely different, which is exactly the class of bug that costs a
 * flash cycle to find. Ask the firmware which format it wants.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#define PSCI_FN_VERSION      0x84000000u
#define PSCI_FN_CPU_SUSPEND  0x84000001u
#define PSCI_FN_CPU_ON       0x84000003u
#define PSCI_FN_AFFINITY_INFO 0x84000004u
#define PSCI_FN_FEATURES     0x8400000Au
#define PSCI_FN_SYSTEM_SUSPEND 0x8400000Eu

/* AArch32 SMC32 calling convention: function id in r0, args r1-r3, result r0.
 * .arch_extension sec is required for the assembler to accept `smc` on a
 * plain -mcpu=cortex-a7 build. */
static uint32_t psci_smc(uint32_t fn, uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t r0 __asm__("r0") = fn;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    __asm__ volatile(".arch_extension sec\n\tsmc #0"
                     : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                     :
                     : "memory");
    return r0;
}

uint32_t psci_version(void)
{
    return psci_smc(PSCI_FN_VERSION, 0, 0, 0);
}

/* 0 (or a positive bitmap) = implemented; PSCI_RET_NOT_SUPPORTED (-1) = not. */
int32_t psci_features(uint32_t fn)
{
    return (int32_t)psci_smc(PSCI_FN_FEATURES, fn, 0, 0);
}

int32_t psci_cpu_suspend(uint32_t power_state, uint32_t entry, uint32_t ctx)
{
    return (int32_t)psci_smc(PSCI_FN_CPU_SUSPEND, power_state, entry, ctx);
}

int32_t psci_cpu_on(uint32_t target, uint32_t entry, uint32_t ctx)
{
    return (int32_t)psci_smc(PSCI_FN_CPU_ON, target, entry, ctx);
}

int32_t psci_affinity_info(uint32_t target, uint32_t level)
{
    return (int32_t)psci_smc(PSCI_FN_AFFINITY_INFO, target, level, 0);
}

/* PSCI_SYSTEM_SUSPEND — the spec's own "suspend to RAM" in a single call.
 *
 * WHY THIS IS WORTH TRYING AFTER cpu-pc FAILED. CPU_SUSPEND makes the caller
 * responsible for composing a platform-specific StateID and for whatever the
 * platform needs done around it. SYSTEM_SUSPEND takes no state at all: the
 * caller only says "put the whole system in its deepest state, and re-enter me
 * HERE". Everything else — cluster, L2, rails, DDR self-refresh, and crucially
 * the warm-boot bookkeeping — is the firmware's problem, not ours. If TZ
 * implements it, it is doing the part we cannot see and have been guessing at.
 * Same warm-boot contract as CPU_SUSPEND, so cpu_pc_resume serves as the entry
 * point unchanged. */
int32_t psci_system_suspend(uint32_t entry, uint32_t ctx)
{
    return (int32_t)psci_smc(PSCI_FN_SYSTEM_SUSPEND, entry, ctx, 0);
}

uint32_t g_psci_sys_suspend_ok;   /* FEATURES said SYSTEM_SUSPEND exists */
uint32_t g_psci_cpu_on_ok;        /* FEATURES said CPU_ON exists        */

/* Set once the probe has run: 1 = CPU_SUSPEND is usable, 0 = do not call it.
 * suspend_msm.c consults this instead of calling blind. */
uint32_t g_psci_suspend_ok;

/* The power_state suspend_msm.c passes. Default is perf-l2-wfi.
 *
 * WHY THAT AND NOT DEEPER — the composition rules in the DTB are a wall.
 * Every cluster level except perf-l2-wfi carries qcom,min-child-idx >= 1, and
 * cpu-level index 1 is "pc", which is qcom,is-reset. So system-wfi,
 * system-ret, perf-l2-gdhs, perf-l2-pc and system-pc ALL require the CPU
 * itself to power down first. perf-l2-wfi has no min-child-idx, so plain
 * cpu-wfi satisfies it — making it the only level below WFI reachable without
 * warm-boot code.
 *   StateID = cpu wfi (mode 0) << 0 | perf-l2-wfi (mode 1) << 4 = 0x010
 *   StateType = standby (bit30 clear) -> returns to the caller in place. */
#define PSCI_EXT_POWERDOWN   (1u << 30)
#define PSCI_STATE_PERF_L2_WFI  0x010u
uint32_t g_psci_state = PSCI_STATE_PERF_L2_WFI;

/* Failure accounting: a CPU_SUSPEND that returns an error immediately would
 * turn the suspend loop into a full-speed spin — the exact opposite of the
 * intent, and silent without this. suspend_msm.c prints these per sleep. */
volatile uint32_t g_psci_fail_n;
volatile int32_t  g_psci_last_err;

void psci_report(void)
{
    diag_puts("psci: calling PSCI_VERSION (first SMC this port has ever run)\n");
    diag_flush();          /* flush FIRST: if SMC is fatal, this line survives */

    uint32_t v = psci_version();
    diag_puts("psci: version raw="); diag_puthex(v);
    diag_puts(" -> ");               diag_putdec((v >> 16) & 0xFFFFu);
    diag_puts(".");                  diag_putdec(v & 0xFFFFu);
    diag_puts("\n");

    int32_t f = psci_features(PSCI_FN_CPU_SUSPEND);
    diag_puts("psci: FEATURES(CPU_SUSPEND)="); diag_putdec((uint32_t)f);
    if (f == -1) {
        diag_puts("  NOT SUPPORTED -> staying on plain WFI\n");
    } else {
        g_psci_suspend_ok = 1u;
        diag_puts(" fmt=");
        diag_puts((f & 1) ? "EXTENDED" : "original");
        diag_puts(((f & 2) ? " os-initiated" : " platform-coordinated"));
        diag_puts(" -> state=");
        diag_puthex(g_psci_state);
        diag_puts(" (perf-l2-wfi, standby)\n");
        /* The composed StateID above is only valid in the extended format. If
         * this platform ever reports the original format, fall back to the
         * shallowest state rather than sending a value that would mean
         * something else entirely. */
        if (!(f & 1)) {
            g_psci_state = 0u;
            diag_puts("psci: original format -> falling back to StateID 0\n");
        }
    }
    /* Which of the OTHER doors into a powered-down core does this firmware
     * open? Both matter after three failed cpu-pc attempts:
     *   SYSTEM_SUSPEND — the spec's suspend-to-RAM, where the firmware owns the
     *     part we cannot see. If present, it is the next thing to try.
     *   CPU_ON — proves whether TZ can start a non-secure core at an entry
     *     point WE choose. That is the same plumbing a warm boot needs, but it
     *     can be tested without ever powering our own core down. If CPU_ON
     *     lands, entry-point delivery works and the fault is in the collapse;
     *     if CPU_ON cannot even do it, no PSCI wake path is open to us. */
    int32_t fs = psci_features(PSCI_FN_SYSTEM_SUSPEND);
    diag_puts("psci: FEATURES(SYSTEM_SUSPEND)="); diag_putdec((uint32_t)fs);
    diag_puts(fs == -1 ? "  NOT SUPPORTED\n" : "  PRESENT\n");
    if (fs != -1) g_psci_sys_suspend_ok = 1u;

    int32_t fo = psci_features(PSCI_FN_CPU_ON);
    diag_puts("psci: FEATURES(CPU_ON)="); diag_putdec((uint32_t)fo);
    diag_puts(fo == -1 ? "  NOT SUPPORTED\n" : "  PRESENT\n");
    if (fo != -1) g_psci_cpu_on_ok = 1u;

    /* A sane version is 0x00010000 (1.0) or 0x00000002 (0.2). Anything else —
     * 0, 0xffffffff, garbage — means the SMC did not reach a PSCI
     * implementation, and CPU_SUSPEND must NOT be attempted. */
    if (v == 0u || v == 0xFFFFFFFFu) {
        g_psci_suspend_ok = 0u;
        diag_puts("psci: implausible version -> SMC did not reach PSCI;"
                 " CPU_SUSPEND disabled\n");
    }
    diag_flush();
}

#endif /* PLAT_SOC_MSM */
