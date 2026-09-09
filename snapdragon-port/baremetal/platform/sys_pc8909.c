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
}
extern uint32_t g_cpu_pc_slp_cmd;

static int sleep_vote_ldo(const char *what, uint32_t id, uint32_t uv, uint32_t ma)
{
    uint32_t kv[9] = { K_SWEN, 4, 1, K_UV, 4, uv, K_MA, 4, ma };
    int rc = rpm_smd_request(RPM_SET_SLEEP, T_LDOA, id, kv, sizeof kv);
    con_puts("sys-pc: sleep-set "); con_puts(what); con_puts(" rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts(rc ? " (neg)\n" : "\n");
    return rc;
}

int sys_pc8909_init(void)
{
    if (s_ready) return 1;
    if (rpm_smd_init() < 0) { con_puts("sys-pc: no RPM channel\n"); return 0; }
    int bad = 0;
    /* voltages = the DTS regulator nodes (regulator-min-microvolt) */
    bad |= sleep_vote_ldo("l2 1.2V (dsi/usb phy)", 2u,  1200000u, 10u);
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
#if defined(SYS_PC_XO_SHUTDOWN)
    { uint32_t kv[3] = { K_ENAB, 4, 0u };           /* STAGE B: release the crystal -> RPM may enter XO shutdown / Vdd-min */
#else
    { uint32_t kv[3] = { K_ENAB, 4, 1u };           /* STAGE A: keep the crystal */
#endif
      int rc = rpm_smd_request(RPM_SET_SLEEP, T_CLK0, 0u, kv, sizeof kv);
      con_puts("sys-pc: sleep-set cxo Enab "); con_putdec(kv[2]); con_puts(" rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts("\n");
      bad |= rc; }
    s_votes_ok = (bad == 0);
    mpm_report();                                    /* vMPM write probe -> g_mpm_ram_live */
    rpm_master_stats_line("init");
    gic_handoff_report();
#if defined(SYS_PC_WARM_RESET_DIAG)
    /* A PS_HOLD drop becomes a WARM reset: IMEM/DDR breadcrumbs and the TZ
     * diag ring survive, so the next boot can say where the wake died. */
    pon_ps_hold_warm();
    con_puts("sys-pc: PS_HOLD reset type -> WARM (diagnostic build)\n");
#endif
    s_ready = 1;
    con_puts(s_votes_ok ? "sys-pc: ready\n" : "sys-pc: some sleep-set votes were refused (see above)\n");
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
