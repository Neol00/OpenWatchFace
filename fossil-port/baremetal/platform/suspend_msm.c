/* suspend_msm.c — Gen 6 blocking suspend ("deep sleep" for the OWF app).
 *
 * THE SLEEP-RETRY FREEZE (2026-08-07): OWF's idle policy called
 * esp_deep_sleep_start() after 2 min idle, and the compat stub paused 900 ms
 * and RETURNED. The caller's contract (see sleep_power.h's T5 suspend loop)
 * is "block until a WAKE SOURCE"; returning with the watch still idle just
 * re-triggers the policy, so the firmware locked into an endless
 * enter-sleep/return cycle — cpu=96%, UI dead, log spam.
 *
 * This is the Tuya T5 semantic done natively: quiesce (panel off), BLOCK
 * here until a wake source fires, resume in place (never reboot).
 *
 * RUNG 1 (2026-08-07): the loop no longer runs at 1 kHz.
 *
 * It used to WFI once per 1 ms tick, so "suspend" woke the core 1000 times a
 * second to re-poll a GPIO. That is nearly all of the idle power budget, and
 * it also makes every deeper idle state unreachable by construction: the
 * shallowest system level this SoC offers needs 1.25 ms of residency and
 * system-pc needs 5.3 ms (sda429-hoki-decompiled.dts:1250). A 1 ms tick can
 * never satisfy either. Parking the tick is a PREREQUISITE for rungs 3-5, not
 * merely an optimisation.
 *
 * Now: tick_park() re-points the CNTV comparator at the end of the current
 * chunk, we WFI once, and tick_unpark() hands the skipped ticks to FreeRTOS so
 * scheduler time stays honest across the sleep.
 *
 * WHY CHUNKS AND NOT ONE LONG PARK — two hard limits, both real:
 *
 *   1. THE WATCHDOG. WDT0_BARK/BITE are 20-bit at 32765 Hz, so the longest
 *      window this SoC can be programmed for is ~32 s (msm_wdog.c). An
 *      un-petted sleep longer than that is a reset, not a sleep. We could
 *      wdog_disable() for the duration — and rung 5 will have to, since a
 *      collapsed CPU cannot pet anything — but doing it now would trade the
 *      hang protection away for a saving we cannot yet measure. Chunking keeps
 *      the dog.
 *
 *   2. (RESOLVED 2026-08-07) Button state used to be POLLED, so the chunk WAS
 *      the wake latency and could not be long. pmic_irq.c now routes the PMIC
 *      interrupt (GIC SPI 190 -> INTID 222) and wakes us the instant a button
 *      goes down, so chunk length is decoupled from responsiveness and is set
 *      purely by watchdog petting. See SUSPEND_CHUNK_MS below.
 *
 * WAKE SOURCES:
 *   - kpdpwr / resin, the physical pushers (pmic_pon.c). THESE WERE MISSING:
 *     until now plat_suspend() polled only the touch INT and the timer, so on
 *     a firmware whose documented wake gesture is a button press, no button
 *     could end a sleep. Edge-triggered, not level — see s_armed below.
 *   - the app's armed timer deadline (esp_sleep_enable_timer_wakeup maps to
 *     plat_suspend_set_timer_us): alarms / background checks resume on time.
 *   - touch INT (active low on PLAT_TOUCH_INT_GPIO). Kept because it costs one
 *     GPIO read inside a poll we are doing anyway; the firmware does not rely
 *     on it.
 *
 * Panel off remains the biggest lever we actually have today (AMOLED with no
 * backlight: display off is real power). Rail cuts, clock parking and DDR
 * self-refresh come at rungs 4-5, gated on the sleep_stats.c counters proving
 * each step rather than on inference.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#include "FreeRTOS.h"
#include "task.h"

/* Chunk length while suspended.
 *
 * 250 ms -> 15 s (2026-08-07), now that the PMIC interrupt does the waking.
 *
 * The old value was set by BUTTON LATENCY: with no interrupt routed, a press
 * was invisible until the next poll, so the chunk WAS the response time. That
 * constraint is gone — GIC 222 wakes us the instant a button goes down
 * (measured: "woke by pmic-irq", pmic_irq=1), so chunk length no longer
 * affects responsiveness at all. Wakeups drop 4/s -> 0.067/s, a 60x cut.
 *
 * What bounds it now is watchdog petting, and there are TWO dogs:
 *   - the hardware watchdog: WDT0_BARK/BITE are 20-bit at 32765 Hz, so ~32 s
 *     is the longest window that can be programmed at all (msm_wdog.c).
 *   - the deadman: a FreeRTOS software timer armed at 30 s (main.c:615) that
 *     reboots to fastboot if un-kicked. It is TICK-driven, and this loop calls
 *     xTaskCatchUpTicks() for the whole parked interval BEFORE it reaches
 *     deadman_kick() — so a chunk near 30 s would expire the deadman during
 *     the catch-up and reboot the watch out of a perfectly healthy sleep.
 * 15 s leaves 2x margin against both. Sleeps are still unbounded in aggregate:
 * the loop simply re-parks until a wake source fires.
 *
 * The poll below is kept as the safety net. At one SPMI read per 15 s its cost
 * is nil, and it means a future interrupt regression degrades to a 15 s wake
 * latency instead of a watch that never wakes. */
