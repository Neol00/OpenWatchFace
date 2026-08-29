/* pmic_irq.c — PMIC interrupts (buttons, RTC alarm) as a real wake source.
 *
 * WHY THIS IS A PREREQUISITE FOR DEEP SLEEP, not a nicety. Today suspend wakes
 * on the CPU's own architected timer every 250 ms and polls the buttons. That
 * works only while the core is powered. The moment we use a power-down state,
 * CNTV dies with the core — which is exactly what the DTB says about cpu-level
 * "pc" (sda429-hoki-decompiled.dts:1356):
 *     qcom,use-broadcast-timer;   <- the per-CPU timer cannot wake us here
 *     qcom,is-reset;
 * So a powered-down core needs a wake source OUTSIDE the CPU power domain.
 * Every wake source this firmware actually uses — the physical buttons and the
 * RTC alarm — is on the PMIC, in the always-on domain, and they all arrive on
 * ONE line: qcom,spmi@200f000 "periph_irq" = GIC SPI 190 -> INTID 222.
 * Routing that one interrupt is the whole job.
 *
 * SAFETY SHAPE: this is added ALONGSIDE the existing 250 ms poll, not instead
 * of it. If any register here is wrong, the poll still wakes the watch exactly
 * as it does today and g_pmic_irq_n simply stays 0 — a legible failure instead
 * of a watch that will not wake. Chunks go long only once the counter proves
 * the chain works.
 *
 * REGISTER MAP — from the hoki kernel's drivers/spmi/spmi-pmic-arb.c, NOT from
 * memory. This SoC is arbiter v2 (the same version whose APID table spmi_arb.c
 * already walks; pmic_arb_apid_map_offset_v2() = 0x800 + 4n confirms it).
 * All offsets are relative to the "intr" reg region, 0x3800000 (len 0x200000):
 *     pmic_arb_acc_enable_v2(n)        = intr + 0x1000 * n
 *     pmic_arb_irq_status_v2(n)        = intr + 0x4 + 0x1000 * n
 *     pmic_arb_irq_clear_v2(n)         = intr + 0x8 + 0x1000 * n
 *     pmic_arb_owner_acc_status_v2(m,n)= intr + 0x100000 + 0x1000*m + 0x4*n
 *       (m = EE = 0, n = apid/32 word index)
 *     SPMI_PIC_ACC_ENABLE_BIT          = BIT(0)
 * Per-peripheral interrupt registers (QPNPINT_REG_*, offsets from the
 * peripheral's own base, reached over ordinary SPMI):
 *     +0x10 RT_STS  +0x11 SET_TYPE  +0x12 POLARITY_HIGH  +0x13 POLARITY_LOW
 *     +0x14 LATCHED_CLR  +0x15 EN_SET  +0x16 EN_CLR  +0x18 LATCHED_STS
 * qpnp-pon lives at 0x800, so EN_SET = 0x815; kpdpwr is irq bit 0, resin bit 1
 * (the DTB's interrupt-names order, and the same bit order as RT_STS).
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#define SPMI_INTR_BASE      0x03800000u
#define PIC_ACC_ENABLE(n)   (SPMI_INTR_BASE + 0x1000u * (n))
#define PIC_IRQ_STATUS(n)   (SPMI_INTR_BASE + 0x4u + 0x1000u * (n))
#define PIC_IRQ_CLEAR(n)    (SPMI_INTR_BASE + 0x8u + 0x1000u * (n))
#define PIC_OWNER_ACC(w)    (SPMI_INTR_BASE + 0x100000u + 0x4u * (w))
#define PIC_ACC_ENABLE_BIT  (1u << 0)

#define PIC_APID_MAX        128u
#define PIC_ACC_WORDS       (PIC_APID_MAX / 32u)     /* 4 */

/* GIC SPI 190 (DTB: interrupts = <0 0xbe 0>) -> INTID 32 + 190. */
#define PMIC_ARB_INTID      222u

/* qpnp-pon peripheral (sid 0, base 0x800) */
#define PON_SID             0u
#define PON_BASE            0x0800u
#define PON_INT_SET_TYPE    (PON_BASE + 0x11u)   /* +0x11..0x13 contiguous */
#define PON_INT_LATCHED_CLR (PON_BASE + 0x14u)
#define PON_INT_EN_SET      (PON_BASE + 0x15u)
#define PON_INT_LATCHED_STS (PON_BASE + 0x18u)
#define PON_IRQ_KPDPWR      (1u << 0)
#define PON_IRQ_RESIN       (1u << 1)

