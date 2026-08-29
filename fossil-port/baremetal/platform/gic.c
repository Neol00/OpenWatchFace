/* gic.c — minimal GICv2 driver (distributor + CPU interface).
 * Covers what a single-core RTOS needs: global enable, per-IRQ enable with a
 * priority, and routing SPIs to CPU0. The FreeRTOS CA9 port owns PMR/BPR once
 * the scheduler starts; we set permissive defaults for the pre-scheduler window. */
#include "platform.h"

#define GICD(off)  (PLAT_GICD_BASE + (off))
#define GICC(off)  (PLAT_GICC_BASE + (off))

#define GICD_CTLR        GICD(0x000)
#define GICD_TYPER       GICD(0x004)
#define GICD_ISENABLER   GICD(0x100)   /* +4*n */
#define GICD_ICENABLER   GICD(0x180)   /* +4*n */
#define GICD_ICPENDR     GICD(0x280)   /* +4*n */
#define GICD_ICACTIVER   GICD(0x380)   /* +4*n */
#define GICD_IPRIORITYR  GICD(0x400)   /* byte per IRQ */
#define GICD_ITARGETSR   GICD(0x800)   /* byte per IRQ */
#define GICC_CTLR        GICC(0x000)
#define GICC_PMR         GICC(0x004)
#define GICC_BPR         GICC(0x008)

void gic_init(void)
{
    /* DISOWN ABOOT'S INTERRUPT STATE FIRST. aboot leaves its own sources
     * (USB, charger/fuel-gauge status, ...) ENABLED in the distributor, and
     * charger-status lines are LEVEL-triggered: the moment the scheduler's
     * first context switch unmasks IRQs, an asserted-but-unhandled source
     * re-fires forever - a storm that looks exactly like "hangs at
     * vTaskStartScheduler". Whether it strikes depends on PMIC state (e.g.
     * battery FULL on the charger vs mid-charge), which is why identical
     * images booted deep one day and died at the first context switch the
     * next. Disable + clear EVERYTHING; ours get re-enabled explicitly. */
    uint32_t banks = ((mmio_read(GICD_TYPER) & 0x1Fu) + 1u);  /* 32 IRQs each */
    mmio_write(GICD_CTLR, 0);
    for (uint32_t i = 0; i < banks; i++) {
        mmio_write(GICD_ICENABLER + 4u * i, 0xFFFFFFFFu);
        mmio_write(GICD_ICPENDR   + 4u * i, 0xFFFFFFFFu);
        mmio_write(GICD_ICACTIVER + 4u * i, 0xFFFFFFFFu);
    }

    mmio_write(GICD_CTLR, 1);          /* forward group-0 interrupts */
    mmio_write(GICC_PMR, 0xFF);        /* pre-scheduler: mask nothing */
    mmio_write(GICC_BPR, 0);           /* full preemption granularity */
    mmio_write(GICC_CTLR, 1);          /* CPU interface on */
}

/* Restore the per-CPU half of the GIC after a core power-down.
 *
 * A GICv2 splits into a DISTRIBUTOR (shared, in the always-on/cluster domain,
 * survives cpu-pc along with every SPI enable and priority we programmed) and
 * a CPU INTERFACE, which is per-core and dies with the core. Coming back from
 * cpu_pc_resume with GICC_CTLR at 0 means no interrupt is ever presented and
 * the watch looks alive but deaf — including to the PMIC wake we depend on.
 *
 * PPIs are also banked per-CPU in GICD_ISENABLER0, so the architected-timer
 * tick enable is part of what is lost even though it lives in the
 * distributor's address space; cpu_pc.c re-enables it explicitly. */
/* Is this interrupt pending at the DISTRIBUTOR? The distributor is in the
 * always-on domain, so this is the one view of interrupt state that stays
 * meaningful across a core power-down — and the only way to tell "the wake
 * source never asserted" from "it asserted and nothing acted on it". */
int gic_is_pending(unsigned id)
{
    uintptr_t reg = GICD(0x200) + (id / 32u) * 4u;   /* GICD_ISPENDR */
    return (mmio_read(reg) >> (id % 32u)) & 1u;
}

void gic_cpu_resume(void)
{
    mmio_write(GICC_PMR, 0xFF);
    mmio_write(GICC_BPR, 0);
    mmio_write(GICC_CTLR, 1);
}

void gic_disable_irq(unsigned id)
{
    mmio_write(GICD_ICENABLER + (id / 32u) * 4u, 1u << (id % 32u));
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
