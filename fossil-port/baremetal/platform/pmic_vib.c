/* pmic_vib.c — SPMI PMIC-arbiter master + PM8916 vibration motor.
 *
 * WHY THIS EXISTS (the bring-up problem it solves):
 * The Fossil Gen 4 has no exposed UART, no JTAG, and — until DSI/MDP3 are
 * proven — no display. That means a failed boot and a boot that never started
 * look EXACTLY the same from the outside: a dark, silent watch. Every other
 * debug channel we have is downstream of something unproven:
 *   - the framebuffer needs DSI + MDP3 + panel init (hundreds of lines, untested)
 *   - the ramlog needs a post-mortem RAM dump, which needs fastboot to survive
 *   - the UART needs a pad nobody has confirmed is routed
 *
 * The vibration motor is the ONLY output on this watch that needs none of that.
 * It is one SPMI register write to the PMIC, it needs no clocks we do not
 * already have (SPMI arbiter is left running by aboot), and you feel it with
 * the watch in your hand. It turns "nothing happened" into a real signal:
 *
 *   buzz  -> our code IS executing on the A7. Everything after this point is
 *            a driver bug, and the fault is downstream.
 *   silent-> we never reached main(). The fault is upstream: boot.img packing,
 *            load address, aboot rejecting the image, or startup.S.
 *
 * That single bit of information is what turns blind bring-up into debugging.
 *
 * SOURCES (all verbatim from the stock DTB dumped off the DW6F1, and the
 * vendor 3.18 qpnp-vibrator driver):
 *   vibrator:      qcom,vibrator@c000 on qcom,pm8916@1 (SPMI slave id ONE --
 *                  see the block comment below; this line said 0 for months
 *                  and that is exactly why the motor never moved)
 *                  qcom,vib-vtg-level-mV = 0xc1c = 3100 mV
 *
 * The SPMI arbiter master itself lives in spmi_arb.c (shared with the RTC and
 * other PMIC peripheral drivers). This is a WRITE-ONLY, polled, no-IRQ driver
 * on purpose: the less machinery it depends on, the more trustworthy its
 * signal is.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#if defined(PLAT_SOC_MSM8909)
/* ---- PM8916 vibrator (qpnp-vibrator @ 0xc000, slave id 1) ----------------
 * Shared by every msm8909w watch here: the Wear 2100 is paired with a PM8916
 * on both the Fossil Gen 4 and the TicWatch C2, and the vibrator block is part
 * of that PMIC, so the register map is fixed by the silicon. Only the drive
 * VOLTAGE is a board choice (it depends on the motor fitted), so that is the
 * one value a board header may override.
 * Register map from the vendor qpnp-vibrator driver:
 *   0xc040 VIB_VTG_CTL   [4:0] voltage level, 1.2V + n*100mV
 *   0xc046 VIB_EN_CTL    bit7 enable
 * DTB says qcom,vib-vtg-level-mV = 3100. The driver's conversion is
 *   level = (mV - VTG_MIN 1200) / 100  ->  (3100-1200)/100 = 19 = 0x13
 */
/* SPMI SLAVE ID: 1, NOT 0. This was the bug, and it was in the DT all along.
 * PM8916 presents TWO SPMI slave ids, and the merged device trees of BOTH
 * msm8909w watches put the vibrator on the second one:
 *   firefish-boot.dts:4691  qcom,pm8916@0 { reg = <0x00>; ... }   revid, pon,
 *                           mpp, gpio, rtc, vadc, temp-alarm
 *   firefish-boot.dts:5192  qcom,pm8916@1 { reg = <0x01>; ... }   regulators,
 *   firefish-boot.dts:5421      qcom,vibrator@c000              <-- HERE
 * and skipjack.dts:4608 / 5099 / 5328 is the same tree, node for node.
 *
 * Why sid 0 could not work: arbiter v2 addresses a peripheral by PPID =
 * (sid << 8) | (addr >> 8), so sid 0 asks for PPID 0x0C0 while the motor is
 * PPID 0x1C0. spmi_apid_for() then either finds no channel at all (write
 * returns -1, silently) or -- worse -- finds whatever unrelated peripheral
 * owns 0x0C0 and writes 0x80 into it. Either way the motor never moves.
 *
 * PROBED, NOT ASSUMED. Two device trees agreeing is good evidence but it is
 * still evidence about the kernel's view, so vib_init() asks the arbiter
 * itself which sid actually has a mapped channel for 0xC000 and takes sid 1
 * only if the hardware agrees. */
#define PM8916_VIB_SID_DT   1u      /* device-tree answer; verified at runtime */
#define QPNP_VIB_VTG_CTL    0xC040u
#define QPNP_VIB_EN_CTL     0xC046u
#define QPNP_VIB_EN         (1u << 7)
/* Board-overridable drive level. Both watches' DTBs carry
 * qcom,vib-vtg-level-mV = 0xc1c = 3100 (firefish and skipjack alike), so the
 * shared default is the measured value on both, not an inherited guess. */
#ifndef PLAT_VIB_VTG_MV
#define PLAT_VIB_VTG_MV 3100u
#endif
#define QPNP_VIB_VTG_LEVEL  (((PLAT_VIB_VTG_MV) - 1200u) / 100u)

static uint8_t s_vib_sid;
static int     s_vib_ok;

/* Pick the slave id the ARBITER agrees the vibrator lives on.
 *
 * The ownership check is not optional politeness. pmic_rtc.c already cost a
 * whole flash cycle learning this: writing an SPMI peripheral owned by another
 * execution environment did not fail politely, it tripped an ownership
 * violation and the secure world answered with an instant TZ reset. So this
 * resolves the peripheral READ-ONLY -- channel lookup plus owner EE, both out
 * of arbiter tables -- and only then writes.
 *
 * sid 1 is tried first because that is what both device trees say; sid 0 is
 * kept as a fallback so a board that really did differ degrades to a log line
 * instead of a dead motor. */