/* Census — suspend_msm.c prints these so a dead chain is visible. */
volatile uint32_t g_pmic_irq_n;        /* GIC 222 dispatches            */
volatile uint32_t g_pmic_irq_kpdpwr;   /* kpdpwr edges seen             */
volatile uint32_t g_pmic_irq_resin;    /* resin edges seen              */
volatile uint32_t g_pmic_irq_wake;     /* set by the handler; suspend clears */
volatile uint32_t g_pmic_irq_spurious; /* fired with nothing pending    */
volatile uint32_t g_pmic_irq_stuck;    /* APIDs masked by the storm guard */

static int s_pon_apid = -1;

/* Chained dispatch, mirroring pmic_arb_chained_irq()/periph_interrupt(): the
 * GIC only tells us "some PMIC peripheral fired", so walk the arbiter's
 * owner-accumulator to find which APIDs are pending, then each APID's status
 * to find which interrupt bits.
 *
 * EVERY pending APID must be cleared, not just the ones we care about — an
 * uncleared level-triggered source re-asserts the instant we EOI and becomes
 * an interrupt storm. Unknown APIDs are cleared and counted, never ignored. */
static void pmic_irq_handler(void *arg)
{
    (void)arg;
    uint32_t any = 0;

    g_pmic_irq_n++;

    for (unsigned w = 0; w < PIC_ACC_WORDS; w++) {
        uint32_t acc = mmio_read(PIC_OWNER_ACC(w));
        while (acc) {
            unsigned b    = (unsigned)__builtin_ctz(acc);
            unsigned apid = w * 32u + b;
            acc &= ~(1u << b);

            uint32_t st = mmio_read(PIC_IRQ_STATUS(apid));
            if (!st) continue;
            any = 1;

            if ((int)apid == s_pon_apid) {
                if (st & PON_IRQ_KPDPWR) g_pmic_irq_kpdpwr++;
                if (st & PON_IRQ_RESIN)  g_pmic_irq_resin++;
                g_pmic_irq_wake = 1u;
                /* Clear the peripheral's latch first, then the arbiter — the
                 * other order lets a latch that is still set immediately
                 * re-raise the APID we just cleared. */
                spmi_write8(PON_SID, PON_INT_LATCHED_CLR, (uint8_t)st);
            }
            mmio_write(PIC_IRQ_CLEAR(apid), st);

            /* STORM GUARD. A source we cannot clear re-asserts the instant we
             * EOI, and the core never leaves the handler — the watch goes
             * silent and only a reboot recovers it. Rather than risk that,
             * verify the clear took; if it did not, mask this APID at the
             * arbiter so it can cost us one event instead of the device.
             * Same philosophy as irq.c's "unowned source" disable. */
            if (mmio_read(PIC_IRQ_STATUS(apid))) {
                mmio_write(PIC_ACC_ENABLE(apid), 0u);
                g_pmic_irq_stuck++;
            }
        }
    }

    if (!any) g_pmic_irq_spurious++;
}

/* Returns 0 if the chain was set up, -1 if the PON peripheral is not mapped.
 * Never fatal: a failure here just leaves suspend on its existing poll. */
