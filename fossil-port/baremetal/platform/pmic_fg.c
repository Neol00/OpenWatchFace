/* pmic_fg.c — battery gauge + charger status over SPMI.
 * Gen 6: PM660 FG-GEN3 + SMB2.  Gen 4: PM8916 VM-BMS + linear charger.
 * Gen 6 only: the Gen 4's PM8916 uses the older VM-BMS gauge (different block);
 * it gets -1 stubs so shared firmware code links on both boards.
 *
 * SOURCES (all verified against the published hoki 4.14 kernel,
 * github.com/fossil-engineering/kernel-msm-fossil-cw, drivers/power/supply/qcom/):
 *   DTB: qpnp,fg compatible "qcom,fg-gen3", children fg-batt-soc@4000 and
 *        fg-batt-info@4100 on pm660@0 (SPMI sid 0); charger qcom,qpnp-smb2
 *        with qcom,chgr@1000, usb@1300.
 *   fg-reg.h:  BATT_SOC_FG_MONOTONIC_SOC     0x4009 (shadow copy 0x400A)
 *              BATT_INFO_BATT_TEMP_LSB/MSB   0x4150/0x4151 (MSB mask [2:0])
 *              BATT_INFO_VBATT_LSB/MSB       0x41A0/0x41A1 (copy 0x41A6/7)
 *              BATT_INFO_IBATT_LSB/MSB       0x41A2/0x41A3 (copy 0x41A8/9)
 *   fg-util.c: msoc% = round(raw * 100 / 255)             (FULL_SOC_RAW 255)
 *              vbatt_uV = raw16 * 122070 / 1000           (LE byte order)
 *              ibatt_uA = sext16(raw) * 488281 / 1000     (+ = discharging)
 *              temp_K*4 = ((msb & 7) << 8) | lsb          (0.25 K per LSB)
 *   smb-reg.h: BATTERY_CHARGER_STATUS_1 = 0x1006, status = bits [2:0]
 *              (0 trickle, 1 pre, 2 fast, 3 fullon, 4 taper, 5 terminate,
 *               6 inhibit, 7 disable — "charging" = 0..4)
 *              USBIN INT_RT_STS = 0x1310, bit 4 = USBIN_PLUGIN_RT_STS
 *
 * The gauge runs autonomously (aboot + the stock OS have long since programmed
 * its battery profile into SRAM); we only READ live output registers, so there
 * is nothing to initialize and nothing we can corrupt. Reads use the same
 * shadow-copy double-read the kernel does to dodge mid-update tearing.
 */
#include "platform.h"
#include <stdio.h>
#if defined(PLAT_SOC_MSM)

#if defined(PLAT_BOARD_FOSSIL_GEN6)

#define FG_SID              0u
#define FG_MSOC             0x4009u
#define FG_MSOC_CP          0x400Au
#define FG_BATT_TEMP_LSB    0x4150u   /* 2 bytes, MSB mask [2:0] */
#define FG_VBATT_LSB        0x41A0u   /* 2 bytes LE */
#define FG_VBATT_LSB_CP     0x41A6u
#define FG_IBATT_LSB        0x41A2u   /* 2 bytes LE, signed */
#define FG_IBATT_LSB_CP     0x41A8u

#define CHG_STATUS_1        0x1006u   /* BATTERY_CHARGER_STATUS_1 */
#define CHG_STATUS_MASK     0x07u
#define USB_INT_RT_STS      0x1310u
#define USBIN_PLUGIN_BIT    (1u << 4)

#define FG_READ_TRIES       3

/* Read a 2-byte live register with its shadow copy, kernel-style: the value is
 * trustworthy only when both copies agree (the FG updates them non-atomically). */
static int fg_read16_cp(uint16_t addr, uint16_t addr_cp, uint16_t *out)
{
    uint8_t a[2], b[2];
    for (unsigned t = 0; t < FG_READ_TRIES; t++) {
        if (spmi_read(FG_SID, addr, a, 2) < 0) return -1;
        if (spmi_read(FG_SID, addr_cp, b, 2) < 0) return -1;
        if (a[0] == b[0] && a[1] == b[1]) {
            *out = (uint16_t)a[0] | ((uint16_t)a[1] << 8);
            return 0;
        }
    }
    return -1;
}

/* Monotonic state-of-charge, 0-100. -1 on error. */
int fg_batt_percent(void)
{
    uint8_t cap[2];
    for (unsigned t = 0; t < FG_READ_TRIES; t++) {
        if (spmi_read(FG_SID, FG_MSOC, cap, 2) < 0) return -1;   /* 0x4009+0x400A */
        if (cap[0] == cap[1])
            return (int)(((unsigned)cap[0] * 100u + 127u) / 255u);
    }
    return -1;
}

