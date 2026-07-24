/* linker.ld — fossil-port bare-metal image layout.
 * LINK_BASE is injected by the Makefile (--defsym) per platform.
 * The image is a flat binary: LMA == VMA, startup.S self-relocates if the
 * loader drops it elsewhere. */
ENTRY(_start)

SECTIONS
{
    . = LINK_BASE;
    __image_start = .;

    .text : {
        KEEP(*(.text.vectors))      /* vector table MUST be first (VBAR target) */
        *(.text.startup)
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(4);
    __image_copy_end = .;           /* self-relocation copies up to here */

    .bss (NOLOAD) : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /* Post-mortem ring log: survives a warm reboot (DDR retains), NOT cleared
     * with .bss. ramlog.c validates the magic instead of trusting contents. */
    .ramlog (NOLOAD) : ALIGN(4096) {
        __ramlog_start = .;
        . += 64K;
        __ramlog_end = .;
    }

    . = ALIGN(4096);
    . += 16K;                       /* SVC stack */
    __stack_top = .;
    . += 4K;                        /* IRQ stack */
    __irq_stack_top = .;

    __image_end = .;

    /* newlib _sbrk heap grows upward from here (toward PLAT_DDR_SAFE_END). */
    PROVIDE(end = .);
    PROVIDE(_end = .);
}
