/* sys_pc8909.c — SYSTEM power collapse on the msm8909w watches (2026-09-07).
 *
 * WHY: with CPU0 + CPU1 collapsed, the radio off, every AP clock we own
 * gated and the cluster on the crystal, the C2 still lost 4.18 -> 3.90 V in
 * 2 h 30 asleep -- the same slope as with none of that. The AP-visible
 * register census is identical before and after every collapse. What is left
 * is the platform floor: nothing has ever told the RPM the application
 * processor is asleep, so CX/MX sit at their active corners, DDR never enters
 * self-refresh and every bus stays at its active rate. The stock kernel's
 * "l2-pc" cluster level (skipjack DTS: qcom,notify-rpm, reset-level 3,
 * qcom,no-cache-flush, plus the lpm-wa-skip-l2-spm workaround) is what gets
 * the RPM to apply the APSS SLEEP SET. This file does what lpm-levels.c does
 * for that level:
 *   1. sleep-set votes (RPM set 1) for the rails that must survive: the
 *      panel/DSI (l2 1.2 V, l6 1.8 V, l18), touch + sensors (l17), eMMC
 *      (l8 + l5 io), l7 (XO/USB 1.8 V) and -- STAGE A -- the crystal itself
 *      (cxo Enab 1) so the DSI PLL, QTimer and USB keep their clock. Stage B
 *      releases the crystal once stage A is proven.
 *   2. MPM: pin 62 (PMIC arbiter, GIC 222 -- DTS gic-map 0x3e->0xde) armed
 *      level-high, no timed wake (~0), doorbell 2 (ipc-bit-offset 1).
 *   3. GIC distributor saved (the cluster domain resets it).
 *   4. L2 SAW start address + PC_MODE + SLP_CMD WITHOUT SPM_EN (the
 *      workaround); TZ enables it when TERMINATE_PC gets L2 flag 1.
 * cpu_pc8909_sleep() then collapses CPU0 with flag 1; TZ flushes the L2
 * (no-cache-flush = TZ does it), the L2 SAW runs the pc sequence with the
 * RPM handshake, the RPM applies the sleep set. Wake = PMIC line -> MPM ->
 * RPM -> cluster up -> TZ warm boot -> cpu_pc_resume -> gicd_restore.
 * Everything else (tick, QTimer frame, SPM cpu0) is what the plain collapse
 * already restores. */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

#define K_SWEN 0x6e657773u  /* "swen" */
#define K_UV   0x00007675u  /* "uv"   */
#define K_MA   0x0000616du  /* "ma"   */
#define K_ENAB 0x62616e45u  /* "Enab" */
#define T_LDOA 0x616f646cu  /* "ldoa" */
#define T_CLK0 0x306b6c63u  /* "clk0" */
#define RPM_SET_SLEEP 1u

static int s_votes_ok, s_ready;
/* drivers/soc/qcom/rpm_master_stat.c, version 2, qcom,master-offset 4096:
 * struct { u32 active_cores; u32 numshutdowns; u64 shutdown_req; u64 wakeup_ind;
 *          u64 bringup_req; u64 bringup_ack; u32 wakeup_reason; u32 last_sleep;
 *          u32 last_wake; u32 xo_count; ... } at 0x60150 + master*4096. */
