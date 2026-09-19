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

/* WHICH VIBRATOR BLOCK — asked as a capability, not as "which watch".
 * This used to select PM8916 on PLAT_SOC_MSM8909 and PM660 on
 * PLAT_BOARD_FOSSIL_GEN6, which silently assumed 8909 implies PM8916. The
 * Fossil Gen 5 (triggerfish, Wear 3100) breaks that: it is an msm8909 AP with
 * a PM660, so the old guards would have handed it the PM8916 single-enable-bit
 * driver and written 0x80 into a register that block does not have. The Gen 6
 * keeps working through the compatibility shim below. */
#if defined(PLAT_BOARD_FOSSIL_GEN6) && !defined(PLAT_VIB_PM660_HAPTICS)
#define PLAT_VIB_PM660_HAPTICS 1
#endif

#if defined(PLAT_VIB_PM660_HAPTICS)
#define PLAT_VIB_BLOCK_PM660 1
#elif defined(PLAT_SOC_MSM8909)
#define PLAT_VIB_BLOCK_PM8916 1
#endif

#if defined(PLAT_VIB_BLOCK_PM8916)
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

#elif defined(PLAT_VIB_BLOCK_PM660)
/* ---- PM660 haptics (qcom,pm660-haptics @ 0xc000, SPMI slave id 1) --------
 * DIFFERENT BLOCK from the Gen 4's PM8916 vibrator: qpnp-haptics is a waveform
 * player, not a single enable bit, so it needs a mode + drive config before
 * PLAY does anything. DTB (qcom,haptics@c000 on qcom,pm660@1):
 *   qcom,actuator-type = "erm"      -> ERM, not LRA (no resonance tracking)
 *   qcom,vmax-mv       = 0xc80      -> 3200 mV
 *   qcom,ilim-ma       = 0x190      -> 400 mA
 *   qcom,play-rate-us  = 0x2710     -> 10000 us
 *
 * REGISTER MAP — THIS IS WHAT WAS WRONG (fixed 2026-09-12, Gen 5 motor dead).
 * The compatible string is "qcom,pm660-haptics", and that binding is served by
 * drivers/leds/leds-qpnp-haptics.c, NOT by the older qpnp-haptic.c. The map
 * this file used before (ACT_TYPE 0x23, PLAY 0x43, EN 0x44, VMAX 0x4A,
 * PLAY_MODE 0x4D) matches NEITHER driver — every write landed on an unrelated
 * offset inside the peripheral, so the block was never configured and PLAY was
 * never asserted. SPMI reported success for all of it, which is why this looked
 * like working code. The real offsets from the base, per leds-qpnp-haptics.c:
 *   0x0A STATUS_1     bit1 BUSY, bit3 SC_FLAG (short-circuit latched)
 *   0x46 EN           bit7 module enable
 *   0x4B AUTO_RES_CTRL bit7 auto-resonance (LRA only -> off for an ERM)
 *   0x4C ACT_TYPE     bit0: 0 = LRA, 1 = ERM
 *   0x4D WAV_SHAPE    bit0: 0 = square, 1 = sine
 *   0x4E PLAY_MODE    [5:4] WF_SOURCE: 0 = direct play (no waveform table)
 *   0x51 VMAX_CFG     [5:1] vmax, one step per 116 mV
 *   0x52 ILIM_CFG     bit0: 0 = 400 mA, 1 = 800 mA
 *   0x54 RATE_CFG1    play rate low  8 bits, one step per 5 us
 *   0x55 RATE_CFG2    play rate high 4 bits
 *   0x5C BRAKE        brake pattern (0 = no braking)
 *   0x70 PLAY         bit7 PLAY, bit0 PAUSE
 *
 * Direct-play mode is deliberate — it is the fewest moving parts that produces
 * a buzz, which is the whole point of this driver during bring-up. */
