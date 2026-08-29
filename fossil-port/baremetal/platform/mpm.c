/* mpm.c — MSM Power Manager (vMPM) observation, and the road to waking a
 * power-collapsed core.
 *
 * WHY: cpu_pc_selftest() proved our warm-boot save/restore is correct, so the
 * cpu-pc failure is the WAKE — the core switched off and the PMIC interrupt
 * never brought it back. On MSM the always-on block responsible for that is
 * the MPM: it watches a fixed set of wake pins while the AP is down and pokes
 * the RPM to power things back up.
 *
 * REGISTER LAYOUT — verified against the vendor mpm-of.c, not remembered:
 *     msm_mpm_read(reg, sub):
 *         offset = reg * MSM_MPM_REG_WIDTH + sub + 2
 *         readl(vmpm_base + offset * 4)
 *     MSM_MPM_REG_WIDTH = DIV_ROUND_UP(num_mpm_irqs, 32) = 64/32 = 2
 *     regs: 0 ENABLE, 1 FALLING_EDGE, 2 RISING_EDGE, 3 POLARITY, 4 STATUS
 * The "+2" is not padding — the driver comments it as "the 64 bit timer in
 * the vMPM mapping". That timer is the always-on wake clock, and it is how
 * timed wake from a collapsed core will eventually work (the architected
 * timer cannot: it dies with the core, which is what qcom,use-broadcast-timer
 * on cpu-pc means).
 *
 * Addresses from the DTB (sda429-hoki-decompiled.dts:4252):
 *     wake-gic: reg = <0x601d0 0x1000>, <0xb011008 0x4>
 *               reg-names = "vmpm", "ipc"
 *               qcom,num-mpm-irqs = <0x40>
 *               interrupts = <0 0xab 1>   -> MPM's own IRQ, SPI 171/INTID 203
 * 0x601d0 sits inside RPM MSG RAM (0x60000 len 0x8000) and the flat map makes
 * it plain Device memory, so no mapping work is needed.
 *
 * THE PIN — FOUND, not guessed (mpm-sdm429.c, the SoC-specific table this
 * driver family keys off "qcom,mpm-gic-sdm429"):
 *     const struct mpm_pin mpm_sdm429_gic_chip_data[] = {
 *         {2, 216}, {49, 172}, {53, 104}, {58, 166}, {62, 222}, {-1},
 *     };
 * with struct mpm_pin { int pin; irq_hw_number_t hwirq; } (mpm.h). GIC hwirq
 * 222 is our SPMI arbiter interrupt — the DT says SPI 190 and the GIC domain
 * maps SPI n to hwirq n+32 — so the PMIC (buttons, RTC alarm) is MPM PIN 62.
 * Only five GIC interrupts on this SoC can wake a collapsed core at all, and
 * ours is one of them.
 *
 * THE DOORBELL: msm_mpm_send_interrupt() is writel(2, ipc_reg) — a hardcoded
 * 2, which is why hoki's DTB carries no qcom,ipc-bit-offset (the older
 * "qcom,mpm-v2" driver read it from DT; this one does not).
 *
 * DETECT TYPE: level-high. The per-peripheral PON interrupt is edge-triggered
 * (pmic_irq.c programs rising-only), but the ARBITER SUMMARY line into the
 * GIC is a level: it stays asserted while any APID has an unserviced
 * interrupt. A collapsed core cannot run the handler that clears it, so the
 * line will simply be high and stay high — exactly what level-high detect
 * wakes on. Edge detect would race the collapse and could miss it entirely.
 *
 * OLD NOTE, KEPT because the reasoning still applies to the GPIO table:
 * The MPM knows nothing about GIC interrupt numbers: it has 64 numbered pins,
 * and each SoC has a fixed table saying which GIC interrupt is wired to which
 * pin. In the OLD driver (compatible "qcom,mpm-v2") that table is in the
 * device tree as <pin, hwirq> tuples. hoki uses the NEWER split wake-gic /
 * wake-gpio nodes, whose DTB carries no map at all — so its table is compiled
 * into a driver keyed on "qcom,mpm-gic-sdm429", which we do not have.
 *
 * Guessing a pin number is not viable: arming the wrong pin looks exactly
 * like arming none, so a wrong guess costs a whole flash cycle and teaches
 * nothing. Instead, MEASURE it. STATUS is a live per-pin view, so:
 *     dump at suspend entry -> press a button -> dump at wake
 * and the STATUS bit that changed IS the MPM pin behind the PMIC's GIC 190.
 * Everything here is read-only until that number is known.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#define MPM_VMPM_BASE   0x000601D0u
#define MPM_IPC_REG     0x0B011008u
#define MPM_NR_PINS     64u
#define MPM_REG_WIDTH   (MPM_NR_PINS / 32u)          /* 2 words per bank */

