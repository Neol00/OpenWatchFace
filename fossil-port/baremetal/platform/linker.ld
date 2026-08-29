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
        /* The ARM zImage prologue MUST be first: aboot parses the magic at
         * +0x24 and enters the image at +0x00. The vector table can no longer
         * live here (it would occupy the NOP sled), so it moved to its own
         * 32-byte-aligned block and VBAR is pointed at it. See startup.S. */
        KEEP(*(.text.head))
        KEEP(*(.text.vectors))
        *(.text.startup)
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    /* C++ static constructors. THE 3-SECOND BUG (2026-08-03): this section
     * existed but NOTHING ever ran it, so every global C++ object (USBSerial!)
     * kept a NULL vtable pointer — the .ino banner println was a call through
     * NULL, a wild access into the XPU-protected 0x0 region: instant TZ reset
     * on recovery boots, prefetch-abort (misread as "deep setup hang") on
     * fastboot boots. main() now walks these before starting the scheduler. */
    .init_array : {
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(.init_array*))
        KEEP(*(SORT(.ctors*)))
        __init_array_end = .;
    }
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
