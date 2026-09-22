/* sleep_floor.c — what still burns power INSIDE the collapsed window on the
 * msm8909w watches, and the knobs that do not need TrustZone (2026-09-08).
 *
 * THE NUMBER: with CPU0 and CPU1 collapsed, the radio PAS-shut, the panel in
 * sleep-in, every AP clock branch we own gated and the cluster on the crystal,
 * the C2 still lost 4.18 -> 3.90 V in 2 h 30 (v152/v162) -- the same slope as
 * the bare no-watchdog collapse. ~35 mA. Every AP-visible register is the same
 * before and after a collapse (v141 census), so the cores are off. The stock
 * kernel reaches ~1 mA in suspend because the RPM applies the APSS SLEEP SET
 * (XO shutdown, VDD-min, DDR self-refresh); every attempt at that handshake
 * (SYS_PC_8909 stages 0-5) reset at wake before TZ re-entered us.
 *
 * WHAT THIS FILE IS ABOUT: the RPM's ACTIVE set is applied while the APSS is
 * considered awake, and nothing in this firmware ever changed it. It is still
 * whatever SBL1/aboot's boot-time requests left in it:
 *   - the DDR/BIMC clock at its boot rate, no bandwidth vote ever lowered it
 *     (we even hold a 393 MB/s EBI vote from the PAS bring-up, never released);
 *   - VDD_CX (pm8916 s1) and VDD_MX (l3) at their boot corner;
 *   - every LDO the SBL turned on -- the stock kernel switches the ones with
 *     no consumer off 30 s after boot (regulator_init_complete); we never did.
 *     On the C2's own tree l1, l4, l10, l12, l14 have NO consumer at all;
 *   - GPLL1 / GPLL2 (912 MHz) and the crypto/prng branch clocks voted on by
 *     the bootloader through APCS_GPLL_ENA_VOTE / APCS_CLOCK_BRANCH_ENA_VOTE;
 *   - the USB PHY: never put in PORTSC.PHCD low-power, so its 480 MHz PLL runs
 *     with no cable attached, and its 3.3 V rail (l13) stays up.
 * None of these needs the RPM handshake: they are ordinary active-set votes
 * over the proven SMD channel plus two GCC register writes, and every one is
 * undone at wake. Each step is measured on the desk: with the SMB231 input
 * suspended the watch runs from its cell while the cable keeps the log alive,
 * and the STC3117 reports the cell current after every step (SLEEP_FLOOR
 * ladder). Read the "floor:" lines. */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909) && defined(SLEEP_FLOOR)
#include "FreeRTOS.h"
#include "task.h"

int cpu_volt_mv(void);   /* cpu_volt_a7.c */

#define K_SWEN 0x6e657773u  /* "swen" */
#define K_UV   0x00007675u  /* "uv"   */
#define K_MA   0x0000616du  /* "ma"   */
#define K_KHZ  0x007a484bu  /* "KHz"  */
#define K_CORN 0x6e726f63u  /* "corn" */
#define K_BW   0x00007762u  /* "bw"   */
#define T_LDOA 0x616f646cu  /* "ldoa" */
#define T_SMPA 0x61706d73u  /* "smpa" */
#define T_CLK2 0x326b6c63u  /* "clk2" RPM_MEM_CLK_TYPE: id 0 = bimc */
#define T_BSLV 0x766c7362u  /* "bslv" bus slave bw: id 0 = EBI (DDR) */

/* corner values as the RPM takes them (enum - CORNER_NONE): 1 retention,
 * 2 svs_krait, 3 svs_soc (= gcc VDD_DIG_LOW), 4 normal, 5 turbo, 6 super */
#define CORNER_SVS_SOC 3u
#define CORNER_NORMAL  4u

#define REG_SID 1u                  /* pm8916@1 carries the regulators */
#define GCC_APCS_GPLL_ENA_VOTE   0x45000u
#define GCC_APCS_BRANCH_ENA_VOTE 0x45004u
#define GPLL_VOTE_GPLL1  (1u << 1)
#define GPLL_VOTE_GPLL2  (1u << 3)
#define GPLL_VOTE_BIMC   (1u << 2)  /* never touched */
#define BRANCH_VOTE_CRYPTO_AHB (1u << 0)
#define BRANCH_VOTE_CRYPTO_AXI (1u << 1)
#define BRANCH_VOTE_CRYPTO     (1u << 2)
#define BRANCH_VOTE_PRNG_AHB   (1u << 8)
#define BRANCH_SLEEP_MASK (BRANCH_VOTE_CRYPTO_AHB | BRANCH_VOTE_CRYPTO_AXI | BRANCH_VOTE_CRYPTO | BRANCH_VOTE_PRNG_AHB)

/* Steps, bit-maskable with -DSLEEP_FLOOR_SKIP=<mask> */
#define STEP_DDR      (1u << 0)   /* EBI bw 0 + bimc KHz 0 */
#define STEP_PLL      (1u << 1)   /* GPLL1/GPLL2 + crypto/prng votes */
#define STEP_LDO_A    (1u << 2)   /* l1 l4 l12 (no consumer in the DT, not plausibly sensors) */
#define STEP_LDO_B    (1u << 3)   /* l10 l14 (no consumer in the AP DT; could be modem-side sensor rails) */
#define STEP_CX       (1u << 4)   /* VDD_CX corner -> SVS_SOC */
#define STEP_USB      (1u << 5)   /* PHY low-power + l13 off, only with no cable */
#define STEP_SMPS     (1u << 6)   /* bucks s1..s4 read MODE_CTL 0x80 = forced PWM: vote AUTO, direct write if we own them */
#define STEP_RAILS    (1u << 7)   /* l9 (3.3 V PA) and s3 (RF) were still ON after wlan_power_off's swen-0 vote: re-vote, direct off if writable */
#define STEP_ONE      (1u << 8)   /* -DSLEEP_RAILS_OFF=<mask>: probe rails, off at sleep entry, back on at wake */
#define K_SSMD 0x646d7373u  /* "ssmd" smps mode: 0 auto, 1 ipeak, 2 pwm */

