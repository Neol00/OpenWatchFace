/* sleep_stats.c — Gen 6 sleep-state accounting + deep-sleep feasibility probe.
 *
 * PURPOSE (rung 2 of the deep-sleep ladder): make every later sleep experiment
 * PASS/FAIL instead of "the battery seemed to last longer". Three read-only
 * data sources, all documented in the device's own DTB, tell us whether the
 * SoC actually reached a low-power state:
 *
 *   qcom,rpm-stats@200000        (sda429-hoki-decompiled.dts:1365)
 *     RPM's own record of system-level sleep: how many times the SoC entered
 *     VDD-min / XO-shutdown and how long it stayed. If our system-pc attempt
 *     (rung 5) works, THIS counter increments. If it does not increment, we
 *     did not sleep, no matter what the current meter says.
 *
 *   qcom,rpm-master-stats@60150  (line 1372)
 *     Per-master accounting; APSS is master 0. numshutdowns is the count of
 *     APSS power collapses — the rung 4 (cpu-pc) and rung 5 success signal.
 *
 *   qcom,mpm2-sleep-counter@4a3000, 32768 Hz (line 5196)
 *     Free-running always-on counter. It keeps ticking while the ARM
 *     architected timer is powered down, so it is both the deep-sleep
 *     timebase and an independent measure of how long we were actually out —
 *     cross-checking it against CNTVCT is how we will detect that the arch
 *     timer stopped (the signature of a real power collapse).
 *
 * HONESTY RULES (same as pwr_diag.c): every decoded number is printed next to
 * the RAW words it came from, so a wrong struct layout is VISIBLE rather than
 * silently plausible. The rpm-stats record layout in particular is version-
 * dependent (qcom,sleep-stats-version = <2> here) and is the part of this file
 * most likely to be wrong on first contact with hardware — hence the hexdump.
 *
 * EVERYTHING HERE IS READ-ONLY. No PMIC writes, no clock writes, no register
 * we do not have a documented reason to touch. This file cannot brick a boot.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

/* ---- MPM2 always-on sleep counter ---------------------------------------- */
#define MPM2_SLEEP_COUNTER  0x004A3000u
#define MPM2_HZ             32768u

uint32_t mpm_sleep_counter(void)
{
    return mmio_read(MPM2_SLEEP_COUNTER);
}

/* ---- RPM stats ------------------------------------------------------------
 * DTB: reg = <0x200000 0x1000  0x290014 0x4  0x29001c 0x4>,
 *      reg-names = "phys_addr_base", "offset_addr", "heap_phys_addrbase".
 * Vendor drivers/soc/qcom/rpm_stats.c, version 2 record:
 *     u32 stat_type;      // FourCC, e.g. 'vmin' / 'xosd' — printable ASCII
 *     u32 count;          // times entered
 *     u64 last_entered_at;
 *     u64 last_exited_at;
 *     u64 accumulated;    // total duration in sleep-clock ticks
 *     u32 client_votes;
 *     u32 reserved[3];
 *  = 48 bytes per record, records laid out back to back at
 *     phys_addr_base + readl(offset_addr).
 * The stat_type FourCC is the self-check: if the first word decodes to four
 * printable ASCII characters, the layout and offset are right. If it does not,
 * the hexdump below is the ground truth and the decode is to be ignored.
 *
 * CONFIRMED ON HARDWARE 2026-08-07, and the self-check did its job: the first
 * cut used 32 (the VERSION 1 record, no client_votes/reserved tail). stat[0]
 * decoded as 'vlow' but stat[1] came out as dots — and the raw dump showed
 * 'vmin' sitting at +16 into it, i.e. the real stride is 48. Baseline counts
 * are both 0, which is correct: nothing has entered a system low-power state
 * yet. These are the two counters rung 5 must move. */
#define RPM_STATS_BASE      0x00200000u
#define RPM_STATS_OFFSET    0x00290014u
#define RPM_STAT_RECORDS    2u
#define RPM_STAT_RECSZ      48u

static void put_fourcc(uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        uint8_t c = (uint8_t)(v >> (i * 8));
        con_putc((c >= 0x20u && c < 0x7Fu) ? (char)c : '.');
    }
}

/* ---- master stats ---------------------------------------------------------
 * DTB: reg = <0x60150 0x5000>, masters "APSS\0MPSS\0PRONTO\0TZ\0LPASS",
 *      qcom,master-offset = <0x1000>, version 2.
 * Vendor rpm_master_stat.c record head:
 *     u32 active_cores; u32 numshutdowns;
 *     u64 shutdown_req; u64 wakeup_ind; u64 bringup_req; u64 bringup_ack;
 * APSS is master index 0. numshutdowns is the number we care about. */
#define RPM_MASTER_BASE     0x00060150u
#define RPM_MASTER_APSS     0u
#define RPM_MASTER_STRIDE   0x1000u