#define QPNP_HAP_STATUS_1   (PLAT_HAP_BASE + 0x0Au)
#define QPNP_HAP_EN_CTL     (PLAT_HAP_BASE + 0x46u)
#define QPNP_HAP_AUTO_RES   (PLAT_HAP_BASE + 0x4Bu)
#define QPNP_HAP_ACT_TYPE   (PLAT_HAP_BASE + 0x4Cu)
#define QPNP_HAP_WAV_SHAPE  (PLAT_HAP_BASE + 0x4Du)
#define QPNP_HAP_PLAY_MODE  (PLAT_HAP_BASE + 0x4Eu)
#define QPNP_HAP_VMAX_CFG   (PLAT_HAP_BASE + 0x51u)
#define QPNP_HAP_ILIM_CFG   (PLAT_HAP_BASE + 0x52u)
#define QPNP_HAP_RATE_CFG1  (PLAT_HAP_BASE + 0x54u)
#define QPNP_HAP_RATE_CFG2  (PLAT_HAP_BASE + 0x55u)
#define QPNP_HAP_EN_CTL2    (PLAT_HAP_BASE + 0x48u)
#define QPNP_HAP_BRAKE      (PLAT_HAP_BASE + 0x5Cu)
#define QPNP_HAP_PLAY       (PLAT_HAP_BASE + 0x70u)

#define QPNP_HAP_BUSY_BIT    (1u << 1)
#define QPNP_HAP_SC_FLAG_BIT (1u << 3)
#define QPNP_HAP_ACT_ERM     0x01u
#define QPNP_HAP_WAVE_SQUARE 0x00u
#define QPNP_HAP_PLAY_BIT    (1u << 7)
#define QPNP_HAP_EN          (1u << 7)
#define QPNP_HAP_MODE_DIRECT 0x00u   /* WF_SOURCE [5:4] = 0 */
#define QPNP_HAP_BRAKE_EN    (1u << 0)   /* EN_CTL2 bit0 */

/* ---- BRAKING: what makes a buzz feel like a TICK instead of a "wrrrr" ------
 * An ERM does not stop when the drive stops. The rotor has momentum and coasts
 * for tens of milliseconds, and that tail is what a wearer perceives as the
 * motor being slow, sluggish or delayed -- the harder it was driven, the longer
 * and more noticeable the tail. Raising the drive level without braking makes
 * this WORSE, not better, which is exactly what happened on the Gen 5 in v295.
 *
 * Braking drives the motor briefly in REVERSE to kill that momentum, so the
 * buzz ends when the pulse ends. The block does it in hardware from a 4-entry
 * pattern in HAP_BRAKE, two bits per entry, oldest in [1:0]. This watch's own
 * DTB says what the vendor uses for it (qcom,haptics@c000, wf_0):
 *
 *     qcom,wf-brake-pattern = <0x1000000>   ->  bytes 01 00 00 00
 *
 * i.e. one reverse step then nothing: brake hard, immediately, once. That is
 * the value below. Braking is gated by BRAKE_EN in EN_CTL2 (0x48) -- the
 * pattern alone does nothing without it, which is why v295 had a brake pattern
 * register write of 0 AND no enable, and coasted freely. */
#ifndef PLAT_HAP_BRAKE_EN
#define PLAT_HAP_BRAKE_EN 1
#endif
#ifndef PLAT_HAP_BRAKE_PATTERN
#define PLAT_HAP_BRAKE_PATTERN 0x01u   /* from this watch's DTB: 01 00 00 00 */
#endif

/* VMAX_CFG holds the drive level in bits [5:1], one step per 116 mV, so the
 * block's range is 116 mV (1) to 3596 mV (31) and 3596 is FULL SCALE.
 *
 * ROUNDING, not truncation. qpnp_haptics_vmax_config() uses DIV_ROUND_CLOSEST,
 * and the difference is not cosmetic: 3200 mV truncated is 27 steps = 3132 mV,
 * rounded it is 28 = 3248 mV. Truncating threw away 116 mV of drive on every
 * board using this file. Clamped to the block's own range first, as the vendor
 * does, so an out-of-range board value can never wrap into a tiny level. */
#define QPNP_HAP_VMAX_STEP_MV 116u
#define QPNP_HAP_VMAX_MIN_MV  116u
#define QPNP_HAP_VMAX_MAX_MV  3596u
#define QPNP_HAP_VMAX_CLAMP(mv) \
    ((mv) < QPNP_HAP_VMAX_MIN_MV ? QPNP_HAP_VMAX_MIN_MV : \
     (mv) > QPNP_HAP_VMAX_MAX_MV ? QPNP_HAP_VMAX_MAX_MV : (mv))
#define QPNP_HAP_VMAX_STEPS(mv) \
    (((QPNP_HAP_VMAX_CLAMP(mv) + (QPNP_HAP_VMAX_STEP_MV / 2u)) / QPNP_HAP_VMAX_STEP_MV))
