/* arduino_main.cpp — Arduino setup()/loop() harness for the snapdragon-port runtime.
 *
 * The runtime (main.c) normally spawns ui_demo's ui_task. For the real firmware
 * it instead spawns THIS task, which runs the Arduino contract: setup() once, then
 * loop() forever. OpenWatchFace's loop() already paces itself (lv_timer_handler +
 * its own delays), so we only add a minimum yield so the scheduler can run other
 * tasks (e.g. the dead-man timer service).
 *
 * The runtime's main() creates this via owf_app_task when built with -DOWF_APP.
 */
#include "FreeRTOS.h"
#include "task.h"
#if defined(GLASS_DIAG)
#include <cstdio>
extern "C" void fb_text_dump(const char *s);
extern "C" int  touch_read(unsigned short *x, unsigned short *y);
extern "C" void usb_diag(unsigned int *pc, unsigned int *st,
                         unsigned int *vid, int *cfg);
extern "C" uint8_t g_touch_hostcmd_rb;
extern "C" uint32_t g_touch_last_rpts, g_touch_wdog_cnt;
#endif
#if defined(STOP_VCALL)
#include "HWCDC.h"   /* OwfSerial: the same virtual-print path as USBSerial */
#endif

extern "C" void setup(void);
extern "C" void loop(void);
extern "C" void bootmark(uint32_t);   /* IMEM breadcrumbs — platform.h */
extern "C" void wdog_extend(uint32_t);
extern "C" void con_puts(const char *);
extern "C" void kf_tz_ring_dump(void);
extern "C" void kmsg_forensics_report(void);
extern "C" void deadman_kick(void);
extern "C" int  recovery_gate(void);
extern "C" int  smem_init(void);       /* platform/smem.c — WiFi foundation */
extern "C" uint32_t ddr_size_detect(void); /* platform/ddr_size.c */
extern "C" int  smem_ok(void);
extern "C" void smem_diag_dump(void);
extern "C" void smem_scan_report(void);
extern "C" void scm_diag(void);
extern "C" void rpm_diag(void);
extern "C" void wcnss_boot_diag(void);
extern "C" void fg_diag_text(char *buf, unsigned cap);
extern "C" void dsi_bl_selftest(char *buf, unsigned cap);
#if (defined(FG_DIAG) || defined(BL_TEST)) && !defined(GLASS_DIAG)
extern "C" void fb_text_dump(const char *s);
#endif
extern "C" void fb_idle_refresh(unsigned int max_age_ms);
extern "C" void fb_perf_loop_tick(unsigned int body_ms);
extern "C" void pwr_diag_poll(unsigned int body_ms);
extern "C" void sleep_stats_report(void);
extern "C" void psci_report(void);
extern "C" int  pmic_irq_init(void);
extern "C" void sensor_scan(void);
extern "C" void cpu_pc8909_init(void);
extern "C" void smp_cpu1_test_poll(void);
extern "C" void touch_ft_arm_irq(void);
extern "C" void cpu_pc8909_prev_report(void);
extern "C" void rtc_probe_report(void);
extern "C" void bt_wcn3990_probe(void);   /* Gen 6 BLE bring-up probe */
extern "C" int  bt_probe_step(void);      /* ...stepped: one slice per loop */
extern "C" int  pmic_irq_alarm_init(void);
extern "C" void rtc_alarm_test_start(void);
extern "C" void rtc_alarm_test_poll(void);
extern "C" void cpu_pc_selftest_run(void);
extern "C" void cpu_pc_cpu_on_test(void);
extern "C" void cpu_pc_prev_mark_report(void);
extern "C" void mpm_report(void);
extern "C" unsigned int timer_ms(void);
extern "C" void blackbox_flush(void);
extern "C" void blackbox_sync(void);
extern "C" void blackbox_print_previous(void);
extern "C" void logfile_flush(void);   /* ramlog -> /owf-log.txt on FFat */
extern "C" int  usb_dev_init(void);    /* CDC-ACM log console over USB */
extern "C" void usb_poll(void);
extern "C" int  usb_is_configured(void);
extern "C" void con_flush(void);   /* commit an unterminated log line */
extern "C" void con_puts(const char *);
extern "C" void con_putdec(unsigned int);
extern "C" int  storage_init(void);
extern "C" int  storage_stairs(void);
extern "C" void storage_show_log(unsigned int hold_ms);
extern "C" unsigned int storage_diag_color(void);
extern "C" void fb_trace(unsigned int xrgb);
extern "C" void timer_delay_ms(unsigned int ms);
#if defined(VISUAL_TRACE)
extern "C" void fb_trace(uint32_t);
#endif

