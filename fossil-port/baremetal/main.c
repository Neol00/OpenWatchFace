/* main.c — Phase 2 proof: FreeRTOS multitasking on the fossil-port runtime.
 *
 * Two tasks demonstrate the full contract the firmware will rely on:
 *  - heartbeat: vTaskDelay timing (tick IRQ, preemption) checked against the
 *    free-running arch timer, plus an FPU computation (per-task VFP context).
 *  - counter: busy-ish worker at lower priority proving preemptive scheduling.
 * Next step stacks LVGL + the ramfb framebuffer on top of this.
 */
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"

extern char __image_start[], __image_end[], __ramlog_start[];

static volatile uint32_t s_counter;

static void counter_task(void *arg)
{
    (void)arg;
    for (;;) s_counter++;              /* lowest prio: soaks idle time */
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t beat = 0;
    float drift = 0.25f;               /* exercise per-task FPU context */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        drift *= 1.5f;
        con_puts("beat "); con_putdec(++beat);
        con_puts("  rtos-ticks="); con_putdec((uint32_t)xTaskGetTickCount());
        con_puts("  wall-ms="); con_putdec(timer_ms());
        con_puts("  counter="); con_putdec(s_counter);
        con_puts("  fpu="); con_putdec((uint32_t)drift);
        con_puts("\n");
    }
}