/* Battery voltage in mV. -1 on error. */
int fg_batt_mv(void)
{
    uint16_t raw;
    if (fg_read16_cp(FG_VBATT_LSB, FG_VBATT_LSB_CP, &raw) < 0) return -1;
    /* uV = raw * 122070 / 1000  ->  mV = raw * 122070 / 1000000 */
    return (int)(((uint64_t)raw * 122070u) / 1000000u);
}

/* Battery current in mA, positive = DISCHARGING (kernel convention).
 * Returns -32768 on error (a real current never hits that exactly). */
int fg_batt_ma(void)
{
    uint16_t raw;
    if (fg_read16_cp(FG_IBATT_LSB, FG_IBATT_LSB_CP, &raw) < 0) return -32768;
    int32_t s = (int16_t)raw;                       /* sign bit is bit 15 */
    return (int)(((int64_t)s * 488281) / 1000000);  /* uA -> mA */
}

/* Battery temperature in deci-degC (e.g. 251 = 25.1 C). -9999 on error. */
int fg_batt_temp_dc(void)
{
    uint8_t buf[2];
    if (spmi_read(FG_SID, FG_BATT_TEMP_LSB, buf, 2) < 0) return -9999;
    unsigned k4 = ((unsigned)(buf[1] & 0x07u) << 8) | buf[0];   /* kelvin * 4 */
    return (int)((k4 * 10u + 2u) / 4u) - 2730;
}

/* USB power present? 1/0, -1 on error. */
int chg_usb_present(void)
{
    uint8_t v;
    if (spmi_read8(FG_SID, USB_INT_RT_STS, &v) < 0) return -1;
    return (v & USBIN_PLUGIN_BIT) ? 1 : 0;
}

/* Actively charging? 1/0, -1 on error. Status 0..4 = trickle/pre/fast/fullon/
 * taper are all "charging"; 5..7 = terminate/inhibit/disable are not. */
int chg_charging(void)
{
    uint8_t v;
    if (spmi_read8(FG_SID, CHG_STATUS_1, &v) < 0) return -1;
    return ((v & CHG_STATUS_MASK) <= 4u) ? 1 : 0;
}

#else /* Gen 4: PM8916 VM-BMS + linear charger */
/* THE GEN 4 IS A COMPLETELY DIFFERENT POWER STACK, which is why the Gen 6's
 * code above reported 0 V here rather than a wrong number: none of it runs.
 * From this watch's own DTB:
 *   qcom,vmbms          qpnp-vm-bms, child qcom,vm-bms@4000  (SID 0)
 *   qcom,linear-charger qpnp-linear-charger (LBC), not SMB2
 *   vadc@3100           qpnp-vadc
 * versus the Gen 6's FG-GEN3 fuel gauge and SMB2 charger.
 *
 * "VM" is voltage-mode: this gauge has NO current sense at all. It samples
 * battery VOLTAGE into a hardware FIFO and the kernel does the rest in
 * software — voltage to open-circuit voltage to state-of-charge, through the
 * battery profile in the device tree. So fg_batt_ma() cannot be implemented
 * on this watch at any effort; it reports "unknown" and means it.
 *
 * Registers verbatim from drivers/power/qpnp-vm-bms.c, offsets from the
 * vm-bms base 0x4000:
 *   FIFO_0_LSB_REG  0xC0  latest raw VBAT sample, 2 bytes LE
 *   OCV_DATA0_REG   0x6A  hardware open-circuit voltage, 2 bytes LE
 *   STATUS1_REG     0x08  FSM state
 * and the charger-present bit from the sibling node qcom,qpnp-chg-pres@1008,
 * QPNP_CHARGER_PRESENT = BIT(7).
 *
 * Raw-to-microvolt conversion, verbatim from vadc_reading_to_uv() with
 * vadc_bms = true (no intrinsic-offset subtraction — BMS readings are
 * pre-compensated) followed by adjust_vbatt_reading():
 *
 *     uv = raw * V_PER_BIT_MUL_FACTOR / V_PER_BIT_DIV_FACTOR
 *        = raw * 97656 / 1000                     (97.656 uV per bit)
 *     vbatt_uv = VBATT_MUL_FACTOR * uv            (x3)
 *
 * THE x3 IS NOT OPTIONAL and leaving it out is what made this read 0 V on the
 * first attempt: the cell goes through a DIVIDE-BY-THREE network before the
 * ADC, so the converted sample is a third of the battery voltage. On hardware
 * the raw FIFO read 0x39E9 = 14825, i.e. 1447 mV — which the plausibility
 * window below correctly rejected as impossible for a li-ion cell, rather
 * than reporting it. x3 gives 4343 mV.
 *
 * STILL UNCOMPENSATED: the kernel additionally trims this against the VADC's
 * 0.625 V and 1.25 V references (calib_vadc), and applies a temperature
 * compensation. Both need the VADC block, which is not ported — so the kernel
 * takes the branch quoted above, "don't adjust if not calibrated", and returns
 * the bare x3, which is exactly what this does. Expect a few percent of ADC
 * gain error; porting vadc@3100 is what fixes it, and would also give battery
 * temperature.
 */
