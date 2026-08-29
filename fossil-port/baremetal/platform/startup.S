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

    /* ======================= IMAGE HEAD =====================================
     * Byte-for-byte the layout of a REAL ARM zImage, because that is what this
     * watch's aboot loads and hands off to. Diffed against the AsteroidOS hoki
     * boot.img that provably runs on this device (2026-08-02):
     *
     *   0x00..0x1c  8x  mov r0,r0   <- NOP sled; entry is at 0x00 and falls
     *                                  through it into the branch
     *   0x20        b   reset       <- (real zImage: ea000003 -> 0x34)
     *   0x24        0x016f2818      <- magic; aboot parses this to FIND THE
     *                                  APPENDED DTB (a raw payload gets the
     *                                  "dtb not found" error we saw)
     *   0x28        zImage_start = 0
     *   0x2c        zImage_end       <- where mk-bootimg appends the DTB
     *   0x30        0x04030201      <- endianness flag. OUR OLD IMAGE HAD A
     *                                  LITERAL-POOL WORD HERE. A loader that
     *                                  validates it would reject the image —
     *                                  and "aboot never transferred control"
     *                                  is exactly the symptom we are chasing.
     *   0x34        code
     *
     * The old layout put the EXCEPTION VECTOR TABLE at 0x00, which collided
     * with the NOP sled and the flag word. The vectors now live in their own
     * aligned block below and VBAR is programmed to point at them, which is
     * how a normal ARM image does it anyway. */
    .section .text.head, "ax"
    .global _start
_start:
    mov     r0, r0                  /* 0x00 */
    mov     r0, r0
    mov     r0, r0
    mov     r0, r0
    mov     r0, r0
    mov     r0, r0
    mov     r0, r0
    mov     r0, r0                  /* 0x1c */
    b       reset                   /* 0x20 */
    .word   0x016f2818              /* 0x24 zImage magic */
    .word   0                       /* 0x28 zImage_start (relative) */
    .word   (__image_copy_end - _start) /* 0x2c zImage_end -> DTB append point */
    .word   0x04030201              /* 0x30 endianness flag (little-endian) */

    .section .text.vectors, "ax"
    .align  5                       /* VBAR requires 32-byte alignment */
    .global _vectors
_vectors:
    b       reset                   /* 0x00 reset */
    b       hang_undef              /* 0x04 undefined instruction */
    ldr     pc, =FreeRTOS_SWI_Handler   /* 0x08 svc: FreeRTOS yield */
    b       hang_pabt               /* 0x0c prefetch abort */
    b       hang_dabt               /* 0x10 data abort */
    b       .                       /* 0x14 reserved */
    ldr     pc, =FreeRTOS_IRQ_Handler   /* 0x18 IRQ: FreeRTOS/GIC dispatch */
    b       hang_fiq                /* 0x1c FIQ: unexpected - report it too */

    .section .text.startup, "ax"
reset:
    cpsid   if                  /* mask IRQ+FIQ */

#if defined(WDOG_TEST)
    /* --- WDOG_TEST: the minimal "did our code execute at all?" probe -------
     *
     * THE PROBLEM THIS SOLVES: every image so far shows aboot's logo for ~15 s
     * and then warm-resets into Wear OS, with no vibration — which is exactly
     * what the APPS watchdog does when NOTHING pets it. That is indistinguish-
     * able from "our code never ran". Every other channel is gone: this kernel
     * is built CONFIG_DEVMEM=n / CONFIG_PSTORE=n (so IMEM and DDR cannot be
     * read back from Linux), the motor needs SPMI (a suspect), the display is
     * what we are debugging, and reboot_to_bootloader() is itself unproven —
     * so even "does it return to fastboot" conflates two failures.
     *
     * The watchdog is the way out, because aboot leaves it RUNNING and we know
     * its registers. Reprogramming its timeout changes WHEN the watch reboots,
     * and that is observable with a stopwatch and nothing else:
     *
     *     reboots after ~3 s   -> OUR CODE RAN (it reprogrammed the dog)
     *     reboots after ~15 s  -> our code never executed a single instruction
     *
     * Dependencies: four MMIO stores to 0x0b017000, a region aboot itself uses.
     * No MMU, no relocation, no stack, no C, no SPMI, no display, no reboot
     * path. This runs before literally everything else, so a "3 s" result also
     * means aboot's handoff, the load address and the image format are all
     * fine. 3 * 32765 = 98295 ticks (WDT_HZ from the vendor driver). */
    ldr     r4, =0x0B017000
    mov     r5, #0
    str     r5, [r4, #8]        /* WDT0_EN = 0 (pause while reprogramming) */
    ldr     r5, =98295          /* 3 s at 32765 Hz */
    str     r5, [r4, #16]       /* WDT0_BARK_TIME */
    str     r5, [r4, #20]       /* WDT0_BITE_TIME */
    mov     r5, #1
    str     r5, [r4, #4]        /* WDT0_RST = 1 (restart the countdown) */
    str     r5, [r4, #8]        /* WDT0_EN = 1 */
    dsb
1:  b       1b                  /* spin: let the dog bite and prove the point */
#endif

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
    ldr     r0, =_vectors               /* NOT _start: the image head is now a
                                           zImage prologue, vectors moved out */
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

/* Faults: land in distinct labels; each writes its marker into boot_fault and
 * then REPORTS ITSELF by reprogramming the APPS watchdog to a distinct short
 * timeout before parking (2026-08-03: a silent `b .` made a CPU fault
 * indistinguishable from a hang — the wdog just bit at whatever was last
 * armed). Pure asm, no stack: abort/undef modes have no SP set up here.
 * Stopwatch decode (fault at ~3 s + value): FIQ ~6 s, DABT ~9 s, UNDEF ~12 s,
 * PABT ~20 s — all clear of the stage-table readings. */
.macro fault_stub name, code, secs
\name:
    ldr     r0, =boot_fault
    mov     r1, #\code
    str     r1, [r0]
    ldr     r0, =0x0B017000         /* APPS wdog: report fault class by time */
    mov     r1, #0
    str     r1, [r0, #0x08]         /* EN = 0 while reprogramming */
    mov     r2, #0x60000            /* ~200 us spin: the wdog runs on the     */
90: subs    r2, r2, #1              /* 32 kHz sleep clock and each write needs */
    bne     90b                     /* several slow-clock cycles to latch      */
    ldr     r1, =(\secs * 32765)
    str     r1, [r0, #0x10]         /* BARK */
    str     r1, [r0, #0x14]         /* BITE */
    mov     r2, #0x60000
91: subs    r2, r2, #1
    bne     91b
    mov     r1, #1
    str     r1, [r0, #0x08]         /* EN = 1 (before RST: vendor order) */
    mov     r2, #0x60000
92: subs    r2, r2, #1
    bne     92b
    str     r1, [r0, #0x04]         /* RST = pet -> countdown from now */
    dsb     sy
    b       .
.endm
    fault_stub hang_undef, 1, 9
    fault_stub hang_pabt,  3, 17
    fault_stub hang_dabt,  4, 6
    fault_stub hang_fiq,   5, 5

    .section .data
    .global boot_r2
boot_r2:    .word 0
    .global boot_fault
boot_fault: .word 0
