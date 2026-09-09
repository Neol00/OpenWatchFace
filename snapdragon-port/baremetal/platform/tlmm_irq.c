/* tlmm_irq.c — TLMM GPIO interrupts on the msm8909w (summary IRQ SPI 208).
 *
 * pinctrl-msm8916.c register layout (same TLMM block on the 8909):
 *   per pin +0x8  INTR_CFG:  bit0 enable, bit1 polarity, bits[3:2] detect
 *                 (0 level, 1 rising, 2 falling, 3 both), bit4 raw-status
 *                 enable, bits[7:5] target proc (4 = APPS/KPSS)
 *   per pin +0xC  INTR_STATUS: bit0, write 0 to clear
 * One summary line for all pins: interrupts = <0 208 0> -> GIC 240. */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

#define TLMM_BASE        0x01000000u
#define INTR_CFG(n)      (TLMM_BASE + 0x1000u * (n) + 0x8u)
#define INTR_STATUS(n)   (TLMM_BASE + 0x1000u * (n) + 0xCu)
#define TLMM_SUMMARY_IRQ (32u + 208u)

#define MAX_PINS 4
static struct { uint32_t pin; void (*fn)(void *); void *arg; } s_pins[MAX_PINS];
static unsigned s_n;
volatile uint32_t g_tlmm_irq_n, g_tlmm_irq_spurious;

static void summary_handler(void *arg)
{
    (void)arg;
    uint32_t any = 0;
    g_tlmm_irq_n++;
    for (unsigned i = 0; i < s_n; i++) {
        if (mmio_read(INTR_STATUS(s_pins[i].pin)) & 1u) {
            mmio_write(INTR_STATUS(s_pins[i].pin), 0u);
            __asm__ volatile("dsb sy" ::: "memory");
            any = 1;
            if (s_pins[i].fn) s_pins[i].fn(s_pins[i].arg);
        }
    }
    if (!any) g_tlmm_irq_spurious++;
}

/* detect: 1 rising, 2 falling, 3 both edges. The pin must already be an input. */
/* Mask/unmask the whole TLMM summary line at the GIC (suspend: the touch
 * controller's INT was waking a collapsed core ~every 0.7 s idle and 10x/s
 * when the glass was brushed, C2 2026-09-06). */
void tlmm_irq_mask(int mask)
{
    if (mask) gic_disable_irq(TLMM_SUMMARY_IRQ);
    else      gic_enable_irq(TLMM_SUMMARY_IRQ, 0xC0);
}

int tlmm_irq_enable(uint32_t pin, uint32_t detect, void (*fn)(void *), void *arg)
{
    if (s_n >= MAX_PINS || pin >= 113u) return -1;
    if (s_n == 0) {
        irq_register(TLMM_SUMMARY_IRQ, summary_handler, 0);
        gic_enable_irq(TLMM_SUMMARY_IRQ, 0xC0);     /* 0xC0: see irq.c on the stuck GICC priority */
    }
    s_pins[s_n].pin = pin; s_pins[s_n].fn = fn; s_pins[s_n].arg = arg; s_n++;
    mmio_write(INTR_STATUS(pin), 0u);
    mmio_write(INTR_CFG(pin), 1u | (1u << 1) | ((detect & 3u) << 2) | (1u << 4) | (4u << 5));
    __asm__ volatile("dsb sy" ::: "memory");
    con_dbg("tlmm-irq: gpio"); con_dbg_dec(pin); con_dbg(" cfg="); con_dbg_hex(mmio_read(INTR_CFG(pin)));
    con_dbg(" (summary irq 240)\n");
    return 0;
}

#endif
