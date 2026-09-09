/* spm_8909.c — SAW2 / SPM v2 programming for the msm8909w (Gen 4, C2).
 *
 * The SPM (Subsystem Power Manager) is the per-core / per-L2 sequencer that
 * actually switches power when the core executes WFI. Linux (drivers/soc/qcom/
 * spm.c + spm_devices.c, firefish 3.18 branch) loads each mode's command
 * sequence into SPM_SLP_SEQ_ENTRY at boot and, right before a sleep, writes
 * SPM_CTL = enable | mode bits | (start offset << 4). Without this the TZ
 * TERMINATE_PC call just executes a plain WFI and nothing powers down.
 *
 * Everything below is taken from the DTS nodes qcom,spm@b089000 (cpu0) and
 * qcom,spm@b012000 (system-l2) of firefish-stock.dtb (identical SoC on the
 * C2):
 *   saw2-cfg 0x01 (cpu) / 0x14 (l2), spm-dly 0x3c102800, spm-ctl 0x0e
 *   cpu  wfi [60 03 60 0b 0f]              spm_en
 *        spc 20 10 80 30 90 5b 60 03 60 3b 76 76 0b 94 5b 80 10 26 30 0f
 *                                            spm_en pc_mode
 *        pc  (same bytes)                  spm_en pc_mode slp_cmd_mode
 *   l2   ret [00 03 00 0f]                 spm_en
 *        gdhs 00 20 32 6b c0 e0 d0 42 03 50 4e 02 02 d0 e0 c0 22 6b 02 32 50 0f
 *                                            spm_en pc_mode
 *        pc  00 20 32 b0 6b c0 e0 d0 42 51 11 07 01 41 b0 50 4e 02 02 d0 e0
 *            c0 22 6b 02 32 52 0f          spm_en pc_mode slp_cmd_mode
 *   l2 pmic-data0 0x5030080, data1 0x30000, data4 0x10080, data5 0x10000
 * SPM_CTL bits (spm_devices.c): SPM_EN 0, ISAR 3, RET_MODE 15, PC_MODE 16,
 * SLP_CMD 17; sequence start offset in bits [12:4]. Register offsets for
 * SAW2 v2.1 (version 0x2010xxxx): CFG 0x08, SPM_STS 0x0C, SPM_CTL 0x30,
 * SPM_DLY 0x34, PMIC_DATA_n 0x40+4n, SEQ_ENTRY 0x80, VERSION 0xFD0;
 * v3.0 moves SEQ_ENTRY to 0x400. */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

#define SPM_CPU0  0x0B089000u
#define SPM_L2    0x0B012000u

#define R_ID      0x04u
#define R_CFG     0x08u
#define R_STS     0x0Cu
#define R_CTL     0x30u
#define R_DLY     0x34u
#define R_PMIC0   0x40u
#define R_VERSION 0xFD0u

#define CTL_SPM_EN   (1u << 0)
#define CTL_PC_MODE  (1u << 16)
#define CTL_SLP_CMD  (1u << 17)

static const uint8_t k_cpu_wfi[] = { 0x60,0x03,0x60,0x0b,0x0f };
static const uint8_t k_cpu_pc[]  = { 0x20,0x10,0x80,0x30,0x90,0x5b,0x60,0x03,0x60,0x3b,0x76,0x76,0x0b,0x94,0x5b,0x80,0x10,0x26,0x30,0x0f };
static const uint8_t k_l2_ret[]  = { 0x00,0x03,0x00,0x0f };
static const uint8_t k_l2_gdhs[] = { 0x00,0x20,0x32,0x6b,0xc0,0xe0,0xd0,0x42,0x03,0x50,0x4e,0x02,0x02,0xd0,0xe0,0xc0,0x22,0x6b,0x02,0x32,0x50,0x0f };
static const uint8_t k_l2_pc[]   = { 0x00,0x20,0x32,0xb0,0x6b,0xc0,0xe0,0xd0,0x42,0x51,0x11,0x07,0x01,0x41,0xb0,0x50,0x4e,0x02,0x02,0xd0,0xe0,0xc0,0x22,0x6b,0x02,0x32,0x52,0x0f };

struct spm { uint32_t base, seq_off; uint32_t ctl[3]; int ok; };
static struct spm s_cpu0, s_cpu1, s_cpu2, s_cpu3, s_l2;
#define SPM_CPU1  0x0B099000u
#define SPM_CPU2  0x0B0A9000u
#define SPM_CPU3  0x0B0B9000u

