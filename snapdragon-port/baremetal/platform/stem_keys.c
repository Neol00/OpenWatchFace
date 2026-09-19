/* stem_keys.c — the gpio_keys pushers on TLMM (Wear 2100 watches, 2026-09-15).
 *
 * Stock device trees (gpio_keys, "gpio-keys", active low, bias-pull-up,
 * 2 mA, gpio-key,wakeup, 15 ms debounce):
 *   Fossil Gen 4 (firefish)   STEM_1 gpio91 (KEY 0x109), STEM_2 gpio90 (0x10a)
 *   TicWatch C2/C2+ (skipjack) STEM_1 gpio91
 *   TicWatch S2 (tunny)       gpio_keys status "disabled" -> no pusher
 * MPM wake pins from the same trees' qcom,gpio-map: gpio90 = MPM 30,
 * gpio91 = MPM 32 (mpm.c arms them next to the PMIC pin for a collapse).
 *
 * The level is one MMIO read (no SPMI), so the app polls it every loop pass.
 * The edge interrupt only exists to wake a suspended core: the handler counts
 * edges and suspend_msm.c compares the count against its sleep-entry snapshot,
 * the same pattern as pmic_irq.c's kpdpwr/resin counters. */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909) && defined(PLAT_BTN_STEM1_GPIO)

extern int tlmm_irq_enable(uint32_t pin, uint32_t detect, void (*fn)(void *), void *arg);

#if defined(PLAT_BTN_STEM2_GPIO)
static const uint32_t s_pin[] = { PLAT_BTN_STEM1_GPIO, PLAT_BTN_STEM2_GPIO };
#else
static const uint32_t s_pin[] = { PLAT_BTN_STEM1_GPIO };
#endif
#define N_STEMS (sizeof s_pin / sizeof s_pin[0])

volatile uint32_t g_stem_irq_n[2];
static int s_cfg, s_armed;

/* Pull-up input, as the stock gpio_key_active/suspend states. Lazy, because the
 * app polls from its first loop pass and the IRQ is only armed at +20 s: an
 * unconfigured pin can float low and read as a held button. */
static void stem_keys_setup(void)
{
    if (s_cfg) return;
    s_cfg = 1;
    for (unsigned i = 0; i < N_STEMS; i++) tlmm_cfg(s_pin[i], 0u, 3u /* pull-up */, 2u, 0);
}

static void stem_irq(void *arg)
{
    g_stem_irq_n[(unsigned)(uintptr_t)arg]++;
}

/* 1 = held, 0 = released (or no such pusher on this board). */
int stem_key_pressed(unsigned idx)
{
    if (idx >= N_STEMS) return 0;
    stem_keys_setup();
    return tlmm_in(s_pin[idx]) == 0;
}

void stem_keys_arm_irq(void)
{
    if (s_armed) return;
    s_armed = 1;
    stem_keys_setup();
    for (unsigned i = 0; i < N_STEMS; i++) {
        int rc = tlmm_irq_enable(s_pin[i], 2u /* falling = press */, stem_irq, (void *)(uintptr_t)i);
        if (rc == 0) tlmm_irq_set_wake(s_pin[i]);
        con_puts("stem-keys: STEM_"); con_putdec(i + 1u); con_puts(" gpio"); con_putdec(s_pin[i]);
        con_puts(rc == 0 ? " edge irq armed (wake source)" : " edge irq NOT armed (table full)");
        con_puts(" level="); con_putdec((uint32_t)tlmm_in(s_pin[i])); con_puts("\n");
    }
}

uint32_t stem_keys_irq_total(void)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < N_STEMS; i++) n += g_stem_irq_n[i];
    return n;
}

/* After a system collapse the MPM, not the TLMM latch, is the record of what
 * woke us (the vendor kernel re-injects from MPM STATUS for the same reason). */
int stem_keys_mpm_fired(void)
{
    int f = 0;
#if defined(PLAT_BTN_STEM1_MPM)
    f |= (mpm_status_word(PLAT_BTN_STEM1_MPM / 32u) >> (PLAT_BTN_STEM1_MPM % 32u)) & 1u;
#endif
#if defined(PLAT_BTN_STEM2_MPM)
    f |= (mpm_status_word(PLAT_BTN_STEM2_MPM / 32u) >> (PLAT_BTN_STEM2_MPM % 32u)) & 1u;
#endif
    return f;
}

#else
int      stem_key_pressed(unsigned idx) { (void)idx; return 0; }
void     stem_keys_arm_irq(void) { }
uint32_t stem_keys_irq_total(void) { return 0; }
int      stem_keys_mpm_fired(void) { return 0; }
#endif
