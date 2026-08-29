/* cpu_suspend.S — CPU power-down (warm boot) for the Fossil Gen 6.
 *
 * THE PROBLEM. Every low-power state on this SoC below perf-l2-wfi requires
 * the CPU core itself to power down: system-wfi, system-ret, perf-l2-gdhs,
 * perf-l2-pc and system-pc all carry qcom,min-child-idx >= 1, and cpu-level
 * index 1 is "pc", which is qcom,is-reset (sda429-hoki-decompiled.dts:1349).
 * A core that powers down forgets everything — registers, MMU tables, caches,
 * the lot — and the secure world re-enters us at a bare entry point with the
 * MMU OFF and caches OFF. Warm boot is the code that puts it all back.
 *
 * WHAT MAKES THIS TRACTABLE HERE. Linux's cpu_resume has to be written
 * position-independently and translate virtual to physical by hand, because
 * the kernel runs at a virtual offset. We do not: mmu.c installs a FLAT
 * identity map (mmu_enable_flat), so every link address IS its physical
 * address. cpu_pc_resume can therefore be handed straight to PSCI as the
 * entry point, and can dereference ordinary symbols the moment it starts.
 *
 * ORDER OF OPERATIONS ON RESUME — the part that is easy to get fatally wrong:
 *   1. INVALIDATE the L1 D-cache by set/way. On power-up its contents are
 *      UNDEFINED, not empty. Enabling SCTLR.C with garbage lines resident
 *      means the first read of any variable can return rubbish from a
 *      previous life. Invalidate BEFORE enabling, and invalidate rather than
 *      clean — there is nothing worth writing back.
 *   2. Restore the translation registers (TTBR0/TTBCR/DACR + PRRR/NMRR) while
 *      still running unmapped.
 *   3. Only then write SCTLR to turn the MMU and caches on. We are executing
 *      identity-mapped, so the PC stays valid across the switch.
 *   4. Rebuild SP (SVC and IRQ), FPEXC, and pop the callee-saved registers so
 *      cpu_pc_suspend returns normally to its C caller.
 * No stack exists until step 4, so steps 1-3 are register-only.
 *
 * BEFORE SUSPENDING we must CLEAN the D-cache: the saved context and the
 * pushed stack frame live in write-back memory, and the resume path reads
 * them with caches off. Anything still sitting dirty in L1 when the core
 * loses power is simply gone. L1 clean-to-L2 is enough — L2 is in the cluster
 * power domain and survives cpu-pc (it is perf-l2-pc that would kill it).
 *
 * BREADCRUMBS. If the resume path faults there is no log — the UART/USB stack
 * is not up and DDR contents are suspect. So each step stamps a word into
 * IMEM at 0x08600840 (on-die SRAM, survives a warm reset, readable from
 * fastboot/next boot). Whatever value is there names the last step reached.
 */
    .syntax unified
    .arm

#define IMEM_MARK   0x08600840

/* Context slot offsets — must match the layout comment in cpu_pc.c. */
#define CTX_SCTLR       0
#define CTX_CPACR       4
#define CTX_TTBR0       8
#define CTX_TTBR1       12
#define CTX_TTBCR       16
#define CTX_DACR        20
#define CTX_VBAR        24
#define CTX_CONTEXTIDR  28
#define CTX_TPIDRURO    32
#define CTX_TPIDRPRW    36
#define CTX_PRRR        40
#define CTX_NMRR        44
#define CTX_SP_SVC      48
#define CTX_CPSR        52
#define CTX_SP_IRQ      56
#define CTX_FPEXC       60
#define CTX_SIZE        64

    .section .text, "ax"

/* ---- mark(value): stamp IMEM. Clobbers r11, r12 only. -------------------- */
.macro MARK val
    ldr     r12, =IMEM_MARK
    mov     r11, #\val
    str     r11, [r12]
    dsb
.endm

/* ==========================================================================
 * int cpu_pc_suspend(uint32_t power_state)
 *
 * Returns 0 if we actually powered down and came back through cpu_pc_resume,
 * or the PSCI return code if the firmware declined (in which case nothing was
 * lost and the caller simply carries on).
 * ========================================================================== */
    .global cpu_pc_suspend
    .type   cpu_pc_suspend, %function