static uint32_t rd(uint32_t b, uint32_t o) { return mmio_read(b + o); }
static void     wr(uint32_t b, uint32_t o, uint32_t v) { mmio_write(b + o, v); __asm__ volatile("dsb sy" ::: "memory"); }

/* Write one sequence at byte offset *off (kernel msm_spm_drv_write_seq_data,
 * without the shadow: read-modify-write the partial word). Returns the start
 * offset of this sequence. */
static uint32_t seq_write(struct spm *s, const uint8_t *cmd, unsigned n, uint32_t *off)
{
    uint32_t start = *off;
    for (unsigned i = 0; i < n; i++, (*off)++) {
        uint32_t w = *off / 4u, sh = (*off % 4u) * 8u;
        uint32_t v = rd(s->base, s->seq_off + 4u * w);
        v = (v & ~(0xFFu << sh)) | ((uint32_t)cmd[i] << sh);
        wr(s->base, s->seq_off + 4u * w, v);
    }
    return start;
}

static int spm_dev_init(struct spm *s, uint32_t base, uint32_t cfg, const char *name)
{
    uint32_t ver = rd(base, R_VERSION), major = ver >> 28, minor = (ver >> 16) & 0xFFF;
    s->base = base;
    s->seq_off = (major == 2u) ? 0x80u : 0x400u;
    con_puts("spm: "); con_puts(name); con_puts(" version "); con_puthex(ver);
    con_puts(" id "); con_puthex(rd(base, R_ID)); con_puts(" ctl "); con_puthex(rd(base, R_CTL));
    if (major != 2u && major != 3u) { con_puts(" -- unknown SAW, not programmed\n"); return 0; }
    (void)minor;
    /* v187: CORRECTION of v175. spm_devices.c probe writes every qcom,saw2-*
     * DTS value straight to the SAW via msm_spm_drv_upd_reg_shadow (cfg,
     * spm-dly, spm-ctl 0x0e, pmic-data0/1/4/5); the bootloader leaves ALL of
     * them at zero (C2 boot log 2026-09-09). Log the boot values, then write
     * the DTS values like the kernel does. */
    con_puts("\n  saw boot regs: cfg="); con_puthex(rd(base, R_CFG)); con_puts(" dly="); con_puthex(rd(base, R_DLY));
    con_puts(" vctl="); con_puthex(rd(base, 0x1Cu)); con_puts(" avs_ctl="); con_puthex(rd(base, 0x20u));
    con_puts(" pmic0/1/4/5="); con_puthex(rd(base, R_PMIC0)); con_puts("/"); con_puthex(rd(base, R_PMIC0 + 4u));
    con_puts("/"); con_puthex(rd(base, R_PMIC0 + 0x10u)); con_puts("/"); con_puthex(rd(base, R_PMIC0 + 0x14u));
    con_puts(" sts="); con_puthex(rd(base, R_STS));
    wr(base, R_CFG, cfg);
    wr(base, R_DLY, 0x3c102800u);
    wr(base, R_CTL, 0x0eu);                 /* qcom,saw2-spm-ctl init value; a mode write replaces it */
    /* Clear the sequence RAM we use (num entries from ID[31:24]). */
    uint32_t nent = (rd(base, R_ID) >> 24) & 0xFFu;
    if (nent == 0 || nent > 64u) nent = 32u;
    for (uint32_t i = 0; i < nent; i++) wr(base, s->seq_off + 4u * i, 0);
    s->ok = 1;
    con_puts(" ok, "); con_putdec(nent); con_puts(" seq words\n");
    return 1;
}