#define QPNP_HAP_VMAX_REG(mv) (uint8_t)((QPNP_HAP_VMAX_STEPS(mv) & 0x1Fu) << 1)

/* Play rate is programmed in 5 us steps across two registers (12 bits). */
#ifndef PLAT_HAP_PLAY_RATE_US
#define PLAT_HAP_PLAY_RATE_US 10000u   /* DTB qcom,play-rate-us = 0x2710 */
#endif
#define QPNP_HAP_RATE_STEP_US 5u
#define QPNP_HAP_RATE_VAL     ((PLAT_HAP_PLAY_RATE_US) / QPNP_HAP_RATE_STEP_US)

/* ILIM: bit0 selects the current limit. DTB says 400 mA. */
#ifndef PLAT_HAP_ILIM_MA
#define PLAT_HAP_ILIM_MA 400u
#endif
#define QPNP_HAP_ILIM_VAL ((PLAT_HAP_ILIM_MA) > 400u ? 1u : 0u)

static uint8_t s_vib_sid;
static int     s_vib_ok;

/* Resolve the slave id the ARBITER agrees the haptics block lives on, exactly
 * as the PM8916 branch does. This is not politeness: writing an SPMI peripheral
 * owned by another execution environment trips an ownership violation and the
 * secure world answers with an instant TZ reset (pmic_rtc.c cost a flash cycle
 * learning that). Resolve read-only first, write only if we own it.
 *
 * PLAT_PMIC_SID (1) is the device tree's answer and is tried first; sid 0 is a
 * fallback so a board that really differs degrades to a log line, not a reset. */
static int vib_resolve_sid(void)
{
    static const uint8_t cand[2] = { (uint8_t)PLAT_PMIC_SID, 0u };

    for (unsigned i = 0; i < 2u; i++) {
        int apid = spmi_apid_of(cand[i], QPNP_HAP_EN_CTL);
        int ee   = spmi_owner_ee(cand[i], QPNP_HAP_EN_CTL);
        bdiag_puts("hap: sid=");   bdiag_putdec(cand[i]);
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
    int rc = 0;

    if (!s_vib_ok) {
        if (vib_resolve_sid() < 0) {
            con_puts("hap: no writable haptics channel - motor disabled\n");
            return -1;
        }
        s_vib_ok = 1;
    }

    /* Module OFF while reconfiguring: the vendor driver never changes the
     * actuator type or the play mode with EN asserted. */
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_EN_CTL,    0u);

    rc |= spmi_write8(s_vib_sid, QPNP_HAP_ACT_TYPE,  QPNP_HAP_ACT_ERM);
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_AUTO_RES,  0u);   /* LRA-only feature */
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_WAV_SHAPE, QPNP_HAP_WAVE_SQUARE);
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_PLAY_MODE, QPNP_HAP_MODE_DIRECT);
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_VMAX_CFG,
                      QPNP_HAP_VMAX_REG(PLAT_HAP_VMAX_MV));
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_ILIM_CFG,  (uint8_t)QPNP_HAP_ILIM_VAL);
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_RATE_CFG1,
                      (uint8_t)(QPNP_HAP_RATE_VAL & 0xFFu));
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_RATE_CFG2,
                      (uint8_t)((QPNP_HAP_RATE_VAL >> 8) & 0x0Fu));
    /* Brake pattern first, then the enable that arms it. */
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_BRAKE,
                      (uint8_t)(PLAT_HAP_BRAKE_EN ? PLAT_HAP_BRAKE_PATTERN : 0u));
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_EN_CTL2,
                      (uint8_t)(PLAT_HAP_BRAKE_EN ? QPNP_HAP_BRAKE_EN : 0u));
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_PLAY,      0u);   /* known-idle */

    /* Module enable stays on from here; PLAY is what gates the motor. */
    rc |= spmi_write8(s_vib_sid, QPNP_HAP_EN_CTL,    QPNP_HAP_EN);

    return rc ? -1 : 0;
}

void vib_set(int on)
{
    if (!s_vib_ok) return;          /* never write an unowned peripheral */
    spmi_write8(s_vib_sid, QPNP_HAP_PLAY, on ? QPNP_HAP_PLAY_BIT : 0u);
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
