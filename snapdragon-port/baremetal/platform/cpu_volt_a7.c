/* cpu_volt_a7.c — CPU rail (VDD_APC) control for the Fossil Gen 4.
 *
 * WHAT THIS IS FOR. cpu_clk_a7.c can drop the A7 to 400 or 200 MHz, but on its
 * own that only saves the frequency term of the power equation: the rail stays
 * wherever aboot left it, which is the voltage its own boot frequency needed.
 * Dynamic power goes as V^2*f, so running 200 MHz at 1.225 V instead of the
 * 1.05 V that frequency actually requires throws away more than the frequency
 * drop gained. This file supplies the missing half.
 *
 * WHERE THE RAIL LIVES — and it is NOT the RPM, contrary to an earlier note in
 * this port. The chain, entirely from this watch's own DTB:
 *
 *     cpu-vdd-supply -> qcom,cpr-regulator "apc_corner"   (rbcpr @0xb018000,
 *                                                          efuse_addr 0x58000)
 *                       vdd-apc-supply -> qcom,spm-regulator "8916_s2"
 *                                         @0x1700 on qcom,pm8916@1  (SID 1)
 *
 * So it is an SMPS on the PMIC, reachable over the same SPMI arbiter this port
 * already drives for the vibrator, the PON reboot reason and the fuel gauge.
 * spm_regulator_write_voltage() prefers msm_spm_set_vdd() but FALLS BACK to a
 * plain SPMI byte write to base + QPNP_SMPS_REG_VOLTAGE_SETPOINT (0x41); that
 * fallback is what this implements. (Consequence to remember: if CPU power
 * collapse is ever ported, the SPM would restore its own vlevel and override
 * whatever we wrote here.)
 *
 * WHAT VOLTAGE TO USE — the chip already knows, per die.
 * Guessing at millivolts is exactly what HARDWARE.md warns against (the
 * ESP32-S3's dig_dbias trim does not port; pick a characterised corner). We do
 * not have to guess: CPR's fuses store the factory-measured voltage for THIS
 * die. cpr_pvs_per_corner_init() in cpr-regulator.c:
 *
 *     efuse_bits = read(row, lsb, width)
 *     sign  = (efuse_bits & BIT(width-1)) ? -1 : 1
 *     steps = efuse_bits & (BIT(width-1) - 1)
 *     pvs_corner_v[i] = ref_uv[i] + sign * steps * step_size_uv
 *     ... then clamped to [fuse_floor, fuse_ceiling]
 *
 * with this watch's properties:
 *     qcom,cpr-fuse-init-voltage = <0x1a 0x24 0x06 0>, <0x1a 0x12 0x06 0>,
 *                                  <0x1a 0x00 0x06 0>   (row 26, bits 36/18/0)
 *     qcom,cpr-init-voltage-step = <0x2710>             (10 mV per step)
 *     qcom,cpr-init-voltage-ref  = ceiling               (1050/1225/1350 mV)
 *     qcom,cpr-voltage-floor     = 1050/1050/1155 mV
 * QFPROM rows are 8 bytes (BYTES_PER_FUSE_ROW), read straight from
 * efuse_base + row*8 when the DT's use-tz-api flag is 0 — which it is here.
 *
 * This is OPEN-LOOP CPR: the stock kernel additionally runs the closed loop,
 * trimming continuously against measured ring-oscillator quotients. We use the
 * fused starting point and stop there. It leaves a little efficiency on the
 * table and is far simpler to reason about.
 *
 * SAFETY RULES, in order of importance:
 *   1. NEVER below the DT floor for the corner. The clamp is unconditional.
 *   2. Raising frequency: voltage FIRST, then frequency. Lowering: frequency
 *      first, then voltage. Backwards leaves the core briefly underv olted at
 *      the higher clock. cpu_clk_set_mhz() owns that ordering.
 *   3. If anything about the fuse or the regulator reads implausibly, do
 *      nothing and stay at the bootloader's voltage. An undervolt fault is
 *      silent corruption, not a clean stop, so "leave it alone" is always the
 *      correct fallback.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

/* --- the SPM-regulator (PM8916 SMPS2) ----------------------------------- */
/* WHICH REGULATOR IS THE CPU RAIL — per board, from that board's own DTB.
 *
 * These were hardcoded to the PM8916's 8916_s2 at 0x1700, which is right for
 * the Fossil Gen 4 and the TicWatches and WRONG for the Fossil Gen 5: its
 * vdd-apc-supply resolves to pm660_s1 at spm-regulator@1400 on pm660@1. Since
 * the whole file is guarded on PLAT_SOC_MSM8909 -- true of the Gen 5 as well,
 * because its Wear 3100 is the same AP -- a frequency change there would have
 * written 0x1741 on a PM660, i.e. the VOLT_SET register of some unrelated
 * peripheral. Board data belongs in the board header. */
