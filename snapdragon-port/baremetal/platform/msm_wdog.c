/* msm_wdog.c — the Qualcomm APPS watchdog (qcom,msm-watchdog @ 0xb017000).
 *
 * WHY THIS EXISTS — THE 15-SECOND MYSTERY, SOLVED ON THE FIRST REAL BOOT:
 * aboot hands over with this watchdog ARMED (DTB: qcom,bark-time = 0x2af8 =
 * 11000 ms). The stock kernel's watchdog_v2 driver takes it over and pets it;
 * our image did neither, so every boot died at bark(11 s)+bite and warm-reset
 * into the STOCK boot chain — observed on hardware as "fossil logo for ~15 s,
 * then reboots into Wear OS", on every image, regardless of what the code did.
 *
 * It has since become the port's ONLY reliable debug channel: this watch has
 * no UART pad, its Linux is built CONFIG_DEVMEM=n/CONFIG_PSTORE=n so no memory
 * can be read back, and the display is the thing under investigation. So each
 * boot milestone sets a distinct timeout and a stopwatch reading names the
 * last one reached (wdog_stage below).
 *
 * Registers from the vendor kernel drivers/soc/qcom/watchdog_v2.c:
 *   base + 0x04  WDT0_RST        write 1 = pet
 *   base + 0x08  WDT0_EN         bit 0 = enable
 *   base + 0x10  WDT0_BARK_TIME
 *   base + 0x14  WDT0_BITE_TIME
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#define WDOG_BASE      0x0B017000u
#define WDT0_RST       0x04u
#define WDT0_EN        0x08u
#define WDT0_BARK_TIME 0x10u
#define WDT0_BITE_TIME 0x14u

/* Counter runs off the 32 kHz sleep clock (vendor driver: WDT_HZ = 32765). */
#define WDT_HZ         32765u

/* HARD LIMIT — learned the expensive way on 2026-08-03.
 * WDT0_BARK_TIME/WDT0_BITE_TIME are 20-BIT registers. At 32765 Hz that caps a
 * timeout at 0xFFFFF / 32765 = ~32 s, and anything larger is silently
 * TRUNCATED, not clamped: a 45 s request became exactly 13 s, which read back
 * as the ~15 s "our code never ran" baseline and sent this investigation
 * chasing ghosts for several test cycles. Clamp so that can never recur. */
#define WDOG_MAX_SEC   31u

/* SLOW-CLOCK LATCH SYNC (2026-08-03, the flaky-readings root cause). This
 * block runs on the 32 kHz sleep clock; a register write needs several slow
 * clock cycles (~100+ us) to synchronize into that domain. Firing EN=0 /
 * BARK / BITE / EN=1 / RST back-to-back at CPU speed means writes latch only
 * SOMETIMES — a re-arm that silently drops its RST leaves the OLD countdown
 * running, so the bite time reflects a stale stage: readings 24/24/31 s from
 * near-identical images, and every "it booted deeper yesterday" contradiction.
 * The vendor driver syncs after every write; do the same (~180 us each). */
static void wd_sync(void)
{
    uint64_t t0 = timer_ticks();
    uint32_t n  = timer_freq_hz() / 5500u;       /* ~6 sleep-clock cycles */
    while ((uint64_t)(timer_ticks() - t0) < n) { }
}

/* PREFERRED during bring-up: keep the dog, just make its window long, so a
 * hang still recovers the watch by itself instead of needing a force-reboot. */
void wdog_extend(uint32_t sec)
{
    uint32_t ticks;
    if (sec == 0) return;        /* never program a zero bite time: that is an
                                    instant reset, not "disabled" */
    if (sec > WDOG_MAX_SEC) sec = WDOG_MAX_SEC;
    ticks = sec * WDT_HZ;
    mmio_write(WDOG_BASE + WDT0_EN, 0);          /* pause while reprogramming */
    wd_sync();
    mmio_write(WDOG_BASE + WDT0_BARK_TIME, ticks);
    mmio_write(WDOG_BASE + WDT0_BITE_TIME, ticks);
    wd_sync();
    /* ORDER MATTERS: enable FIRST, then pet — the vendor driver does exactly
     * this, and a RST written while the block is disabled does not restart the
     * counter. */
    mmio_write(WDOG_BASE + WDT0_EN, 1);
    wd_sync();
    mmio_write(WDOG_BASE + WDT0_RST, 1);         /* restart the countdown */
    __asm__ volatile("dsb sy" ::: "memory");
    wd_sync();
}

/* Pet the dog (restart its countdown) — call from long-running loops. */
void wdog_pet(void)
{
    mmio_write(WDOG_BASE + WDT0_RST, 1);
}