/* mode 0 = wfi/ret (clock gating), 1 = standalone pc / gdhs, 2 = pc (with RPM handshake) */
void spm_init(void)
{
    uint32_t off;
    if (spm_dev_init(&s_cpu0, SPM_CPU0, 0x01u, "cpu0")) {
        off = 0;
        s_cpu0.ctl[0] = CTL_SPM_EN | (seq_write(&s_cpu0, k_cpu_wfi, sizeof k_cpu_wfi, &off) << 4);
        s_cpu0.ctl[1] = CTL_SPM_EN | CTL_PC_MODE | (seq_write(&s_cpu0, k_cpu_pc, sizeof k_cpu_pc, &off) << 4);
        s_cpu0.ctl[2] = s_cpu0.ctl[1] | CTL_SLP_CMD;
        wr(SPM_CPU0, R_CTL, s_cpu0.ctl[0]);                /* idle = clock gating */
    }
    if (spm_dev_init(&s_cpu1, SPM_CPU1, 0x01u, "cpu1")) {   /* same sequences as cpu0 (DTS spm@b099000) */
        off = 0;
        s_cpu1.ctl[0] = CTL_SPM_EN | (seq_write(&s_cpu1, k_cpu_wfi, sizeof k_cpu_wfi, &off) << 4);
        s_cpu1.ctl[1] = CTL_SPM_EN | CTL_PC_MODE | (seq_write(&s_cpu1, k_cpu_pc, sizeof k_cpu_pc, &off) << 4);
        s_cpu1.ctl[2] = s_cpu1.ctl[1] | CTL_SLP_CMD;
        wr(SPM_CPU1, R_CTL, s_cpu1.ctl[0]);
    }
    /* v180: cpu2/cpu3 SAWs (DTS spm@b0a9000 / spm@b0b9000, same sequences)
     * for the park-only cores that give TZ its full cluster picture. */
#if defined(SPM_CPU23_INIT)
    con_puts("spm: probing cpu2/cpu3 SAWs at 0xb0a9000/0xb0b9000 ...\n"); con_flush(); usb_poll();
    if (spm_dev_init(&s_cpu2, SPM_CPU2, 0x01u, "cpu2")) {
        off = 0;
        s_cpu2.ctl[0] = CTL_SPM_EN | (seq_write(&s_cpu2, k_cpu_wfi, sizeof k_cpu_wfi, &off) << 4);
        s_cpu2.ctl[1] = CTL_SPM_EN | CTL_PC_MODE | (seq_write(&s_cpu2, k_cpu_pc, sizeof k_cpu_pc, &off) << 4);
        s_cpu2.ctl[2] = s_cpu2.ctl[1] | CTL_SLP_CMD;
        wr(SPM_CPU2, R_CTL, s_cpu2.ctl[0]);
    }
    if (spm_dev_init(&s_cpu3, SPM_CPU3, 0x01u, "cpu3")) {
        off = 0;
        s_cpu3.ctl[0] = CTL_SPM_EN | (seq_write(&s_cpu3, k_cpu_wfi, sizeof k_cpu_wfi, &off) << 4);
        s_cpu3.ctl[1] = CTL_SPM_EN | CTL_PC_MODE | (seq_write(&s_cpu3, k_cpu_pc, sizeof k_cpu_pc, &off) << 4);
        s_cpu3.ctl[2] = s_cpu3.ctl[1] | CTL_SLP_CMD;
        wr(SPM_CPU3, R_CTL, s_cpu3.ctl[0]);
    }
#endif
    if (spm_dev_init(&s_l2, SPM_L2, 0x14u, "l2")) {
#if !defined(SPM_NO_PMIC_DATA)
        wr(SPM_L2, R_PMIC0 + 0x00u, 0x5030080u);   /* qcom,saw2-pmic-data0/1/4/5 (device DTB) */
        wr(SPM_L2, R_PMIC0 + 0x04u, 0x30000u);
        wr(SPM_L2, R_PMIC0 + 0x10u, 0x10080u);
        wr(SPM_L2, R_PMIC0 + 0x14u, 0x10000u);
#endif
        off = 0;
        s_l2.ctl[0] = CTL_SPM_EN | (seq_write(&s_l2, k_l2_ret, sizeof k_l2_ret, &off) << 4);
        s_l2.ctl[1] = CTL_SPM_EN | CTL_PC_MODE | (seq_write(&s_l2, k_l2_gdhs, sizeof k_l2_gdhs, &off) << 4);
        s_l2.ctl[2] = CTL_SPM_EN | CTL_PC_MODE | CTL_SLP_CMD | (seq_write(&s_l2, k_l2_pc, sizeof k_l2_pc, &off) << 4);
#if !defined(SPM_NO_L2_VDD_INIT)
        { extern int cpu_volt_vset_raw(void); int v = cpu_volt_vset_raw();
          if (v >= 0) (void)spm_l2_set_vdd((uint32_t)v); else con_puts("spm-l2 set_vdd: APC VSET unreadable, skipped\n"); }
#endif
#if defined(L2_SAW_AP_ENABLE)
        wr(SPM_L2, R_CTL, s_l2.ctl[0]);
#else
        /* v155 (2026-09-07): the vendor kernel NEVER sets SPM_EN on the L2 SAW
         * from the AP (saw2-spm-ctl init 0x0e, config_low_power_mode_addr for
         * the levels); TZ enables it from the TERMINATE_PC L2 flag. With EN
         * set here the pc sequence started on its own the moment both cores
         * went idle -> PMIC reset at wake (v153/v154 stage 1). */
        wr(SPM_L2, R_CTL, s_l2.ctl[0] & ~CTL_SPM_EN);
#endif
    }
}