#if !defined(PLAT_APC_SID)
#define PLAT_APC_SID            1u
#endif
#if !defined(PLAT_APC_SPMI_BASE)
#define PLAT_APC_SPMI_BASE      0x1700u
#endif
#define APC_SID                 PLAT_APC_SID
#define APC_SPMI_BASE           PLAT_APC_SPMI_BASE
#define QPNP_SMPS_REG_TYPE      (APC_SPMI_BASE + 0x04u)
#define QPNP_SMPS_REG_SUBTYPE   (APC_SPMI_BASE + 0x05u)
#define QPNP_SMPS_REG_VOLT_RANGE (APC_SPMI_BASE + 0x40u)
#define QPNP_SMPS_REG_VOLT_SET  (APC_SPMI_BASE + 0x41u)
/* FTS426 (PM660 s1) instead uses a 12-bit setpoint in MILLIVOLTS split across
 * two registers, and has no voltage-range register at all -- 0x40 is the LSB
 * of the setpoint there, not a range selector. Reading 0x40 as a "range" is
 * what made the Gen 5 report the rail as n/a. */
#define QPNP_FTS426_REG_VOLT_LSB  (APC_SPMI_BASE + 0x40u)
#define QPNP_FTS426_REG_VOLT_MSB  (APC_SPMI_BASE + 0x41u)
#define FTS426_MIN_UV           320000
#define FTS426_MAX_UV           1352000
#define FTS426_STEP_UV          4000

/* voltage_range tables from spm-regulator.c: {min_uV, set_point_min_uV,
 * max_uV, step_uV}. The PM8916's SMPS is a ULT HF buck. Range 1 uses a 5-bit
 * VSET with a different encoding, so we support range 0 only and refuse
 * otherwise rather than write a value we cannot compute correctly. */
#define ULT_HF_R0_MIN_UV        375000
#define ULT_HF_R0_MAX_UV        1562500
#define ULT_HF_R0_STEP_UV       12500

/* --- CPR fuses ----------------------------------------------------------- */
#define QFPROM_BASE             0x00058000u
#define BYTES_PER_FUSE_ROW      8u
#define CPR_FUSE_ROW            0x1Au
#define CPR_FUSE_WIDTH          6u
#define CPR_INIT_VOLT_STEP_UV   10000

/* qcom,cpr-init-voltage-ref (== ceiling) / qcom,cpr-voltage-floor, in uV,
 * indexed by FUSE corner 1..3. Index 0 unused, to match the kernel's 1-based
 * CPR_FUSE_CORNER_MIN. */
#if !defined(PLAT_CPR_CEIL_UV)
#define PLAT_CPR_CEIL_UV  { 0, 1050000, 1225000, 1350000 }
#endif
#if !defined(PLAT_CPR_FLOOR_UV)
#define PLAT_CPR_FLOOR_UV { 0, 1050000, 1050000, 1155000 }
#endif
static const int k_ceiling_uv[4] = PLAT_CPR_CEIL_UV;
static const int k_floor_uv[4]   = PLAT_CPR_FLOOR_UV;
/* bit offset of each corner's 6-bit field within fuse row 0x1a */
static const uint8_t k_fuse_lsb[4] = { 0, 0, 18, 36 };

