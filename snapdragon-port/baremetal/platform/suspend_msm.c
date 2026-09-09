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
#if defined(PLAT_BOARD_FOSSIL_GEN6) || defined(PLAT_BOARD_FOSSIL_GEN4) || defined(PLAT_BOARD_TICWATCH_C2)

/* 8909w PORT (2026-09-03): the Gen 4 and the C2 run this same loop. What
 * differs is gated on SUSPEND_GEN6 below:
 *   - PSCI: the msm8909w has no PSCI (its DTB has no arm,psci node; power
 *     collapse is the legacy TZ "terminate pc" path, qcom,pc-mode =
 *     "tz_l2_int"), so the 8909w boards sleep with a bare WFI. The tick is
 *     still parked, so it is one wake per chunk, not one per millisecond.
 *   - panel: the Gen 6 panel needs a page select (0xFE 0x00) before the DCS
 *     display on/off; the Gen 4 and C2 panels take plain 0x28 / 0x29.
 *   - MPM census, RPM sleep counters and the cpu-pc experiments are Gen 6
 *     instrumentation and stay there.
 *   - wake sources: buttons only (kpdpwr / resin via the PMIC interrupt, GIC
 *     SPI 190 on both PMICs) plus the app's armed timer. The 8909w watches
 *     have no touch wake by design. */
#if defined(PLAT_BOARD_FOSSIL_GEN6)
#  define SUSPEND_GEN6 1
#else
#  define SUSPEND_GEN6 0
#endif

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
#define SUSPEND_CHUNK_MS   29000u   /* 2026-09-06: one wake per 29 s on battery; the APPS wdog ceiling is 31 s and it is re-armed just before each collapse, so keep a margin under it */

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

/* DARK RESUME (2026-09-03, user request: periodic checks must not light the
 * screen). With s_keep_dark set, a TIMER wake returns with the panel still
 * off so the app can run its background check in the dark and re-suspend;
 * the app calls plat_display_on() when it decides on a full wake. A BUTTON
 * wake always relights here. s_panel_off tracks the DCS state so the relight
 * is idempotent and a sleep entered with the panel already off does not
 * re-send display-off. */
static int s_keep_dark;
static int s_panel_off;
static int s_last_cause;              /* PLAT_WAKE_* */

void plat_suspend_keep_dark(int on) { s_keep_dark = on; }
int  plat_suspend_last_cause(void)  { return s_last_cause; }

static void panel_dcs(uint8_t cmd)
{
#if SUSPEND_GEN6
    dsi_dcs_write(0xFE, 0x00, 1);
    dsi_dcs_write(cmd, 0x00, 0);
#else
    dsi_dcs_write(cmd, 0, 0);
#endif
}
static int s_panel_sleeping;     /* DCS 0x10 sent: DDIC analog + boost off */
void plat_display_on(void)
{
    if (!s_panel_off) return;
    if (s_panel_sleeping) { panel_dcs(0x11); timer_delay_ms(120u); s_panel_sleeping = 0; }
    panel_dcs(0x29);
    s_panel_off = 0;
}

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