#define BOOTMARK_APP   9u
#define BOOTMARK_LOOP 11u

#if defined(BUZZ_TRACE)
/* BUZZ_TRACE — progress reporting through the VIBRATION MOTOR.
 *
 * Earned this on 2026-08-02: the first flash-to-recovery boot produced our
 * sign-of-life buzz, which proves SPMI/PMIC works. That makes the motor the
 * first genuinely reliable output channel this port has ever had (the display
 * is the thing under investigation, this kernel blocks every memory-readback
 * route, and there is no UART pad). So the firmware now COUNTS OUT how far it
 * got, and then keeps a heartbeat going so "alive but not drawing" can be told
 * apart from "hung" — which is exactly the ambiguity of a frozen boot logo.
 *
 *   1 long  (in main)  reached main(): MMU + timer + GIC + SPMI all up
 *   2 short            app task running, setup() about to start
 *   3 short            display bring-up returned OK (framebuffer claimed)
 *   4 short            display bring-up FAILED
 *   5 short            setup() finished, first loop() completed
 *   then 1 short every 3 s = heartbeat: the firmware is RUNNING
 *
 * Silence after N means it hung right after milestone N. */
extern "C" void vib_buzz(unsigned n, uint32_t ms);
extern "C" uint32_t timer_ms(void);
#endif

#if defined(WDOG_TRACE)
/* WDOG_TRACE — report progress by WHEN THE WATCH REBOOTS.
 *
 * The motor is NOT trustworthy as a signal: the bootloader itself buzzes once
 * on the "bootloader is unlocked" screen, so a single buzz proves nothing and
 * SPMI remains unverified. The watchdog, by contrast, is PROVEN — reprogram-
 * ming it visibly moved the reboot from 15 s to 4-5 s.
 *
 * So each milestone sets a LONGER watchdog timeout, and the time from power-on
 * to reboot names the last milestone reached. Stopwatch only; no SPMI, no
 * display, no shell:
 *
 *   reboots ~15 s  -> our code never ran (this is aboot's own 11 s dog)
 *   reboots ~25 s  -> reached main(), hung before the app task
 *   reboots ~40 s  -> app task started, hung inside setup() before display
 *   reboots ~55 s  -> display init returned, hung after it
 *   NEVER reboots  -> reached loop() and keeps petting: THE FIRMWARE IS ALIVE
 */
extern "C" void wdog_stage(unsigned stage);
#endif
/* Outside the WDOG_TRACE guard: the app loop pets the dog in EVERY build now
 * (see the note at the pet call), so the declaration must be unconditional. */
extern "C" void wdog_pet(void);