#define BMS_BASE            0x4000u
#define BMS_STATUS1         (BMS_BASE + 0x08u)
#define BMS_OCV_DATA0       (BMS_BASE + 0x6Au)
#define BMS_FIFO_0_LSB      (BMS_BASE + 0xC0u)
#define CHG_PRES_REG        0x1008u
#define CHG_PRESENT_BIT     (1u << 7)

#define BMS_UV_MUL          97656u
#define BMS_UV_DIV          1000u
#define BMS_VBATT_MUL       3u      /* VBATT_MUL_FACTOR: /3 divider ahead of the ADC */

/* Plausibility window for a single-cell li-ion. A reading outside it means we
 * read the wrong register or the FIFO has not been populated yet — better to
 * fall back than to report a confident lie. */
#define VBAT_MIN_MV  2500
#define VBAT_MAX_MV  4600

static int bms_read16(uint32_t addr, uint16_t *out)
{
    uint8_t lo = 0, hi = 0;
    if (spmi_read8(0, addr, &lo) < 0)     return -1;
    if (spmi_read8(0, addr + 1u, &hi) < 0) return -1;
    *out = (uint16_t)(lo | ((uint16_t)hi << 8));
    return 0;
}

static int bms_raw_to_mv(uint16_t raw)
{
    /* 97656/1000 uV per bit, then the x3 divider network. Done in 32-bit:
     * raw <= 0xFFFF so raw * 97656 stays below 2^32, and the x3 is applied
     * after the divide by 1000 to keep it there. */
    uint32_t uv = ((uint32_t)raw * BMS_UV_MUL) / BMS_UV_DIV;
    return (int)((uv * BMS_VBATT_MUL) / 1000u);
}

int fg_batt_mv(void)
{
    uint16_t raw = 0;
    int mv;

    /* Live sample first. */
    if (bms_read16(BMS_FIFO_0_LSB, &raw) == 0) {
        mv = bms_raw_to_mv(raw);
        if (mv >= VBAT_MIN_MV && mv <= VBAT_MAX_MV) return mv;
    }
    /* The FIFO can be empty right after a mode change; the hardware OCV is
     * always populated and is close to VBAT on a watch that is mostly idle. */
    if (bms_read16(BMS_OCV_DATA0, &raw) == 0) {
        mv = bms_raw_to_mv(raw);
        if (mv >= VBAT_MIN_MV && mv <= VBAT_MAX_MV) return mv;
    }
    return -1;
}

/* Battery profile: qcom,pc-temp-ocv-lut from THIS watch's DTB
 * (qcom,battery-data -> qcom,palladium-batterydata, "palladium_1500mah"),
 * 25 C column, extracted straight from the tree rather than retyped. Pairs are
 * (state-of-charge %, open-circuit voltage mV), monotonically decreasing.
 *
 * The kernel picks a column by battery temperature and interpolates between
 * columns; without the VADC we have no thermistor reading, so this uses the
 * 25 C curve alone. At watch temperatures the columns differ by a few tens of
 * mV, i.e. a percent or two — acceptable, and far better than nothing. */
static const struct { uint8_t soc; uint16_t ocv_mv; } k_ocv_lut[] = {
    { 100, 4167 }, {  95, 4112 }, {  90, 4072 }, {  85, 4025 }, {  80, 3984 },
    {  75, 3957 }, {  70, 3929 }, {  65, 3900 }, {  60, 3858 }, {  55, 3827 },
    {  50, 3807 }, {  45, 3792 }, {  40, 3780 }, {  35, 3772 }, {  30, 3765 },
    {  25, 3753 }, {  20, 3731 }, {  16, 3704 }, {  13, 3687 }, {  11, 3683 },
    {  10, 3682 }, {   9, 3681 }, {   8, 3680 }, {   7, 3676 }, {   6, 3667 },
    {   5, 3638 }, {   4, 3591 }, {   3, 3528 }, {   2, 3445 }, {   1, 3308 },
    {   0, 3000 },
};