uint32_t rpm_apss_shutdowns(void)
{
    uintptr_t m = RPM_MASTER_BASE + RPM_MASTER_APSS * RPM_MASTER_STRIDE;
    return mmio_read(m + 4u);            /* numshutdowns */
}

/* ---- one-shot feasibility report ------------------------------------------
 * Printed once, a few seconds into the boot, BEFORE anything tries to sleep.
 * This is the rung-2 deliverable: after one flash we know
 *   (a) whether the PMIC RTC alarm can be armed from EE0 (timer wake), and
 *   (b) whether the RPM/MPM counters decode, so rungs 3-5 are measurable.
 * Deferred like pwr_diag's FG read: the SPMI touches here are reads only, but
 * announcing them before they happen keeps the "log ends HERE" diagnostic
 * working if any of it turns out to be hostile after all. */
void sleep_stats_report(void)
{
    diag_puts("\nsleep: === deep-sleep feasibility probe (read-only) ===\n");

    /* -- (a) wake-source ownership. The whole timer-wake design depends on
     *    this one answer, and it costs a table read to get. -- */
    int alarm_ee  = spmi_owner_ee(0u, 0x6146u);   /* RTC alarm ctrl  */
    int ctr_ee    = spmi_owner_ee(0u, 0x6046u);   /* RTC counter ctrl */
    int pon_ee    = spmi_owner_ee(0u, 0x0810u);   /* qpnp-pon RT_STS  */

    diag_puts("sleep: owner EE  rtc_alarm="); diag_putdec((uint32_t)alarm_ee);
    diag_puts(" rtc_ctr=");                   diag_putdec((uint32_t)ctr_ee);
    diag_puts(" pon=");                       diag_putdec((uint32_t)pon_ee);
    diag_puts("   (we are EE0; 0 = writable by us)\n");

    diag_puts("sleep: RTC alarm arming is ");
    switch (rtc_alarm_writable()) {
    case 1:  diag_puts("AVAILABLE -> timer wake via PMIC alarm\n"); break;
    case 0:  diag_puts("DENIED (secure-owned) -> timer wake must use the MPM"
                      " sleep counter instead\n"); break;
    default: diag_puts("UNKNOWN (peripheral not in the arbiter table)\n"); break;
    }

    uint32_t match = 0; uint8_t actl = 0;
    if (rtc_alarm_read(&match, &actl) == 0) {
        diag_puts("sleep: alarm match="); diag_puthex(match);
        diag_puts(" ctrl=");              diag_puthex(actl);
        diag_puts(actl & 0x80u ? " (ENABLED)\n" : " (disabled)\n");
    } else {
        diag_puts("sleep: alarm read FAILED (spmi)\n");
    }

    /* -- (b) do the sleep counters decode? -- */
    uint32_t mpm0 = mpm_sleep_counter();
    uint32_t cnt0 = timer_ms();
    diag_puts("sleep: mpm2 counter="); diag_putdec(mpm0);
    diag_puts(" (");                    diag_putdec(mpm0 / MPM2_HZ);
    diag_puts(" s since PMIC power-on) cntvct=");
    diag_putdec(cnt0); diag_puts(" ms since our boot\n");

    diag_puts("sleep: apss numshutdowns="); diag_putdec(rpm_apss_shutdowns());
    diag_puts("\n");

    uint32_t off = mmio_read(RPM_STATS_OFFSET);
    uintptr_t rec = RPM_STATS_BASE + off;
    diag_puts("sleep: rpm-stats offset="); diag_puthex(off);
    diag_puts(" -> ");                     diag_puthex((uint32_t)rec);
    diag_puts("\n");

    for (unsigned i = 0; i < RPM_STAT_RECORDS; i++) {
        uintptr_t r = rec + i * RPM_STAT_RECSZ;
        uint32_t type  = mmio_read(r + 0u);
        uint32_t count = mmio_read(r + 4u);
        diag_puts("sleep:  stat["); diag_putdec(i); diag_puts("] type=");
        put_fourcc(type);
        diag_puts(" count=");  diag_putdec(count);
        /* accumulated duration, low word — nonzero proves real residency */
        diag_puts(" accum_lo="); diag_puthex(mmio_read(r + 24u));
        diag_puts("  raw:");
        for (unsigned w = 0; w < RPM_STAT_RECSZ / 4u; w++) {
            diag_puts(" "); diag_puthex(mmio_read(r + 4u * w));
        }
        diag_puts("\n");
    }
    diag_puts("sleep: === end probe ===\n");
    diag_flush();
}

/* Compact one-liner for the periodic census — call after each suspend so the
 * counters can be diffed across a sleep. */
void sleep_stats_line(void)
{
    diag_puts("sleep: mpm=");      diag_putdec(mpm_sleep_counter());
    diag_puts(" apss_shutdowns="); diag_putdec(rpm_apss_shutdowns());
    diag_puts("\n");
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