static int s_pvs_uv[4];      /* resolved per-die voltages, 0 = not resolved */
static int s_resolved;

static uint64_t qfprom_row(uint32_t row)
{
    uintptr_t a = (uintptr_t)QFPROM_BASE + (uintptr_t)row * BYTES_PER_FUSE_ROW;
    uint32_t lo = mmio_read(a);
    uint32_t hi = mmio_read(a + 4u);
    return ((uint64_t)hi << 32) | lo;
}

/* ---- MANUAL UNDERVOLT ---------------------------------------------------
 * A user-driven offset, in whole steps of the rail's own granularity, applied
 * on top of the CPR-resolved voltage for whatever corner is selected. It is
 * allowed BELOW the DT floor -- that is the entire point of it, and it is why
 * it is a manual control rather than something the port decides on its own.
 *
 * The DT floor is the voltage Qualcomm guarantees for the WORST die that
 * passes binning at that corner, across the whole temperature range, with
 * margin. Most individual dies are stable well under it; how far under is a
 * property of the specific chip and can only be found by trying. What the
 * floor really buys is that a part which is fine on the bench is still fine
 * at -20 C with a hot radio and an aged battery, so anything found by hand
 * here should be backed off from the edge, not left at it.
 *
 * Two things bound it:
 *   - UV_MAX_STEPS caps how far the UI can go at all;
 *   - UV_HARD_FLOOR_UV is an absolute refusal point no offset can cross,
 *     so a long press on the minus button cannot walk the rail into a region
 *     where the SPMI write itself becomes unreliable.
 * Neither is a stability claim. Below the DT floor the only test is the watch
 * staying up, and the failure mode is a hang or silent corruption, not a
 * clean error -- so settings_store.h arms this on a one-boot probation and
 * throws it away if the watch does not survive to confirm it. */
#if !defined(PLAT_CPU_UV_STEP_UV)
#define PLAT_CPU_UV_STEP_UV     ULT_HF_R0_STEP_UV      /* 12.5 mV on PM8916 */
#endif
#if !defined(PLAT_CPU_UV_MAX_STEPS)
/* -500 mV of range, and a hard floor at 550 mV. Both are set to be REACHABLE
 * rather than conservative: the point of the control is to find where this
 * particular die stops working, and a limit that stops short of the edge just
 * hides it. From the 1050 mV corner-1 voltage the bottom of the range is
 * 550 mV, which no msm8909w will run at -- so the stepper is guaranteed to be
 * able to reach a crash, which is the only way to prove the writes are real.
 * The rail's own encoding refuses below 375 mV regardless. */
#define PLAT_CPU_UV_MAX_STEPS   40                     /* -500 mV at 12.5 mV */
#endif
#if !defined(PLAT_CPU_UV_HARD_FLOOR_UV)
#define PLAT_CPU_UV_HARD_FLOOR_UV 550000
#endif
static int s_uv_steps;       /* <= 0; each step is PLAT_CPU_UV_STEP_UV */
static int s_last_mhz;       /* frequency of the last successful set */

static int s_fuse_ok;        /* 1 = the fuse row read plausibly */
static int s_last_corner;    /* CPR fuse corner of the last successful set */

/* Resolve the per-die open-loop voltage for each fuse corner. Idempotent.
 *
 * WHEN THE FUSE DOES NOT READ. QFPROM is a plain MMIO window here (the DT's
 * use-tz-api flag is 0), but an unmapped or access-denied read comes back as
 * all-ones rather than faulting. All-ones decodes as sign=-1, steps=31, i.e.
 * "subtract 310 mV from the ceiling", which then clamps to the FLOOR on every
 * corner -- a uniform, plausible-looking undervolt that is not this die's
 * characterised voltage at all. Treat an all-ones or all-zero row as no fuse
 * data and fall back to the CEILING, which is the DT's own safe value and
 * exactly what the stock driver uses before its closed loop starts trimming.
 * A missing fuse must cost efficiency, never margin. */