/* ---- boot-progress reporting by reboot TIMING ----------------------------
 * Each milestone sets a distinct timeout; the stopwatch reading from power-on
 * names the last milestone reached. A stage mapped to 0 leaves the watchdog
 * alone (used for milestones already proven on hardware, so the whole usable
 * range can be spent on whatever is still unknown). */
#if defined(DISPLAY_BISECT)
/* FRONTIER TABLE (2026-08-03, rev 3). Everything up to and including the
 * display bring-up is PROVEN on hardware, so those stages are 0 = "do not
 * touch the watchdog"; the whole range serves the remaining unknown: the rest
 * of setup() and the first loop().
 *
 * DECODE RULE: stopwatch reading ~= (when the marker fired) + (armed value).
 * Frontier markers fire ~3-5 s after power-on; the heap probe fires ~1 s in.
 * rev 3 exists because rev 2 had two reading COLLISIONS, both hit on hardware:
 *   - 32 armed 12 s -> fired at 15-17 s = aboot's ~15 s "never ran" baseline
 *   - 34 (21 s -> 24-26 s) vs 37 (24 s at ~1 s -> ~25 s): a 26 s reading was
 *     ambiguous between "setup COMPLETE, first loop() hung" and "hang in the
 *     proven region" - the 2026-08-03 26 s measurement, which forced this
 *     re-cut instead of yielding an answer.
 * Expected readings now (all reboot into WEAR OS; the deadman's ~30 s return
 * to FASTBOOT means a healthy loop - destination disambiguates):
 *   ~6 halt=heap malloc failed | 7-9 after 30 | 11-13 after 31 | 18-20 after
 *   32 | 22-24 after 33 | 26-28 SETUP DONE, loop() hung | ~29 heap small |
 *   ~31 hang in the proven region (stage 1 or 37 armed) */
/* rev 4 (2026-08-03, after the 26 s + 32 s pair): those two runs TOGETHER
 * prove the heap probe passes and the hang sits between the app task starting
 * and OWF_STAGE(30) - i.e. inside "proven" setup(). But that proof came from
 * the 00:59 binary; the .ino (01:04) and main.c (01:16) changed since, so the
 * region is suspect again and gets fine markers back. Frontier 30-34 are
 * compressed into a muddy ~27-32 s band on purpose - they will not fire while
 * the hang is upstream; re-cut when setup() completes again. */