cpu_pc_suspend:
    push    {r4-r11, lr}
    /* Hold power_state in r9, NOT r4: v7_clean_dcache_l1 below clobbers r0-r8,
     * so r4 would be destroyed between here and the SMC and we would hand PSCI
     * a garbage state. r9 and the MARK scratch (r11/r12) are untouched by it. */
    mov     r9, r0

    MARK    0x10
    ldr     r1, =cpu_pc_ctx
    bl      cpu_pc_save_ctx

    MARK    0x11

    /* ---- clean L1 D-cache so the context + stack reach DRAM --------------- */
    bl      v7_clean_dcache_l1

    MARK    0x12

    /* ---- PSCI_CPU_SUSPEND(power_state, entry, context_id) ----------------- */
    ldr     r0, =0x84000001
    mov     r1, r9                      /* power_state (see r9 note above)    */
    ldr     r2, =cpu_pc_resume          /* identity-mapped == physical addr   */
    mov     r3, #0                      /* context_id (unused)                */
    dsb
    isb
    .arch_extension sec
    smc     #0

    /* Reached only if PSCI returned WITHOUT powering down. r0 = its code. */
    MARK    0x13
    pop     {r4-r11, pc}
    .size   cpu_pc_suspend, . - cpu_pc_suspend

/* ==========================================================================
 * int cpu_pc_selftest(void)
 *
 * WHY THIS EXISTS (2026-08-07). The first cpu-pc image made the watch power
 * off and stay off until a button press produced a COLD boot. That proves the
 * power-down half works — the core really did switch off — but it cannot
 * distinguish the two ways the other half can fail:
 *     (a) the core was never woken (interrupt cannot reach a collapsed core), or
 *     (b) the core woke and our resume path faulted.
 * IMEM breadcrumbs cannot tell us either, because a full power-off clears IMEM.
 *
 * So: exercise the ENTIRE restore path with the core still powered. Save the
 * context exactly as the real path does, then instead of the SMC, turn the MMU
 * and caches off by hand — which is precisely the state PSCI re-enters us in —
 * and branch to cpu_pc_resume. If our restore code is correct this returns
 * normally with the log intact. If it is broken, it breaks HERE, where the
 * failure is observable, instead of inside an unrecoverable power-down.
 *
 * Turning the MMU off mid-execution is safe for us specifically because the
 * map is a flat identity map: the PC remains valid across the switch.
 * ========================================================================== */
    .global cpu_pc_selftest
    .type   cpu_pc_selftest, %function
cpu_pc_selftest:
    push    {r4-r11, lr}
    MARK    0x30
    ldr     r1, =cpu_pc_ctx
    bl      cpu_pc_save_ctx
    bl      v7_clean_dcache_l1
    MARK    0x31

    /* Drop to the PSCI re-entry state: MMU off, D-cache off, I-cache off. */
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #(1 << 0)           /* M: MMU      */
    bic     r0, r0, #(1 << 2)           /* C: D-cache  */
    bic     r0, r0, #(1 << 12)          /* I: I-cache  */
    dsb
    mcr     p15, 0, r0, c1, c0, 0
    isb
    MARK    0x32
    b       cpu_pc_resume               /* returns to OUR caller via the pop */
    .size   cpu_pc_selftest, . - cpu_pc_selftest

/* ==========================================================================
 * int cpu_sys_suspend(uint32_t ctx)
 *
 * PSCI_SYSTEM_SUSPEND(entry_point, context_id). Same save/clean/resume shape
 * as cpu_pc_suspend — only the function id and the absence of a power_state
 * differ — so it shares cpu_pc_resume as its entry point and returns 0 through
 * the same pop if the system really did suspend.
 * ========================================================================== */
    .global cpu_sys_suspend
    .type   cpu_sys_suspend, %function
cpu_sys_suspend:
    push    {r4-r11, lr}
    mov     r9, r0                      /* context_id (r0-r8 die below) */

    MARK    0x18
    ldr     r1, =cpu_pc_ctx
    bl      cpu_pc_save_ctx
    bl      v7_clean_dcache_l1
    MARK    0x19

    ldr     r0, =0x8400000E             /* PSCI_SYSTEM_SUSPEND */
    ldr     r1, =cpu_pc_resume          /* identity-mapped == physical */
    mov     r2, r9
    mov     r3, #0
    dsb
    isb
    .arch_extension sec
    smc     #0

    MARK    0x1A                        /* returned without suspending */
    pop     {r4-r11, pc}
    .size   cpu_sys_suspend, . - cpu_sys_suspend