#if defined(PLAT_SOC_MSM8909)
extern volatile uint32_t g_smp_beat, g_smp_busy_ticks;   /* smp_8909.c */
#else
static uint32_t g_smp_beat, g_smp_busy_ticks;            /* no second core here */
#endif
extern volatile uint32_t g_pmic_irq_alarm;   /* pmic_irq.c: RTC alarm interrupts */
extern volatile uint32_t g_irq_hist[];        /* irq.c: per-INTID service counts */
#define IRQ_HIST_N 256   /* == IRQ_TABLE_SIZE in irq.c */
static uint32_t s_irq_snap[IRQ_HIST_N];
static void irq_hist_snap(void) { for (unsigned i = 0; i < IRQ_HIST_N; i++) s_irq_snap[i] = g_irq_hist[i]; }
static void irq_hist_report(void)
{
    con_puts("suspend: irqs during sleep:");
    for (unsigned k = 0; k < 6; k++) {              /* top 6 by count */
        unsigned best = 0; uint32_t bd = 0;
        for (unsigned i = 0; i < IRQ_HIST_N; i++) { uint32_t d = g_irq_hist[i] - s_irq_snap[i]; if (d > bd) { bd = d; best = i; } }
        if (!bd) break;
        con_puts(" id"); con_putdec(best); con_puts("x"); con_putdec(bd);
        s_irq_snap[best] = g_irq_hist[best];         /* consume so the next pick is the next largest */
    }
    con_puts("\n");
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
    /* STALE-ENUMERATION BUG (2026-09-06). usb_is_configured() is s_running &&
     * s_configured, and s_configured is cleared ONLY by a USB bus reset
     * (usb_ci.c handle_reset) — unplugging the cable clears nothing. Every
     * test boot enumerates in order to be flashed, so this stayed true for
     * the rest of the boot and every later sleep took the 250 ms host cadence
     * on battery: a measured 2000 parks in 502 s, ~116x the intended wake
     * rate, which is where the heat and the drain came from. Require VBUS to
     * actually be present as well; the PMIC sense that makes this trustworthy
     * landed in v136. */
    int usb_live = usb_is_configured() && (chg_usb_present() == 1);
    uint32_t chunk_ms = usb_live ? SUSPEND_CHUNK_USB_MS : SUSPEND_CHUNK_MS;
    int floor_cable = usb_live; (void)floor_cable;   /* sleep_floor.c: measure the ladder only with a host listening */
#if defined(SLEEP_NO_WDOG)
    /* No watchdog to pet, so no reason to wake at all: on battery the core
     * collapses ONCE and stays down until the PMIC button IRQ (GIC 190,
     * proven) or the armed timer (QTimer clamp below + RTC alarm backup).
     * The USB cadence is kept only while a host is attached, for the log. */
    chunk_ms = 24u * 3600u * 1000u;   /* cable or not: ONE collapse, no cadence */
    usb_live = 0;
#endif
    uint64_t chunk_ticks = (uint64_t)(hz / 1000u) * chunk_ms;
    uint32_t parks = 0, caught = 0;
    uint32_t pc_ms = 0, pc_n = 0, pc_fail = 0;   /* collapse residency, see the report below */
    /* CPU1 is brought up for frame pushes and NOTHING in this path takes it
     * down. It sits in WFI, which on an A7 is clock gating, not power off —
     * the cluster and its L2 cannot reach any low-power state while a second
     * core is merely gated. Count its wakes across the sleep: a beat count
     * that climbs while we are supposedly collapsed means CPU1 is being woken
     * (and is holding the cluster up) for the whole night. */
    uint32_t beat0 = g_smp_beat, busy0 = g_smp_busy_ticks;
    int armed_kpd = 0, armed_resin = 0;
    const char *why = "?";
    uint32_t irq0 = g_pmic_irq_n;
    g_pmic_irq_wake = 0u;            /* consume anything latched pre-sleep */
#if defined(PLAT_SOC_MSM8909)
    /* CPU1 serves frame pushes: make sure none is in flight before the
     * cluster's core 0 collapses, and back the QTimer wake with the PMIC RTC
     * alarm (armable + delivering on this PMIC, proven 2026-09-06). */
    if (smp_flush_available()) smp_flush_wait();
    uint32_t alarm0 = g_pmic_irq_alarm;
    if (deadline) {
        uint64_t left = deadline > (uint64_t)timer_ms() ? deadline - (uint64_t)timer_ms() : 0;
        rtc_alarm_arm((uint32_t)(left / 1000u) + 1u);
    }
#endif

    /* Quiesce: panel display OFF (DCS 0x28, page 0 first — same page dance
     * as the re-pin path). Self-refresh stops driving the OLED: true black,
     * and the DDIC keeps its RAM + init state so 0x29 restores instantly. */
    if (!s_panel_off) { panel_dcs(0x28); s_panel_off = 1; }
#if defined(PLAT_SOC_MSM8909)
    /* The PPG LEDs must never ride through a sleep. force_off() now returns
     * whether its writes were ACKed: a failure here means the sensor is still
     * powered and is a prime suspect for a hot watch. Reported once per boot —
     * no extra bus traffic, unlike the register readback this replaced (which
     * ran ~30 bit-banged transactions on the modem-owned bus at sleep entry). */
    { int hr_off = pah8011_force_off();
      static int once = 0;
      if (!once) { once = 1; con_puts(hr_off ? "suspend: hr sensor off (acked)\n"
                                             : "suspend: HR SENSOR OFF FAILED - LEDs may still be on\n"); } }
    /* The touch controller stays awake with the panel off and its INT line
     * dragged the collapsed core back ~22 times per 15 s chunk (and 10x/s when
     * the glass was brushed). Hibernate it and mask its summary IRQ; the exit
     * path resets it awake. Touch is not a wake source by design. */
    touch_set_sleep(1);
    tlmm_irq_mask(1);
#if defined(SLEEP_NO_WDOG)
    /* QUIESCE SET (2026-09-07), after the no-wake sleep cut the drain from
     * ~95 to ~35 mA. Everything here was simply never switched off before:
     *  - panel SLEEP-IN (DCS 0x10): display-off alone leaves the DDIC's
     *    analog front end and OLED boost running; the off-command in the
     *    vendor panel node is 0x28 then 0x10. Wake = 0x11, 120 ms, 0x29.
     *  - WCNSS: wlan_down() left the firmware resident and the boot's RPM
     *    votes in place -- l5/l7/l9 (3.3 V PA rail at 515 mA)/s3, both XO
     *    buffers, the crystal, SNOC 200 MHz, PCNOC 100 MHz. PAS shutdown +
     *    release; the next ble_begin()/WiFi use boots it again.
     *  - cluster clock to the 19.2 MHz crystal while CPU0 is down (CPU1 and
     *    the L2 are otherwise clocked at 200 MHz all night). */
    if (!s_panel_sleeping) { panel_dcs(0x10); s_panel_sleeping = 1; timer_delay_ms(20u); }
#if defined(SLEEP_PAS_KILL_RADIO)
    if (wcnss_fw_resident()) { con_puts("suspend: radio power-off\n"); wlan_power_off(); }
#else
    /* v168: never PAS-kill the radio for a sleep -- its RPM votes would
     * outlive it (Gen 4 v166/v167 census: l9 held ON by the dead session).
     * Firmware stays resident and idles itself, exactly like stock. */
    if (wcnss_fw_resident()) { con_puts("suspend: radio idle (resident)\n"); wlan_idle(); }
#endif
#endif
#endif

    con_dbg("suspend: panel off, tickless ");
    con_dbg_dec(chunk_ms);
    con_dbg(usb_live ? " ms chunks, USB attached (buttons" : " ms chunks (buttons");
#if defined(SUSPEND_TOUCH_WAKE)
    con_dbg("/touch");
#endif
    if (deadline) con_dbg("/timer");
    /* Log the touch INT level even though it is no longer a wake source: if it
     * is already low at entry the controller is asserting with nobody touching
     * the glass, which is the ESD misbehaviour that made it a bad wake source. */
    con_dbg(" wake)");
#if SUSPEND_GEN6
    diag_puts(" touch_int=");
    diag_putdec((uint32_t)tlmm_in(PLAT_TOUCH_INT_GPIO));
#endif
    con_dbg("\n");
    con_flush();

#if SUSPEND_GEN6
    /* MPM census either side of the sleep. The STATUS bit that differs
     * between these two lines is the MPM pin behind the PMIC's GIC 190 —
     * which is the one number missing before a collapsed core can be woken. */
    mpm_dump("sleep-in");
#endif

#if defined(SLEEP_NO_WDOG)
    /* NO WATCHDOG AT ALL ACROSS THE SLEEP (user request, 2026-09-07): the APPS
     * dog is stopped here and not touched again until wake. A resume that
     * never lands now hangs the watch instead of rebooting it. */
    deadman_disarm();
    wdog_disable();
    con_puts("suspend: watchdog DISABLED for the sleep\n");
#else
    wdog_extend(SUSPEND_WDOG_SEC);
#endif
    irq_hist_snap();
#if defined(SLEEP_BATT_DIAG) && defined(PLAT_CHG_SMB231)
    /* SLEEP CURRENT ON THE CABLE (2026-09-07): suspend the charger input so the
     * watch runs from its cell while USB stays enumerated for the log, then
     * print cell voltage + STC3117 current every ~2 s of sleep. This is the
     * A/B between the collapse path and the WFI path without a battery run. */
    smb231_charger_suspend(1);
    con_puts("sleep-batt: charger input SUSPENDED (gpio58 high), running from cell\n");
    uint32_t diag_every = (2000u / (chunk_ms ? chunk_ms : 1u)); if (!diag_every) diag_every = 1u;
    uint32_t diag_n = 0;
#endif
#if defined(USE_CPU_PC_8909)
    cpu_pc8909_state_line("sleep-entry");
#endif

#if defined(SLEEP_NO_WDOG) && defined(PLAT_SOC_MSM8909)
    /* Order matters: everything that still needs a bus goes first, then the
     * buses, then the cluster clock. Wake undoes it in reverse. */
    logfile_flush();                                  /* last eMMC write before its clocks stop */
#if defined(PLAT_CHG_SMB231)
    int soc0 = smb231_soc_x512(); uint32_t soc_t0 = timer_ms();
#endif
#if defined(SYS_PC_8909) && defined(SYS_PC_STAGE) && SYS_PC_STAGE == 5
    (void)smp_cpu1_pc_request_mode(2u);               /* CPU1 off via its SPM on WFI, TZ not involved */
#else
    (void)smp_cpu1_pc_request();                      /* CPU1 off (own SPM + TERMINATE_PC) */
#endif
    gcc_blsp_sleep(1);                                /* QUP cores: touch, charger, sensors */
    gcc_sdcc1_sleep(1);                               /* eMMC */
    gcc_mdss_sleep(1);                                /* MDP + DSI link clocks */
    if (usb_is_configured() && chg_usb_present() == 1) { usb_poll(); usb_irq_arm(1); }   /* cable: USB completions wake the core (USB_IRQ_WAKE builds) */
    con_puts("suspend: buses gated, cluster clock -> XO\n"); con_flush();
#if defined(SYS_PC_8909) && !defined(SYS_PC_XO_PARK)
    /* v175: the kernel's ramp_down_last_cpu() = a7ssmux to its SAFE rate,
     * 400 MHz on GPLL0 (msm8909.dtsi qcom,safe-freq), never the crystal. We
     * are already at 400 MHz GPLL0 while awake (a7cfg 0x403), so for the
     * system collapse the core stays there: TZ's warm boot + the RPM
     * handshake at 19.2 MHz was the one timing we never matched. */
    int xo_parked = 0;
    con_puts("suspend: cluster clock kept at 400 MHz GPLL0 for the system collapse\n");
#else
    int xo_parked = (cpu_clk_sleep_enter() == 0);
#endif
#if defined(SLEEP_FLOOR)
    /* RPM active-set trimming (DDR vote, CX corner, stray LDOs, spare PLLs,
     * USB PHY), measured step by step on a cable. Undone in sleep_floor_exit. */
    sleep_floor_enter(floor_cable);
#endif
#if defined(SLEEP_QUIESCE)
    /* The AP's OWN registers, which the RPM sleep set never sees: the
     * bootloader's GPLL1/GPLL2 + crypto/PRNG votes, and the USB PHY PLL when
     * no cable is attached. Costs microseconds, no RPM traffic, no ladder. */
    sleep_quiesce_enter(usb_is_configured() && chg_usb_present() == 1);
#endif
#if defined(SYS_PC_8909)
    (void)sys_pc8909_prepare(deadline);                       /* cluster off + RPM sleep set; falls back to plain collapse */
#endif
#endif
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
#if defined(USE_CPU_PC_8909)
        cpu_pc8909_mark(0x3E); cpu_pc8909_trace(0x3E);          /* top of the next chunk */
#endif
#if !SUSPEND_GEN6
        /* msm8909w: TrustZone TERMINATE_PC + SPM standalone-pc, wake by the
         * always-on QTimer frame (this chunk's park time) or the PMIC line.
         * Once it declines, stay on WFI for the rest of this boot. */
#if defined(USE_CPU_PC_8909)
        if (!cpu_pc8909_ready() && parks == 0u) con_puts("cpu-pc: not ready (init failed at the 20 s one-shot) - WFI suspend\n");
        if (cpu_pc8909_ready() && (g_cpu_pc_attempts == 0u || g_cpu_pc_ok)) {
            uint32_t pc_t0 = timer_ms();
            did_pc = cpu_pc8909_sleep(park);
            if (did_pc) { pc_ms += timer_ms() - pc_t0; pc_n++; } else pc_fail++;
            if (did_pc) {
                parks++;
                /* proven 83/83 on the C2 (2026-09-06): report the first few
                 * collapses of a sleep, then one in fifty, not every one */
                if (g_cpu_pc_attempts <= 3u || (g_cpu_pc_attempts % 50u) == 0u) cpu_pc8909_report();
                cpu_pc8909_mark(0x38);
#if defined(PC_TRACE)
                if (g_cpu_pc_attempts <= 3u) { con_flush(); blackbox_sync(); }
#endif
                cpu_pc8909_mark(0x3B); cpu_pc8909_trace(0x3B);
            }
            else        { con_puts("cpu-pc: declined, WFI from now on\n"); cpu_pc8909_report(); }
        }
#endif
#elif defined(USE_SYS_SUSPEND)
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
            if (SUSPEND_GEN6 && g_psci_suspend_ok) {
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
#if !SUSPEND_GEN6 && defined(USE_CPU_PC_8909)
        if (did_pc) cpu_pc8909_mark(0x39);
#endif
#if !defined(SLEEP_NO_WDOG)
        wdog_pet();
        deadman_kick();
#endif
#if defined(SLEEP_BATT_DIAG) && defined(PLAT_CHG_SMB231)
        if ((++diag_n % diag_every) == 0u) {
            int ma = fg_batt_ma();
            con_puts("sleep-batt: t="); con_putdec(timer_ms() - t0);
            con_puts(" mv="); con_putdec((uint32_t)fg_batt_mv());
            con_puts(" ma=");
            if (ma == -32768) con_puts("?"); else { if (ma < 0) { con_puts("-"); ma = -ma; } con_putdec((uint32_t)ma); }
            con_puts(" chg="); con_putdec((uint32_t)smb231_charging());
            con_puts(" pc="); con_putdec(did_pc ? 1u : 0u);
            con_puts("\n"); con_flush(); usb_poll();
        }
#endif
#if defined(USE_CPU_PC_8909)
        cpu_pc8909_mark(0x3C); cpu_pc8909_trace(0x3C);
#endif
        usb_poll();
#if defined(USE_CPU_PC_8909)
        cpu_pc8909_mark(0x3D); cpu_pc8909_trace(0x3D);
#endif
        con_flush();

        /* --- wake checks ---------------------------------------------------
         * Interrupt first: if the PMIC IRQ chain is live, the handler has
         * already latched the event and we do not need to touch the bus. The
         * poll below remains the safety net for as long as the chain is
         * unproven. */
#if defined(SYS_PC_8909)
        if (g_cpu_pc_l2_off) {   /* v155 breadcrumb: what the wake path sees right after a cluster resume */
            con_puts("sys-pc: resumed: pmic_irq_wake="); con_putdec(g_pmic_irq_wake);
            con_puts(" gic222 pend="); con_putdec((uint32_t)gic_is_pending(222u));
            con_puts(" en="); con_puthex(mmio_read(PLAT_GICD_BASE + 0x100u + 4u * 6u));
            con_puts(" gicd_ctlr="); con_puthex(mmio_read(PLAT_GICD_BASE));
            con_puts(" gicc_ctlr="); con_puthex(mmio_read(PLAT_GICC_BASE));
            con_puts("\n"); con_flush(); usb_poll();
        }
#endif
#if defined(PLAT_SOC_MSM8909)
        if (g_pmic_irq_wake && g_pmic_irq_alarm == alarm0 && (timer_ms() - t0) < 300u) g_pmic_irq_wake = 0u;  /* same press */
        if (g_pmic_irq_wake) { why = (g_pmic_irq_alarm != alarm0) ? "timer" : "pmic-irq"; break; }
#else
        if (g_pmic_irq_wake) { why = "pmic-irq"; break; }
#endif
        int b = button_wake(&armed_kpd, &armed_resin);
        if (b && (timer_ms() - t0) < 300u) b = 0;   /* the press that forced this sleep, still settling */
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

    /* Resume: panel back on — unless this is a timer wake and the app asked
     * for a dark resume (background check with the screen off). Every
     * subsequent fb_kick re-pins the window, so no further display state
     * restoration is needed. */
#if defined(PLAT_SOC_MSM8909)
#if defined(USE_CPU_PC_8909)
    cpu_pc8909_state_line("sleep-exit");
#endif
#if defined(SLEEP_BATT_DIAG) && defined(PLAT_CHG_SMB231)
    smb231_charger_suspend(0);
    con_puts("sleep-batt: charger input restored\n");
#endif
#if defined(SLEEP_NO_WDOG) && defined(PLAT_SOC_MSM8909)
#if defined(SYS_PC_8909)
    sys_pc8909_finish();
#endif
#if defined(SLEEP_FLOOR)
    sleep_floor_exit();
#endif
#if defined(SLEEP_QUIESCE)
    sleep_quiesce_exit();          /* PLL votes back before any branch re-enable */
#endif
    usb_irq_arm(0);
    if (xo_parked) cpu_clk_sleep_exit();
    gcc_mdss_sleep(0);
    gcc_sdcc1_sleep(0);
    gcc_blsp_sleep(0);
    (void)smp_cpu1_wake();
#if defined(SYS_PC_8909)
    /* v193: the USB console did not come back after the first real cluster
     * collapses (RPM sleep set applied). Bring the controller up from scratch
     * whenever a cable is present; ~300 ms, host re-enumerates. */
    if (pc_n && chg_usb_present() == 1) { con_puts("usb: re-init after the system collapse\n"); (void)usb_dev_reinit(); }
#endif
#if defined(PLAT_CHG_SMB231)
    /* TRUE AVERAGE SLEEP CURRENT (v190): STC3117 SOC delta over the sleep.
     * 1 LSB = 1/512 %; cell = PLAT_BATT_MAH. Only meaningful on battery and
     * over long sleeps (1 LSB ~ 0.7 mAh on a 350 mAh cell). */
    { int soc1 = smb231_soc_x512(); uint32_t dt = timer_ms() - soc_t0;
      con_puts("sleep-gauge: soc "); 
      if (soc0 >= 0 && soc1 >= 0) {
          con_putdec((uint32_t)soc0 * 100u / 512u); con_puts(" -> "); con_putdec((uint32_t)soc1 * 100u / 512u); con_puts(" (x0.01%) over ");
          con_putdec(dt / 1000u); con_puts(" s");
          if (dt >= 60000u && soc1 > soc0) {
              /* v200: the STC3117 re-bases its SOC from the open-circuit voltage
               * after a long rest (and a cable charges it). A rising SOC is not
               * a negative current; say what it is. */
              con_puts(" = gauge re-estimated SOC upward (OCV correction or charging), no current figure");
          } else if (dt >= 60000u) {
              int d = soc0 - soc1;                         /* + = discharged */
              /* mA = d/512 % * mAh / 100 / hours = d * mAh * 3600000 / (512*100*dt) */
              int64_t ma100 = (int64_t)d * PLAT_BATT_MAH * 3600000LL * 100 / (512LL * 100 * dt);
              con_puts(" = avg "); if (ma100 < 0) { con_puts("-"); ma100 = -ma100; }
              con_putdec((uint32_t)(ma100 / 100)); con_puts("."); con_putdec((uint32_t)((ma100 % 100) / 10)); con_puts(" mA");
          } else con_puts(" (too short for an average)");
      } else { con_puts("unavailable (entry rc "); con_putdec((uint32_t)(soc0 < 0 ? -soc0 : 0)); con_puts(" exit rc "); con_putdec((uint32_t)(soc1 < 0 ? -soc1 : 0)); con_puts(": 1 no gauge, 2 bus, 3 GG_RUN was clear, 4 soc read)"); }
      con_puts("\n"); }
#endif
#if defined(SYS_PC_8909)
    pon_crumb_write(0x40u);
#endif
#endif
#if defined(SLEEP_NO_WDOG)
    wdog_extend(30u);                /* re-arm only now that we are awake */
#endif
    rtc_alarm_disarm();
    tlmm_irq_mask(0);
    touch_set_sleep(0);              /* reset pulse: the only way out of hibernate */
#endif
    s_last_cause = (why[0] == 't') ? PLAT_WAKE_TIMER : PLAT_WAKE_BUTTON;
    if (!(s_keep_dark && s_last_cause == PLAT_WAKE_TIMER)) plat_display_on();

    /* THE NUMBER THAT MATTERS (2026-09-06): the C2 lost 4.15 -> 3.67 V in
     * 3 h 50 min asleep, i.e. tens of mA, which is cluster-awake territory
     * rather than a leaking sensor. Time-inside-collapse vs wall time says
     * straight away whether the core is actually off for the sleep or whether
     * we are only parking the tick. Anything well under ~99% means the drain
     * is us, not a rail. */
    { uint32_t tot = timer_ms() - t0;
      con_puts("suspend: residency "); con_putdec(pc_ms); con_puts("/"); con_putdec(tot);
      con_puts(" ms collapsed ("); con_putdec(tot ? (pc_ms / (tot / 100u ? tot / 100u : 1u)) : 0u);
      con_puts("%), chunks="); con_putdec(parks);
      con_puts(" pc_ok="); con_putdec(pc_n); con_puts(" pc_fail="); con_putdec(pc_fail);
      con_puts(" cpu1_wakes="); con_putdec(g_smp_beat - beat0);
      con_puts(" cpu1_busy_ticks="); con_putdec(g_smp_busy_ticks - busy0);
      con_puts("\n"); }

    uint32_t slept = timer_ms() - t0;
    g_pwr_sleep_ms += slept;         /* book it as sleep, not compute */

#if !SUSPEND_GEN6 && defined(USE_CPU_PC_8909)
    if (parks) cpu_pc8909_mark(0x3A);
#endif
    irq_hist_report();
    con_puts("suspend: woke by "); con_puts(why);
    if (s_panel_off) con_puts(" (dark)");
    con_puts(" after ");           con_putdec(slept);
    con_puts(" ms; parks=");       con_putdec(parks);
    diag_puts(" ticks_caught=");   diag_putdec(caught);
    /* Wakeups per second while suspended: the number rung 1 exists to move.
     * Was 1000 by construction; expect ~1000/SUSPEND_CHUNK_MS now. */
    /* Per MINUTE now, not per second: at a 15 s chunk the per-second figure
     * rounds to 0 for every realistic sleep and stops being informative.
     * Was 60000/min (the 1 kHz tick), then 240/min, now ~4/min. */
    if (slept >= 1000u) {
        con_dbg(" wakeups/min=");
        con_dbg_dec((uint32_t)((uint64_t)parks * 60000u / slept));
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
    if (g_pmic_irq_spurious) { con_dbg(" spur="); con_dbg_dec(g_pmic_irq_spurious); }
    if (g_pmic_irq_stuck)    { con_dbg(" stuck="); con_dbg_dec(g_pmic_irq_stuck); }
    if (g_psci_fail_n) {
        con_dbg(" psci_fail="); con_dbg_dec(g_psci_fail_n);
        con_dbg(" err=");       con_dbg_dec((uint32_t)g_psci_last_err);
    }
    con_dbg("\n");
#if SUSPEND_GEN6
    mpm_dump("wake");
    sleep_stats_line();
#endif
    con_flush();
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 || PLAT_BOARD_FOSSIL_GEN4 || PLAT_BOARD_TICWATCH_C2 */