static void cpu_volt_resolve(void)
{
    if (s_resolved) return;
    s_resolved = 1;

    uint64_t row = qfprom_row(CPR_FUSE_ROW);
    s_fuse_ok = (row != 0ull && row != ~0ull);
    if (!s_fuse_ok) {
        for (unsigned i = 1; i <= 3; i++) s_pvs_uv[i] = k_ceiling_uv[i];
        con_puts("cpu-volt: CPR fuse row unreadable; using DT ceilings\n");
        return;
    }

    for (unsigned i = 1; i <= 3; i++) {
        uint32_t bits = (uint32_t)((row >> k_fuse_lsb[i]) &
                                   ((1u << CPR_FUSE_WIDTH) - 1u));
        int sign  = (bits & (1u << (CPR_FUSE_WIDTH - 1u))) ? -1 : 1;
        int steps = (int)(bits & ((1u << (CPR_FUSE_WIDTH - 1u)) - 1u));
        int uv = k_ceiling_uv[i] + sign * steps * CPR_INIT_VOLT_STEP_UV;

        if (uv > k_ceiling_uv[i]) uv = k_ceiling_uv[i];
        if (uv < k_floor_uv[i])   uv = k_floor_uv[i];
        s_pvs_uv[i] = uv;

        bdiag_puts("cpu-volt: fuse corner "); bdiag_putdec(i);
        bdiag_puts(" bits=");   bdiag_puthex(bits);
        bdiag_puts(" -> ");     bdiag_putdec((uint32_t)(uv / 1000));
        bdiag_puts(" mV (ceiling ");
        bdiag_putdec((uint32_t)(k_ceiling_uv[i] / 1000)); bdiag_puts(")\n");
    }
}

/* Live setpoint in mV, or -1: the decode that matches this board's part. */
static int cpu_volt_mv_raw(void)
{
#if defined(PLAT_APC_FTS426)
    uint8_t lo = 0, hi = 0;
    if (spmi_read8(APC_SID, QPNP_FTS426_REG_VOLT_LSB, &lo) < 0) return -1;
    if (spmi_read8(APC_SID, QPNP_FTS426_REG_VOLT_MSB, &hi) < 0) return -1;
    return (int)(((uint32_t)hi << 8) | lo);      /* already millivolts */
#else
    uint8_t range = 0, vset = 0;
    if (spmi_read8(APC_SID, QPNP_SMPS_REG_VOLT_RANGE, &range) < 0) return -1;
    if (range != 0) return -1;                   /* range 1 has its own encoding */
    if (spmi_read8(APC_SID, QPNP_SMPS_REG_VOLT_SET, &vset) < 0) return -1;
    return (int)((vset * ULT_HF_R0_STEP_UV + ULT_HF_R0_MIN_UV) / 1000);
#endif
}

