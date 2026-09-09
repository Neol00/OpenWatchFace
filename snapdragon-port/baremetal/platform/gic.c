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

uint32_t g_gic_handoff_en[32], g_gic_handoff_banks;
static const unsigned k_tz_irqs[] = { 206u, 269u };
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
    if (banks > 32u) banks = 32u;
    for (uint32_t i = 0; i < banks; i++) g_gic_handoff_en[i] = mmio_read(GICD_ISENABLER + 4u * i);
    g_gic_handoff_banks = banks;
    mmio_write(GICD_CTLR, 0);
    for (uint32_t i = 0; i < banks; i++) {
        mmio_write(GICD_ICENABLER + 4u * i, 0xFFFFFFFFu);
        mmio_write(GICD_ICPENDR   + 4u * i, 0xFFFFFFFFu);
        mmio_write(GICD_ICACTIVER + 4u * i, 0xFFFFFFFFu);
    }
    /* TRUSTZONE'S OWN INTERRUPTS (v173, 2026-09-08). The stock C2+ tzdbg shows
     * TZ servicing two "SPI RPM" interrupts itself, INTIDs 206 and 269: the
     * RPM talks to TZ through them, and a cluster wake is RPM-driven. The
     * blanket disable above took them away at every boot; a CPU-only collapse
     * never needed them, a system collapse plausibly does (every one so far
     * ended in a PS_HOLD reset = TZ/RPM fatal). Put back exactly the ones that
     * were enabled at handoff; they are group 0, so they never reach us. */
    for (unsigned k = 0; k < sizeof k_tz_irqs / sizeof k_tz_irqs[0]; k++) {
        unsigned id = k_tz_irqs[k];
#if defined(TZ_RPM_IRQS_FORCE)
        /* v174: they were NOT enabled at handoff (C2 log) -- stock TZ turns them
         * on later. Enable them ourselves, targeted at cpu0. */
        if (id / 32u < banks) {
            uintptr_t tgt = GICD_ITARGETSR + (id & ~3u);
            mmio_write(tgt, mmio_read(tgt) | (0x01u << ((id & 3u) * 8)));
            mmio_write(GICD_ISENABLER + (id / 32u) * 4u, 1u << (id % 32u));
        }
#else
        if (id / 32u < banks && ((g_gic_handoff_en[id / 32u] >> (id % 32u)) & 1u))
            mmio_write(GICD_ISENABLER + (id / 32u) * 4u, 1u << (id % 32u));
#endif
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

/* DISTRIBUTOR SAVE/RESTORE (2026-09-07). With the L2 flag the whole cluster
 * domain powers down and the distributor comes back at reset (the kernel's
 * gic_dist_save/restore on CPU_CLUSTER_PM). Enables, priorities, targets and
 * edge/level config for every bank; CTLR last. */
#define GICD_ICFGR       GICD(0xC00)   /* 2 bits per IRQ */
static uint32_t s_gd_en[32], s_gd_pri[256], s_gd_tgt[256], s_gd_cfg[64];
static uint32_t s_gd_banks;
void gicd_save(void)
{
    s_gd_banks = ((mmio_read(GICD_TYPER) & 0x1Fu) + 1u);
    if (s_gd_banks > 32u) s_gd_banks = 32u;
    for (uint32_t i = 0; i < s_gd_banks; i++) {
        s_gd_en[i]  = mmio_read(GICD_ISENABLER + 4u * i);
        s_gd_cfg[2u * i]     = mmio_read(GICD_ICFGR + 8u * i);
        s_gd_cfg[2u * i + 1] = mmio_read(GICD_ICFGR + 8u * i + 4u);
        for (uint32_t w = 0; w < 8u; w++) {
            s_gd_pri[8u * i + w] = mmio_read(GICD_IPRIORITYR + 32u * i + 4u * w);
            s_gd_tgt[8u * i + w] = mmio_read(GICD_ITARGETSR  + 32u * i + 4u * w);
        }
    }
}
void gicd_restore(void)
{
    mmio_write(GICD_CTLR, 0);
    for (uint32_t i = 0; i < s_gd_banks; i++) {
        mmio_write(GICD_ICENABLER + 4u * i, 0xFFFFFFFFu);
        mmio_write(GICD_ICPENDR   + 4u * i, 0xFFFFFFFFu);
        mmio_write(GICD_ICACTIVER + 4u * i, 0xFFFFFFFFu);
        if (i >= 1u) {                              /* ICFGR0/ITARGETSR0-7 are read-only/banked */
            mmio_write(GICD_ICFGR + 8u * i,      s_gd_cfg[2u * i]);
            mmio_write(GICD_ICFGR + 8u * i + 4u, s_gd_cfg[2u * i + 1]);
        }
        for (uint32_t w = 0; w < 8u; w++) {
            mmio_write(GICD_IPRIORITYR + 32u * i + 4u * w, s_gd_pri[8u * i + w]);
            if (i >= 1u) mmio_write(GICD_ITARGETSR + 32u * i + 4u * w, s_gd_tgt[8u * i + w]);
        }
        mmio_write(GICD_ISENABLER + 4u * i, s_gd_en[i]);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(GICD_CTLR, 1);
}

/* GICD_ICFGR: 2 bits per IRQ, bit1 = edge-triggered. The MPM IPC line
 * (SPI 171) is requested IRQF_TRIGGER_RISING by the vendor mpm-of.c; as a
 * level it storms (2026-08-07 log death), as an edge it fires once per wake. */
void gic_set_edge(unsigned id, int edge)
{
    uintptr_t reg = GICD_ICFGR + (id / 16u) * 4u;
    uint32_t sh = (id % 16u) * 2u + 1u;
    uint32_t v = mmio_read(reg);
    v = edge ? (v | (1u << sh)) : (v & ~(1u << sh));
    mmio_write(reg, v);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Boot-time census: what aboot/TZ left enabled, and whether TZ's two RPM
 * interrupts are back on. Read after gic_init. */
void gic_handoff_report(void)
{
    con_puts("gic: handoff enabled:");
    for (uint32_t i = 0; i < g_gic_handoff_banks; i++) {
        uint32_t m = g_gic_handoff_en[i];
        for (uint32_t b = 0; m; b++, m >>= 1) if (m & 1u) { con_puts(" "); con_putdec(32u * i + b); }
    }
    con_puts(" | tz-rpm irqs now:");
    for (unsigned k = 0; k < sizeof k_tz_irqs / sizeof k_tz_irqs[0]; k++) {
        unsigned id = k_tz_irqs[k];
        con_puts(" "); con_putdec(id); con_puts((mmio_read(GICD_ISENABLER + (id / 32u) * 4u) >> (id % 32u)) & 1u ? "=on" : "=off");
    }
    con_puts("\n");
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