void rpm_master_stats_line(const char *tag)
{
    static const char *names[3] = { "APSS", "MPSS", "PRONTO" };
    con_puts("rpm-masters["); con_puts(tag); con_puts("]:");
    for (unsigned m = 0; m < 3u; m++) {
        uint32_t b = 0x60150u + m * 4096u;
        con_puts(" "); con_puts(names[m]); con_puts(" cores="); con_puthex(mmio_read(b));
        con_puts(" shutdowns="); con_putdec(mmio_read(b + 4u)); con_puts(" xo="); con_putdec(mmio_read(b + 52u));
        con_puts(" reason="); con_putdec(mmio_read(b + 40u));
    }
    con_puts("\n");
    /* v403: the RPM's OWN sleep statistics (drivers/soc/qcom/rpm_stats.c version 2, stock DT
     * qcom,rpm-stats@29dba0): 2 records x 48 B { stat_type 4 chars ("xosd" = chip-wide XO
     * shutdown, "vmin" = the lowest RPM state), count, last_entered u64, last_exited u64,
     * accumulated u64 (19.2 MHz ticks), client_votes }. These counters moving is the proof the
     * whole chip went into XO shutdown / vmin, which the always-awake modem prevented until v402. */
    con_puts("rpm-stats["); con_puts(tag); con_puts("]:");
    for (unsigned r = 0; r < 2u; r++) {
        uint32_t b = 0x0029dba0u + r * 48u, ty = mmio_read(b);
        char nm[5] = { (char)ty, (char)(ty >> 8), (char)(ty >> 16), (char)(ty >> 24), 0 };
        for (unsigned k = 0; k < 4u; k++) if (nm[k] < 32 || nm[k] > 126) nm[k] = '?';
        uint32_t acc_lo = mmio_read(b + 24u), acc_hi = mmio_read(b + 28u);
        uint64_t acc = ((uint64_t)acc_hi << 32) | acc_lo;
        con_puts(" "); con_puts(nm); con_puts(" count="); con_putdec(mmio_read(b + 4u));
        con_puts(" last-in="); con_puthex(mmio_read(b + 8u)); con_puts(" last-out="); con_puthex(mmio_read(b + 16u));
        con_puts(" total-ms="); con_putdec((uint32_t)(acc / 19200u)); con_puts(" votes="); con_puthex(mmio_read(b + 32u));
    }
    /* v405: S3 is OFF on the stock C2+ even awake (0 consumers); ours stays on because a vote we
     * do not own holds it (RPM boot config or the modem, which never voted before v402). Read the
     * buck's EN_CTL (SPMI sid 1, S3 = 0x1A00 + 0x46; bit7 = enabled) right here, while awake. */
    { uint8_t en = 0xFF; int rc = spmi_read8(1u, 0x1A46u, &en);
      con_puts(" | s3 EN_CTL="); if (rc < 0) con_puts("read-failed"); else { con_puthex(en); con_puts(en & 0x80u ? " (ON)" : " (off)"); } }
    con_puts("\n");
}
extern uint32_t g_cpu_pc_slp_cmd;

#if defined(MSS_PROBE)
/* 2026-09-13: is the modem (MPSS) on by default? RPM shows MPSS cores=1
 * shutdowns=0 forever on our firmware; stock counts thousands. skipjack DT:
 * qcom,mss@4080000 "qcom,pil-q6v55-mss", pil-self-auth (MBA, NOT a PAS id),
 * restart_reg 0x183e000 (GCC MSS restart, bit0 = Q6 held in reset). Only GCC
 * and the PMIC are touched here (always clocked); qdsp6/rmb space may be gated. */
#define MSS_RESTART_REG 0x0183e000u
static void mss_probe(const char *tag)
{
    uint8_t s3 = 0xFF, l2 = 0xFF, l3 = 0xFF;
    (void)spmi_read8(1u, 0x1A46u, &s3); (void)spmi_read8(1u, 0x4146u, &l2); (void)spmi_read8(1u, 0x4246u, &l3);
    con_puts("mss-probe["); con_puts(tag); con_puts("]: restart_reg="); con_puthex(mmio_read(MSS_RESTART_REG));
    con_puts(" s3 en="); con_puthex(s3); con_puts(" l2 en="); con_puthex(l2); con_puts(" l3 en="); con_puthex(l3);
    con_puts(" pas_supported(1)="); con_putdec((uint32_t)scm_pas_is_supported(1u)); con_puts("\n");
    rpm_master_stats_line(tag);
    con_flush(); usb_poll();
}
#endif