#define SUSPEND_CHUNK_MS   15000u

/* Chunk used while a USB host is attached and listening.
 *
 * THE LOST-LOG BUG (2026-08-07): going to 15 s chunks silently killed logging
 * the moment the watch slept. usb_poll() is called ONCE PER CHUNK — it is the
 * only thing that services the CDC bulk endpoint — so at 15 s the host's IN
 * requests go unanswered for 15 s at a time and `cat /dev/ttyACM0` simply
 * stops. Nothing was wrong with the sleep; the log just could not get out.
 *
 * This is a genuine conflict, not something to tune away: long chunks are the
 * whole point when running on battery, and frequent polling is required for a
 * live log. So pick per sleep. A cable attached means development (and, on
 * this watch, charging — which the firmware already treats as never-sleep), so
 * the power cost of 250 ms chunks there is irrelevant. */
#define SUSPEND_CHUNK_USB_MS  250u

/* Longer than a chunk, under the ~32 s hardware ceiling. */
#define SUSPEND_WDOG_SEC   30u

/* pwr_diag's census splits each window into compute / sleep / display-wait, and
 * its "sleep" bucket is fed ONLY by arduino_glue.cpp's delay(). plat_suspend()
 * does not go through delay(), so before this every millisecond spent suspended
 * was booked as COMPUTE — the 2026-08-07 log showed cpu=53% for a window that
 * was 40% asleep. Credit the time here, or every deeper rung will report a
 * power regression it did not cause. */
extern volatile uint32_t g_pwr_sleep_ms;

static uint64_t s_wake_deadline_ms;   /* 0 = no timer wake armed */

void plat_suspend_set_timer_us(unsigned long long us)
{
    s_wake_deadline_ms = (uint64_t)timer_ms() + (us + 999u) / 1000u;
}

/* A button already held when we enter suspend must NOT immediately wake us —
 * that is the press that asked for sleep, or a user resting a finger on the
 * case. Arm each button only once it has been seen released, then wake on the
 * next press. Same shape as an edge trigger, done in software. */
static int button_wake(int *armed_kpd, int *armed_resin)
{
    int kpd   = pon_kpdpwr_pressed();
    int resin = pon_resin_pressed();

    if (kpd == 0)   *armed_kpd   = 1;      /* released -> arm */
    if (resin == 0) *armed_resin = 1;
    /* SPMI error returns -1: treat as "released" (do not arm, do not wake) —
     * a flaky arbiter read must never manufacture a wake event. */
    if (kpd   == 1 && *armed_kpd)   return 1;
    if (resin == 1 && *armed_resin) return 2;
    return 0;
}

