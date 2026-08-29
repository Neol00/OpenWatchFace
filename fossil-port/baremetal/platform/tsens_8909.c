/* ============================================================================
 *  tsens_8909.c — die temperature on msm8909w / APQ8009W (Wear 2100).
 *
 *  Fills in what gen4_stubs.c used to answer -9999 to: the Power app's SoC
 *  temperature on the Fossil Gen 4 and the TicWatch C2. The Gen 6 reads its
 *  own TSENS in pwr_diag.c; that code is for a different TSENS generation with
 *  a different register layout and a different fuse map, so it does not carry
 *  over — this is the msm8909 block, ported from the vendor driver
 *  (drivers/thermal/msm-tsens.c, tsens_calib_msm8909_sensors()).
 *
 *  ADDRESSES, from the skipjack DTB's tsens@4a8000 (identical on firefish —
 *  it is the SoC's own block):
 *      reg = <0x4a8000 0x2000   0x5c000 0x1000>
 *              tsens_physical    tsens_eeprom_physical
 *      qcom,sensors = 5, qcom,slope = <3000 x5>
 *
 *  WHY CALIBRATION IS NOT OPTIONAL. The sensor returns a 10-bit ADC code, and
 *  the code-to-Celsius mapping is per-die: it lives in QFPROM fuses blown at
 *  test. Reading the code and applying a nominal slope gives a number that
 *  looks plausible and can be tens of degrees wrong, which is worse than no
 *  reading at all — a thermal number nobody can trust is one nobody should
 *  act on. So this decodes the real fuses, and falls back to the vendor's own
 *  "calibrationless" constants only when the fuse says uncalibrated.
 *
 *  CALIBRATION MODES (fuse field CAL_SEL):
 *      0 = none  -> point1 = 500, point2 = 780 for every sensor (vendor's
 *                   own defaults; the reading is then nominal, not per-die)
 *      2 = one point  -> per-sensor point1 only; slope stays the DT's 3000
 *      3 = two point  -> per-sensor point1 and point2; slope is computed
 *                        between the 30 degC and 120 degC calibration points
 *  Sensors 1, 3 and 4 carry small fixed corrections (the D30/D120 workarounds)
 *  that the vendor driver applies unconditionally — reproduced verbatim,
 *  because their justification is not in the source and guessing at it would
 *  only introduce error.
 * ========================================================================== */
#include "platform.h"

#if defined(PLAT_SOC_MSM8909)

#define TSENS_BASE            0x004A8000u
#define TSENS_EEPROM_BASE     0x005C000Cu - 0xCu   /* 0x5c000 */
#define TSENS_CTRL            (TSENS_BASE + 0x0000u)
#define   TSENS_EN            (1u << 0)
#define TSENS_S0_STATUS       (TSENS_BASE + 0x1030u)
#define TSENS_TRDY            (TSENS_BASE + 0x105Cu)
#define   TSENS_TRDY_MASK     (1u << 0)
#define TSENS_STATUS_TEMP_MASK 0x3FFu

/* Fuse words. TSENS_8939_EEPROM(n) = n + 0xa0 in the vendor driver, and the
 * msm8909 path reads +0x0, +0x4 and +0x3c from there. */
#define TSENS_CAL0            (0x005C0000u + 0xA0u)
#define TSENS_CAL1            (0x005C0000u + 0xA4u)
#define TSENS_CAL2            (0x005C0000u + 0xDCu)

#define TSENS_NUM_SENSORS     5u
#define TSENS_FACTOR          1000
#define TSENS_CAL_DEGC_POINT1 30
#define TSENS_CAL_DEGC_POINT2 120
#define TSENS_DT_SLOPE        3000    /* qcom,slope, used when not two-point */

#define CAL_SEL_MASK          0x00070000u
#define CAL_SEL_SHIFT         16
#define CAL_NONE              0u
#define CAL_ONE_POINT_OPT2    2u
#define CAL_TWO_POINT         3u

/* Per-sensor point-1/point-2 fuse fields (mask, shift, which word). */
static const struct { uint32_t mask; uint8_t shift; uint8_t word; } k_p1[5] = {
    { 0x0000003Fu,  0, 0 }, { 0x0003F000u, 12, 0 }, { 0x3F000000u, 24, 0 },
    { 0x000003F0u,  4, 1 }, { 0x003F0000u, 16, 1 },
};
static const struct { uint32_t mask; uint8_t shift; uint8_t word; } k_p2[5] = {
    { 0x00000FC0u,  6, 0 }, { 0x00FC0000u, 18, 0 }, { 0xC0000000u, 30, 0 },
    { 0x0000FC00u, 10, 1 }, { 0x0FC00000u, 22, 1 },
};
/* The vendor's fixed corrections, applied to sensors 1, 3 and 4 only. */
static const int k_d30_wa[5]  = { 0, 10, 0,  9,  8 };
static const int k_d120_wa[5] = { 0,  6, 0,  9, 10 };

static int s_slope[TSENS_NUM_SENSORS];
static int s_offset[TSENS_NUM_SENSORS];
static int s_inited;

