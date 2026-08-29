/* pmic_pon.c — qpnp-power-on button state over SPMI (both watches).
 *
 * The watch's physical pushers that route through the PMIC are readable with a
 * single SPMI register read, no GPIO/TLMM driver needed:
 *   DTB (both watches): qcom,qpnp-power-on @ 0x800 on PMIC sid 0, with
 *   "kpdpwr" (the power/crown button) and "resin" (second pusher) sub-nodes.
 *   Kernel qpnp-power-on.c: PON_RT_STS = base + 0x10,
 *     bit 0 = KPDPWR_ON (set while physically held down)
 *     bit 1 = RESIN_ON
 *
 * This is real-time level state — exactly what the firmware's polled BOOT
 * button path wants (it debounces in software already). IRQ wiring (PMIC intr
 * block -> arbiter -> GIC) is a later, optional refinement.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#define PON_SID       0u
#define PON_RT_STS    0x0810u
#define PON_KPDPWR_ON (1u << 0)
#define PON_RESIN_ON  (1u << 1)

/* 1 = held down, 0 = released, -1 = SPMI error (treat as released). */
int pon_kpdpwr_pressed(void)
{
    uint8_t v;
    if (spmi_read8(PON_SID, PON_RT_STS, &v) < 0) return -1;
    return (v & PON_KPDPWR_ON) ? 1 : 0;
}

int pon_resin_pressed(void)
{
    uint8_t v;
    if (spmi_read8(PON_SID, PON_RT_STS, &v) < 0) return -1;
    return (v & PON_RESIN_ON) ? 1 : 0;
}

#endif /* PLAT_SOC_MSM */
