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
#define K_SSMD 0x646d7373u  /* "ssmd" smps mode: 0 auto, 1 ipeak, 2 pwm */
#ifndef SLEEP_FLOOR_SKIP
#define SLEEP_FLOOR_SKIP 0u
#endif

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
    con_puts("floor:   "); con_puts(what); con_puts(" rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts(rc ? " (neg)\n" : "\n");
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
    static int inited;
    if (!inited) { inited = 1; if (rpm_smd_init() < 0) { con_puts("floor: no RPM channel\n"); return; } }
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
