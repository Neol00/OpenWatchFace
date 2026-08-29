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

/* Must exceed every INTID we register. The PMIC/SPMI arbiter interrupt is
 * GIC SPI 190 -> INTID 32 + 190 = 222 (same SPI/PPI offset convention as the
 * CNTV note below), so the old 128 would have silently dropped it: dispatch is
 * gated on `id < IRQ_TABLE_SIZE`, and an unregistered ID takes the
 * "unowned source" path and gets DISABLED at the distributor. */
#define IRQ_TABLE_SIZE  256
/* Virtual-timer interrupt ID. ARM's canonical number is 27 (what QEMU virt
 * uses), but Qualcomm wires the arch-timer PPIs differently: the REAL hoki
 * device tree says <GIC_PPI 4> = INTID 20 for CNTV, confirmed live by the
 * stock kernel's /proc/interrupts (arch_timer on hwirq 20). Enabling 27 here
 * meant the tick never arrived — and the pending-but-unhandled INTID 20 from
 * CNTV expiry went to the secure side: instant TZ reset ~1 ms after the
 * scheduler enabled the timer (the 2026-08-03 "3.5 s into Wear OS" deaths). */
#if defined(PLAT_SOC_MSM)
#define VTIMER_PPI      20
#else
#define VTIMER_PPI      27
#endif
#define SPURIOUS_ID     1023u

typedef void (*irq_fn)(void *arg);
static struct { irq_fn fn; void *arg; } s_handlers[IRQ_TABLE_SIZE];

static uint32_t s_tick_period;
static uint64_t s_tick_deadline;

/* ===== THE 100%-DUTY BUG (2026-08-07) ======================================
 * The watch ran hot (tbat 43-45 C at "idle") because this port never executed
 * a single WFI: timer_delay_ms() spun on the counter, fb_kick() spun polling
 * MMIO for ~13 ms per frame, and the FreeRTOS idle task had no idle hook — a
 * Cortex-A53 at 1.036 GHz burning 100% duty around the clock, where the
 * ESP32/Tuya ports of this same firmware idle at 0-1%.
 *
 * Every wait site now drops into WFI once this flag says the 1 kHz tick is
 * live (a WFI with no interrupt source ever firing would sleep forever, so
 * pre-scheduler code paths keep spinning). Wake latency is bounded by the
 * tick: <= 1 ms, ~0.5 ms average per wait. */
volatile uint32_t g_tick_armed;

/* FreeRTOS idle hook (configUSE_IDLE_HOOK): runs when every task is blocked —
 * which is most of every second on a watch. Sleep until the next interrupt. */
void vApplicationIdleHook(void)
{
    if (g_tick_armed) __asm__ volatile("wfi");
}

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
#if defined(WDOG_TRACE)
    wdog_stage(43);                     /* port asserts passed; tick config   */
#endif
    s_tick_period = timer_freq_hz() / configTICK_RATE_HZ;
#if defined(PLAT_SOC_MSM)
    /* Priority 0xC0, NOT the usual 0xF8 (measured on hardware, 2026-08-03):
     * aboot hands over the GICC with an un-EOI'd ACTIVE interrupt whose
     * running priority gates presentation of everything numerically >= it.
     * The gate cannot be cleared from our side (APR0-3 clear and an EOIR
     * priority-drop drain both failed on hardware — it is secure-side
     * state), but it measured > 0xC0, i.e. >= 0xD0 at this GIC's 16-level
     * granularity. 0xC0 is the exact needle-thread: urgent enough to present
     * under the stuck gate, and equal to the FreeRTOS API ceiling
     * (configMAX_API_CALL_INTERRUPT_PRIORITY 12 << 4) so critical sections
     * still mask it (GICv2 masks priority == PMR). Any future IRQ we enable
     * (touch etc.) must use 0xC0 for the same reason. */
    gic_enable_irq(VTIMER_PPI, 0xC0);
#else
    gic_enable_irq(VTIMER_PPI, 0xF8);   /* least-urgent priority for the tick */
#endif
    s_tick_deadline = timer_ticks() + s_tick_period;
    cntv_cval_write(s_tick_deadline);
    cntv_ctl_write(1);                  /* enable, IRQ unmasked */
    g_tick_armed = 1u;                  /* WFI is now safe: worst case the
                                         * tick wakes the core in <= 1 ms */
#if defined(WDOG_TRACE)
    wdog_stage(44);                     /* tick armed; next: first ctx switch */
#endif
#if defined(DELAY_BISECT2)
    /* Bisect point 2 (2026-08-03): timer ARMED, CPU interrupts still masked.
     * Reset during this pause -> the hardware objects to the ARMING itself
     * (GICD PPI writes / CNTV_CTL). Reset ~5 s later -> the killer is IRQ
     * delivery / the first context switches. */
    con_puts("DELAY_BISECT2: 5 s pause, tick armed, IRQs still masked\n");
    timer_delay_ms(5000);
#endif
}

/* ===== TICKLESS SUSPEND WINDOW (rung 1, 2026-08-07) ========================
 * A 1 kHz tick means the core wakes 1000x/s. That is fine when awake, but in
 * suspend it is the whole power budget AND it structurally blocks every deeper
 * idle state: the shallowest system level this SoC offers (system-wfi) needs
 * 1.25 ms of residency and system-pc needs 5.3 ms, so a 1 ms tick can never
 * meet the entry criteria. Parking the tick is therefore a prerequisite for
 * rungs 3-5, not just an optimisation.
 *
 * tick_park() re-points the single CNTV comparator at a far deadline instead
 * of the next 1 ms boundary; tick_unpark() restores 1 kHz and reports how many
 * ticks the scheduler never saw, so the caller can hand them to
 * xTaskCatchUpTicks() and keep FreeRTOS time honest.
 *
 * Accounting: the parked deadline still fires the normal tick handler once
 * (that is what wakes us), and that firing DOES increment the scheduler. So
 * the missed count is elapsed-periods minus the firings actually serviced —
 * counted here rather than assumed, because a park can also end early on an
 * unrelated interrupt with zero tick firings. */