#if defined(MSS_SAW_SCAN)
/* ---- MSS Q6 SAW2 hunt + RPM handshake (2026-09-13, route 1, take 2) -------
 * v334 showed PRONTO already asleep (cores=0 shutdowns=13); the ONLY master
 * pinning the RPM out of vmin is MPSS (cores=1 shutdowns=0, modem never
 * booted). Its SAW2 is not in the AP device tree (the mss node maps only
 * qdsp6 0x4080000/0x100, rmb, halt regs, restart_reg). This walks the MSS
 * register window for the SAW2 signature (+0xfd0 VERSION = 0x2xxxxxxx /
 * 0x3xxxxxxx, +0x04 ID like Pronto's 0x200b0811) with gcc_mss_cfg_ahb_clk on
 * (GCC 0x49000, clock-gcc-8909.c), announcing every page before it is read:
 * an unclocked or XPU-guarded page is a NoC error -> TZ reset, and the last
 * printed address then names the page to skip next time. On a hit it loads
 * {07 0f} (sleep with RPM handshake, end) at SEQ_ENTRY (0x400 on v3, 0x80 on
 * v2) and starts it with SPM_EN|PC_MODE|SLP_CMD; MPSS shutdowns moving in the
 * RPM master stats is the only success signal. */
#define GCC_MSS_CFG_AHB_CBCR     0x01849000u
#define GCC_MSS_Q6_BIMC_AXI_CBCR 0x01849004u
static void saw_sync(void) { con_flush(); usb_poll(); blackbox_sync(); }
#define MPSS_SHUTDOWNS           (0x60150u + 4096u + 4u)
static void saw_regs(const char *tag, uint32_t base, uint32_t seq)
{
    con_puts("mss-saw["); con_puts(tag); con_puts("]: STS "); con_puthex(mmio_read(base + 0x0cu));
    con_puts(" CTL "); con_puthex(mmio_read(base + 0x30u)); con_puts(" CFG "); con_puthex(mmio_read(base + 0x08u));
    con_puts(" SEQ0 "); con_puthex(mmio_read(base + seq)); con_puts("\n"); saw_sync();
}
static int saw_try_handshake(uint32_t base, uint32_t ver)
{
    uint32_t seq = (ver >> 28) >= 3u ? 0x400u : 0x80u;
    uint32_t before = mmio_read(MPSS_SHUTDOWNS);
    saw_regs("found", base, seq);
    con_puts("mss-saw: loading {07 0f} at +"); con_puthex(seq); con_puts(" and starting EN|PC_MODE|SLP_CMD ...\n"); saw_sync();
    mmio_write(base + 0x30u, 0u);
    mmio_write(base + seq, 0x00000f07u);
    __asm__ volatile("dsb" ::: "memory");
    mmio_write(base + 0x30u, (1u << 0) | (1u << 16) | (1u << 17));
    __asm__ volatile("dsb" ::: "memory");
    timer_delay_ms(20u);
    saw_regs("+20ms", base, seq);
    timer_delay_ms(300u);
    rpm_master_stats_line("mss-saw");
    if (mmio_read(MPSS_SHUTDOWNS) != before) { con_puts("mss-saw: MPSS shutdowns MOVED -> RPM took the handshake\n"); saw_sync(); return 1; }
    con_puts("mss-saw: MPSS shutdowns unchanged\n"); saw_sync();
    return 0;
}
static void mss_saw_scan(void)
{
    uint32_t v = mmio_read(GCC_MSS_CFG_AHB_CBCR);
    con_puts("mss-saw: GCC mss_cfg_ahb "); con_puthex(v); con_puts(" q6_bimc_axi "); con_puthex(mmio_read(GCC_MSS_Q6_BIMC_AXI_CBCR)); con_puts("\n"); saw_sync();
    if (!(v & 1u)) {
        mmio_write(GCC_MSS_CFG_AHB_CBCR, v | 1u);
        for (unsigned i = 0; i < 100u && (mmio_read(GCC_MSS_CFG_AHB_CBCR) & 0x80000000u); i++) timer_delay_us(10u);
        con_puts("mss-saw: enabled mss_cfg_ahb -> "); con_puthex(mmio_read(GCC_MSS_CFG_AHB_CBCR)); con_puts("\n"); saw_sync();
    }
    con_puts("mss-saw: begin (blackbox marker)\n"); saw_sync(); timer_delay_ms(200u); saw_sync();
#if defined(MSS_Q6_PARTIAL_UP)
    /* v339: the stock PIL (pil-q6v5.c __pil_q6v55_reset) only touches the Q6
     * sub-blocks after bus_clk (mss_q6_bimc_axi), mem_clk (boot_rom_ahb, voted
     * bit 7) and the Q6's own XO branch (QDSP6SS_XO_CBCR +0x38) are on and the
     * BHS head switch (PWR_CTL bit 24) is closed: "BHS require xo cbcr to be
     * enabled". v336-v338 read the SAW page with all of that off. Do the same
     * power-up, stop short of releasing the core reset, then read the page. */
    v = mmio_read(GCC_MSS_Q6_BIMC_AXI_CBCR);
    if (!(v & 1u)) { mmio_write(GCC_MSS_Q6_BIMC_AXI_CBCR, v | 1u);
        for (unsigned i = 0; i < 100u && (mmio_read(GCC_MSS_Q6_BIMC_AXI_CBCR) & 0x80000000u); i++) timer_delay_us(10u); }
    con_puts("mss-saw: q6_bimc_axi -> "); con_puthex(mmio_read(GCC_MSS_Q6_BIMC_AXI_CBCR));
    mmio_write(0x01845004u, mmio_read(0x01845004u) | (1u << 7));      /* boot_rom_ahb vote */
    con_puts(" boot_rom_ahb cbcr "); con_puthex(mmio_read(0x0181300cu)); con_puts("\n"); saw_sync();
    con_puts("mss-saw: QDSP6SS_XO_CBCR "); con_puthex(mmio_read(0x04080038u));
    mmio_write(0x04080038u, mmio_read(0x04080038u) | 1u);
    for (unsigned i = 0; i < 100u && (mmio_read(0x04080038u) & 0x80000000u); i++) timer_delay_us(1u);
    con_puts(" -> "); con_puthex(mmio_read(0x04080038u)); con_puts("\n"); saw_sync();
    v = mmio_read(0x04080030u) | (1u << 24);                          /* BHS_ON */
    mmio_write(0x04080030u, v); __asm__ volatile("dsb" ::: "memory"); timer_delay_us(5u);
    v |= (1u << 25);                                                   /* LDO_BYP */
    mmio_write(0x04080030u, v);
    con_puts("mss-saw: PWR_CTL -> "); con_puthex(mmio_read(0x04080030u)); con_puts(" BHS_STATUS "); con_puthex(mmio_read(0x04080078u)); con_puts("\n"); saw_sync();
#endif
    con_puts("mss-saw: reading QDSP6SS 0x4080014 (RESET) ...\n"); saw_sync();
    con_puts("mss-saw: QDSP6SS RESET "); con_puthex(mmio_read(0x04080014u)); con_puts(" GFMUX "); con_puthex(mmio_read(0x04080020u));
    con_puts(" PWR_CTL "); con_puthex(mmio_read(0x04080030u)); con_puts("\n"); saw_sync();
    /* v337: no page walk (v336: 0x04080fd0 = NoC reset). Candidates from the
     * modem's own code: Hexagon immext extenders in c2-modem.img reference
     * 0x04081000 14x with in-page offsets < 0x40 (SAW2 SECURE/ID/CFG/STS/CTL
     * live there), 0x040ac000 13x; 0x040b0000 is the 8974 layout analogy. */
#if defined(MSS_SAW_ADDR)
    static const uint32_t cand[] = { MSS_SAW_ADDR };     /* v338: one candidate per image */
#else
    static const uint32_t cand[] = { 0x04081000u, 0x040b0000u, 0x040ac000u, 0x040c1000u };
#endif
    unsigned hits = 0;
    for (unsigned k = 0; k < sizeof cand / sizeof cand[0]; k++) {
        uint32_t a = cand[k];
        {
            con_puts("mss-saw: @"); con_puthex(a); con_puts(" (reading +0xfd0 in 200 ms)\n"); saw_sync();
            timer_delay_ms(200u); saw_sync();   /* v338: the eMMC write must land before a possible NoC reset */
            uint32_t ver = mmio_read(a + 0xfd0u), id = mmio_read(a + 0x04u);
            con_puts(" ver "); con_puthex(ver); con_puts(" id "); con_puthex(id); con_puts("\n"); saw_sync();
            if (((ver >> 28) == 2u || (ver >> 28) == 3u) && (ver & 0x000fffffu) == 0u) {
                hits++;
                if (saw_try_handshake(a, ver)) { con_puts("mss-saw: done\n"); return; }
            }
        }
    }
    con_puts("mss-saw: scan complete, candidates "); con_putdec(hits); con_puts(", no handshake taken\n"); saw_sync();
}
#endif