void main(void)
{
#if defined(PLAT_SOC_MSM)
    /* Breadcrumbs FIRST — before the MMU, which is itself a suspect. startup.S
     * already wrote stage 1 ("aboot jumped to us") in assembly; from here each
     * stage is recorded in IMEM so a failed boot can be read out afterwards
     * from a rooted Linux (devmem 0x08600804). See bootmark.c.
     *
     * Widen the APPS watchdog immediately: aboot leaves it armed at an 11 s
     * bark, which was warm-resetting every one of our boots into the stock OS
     * at ~15 s regardless of what the code did. 120 s keeps it as a free
     * recovery path (a hang still reboots the watch by itself) while giving
     * slow init all the room it needs. */
    bootmark(BOOTMARK_RELOCATED);
#if defined(WDOG_TRACE)
    /* WATCHDOG STAIRCASE — every milestone sets a distinct reboot timeout, so
     * a stopwatch reading from power-on names the last milestone reached.
     * The stage->seconds table lives in platform/msm_wdog.c; stages are 2 s
     * apart, all inside the 20-bit register's ~32 s ceiling (values above that
     * TRUNCATE silently — a 45 s request became 13 s and wasted several test
     * cycles by masquerading as the ~15 s "never ran" baseline).
     *
     *   ~15 s = ABOOT'S OWN DOG: our code never ran (14-16 s left empty)
     *    4 s  = main() entered, died in mmu_enable_flat() building tables
     *    6 s  = L1 table built, died turning the MMU/caches ON
     *    8 s  = MMU + caches ON, executing translated
     *   10 s  = mmu_enable_flat() returned
     *   12 s  = uart_init survived
     *   18 s  = ramlog_init survived
     *   20 s  = gic_init survived
     *   22 s  = vib_init + vib_buzz + an SPMI read survived (SPMI REALLY WORKS)
     *   24 s  = xTaskCreate OK (dying here = scheduler never ran our task)
     *   26 s  = the app task RAN (hung inside setup())
     *   28 s  = display init returned
     *   30 s  = setup() done, first loop() complete
     *    2 s  = xTaskCreate FAILED (heap)
     *    3 s  = vTaskStartScheduler returned (must never happen)
     *  never  = reached loop() and keeps petting: THE FIRMWARE IS ALIVE */
    wdog_stage(1);
#elif defined(NO_AUTO_REBOOT)
    /* "Let it run, do not reboot on me" build (for the FLASH-to-recovery test
     * loop). Fully disarm the APPS watchdog so nothing yanks the watch away
     * while you are looking at it. The cost: a hang needs a physical
     * force-reboot, since our FreeRTOS dead-man is disarmed too. */
    wdog_disable();
#else
    wdog_extend(120u);
#endif
    bootmark(BOOTMARK_WDOG_OFF);

#if defined(ENTRY_STOP)
    /* RECOVERY-PATH BISECT (2026-08-03): the full-size firmware dies ~3 s
     * into a recovery-partition boot while the same bytes run 20 s+ via
     * fastboot boot. This stops the world at the very first C instruction:
     *   ~31 s reboot  -> main() IS reached from recovery; the killer is later
     *   ~3 s  reboot  -> the image never gets here; the problem is LK-side
     * (30 s, NOT more: the 20-bit wdog register truncates above ~32 s.) */
    wdog_extend(30u);
    for (;;) { }
#endif
#endif

    mmu_enable_flat();
#if defined(PLAT_SOC_MSM)
    bootmark(BOOTMARK_MMU);
#endif
#if defined(WDOG_TRACE)
    wdog_stage(4);           /* MMU survived */
#endif
    uart_init();
#if defined(WDOG_TRACE)
    wdog_stage(5);           /* uart_init survived (no-op on this board) */
#endif
    ramlog_init();
#if defined(WDOG_TRACE)
    wdog_stage(6);           /* ramlog_init survived */
#endif
    gic_init();
#if defined(PLAT_SOC_MSM)
    bootmark(BOOTMARK_GIC);
#endif
#if defined(WDOG_TRACE)
    wdog_stage(7);           /* gic_init survived */
#endif

#if defined(TICK_PROBE4) && defined(PLAT_SOC_MSM)
    /* Probe 4 — CLEAN (earlier probes read IAR first, which can ACK a stray
     * pending interrupt and stick the running priority OURSELVES). No IAR
     * touch until the decisive read. reading ~= 2 + armed:
     *   armed 25 (~27 s): IAR acks 20 first try - GIC path fully healthy,
     *                     the no-tick-in-SAFETY problem is port-side (PMR).
     *   armed  4 (~6 s):  RPR clean, still refused, prio-0 doesn't help.
     *   armed  7 (~9 s):  RPR clean, refused at 0xF8 but ACKS at prio 0.
     *   armed 14 (~16 s): RPR stuck (aboot's, genuinely), prio-0 no help.
     *   armed 17 (~19 s): RPR stuck, but prio-0 PRESENTS - workaround known.
     */
    {
        gic_enable_irq(20, 0xF8);
        uint64_t dl = timer_ticks() + (timer_freq_hz() / 500u);
        __asm__ volatile("mcrr p15, 3, %0, %1, c14"
                         :: "r"((uint32_t)dl), "r"((uint32_t)(dl >> 32)));
        __asm__ volatile("mcr p15, 0, %0, c14, c3, 1" :: "r"(1u));
        timer_delay_ms(6);
        uint32_t rpr  = mmio_read(0x0B002000u + 0x14u);   /* before any ack */
        uint32_t pend = (mmio_read(0x0B000000u + 0x200u) >> 20) & 1u;
        uint32_t iar  = mmio_read(0x0B002000u + 0x0Cu) & 0x3FFu;
        uint32_t armed;
        if (iar == 20u) {
            armed = 25u;
        } else {
#if defined(TICK_PROBE_CURE)
            /* Probe 5 — which PERMANENT cure unblocks presentation at the
             * NORMAL tick priority (0xF8)? reading ~= 2 + armed:
             *   ~27 = GICC_APR0-3 clear alone fixes it  -> ship in gic_init
             *   ~16 = EOIR priority-drop drain fixes it -> ship in gic_init
             *   ~10 = neither, but stuck RPR > 0xC0     -> mid-prio workaround
             *   ~7  = neither, RPR <= 0xC0              -> deeper surgery
             */
            mmio_write(0x0B002000u + 0xD0u, 0);
            mmio_write(0x0B002000u + 0xD4u, 0);
            mmio_write(0x0B002000u + 0xD8u, 0);
            mmio_write(0x0B002000u + 0xDCu, 0);
            uint32_t iar2 = mmio_read(0x0B002000u + 0x0Cu) & 0x3FFu;
            if (iar2 == 20u) {
                armed = 25u;
            } else {
                for (uint32_t n = 0; n < 32; n++)
                    mmio_write(0x0B002000u + 0x10u, 31u);  /* EOIR: prio drop */
                uint32_t iar3 = mmio_read(0x0B002000u + 0x0Cu) & 0x3FFu;
                if (iar3 == 20u) armed = 14u;
                else             armed = 5u + 3u * ((rpr > 0xC0u) ? 1u : 0u);
            }
            con_puts("TICK_P5: iar2="); con_putdec(iar2); con_puts("\n");
#else
            uint32_t stuck = (rpr < 0xF8u) ? 1u : 0u;
            gic_enable_irq(20, 0x00);                     /* highest prio */
            timer_delay_ms(2);
            uint32_t iar2 = mmio_read(0x0B002000u + 0x0Cu) & 0x3FFu;
            armed = 4u + 10u * stuck + 3u * ((iar2 == 20u) ? 1u : 0u);
#endif
        }
        con_puts("TICK_P4: rpr="); con_puthex(rpr);
        con_puts(" pend20="); con_putdec(pend);
        con_puts(" iar="); con_putdec(iar); con_puts("\n");
        wdog_extend(armed);
        for (;;) { }
    }
#endif

#if defined(TICK_PROBE) && defined(PLAT_SOC_MSM)
    /* TICK_PROBE (2026-08-03) — the XPU-carveout fix stopped the crashes but
     * the tick (virt timer INTID 20) still never delivers. Ask the hardware
     * three yes/no questions, no scheduler involved, and encode the answers
     * in the wdog: reading ~= 7 + 3*(A + 2B + 4C):
     *   A: CNTV comparator raises ISTATUS after the deadline
     *   B: our GICD_ISENABLER0 bit-20 write actually sticks (0 = secure/WI)
     *   C: GICC_IAR acknowledges INTID 20 (full delivery to the CPU boundary)
     * A=1,B=1,C=1 (~28 s) -> GIC path fine, problem is in the port's moment
     * of delivery; B=0 -> INTID 20 is secure-group, switch tick to CNTP;
     * A=0 -> virtual comparator dead, switch to CNTP. */
    {
        gic_enable_irq(20, 0xF8);
        uint32_t b = (mmio_read(0x0B000000u + 0x100u) >> 20) & 1u;
        uint64_t now = timer_ticks();
        uint64_t dl  = now + (timer_freq_hz() / 500u);      /* 2 ms */
        __asm__ volatile("mcrr p15, 3, %0, %1, c14"
                         :: "r"((uint32_t)dl), "r"((uint32_t)(dl >> 32)));
        __asm__ volatile("mcr p15, 0, %0, c14, c3, 1" :: "r"(1u));
        timer_delay_ms(6);
        uint32_t ctl;
        __asm__ volatile("mrc p15, 0, %0, c14, c3, 1" : "=r"(ctl));
        uint32_t a = (ctl >> 2) & 1u;                       /* ISTATUS */
        uint32_t iar = mmio_read(0x0B002000u + 0x0Cu);      /* ack */
        uint32_t c = ((iar & 0x3FFu) == 20u) ? 1u : 0u;
        con_puts("TICK_PROBE: istatus="); con_putdec(a);
        con_puts(" enable20="); con_putdec(b);
        con_puts(" iar="); con_putdec(iar & 0x3FFu); con_puts("\n");
#if defined(TICK_PROBE_SCAN)
        /* Round 2 (after A=1,B=1,C=0): WHICH INTID does the firing CNTV
         * comparator actually pend at the distributor? Scan GICD_ISPENDR.
         * reading ~= 2 + armed; armed = INTID-12 for INTIDs 16..31
         * (16->4s ... 20->8s ... 27->15s ... 31->19s), 22 = some SPI 32..63,
         * 28 = NOTHING pending (line never reaches the GICD -> use CNTP). */
        {
            uint32_t pend0 = mmio_read(0x0B000000u + 0x200u);
            uint32_t pend1 = mmio_read(0x0B000000u + 0x204u);
            con_puts("TICK_SCAN: ispendr0="); con_puthex(pend0);
            con_puts(" ispendr1="); con_puthex(pend1); con_puts("\n");
            uint32_t armed = 28u;                      /* nothing pending */
            for (uint32_t id = 16; id < 32; id++)
                if (pend0 & (1u << id)) { armed = id - 12u; break; }
            if (armed == 28u && pend1) armed = 22u;    /* an SPI pends */
#if defined(TICK_PROBE_APR)
            /* Round 3 (INTID 20 pends but IAR refuses): suspect a STALE
             * ACTIVE interrupt in the CPU interface left by aboot — running
             * priority stuck high blocks all group-1 presentation, and
             * GICD_ICACTIVER does NOT clear the GICC's Active Priority
             * Registers. Read RPR, clear APR0-3, re-try IAR.
             * reading ~= 2 + 4 + 12*(IAR acks 20 now) + 6*(RPR was stuck):
             *   ~6  = no stuck RPR, still refused (mystery deeper)
             *   ~12 = RPR stuck, APR clear NOT enough
             *   ~18 = acks now, RPR looked idle (transient?)
             *   ~24 = RPR WAS stuck + APR clear FIXED IT (fix goes in gic_init)
             */
            {
                uint32_t rpr = mmio_read(0x0B002000u + 0x14u);
                con_puts("TICK_APR: rpr="); con_puthex(rpr);
                mmio_write(0x0B002000u + 0xD0u, 0);
                mmio_write(0x0B002000u + 0xD4u, 0);
                mmio_write(0x0B002000u + 0xD8u, 0);
                mmio_write(0x0B002000u + 0xDCu, 0);
                uint32_t iar2 = mmio_read(0x0B002000u + 0x0Cu);
                con_puts(" iar2="); con_putdec(iar2 & 0x3FFu); con_puts("\n");
                armed = 4u + 12u * (((iar2 & 0x3FFu) == 20u) ? 1u : 0u)
                           + 6u * ((rpr < 0xF8u) ? 1u : 0u);
            }
#endif
            wdog_extend(armed);
        }
#else
        wdog_extend(5u + 3u * (a + 2u * b + 4u * c));
#endif
        for (;;) { }
    }
#endif

#if defined(PING_TEST) && defined(PLAT_SOC_MSM)
    /* PING_TEST — the smallest possible round-trip diagnostic, and the FIRST
     * hardware experiment when there is no way to feel the buzz or read the UART.
     *
     * It depends on NOTHING downstream of the reset path: no vibrator (an
     * unproven SPMI-v1 assumption on this v2 arbiter), no FreeRTOS scheduler, no
     * display. It just proves — via the ONE externally observable signal, the
     * watch reappearing in fastboot — that aboot accepted our image, startup.S
     * self-relocated correctly on real DDR, the MMU/GIC came up (con_puts and
     * timer both run through the enabled MMU here), and the PS_HOLD warm-reset +
     * imem restart-reason cookie actually land us back in the bootloader.
     *
     * If this image round-trips to fastboot, every remaining failure is
     * downstream (FreeRTOS, haptics, display) and can be bisected from here. If
     * it does NOT, the fault is upstream and none of the richer images would
     * have run either. */
    con_puts("\nPING_TEST [" PLAT_NAME "]: reached main; mmu+uart+gic up\n");
    con_puts("boot_r2=");   con_puthex(boot_r2);
    con_puts(" boot_fault="); con_puthex(boot_fault);
    con_puts("\nPING_TEST: rebooting to bootloader in 6s\n");
    timer_delay_ms(6000);
    reboot_to_bootloader();
    for (;;) { __asm__ volatile("wfi"); }
#endif

#if defined(DISPLAY_TEST) && defined(PLAT_SOC_MSM)
    /* DISPLAY_TEST rev 3 (2026-08-03) — MDSS clock bring-up + pipe takeover.
     *
     * fb_init() now (a) powers the MDSS GDSC and enables every MDSS branch
     * clock at the GCC (the fix for the "touch an unclocked block, hang the
     * bus" family), (b) probes which pipe aboot's splash used and repoints
     * its SRC0_ADDR at OUR buffer (the splash region itself is XPU-protected
     * — CPU writes there can never work), then fb_flush_all() latches with
     * CTL_FLUSH and fires CTL_START.
     *
     * TWO output channels:
     *   SCREEN: flashing white/black at 1 Hz = takeover works, campaign won.
     *   STOPWATCH (wdog armed once, then never petted; add ~2 s boot):
     *     8 s  MDSS GDSC refused to power on
     *    12 s  GDSC on but core clocks (ahb/axi/mdp/vsync) stuck
     *    16 s  clocks fine but NO live pipe found -> aboot's MDP state was
     *          lost (GDSC had collapsed); full MDP5+DSI+PLL init is next
     *    20 s  pipe taken over BUT the DSI byte/pixel clocks are dead ->
     *          12nm DSI PLL bring-up is the missing piece
     *    26 s  everything up, kickoffs firing -> if the screen STILL does
     *          not change, the fault is in the kickoff/DSI-push details
     * Nothing here can hang: no LVGL, no touch, no I2C. */
    vib_init();
    vib_buzz(1, 400);

    void *probe_fb = fb_init(PLAT_PANEL_W, PLAT_PANEL_H);
    uint32_t st = gcc_mdss_status();
    con_puts("DISPLAY_TEST: fb="); con_puthex((uint32_t)(uintptr_t)probe_fb);
    con_puts(" gcc="); con_puthex(st);
    con_puts(" w="); con_putdec(fb_width());
    con_puts(" h="); con_putdec(fb_height());
    con_puts(" bpp="); con_putdec(fb_bpp()); con_puts("\n");

    /* display6 staircase (2026-08-03): full from-scratch PHY + PLL
     * programming (relock-with-retained-config disproven by display4/5).
     *    8 s  GDSC/core clocks failed (regression — should not happen)
     *   10 s  esc0 stuck — esc0 is XO-sourced, so this is a GCC-side fault,
     *         NOT the PLL (would redirect the whole investigation)
     *   12 s  PLL would not lock even fully programmed
     *   16 s  no live pipe (regression)
     *   20 s  PLL locked + esc0 fine but byte/pclk STILL stuck
     *   26 s  entire clock chain up + kicking -> the screen is the verdict */
    uint32_t armed;
    if (!(st & GCC_MDSS_ST_GDSC_ON) || !(st & GCC_MDSS_ST_CORE_CLKS))
                                                armed = 8u;
    else if (!(st & GCC_MDSS_ST_DSI_CLKS)) {
        if (!dsi_pll_12nm_status_locked())      armed = 12u;
        else if (!(st & GCC_MDSS_ST_ESC0_CLK))  armed = 10u;
        else                                    armed = 20u;
    }
    else if (!probe_fb || fb_bpp() == 0u)       armed = 16u;
    else                                        armed = 26u;
    wdog_extend(armed);

    /* display7: fb_kick is now a synchronous transaction (wait + ack of
     * PP0_DONE and CMD_MDP_DONE — display6's solid-white proved frame #2+
     * were swallowed without the acks). Measure SIX flips, then re-arm the
     * wdog with the count so the stopwatch reports repeat-kickoff health:
     *   12 s  0 frames completed (regression vs display6)
     *   20 s  exactly 1 (the display6 behaviour: acks did not help)
     *   22 s  2-5 (flaky — timing detail)
     *   26 s  all 6 (screen must be FLASHING: campaign complete)
     * The flashing continues until the wdog fires either way. */
    if (probe_fb) {
        volatile uint8_t *p = (volatile uint8_t *)probe_fb;
        uint32_t n = fb_width() * fb_height() * fb_bpp();
        uint32_t ok = 0;
        for (uint32_t pass = 0;; pass++) {
            uint8_t colour = (pass & 1u) ? 0x00u : 0xFFu;
            for (uint32_t i = 0; i < n; i++) p[i] = colour;
            fb_flush_all();
            if (pass < 6u && fb_last_kick_err() == 0u) ok++;
            if (pass == 5u) {
                /* display10: DSI error census outranks the ok-count.
                 *   10 = ACK errors (panel NAKs our packets)
                 *   14 = PHY lane errors
                 *   18 = FIFO over/underflow
                 *   22 = HS TX timeout
                 *   26 = engines clean + all kicks completed */
                uint32_t err = fb_error_classes(), code;
                if      (err & 1u) code = 10u;
                else if (err & 4u) code = 14u;
                else if (err & 8u) code = 18u;
                else if (err & 2u) code = 22u;
                else code = (ok == 6u) ? 26u : (ok == 1u) ? 20u : 12u;
                wdog_extend(code);
            }
            timer_delay_ms(1000);   /* back to display8's proven cadence */
        }
    }
    for (;;) { }
#endif

#if defined(STAGE_REPORT) && defined(PLAT_SOC_MSM)
    /* STAGE_REPORT — report how far we got using the ONLY channel this watch
     * actually has.
     *
     * Everything else was a dead end, and the device's own kernel config says
     * why: AsteroidOS is built with CONFIG_DEVMEM=n and CONFIG_PSTORE=n, so
     * there is no /dev/mem, no devmem, no pstore — raw memory CANNOT be read
     * back from Linux on this watch. The DDR ramlog is worse than useless
     * (it links inside the next kernel's load area). The motor needs SPMI,
     * which is itself a prime suspect. The display is what we are debugging.
     *
     * What IS reliably observable, and already demonstrated on this device, is
     * WHERE THE WATCH ENDS UP after it reboots. There are three distinguishable
     * destinations, so one boot carries ~1.5 bits — enough to bisect:
     *
     *   ends in WEAR OS   -> we never reached this code; the hardware watchdog
     *                        bit at ~15 s (that is what a stock warm reset does)
     *   ends in RECOVERY  -> our code RAN, SPMI answered, but the DISPLAY probe
     *      (AsteroidOS)     failed: fb_init() found no usable framebuffer
     *   ends in FASTBOOT  -> our code RAN and the display probe SUCCEEDED
     *
     * Plus the motor as an independent bit: if you feel a buzz, SPMI works too.
     * No device-side tooling, nothing to install, nothing to read out. */
    con_puts("\nSTAGE_REPORT: main reached; probing\n");

    vib_init();
    vib_buzz(2, 200);            /* independent SPMI bit: felt = arbiter OK */

    void *fb = fb_init(PLAT_PANEL_W, PLAT_PANEL_H);
    con_puts("STAGE_REPORT: fb=" ); con_puthex((uint32_t)(uintptr_t)fb);
    con_puts("\n");

    if (fb) {
        /* Paint the whole buffer white and kick the MDP, so if the panel IS
         * updatable you SEE it go white before the reboot — that alone would
         * prove the command-mode kickoff works. */
        volatile uint32_t *p = (volatile uint32_t *)fb;
        for (uint32_t i = 0; i < fb_width() * fb_height(); i++) p[i] = 0x00FFFFFFu;
        fb_flush_all();
        timer_delay_ms(4000);    /* long enough to see it */
        con_puts("STAGE_REPORT: display OK -> rebooting to FASTBOOT\n");
        reboot_to_bootloader();
    } else {
        timer_delay_ms(2000);
        con_puts("STAGE_REPORT: no framebuffer -> rebooting to RECOVERY\n");
        reboot_to_recovery();
    }
    for (;;) { __asm__ volatile("wfi"); }
#endif

#if defined(PLAT_SOC_MSM)
    /* SIGN OF LIFE — the first thing that happens on this watch.
     *
     * With no UART pad and no proven display, a dark watch is ambiguous: it
     * means either "the image never ran" or "the image ran and the display
     * failed", and those need opposite fixes. The motor disambiguates it, so
     * buzz BEFORE touching any display code.
     *
     * Pattern vocabulary (count the pulses through the case):
     *   1 long   - reached main(); MMU + timer + GIC are up
     *   2 short  - display stack reported success
     *   3 short  - display stack FAILED (see ramlog)
     *   (nothing)- never got here: boot.img/load-address/startup.S problem
     */
    /* The watchdog was already widened at the top of main() — the very first
     * thing done, because at aboot's 11 s bark it was killing every boot. */
    vib_init();
    vib_buzz(1, 400);
    bootmark(BOOTMARK_VIB);
    /* Record what the SPMI layer actually managed: a PMIC register read-back
     * proves arbiter addressing works even when the motor stays silent (a dead
     * motor and a misprogrammed arbiter look identical from outside). */
#if defined(PLAT_BOARD_FOSSIL_GEN6)   /* PM660 haptics regs; the Gen 4's PM8916
                                         vibrator block has a different map */
    {
        uint8_t v = 0xEE;
        int rc = spmi_read8(PLAT_PMIC_SID, PLAT_HAP_BASE + 0x44u, &v); /* EN_CTL */
        bootmark_aux(0, ((uint32_t)(rc & 0xFF) << 16) | v);
    }
#endif
#if defined(WDOG_TRACE)
    wdog_stage(8);           /* the whole SPMI/vibration path survived */
#endif
#endif

    con_puts("\nOpenWatchFace bare-metal runtime [" PLAT_NAME "] + FreeRTOS " tskKERNEL_VERSION_NUMBER "\n");
    bdiag_puts("image:  ");  bdiag_puthex((uint32_t)(uintptr_t)__image_start);
    bdiag_puts("..");        bdiag_puthex((uint32_t)(uintptr_t)__image_end);
    bdiag_puts("\nramlog: "); bdiag_puthex((uint32_t)(uintptr_t)__ramlog_start);
    bdiag_puts(ramlog_had_previous() ? " (previous boot log preserved)\n" : " (fresh)\n");
    bdiag_puts("timer:  ");  bdiag_putdec(timer_freq_hz()); bdiag_puts(" Hz\n");

#if defined(SAFETY_TEST) && defined(PLAT_SOC_MSM)
    /* SAFETY_TEST build: prove the RECOVERY PATH in isolation before trusting
     * the display stack. No DSI/panel/LVGL/touch — just arm a short dead-man and
     * let it reboot us to fastboot. If this image reaches fastboot on its own,
     * the escape hatch works and the full firmware is safe to boot. If it does
     * NOT (watch hangs / reboots to stock), we learned that WITHOUT the whole
     * UI stack confusing the result. Recover meanwhile by draining the battery.
     * A 1 Hz heartbeat prints to the ramlog so a post-reboot fastboot RAM dump
     * shows how far it got. */
    con_puts("SAFETY_TEST: watchdog-only image, no display\n");
    deadman_arm(15000u);            /* short: you are not waiting long */
    con_puts("SAFETY_TEST: dead-man armed 15s; expect reboot-to-fastboot\n");
#if defined(DELAY_BISECT)
    /* Localize an EXTERNAL ~3 s reset that no wdog value of ours explains:
     * a 5 s polled delay HERE moves the reset to ~8 s if the killer is in
     * the scheduler-start path (everything after this line), and leaves it
     * at ~3 s if the killer already ran (deadman/timer create, prints,
     * earlier init). Timing, not guessing. */
    con_puts("DELAY_BISECT: 5 s pause before the scheduler\n");
    timer_delay_ms(5000);
#endif
    bdiag_puts("starting scheduler\n");
    vTaskStartScheduler();
    con_puts("!! scheduler returned\n");
    for (;;) { }
#else
#if defined(OWF_APP)
    /* Real firmware: run OpenWatchFace's setup()/loop() (compat/arduino_main.cpp)
     * instead of the ui_demo task. Big stack — the firmware's setup()/LVGL call
     * chains are deep. */

    /* RUN THE C++ STATIC CONSTRUCTORS. Found 2026-08-03 after a nine-image
     * bisect: .init_array was linked but never executed, so every global C++
     * object (USBSerial, Serial, ...) had a NULL vtable pointer, and setup()'s
     * banner println — the firmware's FIRST global virtual call — dereferenced
     * NULL into the XPU-protected 0x0 region: instant hard reset on recovery
     * boots, prefetch abort (misdecoded for a day as a "late-setup hang") on
     * fastboot boots. Must run after MMU+bss (startup.S) and before any task
     * can touch a global C++ object. */
    {
        extern void (*__init_array_start[])(void);
        extern void (*__init_array_end[])(void);
        for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
            (*f)();
        bdiag_puts("static ctors: ");
        bdiag_putdec((uint32_t)(__init_array_end - __init_array_start));
        bdiag_puts(" ran\n");
    }

    extern void owf_app_task(void *);
    (void)heartbeat_task; (void)counter_task;
    {
#if defined(WDOG_TRACE) && defined(HEAP_PROBE)
        /* HEAP PROBE (2026-08-03). Two independent runs reported "xTaskCreate
         * failed at every stack size", which cannot be a plain out-of-memory:
         * ucHeap is a 16 MB .bss array and the largest request is 64 KB. So
         * test the allocator itself, before FreeRTOS gets a chance to hide the
         * result, and report through the watchdog:
         *   30 s -> even a 4 KB pvPortMalloc fails: the heap is not usable at
         *           all (heap never initialised / .bss not really zeroed /
         *           ucHeap corrupted by something writing over it)
         *   27 s -> 4 KB works but 64 KB does not: sizing or fragmentation
         *   24 s -> both work and >=8 MB is free: the allocator is HEALTHY and
         *           the fault is inside xTaskCreate/FreeRTOS, not the heap
         *   22 s -> allocations work but the free count is implausibly small
         * Each failing case halts on the spot so its code is what you read. */
        {
            void *p1 = pvPortMalloc(4096);
            if (!p1) {
                con_puts("!! pvPortMalloc(4KB) FAILED — heap unusable\n");
                wdog_stage(39);
                for (;;) { }
            }
            void *p2 = pvPortMalloc(65536);
            if (!p2) {
                con_puts("!! pvPortMalloc(64KB) FAILED — heap too small\n");
                wdog_stage(38);
                for (;;) { }
            }
            vPortFree(p2);
            vPortFree(p1);
            con_puts("heap: free=");
            con_putdec((uint32_t)xPortGetFreeHeapSize());
            con_puts(" bytes\n");
            wdog_stage(xPortGetFreeHeapSize() >= (8u << 20) ? 37u : 40u);
        }
#endif

        /* A 64 KB stack out of a 16 MB heap must not fail — yet a 2 s reboot
         * (the "xTaskCreate FAILED" code) was observed once on 2026-08-03.
         * Rather than dying there, DEGRADE: retry with progressively smaller
         * stacks so the boot continues, and report which one took via a
         * distinct stage. That turns a dead end into data, and if a smaller
         * stack works it points at heap fragmentation/sizing rather than the
         * heap being absent. */
        static const uint16_t k_stack_try[] = { 16384, 8192, 4096 };
        BaseType_t ok = pdFAIL;
        unsigned attempt = 0;
        for (; attempt < sizeof k_stack_try / sizeof k_stack_try[0]; attempt++) {
            ok = xTaskCreate(owf_app_task, "owf", k_stack_try[attempt],
                             NULL, 3, NULL);
            if (ok == pdPASS) break;
            con_puts("!! xTaskCreate failed at stack ");
            con_putdec(k_stack_try[attempt]); con_puts(" words\n");
        }
#if defined(WDOG_TRACE)
        if (ok != pdPASS) {
            con_puts("!! xTaskCreate FAILED at every stack size\n");
            /* Split the failure by CAUSE, not just the fact (2026-08-03: a
             * bare "3 s" reading was ambiguous between this and scheduler-
             * return): heap starved -> ~3.5 s; heap FINE but create failed
             * (internal/port problem) -> ~5.5 s. */
            wdog_extend(xPortGetFreeHeapSize() < (1u << 20) ? 2u : 4u);
            for (;;) { }
        }
        /* 9 = full stack (silent), 35/36 = had to fall back (distinct codes) */
        wdog_stage(attempt == 0 ? 9u : (34u + attempt));
#else
        (void)ok;
#endif
    }
#else
    extern void ui_task(void *);
    xTaskCreate(ui_task,        "ui",        8192, NULL, 3, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 1024, NULL, 2, NULL);
    xTaskCreate(counter_task,   "counter",   256,  NULL, 1, NULL);
#endif

#if defined(PLAT_SOC_MSM)
    /* SAFETY NET (see reboot_msm.c): this watch has no button force-reset and no
     * button route to fastboot, so a hung custom image would otherwise be
     * recoverable only by draining the battery. Arm a dead-man BEFORE the
     * scheduler runs: if nothing kicks it within the timeout, it reboots to the
     * bootloader. During bring-up NOTHING kicks it, so EVERY boot — working or
     * hung — returns to fastboot after 30 s. Raise/kick/disarm once trusted.
     * (xTimerStart before vTaskStartScheduler queues the command; it runs as
     * soon as the timer service task starts.) */
#if !defined(NO_AUTO_REBOOT)
    deadman_arm(30000u);
#else
    con_puts("NO_AUTO_REBOOT: dead-man DISARMED; the watch will never "
             "reboot itself. Force-reboot by hand if it hangs.\n");
#endif
    bootmark(BOOTMARK_SCHED);
#endif /* PLAT_SOC_MSM */

#if defined(WDOG_TRACE)
    wdog_stage(41);          /* everything before the scheduler survived */
#endif
#if defined(STOP_SCHED)
    /* Recovery 3 s bisect, point 2: ALL platform init + task creation done,
     * scheduler NOT started. ~31 s reboot = alive here; ~3 s = killer above. */
    wdog_extend(30u);
    for (;;) { }
#endif
    bdiag_puts("starting scheduler\n");
    vTaskStartScheduler();

    con_puts("!! scheduler returned\n");
#if defined(WDOG_TRACE)
    wdog_extend(7);          /* must never happen; reads ~8.5 s, distinct from
                                both xTaskCreate-fail codes */
#endif
    for (;;) { }
#endif /* SAFETY_TEST */
}
