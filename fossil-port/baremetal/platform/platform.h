/* platform.h — selects the board header and declares the tiny platform API
 * every fossil-port target implements. Build sets exactly one PLAT_BOARD_*. */
#pragma once
#include <stdint.h>

#if defined(PLAT_BOARD_QEMU_VIRT)
#include "../boards/qemu_virt.h"
#elif defined(PLAT_BOARD_FOSSIL_GEN4)
#include "../boards/fossil_gen4.h"
#else
#error "platform.h: define PLAT_BOARD_QEMU_VIRT or PLAT_BOARD_FOSSIL_GEN4"
#endif

static inline void mmio_write(uintptr_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }
static inline uint32_t mmio_read(uintptr_t addr) { return *(volatile uint32_t *)addr; }

/* uart_<type>.c */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint32_t v);
void uart_putdec(uint32_t v);

/* ramlog.c — post-mortem ring buffer in .ramlog (survives warm reboot) */
void ramlog_init(void);          /* keeps previous contents if magic is valid */
void ramlog_putc(char c);
int  ramlog_had_previous(void);  /* nonzero if a prior boot's log was found */

/* timer.c — ARM architected timer (polled; IRQ mode arrives with FreeRTOS) */
uint32_t timer_freq_hz(void);
uint64_t timer_ticks(void);
uint32_t timer_ms(void);
void     timer_delay_ms(uint32_t ms);

/* mmu.c — identity map + caches (MUST run before LVGL/newlib: unaligned
 * accesses fault while the MMU is off) */
void mmu_enable_flat(void);

/* gic.c — GICv2 */
void gic_init(void);
void gic_enable_irq(unsigned id, uint8_t priority);

/* irq.c — dispatch table (tick PPI 27 is claimed internally) */
void irq_register(unsigned id, void (*fn)(void *), void *arg);

/* fb_*.c — framebuffer (ramfb on QEMU; MDP3/DSI on the watch later).
 * Returns an XRGB8888 buffer of w*h pixels, or NULL. */
void *fb_init(uint32_t w, uint32_t h);

/* console: fan out to UART + ramlog */
void con_putc(char c);
void con_puts(const char *s);
void con_puthex(uint32_t v);
void con_putdec(uint32_t v);

extern uint32_t boot_r2;      /* r2 as received from the loader (DTB/ATAGS?) */
extern uint32_t boot_fault;   /* nonzero = a fault stub parked the CPU */