/* ---- CUMULATIVE RAIL PROBE (2026-09-13, owner's request) -----------------
 * The goal is to match the stock C2+'s set of OFF rails exactly. Rails are
 * qualified ONE AT A TIME, but once a rail passes it stays in the set for every
 * later test -- so each image is "everything proven so far, plus one". That is
 * what -DSLEEP_RAILS_OFF=<mask> is: bit N = pm8916 LDO N, voted off after the
 * census and voted back on FIRST in sleep_floor_exit, before anything else
 * re-enables (a display rail has to be up before plat_display_on runs).
 *
 * Qualified so far, on the C2:
 *   l6  (v310) - DSI/panel vddio. Works, but the DDIC loses its state and comes
 *                back at POR: suspend_msm.c re-sends MADCTL. See finding 109.
 *   l12 (v311) - sdhci vdd-io + tpiu. Uneventful, as expected.
 * Target list still open: l18. s3 is NOT reachable by vote, and not because of
 * Pronto: on pm8916 s3 is the input supply of l1/l2/l3 (vdd_l1_l2_l3), and the
 * RPM keeps a parent buck up while any child LDO is on. l2 is the DDR 1.2 V
 * and l3 is VDD_MX, so s3 has a holder in every state including vdd-min. The
 * stock "8916_s3 disabled users=0" line is the kernel's own APSS vote count
 * (rpm-smd-regulator has no readback), not the rail.
 *
 * RESTORE VOLTAGES ARE DECODED FROM THE HARDWARE, not guessed. pm8916's ULT
 * PLDO has a single range: base 1750000 uV, step 12500 uV (see
 * kernels/skipjack-3.18/skipjack-kernel/drivers/regulator/qpnp-regulator.c,
 * ult_pldo_ranges[]), so uV = 1750000 + 12500 * VSET where VSET is the second
 * byte of the r/v=RANGE/VSET pair this file's own census prints. An earlier
 * table had l18 at 2800000 from a bad dtb parse and would have restored it
 * 100 mV high. Decode the census byte; do not trust regulator-min-microvolt.
 * NOTE l1 is ULT NLDO (base 375000, same step) -- different formula, not here.
 *
 * FAILURE MODE, so it is not a surprise: a rail that turns out to be load
 * bearing for the panel wakes the watch to a dark screen and needs a power
 * cycle. Not a brick, and the USB log still comes out. */
#ifndef SLEEP_RAILS_OFF
#define SLEEP_RAILS_OFF 0u
#endif
#define RAIL_BIT(n) (1u << (n))
/* id -> restore uV, from the VSET decode above. `smps` selects the RPM resource
 * (smpa vs ldoa) and the SPMI register block, so bucks can be probed too.
 *
 * s3 IS A BUCK, which is why the mask could not reach it before: every entry
 * voted T_LDOA. It is bit 3 of the SMPS half of the mask (SMPS_BIT(3)), not
 * bit 3 of the LDO half -- l3 and s3 are different rails. s3 reads ON from the
 * bootloader on this watch, not from our radio vote: with both radios disabled
 * wlan_up() never runs (nothing turns it on) and wlan_idle() returns at its
 * first line (nothing turns it off), and the census still shows s3:ON. */
static const struct { uint8_t id; uint8_t smps; uint32_t uv; } k_probe_uv[] = {
    {  6u, 0u, 1800000u },   /* l6  VSET 0x04 */
    {  9u, 0u, 3300000u },   /* l9  iris vddpa 3.3 V; stock has it off with WiFi off */
    { 11u, 0u, 2950000u },   /* l11 VSET 0x60 */
    { 12u, 0u, 1800000u },   /* l12 VSET 0x04 */
    { 17u, 0u, 2850000u },   /* l17 VSET 0x58 */
    { 18u, 0u, 2700000u },   /* l18 VSET 0x4c */
    {  3u, 1u, 1300000u },   /* s3  iris vddrfa; wcnss.c:846 votes this uV */
};
/* The mask is split: bits 0-23 are LDOs, bits 24-31 are SMPS. */
#define SMPS_BIT(n) (1u << (24u + (n)))
static uint32_t s_probe_off;    /* rails this sleep actually switched off */

static uint32_t s_gpll_vote0, s_branch_vote0, s_applied;
static uint8_t  s_smps_mode0[5], s_smps_direct;      /* direct MODE_CTL writes to undo */
static uint8_t  s_rail_direct_l9, s_rail_direct_s3;   /* direct EN_CTL writes to undo */
static uint8_t  s_ldo_was_on[19];