/* v186 (2026-09-09): what the STOCK kernel writes into the L2 SAW that we
 * never did. drivers/regulator/spm-regulator.c ("8916_s2", the APC rail,
 * qcom,cpu-vctl-list = all four cpus -> the L2 SAW is the vctl device) calls
 * msm_spm_set_vdd(cpu, vlevel) at probe, "Initialize SAW voltage control
 * register": vlevel = the rail's raw VOLTAGE_SETPOINT byte. spm.c
 * msm_spm_drv_set_vdd: RST=1, VCTL and PMIC_DATA_3 = (VCTL & ~0x700FF) |
 * vlevel | (vctl_port 0 << 16), then poll PMIC_STS until the FSM is idle
 * and the low byte reads back vlevel (vctl-timeout-us). The L2 pc/gdhs
 * sequences send PMIC data through this channel to lower the rail on the
 * way down and RESTORE it on the way up; with PMIC_DATA_3 never programmed
 * the wake half restores whatever the bootloader left there. */
#define R_PMIC_STS 0x14u
#define R_RST      0x18u
#define R_VCTL     0x1Cu
#define R_AVS_CTL  0x20u
int spm_l2_set_vdd(uint32_t vlevel)
{
    if (!s_l2.ok) return -1;
    uint32_t id = rd(SPM_L2, R_ID);
    con_puts("spm-l2 set_vdd: vlevel="); con_puthex(vlevel);
    con_puts(" arb_present="); con_putdec((id >> 2) & 1u);
    con_puts(" before: vctl="); con_puthex(rd(SPM_L2, R_VCTL)); con_puts(" pmic3="); con_puthex(rd(SPM_L2, R_PMIC0 + 0x0Cu));
    con_puts(" pmic_sts="); con_puthex(rd(SPM_L2, R_PMIC_STS)); con_puts(" avs_ctl="); con_puthex(rd(SPM_L2, R_AVS_CTL));
    if (!((id >> 2) & 1u)) { con_puts(" -> no PMIC arbiter on this SAW, skipped\n"); return -1; }
    uint32_t avs = rd(SPM_L2, R_AVS_CTL);
    if (avs & 1u) wr(SPM_L2, R_AVS_CTL, avs & ~1u);            /* kernel: disable AVS around the write */
    uint32_t data = (vlevel & 0xFFu);                             /* vctl_port 0 */
    wr(SPM_L2, R_RST, 1u);                                        /* kick the FSM back to idle */
    wr(SPM_L2, R_VCTL, (rd(SPM_L2, R_VCTL) & ~0x700FFu) | data);
    wr(SPM_L2, R_PMIC0 + 0x0Cu, (rd(SPM_L2, R_PMIC0 + 0x0Cu) & ~0x700FFu) | data);
    int left = 500; uint32_t sts = 0;                             /* source dtsi: 500 us; shipped DTB: 50 */
    do { timer_delay_us(1); sts = rd(SPM_L2, R_PMIC_STS) & 0x300FFu;
         if (((sts & 0x30000u) == 0u) && ((sts & 0xFFu) == data)) break; } while (--left);
    if (avs & 1u) wr(SPM_L2, R_AVS_CTL, avs);
    con_puts(" after: vctl="); con_puthex(rd(SPM_L2, R_VCTL)); con_puts(" pmic3="); con_puthex(rd(SPM_L2, R_PMIC0 + 0x0Cu));
    con_puts(" pmic_sts="); con_puthex(sts); con_puts(left ? " OK\n" : " TIMEOUT (wrong level)\n");
    return left ? 0 : -1;
}
int  spm_ready(void) { return s_cpu0.ok; }
void spm_cpu0_mode(int mode)
{
    if (!s_cpu0.ok) return;
    wr(SPM_CPU0, R_CTL, s_cpu0.ctl[mode < 0 ? 0 : mode > 2 ? 2 : mode]);
    (void)rd(SPM_CPU0, R_STS);
}
void spm_l2_mode(int mode)
{
    if (!s_l2.ok) return;
    wr(SPM_L2, R_CTL, s_l2.ctl[mode < 0 ? 0 : mode > 2 ? 2 : mode]);
    (void)rd(SPM_L2, R_STS);
}
void spm_cpu1_mode(int mode)
{
    if (!s_cpu1.ok) return;
    wr(SPM_CPU1, R_CTL, s_cpu1.ctl[mode < 0 ? 0 : mode > 2 ? 2 : mode]);
    (void)rd(SPM_CPU1, R_STS);
}
int  spm_cpu1_ready(void) { return s_cpu1.ok; }
/* Any core's SAW (runs ON that core for the park path). */
void spm_cpu_mode_n(unsigned cpu, int mode)
{
    struct spm *s = cpu == 0u ? &s_cpu0 : cpu == 1u ? &s_cpu1 : cpu == 2u ? &s_cpu2 : &s_cpu3;
    if (!s->ok) return;
    wr(s->base, R_CTL, s->ctl[mode < 0 ? 0 : mode > 2 ? 2 : mode]);
    (void)rd(s->base, R_STS);
}
int spm_cpu_ready_n(unsigned cpu) { struct spm *s = cpu == 0u ? &s_cpu0 : cpu == 1u ? &s_cpu1 : cpu == 2u ? &s_cpu2 : &s_cpu3; return s->ok; }
/* skipjack DTS carries qcom,lpm-wa-skip-l2-spm: the kernel programs the L2
 * SAW's start address + mode bits WITHOUT SPM_EN and lets TZ enable it from
 * the TERMINATE_PC L2 flag (msm_spm_config_low_power_mode_addr). */