/* +2 words: the vMPM mapping opens with a 64-bit timer (vendor comment). */
#define MPM_REG(reg, sub) \
    (MPM_VMPM_BASE + (((reg) * MPM_REG_WIDTH + (sub) + 2u) * 4u))

enum {
    MPM_ENABLE   = 0,
    MPM_FALLING  = 1,
    MPM_RISING   = 2,
    MPM_POLARITY = 3,
    MPM_STATUS   = 4,
};

static void mpm_bank(const char *name, unsigned reg)
{
    con_puts(name);
    for (unsigned s = 0; s < MPM_REG_WIDTH; s++) {
        con_puts(" ");
        con_puthex(mmio_read(MPM_REG(reg, s)));
    }
}

/* Live STATUS, the two words that name which wake pins are asserting. */
uint32_t mpm_status(unsigned word)
{
    return (word < MPM_REG_WIDTH) ? mmio_read(MPM_REG(MPM_STATUS, word)) : 0u;
}

/* Full read-only census. `tag` says where in the flow it was taken so two
 * dumps can be diffed against each other. */
void mpm_dump(const char *tag)
{
    diag_puts("mpm["); diag_puts(tag); diag_puts("]");
    /* The 64-bit always-on timer that occupies the first two words. */
    diag_puts(" timer=");
    diag_puthex(mmio_read(MPM_VMPM_BASE + 4u));
    diag_puts(":");
    diag_puthex(mmio_read(MPM_VMPM_BASE + 0u));
    mpm_bank(" en=",   MPM_ENABLE);
    mpm_bank(" fall=", MPM_FALLING);
    mpm_bank(" rise=", MPM_RISING);
    mpm_bank(" pol=",  MPM_POLARITY);
    mpm_bank(" sts=",  MPM_STATUS);
    diag_puts("\n");
}

/* ---- programming -------------------------------------------------------- */

/* MPM pin for GIC hwirq 222 = SPI 190 = the SPMI arbiter summary interrupt,
 * i.e. every PMIC wake source this firmware uses. From mpm_sdm429_gic_chip_data. */
#define MPM_PIN_PMIC    62u

/* MPM's own interrupt (DTB: interrupts = <0 0xab 1>) -> INTID 32 + 171. */
#define MPM_IPC_INTID   203u

volatile uint32_t g_mpm_ram_live;  /* vMPM writes actually stick */
volatile uint32_t g_mpm_irq_n;       /* MPM IPC interrupts taken       */
volatile uint32_t g_mpm_last_sts0;   /* STATUS words at the last wake  */
volatile uint32_t g_mpm_last_sts1;

static void mpm_rmw(unsigned reg, unsigned pin, int on)
{
    unsigned idx = pin / 32u, bit = pin % 32u;
    uint32_t v = mmio_read(MPM_REG(reg, idx));
    v = on ? (v | (1u << bit)) : (v & ~(1u << bit));
    mmio_write(MPM_REG(reg, idx), v);
}

static int mpm_enabled(unsigned pin)
{
    return (mmio_read(MPM_REG(MPM_ENABLE, pin / 32u)) >> (pin % 32u)) & 1u;
}

