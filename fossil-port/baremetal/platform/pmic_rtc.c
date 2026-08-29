/* pmic_rtc.c — Qualcomm PMIC RTC over SPMI (PM8916 on the Gen 4, PM660 on the
 * Gen 6 — both use the pm8941-class register layout).
 *
 * The RTC is a free-running 32-bit seconds counter with battery backup. The
 * stock OS (Wear OS / AsteroidOS) keeps it at UTC Unix time, so on a watch
 * that has ever synced with a phone the counter IS the wall clock — reading it
 * at boot gives us real time-of-day with no radio at all.
 *
 * SOURCES:
 *   DTB (both watches): qcom,pm{8916,660}_rtc — rtc rw @0x6000, alarm @0x6100,
 *     on PMIC slave id 0.
 *   Register map from the kernel rtc-pm8xxx.c pm8941_regs (PM8916 and PM660
 *     both bind this layout):
 *       0x6040..0x6043  RTC_WRITE   (seconds, little-endian)
 *       0x6046          RTC_CTRL    bit7 = enable
 *       0x6048..0x604B  RTC_READ    (seconds, little-endian)
 *       0x6140..0x6143  ALARM_RW    (match value, little-endian)
 *       0x6146          ALARM_CTRL  bit7 = alarm enable
 *
 * CAVEATS, stated up front:
 *   - On many production devices the RTC WRITE path is locked down to the
 *     secure world (the kernel driver needs an explicit allow-set-time DT flag
 *     for a reason). rtc_write_epoch() may therefore fail or be silently
 *     ignored on hardware. That is fine: owf_time.cpp keeps the RAM offset as
 *     the source of truth for the session and the RTC write is best-effort.
 *   - Everything here is polled + bounded via spmi_arb.c — a dead arbiter
 *     costs a few ms and returns -1, it can never hang the boot.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#define RTC_SID          0u        /* qcom,pm{8916,660}@0 */

#define RTC_WRITE_BASE   0x6040u
#define RTC_CTRL         0x6046u
#define RTC_READ_BASE    0x6048u

#define RTC_CTRL_ENABLE  (1u << 7)

/* Read the 32-bit seconds counter. Double-read to dodge the ripple-carry
 * window (the four bytes are latched per-byte, not atomically: a read that
 * straddles a second boundary can pair a new LSB with old upper bytes). */
int rtc_read_epoch(uint32_t *sec)
{
    uint8_t a[4], b[4];
    unsigned tries;

    if (spmi_read(RTC_SID, RTC_READ_BASE, a, 4) < 0) return -1;
    for (tries = 0; tries < 3; tries++) {
        if (spmi_read(RTC_SID, RTC_READ_BASE, b, 4) < 0) return -1;
        if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3])
            break;
        a[0] = b[0]; a[1] = b[1]; a[2] = b[2]; a[3] = b[3];
    }
    *sec = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

/* ---- alarm block (rung 2 probe, READ-ONLY) -------------------------------
 * The PMIC RTC alarm is the wake source deep sleep wants: it lives in the
 * always-on domain, so it survives APSS power collapse, and it raises the same
 * PMIC interrupt line the buttons use (spmi periph_irq, GIC SPI 190).
 *
 * Whether we may ARM it is an open question. The counter write at 0x6046 is
 * secure-owned on this unit (see rtc_write_epoch above). The alarm is a
 * SEPARATE peripheral — 0x61xx, therefore a different PPID and a different
 * arbiter APID — so it may well be owned by EE0 even though the counter is
 * not. May. This probe answers it without writing anything: read the alarm
 * registers over the observer path, and ask the arbiter's ownership table who
 * owns the write path. No arming, no TZ-reset risk. */
#define ALARM_RW_BASE    0x6140u
#define ALARM_CTRL       0x6146u
#define ALARM_CTRL_EN    (1u << 7)

/* Read the alarm match value + control byte. 0 ok, -1 SPMI error. */
int rtc_alarm_read(uint32_t *match, uint8_t *ctrl)
{
    uint8_t v[4];
    if (spmi_read(RTC_SID, ALARM_RW_BASE, v, 4) < 0) return -1;
    if (spmi_read8(RTC_SID, ALARM_CTRL, ctrl) < 0) return -1;
    *match = (uint32_t)v[0] | ((uint32_t)v[1] << 8)
           | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    return 0;
}

/* Can we arm the alarm? 1 = yes (EE0 owns it), 0 = no, -1 = unmapped.
 * Read-only: consults the arbiter ownership table, never the alarm itself. */
int rtc_alarm_writable(void)
{
    return spmi_writable(RTC_SID, ALARM_CTRL);
}

/* Set the counter. Kernel-mirrored sequence: disable, write byte0=0 first (so
 * a mid-write carry can't propagate garbage), then bytes 1-3, then the real
 * byte0, then re-enable. Best-effort — see the write-lockdown caveat above. */
int rtc_write_epoch(uint32_t sec)
{
#if defined(PLAT_RTC_WRITE_DISABLED)
    /* Gen 6 (2026-08-03): the RTC write path is secure-world-owned on this
     * unit — the RTC_CTRL write below caused an SPMI arb ownership violation
     * = instant TZ reset ~3 s into every full-firmware recovery boot (found
     * via the VISUAL_TRACE color staircase: died green->yellow, the clock
     * block). Reads are observer-path and stay enabled. The RAM offset in
     * owf_time.cpp remains the session clock; persisting time needs a
     * validated path first. */
    (void)sec;
    return -1;
#else
    uint8_t ctrl, v[4];
    int rc = 0;

    v[0] = (uint8_t)(sec & 0xFF);
    v[1] = (uint8_t)((sec >> 8) & 0xFF);
    v[2] = (uint8_t)((sec >> 16) & 0xFF);
    v[3] = (uint8_t)((sec >> 24) & 0xFF);

    if (spmi_read8(RTC_SID, RTC_CTRL, &ctrl) < 0) return -1;

    rc |= spmi_write8(RTC_SID, RTC_CTRL, (uint8_t)(ctrl & ~RTC_CTRL_ENABLE));
    rc |= spmi_write8(RTC_SID, RTC_WRITE_BASE, 0);
    rc |= spmi_write(RTC_SID, RTC_WRITE_BASE + 1, &v[1], 3);
    rc |= spmi_write8(RTC_SID, RTC_WRITE_BASE, v[0]);
    rc |= spmi_write8(RTC_SID, RTC_CTRL, (uint8_t)(ctrl | RTC_CTRL_ENABLE));
    if (rc) return -1;

    /* Verify the write actually landed (catches the locked-down case, where
     * the arbiter ACKs but the counter never changes). */
    uint32_t back;
    if (rtc_read_epoch(&back) < 0) return -1;
    return (back - sec) <= 2u ? 0 : -1;
#endif /* PLAT_RTC_WRITE_DISABLED */
}

#endif /* PLAT_SOC_MSM */