#if !defined(SYS_PC_NO_SLEEP_SET)
/* LOAD VOTED WITH EVERY SLEEP-SET RAIL = THE STOCK KERNEL'S OWN LOW-POWER VOTE (2026-09-22).
 * rpm-smd-regulator.c (skipjack 3.18): a regulator is in HPM when its "ma" vote is >=
 * qcom,hpm-min-load, and when the kernel asks for LPM it votes hpm_min_load - 1000 uA
 * (rpm_vreg_lpm_max_uA, LOAD_THRESHOLD_STEP). skipjack-stock.dts: hpm-min-load is 10000 uA for
 * l1..l12, l17, l18 and 5000 uA for l13..l16. So the stock LPM votes are 9 mA and 4 mA, and
 * those are what is sent here -- not the 10 mA this file used to send (= the HPM threshold on
 * most rails, above it on l13/l16) and not an invented 1 mA. -DSYS_PC_SLEEP_MA=<n> forces one
 * value on every rail (10 = the old behaviour). */
static uint32_t sleep_ma_for(uint32_t id)
{
#if defined(SYS_PC_SLEEP_MA)
    (void)id; return (uint32_t)(SYS_PC_SLEEP_MA);
#else
    return (id >= 13u && id <= 16u) ? 4u : 9u;
#endif
}
static int sleep_vote_ldo(const char *what, uint32_t id, uint32_t uv, uint32_t ma)
{
    ma = sleep_ma_for(id);
    uint32_t kv[9] = { K_SWEN, 4, 1, K_UV, 4, uv, K_MA, 4, ma };
    int rc = rpm_smd_request(RPM_SET_SLEEP, T_LDOA, id, kv, sizeof kv);
    con_puts("sys-pc: sleep-set "); con_puts(what); con_puts(" rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts(rc ? " (neg)\n" : "\n");
    return rc;
}
#endif

int sys_pc8909_init(void)
{
    if (s_ready) return 1;
    if (rpm_smd_init() < 0) { con_puts("sys-pc: no RPM channel\n"); return 0; }
    int bad = 0;
#if defined(SYS_PC_NO_SLEEP_SET)
    /* chime64 (user): the app sound dies right after the "sys-pc: sleep-set ..." burst. Send none of
     * it -- no sleep-set LDO votes, no cxo sleep vote -- and see if the sound survives. */
    con_puts("sys-pc: sleep-set votes SKIPPED (SYS_PC_NO_SLEEP_SET)\n");
#else
#if defined(PLAT_SYS_PC_SLEEP_LDOS)
    /* Board-supplied sleep set (2026-09-10). The list below is PM8916 rail
     * NUMBERS; on a PM660 the same numbers are different rails (l13 = eMMC I/O
     * + touch I2C, l12 = DSI vddio + USB 1.8 V on the Gen 5), so a board whose
     * PMIC differs names its own rails from its own DT. */
    { static const struct { const char *what; uint32_t id, uv; } k[] = { PLAT_SYS_PC_SLEEP_LDOS };
      for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) bad |= sleep_vote_ldo(k[i].what, k[i].id, k[i].uv, 10u); }
#else
    /* voltages = the DTS regulator nodes (regulator-min-microvolt) */
#if defined(SYS_PC_S3_KEEP)
    bad |= sleep_vote_ldo("l2 1.2V (dsi/usb phy)", 2u,  1200000u, 10u);
#endif
    bad |= sleep_vote_ldo("l5 1.8V (emmc io)",      5u,  1800000u, 10u);
    bad |= sleep_vote_ldo("l6 1.8V (panel io)",     6u,  1800000u, 10u);
    bad |= sleep_vote_ldo("l7 1.8V (xo/usb)",       7u,  1800000u, 10u);
    bad |= sleep_vote_ldo("l8 2.9V (emmc)",         8u,  2900000u, 10u);
    bad |= sleep_vote_ldo("l17 2.85V (touch/sens)", 17u, 2850000u, 10u);
    bad |= sleep_vote_ldo("l18 2.8V (panel)",       18u, 2800000u, 10u);
    /* v198: rails the C2 DTS hangs peripherals on that were dropped by Vdd-min
     * (the gauge/charger I2C side went dead after the first XO-shutdown sleep):
     * l13 3.075 V = HSUSB 3p3 + mic bias, l16 1.8 V = touch I2C, l11/l12 1.8 V
     * = sdhci2 vdd/io (+ tpiu/qpdi). 10 mA LPM each. */
    bad |= sleep_vote_ldo("l13 3.075V (usb 3p3)",   13u, 3075000u, 10u);
    bad |= sleep_vote_ldo("l16 1.8V (touch i2c)",   16u, 1800000u, 10u);
    bad |= sleep_vote_ldo("l11 1.8V (sdhci2 vdd)",  11u, 1800000u, 10u);
    bad |= sleep_vote_ldo("l12 1.8V (sdhci2 io)",   12u, 1800000u, 10u);
#if !defined(SYS_PC_S3_KEEP)
    /* 2026-09-13: S3 is OFF in real sleep on the stock C2+. The floor ladder
     * voted it off in the ACTIVE set (set 0), which the RPM overrides while the
     * APSS is awake ("STILL ON: the vote lost"); only the SLEEP set can drop it.
     * We never voted S3 in set 1 and kept l2 (an S3 child) on in the sleep set,
     * so the RPM held S3 up through vdd-min. -DSYS_PC_S3_KEEP restores old. */
    { uint32_t kv[9] = { K_SWEN, 4, 0u, K_UV, 4, 1300000u, K_MA, 4, 0u };
      int rc = rpm_smd_request(RPM_SET_SLEEP, 0x61706d73u /* "smpa" */, 3u, kv, sizeof kv);
      con_puts("sys-pc: sleep-set s3 off rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts("\n");
      bad |= rc; }
#endif
#endif
#if defined(SYS_PC_XO_SHUTDOWN)
    { uint32_t kv[3] = { K_ENAB, 4, 0u };           /* STAGE B: release the crystal -> RPM may enter XO shutdown / Vdd-min */
#else
    { uint32_t kv[3] = { K_ENAB, 4, 1u };           /* STAGE A: keep the crystal */
#endif
      int rc = rpm_smd_request(RPM_SET_SLEEP, T_CLK0, 0u, kv, sizeof kv);
      con_puts("sys-pc: sleep-set cxo Enab "); con_putdec(kv[2]); con_puts(" rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts("\n");
      bad |= rc; }
#endif /* SYS_PC_NO_SLEEP_SET */
    s_votes_ok = (bad == 0);
    mpm_report();                                    /* vMPM write probe -> g_mpm_ram_live */
    rpm_master_stats_line("init");
#if defined(MSS_SAW_SCAN)
    mss_saw_scan();
#endif
#if defined(MSS_BOOT)
    mss_boot_start();
#endif
#if defined(MSS_PROBE)
    mss_probe("mss-before");
    /* Hold the Q6 in reset the way pil-q6v5 shutdown ends (restart_reg = 1).
     * Line flushed first: an XPU reset right here names this write. */
    con_puts("mss-probe: asserting MSS restart_reg=1 ...\n"); con_flush(); usb_poll();
    mmio_write(MSS_RESTART_REG, mmio_read(MSS_RESTART_REG) | 1u);
    timer_delay_ms(200u);
    mss_probe("mss-after-reset");
#endif
    gic_handoff_report();
#if defined(SYS_PC_WARM_RESET_DIAG)
    /* A PS_HOLD drop becomes a WARM reset: IMEM/DDR breadcrumbs and the TZ
     * diag ring survive, so the next boot can say where the wake died. */
    pon_ps_hold_warm();
    con_puts("sys-pc: PS_HOLD reset type -> WARM (diagnostic build)\n");
#endif
    s_ready = 1;
#if defined(SYS_PC_NO_SLEEP_SET)
    con_puts("sys-pc: ready\n");
#else
    con_puts(s_votes_ok ? "sys-pc: ready\n" : "sys-pc: some sleep-set votes were refused (see above)\n");
#endif
    return 1;
}

static uint32_t s_l2sts0;
static uint32_t s_ipc0;
/* deadline_ms = plat_suspend's wake deadline (timer_ms domain), 0 = none. */
int sys_pc8909_prepare(uint64_t deadline_ms)
{
    if (!s_ready) (void)sys_pc8909_init();
    if (!spm_l2_ready()) { con_puts("sys-pc: no L2 SAW\n"); return 0; }
    /* Timed wake the RPM can honour with the cluster (and later the XO) off:
     * the vMPM 64-bit word in absolute QTimer ticks, what msm_mpm_enter_sleep
     * writes. CNTPCT == CNTVCT here (cpu-pc boot line). The RTC alarm stays
     * armed as the backup. */
    uint64_t wake = 0;
    if (deadline_ms) {
        uint64_t now_ms = timer_ms();
        uint64_t left = deadline_ms > now_ms ? deadline_ms - now_ms : 1u;
        wake = timer_ticks() + left * (timer_freq_hz() / 1000u);
    }
    if (!mpm_arm_pmic_wake(wake)) { con_puts("sys-pc: MPM pin 62 did not arm -> plain collapse\n"); return 0; }
    /* v171: the RPM's wake interrupt into the GIC (vendor: must be enabled). */
    mpm_ipc_irq_arm();
    s_ipc0 = g_mpm_irq_n;
#if !defined(SYS_PC_S3_KEEP) && !defined(PLAT_SYS_PC_SLEEP_LDOS)
    /* 2026-09-13: the init-time vote prints ~20 s into boot only, so no sleep
     * log ever showed it. Re-send per sleep (idempotent) and log it here.
     * The awake EN bit cannot prove anything: the RPM keeps s3 on in the
     * active set; only the sleep set applies inside the collapse. */
    { uint32_t kv[9] = { K_SWEN, 4, 0u, K_UV, 4, 1300000u, K_MA, 4, 0u };
      int rc = rpm_smd_request(RPM_SET_SLEEP, 0x61706d73u /* "smpa" */, 3u, kv, sizeof kv);
      con_puts("sys-pc: sleep entry: sleep-set s3 off rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts(rc ? " (REFUSED)\n" : "\n"); }
#endif
    gicd_save();
    s_l2sts0 = spm_l2_sts();
    /* BISECT (v154): the first full attempt reset the watch at wake (PON
     * HARD_RESET, resume never entered). Stage knob:
     *   0 = TZ flag L2_OFF only, L2 SAW left as at boot (ret, SPM_EN)
     *   1 = + L2 SAW in PC_MODE without SLP_CMD (cluster off, no RPM handshake)
     *   2 = + SLP_CMD (RPM notified, sleep set applied)  -- the v153 path
     *   6 = KERNEL-FAITHFUL (v171, from the msm-3.18 sources, not memory):
     *       L2 SAW ctl = SPM_EN | PC_MODE | start(pc) (SLP_CMD stripped:
     *       no qcom,supports-rpm-hs), CPU0 SAW = pc with SLP_CMD stripped
     *       (= our mode 1), TZ flag MSM_SCM_L2_GDHS (3) because the level is
     *       qcom,no-cache-flush, CPU1 down through TERMINATE_PC so TZ's core
     *       count is right, MPM IPC irq enabled, vMPM timer = the deadline. */
#ifndef SYS_PC_STAGE
#define SYS_PC_STAGE 6
#endif
#if SYS_PC_STAGE == 1
    spm_l2_mode_noen(1);                             /* gdhs/pc-mode sequence, no slp_cmd */
    g_cpu_pc_l2_off = 1u;
#elif SYS_PC_STAGE == 3
    spm_l2_mode_noen(1);                             /* gdhs sequence + TZ flag MSM_SCM_L2_GDHS (3): the kernel's idle combo */
    g_cpu_pc_l2_off = 3u;
#elif SYS_PC_STAGE == 5
    spm_l2_mode_noen_noslp(2);
    g_cpu_pc_l2_off = 3u;
#elif SYS_PC_STAGE == 6
    spm_l2_mode_kernel_pc();
    g_cpu_pc_l2_off = 3u;
#elif SYS_PC_STAGE == 7
    spm_l2_mode_kernel_gdhs();                       /* kernel l2-gdhs level: no RPM handshake, no APC rail cmds */
    g_cpu_pc_l2_off = 3u;
#elif SYS_PC_STAGE == 8
    spm_l2_mode_kernel_pc();                         /* kernel l2-pc ctl, but TZ flag L2_OFF (1): TZ flushes the L2 itself */
    g_cpu_pc_l2_off = 1u;
#elif SYS_PC_STAGE == 10
    /* TZ INTERVIEW: flag 3 (GDHS) with the L2 SAW ctl 0, so the cluster never
     * powers down and TZ warm-boots us (stage 0 proved that). Afterwards we
     * dump TZ's diag ring + counters: what TZ did with a GDHS request. */
    spm_l2_ctl_zero();
    g_cpu_pc_l2_off = 3u;
#elif SYS_PC_STAGE == 9
    spm_l2_mode_noen_noslp(2);                       /* kernel l2-pc ctl minus SPM_EN: the literal "TZ enables it" reading */
    g_cpu_pc_l2_off = 3u;
#elif SYS_PC_STAGE == 4
    g_cpu_pc_slp_cmd = 1u;
    g_cpu_pc_l2_off = 1u;
#elif SYS_PC_STAGE >= 2
    spm_l2_mode_noen(2);                             /* pc + slp_cmd, no SPM_EN (TZ) */
    g_cpu_pc_l2_off = 1u;
#else
    g_cpu_pc_l2_off = 1u;
#endif
    tz_boot_counters("pre-collapse");
    rpm_master_stats_line("pre-collapse");
    con_puts("sys-pc: armed (stage "); con_putdec(SYS_PC_STAGE); con_puts("): mpm pin 62, ipc irq 203 on, vmpm wake=");
    con_puthex((uint32_t)(wake >> 32)); con_puthex((uint32_t)wake);
    con_puts(", gicd saved, l2ctl="); con_puthex(spm_l2_ctl());
    con_puts(" -> TERMINATE_PC(flag "); con_putdec(g_cpu_pc_l2_off); con_puts(")\n"); con_flush();
    return 1;
}

void sys_pc8909_finish(void)
{
    if (!g_cpu_pc_l2_off) return;
    g_cpu_pc_l2_off = 0u;
    g_cpu_pc_slp_cmd = 0u;
    uint32_t s0 = mpm_status_word(0), s1 = mpm_status_word(1);
    mpm_disarm_pmic_wake();
    mpm_ipc_irq_disarm();
    mpm_status_clear();
    uint32_t l2c = spm_l2_ctl(), l2s = spm_l2_sts();
#if SYS_PC_STAGE >= 6
    spm_l2_ctl_zero();                               /* kernel: default level "l2-cache-active" -> ctl 0 */
#else
    spm_l2_mode_noen(0);
#endif
    con_puts("sys-pc: back: l2sts "); con_puthex(s_l2sts0); con_puts(" -> "); con_puthex(l2s);
    con_puts(" l2ctl "); con_puthex(l2c);
    pon_crumb_write(0x30u);
    con_puts(" mpm-status "); con_puthex(s0); con_puts("/"); con_puthex(s1);
    con_puts(" ipc-irqs="); con_putdec(g_mpm_irq_n - s_ipc0); if (g_mpm_irq_storm) con_puts("(STORM, disabled)");
    con_puts(" ipc-sts="); con_puthex(g_mpm_last_sts0); con_puts("/"); con_puthex(g_mpm_last_sts1);
    { extern uint32_t g_gicd_restored; con_puts(" gicd-restored="); con_putdec(g_gicd_restored); }
    con_puts("\n");
    tz_boot_counters("post-resume");
    rpm_master_stats_line("post-resume");
#if SYS_PC_STAGE == 10
    con_puts("tz-log tail after TERMINATE_PC(3) with the L2 SAW idle:\n"); tz_log_tail(1200u); con_puts("\n");
#endif
}

#endif /* PLAT_SOC_MSM8909 */