static const uint8_t k_stage_sec[] = {
    /* 0 unused */ 0,
    /* 1  */ 31,   /* main() entered            (pre-probe hang ~31) */
    /* 2-8   proven: MMU, GIC, SPMI */
             0,0,0,0,0,0,0,
    /* 9  */ 25,   /* xTaskCreate returned OK    (reads ~27-28 s)    */
    /* 10 */  5,   /* app task RAN               (reads ~6 s)        */
    /* 11 */  0,   /* reached display block                          */
    /* 12 */ 17,   /* fb_init returned           (reads ~20 s)       */
    /* 13 */  2,   /* xTaskCreate FAILED                             */
    /* 14 */  3,   /* scheduler returned                             */
    /* 15 */  0,   /* USBSerial + banner                             */
    /* 16 */  0,   /* settings/weather/timer load                    */
    /* 17 */  7,   /* haptics_init done          (reads ~8-9 s)      */
    /* 18 */  0,   /* health/calib load                              */
    /* 19 */  9,   /* overclock_check done       (reads ~11 s)       */
    /* 20 */  0,   /* about to Wire.begin                            */
    /* 21 */ 11,   /* Wire.begin survived        (reads ~13 s)       */
    /* 22 */ 15,   /* board_power_begin done     (reads ~17-18 s)    */
    /* 23 */  0,   /* rails_load_state                               */
    /* 24-27 fb_init internals + lv_init */ 0,0,0,0,
    /* 28 */ 19,   /* display created + bound    (reads ~22 s)       */
    /* 29 */ 21,   /* touch_init returned        (reads ~24-25 s)    */
    /* ---- frontier rev 7 (2026-08-03): fastboot-boot runs reach ~23-24 s =
     * the stage-29/30 zone; spread fine markers across it. Fire ~2.5-3 s. -- */
    /* 30 */  4,   /* sd_set_bus_ready           (reads ~7 s)        */
    /* 31 */  6,   /* settings_apply_brightness  (reads ~9 s)        */
    /* 32 */  8,   /* pre drain_diag_boot_report (reads ~11 s)       */
    /* 33 */ 11,   /* diag report done           (reads ~14 s)       */
    /* 34 */ 14,   /* setup() COMPLETE           (reads ~17 s)       */
    /* 35 */  0,   /* stack fallback 8192 (boot continues; silent)   */
    /* 36 */  0,   /* stack fallback 4096 (boot continues; silent)   */
    /* ---- direct heap probe (main.c); 38/39 halt, 37/40 continue --------- */
    /* 37 */ 29,   /* heap HEALTHY - covers probe->task-start (~30)  */
    /* 38 */  2,   /* 64 KB malloc failed: halts (reads ~3, fatal)   */
    /* 39 */  2,   /* 4 KB malloc failed: halts  (reads ~3, fatal)   */
    /* 40 */ 29,   /* free count small - treated like 37   (~30)     */
    /* ---- rev 5: bisecting the probe -> task-start window ---------------- */
    /* 41 */ 21,   /* about to vTaskStartScheduler (reads ~23-24 s)  */
    /* 42 */ 17,   /* app task ENTERED, pre-buzz   (reads ~19-20 s)  */
    /* ---- rev 6: inside xPortStartScheduler ------------------------------ */
    /* 43 */  9,   /* port asserts PASSED, tick config entered (~12) */
    /* 44 */ 14,   /* tick armed; died in first ctx switch     (~17) */
    /* ---- rev 7b: splitting the 22-23 s zone ----------------------------- */
    /* 45 */ 25,   /* lv_indev created, in touch_init  (reads ~27.5) */
    /* 46 */ 23,   /* lv_indev_create returned         (reads ~25.5) */
    /* 47 */ 27,   /* set_type returned                (reads ~29.5) */
};
#else
static const uint8_t k_stage_sec[] = {
    /* 0 unused */ 0,
    /* 1  */  4,   /* main() entered, before the MMU                */
    /* 2  */  6,   /* L1 page table built                           */
    /* 3  */  8,   /* MMU + caches ON, executing translated         */
    /* 4  */ 10,   /* mmu_enable_flat() returned                    */
    /* 5  */ 12,   /* uart_init survived (no-op on this board)      */
    /* 6  */ 18,   /* ramlog_init survived                          */
    /* 7  */ 20,   /* gic_init survived                             */
    /* 8  */ 22,   /* vib_init + vib_buzz + SPMI read survived      */
    /* 9  */ 24,   /* xTaskCreate returned OK                       */
    /* 10 */ 26,   /* app task RAN (setup() starting)               */
    /* 11 */ 28,   /* setup() reached the display block             */
    /* 12 */ 30,   /* fb_init() returned                            */
    /* 13 */  2,   /* xTaskCreate FAILED (heap) - unmistakably fast */
    /* 14 */  3,   /* vTaskStartScheduler returned - must not happen*/

    /* ---- markers INSIDE the firmware's setup() (OWF_STAGE in the .ino) -----
     * 26 s ("app task ran, hung in setup()") was as deep as the trace could
     * see, and setup() is hundreds of lines, so it is bisected here. These are
     * reached ~2-3 s into the boot, so the stopwatch reading is roughly the
     * value below PLUS ~3 s — still unambiguous because nothing else in the
     * table sits within 2 s of them once stages 1-10 have been passed. */
    /* 15 */  5,   /* USBSerial.begin + banner                      */
    /* 16 */  7,   /* settings_load / weather_load / timer_load      */
    /* 17 */  9,   /* haptics_init (drives the motor pin -> SPMI)    */
    /* 18 */ 11,   /* health_load + calib_load                       */
    /* 19 */ 13,   /* overclock_check_recovery                       */
    /* 20 */ 17,   /* ABOUT to call Wire.begin -> the I2C controller */
    /* 21 */ 19,   /* Wire.begin SURVIVED                            */
    /* 22 */ 21,   /* board_power_begin (fuel gauge over SPMI)       */
    /* 23 */ 23,   /* rails_load_state                               */
    /* ---- inside fb_init() (the current frontier) ------------------------ */
    /* 24 */ 25,   /* fb_init entered                                */
    /* 25 */ 27,   /* geometry settled, about to write the buffer    */
    /* 26 */ 29,   /* framebuffer CLEARED (the suspect write passed) */
};
#endif /* DISPLAY_BISECT */

void wdog_stage(unsigned stage)
{
    uint8_t sec;
    if (stage == 0 || stage >= sizeof k_stage_sec) return;
    sec = k_stage_sec[stage];
    /* 0 means "this milestone is already proven — leave the watchdog alone".
     * MUST be checked here: wdog_extend(0) programs a ZERO bite time, i.e. an
     * instant reset. That bug made every boot die ~2 s in (at stage 2, the
     * first zero entry) and produced two worthless test cycles on hardware. */
    if (sec == 0) return;
    wdog_extend(sec);
}

void wdog_disable(void)
{
    mmio_write(WDOG_BASE + WDT0_RST, 1);   /* pet once: full window if the
                                              disable were ever ignored */
    wd_sync();
    mmio_write(WDOG_BASE + WDT0_EN, 0);
    __asm__ volatile("dsb sy" ::: "memory");
    wd_sync();
}

#endif /* PLAT_SOC_MSM */
