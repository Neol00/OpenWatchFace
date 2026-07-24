/* startup.S — ARMv7-A bare-metal entry for the fossil-port runtime.
 *
 * Contract with the loader (aboot on the watch, QEMU -kernel on the host):
 * we may be dropped at ANY address in DDR, in SVC or another PL1 mode, MMU and
 * caches off, r0-r2 possibly carrying Linux boot goo (ignored for now — r2 may
 * point at DTB/ATAGS; preserved into boot_r2 for later use).
 *
 * What it does, in order:
 *   1. mask IRQ/FIQ, force SVC mode
 *   2. self-relocate the whole image to PLAT_LINK_BASE if loaded elsewhere
 *   3. set VBAR to our vector table
 *   4. enable VFP/NEON (the firmware and LVGL are built hard-float)
 *   5. set stacks (SVC + IRQ), clear .bss
 *   6. call main() — never returns
 *
 * Secondary cores: aboot only releases core 0 on msm8909; cores 1-3 stay off
 * until we ever want them (we don't, per the port plan).
 * MMU/caches: left OFF in the skeleton. The FreeRTOS step adds the identity
 * map + I/D cache enable (worth ~10x on memcpy-heavy LVGL flushes).
 */
    .syntax unified
    .arm

    .section .text.vectors, "ax"
    .global _start
_start:
    b       reset                   /* 0x00 reset */
    b       hang_undef              /* 0x04 undefined instruction */
    ldr     pc, =FreeRTOS_SWI_Handler   /* 0x08 svc: FreeRTOS yield */
    b       hang_pabt               /* 0x0c prefetch abort */
    b       hang_dabt               /* 0x10 data abort */
    b       .                       /* 0x14 reserved */
    ldr     pc, =FreeRTOS_IRQ_Handler   /* 0x18 IRQ: FreeRTOS/GIC dispatch */
    b       .                       /* 0x1c FIQ */

    .section .text.startup, "ax"
reset:
    cpsid   if                  /* mask IRQ+FIQ */
    /* force SVC mode (aboot enters SVC; be defensive anyway) */
    mrs     r4, cpsr
    bic     r4, r4, #0x1f
    orr     r4, r4, #0x13
    orr     r4, r4, #0xc0       /* keep I+F masked across the write */
    msr     cpsr_c, r4

    mov     r10, r2             /* stash possible DTB/ATAGS pointer */

    /* ---- self-relocate to PLAT_LINK_BASE ----------------------------------
     * adr can only reference the current section, so anchor on `reset` and
     * subtract its link-time offset to recover the actual image base. */
    ldr     r0, =__image_start  /* where we WANT to run (link addr) */
    adr     r1, reset           /* run addr of reset (PC-relative) */
    ldr     r2, =reset          /* link addr of reset */
    sub     r2, r2, r0          /* reset's offset inside the image */
    sub     r1, r1, r2          /* actual image base we're running from */
    cmp     r0, r1
    beq     at_home
    ldr     r2, =__image_copy_end
    sub     r2, r2, r0          /* byte count: text+rodata+data */
1:  ldmia   r1!, {r3-r6}
    stmia   r0!, {r3-r6}
    subs    r2, r2, #16
    bgt     1b
    ldr     r0, =at_home        /* absolute jump into the relocated copy */
    bx      r0

at_home:
    /* ---- vectors ----------------------------------------------------------- */
    ldr     r0, =_start
    mcr     p15, 0, r0, c12, c0, 0      /* VBAR */

    /* ---- VFP/NEON on ------------------------------------------------------- */
    mrc     p15, 0, r0, c1, c0, 2       /* CPACR */
    orr     r0, r0, #(0xf << 20)        /* cp10+cp11 full access */
    mcr     p15, 0, r0, c1, c0, 2
    isb
    mov     r0, #(1 << 30)              /* FPEXC.EN */
    vmsr    fpexc, r0

    /* ---- stacks ------------------------------------------------------------ */
    cps     #0x12                       /* IRQ mode */
    ldr     sp, =__irq_stack_top
    cps     #0x13                       /* back to SVC */
    ldr     sp, =__stack_top

    /* ---- clear .bss -------------------------------------------------------- */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
2:  cmp     r0, r1
    strlt   r2, [r0], #4
    blt     2b

    ldr     r0, =boot_r2                /* export the stashed r2 */
    str     r10, [r0]

    bl      main
    b       .

/* Faults: land in distinct labels so a debugger / ramlog dump can tell them
 * apart; each writes its marker into boot_fault (readable post-mortem) then
 * parks. No UART use here — keep fault paths dependency-free. */
.macro fault_stub name, code
\name:
    ldr     r0, =boot_fault
    mov     r1, #\code
    str     r1, [r0]
    b       .
.endm
    fault_stub hang_undef, 1
    fault_stub hang_pabt,  3
    fault_stub hang_dabt,  4

    .section .data
    .global boot_r2
boot_r2:    .word 0
    .global boot_fault
boot_fault: .word 0