/* ---- census -------------------------------------------------------------- */
static uint16_t reg_base(int is_smps, unsigned n)
{
    return is_smps ? (uint16_t)(0x1400u + (n - 1u) * 0x300u) : (uint16_t)(0x4000u + (n - 1u) * 0x100u);
}
/* Returns ENABLE_CTL bit7, or -1 if the read failed. Prints one entry. */
static int reg_line(int is_smps, unsigned n)
{
    uint16_t b = reg_base(is_smps, n);
    uint8_t st = 0xFF, mode = 0xFF, en = 0xFF, rng = 0xFF, vset = 0xFF;
    if (spmi_read8(REG_SID, b + 0x46u, &en) < 0) { con_puts(is_smps ? " s" : " l"); con_putdec(n); con_puts(":rd-fail"); return -1; }
    (void)spmi_read8(REG_SID, b + 0x08u, &st);
    (void)spmi_read8(REG_SID, b + 0x45u, &mode);
    (void)spmi_read8(REG_SID, b + 0x40u, &rng);
    (void)spmi_read8(REG_SID, b + 0x41u, &vset);
    con_puts(is_smps ? " s" : " l"); con_putdec(n);
    con_puts((en & 0x80u) ? ":ON" : ":off");
    con_puts(" st="); con_puthex(st); con_puts(" md="); con_puthex(mode);
    con_puts(" r/v="); con_puthex(rng); con_puts("/"); con_puthex(vset);
    return (en & 0x80u) ? 1 : 0;
}
void sleep_floor_census(const char *tag)
{
    con_puts("floor-census["); con_puts(tag); con_puts("] pm8916 regulators (st=STATUS1 md=MODE_CTL r/v=RANGE/VSET):\n");
    con_puts("  smps:");
    for (unsigned n = 1; n <= 4; n++) (void)reg_line(1, n);
    con_puts("\n  ldo: ");
    for (unsigned n = 1; n <= 18; n++) {
        int on = reg_line(0, n);
        s_ldo_was_on[n] = (uint8_t)(on == 1);
        if (n == 6 || n == 12) con_puts("\n       ");
    }
    con_puts("\n  gcc: gpll_vote="); con_puthex(mmio_read(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE));
    con_puts(" branch_vote="); con_puthex(mmio_read(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE));
    con_puts(" gpll0="); con_puthex(mmio_read(PLAT_GCC_BASE + 0x21000u));
    con_puts(" gpll1="); con_puthex(mmio_read(PLAT_GCC_BASE + 0x20000u));
    con_puts(" gpll2="); con_puthex(mmio_read(PLAT_GCC_BASE + 0x25000u));
    con_puts(" bimc_pll?="); con_puthex(mmio_read(PLAT_GCC_BASE + 0x23000u));
    con_puts(" bimc_rcg?="); con_puthex(mmio_read(PLAT_GCC_BASE + 0x32004u)); con_puts("/"); con_puthex(mmio_read(PLAT_GCC_BASE + 0x32008u));
    { uint32_t portsc, usbsts, vid; int cfg; usb_diag(&portsc, &usbsts, &vid, &cfg);
      con_puts(" usb_portsc="); con_puthex(portsc); }
    con_puts(" apc_mv="); con_putdec((uint32_t)cpu_volt_mv());
    con_puts(" spmi-writable s1/s3/l9/l12="); con_putdec((uint32_t)(spmi_writable(REG_SID, 0x1445u) == 1));
    con_putdec((uint32_t)(spmi_writable(REG_SID, 0x1A46u) == 1)); con_putdec((uint32_t)(spmi_writable(REG_SID, 0x4846u) == 1));
    con_putdec((uint32_t)(spmi_writable(REG_SID, 0x4B46u) == 1));
    con_puts("\n"); con_flush(); usb_poll();
}

