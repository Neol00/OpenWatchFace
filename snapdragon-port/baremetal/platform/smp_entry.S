/* smp_entry.S — cold-boot entry for a secondary Cortex-A7 core (msm8909w).
 *
 * TZ jumps here in NS SVC mode with the MMU and caches off and no stack
 * after smp_8909.c has registered this address (SCM BOOT_ADDR, cold-boot
 * flag for the core) and unclamped the core through its ACC block. Mirrors
 * Linux's secondary_startup + v7_invalidate_l1 + the A7 proc setup: SMP bit
 * in ACTLR before the caches, the SAME page table CPU0 built (identity map,
 * Normal memory is marked Shareable), then a per-core stack and C. */
    .syntax unified
    .arm
    .text

    .global smp_secondary_entry
    .type   smp_secondary_entry, %function
smp_secondary_entry:
    cpsid   if
    mrs     r4, cpsr
    bic     r4, r4, #0x1f
    orr     r4, r4, #0xd3               /* SVC, I+F masked */
    msr     cpsr_c, r4

    /* vectors (never taken while I/F are masked, but VBAR must be sane) */
    ldr     r0, =_vectors
    mcr     p15, 0, r0, c12, c0, 0

    /* VFP/NEON access like CPU0 */
    mrc     p15, 0, r0, c1, c0, 2
    orr     r0, r0, #(0xf << 20)
    mcr     p15, 0, r0, c1, c0, 2
    isb
    mov     r0, #(1 << 30)
    vmsr    fpexc, r0

    /* v7_invalidate_l1: L1 D-cache may hold junk after power-up */
    mov     r0, #0
    mcr     p15, 2, r0, c0, c0, 0       /* CSSELR = L1 data */
    isb
    mrc     p15, 1, r0, c0, c0, 0       /* CCSIDR */
    movw    r1, #0x7fff
    and     r2, r1, r0, lsr #13         /* sets - 1 */
    movw    r1, #0x3ff
    and     r3, r1, r0, lsr #3          /* ways - 1 */
    add     r2, r2, #1
    and     r0, r0, #0x7
    add     r0, r0, #4                  /* set shift */
    clz     r1, r3                      /* way shift */
    add     r4, r3, #1
1:  sub     r2, r2, #1
    mov     r3, r4
2:  subs    r3, r3, #1
    mov     r5, r3, lsl r1
    mov     r6, r2, lsl r0
    orr     r5, r5, r6
    mcr     p15, 0, r5, c7, c6, 2       /* DCISW */
    bgt     2b
    cmp     r2, #0
    bgt     1b
    dsb     st
    mcr     p15, 0, r2, c7, c5, 0       /* ICIALLU */
    mcr     p15, 0, r2, c7, c5, 6       /* BPIALL */
    mcr     p15, 0, r2, c8, c7, 0       /* TLBIALL */
    dsb
    isb

    /* ACTLR.SMP before the caches (A7 requirement; NS write allowed when
     * TZ set NSACR.NS_SMP, which Linux relies on too) */
    mrc     p15, 0, r0, c1, c0, 1
    orr     r0, r0, #(1 << 6)
    mcr     p15, 0, r0, c1, c0, 1
    isb

    /* MMU: CPU0's table, same TTBR0 attributes */
    mov     r0, #1
    mcr     p15, 0, r0, c3, c0, 0       /* DACR dom0 client */
    mov     r0, #0
    mcr     p15, 0, r0, c2, c0, 2       /* TTBCR */
    ldr     r0, =g_mmu_ttbr0
    ldr     r0, [r0]
    mcr     p15, 0, r0, c2, c0, 0       /* TTBR0 */
    isb
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #(1 << 0)           /* M */
    orr     r0, r0, #(1 << 2)           /* C */
    orr     r0, r0, #(1 << 11)          /* Z */
    orr     r0, r0, #(1 << 12)          /* I */
    bic     r0, r0, #(1 << 1)           /* A */
    dsb
    mcr     p15, 0, r0, c1, c0, 0
    isb

    /* v180: which core? CPU1 = the service core; CPU2/CPU3 park through TZ
     * (smp_park_main never returns). Own small stacks for the parked cores. */
    mrc     p15, 0, r0, c0, c0, 5       /* MPIDR */
    and     r0, r0, #0xff
    cmp     r0, #1
    bhi     4f
    ldr     sp, =smp_cpu1_stack_top
    bl      smp_secondary_main
3:  wfi
    b       3b
4:  ldr     r1, =smp_park_stacks        /* 2 x 1 KB: cpu2 at +1K top, cpu3 at +2K top */
    sub     r2, r0, #1                  /* 1 for cpu2, 2 for cpu3 */
    add     sp, r1, r2, lsl #10
    bl      smp_park_main
5:  wfi
    b       5b
    .size   smp_secondary_entry, . - smp_secondary_entry

/* pc_warm_entry -- the ONE warm-boot address registered with TZ through the
 * multi-cluster boot API (like the kernel's msm_pm_boot_entry): dispatch on
 * MPIDR.Aff0. MMU off, ARM state, nothing set up yet. */
    .global pc_warm_entry
    .type   pc_warm_entry, %function
pc_warm_entry:
    mrc     p15, 0, r0, c0, c0, 5       /* MPIDR */
    and     r0, r0, #0xff
    cmp     r0, #0
    ldreq   pc, =cpu_pc_resume
    ldr     pc, =smp_secondary_entry
    .ltorg
    .size   pc_warm_entry, . - pc_warm_entry

/* void smp_cpu1_wfi_collapse(void) -- runs ON CPU1, IRQ/FIQ masked. Clean
 * L1, then a plain WFI with the SPM in its standalone-pc mode: the SPM
 * powers the core down on the standby signal, TrustZone is NOT involved, so
 * its running-core count stays what aboot handed over (one). Returns if the
 * SPM only clock-gated; after a real collapse the core restarts at the reset
 * vector (TZ -> our warm/cold entry, or our ACC cold boot). */
    .global smp_cpu1_wfi_collapse
    .type   smp_cpu1_wfi_collapse, %function
smp_cpu1_wfi_collapse:
    push    {r4-r11, lr}
    bl      v7_clean_dcache_l1
    dsb
    wfi
    pop     {r4-r11, pc}
    .size   smp_cpu1_wfi_collapse, . - smp_cpu1_wfi_collapse

/* int smp_cpu1_collapse(void) -- runs ON CPU1, IRQ/FIQ masked. Clean this
 * core's L1 (v7_clean_dcache_l1 clobbers r0-r8, hence the wrapper), then the
 * legacy TERMINATE_PC SMC (L2 flag 0). Returns only if TZ declined; a real
 * collapse comes back through smp_secondary_entry (warm-boot address). */
    .global smp_cpu1_collapse
    .type   smp_cpu1_collapse, %function
smp_cpu1_collapse:
    push    {r4-r11, lr}
    bl      v7_clean_dcache_l1
    /* v199: the kernel's msm_pm_pc_hotplug() flag for a core going OFFLINE:
     * SCM_CMD_CORE_HOTPLUGGED (0x10) | L2 flag 0. Tells TZ this core is not
     * coming back on the cluster wake (we cold-boot it ourselves). */
    mov     r0, #0x10
    bl      scm_terminate_pc
    pop     {r4-r11, pc}
    .size   smp_cpu1_collapse, . - smp_cpu1_collapse