/* Write the rail. Returns the voltage actually programmed in uV, or <0. */
static int cpu_volt_set_uv(int uv)
{
#if defined(PLAT_CPU_VOLT_READONLY)
    /* Rail reported but never written on this board (no verified setpoint
     * encoding). Lowering the clock at the boot voltage is always safe. */
    (void)uv;
    return -1;
#elif defined(PLAT_APC_FTS426)
    /* FTS426: 12-bit setpoint in mV, LSB then MSB. Round the request UP to the
     * part's 4 mV granularity so a rounding error can only ever land ABOVE the
     * characterised voltage. */
    if (uv < FTS426_MIN_UV || uv > FTS426_MAX_UV) return -1;
    uint32_t mv = (uint32_t)((uv + 999) / 1000);
    mv = ((mv + (FTS426_STEP_UV / 1000) - 1u) / (FTS426_STEP_UV / 1000))
         * (FTS426_STEP_UV / 1000);
    if (mv > 0xFFFu) return -1;

    /* The setpoint spans two registers, so it is briefly INCONSISTENT between
     * the two writes. Write the MSB first when raising and last when lowering,
     * so the half-written value is never lower than both the old and the new
     * one -- an intermediate that is too high is harmless, one that is too low
     * is an undervolt. */
    uint8_t lo = (uint8_t)(mv & 0xFFu), hi = (uint8_t)((mv >> 8) & 0xFFu);
    int cur = cpu_volt_mv_raw();
    int raising = (cur < 0) || ((int)mv > cur);
    if (raising) {
        if (spmi_write8(APC_SID, QPNP_FTS426_REG_VOLT_MSB, hi) < 0) return -1;
        if (spmi_write8(APC_SID, QPNP_FTS426_REG_VOLT_LSB, lo) < 0) return -1;
    } else {
        if (spmi_write8(APC_SID, QPNP_FTS426_REG_VOLT_LSB, lo) < 0) return -1;
        if (spmi_write8(APC_SID, QPNP_FTS426_REG_VOLT_MSB, hi) < 0) return -1;
    }
    timer_delay_ms(2);          /* let the buck ramp before anything leans on it */

    /* Confirm the part took it. If the SPM owns the rail our bytes can be
     * overwritten, and a silently-ignored undervolt request is fine but a
     * silently-ignored value we then BELIEVE is not. */
    int got = cpu_volt_mv_raw();
    if (got < 0 || (uint32_t)got != mv) return -1;
    return got * 1000;
#else
    uint8_t range = 0;

    if (uv < ULT_HF_R0_MIN_UV || uv > ULT_HF_R0_MAX_UV) return -1;
    if (spmi_read8(APC_SID, QPNP_SMPS_REG_VOLT_RANGE, &range) < 0) {
        con_puts("cpu-volt: range read failed; leaving the rail alone\n");
        return -1;
    }
    if (range != 0) {
        con_puts("cpu-volt: SMPS not in range 0 (range=");
        con_puthex(range); con_puts("); refusing\n");
        return -1;
    }

    /* vlevel = DIV_ROUND_UP(uV - min_uV, step_uV) — rounding UP is what keeps
     * a rounding error on the safe side of the requested voltage. */
    uint32_t vlevel = (uint32_t)((uv - ULT_HF_R0_MIN_UV + ULT_HF_R0_STEP_UV - 1)
                                 / ULT_HF_R0_STEP_UV);
    if (vlevel > 0xFFu) return -1;

    if (spmi_write8(APC_SID, QPNP_SMPS_REG_VOLT_SET, (uint8_t)vlevel) < 0) {
        con_puts("cpu-volt: setpoint write failed\n");
        return -1;
    }
    /* Let the buck ramp before anything depends on the new level. The kernel
     * computes this from a measured step rate; a flat 2 ms dwarfs any change
     * we make here (at most 300 mV) and costs nothing at a UI event. */
    timer_delay_ms(2);
    return (int)(vlevel * ULT_HF_R0_STEP_UV + ULT_HF_R0_MIN_UV);
#endif
}

/* Map one of cpu_clk_a7.c's frequencies to its CPR FUSE corner.
 * qcom,cpr-corner-frequency-map gives virtual corners 1..9 =
 *   200, 400, 533.33, 800, 998.4, 1094.4, 1190.4, 1248, 1267.2 MHz
 * and qcom,cpr-corner-map = <1 1 2 2 3 3 3 3 3> folds those onto the three
 * FUSE corners. Our ladder only reaches virtual corners 1-4. */
static int fuse_corner_for_mhz(int mhz)
{
    if (mhz <= 400) return 1;    /* virtual 1-2 -> fuse 1 */
    if (mhz <= 800) return 2;    /* virtual 3-4 -> fuse 2 */
    return 3;
}

/* Public: set the rail to the characterised voltage for this frequency.
 * Returns programmed uV, or <0 if nothing was changed. */