/* ---- votes --------------------------------------------------------------- */
static int vote(const char *what, uint32_t type, uint32_t id, const uint32_t *kv, uint32_t bytes)
{
    int rc = rpm_smd_request(0u, type, id, kv, bytes);
    con_puts("floor:   "); con_puts(what); con_puts(" rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc));
    /* -1 is ambiguous and one of its two causes is SILENT: rpm_smd_request()
     * returns it both for a transport failure (which prints an "rpm:" line of
     * its own) and for "the channel was never open" (which prints nothing).
     * On the C2+ every vote came back -1 with no rpm: line and it read as the
     * RPM refusing us; it was the channel. Say which. */
    if (rc == -1) con_puts(rpm_smd_is_open() ? " (neg: transport)" : " (neg: RPM CHANNEL NOT OPEN - no vote was sent)");
    else if (rc) con_puts(" (neg)");
    con_puts("\n");
    con_flush(); usb_poll();
    return rc;
}
static int vote_kv(const char *what, uint32_t type, uint32_t id, uint32_t key, uint32_t val)
{   uint32_t kv[3] = { key, 4u, val }; return vote(what, type, id, kv, sizeof kv); }
static int vote_ldo_on(const char *what, uint32_t id, uint32_t uv)
{   uint32_t kv[9] = { K_SWEN, 4u, 1u, K_UV, 4u, uv, K_MA, 4u, 10u }; return vote(what, T_LDOA, id, kv, sizeof kv); }

/* DT regulator-min-microvolt for the rails we may switch off (restore value) */
static const struct { uint8_t id; uint32_t uv; uint8_t group; } k_ldo[] = {
    {  1u, 1225000u, 'A' }, {  4u, 2050000u, 'A' }, { 12u, 1800000u, 'A' },
    { 10u, 2700000u, 'B' }, { 14u, 1800000u, 'B' },
};

/* ---- measurement (ladder) ------------------------------------------------ */
static int s_measure;
static uint32_t s_step_ms;
static void park_ms(uint32_t ms)
{
    uint32_t hz = timer_freq_hz();
    uint32_t t0 = timer_ms();
    while ((uint32_t)(timer_ms() - t0) < ms) {
        if (g_tick_armed) {
            tick_park(timer_ticks() + (uint64_t)(hz / 1000u) * 250u);
            __asm__ volatile("wfi");
            uint32_t missed = tick_unpark();
            if (missed) xTaskCatchUpTicks((TickType_t)missed);
        } else timer_delay_ms(250u);
        usb_poll();
    }
}
static void measure(const char *what)
{
    if (!s_measure) return;
    /* the STC3117 refreshes REG_CURRENT once per conversion period; the first
     * read after a change may still be the old window, so wait, then average
     * two spaced reads. Buses are re-clocked only for the two reads. */
    park_ms(s_step_ms);
    gcc_blsp_sleep(0);
    int ma1 = fg_batt_ma(), mv = fg_batt_mv();
#if defined(PLAT_CHG_SMB231)
    int chg = smb231_charging();
#else
    int chg = -1;                       /* PM8916 LBC boards: no external charger status here */
#endif
    int vbus = chg_usb_present();
    gcc_blsp_sleep(1);
    park_ms(2500u);
    gcc_blsp_sleep(0);
    int ma2 = fg_batt_ma();
    gcc_blsp_sleep(1);
    con_puts("floor: "); con_puts(what); con_puts(": mv="); con_putdec((uint32_t)mv); con_puts(" ma=");
    if (ma1 == -32768 || ma2 == -32768) con_puts("?");
    else { int ma = (ma1 + ma2) / 2; if (ma < 0) { con_puts("-"); ma = -ma; } con_putdec((uint32_t)ma);
           con_puts(" ("); if (ma1 < 0) { con_puts("-"); ma1 = -ma1; } con_putdec((uint32_t)ma1); con_puts("/");
           if (ma2 < 0) { con_puts("-"); ma2 = -ma2; } con_putdec((uint32_t)ma2); con_puts(")"); }
    con_puts(" chg="); con_putdec((uint32_t)chg); con_puts(" vbus="); con_putdec((uint32_t)vbus);
    con_puts(" bimc_rcg?="); con_puthex(mmio_read(PLAT_GCC_BASE + 0x32004u)); con_puts("/"); con_puthex(mmio_read(PLAT_GCC_BASE + 0x32008u));
    con_puts("\n"); con_flush(); usb_poll();
}

/* Call after every AP-side quiesce (buses gated, cluster on XO), before the
 * collapse. cable = a USB host is attached and listening. */
void sleep_floor_enter(int cable)
{
    /* Re-check EVERY sleep, not once ever: rpm_smd_init() is idempotent
     * (smd.c returns 0 when already open), and a one-shot `static int inited`
     * meant a channel that was not up at the first sleep was never retried --
     * every later vote then failed with a silent -1. */
    if (rpm_smd_init() < 0) { con_puts("floor: no RPM channel\n"); return; }
    s_applied = 0;
#ifndef SLEEP_FLOOR_STEP_MS
#define SLEEP_FLOOR_STEP_MS 6000u
#endif
    s_step_ms = SLEEP_FLOOR_STEP_MS;
    s_measure = cable;
    gcc_blsp_sleep(0);
    sleep_floor_census("sleep-entry");
    gcc_blsp_sleep(1);
    con_puts("floor: skip-mask "); con_puthex(SLEEP_FLOOR_SKIP); con_puts(s_measure ? ", measuring each step\n" : ", applying (no cable, no measurement)\n");
    measure("baseline (v162 quiesce)");

    if (!(SLEEP_FLOOR_SKIP & STEP_DDR)) {
        vote_kv("EBI bw 0 (bslv id0, releases the PAS 393 MB/s vote)", T_BSLV, 0u, K_BW, 0u);
        vote_kv("bimc 0 KHz (clk2 id0)", T_CLK2, 0u, K_KHZ, 0u);
        s_applied |= STEP_DDR;
        measure("+ddr votes released");
    }
    if (!(SLEEP_FLOOR_SKIP & STEP_PLL)) {
        s_gpll_vote0 = mmio_read(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE);
        s_branch_vote0 = mmio_read(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE);
        mmio_write(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE, s_gpll_vote0 & ~(GPLL_VOTE_GPLL1 | GPLL_VOTE_GPLL2));
        mmio_write(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE, s_branch_vote0 & ~BRANCH_SLEEP_MASK);
        __asm__ volatile("dsb sy" ::: "memory");
        s_applied |= STEP_PLL;
        con_puts("floor:   gpll vote "); con_puthex(s_gpll_vote0); con_puts(" -> "); con_puthex(mmio_read(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE));
        con_puts(", branch vote "); con_puthex(s_branch_vote0); con_puts(" -> "); con_puthex(mmio_read(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE));
        con_puts(" (gpll1 now "); con_puthex(mmio_read(PLAT_GCC_BASE + 0x20000u)); con_puts(" gpll2 "); con_puthex(mmio_read(PLAT_GCC_BASE + 0x25000u)); con_puts(")\n");
        measure("+gpll1/gpll2/crypto/prng votes off");
    }
    for (unsigned g = 0; g < 2u; g++) {
        uint32_t bit = g ? STEP_LDO_B : STEP_LDO_A;
        if (SLEEP_FLOOR_SKIP & bit) continue;
        int any = 0;
        for (unsigned i = 0; i < sizeof k_ldo / sizeof k_ldo[0]; i++) {
            if (k_ldo[i].group != (g ? 'B' : 'A') || !s_ldo_was_on[k_ldo[i].id]) continue;
            char what[24] = "l"; unsigned p = 1; uint32_t id = k_ldo[i].id;
            if (id >= 10u) what[p++] = (char)('0' + id / 10u); what[p++] = (char)('0' + id % 10u);
            what[p++] = ' '; what[p++] = 'o'; what[p++] = 'f'; what[p++] = 'f'; what[p] = 0;
            vote_kv(what, T_LDOA, id, K_SWEN, 0u);
            /* POSITIVE CONTROL: does an APSS disable vote move the enable bit at all? */
            { uint8_t en = 0xFF; timer_delay_ms(5u); (void)spmi_read8(REG_SID, reg_base(0, id) + 0x46u, &en);
              con_puts("floor:   -> l"); con_putdec(id); con_puts(" EN_CTL now "); con_puthex(en); con_puts((en & 0x80u) ? " (STILL ON: vote ignored)\n" : " (off: votes work)\n"); }
            any = 1;
        }
        s_applied |= bit;
        if (any) measure(g ? "+l10/l14 off" : "+l1/l4/l12 off");
        else { con_puts(g ? "floor:   l10/l14 already off\n" : "floor:   l1/l4/l12 already off\n"); }
    }
    if (!(SLEEP_FLOOR_SKIP & STEP_CX)) {
        vote_kv("vdd_cx corner 3 SVS_SOC (smpa id1 corn)", T_SMPA, 1u, K_CORN, CORNER_SVS_SOC);
        s_applied |= STEP_CX;
        gcc_blsp_sleep(0); con_puts("floor:   s1 now:"); (void)reg_line(1, 1u); con_puts("\n"); gcc_blsp_sleep(1);
        measure("+cx SVS_SOC");
    }
    if (!(SLEEP_FLOOR_SKIP & STEP_SMPS)) {
        static const char *nm[5] = { "", "s1", "s2", "s3", "s4" };
        s_smps_direct = 0;
        for (unsigned n = 1; n <= 4; n++) (void)spmi_read8(REG_SID, reg_base(1, n) + 0x45u, &s_smps_mode0[n]);
        for (unsigned n = 1; n <= 4; n++) {
            char what[40] = "smps mode AUTO (ssmd 0) "; unsigned p = 24; what[p++] = nm[n][0]; what[p++] = nm[n][1]; what[p] = 0;
            vote_kv(what, T_SMPA, n, K_SSMD, 0u);
        }
        timer_delay_ms(5u);
        con_puts("floor:   modes after vote:");
        for (unsigned n = 1; n <= 4; n++) {
            uint8_t m = 0xFF; (void)spmi_read8(REG_SID, reg_base(1, n) + 0x45u, &m);
            con_puts(" "); con_puts(nm[n]); con_puts("="); con_puthex(m);
            if ((m & 0x80u) && spmi_writable(REG_SID, reg_base(1, n) + 0x45u) == 1 && n != 2u) {   /* never the APC buck */
                if (spmi_write8(REG_SID, reg_base(1, n) + 0x45u, 0x40u) == 0) { s_smps_direct |= (uint8_t)(1u << n); con_puts("(direct->0x40)"); }
            }
        }
        con_puts("\n");
        s_applied |= STEP_SMPS;
        measure("+smps auto mode");
    }
    if (!(SLEEP_FLOOR_SKIP & STEP_RAILS)) {
        { uint32_t kv[9] = { K_SWEN, 4u, 0u, K_UV, 4u, 3300000u, K_MA, 4u, 0u }; vote("l9 off (swen 0, ma 0)", T_LDOA, 9u, kv, sizeof kv); }
        { uint32_t kv[9] = { K_SWEN, 4u, 0u, K_UV, 4u, 1300000u, K_MA, 4u, 0u }; vote("s3 off (swen 0, ma 0)", T_SMPA, 3u, kv, sizeof kv); }
        timer_delay_ms(5u);
        uint8_t e9 = 0, e3 = 0;
        (void)spmi_read8(REG_SID, 0x4846u, &e9); (void)spmi_read8(REG_SID, 0x1A46u, &e3);
        con_puts("floor:   after vote l9 en="); con_puthex(e9); con_puts(" s3 en="); con_puthex(e3);
        s_rail_direct_l9 = s_rail_direct_s3 = 0;
        if ((e9 & 0x80u) && spmi_writable(REG_SID, 0x4846u) == 1 && spmi_write8(REG_SID, 0x4846u, 0x00u) == 0) { s_rail_direct_l9 = 1; con_puts(" l9 direct off"); }
        if ((e3 & 0x80u) && spmi_writable(REG_SID, 0x1A46u) == 1 && spmi_write8(REG_SID, 0x1A46u, 0x00u) == 0) { s_rail_direct_s3 = 1; con_puts(" s3 direct off"); }
        con_puts("\n");
        s_applied |= STEP_RAILS;
        measure("+l9/s3 off");
    }
    s_probe_off = 0u;
    if (SLEEP_RAILS_OFF) {
        /* After the census, so s_ldo_was_on is populated. */
        for (unsigned i = 0; i < sizeof k_probe_uv / sizeof k_probe_uv[0]; i++) {
            uint32_t id = k_probe_uv[i].id;
            int      sm = k_probe_uv[i].smps;
            const char *pfx = sm ? "s" : "l";
            if (!(SLEEP_RAILS_OFF & (sm ? SMPS_BIT(id) : RAIL_BIT(id)))) continue;
            { uint8_t en0 = 0xFF;
              (void)spmi_read8(REG_SID, reg_base(sm, id) + 0x46u, &en0);
              if (!(en0 & 0x80u)) {
                  con_puts("floor:   "); con_puts(pfx); con_putdec(id);
                  con_puts(" already off at entry\n"); continue;
              } }
            uint32_t kv[9] = { K_SWEN, 4u, 0u, K_UV, 4u, k_probe_uv[i].uv, K_MA, 4u, 0u };
            vote("rail off", sm ? T_SMPA : T_LDOA, id, kv, sizeof kv);
            if (sm) {   /* 2026-09-13: the active set loses while awake; the sleep set is what the RPM applies in the collapse */
                int rs = rpm_smd_request(1u /* sleep set */, T_SMPA, id, kv, sizeof kv);
                con_puts("floor:   s"); con_putdec(id); con_puts(" sleep-set off rc "); con_putdec((uint32_t)(rs < 0 ? -rs : rs)); con_puts("\n");
                /* 2026-09-13 holder probe: is the buck on by PIN CONTROL (EN
                 * follows a PMIC pin, +0x47) rather than by votes? Dump the
                 * control block, then vote "pcen" 0 (rpm-smd-regulator.c
                 * PIN_CTRL_ENABLE) in both sets and read EN/PIN_CTL again. */
                uint16_t b = reg_base(1, id);
                con_puts("floor:   s"); con_putdec(id); con_puts(" ctl 0x40..0x4f:");
                for (unsigned o = 0x40u; o <= 0x4fu; o++) { uint8_t v = 0xFF; (void)spmi_read8(REG_SID, (uint16_t)(b + o), &v); con_puts(" "); con_puthex(v); }
                con_puts("\nfloor:   l2 pinctl="); { uint8_t v = 0xFF; (void)spmi_read8(REG_SID, 0x4147u, &v); con_puthex(v); }
                con_puts(" l3 pinctl="); { uint8_t v = 0xFF; (void)spmi_read8(REG_SID, 0x4247u, &v); con_puthex(v); }
                con_puts("\n");
                { uint32_t pk[6] = { 0x6e656370u /* "pcen" */, 4u, 0u, K_SWEN, 4u, 0u };
                  int p0 = rpm_smd_request(0u, T_SMPA, id, pk, sizeof pk);
                  int p1 = rpm_smd_request(1u, T_SMPA, id, pk, sizeof pk);
                  timer_delay_ms(5u);
                  uint8_t e = 0xFF, pc = 0xFF;
                  (void)spmi_read8(REG_SID, (uint16_t)(b + 0x46u), &e); (void)spmi_read8(REG_SID, (uint16_t)(b + 0x47u), &pc);
                  con_puts("floor:   s"); con_putdec(id); con_puts(" pcen 0 rc active "); con_putdec((uint32_t)(p0 < 0 ? -p0 : p0));
                  con_puts(" sleep "); con_putdec((uint32_t)(p1 < 0 ? -p1 : p1));
                  con_puts(" -> en="); con_puthex(e); con_puts(" pinctl="); con_puthex(pc);
                  con_puts((e & 0x80u) ? " (still on)\n" : " (OFF)\n"); }
            }
            timer_delay_ms(5u);
            uint8_t en = 0xFF;
            (void)spmi_read8(REG_SID, reg_base(sm, id) + 0x46u, &en);
            con_puts("floor:   "); con_puts(pfx); con_putdec(id); con_puts(" after vote en="); con_puthex(en);
            if (en & 0x80u) con_puts(" (STILL ON: the vote lost, another master holds it)\n");
            else { con_puts(" (off)\n"); s_probe_off |= (sm ? SMPS_BIT(id) : RAIL_BIT(id)); }
        }
        if (s_probe_off) { s_applied |= STEP_ONE; measure("+probe rails off"); }
    }
    if (!(SLEEP_FLOOR_SKIP & STEP_USB) && !cable && chg_usb_present() != 1) {
        usb_phy_lowpower(1);
        vote_kv("l13 off (USB 3.3 V, no cable)", T_LDOA, 13u, K_SWEN, 0u);
        s_applied |= STEP_USB;
    }
    con_puts("floor: applied "); con_puthex(s_applied); con_puts("\n"); con_flush(); usb_poll();
}

/* Undo in reverse, BEFORE the buses are un-gated and the radio restarted. */
void sleep_floor_exit(void)
{
    if (!s_applied) return;
    /* FIRST, before anything else re-enables: a display rail has to be back
     * before plat_display_on() runs. */
    if (s_applied & STEP_ONE) {
        for (unsigned i = 0; i < sizeof k_probe_uv / sizeof k_probe_uv[0]; i++) {
            uint32_t id = k_probe_uv[i].id;
            int      sm = k_probe_uv[i].smps;
            if (!(s_probe_off & (sm ? SMPS_BIT(id) : RAIL_BIT(id)))) continue;
            if (sm) { uint32_t kv[9] = { K_SWEN, 4u, 1u, K_UV, 4u, k_probe_uv[i].uv, K_MA, 4u, 100u };
                      vote("rail on", T_SMPA, id, kv, sizeof kv); }
            else      vote_ldo_on("rail on", id, k_probe_uv[i].uv);
        }
        timer_delay_ms(2u);
    }
    if (s_applied & STEP_USB) {
        vote_ldo_on("l13 on 3.075 V", 13u, 3075000u);
        usb_phy_lowpower(0);
    }
    if (s_applied & STEP_RAILS) {
        if (s_rail_direct_l9) (void)spmi_write8(REG_SID, 0x4846u, 0x80u);
        if (s_rail_direct_s3) (void)spmi_write8(REG_SID, 0x1A46u, 0x80u);
        /* the RPM votes stay off: the radio restart re-votes what it needs */
    }
    if (s_applied & STEP_SMPS) {
        for (unsigned n = 1; n <= 4; n++) if (s_smps_direct & (1u << n)) (void)spmi_write8(REG_SID, reg_base(1, n) + 0x45u, s_smps_mode0[n]);
        for (unsigned n = 1; n <= 4; n++) vote_kv("smps mode PWM restore (ssmd 2)", T_SMPA, n, K_SSMD, 2u);
    }
    if (s_applied & STEP_CX)
        vote_kv("vdd_cx corner 4 NORMAL", T_SMPA, 1u, K_CORN, CORNER_NORMAL);
    for (unsigned i = 0; i < sizeof k_ldo / sizeof k_ldo[0]; i++) {
        uint32_t bit = (k_ldo[i].group == 'A') ? STEP_LDO_A : STEP_LDO_B;
        if ((s_applied & bit) && s_ldo_was_on[k_ldo[i].id]) {
            char what[24] = "l"; unsigned p = 1; uint32_t id = k_ldo[i].id;
            if (id >= 10u) what[p++] = (char)('0' + id / 10u); what[p++] = (char)('0' + id % 10u);
            what[p++] = ' '; what[p++] = 'o'; what[p++] = 'n'; what[p] = 0;
            vote_ldo_on(what, id, k_ldo[i].uv);
        }
    }
    if (s_applied & STEP_PLL) {
        mmio_write(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE, s_gpll_vote0);
        mmio_write(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE, s_branch_vote0);
        __asm__ volatile("dsb sy" ::: "memory");
        /* let the PLLs lock again before anything sources from them */
        { uint32_t t0 = timer_ms();
          while ((uint32_t)(timer_ms() - t0) < 5u) {
              uint32_t m1 = mmio_read(PLAT_GCC_BASE + 0x20000u), m2 = mmio_read(PLAT_GCC_BASE + 0x25000u);
              if (((s_gpll_vote0 & GPLL_VOTE_GPLL1) == 0u || (m1 & (1u << 31))) &&
                  ((s_gpll_vote0 & GPLL_VOTE_GPLL2) == 0u || (m2 & (1u << 31)))) break;
          } }
        timer_delay_us(200u);
    }
    if (s_applied & STEP_DDR) {
        /* No vote existed before; ask for the top of the table, the RPM clamps. */
        vote_kv("bimc 533000 KHz", T_CLK2, 0u, K_KHZ, 533000u);
    }
    con_puts("floor: restored "); con_puthex(s_applied); con_puts("\n"); con_flush();
    s_applied = 0;
}

#endif /* PLAT_SOC_MSM8909 && SLEEP_FLOOR */

/* ---- release builds: the rail switching alone, without the ladder ----------
 * -DSLEEP_FLOOR carries the rail probe above but also its measurement ladder
 * (~50 s awake before every collapse). The rails that have passed the probe
 * belong in every sleep, so without SLEEP_FLOOR this is just that part: the
 * same SLEEP_RAILS_OFF mask and restore voltages, the same votes, silent
 * unless a vote fails. Called from the same two places as sleep_floor_*. */
#if defined(PLAT_SOC_MSM8909) && !defined(SLEEP_FLOOR)
#ifndef SLEEP_RAILS_OFF
#define SLEEP_RAILS_OFF 0u
#endif
#if SLEEP_RAILS_OFF
#define SR_SWEN 0x6e657773u  /* "swen" */
#define SR_UV   0x00007675u  /* "uv"   */
#define SR_MA   0x0000616du  /* "ma"   */
#define SR_LDOA 0x616f646cu  /* "ldoa" */
#define SR_SMPA 0x61706d73u  /* "smpa" */
#define SR_SID  1u
#define SR_BIT(id, sm) ((sm) ? (1u << (24u + (id))) : (1u << (id)))
/* Keep in step with k_probe_uv[] above (restore uV decoded from VSET). */
static const struct { uint8_t id; uint8_t smps; uint32_t uv; } k_rail[] = {
    {  6u, 0u, 1800000u }, {  8u, 0u, 2900000u }, {  9u, 0u, 3300000u }, { 11u, 0u, 2950000u }, { 12u, 0u, 1800000u },
    { 17u, 0u, 2850000u }, { 18u, 0u, 2700000u }, {  3u, 1u, 1300000u },
};
static uint32_t s_rails_off;
#if defined(PLAT_IMU_RAIL_BIT)
static uint32_t s_imu_rail = PLAT_IMU_RAIL_BIT;   /* known from the board header: no probe, no power cycle */
#else
static uint32_t s_imu_rail;     /* SR_BIT of the rail found to power the LSM6DS3, 0 = not found yet */
#endif
/* Restore voltage per rail, DECODED FROM THE PMIC AT SLEEP ENTRY (2026-09-22): the k_rail[]
 * figures are the C2's and the Gen 4 has the same rails at other voltages (firefish DT: l8 2.85,
 * l11 1.8, l18 2.85 V), so a table would restore them wrong there. pm8916 l4..l18 are ULT PLDO,
 * one range: uV = 1750000 + 12500 * VSET (+0x41) -- the same decode the table came from, so the
 * C2's values do not move. The table stays as the fallback for a failed read and for bucks. */
static uint32_t s_rail_uv[sizeof k_rail / sizeof k_rail[0]];
static uint16_t sr_base(int sm, unsigned n)
{
    return sm ? (uint16_t)(0x1400u + (n - 1u) * 0x300u) : (uint16_t)(0x4000u + (n - 1u) * 0x100u);
}
static void sr_fail(const char *what, int sm, uint32_t id)
{
    con_puts("rails: "); con_puts(sm ? "s" : "l"); con_putdec(id); con_puts(what);
}
void sleep_rails_enter(void)
{
    s_rails_off = 0u;
    if (rpm_smd_init() < 0) { con_puts("rails: no RPM channel, rails left on\n"); return; }
    for (unsigned i = 0; i < sizeof k_rail / sizeof k_rail[0]; i++) {
        uint32_t id = k_rail[i].id; int sm = k_rail[i].smps;
        if (!(SLEEP_RAILS_OFF & SR_BIT(id, sm))) continue;
#if defined(PLAT_SOC_MSM8909)   /* imu_lsm6ds3.c is built on every 8909 board */
        if (s_imu_rail == SR_BIT(id, sm)) {
            con_puts("imu: "); con_puts(sm ? "s" : "l"); con_putdec(id);
            if (lsm6ds3_running()) { con_puts(" kept ON (pedometer/sleep session running), WHO_AM_I "); con_puts(lsm6ds3_alive() ? "ok\n" : "NOT ANSWERING\n"); continue; }
            con_puts(" cut (IMU off)\n");
        }
#endif
        { extern int g_sleep_keep_l8; if (!sm && id == 8u && g_sleep_keep_l8) continue; }   /* card not asleep */
        uint8_t en = 0xFF;
        (void)spmi_read8(SR_SID, sr_base(sm, id) + 0x46u, &en);
        if (!(en & 0x80u)) continue;                       /* already off: nothing to restore */
        s_rail_uv[i] = k_rail[i].uv;
        if (!sm && id >= 4u) { uint8_t vset = 0xFF;
            if (spmi_read8(SR_SID, sr_base(0, id) + 0x41u, &vset) == 0 && vset <= 0x7Cu) s_rail_uv[i] = 1750000u + 12500u * vset; }
        uint32_t kv[9] = { SR_SWEN, 4u, 0u, SR_UV, 4u, s_rail_uv[i], SR_MA, 4u, 0u };
        int rc = rpm_smd_request(0u, sm ? SR_SMPA : SR_LDOA, id, kv, sizeof kv);
        /* SLEEP SET TOO, for LDOs as well (2026-09-22). sys_pc8909_init() votes l6, l11, l12,
         * l17 and l18 ON in the sleep set, and the sleep set is what the RPM applies inside
         * the collapse -- so an active-set-only cut was switched back on for the whole sleep
         * (16 h at 11.2 mA with every one of these "cut"). Left off in set 1 afterwards:
         * every later sleep wants them off as well. */
#ifndef SLEEP_SET_KEEP
#define SLEEP_SET_KEEP 0u     /* bisect: rails in this mask are cut in the ACTIVE set only, i.e. the
                               * boot-time sleep set switches them back on inside the collapse */
#endif
        if (!(SLEEP_SET_KEEP & SR_BIT(id, sm)))
            (void)rpm_smd_request(1u, sm ? SR_SMPA : SR_LDOA, id, kv, sizeof kv);
        timer_delay_ms(5u);
        en = 0xFF;
        (void)spmi_read8(SR_SID, sr_base(sm, id) + 0x46u, &en);
        if (!(en & 0x80u)) s_rails_off |= SR_BIT(id, sm);
        else if (rc) sr_fail(" off vote failed\n", sm, id);
        else         sr_fail(" still on (another master holds it)\n", sm, id);
#if defined(PLAT_SOC_MSM8909)   /* imu_lsm6ds3.c is built on every 8909 board */
        /* WHICH RAIL FEEDS THE IMU (2026-09-22)? The LSM6DS3 is not in the AP's stock tree (the
         * modem owns the sensors there), and with the cuts really holding the step counter came
         * back reset (30 -> hw 0 -> 65536). Find it: after each cut ask WHO_AM_I over the
         * bit-banged bus. The first rail whose cut silences the chip is the IMU's; it is voted
         * back on at once and remembered. From then on it is left ON whenever the pedometer or
         * a sleep session is running (they count through the sleep, as on the other boards) and
         * cut like the rest when the IMU is off. */
        if ((s_rails_off & SR_BIT(id, sm)) && s_imu_rail == 0u && lsm6ds3_present()) {
            timer_delay_ms(10u);
            if (!lsm6ds3_alive()) {
                s_imu_rail = SR_BIT(id, sm);
                con_puts("rails: "); con_puts(sm ? "s" : "l"); con_putdec(id); con_puts(" feeds the IMU (WHO_AM_I lost when it was cut)");
                con_puts(lsm6ds3_running() ? " - back ON, pedometer/sleep session running\n" : " - left off this time, IMU is idle\n");
                if (lsm6ds3_running()) {
                    uint32_t on[9] = { SR_SWEN, 4u, 1u, SR_UV, 4u, s_rail_uv[i], SR_MA, 4u, 10u };
                    (void)rpm_smd_request(0u, sm ? SR_SMPA : SR_LDOA, id, on, sizeof on);
                    (void)rpm_smd_request(1u, sm ? SR_SMPA : SR_LDOA, id, on, sizeof on);
                    s_rails_off &= ~SR_BIT(id, sm);
                }
            }
        }
#endif
    }
}
void sleep_rails_exit(void)
{
    if (!s_rails_off) return;
    for (unsigned i = 0; i < sizeof k_rail / sizeof k_rail[0]; i++) {
        uint32_t id = k_rail[i].id; int sm = k_rail[i].smps;
        if (!(s_rails_off & SR_BIT(id, sm))) continue;
        uint32_t kv[9] = { SR_SWEN, 4u, 1u, SR_UV, 4u, s_rail_uv[i], SR_MA, 4u, sm ? 100u : 10u };
        if (rpm_smd_request(0u, sm ? SR_SMPA : SR_LDOA, id, kv, sizeof kv))
            sr_fail(" ON vote failed\n", sm, id);
        if (sm) (void)rpm_smd_request(1u, SR_SMPA, id, kv, sizeof kv);
    }
    timer_delay_ms(2u);
    s_rails_off = 0u;
}
#endif /* SLEEP_RAILS_OFF */
#endif /* PLAT_SOC_MSM8909 && !SLEEP_FLOOR */