static volatile uint32_t s_tick_parked;
static uint64_t s_park_mark;         /* timer_ticks() when the park began */
static uint32_t s_park_serviced;     /* tick IRQs serviced while parked */

void tick_park(uint64_t deadline)
{
    uint64_t now = timer_ticks();
    /* Never park closer than one tick period: a deadline already in the past
     * arms a comparator that fires immediately (or, worse, has already fired
     * and leaves us waiting a full 64-bit wrap in WFI). */
    if (deadline < now + s_tick_period) deadline = now + s_tick_period;
    s_park_mark     = now;
    s_park_serviced = 0u;
    s_tick_parked   = 1u;
    s_tick_deadline = deadline;
    cntv_cval_write(deadline);
}

uint32_t tick_unpark(void)
{
    uint64_t now = timer_ticks();
    uint32_t elapsed, missed;

    s_tick_parked = 0u;
    elapsed = (uint32_t)((now - s_park_mark) / s_tick_period);
    missed  = (elapsed > s_park_serviced) ? (elapsed - s_park_serviced) : 0u;

    /* Re-arm the 1 kHz cadence from now (not from the stale deadline, which is
     * far in the past and would burst-fire catching up). */
    s_tick_deadline = now + s_tick_period;
    cntv_cval_write(s_tick_deadline);
    return missed;
}

/* Stop the tick before a CPU power-down.
 *
 * The comparator dies with the core anyway, but leaving it armed means the
 * 1 kHz tick keeps firing right up to the moment of collapse — including
 * inside the PSCI call sequence. Silence it first so the only interrupt that
 * can be pending when the core goes down is the one meant to wake it. */
void tick_stop(void)
{
    cntv_ctl_write(0);
    __asm__ volatile("isb" ::: "memory");
}

/* Re-arm the tick after a CPU power-down.
 *
 * CNTV_CTL and CNTV_CVAL are per-CPU and die with the core, so a resumed core
 * has no tick at all — FreeRTOS would simply stop scheduling. The COUNTER
 * (CNTVCT) is a different matter: it is driven by the always-on system
 * counter and keeps running through the power-down, which is what lets
 * cpu_pc.c measure how long we were actually out and catch the scheduler up.
 *
 * The GIC PPI enable is banked per-CPU too, so re-enabling the interrupt is
 * part of this, not optional. */
void tick_rearm(void)
{
    s_tick_parked   = 0u;
    s_tick_period   = timer_freq_hz() / configTICK_RATE_HZ;
    s_tick_deadline = timer_ticks() + s_tick_period;
#if defined(PLAT_SOC_MSM)
    gic_enable_irq(VTIMER_PPI, 0xC0);
#else
    gic_enable_irq(VTIMER_PPI, 0xF8);
#endif
    cntv_cval_write(s_tick_deadline);
    cntv_ctl_write(1);
}

void vClearTickInterrupt(void)
{
#if defined(TICK_STOP_AT)
    /* Park deliberately at tick #TICK_STOP_AT with a distinct wdog window:
     * seeing THAT reading proves ticks are being serviced up to N; the
     * mystery reset striking first proves they are not. Binary-search N. */
    static uint32_t s_ticks;
    if (++s_ticks >= (TICK_STOP_AT)) {
        wdog_extend(10);
        for (;;) { }
    }
#endif
    uint64_t now = timer_ticks();

    if (s_tick_parked) {
        /* The parked deadline just fired — this is the wake that ends a
         * suspend chunk. Record that the scheduler DID see this one tick (so
         * tick_unpark() does not double-count it) and re-arm one period out
         * purely to keep the comparator sane; the suspend loop calls
         * tick_unpark() immediately after its WFI returns and fixes the
         * cadence properly. */
        s_park_serviced++;
        s_tick_deadline = now + s_tick_period;
        cntv_cval_write(s_tick_deadline);
        return;
    }

    s_tick_deadline += s_tick_period;
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
    } else if (id < 1020u) {
        /* Unowned source. If it is level-triggered and still asserted it
         * would re-fire the instant we EOI it - an IRQ storm. Disable it at
         * the distributor so it can only ever cost us one event. */
        gic_disable_irq(id);
        con_puts("!! unexpected IRQ "); con_putdec(id); con_puts(" (disabled)\n");
    }
}

/* -- FreeRTOS hooks ---------------------------------------------------------- */
void vAssertCalled(const char *pcFile, unsigned long ulLine)
{
    portDISABLE_INTERRUPTS();
    con_puts("\nASSERT "); con_puts(pcFile);
    con_puts(":"); con_putdec((uint32_t)ulLine); con_puts("\n");
    /* Report the assert's LINE NUMBER through the only working channel: the
     * watchdog. Reading ~= (fire time ~2 s) + 4 + (line % 26); the candidate
     * configASSERT sites all map to distinct values (table in msm_wdog.c
     * notes / memory). Park - do NOT reboot: reboot_to_bootloader() lands in
     * Wear OS anyway (broken cookie) and would destroy the timing signal. */
#if defined(PLAT_SOC_MSM)
    wdog_extend(4u + (uint32_t)(ulLine % 26u));
#endif
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
