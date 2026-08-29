/* recovery_gate.c — the way back to fastboot on a watch with no working buttons.
 *
 * WHY THIS IS NOT OPTIONAL.
 * The Fossil Gen 4 has no button combination that reaches fastboot (the
 * documented crown+button gesture does not work on this unit), and no button
 * force-reset. As long as the firmware is only RAM-booted that does not
 * matter: a power cycle returns the watch to stock. The moment the `boot`
 * partition is flashed it matters enormously — a build with no route back to
 * the bootloader is a build that can never be replaced, and the watch becomes
 * a brick running whatever was flashed last.
 *
 * There are two independent routes, and they cover different failures:
 *
 *   1. The DEAD-MAN timer (reboot_msm.c). Covers a firmware that HANGS: if
 *      nothing kicks it, the watch reboots itself to fastboot. It is useless
 *      for a firmware that runs perfectly, because a healthy main loop kicks
 *      it forever — which is exactly the situation this file addresses.
 *
 *   2. The Fastboot BUTTON in the power app (OpenWatchFace/app_power.h).
 *      Covers a firmware that WORKS, which the dead-man cannot: a healthy
 *      main loop kicks the dead-man forever. Compiled in unconditionally on
 *      both Fossil watches — an escape hatch you can forget to build in is
 *      not an escape hatch.
 *
 *   3. This gate (-DRECOVERY_GATE, off by default). A window early in every
 *      boot in which touching the screen reboots to the bootloader. It adds
 *      one thing the button cannot: it runs BEFORE the UI, and paints with
 *      the direct framebuffer text path rather than LVGL, so it still works
 *      when the app layer is the broken part.
 *
 * PROVEN ON HARDWARE 2026-08-28: a touch in this window rebooted a Gen 4
 * straight into fastboot. That also validated the PMIC warm-reset fix in
 * reboot_msm.c — every earlier attempt on both watches had landed in the stock
 * OS instead. `-DRECOVERY_GATE_SELFTEST` still builds an image that takes the
 * gate with no touch at all, which is the cheapest way to re-verify the reboot
 * path on a new board.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

/* How long the window stays open. Long enough to react to, short enough not
 * to be an annoyance on every boot; override with -DRECOVERY_GATE_MS=<ms>. */
#if !defined(RECOVERY_GATE_MS)
#define RECOVERY_GATE_MS 5000u
#endif

int touch_read(uint16_t *x, uint16_t *y);

/* Returns 0 if the window closed untouched (the caller carries on booting).
 * Does not return if the screen was touched — the watch reboots to fastboot.
 *
 * ANY touch triggers it, rather than a held press: touch on this watch can lag
 * by seconds until the controller is in full-rate scan, so requiring a
 * sustained hold would make the escape hatch unreliable in exactly the
 * degraded conditions it exists for. A stray trigger is cheap — you land in
 * fastboot and boot again. */
int recovery_gate(void)
{
#if !defined(RECOVERY_GATE) && !defined(RECOVERY_GATE_SELFTEST)
    /* OPT-IN (-DRECOVERY_GATE). Off by default because the firmware now
     * carries a PERMANENT route into the bootloader that costs nothing at
     * boot: the Fastboot button in the power app (OpenWatchFace/app_power.h),
     * which is compiled in unconditionally on these watches. This gate is the
     * bring-up convenience — useful when the UI itself is the thing under
     * suspicion, since it runs before the app takes over and paints with the
     * direct framebuffer text path rather than LVGL. */
    return 0;
#else
    const uint32_t window = RECOVERY_GATE_MS;
    uint32_t t0 = timer_ms();
    uint32_t last_paint = 0xFFFFFFFFu;

#if defined(RECOVERY_GATE_SELFTEST)
    /* Proof-of-recovery build: take the gate with no touch at all, so the
     * ONLY thing under test is whether reboot_to_bootloader() actually lands
     * in fastboot on this watch. Run it from `fastboot boot`. */
    fb_text_dump("SELFTEST\n\nREBOOT TO\nFASTBOOT\nNOW");
    timer_delay_ms(1500);
    con_puts("recovery-gate: SELFTEST, rebooting to bootloader\n");
    reboot_to_bootloader();
    return 0;                      /* unreachable on real hardware */
#endif

    con_puts("recovery-gate: open\n");
    for (;;) {
        uint32_t elapsed = timer_ms() - t0;
        if (elapsed >= window) break;

        /* Repaint once a second with the countdown. fb_text_dump clears and
         * flushes the whole frame, so it is far too heavy to run per poll. */
        uint32_t left = (window - elapsed + 999u) / 1000u;
        if (left != last_paint) {
            last_paint = left;
            char msg[96];
            /* Short lines on purpose: the panel is round, so the usable
             * width at the top and bottom rows is much less than at the
             * middle (gfx_text.c clips each line to the circle's chord). */
            const char *head = "TOUCH SCREEN\nFOR FASTBOOT\n\nBOOTING IN ";
            unsigned i = 0;
            for (const char *p = head; *p; p++) msg[i++] = *p;
            msg[i++] = (char)('0' + (left % 10u));
            msg[i++] = '\n';
            msg[i]   = '\0';
            fb_text_dump(msg);
        }

        uint16_t x = 0, y = 0;
        if (touch_read(&x, &y) > 0) {
            con_puts("recovery-gate: touched, rebooting to bootloader\n");
            fb_text_dump("ENTERING\nFASTBOOT");
            timer_delay_ms(400);
            reboot_to_bootloader();
        }
        timer_delay_ms(20);
    }
    con_puts("recovery-gate: closed, continuing boot\n");
    return 0;
#endif
}

#endif /* PLAT_SOC_MSM */
