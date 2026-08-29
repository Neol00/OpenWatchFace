/* reboot_msm.c — SoC reset + reboot-to-bootloader for the Fossil Gen 4, and a
 * button-less "dead-man" watchdog that is the ONLY guaranteed recovery path on
 * this watch (it has no button force-reset and no button route to fastboot).
 *
 * Mechanism (all three addresses CONFIRMED from the stock DTB, 2026-07-28):
 *   PS_HOLD           restart@4ab000, reg[0] = 0x004ab000  (write 0 -> reset)
 *   TCSR boot-misc    restart@4ab000, reg[1] = 0x0193d100  (boot-mode detect)
 *   imem restart_reason  qcom,msm-imem@8600000 + 0x65c = 0x0860065c
 *
 * reboot-to-bootloader is the Android convention: stash a cookie in imem
 * restart_reason AND in the PMIC's SOFT_RB_SPARE, drop PS_HOLD; aboot reads
 * them after the reset and enters fastboot instead of booting.
 *
 * CORRECTED 2026-08-28: the cookie only survives if the PMIC has been
 * CONFIGURED for a warm reset first. That is a register field, not a property
 * of PS_HOLD, and its default is a hard reset — which power-cycles the PMIC and
 * loses IMEM. See the PON block below.
 *
 * SAFETY MODEL: the watchdog rides the ARM arch-timer + GIC — the SAME path the
 * FreeRTOS tick uses and which is already proven on this runtime — so the timer
 * WILL fire regardless of what the main loop is doing. The only unverified bits
 * are the two register pokes themselves; if the imem cookie is wrong the watch
 * still reboots, just to stock Wear OS (from which adb -> fastboot recovers).
 * The reset itself (PS_HOLD) is the standard msm8909 restart, very likely OK.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#include "FreeRTOS.h"
#include "timers.h"

/* --- confirmed register addresses (see file header / DTB) -----------------
 * VERIFIED IDENTICAL ON BOTH WATCHES (2026-08-01): the Gen 6 (hoki/SDA429W)
 * DTB has the same restart@4ab000 with reg = <0x4ab000 4  0x193d100 4> and the
 * same qcom,msm-imem@8600000 + restart_reason@65c as the Gen 4. So this driver
 * — and the dead-man recovery path built on it — applies unchanged to both. */
#define MSM_PSHOLD_BASE        0x004AB000u
#define MSM_TCSR_BOOT_MISC     0x0193D100u
#define MSM_IMEM_RESTART_REASON 0x0860065Cu

/* Android restart-reason cookies (bootable_bootloader/lk: msm_shared/reboot.h).
 * FIXED 2026-08-03: LK's values are FASTBOOT_MODE = 0x77665500 (low byte
 * 0x00!), RECOVERY_MODE = 0x77665502; "normal" is 0x77665501. The old table
 * here used 0x77 for bootloader -> wrote 0x77665577, which no LK recognizes,
 * so every intentional reboot fell through to the default boot (Wear OS). */
#define RESTART_REASON_BASE    0x77665500u
#define RESTART_MODE_BOOTLOADER 0x00u   /* 0x77665500 = LK FASTBOOT_MODE  */
#define RESTART_MODE_NORMAL    0x01u    /* 0x77665501 = normal boot       */
#define RESTART_MODE_RECOVERY  0x02u    /* 0x77665502 = LK RECOVERY_MODE  */

/* Some msm8909 aboots ALSO consult the TCSR boot-misc-detect register for the
 * download/boot hint. Writing the same low nibble there is belt-and-braces and
 * harmless if aboot ignores it. */
#define TCSR_BOOT_MISC_BOOTLOADER 0x20u