/* ==========================================================================
 * psci_secondary_entry — landing pad for PSCI_CPU_ON.
 *
 * WHY THIS EXISTS (2026-08-17). Three cpu-pc attempts have ended the same way:
 * the core powers down and the wake produces a COLD boot instead of a return
 * to cpu_pc_resume. Two very different faults look identical from outside —
 * either TZ never delivers control to a non-secure entry point we chose, or it
 * does and something in the collapse resets us anyway. CPU_ON tests exactly
 * the first half, on a core we do not need, WITHOUT powering our own core down:
 * if CPU1 lands here, entry-point delivery demonstrably works on this firmware.
 *
 * Entered with the MMU off, caches off, no stack — so this does the absolute
 * minimum: stamp IMEM, bump a counter with a PC-relative load, and park. The
 * core is never taken down again; it just sits in WFI costing a core's leakage,
 * which is a fine price for the answer.
 * ========================================================================== */
    .global psci_secondary_entry
    .type   psci_secondary_entry, %function
psci_secondary_entry:
    cpsid   if
    MARK    0x40
    ldr     r0, =g_psci_cpu_on_landed
    mov     r1, #1
    str     r1, [r0]
    dsb
1:  wfi
    b       1b
    .size   psci_secondary_entry, . - psci_secondary_entry

/* ==========================================================================
 * cpu_pc_save_ctx — r1 = context base. Clobbers r2, r3. Uses no stack, so the
 * SP it records is the caller's, which is what the resume pop depends on.
 * ========================================================================== */
    .type   cpu_pc_save_ctx, %function
