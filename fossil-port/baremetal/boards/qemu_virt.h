/* qemu_virt.h — QEMU -M virt -cpu cortex-a7: the development stand-in.
 * Same ARMv7-A + GICv2 + architected-timer shape as the Wear 3100's A7 cluster,
 * so the runtime (startup, MMU, GIC, FreeRTOS port) is proven here first and
 * only the peripheral drivers remain device-specific. */
#pragma once

#define PLAT_NAME           "qemu-virt"

#define PLAT_DDR_BASE       0x40000000u
#define PLAT_DDR_SIZE       (512u * 1024u * 1024u)
#define PLAT_LINK_BASE      0x40008000u
#define PLAT_DDR_SAFE_END   (PLAT_DDR_BASE + PLAT_DDR_SIZE)

/* GICv2 on -M virt */
#define PLAT_GICD_BASE      0x08000000u
#define PLAT_GICC_BASE      0x08010000u

/* QEMU virt arch-timer frequency (read CNTFRQ at runtime anyway) */
#define PLAT_TIMER_HZ       62500000u

/* PL011 UART on -M virt (maps to -nographic stdio) */
#define PLAT_UART_TYPE_PL011 1
#define PLAT_UART_BASE      0x09000000u