static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* --- the PMIC PON block: what `adb reboot bootloader` ACTUALLY does --------
 *
 * Every reboot_to_bootloader() this port has ever issued landed in the stock
 * OS instead of fastboot, on both watches. The reason, found 2026-08-28 by
 * reading firefish's own kernel rather than guessing again: we were doing
 * three of the stock sequence's five steps, and the missing one was decisive.
 *
 * drivers/power/reset/msm-poweroff.c, msm_restart_prepare():
 *
 *     qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);   <-- WE SKIPPED THIS
 *     qpnp_pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
 *     __raw_writel(0x77665500, restart_reason);
 *     ... scm_disable_sdi(); halt_spmi_pmic_arbiter(); deassert_ps_hold();
 *
 * `need_warm_reset` is true for ANY non-empty reboot command, so a stock
 * `reboot bootloader` always configures a WARM reset first. This file's header
 * used to assert "the cookie survives because PS_HOLD is a WARM reset" — but
 * the reset TYPE is not a property of PS_HOLD, it is a field in the PMIC's
 * PS_HOLD_RST_CTL register, and its default is a HARD reset. A hard reset
 * power-cycles the PMIC and the SoC, so IMEM does not survive and the cookie
 * we carefully wrote is gone before aboot ever looks for it.
 *
 * Registers verbatim from drivers/platform/msm/qpnp-power-on.c, with the PON
 * base 0x800 from this watch's own DTB (qcom,power-on@800 — which also carries
 * qcom,store-hard-reset-reason, so the SOFT_RB_SPARE path is live here, not
 * dead code):
 *
 *   base + 0x01  REVISION2
 *   base + 0x05  PERPH_SUBTYPE
 *   base + 0x5A  PS_HOLD_RST_CTL    [3:0] power-off type, bit 7 RESET_EN
 *   base + 0x5B  PS_HOLD_RST_CTL2   bit 7 RESET_EN on GEN1_V2 / GEN2 parts
 *   base + 0x8F  SOFT_RB_SPARE      restart reason
 */
#define PON_BASE               0x800u
#define PON_REVISION2          (PON_BASE + 0x01u)
#define PON_PERPH_SUBTYPE      (PON_BASE + 0x05u)
#define PON_PS_HOLD_RST_CTL    (PON_BASE + 0x5Au)
#define PON_PS_HOLD_RST_CTL2   (PON_BASE + 0x5Bu)
#define PON_SOFT_RB_SPARE      (PON_BASE + 0x8Fu)

#define PON_RESET_EN             (1u << 7)
#define PON_POWER_OFF_MASK       0x0Fu
#define PON_POWER_OFF_WARM_RESET 0x01u
#define PON_POWER_OFF_HARD_RESET 0x07u

/* PERPH_SUBTYPE values that select the register generation. */
#define PON_SUBTYPE_PRIMARY        0x01u
#define PON_SUBTYPE_SECONDARY      0x02u
#define PON_SUBTYPE_1REG           0x03u
#define PON_SUBTYPE_GEN2_PRIMARY   0x04u
#define PON_SUBTYPE_GEN2_SECONDARY 0x05u

/* enum pon_restart_reason (include/linux/qpnp/power-on.h) */
#define PON_REASON_RECOVERY    0x01u
#define PON_REASON_BOOTLOADER  0x02u

/* HOW FAR THE REASON IS SHIFTED is a RUNTIME decision, not a per-board
 * constant — a distinction that has already produced one wrong value here.
 * The Fossil hoki kernel (fossil-engineering/kernel-msm-fossil-cw, branch
 * fossil-android-msm-hoki-lw1.2-4.14, drivers/input/misc/qpnp-power-on.c):
 *
 *     if (is_pon_gen2(pon) && !pon->legacy_hard_reset_offset)
 *             ... GENMASK(7, 1), (reason << 1);
 *     else    ... GENMASK(7, 2), (reason << 2);
 *
 * and firefish's older msm-3.18 driver has only the second form.
 * `legacy_hard_reset_offset` is the DT property
 * qcom,use-legacy-hard-reset-offset.
 *
 * Applied to the two watches, from their own dumped device trees:
 *   Gen 4 (PM8916): qcom,power-on@800 has no legacy property, but the part is
 *                   a GEN1 subtype, so the gen2 test fails -> shift 2.
 *   Gen 6 (PM660):  qcom,power-on@800 DOES carry
 *                   qcom,use-legacy-hard-reset-offset -> shift 2.
 * So both watches want 2 — and the value this file used to hardcode for the
 * Gen 6, 1, was simply wrong. Rather than swap one constant for another, the
 * rule itself is ported: the subtype is already read for pon_reset_config, so
 * deciding correctly costs nothing and stays right on the next board. */