int fg_batt_percent(void)
{
    int mv = fg_batt_mv();
    if (mv < 0) return -1;

    const unsigned n = sizeof k_ocv_lut / sizeof k_ocv_lut[0];
    if (mv >= k_ocv_lut[0].ocv_mv)      return 100;
    if (mv <= k_ocv_lut[n - 1].ocv_mv)  return 0;

    for (unsigned i = 1; i < n; i++) {
        if (mv >= k_ocv_lut[i].ocv_mv) {
            /* Linear interpolation between the bracketing rows. The curve is
             * very flat from 45% down to 6%, so a nearest-row lookup would
             * quantise the middle of the range into big jumps. */
            int v_hi = k_ocv_lut[i - 1].ocv_mv, s_hi = k_ocv_lut[i - 1].soc;
            int v_lo = k_ocv_lut[i].ocv_mv,     s_lo = k_ocv_lut[i].soc;
            int span = v_hi - v_lo;
            if (span <= 0) return s_lo;
            return s_lo + ((mv - v_lo) * (s_hi - s_lo) + span / 2) / span;
        }
    }
    return 0;
}

/* Voltage-mode gauge: there is no current sense on this watch. Not "not
 * ported" — not present. */
int fg_batt_ma(void) { return -32768; }

/* Battery thermistor hangs off the VADC (LR_MUX1_BATT_THERM), which is not
 * ported yet. */
int fg_batt_temp_dc(void) { return -9999; }

/* --- on-glass gauge diagnostic (-DFG_DIAG) -------------------------------
 * fg_batt_mv() returning -1 has two completely different causes and they need
 * opposite fixes:
 *   (a) the SPMI reads are not landing on this peripheral at all, or
 *   (b) they land fine but the VM-BMS is DISABLED, so its FIFO and OCV
 *       registers are genuinely zero. Nothing has ever enabled it here — the
 *       stock kernel's probe is what sets BMS_EN_BIT, and we do not run it.
 * A raw register dump separates them in one boot. rc is the SPMI return code,
 * so "rc=-1" is case (a) and "rc=0 with zero data" is case (b).
 *
 * This watch has no log path yet, so it goes on the glass. */
void fg_diag_text(char *buf, unsigned cap)
{
    uint8_t st1 = 0xEE, en = 0xEE, pres = 0xEE;
    uint16_t fifo = 0, ocv = 0;
    int rc_st1, rc_en, rc_fifo, rc_ocv, rc_pres;

    rc_st1  = spmi_read8(0, BMS_STATUS1, &st1);
    rc_en   = spmi_read8(0, BMS_BASE + 0x46u, &en);      /* EN_CTL, bit7 = on */
    rc_fifo = bms_read16(BMS_FIFO_0_LSB, &fifo);
    rc_ocv  = bms_read16(BMS_OCV_DATA0, &ocv);
    rc_pres = spmi_read8(0, CHG_PRES_REG, &pres);

    /* snprintf is available (newlib); keep it to short lines — the panel is
     * round and gfx_text.c clips each line to the circle's chord. */
    snprintf(buf, cap,
             "BMS DIAG\n"
             "ST1 %02X R%d\n"
             "EN  %02X R%d\n"
             "FIFO %04X R%d\n"
             "OCV  %04X R%d\n"
             "MV %d\n"
             "CHG %02X R%d",
             st1, rc_st1, en, rc_en, fifo, rc_fifo, ocv, rc_ocv,
             fg_batt_mv(), pres, rc_pres);
}

int chg_usb_present(void)
{
    uint8_t v = 0;
    if (spmi_read8(0, CHG_PRES_REG, &v) < 0) return -1;
    return (v & CHG_PRESENT_BIT) ? 1 : 0;
}

/* The linear charger reports "charging" as: a charger is attached and the
 * battery has not reached end-of-charge. Without the LBC status register
 * decoded, presence is the honest half of the answer; a rising voltage is
 * what the UI actually shows anyway. */
int chg_charging(void)
{
    int pres = chg_usb_present();
    if (pres < 0) return -1;
    if (!pres) return 0;
    int pct = fg_batt_percent();
    return (pct >= 0 && pct < 100) ? 1 : 0;
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
#endif /* PLAT_SOC_MSM */