cpu_pc_save_ctx:
    mrc     p15, 0, r2, c1, c0, 0
    str     r2, [r1, #CTX_SCTLR]
    mrc     p15, 0, r2, c1, c0, 2
    str     r2, [r1, #CTX_CPACR]
    mrc     p15, 0, r2, c2, c0, 0
    str     r2, [r1, #CTX_TTBR0]
    mrc     p15, 0, r2, c2, c0, 1
    str     r2, [r1, #CTX_TTBR1]
    mrc     p15, 0, r2, c2, c0, 2
    str     r2, [r1, #CTX_TTBCR]
    mrc     p15, 0, r2, c3, c0, 0
    str     r2, [r1, #CTX_DACR]
    mrc     p15, 0, r2, c12, c0, 0
    str     r2, [r1, #CTX_VBAR]
    mrc     p15, 0, r2, c13, c0, 1
    str     r2, [r1, #CTX_CONTEXTIDR]
    mrc     p15, 0, r2, c13, c0, 3
    str     r2, [r1, #CTX_TPIDRURO]
    mrc     p15, 0, r2, c13, c0, 4
    str     r2, [r1, #CTX_TPIDRPRW]
    mrc     p15, 0, r2, c10, c2, 0
    str     r2, [r1, #CTX_PRRR]
    mrc     p15, 0, r2, c10, c2, 1
    str     r2, [r1, #CTX_NMRR]

    str     sp, [r1, #CTX_SP_SVC]
    mrs     r2, cpsr
    str     r2, [r1, #CTX_CPSR]

    /* IRQ-mode SP is banked: switch, read, switch back. */
    mrs     r3, cpsr
    cps     #0x12
    mov     r2, sp
    msr     cpsr_c, r3
    str     r2, [r1, #CTX_SP_IRQ]

    vmrs    r2, fpexc
    str     r2, [r1, #CTX_FPEXC]
    bx      lr
    .size   cpu_pc_save_ctx, . - cpu_pc_save_ctx

/* ==========================================================================
 * cpu_pc_resume — PSCI entry point after a real power-down.
 * MMU off, caches off, IRQs masked, SVC mode, NO STACK.
 * ========================================================================== */
    .global cpu_pc_resume
    .type   cpu_pc_resume, %function
cpu_pc_resume:
    cpsid   if                          /* be certain, do not assume */
    MARK    0x20

    /* Step 1: invalidate L1 D-cache. Contents after power-up are UNDEFINED;
     * enabling the cache over them would serve garbage for real variables. */
    mov     r0, #0
    mcr     p15, 2, r0, c0, c0, 0       /* CSSELR = level 0, data            */
    isb
    mrc     p15, 1, r0, c0, c0, 0       /* CCSIDR                            */
    and     r1, r0, #7
    add     r1, r1, #4                  /* r1 = log2(line bytes)             */
    ubfx    r2, r0, #3, #10             /* r2 = associativity - 1  (ways-1)  */
    ubfx    r3, r0, #13, #15            /* r3 = numsets - 1                  */
    clz     r4, r2                      /* r4 = way field shift              */
    mov     r5, r2                      /* r5 = way counter                  */
1:  mov     r6, r3                      /* r6 = set counter                  */
2:  lsl     r7, r5, r4
    lsl     r8, r6, r1
    orr     r7, r7, r8
    mcr     p15, 0, r7, c7, c6, 2       /* DCISW: invalidate set/way         */
    subs    r6, r6, #1
    bge     2b
    subs    r5, r5, #1
    bge     1b
    dsb

    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0       /* ICIALLU  */
    mcr     p15, 0, r0, c7, c5, 6       /* BPIALL   */
    mcr     p15, 0, r0, c8, c7, 0       /* TLBIALL  */
    dsb
    isb

    MARK    0x21

    /* Step 2: translation + system registers, still unmapped. */
    ldr     r1, =cpu_pc_ctx
    ldr     r2, [r1, #CTX_TTBR0]
    mcr     p15, 0, r2, c2, c0, 0
    ldr     r2, [r1, #CTX_TTBR1]
    mcr     p15, 0, r2, c2, c0, 1
    ldr     r2, [r1, #CTX_TTBCR]
    mcr     p15, 0, r2, c2, c0, 2
    ldr     r2, [r1, #CTX_DACR]
    mcr     p15, 0, r2, c3, c0, 0
    ldr     r2, [r1, #CTX_PRRR]
    mcr     p15, 0, r2, c10, c2, 0
    ldr     r2, [r1, #CTX_NMRR]
    mcr     p15, 0, r2, c10, c2, 1
    ldr     r2, [r1, #CTX_VBAR]
    mcr     p15, 0, r2, c12, c0, 0
    ldr     r2, [r1, #CTX_CONTEXTIDR]
    mcr     p15, 0, r2, c13, c0, 1
    ldr     r2, [r1, #CTX_TPIDRURO]
    mcr     p15, 0, r2, c13, c0, 3
    ldr     r2, [r1, #CTX_TPIDRPRW]
    mcr     p15, 0, r2, c13, c0, 4
    ldr     r2, [r1, #CTX_CPACR]        /* before any VFP touch */
    mcr     p15, 0, r2, c1, c0, 2
    isb

    MARK    0x22

    /* Step 3: MMU + caches on. Identity-mapped, so the PC survives. */
    ldr     r2, [r1, #CTX_SCTLR]
    dsb
    mcr     p15, 0, r2, c1, c0, 0
    isb

    MARK    0x23

    /* Step 4: stacks, FPU, and return as if cpu_pc_suspend returned 0. */
    ldr     r1, =cpu_pc_ctx             /* reload: mapped now, same address */
    ldr     r2, [r1, #CTX_SP_IRQ]
    mrs     r3, cpsr
    cps     #0x12
    mov     sp, r2
    msr     cpsr_c, r3
    ldr     sp, [r1, #CTX_SP_SVC]

    ldr     r2, [r1, #CTX_FPEXC]
    vmsr    fpexc, r2

    ldr     r2, =g_cpu_pc_resumes       /* prove we came back this way */
    ldr     r3, [r2]
    add     r3, r3, #1
    str     r3, [r2]
    dsb

    MARK    0x24

    mov     r0, #0                      /* cpu_pc_suspend() -> 0 */
    pop     {r4-r11, pc}
    .size   cpu_pc_resume, . - cpu_pc_resume

/* ==========================================================================
 * v7_clean_dcache_l1 — clean (not invalidate) L1 D-cache by set/way.
 * Geometry read from CCSIDR, never assumed: this port already learned that
 * lesson once when mmu.c hardcoded Cortex-A7 L1 dimensions on what is really
 * an A53. Clobbers r0-r8. Needs a stack (called from cpu_pc_suspend only).
 * ========================================================================== */
    .type   v7_clean_dcache_l1, %function
v7_clean_dcache_l1:
    mov     r0, #0
    mcr     p15, 2, r0, c0, c0, 0       /* CSSELR = level 0, data */
    isb
    mrc     p15, 1, r0, c0, c0, 0       /* CCSIDR */
    and     r1, r0, #7
    add     r1, r1, #4                  /* log2(line bytes) */
    ubfx    r2, r0, #3, #10             /* ways - 1 */
    ubfx    r3, r0, #13, #15            /* sets - 1 */
    clz     r4, r2                      /* way shift */
    mov     r5, r2
1:  mov     r6, r3
2:  lsl     r7, r5, r4
    lsl     r8, r6, r1
    orr     r7, r7, r8
    mcr     p15, 0, r7, c7, c10, 2      /* DCCSW: clean by set/way */
    subs    r6, r6, #1
    bge     2b
    subs    r5, r5, #1
    bge     1b
    dsb
    bx      lr
    .size   v7_clean_dcache_l1, . - v7_clean_dcache_l1

/* Context save area. .data (not .bss) so it is never in a zeroed-at-boot
 * region the resume path might race, and 64-byte aligned so the cache clean
 * covers it in whole lines. */
    .section .data
    .align  6
    .global cpu_pc_ctx
cpu_pc_ctx:
    .space  CTX_SIZE, 0
