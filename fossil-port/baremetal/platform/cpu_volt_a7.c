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
#define APC_SID                 1u
#define APC_SPMI_BASE           0x1700u
#define QPNP_SMPS_REG_TYPE      (APC_SPMI_BASE + 0x04u)
#define QPNP_SMPS_REG_SUBTYPE   (APC_SPMI_BASE + 0x05u)
#define QPNP_SMPS_REG_VOLT_RANGE (APC_SPMI_BASE + 0x40u)
#define QPNP_SMPS_REG_VOLT_SET  (APC_SPMI_BASE + 0x41u)

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
static const int k_ceiling_uv[4] = { 0, 1050000, 1225000, 1350000 };
static const int k_floor_uv[4]   = { 0, 1050000, 1050000, 1155000 };
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

/* Resolve the per-die open-loop voltage for each fuse corner. Idempotent. */
static void cpu_volt_resolve(void)
{
    if (s_resolved) return;
    s_resolved = 1;

    uint64_t row = qfprom_row(CPR_FUSE_ROW);
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

/* Write the rail. Returns the voltage actually programmed in uV, or <0. */
static int cpu_volt_set_uv(int uv)
{
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
    int got = cpu_volt_set_uv(s_pvs_uv[fc]);
    if (got > 0) {
        bdiag_puts("cpu-volt: "); bdiag_putdec((uint32_t)mhz);
        bdiag_puts(" MHz -> fuse corner "); bdiag_putdec((uint32_t)fc);
        bdiag_puts(", rail "); bdiag_putdec((uint32_t)(got / 1000));
        bdiag_puts(" mV\n");
    }
    return got;
}

/* Live rail voltage in mV, or -1. Reported by the power app. */
int cpu_volt_mv(void)
{
    uint8_t range = 0, vset = 0;
    if (spmi_read8(APC_SID, QPNP_SMPS_REG_VOLT_RANGE, &range) < 0) return -1;
    if (range != 0) return -1;
    if (spmi_read8(APC_SID, QPNP_SMPS_REG_VOLT_SET, &vset) < 0) return -1;
    return (int)((vset * ULT_HF_R0_STEP_UV + ULT_HF_R0_MIN_UV) / 1000);
}

#endif /* PLAT_SOC_MSM8909 */