#if defined(PLAT_BOARD_FOSSIL_GEN6)
#define PLAT_PON_LEGACY_HARD_RESET_OFFSET 1   /* hoki DT sets the property */
#else
#define PLAT_PON_LEGACY_HARD_RESET_OFFSET 0   /* firefish DT does not */
#endif

static int pon_is_gen2(uint8_t subtype)
{
    return subtype == PON_SUBTYPE_GEN2_PRIMARY ||
           subtype == PON_SUBTYPE_GEN2_SECONDARY;
}

static unsigned pon_reason_shift(uint8_t subtype)
{
    return (pon_is_gen2(subtype) && !PLAT_PON_LEGACY_HARD_RESET_OFFSET) ? 1u : 2u;
}

static int pon_masked_write(uint32_t addr, uint8_t mask, uint8_t val)
{
    uint8_t v = 0;
    if (spmi_read8(0, addr, &v) < 0) return -1;
    v = (uint8_t)((v & ~mask) | (val & mask));
    return spmi_write8(0, addr, v);
}

/* Port of qpnp_pon_reset_config(). Reads the part's own subtype/revision to
 * pick the register carrying the live RESET_EN bit, exactly as the driver
 * does, rather than assuming a generation. */
static uint8_t pon_reset_config(uint8_t type)
{
    uint8_t subtype = 0, rev2 = 0;
    uint32_t rst_en_reg = PON_PS_HOLD_RST_CTL2;

    if (spmi_read8(0, PON_PERPH_SUBTYPE, &subtype) < 0) {
        con_puts("pon: subtype read FAILED; assuming GEN1_V1\n");
        rst_en_reg = PON_PS_HOLD_RST_CTL;
    } else {
        spmi_read8(0, PON_REVISION2, &rev2);
        if (subtype == PON_SUBTYPE_PRIMARY || subtype == PON_SUBTYPE_SECONDARY)
            rst_en_reg = (rev2 == 0) ? PON_PS_HOLD_RST_CTL    /* GEN1_V1 */
                                     : PON_PS_HOLD_RST_CTL2;  /* GEN1_V2 */
        else if (pon_is_gen2(subtype) || subtype == PON_SUBTYPE_1REG)
            rst_en_reg = PON_PS_HOLD_RST_CTL2;
    }
    bdiag_puts("pon: subtype="); bdiag_puthex(subtype);
    bdiag_puts(" rev2=");        bdiag_puthex(rev2);
    bdiag_puts(" rst_en=");      bdiag_puthex(rst_en_reg);
    bdiag_puts(" type=");        bdiag_puthex(type);
    bdiag_puts("\n");

    /* Disable, wait, set the type, re-enable — the driver's exact order. The
     * delay is ten sleep-clock cycles plus 50% tolerance: the register sits
     * behind the PMIC's slow clock domain and a back-to-back rewrite does not
     * latch. (The same slow-domain trap msm_wdog.c documents.) */
    pon_masked_write(rst_en_reg, PON_RESET_EN, 0);
    timer_delay_ms(1);
    pon_masked_write(PON_PS_HOLD_RST_CTL, PON_POWER_OFF_MASK, type);
    pon_masked_write(rst_en_reg, PON_RESET_EN, PON_RESET_EN);
    return subtype;
}