static void tsens_calibrate(void)
{
    uint32_t c0 = mmio_read(TSENS_CAL0);
    uint32_t c1 = mmio_read(TSENS_CAL1);
    uint32_t c2 = mmio_read(TSENS_CAL2);
    uint32_t mode = (c2 & CAL_SEL_MASK) >> CAL_SEL_SHIFT;
    int base0 = (int)(c2 & 0x000000FFu);
    int base1 = (int)((c2 & 0x0000FF00u) >> 8);
    int p1[TSENS_NUM_SENSORS], p2[TSENS_NUM_SENSORS];
    uint32_t i;

    for (i = 0; i < TSENS_NUM_SENSORS; i++) { p1[i] = 500; p2[i] = 780; }

    if (mode == CAL_ONE_POINT_OPT2 || mode == CAL_TWO_POINT) {
        for (i = 0; i < TSENS_NUM_SENSORS; i++) {
            uint32_t w = (k_p1[i].word == 0u) ? c0 : c1;
            int raw = (int)((w & k_p1[i].mask) >> k_p1[i].shift);
            p1[i] = ((base0 + raw) << 2) - k_d30_wa[i];
        }
    }
    if (mode == CAL_TWO_POINT) {
        for (i = 0; i < TSENS_NUM_SENSORS; i++) {
            int raw;
            if (i == 2u) {
                /* Sensor 2's point-2 field straddles the two fuse words. */
                raw  = (int)((c0 & 0xC0000000u) >> 30);
                raw |= (int)((c1 & 0x0000000Fu) <<  2);
            } else {
                uint32_t w = (k_p2[i].word == 0u) ? c0 : c1;
                raw = (int)((w & k_p2[i].mask) >> k_p2[i].shift);
            }
            p2[i] = ((base1 + raw) << 2) - k_d120_wa[i];
        }
    }

    for (i = 0; i < TSENS_NUM_SENSORS; i++) {
        if (mode == CAL_TWO_POINT) {
            int num = (p2[i] - p1[i]) * TSENS_FACTOR;
            int den = TSENS_CAL_DEGC_POINT2 - TSENS_CAL_DEGC_POINT1;
            s_slope[i] = num / den;
        } else {
            s_slope[i] = TSENS_DT_SLOPE;
        }
        if (s_slope[i] == 0) s_slope[i] = TSENS_DT_SLOPE;   /* never divide by 0 */
        s_offset[i] = (p1[i] * TSENS_FACTOR) - (TSENS_CAL_DEGC_POINT1 * s_slope[i]);
    }

    con_puts("tsens: calib mode "); con_putdec(mode);
    con_puts(mode == CAL_TWO_POINT      ? " (two-point)\n" :
             mode == CAL_ONE_POINT_OPT2 ? " (one-point)\n" :
                                          " (UNCALIBRATED — nominal only)\n");
}

/* Hottest currently-readable sensor, in deci-degC. The die's hot spot is the
 * number that matters, and which sensor is hottest depends on what the SoC is
 * doing. -9999 when TSENS is not enabled or has no conversion ready — the
 * same "unknown" the caller already handles. */
int pwr_soc_temp_dc(void)
{
#if !defined(TSENS_ENABLE)
    /* OPT-IN, AND OFF BY DEFAULT — because shipping this on by default broke
     * the Power app on the first watch that ran it.
     *
     * Entering the Power app calls this function, and on the TicWatch C2 that
     * was an instant hard reset, reproducible every time. The likeliest cause
     * is the one this port has already paid for twice: touching an MSM block
     * that the application cores do not own. A non-secure access to an
     * XPU-protected or clock-gated peripheral does not return an error on this
     * SoC — it either hangs the AHB transaction or trips a violation that
     * resets the watch. TSENS at 0x4a8000 is a plausible candidate for
     * TrustZone ownership; thermal management is exactly the sort of thing the
     * secure world keeps for itself.
     *
     * Note what is NOT suspected: the QFPROM fuse reads. cpu_volt_a7.c already
     * reads CPR fuses at 0x58000 on this same watch and has done so reliably
     * since the undervolt work, so that region is proven accessible.
     *
     * The port's own rule, which I broke by wiring this straight into the app:
     * never touch a new MSM block on a live path until it is proven owned.
     * Build with -DTSENS_ENABLE to test it deliberately — the cost of being
     * wrong is a reboot to fastboot, which is recoverable — and once a watch
     * survives a read, make it the default for that board. */
    return -9999;
#else
    int best = -9999;
    uint32_t i;

    if (!(mmio_read(TSENS_CTRL) & TSENS_EN)) return -9999;
    if (!(mmio_read(TSENS_TRDY) & TSENS_TRDY_MASK)) return -9999;

    if (!s_inited) { tsens_calibrate(); s_inited = 1; }

    for (i = 0; i < TSENS_NUM_SENSORS; i++) {
        uint32_t st = mmio_read(TSENS_S0_STATUS + i * 4u);
        int code = (int)(st & TSENS_STATUS_TEMP_MASK);
        int num, dc;

        if (code == 0) continue;              /* never converted */
        num = (code * TSENS_FACTOR) - s_offset[i];
        /* Round to nearest, matching tsens_tz_code_to_degc(); then x10 for
         * deci-degC, which is the unit the app expects. */
        dc = (num >= 0) ? ((num * 10 + s_slope[i] / 2) / s_slope[i])
                        : ((num * 10 - s_slope[i] / 2) / s_slope[i]);
        /* Plausibility gate: a die outside -40..150 C means the decode is
         * wrong, not that the watch is on fire. Refuse rather than mislead. */
        if (dc < -400 || dc > 1500) continue;
        if (dc > best) best = dc;
    }
    return best;
#endif /* TSENS_ENABLE */
}

#endif /* PLAT_SOC_MSM8909 */
