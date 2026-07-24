/* irq.c — IRQ dispatch + FreeRTOS tick source (ARM generic timer, CNTV).
 *
 * The FreeRTOS CA9 port's FreeRTOS_IRQ_Handler acks the GIC, calls
 * vApplicationIRQHandler(ulICCIAR) with interrupts (above the API ceiling)
 * enabled, then writes EOIR itself. We dispatch through a small table.
 *
 * Tick: virtual timer (PPI 27, CNTV) — usable identically on QEMU virt and the
 * Wear 3100 (both 19.2 MHz there / 62.5 MHz emulated; read from CNTFRQ).
 */
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"

#define IRQ_TABLE_SIZE  128
#define VTIMER_PPI      27
#define SPURIOUS_ID     1023u

typedef void (*irq_fn)(void *arg);
static struct { irq_fn fn; void *arg; } s_handlers[IRQ_TABLE_SIZE];

static uint32_t s_tick_period;
static uint64_t s_tick_deadline;

void irq_register(unsigned id, void (*fn)(void *), void *arg)
{
    if (id < IRQ_TABLE_SIZE) { s_handlers[id].fn = fn; s_handlers[id].arg = arg; }
}

/* -- CNTV accessors ---------------------------------------------------------- */
static inline void cntv_cval_write(uint64_t v)
{ __asm__ volatile("mcrr p15, 3, %0, %1, c14" :: "r"((uint32_t)v), "r"((uint32_t)(v >> 32))); }
static inline void cntv_ctl_write(uint32_t v)
{ __asm__ volatile("mcr p15, 0, %0, c14, c3, 1" :: "r"(v)); }

/* -- FreeRTOS tick plumbing (named in FreeRTOSConfig.h) ----------------------
 * Drift-free tick: advance an absolute CVAL deadline each tick instead of
 * re-arming TVAL (which silently accumulates the IRQ-service latency). */
void vConfigureTickInterrupt(void)
{
    s_tick_period = timer_freq_hz() / configTICK_RATE_HZ;
    gic_enable_irq(VTIMER_PPI, 0xF8);   /* least-urgent priority for the tick */
    s_tick_deadline = timer_ticks() + s_tick_period;
    cntv_cval_write(s_tick_deadline);
    cntv_ctl_write(1);                  /* enable, IRQ unmasked */
}

void vClearTickInterrupt(void)
{
    s_tick_deadline += s_tick_period;
    uint64_t now = timer_ticks();
    if (s_tick_deadline <= now) {       /* fell behind (long critical section) */
        s_tick_deadline = now + s_tick_period;
    }
    cntv_cval_write(s_tick_deadline);
}

extern void FreeRTOS_Tick_Handler(void);

void vApplicationIRQHandler(uint32_t ulICCIAR)
{
    uint32_t id = ulICCIAR & 0x3FFu;

    if (id == SPURIOUS_ID) return;
    if (id == VTIMER_PPI) { FreeRTOS_Tick_Handler(); return; }

    if (id < IRQ_TABLE_SIZE && s_handlers[id].fn) {
        s_handlers[id].fn(s_handlers[id].arg);
    } else {
        con_puts("!! unexpected IRQ "); con_putdec(id); con_puts("\n");
    }
}

/* -- FreeRTOS hooks ---------------------------------------------------------- */
void vAssertCalled(const char *pcFile, unsigned long ulLine)
{
    portDISABLE_INTERRUPTS();
    con_puts("\nASSERT "); con_puts(pcFile);
    con_puts(":"); con_putdec((uint32_t)ulLine); con_puts("\n");
    for (;;) { }
}

void vApplicationMallocFailedHook(void)
{
    portDISABLE_INTERRUPTS();
    con_puts("\nMALLOC FAILED\n");
    for (;;) { }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    portDISABLE_INTERRUPTS();
    con_puts("\nSTACK OVERFLOW in "); con_puts(pcTaskName); con_puts("\n");
    for (;;) { }
}