int pmic_irq_init(void)
{
    s_pon_apid = spmi_apid_of(PON_SID, PON_BASE);
    diag_puts("pmic-irq: pon apid=");
    diag_putdec((uint32_t)s_pon_apid);
    if (s_pon_apid < 0) {
        diag_puts("  NOT MAPPED -> no interrupt wake, poll only\n");
        return -1;
    }

    /* CONFIGURE THE TRIGGER — the step whose absence made a real button press
     * produce no interrupt at all (measured: pmic_irq=0 across an 11 s sleep
     * that was ended by the POLL seeing kpdpwr, while the cumulative counters
     * sat at exactly kpd=4 resin=4 from init and never moved).
     *
     * Enabling an interrupt does not say WHEN it should fire. We inherited
     * whatever the bootloader left in the type/polarity registers, which is
     * evidently not "on a keypress". qpnp-power-on.c requests kpdpwr and resin
     * with IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING (both edges, so press
     * AND release are seen), and qpnpint_irq_set_type() maps that to three
     * contiguous registers:
     *     +0x11 SET_TYPE       bit set = edge-triggered  (clear = level)
     *     +0x12 POLARITY_HIGH  bit set = rising edge
     *     +0x13 POLARITY_LOW   bit set = falling edge
     * Read-modify-write: this peripheral has other interrupts (resin-bark,
     * kpdpwr-resin-bark) whose configuration is not ours to clobber. */
    {
        uint8_t t[3] = { 0, 0, 0 };
        const uint8_t mask = PON_IRQ_KPDPWR | PON_IRQ_RESIN;

        if (spmi_read(PON_SID, PON_INT_SET_TYPE, t, 3) == 0) {
            diag_puts("  type-was type=");   diag_puthex(t[0]);
            diag_puts(" polhi=");            diag_puthex(t[1]);
            diag_puts(" pollo=");            diag_puthex(t[2]);
        }
        /* RISING EDGE ONLY — press, not release.
         *
         * The kernel asks for BOTH edges because a full input driver needs to
         * report key-up as well as key-down. We only need "wake me when a
         * button goes down", and enabling the release edge actively broke
         * sleep: the watch entered suspend, the user let go of the button, the
         * falling edge fired, and it woke 85 ms later. That is the same
         * already-held-at-entry problem button_wake() solves in software with
         * its arming flag — the interrupt path had no such guard, so any edge
         * woke us. Taking only the rising edge gives the identical semantics
         * for free, and halves the interrupt count. */
        t[0] |=  mask;   /* edge-triggered           */
        t[1] |=  mask;   /* on the rising edge (press) */
        t[2] &= ~mask;   /* NOT on release             */
        spmi_write(PON_SID, PON_INT_SET_TYPE, t, 3);
    }

    /* ORDER MATTERS, and it is the kernel's: qpnpint_irq_unmask() enables the
     * APID at the arbiter FIRST, then writes LATCHED_CLR and EN_SET together —
     * clearing the latch in the same breath as enabling so a stale latch
     * cannot fire a spurious interrupt the instant the enable lands. My first
     * cut had the arbiter enable last, leaving a window between EN_SET and
     * acc_enable in which an event could latch unattributed. */
    mmio_write(PIC_ACC_ENABLE(s_pon_apid), PIC_ACC_ENABLE_BIT);
    spmi_write8(PON_SID, PON_INT_LATCHED_CLR, PON_IRQ_KPDPWR | PON_IRQ_RESIN);
    spmi_write8(PON_SID, PON_INT_EN_SET,      PON_IRQ_KPDPWR | PON_IRQ_RESIN);

    /* Priority 0xC0, NOT 0xF8 — see the long note in irq.c: aboot leaves the
     * GICC with a stuck running priority that gates anything numerically >=
     * it, and 0xC0 is the measured needle-thread that still gets presented
     * while remaining maskable by FreeRTOS critical sections. */
    irq_register(PMIC_ARB_INTID, pmic_irq_handler, 0);
    gic_enable_irq(PMIC_ARB_INTID, 0xC0);

    uint8_t en = 0, latched = 0, t2[3] = { 0, 0, 0 };
    spmi_read8(PON_SID, PON_INT_EN_SET, &en);
    spmi_read8(PON_SID, PON_INT_LATCHED_STS, &latched);
    spmi_read(PON_SID, PON_INT_SET_TYPE, t2, 3);
    diag_puts("  type=");      diag_puthex(t2[0]);
    diag_puts("/");            diag_puthex(t2[1]);
    diag_puts("/");            diag_puthex(t2[2]);
    diag_puts(" en=");         diag_puthex(en);
    diag_puts(" latched=");    diag_puthex(latched);
    diag_puts(" acc_en=");     diag_puthex(mmio_read(PIC_ACC_ENABLE(s_pon_apid)));
    diag_puts(" gic intid=");  diag_putdec(PMIC_ARB_INTID);
    diag_puts(" -> armed (poll stays as the safety net)\n");
    return 0;
}

#endif /* PLAT_SOC_MSM */