static void reboot_commit(uint32_t reason_cookie, uint32_t tcsr_hint,
                          uint8_t pon_reason)
{
    /* 1. Configure the PMIC for a WARM reset. Without this the default hard
     *    reset power-cycles the PMIC and wipes the IMEM cookie written below.
     *    A plain reboot (no cookie) keeps the hard reset, matching
     *    msm_restart_prepare's need_warm_reset logic. */
    uint8_t subtype = pon_reset_config(pon_reason ? PON_POWER_OFF_WARM_RESET
                                                  : PON_POWER_OFF_HARD_RESET);

    /* 2. The PMIC restart reason, shifted the way THIS part wants it. */
    unsigned sh = pon_reason_shift(subtype);
    pon_masked_write(PON_SOFT_RB_SPARE,
                     (uint8_t)(0xFFu << sh),
                     (uint8_t)(pon_reason << sh));
    bdiag_puts("pon: reason="); bdiag_puthex(pon_reason);
    bdiag_puts(" shift=");      bdiag_putdec(sh);
    bdiag_puts("\n");

    /* 3. The IMEM cookie, which aboot also consults. */
    mmio_write(MSM_IMEM_RESTART_REASON, reason_cookie);
    if (tcsr_hint) mmio_write(MSM_TCSR_BOOT_MISC, tcsr_hint);
    dsb();

    /* 4. Drop PS_HOLD; the PMIC executes the type configured in step 1.
     *    Control never returns from here on real hardware.
     *
     *    NOT PORTED, deliberately: scm_disable_sdi() and
     *    halt_spmi_pmic_arbiter(). Both exist to make a CRASHING kernel reset
     *    cleanly; neither is needed for a deliberate reboot from healthy code,
     *    and the SCM call is the kind of secure-world write that has hard-reset
     *    this watch before. Add them only with evidence they are needed. */
    mmio_write(MSM_PSHOLD_BASE, 0u);
    dsb();

    for (;;) { __asm__ volatile("wfi"); }
}

void reboot_now(void)
{
    con_puts("reboot: normal reset\n");
    reboot_commit(RESTART_REASON_BASE | RESTART_MODE_NORMAL, 0, 0);
}

void reboot_to_bootloader(void)
{
    con_puts("reboot: -> fastboot\n");
    reboot_commit(RESTART_REASON_BASE | RESTART_MODE_BOOTLOADER,
                  TCSR_BOOT_MISC_BOOTLOADER, PON_REASON_BOOTLOADER);
}

void reboot_to_recovery(void)
{
    con_puts("reboot: -> recovery\n");
    reboot_commit(RESTART_REASON_BASE | RESTART_MODE_RECOVERY, 0,
                  PON_REASON_RECOVERY);
}

/* --- dead-man watchdog ---------------------------------------------------- */
/* A FreeRTOS software timer that reboots to the bootloader unless it is kicked
 * before it expires. Armed in main() BEFORE the UI starts, so a hung or blank
 * boot still recovers on its own. During bring-up, DO NOT kick it: every boot
 * then returns to fastboot after the timeout, working or not. Later, kick it
 * from a known-good point (or a touch gesture) to keep a good boot alive. */
static TimerHandle_t s_deadman;

static void deadman_expired(TimerHandle_t t)
{
    (void)t;
    con_puts("deadman: timeout, rebooting to bootloader\n");
    reboot_to_bootloader();
}

/* timeout_ms: how long an un-kicked boot runs before auto-recovery. */
void deadman_arm(uint32_t timeout_ms)
{
    s_deadman = xTimerCreate("deadman", pdMS_TO_TICKS(timeout_ms),
                             pdFALSE /* one-shot */, NULL, deadman_expired);
    if (s_deadman) xTimerStart(s_deadman, 0);
    bdiag_puts("deadman: armed "); bdiag_putdec(timeout_ms);
    bdiag_puts(" ms (un-kicked boots auto-reboot to fastboot)\n");
}

/* Call to prove the boot is alive; restarts the countdown. Leave UNCALLED during
 * bring-up so recovery is unconditional. */
void deadman_kick(void)
{
    if (s_deadman) xTimerReset(s_deadman, 0);
}

/* Disarm entirely once you trust a build (e.g. from a confirmed-good UI). */
void deadman_disarm(void)
{
    if (s_deadman) xTimerStop(s_deadman, 0);
}

#endif /* PLAT_SOC_MSM */
