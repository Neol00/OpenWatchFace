/* gic.c — minimal GICv2 driver (distributor + CPU interface).
 * Covers what a single-core RTOS needs: global enable, per-IRQ enable with a
 * priority, and routing SPIs to CPU0. The FreeRTOS CA9 port owns PMR/BPR once
 * the scheduler starts; we set permissive defaults for the pre-scheduler window. */
#include "platform.h"

#define GICD(off)  (PLAT_GICD_BASE + (off))
#define GICC(off)  (PLAT_GICC_BASE + (off))

#define GICD_CTLR        GICD(0x000)
#define GICD_ISENABLER   GICD(0x100)   /* +4*n */
#define GICD_ICPENDR     GICD(0x280)
#define GICD_IPRIORITYR  GICD(0x400)   /* byte per IRQ */
#define GICD_ITARGETSR   GICD(0x800)   /* byte per IRQ */
#define GICC_CTLR        GICC(0x000)
#define GICC_PMR         GICC(0x004)
#define GICC_BPR         GICC(0x008)

void gic_init(void)
{
    mmio_write(GICD_CTLR, 1);          /* forward group-0 interrupts */
    mmio_write(GICC_PMR, 0xFF);        /* pre-scheduler: mask nothing */
    mmio_write(GICC_BPR, 0);           /* full preemption granularity */
    mmio_write(GICC_CTLR, 1);          /* CPU interface on */
}

void gic_enable_irq(unsigned id, uint8_t priority)
{
    /* priority byte: 0 = most urgent, 0xF8 = least (32 levels << 3) */
    uintptr_t prio_reg = GICD_IPRIORITYR + (id & ~3u);
    uint32_t shift = (id & 3u) * 8;
    uint32_t v = mmio_read(prio_reg) & ~(0xFFu << shift);
    mmio_write(prio_reg, v | ((uint32_t)priority << shift));

    if (id >= 32) {                    /* SPIs need a target CPU (cpu0) */
        uintptr_t tgt_reg = GICD_ITARGETSR + (id & ~3u);
        v = mmio_read(tgt_reg) | (0x01u << ((id & 3u) * 8));
        mmio_write(tgt_reg, v);
    }
    mmio_write(GICD_ISENABLER + (id / 32u) * 4u, 1u << (id % 32u));
}