void plat_suspend(void)
{
    uint32_t t0 = timer_ms();
    uint64_t deadline = s_wake_deadline_ms;
    s_wake_deadline_ms = 0;              /* consumed — re-armed per sleep */

    uint32_t hz = timer_freq_hz();
    /* Short chunks only while a host is actually listening — see the
     * SUSPEND_CHUNK_USB_MS note. Decided once per sleep, not per chunk, so the
     * cadence cannot change underneath the loop. */
    int usb_live = usb_is_configured();
    uint32_t chunk_ms = usb_live ? SUSPEND_CHUNK_USB_MS : SUSPEND_CHUNK_MS;
    uint32_t chunk_ticks = (hz / 1000u) * chunk_ms;
    uint32_t parks = 0, caught = 0;
    int armed_kpd = 0, armed_resin = 0;
    const char *why = "?";
    uint32_t irq0 = g_pmic_irq_n;
    g_pmic_irq_wake = 0u;            /* consume anything latched pre-sleep */

    /* Quiesce: panel display OFF (DCS 0x28, page 0 first — same page dance
     * as the re-pin path). Self-refresh stops driving the OLED: true black,
     * and the DDIC keeps its RAM + init state so 0x29 restores instantly. */
    dsi_dcs_write(0xFE, 0x00, 1);
    dsi_dcs_write(0x28, 0x00, 0);

    con_puts("suspend: panel off, tickless ");
    con_putdec(chunk_ms);
    con_puts(usb_live ? " ms chunks, USB attached (buttons" : " ms chunks (buttons");
#if defined(SUSPEND_TOUCH_WAKE)
    con_puts("/touch");
#endif
    if (deadline) con_puts("/timer");
    /* Log the touch INT level even though it is no longer a wake source: if it
     * is already low at entry the controller is asserting with nobody touching
     * the glass, which is the ESD misbehaviour that made it a bad wake source. */
    con_puts(" wake)");
    diag_puts(" touch_int=");
    diag_putdec((uint32_t)tlmm_in(PLAT_TOUCH_INT_GPIO));
    con_puts("\n");
    con_flush();

    /* MPM census either side of the sleep. The STATUS bit that differs
     * between these two lines is the MPM pin behind the PMIC's GIC 190 —
     * which is the one number missing before a collapsed core can be woken. */
    mpm_dump("sleep-in");

    wdog_extend(SUSPEND_WDOG_SEC);

    for (;;) {
        /* --- decide how long this chunk may last ------------------------- */
        uint64_t now_t = timer_ticks();
        uint64_t park  = now_t + chunk_ticks;

        if (deadline) {
            /* Do not overshoot the armed wake time: clamp the park to it. */
            uint64_t now_ms = (uint64_t)timer_ms();
            if (now_ms >= deadline) { why = "timer"; break; }
            uint64_t left_ms = deadline - now_ms;
            if (left_ms < (uint64_t)chunk_ms)
                park = now_t + left_ms * (hz / 1000u);
        }

        /* --- CPU POWER COLLAPSE (warm boot) --------------------------------
         * The first state where the core actually switches off. Two
         * conditions, both real limits rather than caution:
         *
         *  - only when NO timer wake is armed. The architected timer dies with
         *    the core (that is what qcom,use-broadcast-timer means), and we
         *    have no broadcast timer yet, so a timed wake cannot be honoured
         *    from cpu-pc. Timed sleeps keep using the chunked path below.
         *  - try once; if PSCI declines, fall back for the rest of this boot
         *    instead of re-asking every chunk.
         *
         * OFF BY DEFAULT, enable with -DUSE_CPU_PC. The first attempt on
         * hardware (2026-08-07) powered the core down successfully and it was
         * never seen again: the watch went dead and a button press produced a
         * COLD boot. The power-down half works; either the wake never reached
         * the collapsed core or our resume path faulted, and until
         * cpu_pc_selftest() says which, leaving this on would just make every
         * sleep a shutdown. */
        int did_pc = 0;
#if defined(USE_SYS_SUSPEND)
        /* PSCI_SYSTEM_SUSPEND instead of a hand-composed CPU_SUSPEND state.
         * Three cpu-pc attempts died the same way; this hands the entire
         * collapse to the firmware, which is the half we cannot see. Same
         * guards as cpu-pc: no timed wake, and stop asking once it declines. */
        if (g_psci_sys_suspend_ok && !deadline &&
            (g_cpu_pc_attempts == 0u || g_cpu_pc_ok)) {
            did_pc = cpu_pc_system_suspend();
            if (did_pc) { parks++; cpu_pc_report(); }
        }
#elif defined(USE_CPU_PC)
        /* SELF-GATING ON THE CPU_ON RESULT. cpu-pc depends on TZ re-entering
         * us at an address we chose; CPU_ON exercises exactly that delivery on
         * a core we do not need. If cpu1 never landed, the same delivery will
         * not work for our own core either, and collapsing would just be the
         * fourth identical shutdown — so do not attempt it. This makes the
         * probe and the attempt fit in one flash cycle without the attempt
         * being a gamble. */
        if (g_psci_cpu_on_landed && g_psci_suspend_ok && !deadline &&
            (g_cpu_pc_attempts == 0u || g_cpu_pc_ok)) {
            did_pc = cpu_pc_sleep();
            if (did_pc) { parks++; cpu_pc_report(); }
        }
#endif
        if (did_pc) goto housekeeping;

        /* --- sleep --------------------------------------------------------
         * One WFI per chunk instead of one per millisecond. g_tick_armed is
         * the same guard every other wait site uses: with no interrupt source
         * live, WFI would never return, so fall back to the old behaviour. */
        if (g_tick_armed) {
            tick_park(park);
            /* PSCI standby (StateID 0, StateType 0) instead of a bare WFI when
             * the probe proved the SMC path reaches a real PSCI
             * implementation. Behaviourally this IS a WFI — the difference is
             * that the secure world is now the one choosing, so it may enter a
             * deeper hardware state on our behalf, and the same call site
             * climbs to retention StateIDs once those are validated. Falls
             * back to WFI whenever the probe was not satisfied. */
            if (g_psci_suspend_ok) {
                int32_t rc = psci_cpu_suspend(g_psci_state, 0u, 0u);
                if (rc != 0) {
                    /* Errored instead of sleeping — without this the loop
                     * would spin at full speed and look like a sleep. Count
                     * it, remember the code, and fall back for this chunk. */
                    g_psci_fail_n++;
                    g_psci_last_err = rc;
                    __asm__ volatile("wfi");
                }
            } else {
                __asm__ volatile("wfi");
            }
            uint32_t missed = tick_unpark();
            if (missed) {
                /* Hand the skipped ticks to the scheduler so task delays and
                 * timeouts do not silently lose the whole sleep duration. */
                xTaskCatchUpTicks((TickType_t)missed);
                caught += missed;
            }
            parks++;
        } else {
            /* No tick source: WFI would never return. Pace the loop anyway —
             * the wake checks below do SPMI reads, and spinning them flat out
             * would be worse than the 1 kHz poll this rung set out to remove.
             * (timer_delay_ms yield-spins when the tick is dead.) */
            timer_delay_ms(chunk_ms);
        }

        /* --- housekeeping, once per chunk rather than once per ms --------- */
housekeeping:
        wdog_pet();
        deadman_kick();
        usb_poll();
        con_flush();

        /* --- wake checks ---------------------------------------------------
         * Interrupt first: if the PMIC IRQ chain is live, the handler has
         * already latched the event and we do not need to touch the bus. The
         * poll below remains the safety net for as long as the chain is
         * unproven. */
        if (g_pmic_irq_wake) { why = "pmic-irq"; break; }
        int b = button_wake(&armed_kpd, &armed_resin);
        if (b) { why = (b == 1) ? "kpdpwr" : "resin"; break; }
#if defined(SUSPEND_TOUCH_WAKE)
        /* OFF BY DEFAULT (2026-08-07) — this was waking the watch by itself.
         *
         * Nothing drains the Raydium controller while suspended, and this
         * controller has known ESD trouble on this unit (boot logs show
         * "ABNORMAL fw_state 0x55" and both ESD rescues firing). A controller
         * that resets or queues a phantom report pulls INT low, and the
         * suspend loop read that as a user tap: measured wake after 2252 ms
         * with pmic_irq=0, i.e. the INT asserted mid-sleep with nobody
         * touching the glass. Intermittent, because it depends on the
         * controller misbehaving.
         *
         * This firmware wakes on buttons and timers by design and never asked
         * for tap-to-wake; it was kept only because the GPIO read was cheap.
         * Cheap is not the same as free when it costs a wake. */
        if (tlmm_in(PLAT_TOUCH_INT_GPIO) == 0) { why = "touch"; break; }
#endif
        if (deadline && (uint64_t)timer_ms() >= deadline) { why = "timer"; break; }
    }

    /* Resume: panel back on. Every subsequent fb_kick re-pins the window, so
     * no further display state restoration is needed. */
    dsi_dcs_write(0xFE, 0x00, 1);
    dsi_dcs_write(0x29, 0x00, 0);

    uint32_t slept = timer_ms() - t0;
    g_pwr_sleep_ms += slept;         /* book it as sleep, not compute */

    con_puts("suspend: woke by "); con_puts(why);
    con_puts(" after ");           con_putdec(slept);
    con_puts(" ms; parks=");       con_putdec(parks);
    diag_puts(" ticks_caught=");   diag_putdec(caught);
    /* Wakeups per second while suspended: the number rung 1 exists to move.
     * Was 1000 by construction; expect ~1000/SUSPEND_CHUNK_MS now. */
    /* Per MINUTE now, not per second: at a 15 s chunk the per-second figure
     * rounds to 0 for every realistic sleep and stops being informative.
     * Was 60000/min (the 1 kHz tick), then 240/min, now ~4/min. */
    if (slept >= 1000u) {
        con_puts(" wakeups/min=");
        con_putdec((uint32_t)((uint64_t)parks * 60000u / slept));
    }
    /* The rest is instrumentation from the deep-sleep work: it answered its
     * questions (the PMIC wake source works, psci_fail stays 0) and now just
     * crowds the line. Kept behind -DSLEEP_DIAG rather than deleted, since it
     * is exactly what would be wanted again if wake behaviour ever regresses.
     * EXCEPTION: a non-zero psci_fail or a stuck IRQ means something is
     * genuinely wrong, so those still print unconditionally. */
    diag_puts(" pmic_irq=");   diag_putdec(g_pmic_irq_n - irq0);
    diag_puts(" kpd=");        diag_putdec(g_pmic_irq_kpdpwr);
    diag_puts(" resin=");      diag_putdec(g_pmic_irq_resin);
    diag_puts(" psci_state="); diag_puthex(g_psci_state);
    diag_puts(" psci_fail=");  diag_putdec(g_psci_fail_n);
    if (g_pmic_irq_spurious) { con_puts(" spur="); con_putdec(g_pmic_irq_spurious); }
    if (g_pmic_irq_stuck)    { con_puts(" stuck="); con_putdec(g_pmic_irq_stuck); }
    if (g_psci_fail_n) {
        con_puts(" psci_fail="); con_putdec(g_psci_fail_n);
        con_puts(" err=");       con_putdec((uint32_t)g_psci_last_err);
    }
    con_puts("\n");
    mpm_dump("wake");
    sleep_stats_line();
    con_flush();
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