static int vib_resolve_sid(void)
{
    static const uint8_t cand[2] = { PM8916_VIB_SID_DT, 0u };

    for (unsigned i = 0; i < 2u; i++) {
        int apid = spmi_apid_of(cand[i], QPNP_VIB_EN_CTL);
        int ee   = spmi_owner_ee(cand[i], QPNP_VIB_EN_CTL);
        bdiag_puts("vib: sid=");   bdiag_putdec(cand[i]);
        bdiag_puts(" apid=");      bdiag_putdec((uint32_t)apid);
        bdiag_puts(" owner_ee=");  bdiag_putdec((uint32_t)ee);
        bdiag_puts("\n");
        if (apid >= 0 && ee == 0) {
            s_vib_sid = cand[i];
            return 0;
        }
    }
    return -1;
}

int vib_init(void)
{
    if (!s_vib_ok) {
        if (vib_resolve_sid() < 0) {
            con_puts("vib: no writable vibrator channel - motor disabled\n");
            return -1;
        }
        s_vib_ok = 1;
    }
    /* Set drive voltage once; enable/disable then only touches EN_CTL. */
    return spmi_write8(s_vib_sid, QPNP_VIB_VTG_CTL, QPNP_VIB_VTG_LEVEL);
}

void vib_set(int on)
{
    if (!s_vib_ok) return;          /* never write an unowned peripheral */
    spmi_write8(s_vib_sid, QPNP_VIB_EN_CTL, on ? QPNP_VIB_EN : 0u);
}

#elif defined(PLAT_BOARD_FOSSIL_GEN6)
/* ---- PM660 haptics (qcom,pm660-haptics @ 0xc000, SPMI slave id 1) --------
 * DIFFERENT BLOCK from the Gen 4's PM8916 vibrator: qpnp-haptics is a waveform
 * player, not a single enable bit, so it needs a mode + drive config before
 * PLAY does anything. DTB (qcom,haptic@c000 on qcom,pm660@1):
 *   qcom,actuator-type = "erm"      -> ERM, not LRA (no resonance tracking)
 *   qcom,vmax-mv       = 0xc80      -> 3200 mV
 *   qcom,play-rate-us  = 0x2710
 *
 * Register map from the vendor qpnp-haptics driver (drivers/.../qpnp-haptics.c):
 *   0xC023 ACT_TYPE   bit0: 0 = LRA, 1 = ERM
 *   0xC043 PLAY       bit7 PLAY_EN
 *   0xC044 EN_CTL     bit7 EN
 *   0xC04A VMAX_CFG   [5:1] vmax, step 116 mV
 *   0xC04C WAVE_SHAPE bit0: 0 = square, 1 = sine
 *   0xC04D PLAY_MODE  [1:0] 0 = direct play (what we want: no waveform table)
 *
 * Direct-play mode is deliberate — it is the fewest moving parts that produces
 * a buzz, which is the whole point of this driver during bring-up. */
#define QPNP_HAP_ACT_TYPE   0xC023u
#define QPNP_HAP_PLAY       0xC043u
#define QPNP_HAP_EN_CTL     0xC044u
#define QPNP_HAP_VMAX_CFG   0xC04Au
#define QPNP_HAP_PLAY_MODE  0xC04Du

#define QPNP_HAP_ACT_ERM    0x01u
#define QPNP_HAP_PLAY_EN    (1u << 7)
#define QPNP_HAP_EN         (1u << 7)
#define QPNP_HAP_MODE_DIRECT 0x00u

/* VMAX_CFG holds the level in bits [5:1], one step per 116 mV. */
#define QPNP_HAP_VMAX_STEP_MV 116u
#define QPNP_HAP_VMAX_REG(mv) (uint8_t)((((mv) / QPNP_HAP_VMAX_STEP_MV) & 0x1Fu) << 1)

int vib_init(void)
{
    int rc = 0;
    rc |= spmi_write8(PLAT_PMIC_SID, QPNP_HAP_ACT_TYPE,  QPNP_HAP_ACT_ERM);
    rc |= spmi_write8(PLAT_PMIC_SID, QPNP_HAP_PLAY_MODE, QPNP_HAP_MODE_DIRECT);
    rc |= spmi_write8(PLAT_PMIC_SID, QPNP_HAP_VMAX_CFG,
                      QPNP_HAP_VMAX_REG(PLAT_HAP_VMAX_MV));
    /* Module enable stays on; PLAY is what gates the motor. */
    rc |= spmi_write8(PLAT_PMIC_SID, QPNP_HAP_EN_CTL,    QPNP_HAP_EN);
    return rc ? -1 : 0;
}

void vib_set(int on)
{
    spmi_write8(PLAT_PMIC_SID, QPNP_HAP_PLAY, on ? QPNP_HAP_PLAY_EN : 0u);
}
#endif

/* Buzz `n` times, `ms` on / `ms` off. Uses the polled arch timer, so it works
 * before the scheduler exists — which is the whole point: this must be usable
 * at the very first instruction of main(). */
void vib_buzz(unsigned n, uint32_t ms)
{
    for (unsigned i = 0; i < n; i++) {
        vib_set(1);
        timer_delay_ms(ms);
        vib_set(0);
        if (i + 1 < n) timer_delay_ms(ms);
    }
}

#endif /* PLAT_SOC_MSM */