int cpu_volt_set_for_mhz(int mhz)
{
    cpu_volt_resolve();
    int fc = fuse_corner_for_mhz(mhz);
    if (fc < 1 || fc > 3 || s_pvs_uv[fc] == 0) return -1;

    /* The manual offset rides on top of the corner voltage, so it survives
     * every later frequency change without the caller having to re-apply it. */
    int want = s_pvs_uv[fc] + s_uv_steps * PLAT_CPU_UV_STEP_UV;
    if (want < PLAT_CPU_UV_HARD_FLOOR_UV) want = PLAT_CPU_UV_HARD_FLOOR_UV;

    int got = cpu_volt_set_uv(want);
    if (got > 0) {
        s_last_corner = fc;
        s_last_mhz    = mhz;
        bdiag_puts("cpu-volt: "); bdiag_putdec((uint32_t)mhz);
        bdiag_puts(" MHz -> fuse corner "); bdiag_putdec((uint32_t)fc);
        bdiag_puts(", rail "); bdiag_putdec((uint32_t)(got / 1000));
        bdiag_puts(" mV\n");
    }
    return got;
}

/* Manual undervolt, in steps of the rail's granularity. Negative lowers the
 * voltage; 0 is the characterised corner value. Returns the offset actually
 * in force after clamping, and re-programs the rail immediately so the change
 * is felt (and seen) at once rather than at the next frequency change. */
int cpu_volt_set_uv_steps(int steps)
{
    if (steps > 0) steps = 0;
    if (steps < -PLAT_CPU_UV_MAX_STEPS) steps = -PLAT_CPU_UV_MAX_STEPS;
    s_uv_steps = steps;
    if (s_last_mhz) (void)cpu_volt_set_for_mhz(s_last_mhz);
    return s_uv_steps;
}
int cpu_volt_get_uv_steps(void)  { return s_uv_steps; }
int cpu_volt_uv_step_uv(void)    { return PLAT_CPU_UV_STEP_UV; }
int cpu_volt_uv_max_steps(void)  { return PLAT_CPU_UV_MAX_STEPS; }

/* Which CPR fuse corner the last frequency selected, or 0 if the rail has
 * never been set. Negated when the fuse row did not read and the DT ceilings
 * are standing in for it, so the Power app can say so in one character. */
int cpu_volt_corner(void)
{
    if (!s_last_corner) return 0;
    return s_fuse_ok ? s_last_corner : -s_last_corner;
}

/* Raw VOLTAGE_SETPOINT byte of the APC rail, -1 on error: the "vlevel"
 * spm-regulator.c hands to msm_spm_set_vdd() at probe. On an FTS426 this is
 * the LOW byte of the 12-bit millivolt setpoint. Used by spm_8909.c. */
int cpu_volt_vset_raw(void)
{
    uint8_t vset = 0;
    if (spmi_read8(APC_SID, QPNP_SMPS_REG_VOLT_SET, &vset) < 0) return -1;
    return (int)vset;
}

/* Identify the APC regulator for the Power app's diagnostic line:
 *   (TYPE << 24) | (SUBTYPE << 16) | (0x40 byte << 8) | (0x41 byte), or -1.
 * TYPE/SUBTYPE are what spm-regulator.c switches its voltage table on:
 * 0x1C/0x0A is an FTS426 (PM660 s1), and the PM8916's ULT HF buck reads
 * differently. Kept after the decode was settled so the next board can be
 * identified the same way instead of by guessing from the PMIC's name. */
int cpu_volt_id_raw(void)
{
    uint8_t t = 0, st = 0, a = 0, b = 0;
    if (spmi_read8(APC_SID, QPNP_SMPS_REG_TYPE, &t) < 0) return -1;
    if (spmi_read8(APC_SID, QPNP_SMPS_REG_SUBTYPE, &st) < 0) return -1;
    if (spmi_read8(APC_SID, APC_SPMI_BASE + 0x40u, &a) < 0) return -1;
    if (spmi_read8(APC_SID, APC_SPMI_BASE + 0x41u, &b) < 0) return -1;
    return (int)(((uint32_t)t << 24) | ((uint32_t)st << 16) |
                 ((uint32_t)a << 8) | b);
}

/* Live rail voltage in mV, or -1. Reported by the power app. */
int cpu_volt_mv(void)
{
    return cpu_volt_mv_raw();
}

#endif /* PLAT_SOC_MSM8909 */