/* Tell the RPM our wake configuration is live. Vendor: writel(2, ipc). */
static void mpm_ring_doorbell(void)
{
    mmio_write(MPM_IPC_REG, 2u);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Arm the PMIC wake pin, level-high. Returns 1 if the enable read back set —
 * checked rather than assumed, because arming the wrong thing is
 * indistinguishable from arming nothing once the core is down, and the caller
 * must NOT power-collapse if this failed. */
int mpm_arm_pmic_wake(void)
{
    /* Never arm against a region whose writes do not stick — the caller would
     * read back a bit that was never stored and collapse the core with no
     * wake source at all. */
    if (!g_mpm_ram_live) return 0;

    /* Stale pending state would wake us instantly; clear both STATUS words. */
    for (unsigned s = 0; s < MPM_REG_WIDTH; s++)
        mmio_write(MPM_REG(MPM_STATUS, s), 0u);

    mpm_rmw(MPM_RISING,   MPM_PIN_PMIC, 0);   /* level, not edge — see header */
    mpm_rmw(MPM_FALLING,  MPM_PIN_PMIC, 0);
    mpm_rmw(MPM_POLARITY, MPM_PIN_PMIC, 1);   /* wake while the line is HIGH  */
    mpm_rmw(MPM_ENABLE,   MPM_PIN_PMIC, 1);
    __asm__ volatile("dsb sy" ::: "memory");

    mpm_ring_doorbell();
    return mpm_enabled(MPM_PIN_PMIC);
}

void mpm_disarm_pmic_wake(void)
{
    mpm_rmw(MPM_ENABLE, MPM_PIN_PMIC, 0);
    __asm__ volatile("dsb sy" ::: "memory");
    mpm_ring_doorbell();
}

/* MPM IPC interrupt: raised after a wake so the AP can see which pins fired.
 * Vendor clears STATUS by writing 0. */
static void mpm_irq_handler(void *arg)
{
    (void)arg;
    g_mpm_irq_n++;
    g_mpm_last_sts0 = mmio_read(MPM_REG(MPM_STATUS, 0));
    g_mpm_last_sts1 = mmio_read(MPM_REG(MPM_STATUS, 1));
    for (unsigned s = 0; s < MPM_REG_WIDTH; s++)
        mmio_write(MPM_REG(MPM_STATUS, s), 0u);
}

/* One-shot boot census + IPC interrupt hookup. */
void mpm_report(void)
{
    diag_puts("mpm: vmpm@"); diag_puthex(MPM_VMPM_BASE);
    diag_puts(" ipc@");      diag_puthex(MPM_IPC_REG);
    diag_puts(" pins=");     diag_putdec(MPM_NR_PINS);
    diag_puts(" pmic_pin="); diag_putdec(MPM_PIN_PMIC);
    diag_puts(" (gic hwirq 222) ipc_intid="); diag_putdec(MPM_IPC_INTID);
    diag_puts("\n");

    /* THE MPM IPC INTERRUPT IS DELIBERATELY *NOT* ENABLED (2026-08-07).
     *
     * The previous image registered and enabled INTID 203 here, and the log
     * died immediately after this very function — no "[power] entering deep
     * sleep", no PWR lines, nothing. That is the signature of an interrupt
     * storm: an asserted source whose handler cannot deassert it re-fires the
     * instant we EOI, the main loop crawls, usb_poll() starves and the log
     * stops. Writing STATUS=0 is what the vendor does, but if the source is
     * asserted for some other reason it never clears.
     *
     * And we do not need it: its only job is to tell the AP which pins fired,
     * which cpu_pc_sleep() can read straight out of STATUS after it resumes.
     * Enabling an interrupt we do not need, whose deassert path is unverified,
     * bought nothing and cost a cycle. mpm_irq_handler stays for when the MPM
     * is understood well enough to wire it deliberately. */
    (void)mpm_irq_handler;

    mpm_dump("boot");

    /* IS THE REGION EVEN LIVE? An all-zero dump reads identically whether
     * nothing is configured or the reads are going nowhere, and that
     * ambiguity is not survivable: if writes do not stick, arming a wake
     * "succeeds" and the core collapses with nothing able to wake it.
     * So prove it — set our pin's ENABLE bit, read it back, restore. No
     * doorbell, no side effects beyond a bit we put back. */
    {
        unsigned idx = MPM_PIN_PMIC / 32u, bit = MPM_PIN_PMIC % 32u;
        uint32_t orig = mmio_read(MPM_REG(MPM_ENABLE, idx));
        mmio_write(MPM_REG(MPM_ENABLE, idx), orig | (1u << bit));
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t back = mmio_read(MPM_REG(MPM_ENABLE, idx));
        mmio_write(MPM_REG(MPM_ENABLE, idx), orig);
        __asm__ volatile("dsb sy" ::: "memory");

        g_mpm_ram_live = ((back >> bit) & 1u) ? 1u : 0u;
        diag_puts("mpm: vmpm write probe ");
        diag_puts(g_mpm_ram_live ? "LIVE" : "DEAD");
        diag_puts(" (wrote bit "); diag_putdec(bit);
        diag_puts(" of en[");      diag_putdec(idx);
        diag_puts("], read back "); diag_puthex(back);
        diag_puts(")\n");
        if (!g_mpm_ram_live)
            diag_puts("mpm: writes do not stick -> MPM wake cannot be armed;"
                     " cpu-pc must stay disabled\n");
    }
    diag_flush();
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