extern "C" void owf_app_task(void *arg)
{
    (void)arg;
#if defined(WDOG_TRACE)
    wdog_stage(42);                  /* FIRST statement: the task was entered */
#endif
    bootmark(BOOTMARK_APP);            /* scheduler ran and reached our task */
#if defined(BUZZ_TRACE)
    vib_buzz(2, 120);
#endif
#if defined(WDOG_TRACE)
    wdog_stage(10);                  /* the app task RAN; hung in setup() */
#endif
#if defined(STOP_VCALL)
    /* Bisect 4d: the STOP_PRINT park (alive) but printing through the SAME
     * C++ virtual-dispatch path setup()'s banner uses (Print::println ->
     * vtable -> OwfSerial::write). 3 s = the virtual print machinery;
     * ~31 s = it is innocent and wdog_stage(1) is the last suspect. */
    { OwfSerial probe_serial; probe_serial.println("vcall probe"); }
    wdog_extend(30u);
    for (;;) { }
#endif
#if defined(STOP_STAGE1)
    /* Bisect 4e: the STOP_PRINT park (alive) but armed via wdog_stage(1)
     * (the table's 31 s entry) exactly like the parks that died. 3 s = the
     * staircase arming itself misbehaves at stage 1; ~32 s = innocent. */
    con_puts("stage1 probe\n");
    wdog_stage(1);
    for (;;) { }
#endif
#if defined(STOP_PRINT)
    /* Recovery 3 s bisect, point 4c: EXACTLY the STOP_SETUP park that
     * survived 32 s — plus ONE console write first. Dies at 3 s = console
     * output from task context is the killer; ~31 s = console innocent. */
    con_puts("stop-print probe\n");
    wdog_extend(30u);
    for (;;) { }
#endif
#if defined(STOP_SETUP)
    /* Recovery 3 s bisect, point 3: scheduler + app task alive, setup() not
     * entered. ~30 s reboot (wdog->WearOS or deadman->fastboot, whichever
     * first) = alive here; ~3 s = killer is in the scheduler/task machinery. */
    wdog_extend(30u);
    for (;;) { }
#endif
    /* Storage first: eMMC -> GPT("userdata") -> write window -> superblock.
     * Fail-soft (no storage = volatile boot, exactly the old behavior), and
     * from here every con_puts of the boot lands in the blackbox on the
     * first flush. aboot just read this image off the same controller, so
     * the SDHCI is alive by construction. */
#if !defined(STORAGE_STAIRS)
    storage_init();
    /* Crash forensics across a POWER-OFF: replay the previous boot's tail
     * from the eMMC blackbox before this boot's first flush overwrites it. */
    blackbox_print_previous();
#endif
    /* USB device mode: brings up the HS controller and exposes the ramlog as a
     * CDC-ACM serial port, so the log can be read live over the cable with
     * `cat /dev/ttyACM0` instead of squinting at a 416x416 screen with no way
     * to scroll. Fail-soft: if the PHY or controller does not come up we log
     * it once and carry on exactly as before. */
    usb_dev_init();

#if defined(SMEM_DIAG)
    /* SMEM header probe -- the first step of the WiFi stack (platform/smem.c).
     * v83 (2026-09-06): back to EXACTLY the v77 form. ddr_size_detect() is
     * not called at boot until the v79+ boot failure is understood; the
     * About screen falls back to the board header's DDR size. */
    if (smem_init() == 0) con_puts("smem: header valid\n");
    smem_diag_dump();
#endif

    setup();

    /* THE WAY BACK TO FASTBOOT (see platform/recovery_gate.c). Runs once,
     * immediately after setup(), which is the earliest point where both the
     * display and touch are up. Touching the screen during the window reboots
     * to the bootloader.
     *
     * This is the ONLY route to fastboot from a HEALTHY firmware on this
     * watch: the buttons do not reach the bootloader, and the dead-man timer
     * only fires when the firmware is hung. Do not remove it from any build
     * that is going to be FLASHED — without it, a working flashed image can
     * never be replaced. */
    recovery_gate();

#if defined(BL_TEST)
    /* Brightness sweep on a white screen, independent of the UI. See
     * platform/dsi_panel.c. */
    {
        static char blbuf[256];
        fb_trace(0xFFFFFFu);              /* full white, held by the panel */
        dsi_bl_selftest(blbuf, sizeof blbuf);
        con_puts(blbuf); con_puts("\n");
        fb_text_dump(blbuf);
        timer_delay_ms(10000);
    }
#endif

#if defined(BL_TEST)
    /* Brightness sweep on a white screen, independent of the UI and the
     * slider. See platform/dsi_panel.c. */
    {
        static char blbuf[256];
        fb_trace(0xFFFFFFu);              /* full white, held by the panel */
        dsi_bl_selftest(blbuf, sizeof blbuf);
        con_puts(blbuf); con_puts("\n");
        fb_text_dump(blbuf);
        timer_delay_ms(10000);
    }
#endif

#if defined(FG_DIAG)
    /* Raw VM-BMS register dump on the glass for 10 s (platform/pmic_fg.c).
     * The Gen 4 has no log path, so this is how the gauge reports whether its
     * SPMI reads land and whether the block is even enabled. */
    {
        static char fgbuf[256];
        fg_diag_text(fgbuf, sizeof fgbuf);
        con_puts(fgbuf); con_puts("\n");
        fb_text_dump(fgbuf);
        timer_delay_ms(10000);
    }
#endif

#if defined(STORAGE_STAIRS)
    /* storage bring-up as a post-setup color staircase — see storage_gen6.c.
     * On failure: the ramlog tail AS TEXT on the glass (dbg_console.cpp) —
     * the driver's own narration, one photo = full transcript. */
    if (storage_stairs() < 0) storage_show_log(45000u);
#endif
#if defined(STORAGE_DIAG)
    /* Replay the recorded storage verdict now that the display provably
     * works (see storage_gen6.c color table). One color, ~1 s. */
    {
        unsigned int c = storage_diag_color();
        if (c) { fb_trace(c); timer_delay_ms(700); }
    }
#endif
#if defined(VISUAL_TRACE)
    /* M7 WHITE: setup() returned; about to enter loop() (PON digitalRead over
     * SPMI, lv_timer_handler, wdog_pet are the first-ever code from here). */
    fb_trace(0xFFFFFFu);
#endif
#if defined(GLASS_DIAG)
    /* GLASS_DIAG — the debug channel that needs NO log pull and NO adb: the
     * watchface UI is replaced by a live diagnostic screen painted with
     * fb_text_dump. Touch is polled directly (we are the only reader) and
     * every press/release is measured HERE, independent of LVGL; USB state
     * is snapshotted from the controller. Photograph the screen = full
     * report. */
    {
        static char buf[512];
        unsigned short tx = 0, ty = 0;
        int  prev_down = 0;
        uint32_t press_t = 0, last_dur = 0, ntaps = 0, nerrs = 0;
        uint32_t last_paint = 0;
        for (;;) {
            int rd = touch_read(&tx, &ty);
            if (rd < 0) nerrs++;
            int down = (rd == 1);
            uint32_t now = timer_ms();
            if (down && !prev_down) press_t = now;
            if (!down && prev_down) { last_dur = now - press_t; ntaps++; }
            prev_down = down;

            usb_poll();

            if ((uint32_t)(now - last_paint) >= 500u) {
                last_paint = now;
                unsigned int pc, st, vid; int cfg;
                usb_diag(&pc, &st, &vid, &cfg);
                snprintf(buf, sizeof buf,
                    "OWF GLASS DIAG\n"
                    "UP: %u S\n"
                    "\n"
                    "TOUCH DN:%d\n"
                    "X:%u Y:%u\n"
                    "HELD: %u MS\n"
                    "LAST TAP: %u MS\n"
                    "TAPS:%u ERR:%u\n"
                    "RPTS:%u WDG:%u\n"
                    "HOSTCMD: %02X\n"
                    "\n"
                    "USB CFG:%d\n"
                    "CCS:%u SPD:%u\n"
                    "PTS:%u PHCD:%u\n"
                    "PORTSC:%08X\n"
                    "STS:%08X\n"
                    "ULPI VID:%04X\n",
                    (unsigned)(now / 1000u),
                    down, tx, ty,
                    down ? (unsigned)(now - press_t) : 0u,
                    (unsigned)last_dur, (unsigned)ntaps, (unsigned)nerrs,
                    (unsigned)g_touch_last_rpts, (unsigned)g_touch_wdog_cnt,
                    g_touch_hostcmd_rb,
                    cfg, pc & 1u, (pc >> 24) & 3u, (pc >> 30) & 3u,
                    (pc >> 23) & 1u, pc, st, vid);
                fb_text_dump(buf);
            }
            wdog_pet();      /* same reason as the main loop below */
            deadman_kick();
            blackbox_flush();
            logfile_flush();
            con_flush();
            timer_delay_ms(10);
            vTaskDelay(1);
        }
    }
#endif /* GLASS_DIAG */
    bool first = true;
#if defined(BUZZ_TRACE)
    uint32_t last_beat = 0;
#endif
    for (;;) {
        {
            /* PERF_BARS discriminator: how long did the BODY take vs how
             * often do bodies RUN? (fb_splash.c draws both on the glass) */
            uint32_t bt0 = timer_ms();
            loop();
            uint32_t body = timer_ms() - bt0;
            fb_perf_loop_tick(body);
#if defined(BT_PROBE) && defined(PLAT_BOARD_FOSSIL_GEN6)
            /* Gen 6 Bluetooth bring-up, one slice per loop iteration. The
             * blocking version starved usb_poll() below for ~25 s and the
             * console host dropped the device; this keeps the loop turning. */
            {
                static bool s_bt_done = false;
                /* 25 s, not 8: let the watch finish booting and settle so a
                 * reset during bring-up is unambiguously the bring-up. */
                if (!s_bt_done && timer_ms() >= 25000u) s_bt_done = (bt_probe_step() == 1);
            }
#endif
            /* Power/thermal/clock census — feeds the Power app's CPU% on
             * every MSM watch. The Gen 6 implements it in pwr_diag.c; the
             * msm8909w watches implement the load half in gen4_stubs.c. Both
             * take the loop-body time measured just above, so the call is
             * unconditional and only the implementation differs. */
            pwr_diag_poll(body);
#if defined(PLAT_BOARD_FOSSIL_GEN6)
            /* One-shot deep-sleep feasibility probe (platform/sleep_stats.c).
             * Deferred to 8 s like pwr_diag's first FG read: everything it
             * touches is read-only, but keeping it clear of the boot means a
             * log that ends mid-probe still names the probe as the last thing
             * running. Answers, in one flash: can we arm the PMIC RTC alarm
             * (timer wake), and do the RPM/MPM sleep counters decode. */
            {
                static bool s_sleep_probed = false;
                if (!s_sleep_probed && timer_ms() >= 8000u) {
                    s_sleep_probed = true;

                    /* DIAGNOSTICS ONLY (-DSLEEP_DIAG). These are gated at the
                     * CALL SITE, not just at their printing, because two of
                     * them have real side effects: the selftest runs the whole
                     * warm-boot restore path with the MMU off, and the CPU_ON
                     * test leaves cpu1 powered and parked in WFI forever. Fine
                     * while investigating, pure waste in a normal build. */
#if defined(SLEEP_DIAG)
                    /* What did the LAST boot's collapse actually reach?
                     * Read IMEM before anything overwrites it. */
                    cpu_pc_prev_mark_report();
                    sleep_stats_report();
#endif
                    /* First SMC this port has ever executed. Runs here, well
                     * before any sleep, so CPU_SUSPEND is never called until
                     * PSCI has been proven to answer. */
                    psci_report();
#if defined(SENSOR_SCAN)
                    sensor_scan();          /* platform/sensor_scan.c */
#endif
                }
                {
                    /* Arm the always-on wake source. Runs after the GIC and
                     * SPMI are known good, and alongside (not instead of) the
                     * existing poll — see platform/pmic_irq.c. */
                    pmic_irq_init();
#if defined(SLEEP_DIAG)
                    /* Validate the warm-boot restore path while the core is
                     * still powered — see platform/cpu_pc.c. */
                    cpu_pc_selftest_run();
                    /* Can TZ start a core at an address we choose at all? */
                    cpu_pc_cpu_on_test();
                    mpm_report();
#endif
                }
            }
#elif defined(PLAT_BOARD_FOSSIL_GEN4) || defined(PLAT_BOARD_TICWATCH_C2)
            /* 8909w one-shot at 8 s (2026-09-03). Until now this block was
             * Gen 6 only, so pmic_irq_init() never ran here and a button
             * press during a battery sleep was only noticed by the 15 s
             * SPMI poll. Same GIC SPI 190 chain on the PM8916. No PSCI probe:
             * the 8909w has none and its TZ takes a different SMC set. */
            {
                static bool s_once = false;
                if (!s_once && timer_ms() >= 20000u) {   /* after the WiFi bring-up settles */
                    s_once = true;
                    pmic_irq_init();
                    touch_ft_arm_irq();     /* gpio13 edge latch: no more blind touch reads */
                    rtc_probe_report();     /* deep-sleep gate: can EE0 arm the PMIC RTC alarm? (read-only) */
                    pmic_irq_alarm_init();  /* RTC alarm IRQ routed; the self-test that armed it is retired (proven v110) */
#if defined(USE_CPU_PC_8909)
                    cpu_pc8909_prev_report();
                    cpu_pc8909_init();      /* SPM sequences + warm-boot address + wake frame */
#endif
#if defined(SENSOR_SCAN)
                    sensor_scan();          /* platform/sensor_scan.c */
#endif
                }
#if !defined(NO_SMP_CPU1)
                if (s_once) smp_cpu1_test_poll();   /* platform/smp_8909.c: boots CPU1 (proven v72), keeps it parked with idle accounting */
#endif
            }
#endif
#if defined(TOUCH_LOG)
            /* Loop timing, once a second, to sit alongside the TPOLL lines in
             * the same log. touch_read() runs inside lv_timer_handler() inside
             * loop(), so THIS is the ceiling on how often touch can be
             * sampled: if body_ms is ~2000 then touch is polled ~0.5x/s and a
             * quick tap cannot possibly be seen as quick. */
            {
                static uint32_t s_n, s_sum, s_max, s_win, s_prev;
                uint32_t t = timer_ms();
                uint32_t period = s_prev ? (t - s_prev) : 0u;
                s_prev = t;
                s_n++; s_sum += body; if (body > s_max) s_max = body;
                if (!s_win) s_win = t;
                if ((uint32_t)(t - s_win) >= 10000u) {  /* 10 s */
                    con_puts("LOOP t="); con_putdec(t);
                    con_puts(" iters="); con_putdec(s_n);
                    con_puts(" avgbody="); con_putdec(s_n ? s_sum / s_n : 0u);
                    con_puts(" maxbody="); con_putdec(s_max);
                    con_puts(" lastperiod="); con_putdec(period);
                    con_puts("\n");
                    s_n = 0; s_sum = 0; s_max = 0; s_win = t;
                }
            }
#endif
        }
        if (first) {
            bootmark(BOOTMARK_LOOP);   /* setup() done */
#if defined(BUZZ_TRACE)
            vib_buzz(5, 120);
#endif
            first = false;
        }
#if defined(BUZZ_TRACE)
        /* Heartbeat: proves the loop keeps turning even if nothing is drawn. */
        if ((uint32_t)(timer_ms() - last_beat) >= 3000u) {
            last_beat = timer_ms();
            vib_buzz(1, 60);
        }
#endif
    /* PET THE APPS WATCHDOG — UNCONDITIONALLY, not only under WDOG_TRACE.
     *
     * This used to sit behind `#if defined(WDOG_TRACE)`, which was correct
     * while WDOG_TRACE builds were the only ones anyone ran: during bring-up
     * the whole point is that the dog bites, because the time-to-reboot is the
     * only channel this watch has for saying how far it got.
     *
     * In a NORMAL build the effect was a hard reset roughly every 31 seconds.
     * main() arms the dog with wdog_extend(120), which clamps to WDOG_MAX_SEC
     * = 31 (the bite register is 20 bits at 32765 Hz, so ~32 s is the ceiling
     * and larger values silently truncate) — and then nothing ever petted it.
     * A perfectly healthy watch rebooted itself on a timer, with no crash and
     * nothing to find.
     *
     * A healthy loop() must pet the dog. Hang recovery is not lost: a real
     * hang stops this pet AND deadman_kick() below, so both recovery paths
     * still fire. Bring-up builds that WANT the reboot use the wdog_stage()
     * ladder, which reprograms the window from a known milestone, or
     * -DNO_AUTO_REBOOT to disarm entirely. */
        wdog_pet();
        /* 2026-08-03, FIRMWARE PROVEN ALIVE: a healthy loop() must also kick
         * the 30 s dead-man (bring-up left it un-kicked BY DESIGN so every
         * boot returned to the bootloader — that design phase is over). A
         * real hang still stops both this kick and wdog_pet -> the watch
         * self-recovers. */
        deadman_kick();
        /* Anti-drift: the DDIC walks its addressing when left frame-less
         * (see fb_idle_refresh) — keep it fed at >= 2 Hz.
         * FB_IDLE_MS override (TOUCH-SYNC EXPERIMENT 2026-08-06): the Raydium
         * is on-cell and may sync its scan to the display frame rate. Stock
         * refreshes ~45 fps, we idle at 2 fps — suspiciously close to the
         * chip's observed one-report-per-2.5s cadence. Build with
         * -DFB_IDLE_MS=33 to force ~30 fps and see if touch cadence follows. */
#ifndef FB_IDLE_MS
#define FB_IDLE_MS 500u
#endif
        fb_idle_refresh(FB_IDLE_MS);
        /* Blackbox: mirror the ramlog to eMMC (rate-limited internally; a
         * no-op until storage_init() has run and armed the write window). */
        blackbox_flush();
        /* Mirror the ramlog into /owf-log.txt so the log is readable in the
         * Files app (and, later, over USB). Rate-limited internally; a no-op
         * until FFat is mounted, and sticky-off if it ever fails. */
        logfile_flush();
        /* Pump the USB stack and push any new ramlog bytes down the cable.
         * One register read when no host is attached, and it never blocks on
         * a host that is not listening. */
        usb_poll();
        con_flush();   /* so a hang mid-line still leaves that line behind */
#if defined(SMEM_SCAN)
        /* The risky SMEM probe runs HERE, once, and never earlier.
         * usb_is_configured() means a host has enumerated us and is reading,
         * so each probed address reaches the cable BEFORE the read that might
         * not return. Run at ~1 s instead (where it used to be) the watch
         * bootloops in total silence -- which is exactly what happened twice.
         * The extra 3 s is slack for the host to open the tty after
         * enumerating. */
        {
            /* HOLD-OFF (2026-09-03): the risky diagnostics used to fire the
             * instant USB enumerated, so a reset there hit before the host
             * could even open the tty -- nothing, not even the blackbox
             * replay printed at boot, was readable. Now they wait 30/35 s
             * after enumeration: open the console, read the boot log and the
             * previous boot's replay, then watch the live lines. */
            static int s_smem_scanned = 0;
            static uint32_t s_usb_cfg_ms = 0;
            if (!s_usb_cfg_ms && usb_is_configured()) {
                s_usb_cfg_ms = timer_ms() ? timer_ms() : 1u;
                con_puts("diag: USB up -- SMEM scan / WCNSS test in 3 s\n");
            }
            if (!s_smem_scanned && s_usb_cfg_ms && timer_ms() - s_usb_cfg_ms > 3000u) {
                s_smem_scanned = 1;
                smem_scan_report();
            }
        }
#endif
#if defined(KMSG_FORENSICS)
        /* Warm-reboot forensics: print TrustZone's ring (stale entries from
         * the previous boot included) and the previous kernel's log lines.
         * Nothing here touches WCNSS or TrustZone state. */
        {
            static int s_kf_ran = 0, s_kf_banner = 0;
            static uint32_t s_kf_cfg_ms = 0;
            if (!s_kf_banner) { s_kf_banner = 1; con_puts("*** FORENSICS IMAGE v3: waiting for USB, dump 3 s after the host connects ***\n"); }
            if (!s_kf_cfg_ms && usb_is_configured()) {
                s_kf_cfg_ms = timer_ms() ? timer_ms() : 1u;
                con_puts("diag: USB up -- forensics dump in 3 s, keep the console open\n");
            }
            if (!s_kf_ran && s_kf_cfg_ms && timer_ms() - s_kf_cfg_ms > 3000u) {
                s_kf_ran = 1;
                wdog_extend(60u); deadman_kick();
                kf_tz_ring_dump();
                kmsg_forensics_report();
            }
        }
#endif
#if defined(WIFI_DIAG)
        /* Steps 1+2 of WIFI-BRINGUP.md, same late slot and for the same
         * reason: TZ and the RPM can both answer with a reset, and a reset
         * before USB is up erases its own evidence. */
        {
            static int s_wifi_diag_ran = 0;
            static uint32_t s_wifi_cfg_ms = 0;      /* see the hold-off note above */
            if (!s_wifi_cfg_ms && usb_is_configured()) s_wifi_cfg_ms = timer_ms() ? timer_ms() : 1u;
            if (!s_wifi_diag_ran && s_wifi_cfg_ms && timer_ms() - s_wifi_cfg_ms > 3000u) {
                s_wifi_diag_ran = 1;
                con_puts("diag: WCNSS test starting now\n");
                wdog_extend(60u);
                deadman_kick();
                /* SMEM is the foundation of everything below (RPM channel
                 * lookup, WCNSS). It used to be initialised only under
                 * SMEM_DIAG; a WIFI_DIAG-only build then failed with
                 * "smd: no info/fifo items for cid 4" (seq v13, 2026-09-02). */
                if (!smem_ok())
                    con_puts(smem_init() == 0 ? "smem: header valid\n" : "smem: init FAILED\n");
                blackbox_sync();
                scm_diag();   /* per-board dispatch lives in scm.c */
                blackbox_sync();
                rpm_diag();
                blackbox_sync();
                wcnss_boot_diag();
                blackbox_sync();
            }
        }
#endif
        vTaskDelay(1);   /* yield a tick so lower-prio tasks (dead-man) run */
    }
}