void spm_l2_mode_noen(int mode)
{
    if (!s_l2.ok) return;
    wr(SPM_L2, R_CTL, s_l2.ctl[mode < 0 ? 0 : mode > 2 ? 2 : mode] & ~CTL_SPM_EN);
    (void)rd(SPM_L2, R_STS);
}
void spm_l2_mode_noen_noslp(int mode)
{
    if (!s_l2.ok) return;
    wr(SPM_L2, R_CTL, s_l2.ctl[mode < 0 ? 0 : mode > 2 ? 2 : mode] & ~(CTL_SPM_EN | CTL_SLP_CMD));
    (void)rd(SPM_L2, R_STS);
}
/* v171: EXACTLY what lpm-levels.c writes for the "l2-pc" level on this DTS
 * (spm_devices.c msm_spm_dev_set_low_power_mode, set_spm_enable=false):
 * ctl = modes[pc].ctl = SPM_EN | PC_MODE | SLP_CMD | start(pc), minus SLP_CMD
 * because qcom,supports-rpm-hs is absent (allow_rpm_hs false) and the level
 * is notify-rpm. SPM_EN IS set (the mode carries qcom,spm_en); the earlier
 * "TZ enables it" reading of the skip-l2-spm workaround was wrong: the
 * workaround only skips the separate set_spm_enable() write. The RPM
 * handshake is the 0xb0 command inside the pc sequence, not the ctl bit. */
void spm_l2_mode_kernel_pc(void)
{
    if (!s_l2.ok) return;
    wr(SPM_L2, R_CTL, s_l2.ctl[2] & ~CTL_SLP_CMD);
    (void)rd(SPM_L2, R_STS);
}
/* The kernel's "l2-gdhs" cluster level: EN | PC_MODE | start(gdhs). Same
 * power-down of the L2 logic, but no 0xb0 RPM handshake and none of the
 * 51 11 07 01 41 APC-rail commands of the pc sequence. */
void spm_l2_mode_kernel_gdhs(void)
{
    if (!s_l2.ok) return;
    wr(SPM_L2, R_CTL, s_l2.ctl[1]);
    (void)rd(SPM_L2, R_STS);
}
/* After the wake the kernel sets the default level "l2-cache-active" =
 * MSM_SPM_MODE_DISABLED, which has no mode entry -> ctl written as 0. */
void spm_l2_ctl_zero(void)
{
    if (!s_l2.ok) return;
    wr(SPM_L2, R_CTL, 0u);
    (void)rd(SPM_L2, R_STS);
}
uint32_t spm_l2_ctl(void) { return s_l2.ok ? rd(SPM_L2, R_CTL) : 0u; }
int  spm_l2_ready(void) { return s_l2.ok; }
uint32_t spm_cpu1_sts(void) { return s_cpu1.ok ? rd(SPM_CPU1, R_STS) : 0u; }
uint32_t spm_cpu0_sts(void) { return s_cpu0.ok ? rd(SPM_CPU0, R_STS) : 0u; }
uint32_t spm_l2_sts(void)   { return s_l2.ok ? rd(SPM_L2, R_STS) : 0u; }

#endif /* PLAT_SOC_MSM8909 */
