/* cpu_clk_a7.c — CPU frequency control for the Fossil Gen 4 (msm8909w A7).
 *
 * CORRECTION TO AN EARLIER CLAIM IN THIS PORT (2026-08-28). The Gen 4 was
 * documented as unable to scale its CPU without first porting the RPM, on the
 * grounds that "the A7 does not own its own clocks". That is half right, and
 * the wrong half was load-bearing: this watch's own DTB has
 *
 *     qcom,clock-a7@0b011050 {
 *         compatible = "qcom,clock-a53-8916";
 *         reg = <0xb011050 0x08  0x5c00c 0x08>;   // rcg-base, efuse
 *         qcom,safe-freq = <0x17d78400>;          // 400 MHz
 *         cpu-vdd-supply = <0xa7>;
 *         clock-names = "clk-4", "clk-5";
 *
 * — an ordinary RCG mux/divider at 0xB011050 that the CPU writes directly,
 * exactly like the GCC RCGs this port already drives for MDSS, BLSP and SDCC.
 * What the RPM really owns is the VOLTAGE (cpu-vdd-supply), and that is what
 * limits going UP, not the clock itself.
 *
 * Register model, from drivers/clk/msm/clock-a7.c (a7ssmux) and the generic
 * RCG code in clock-local2.c (rcg_set_src_div / rcg_update_config):
 *   base + 0x0  CMD_RCGR   bit0 UPDATE (self-clearing), bit31 ROOT_OFF,
 *                          bits 7:4 CONFIG_DIRTY
 *   base + 0x4  CFG_RCGR   bits 4:0 divider, bits 10:8 source select
 *
 * HALF-INTEGER DIVIDERS. a7ssmux sets is_half_divider, and the kernel stores
 * the divider DOUBLED, then writes (doubled - 1):
 *     register value = 2 * N - 1     for a real divisor of N
 * so /1 is 1, /1.5 is 2, /2 is 3, /3 is 5, /4 is 7. Reading back reverses it:
 * doubled = raw + 1 (with raw 0 also meaning bypass).
 *
 * SOURCE SELECT. of_get_clk_src() in clock-a7.c derives the mux value from the
 * clock-name itself — `parents[j].sel = i` for "clk-%d" — so this watch's
 * "clk-4", "clk-5" mean mux select 4 and 5, and the probe treats parents[0]
 * (4) as the aux clock and parents[1] (5) as the main PLL. Select 4 is GPLL0,
 * which clock-gcc-8909.c fixes at 800000000.
 *
 * WHY ONLY FOUR RATES, AND WHY THEY ARE THE SAFE ONES.
 * Every rate here comes from GPLL0 with a divider, so nothing touches a PLL:
 *     800 = 800/1     533 = 800/1.5     400 = 800/2     200 = 800/4
 * (and 800/1.5 is exactly the 533330 kHz that looks so strange in the DTB's
 * qcom,cpufreq-table — it is a half-integer divide, not a typo.)
 *
 * The two rates above these, 1094.4 and 1267.2 MHz, need the dedicated A7 PLL
 * reprogrammed AND a higher voltage corner: the DTB's speed bin
 *     qcom,speed0-bin-v0 = <0 0, 800000000 4, 1267200000 9>
 * puts everything up to 800 MHz at corner 4 and the top rates at corner 9.
 * Voting corner 9 means talking to the RPM, which is not ported — so those
 * rates are refused rather than attempted.
 *
 * Conversely, every rate offered here is safe at ANY corner the bootloader
 * could have left us in: a running CPU is at corner 4 or above, and corner 4
 * already covers the whole 0-800 MHz range. Lowering frequency at a voltage
 * set for a higher one is always safe; it is only wasteful.
 *
 * THE ONE-WAY-DOOR CAVEAT: if aboot left the CPU on the A7 PLL above 800 MHz,
 * selecting any rate here switches the mux to GPLL0 and there is no way back
 * to the PLL rate without programming the PLL. A reboot restores it.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

int cpu_volt_set_for_mhz(int mhz);   /* cpu_volt_a7.c */

#define A7_RCG_BASE     0x0B011050u
#define A7_CMD_RCGR     (A7_RCG_BASE + 0x0u)
#define A7_CFG_RCGR     (A7_RCG_BASE + 0x4u)

#define CFG_DIV_MASK    0x1Fu           /* bits 4:0  */
#define CFG_SRC_MASK    (0x7u << 8)     /* bits 10:8 */
#define CFG_SRC_SHIFT   8u

#define CMD_UPDATE      (1u << 0)
#define CMD_ROOT_OFF    (1u << 31)
#define CMD_DIRTY_MASK  (0xFu << 4)

#define A7_SRC_GPLL0    4u              /* DT "clk-4" -> mux sel 4 (aux)  */
#define A7_SRC_A7PLL    5u              /* DT "clk-5" -> mux sel 5 (PLL)  */
#define GPLL0_MHZ       800u

/* target MHz -> DOUBLED divisor (the register takes doubled - 1) */
static const struct { uint16_t mhz; uint8_t doubled; } k_a7_rates[] = {
    { 800, 2 },      /* 800 / 1   */
    { 533, 3 },      /* 800 / 1.5 */
    { 400, 4 },      /* 800 / 2   */
    { 200, 8 },      /* 800 / 4   */
};

static uint32_t s_boot_cfg;
static int      s_boot_saved;

static int a7_rcg_update(void)
{
    mmio_write(A7_CMD_RCGR, mmio_read(A7_CMD_RCGR) | CMD_UPDATE);
    /* The UPDATE bit is self-clearing when the new config is latched. The
     * kernel spins up to UPDATE_CHECK_MAX_LOOPS at 1 us; bound it in ms. */
    uint32_t t0 = timer_ms();
    while (mmio_read(A7_CMD_RCGR) & CMD_UPDATE) {
        if ((uint32_t)(timer_ms() - t0) > 10u) return -1;
    }
    return 0;
}

int pwr_cpu_mhz(void)
{
    uint32_t cfg = mmio_read(A7_CFG_RCGR);
    uint32_t src = (cfg & CFG_SRC_MASK) >> CFG_SRC_SHIFT;
    uint32_t raw = cfg & CFG_DIV_MASK;
    uint32_t doubled = (raw ? raw : 1u) + 1u;   /* kernel's rcg_get_src_div */

    if (src != A7_SRC_GPLL0)
        return -1;    /* on the A7 PLL: its rate needs the PLL block, not ported */
    if (!doubled) return -1;
    return (int)((GPLL0_MHZ * 2u) / doubled);
}

int cpu_clk_set_mhz(int mhz)
{
    unsigned i;
    uint32_t cfg;

    if (!s_boot_saved) {                 /* remember what aboot chose, once */
        s_boot_cfg = mmio_read(A7_CFG_RCGR);
        s_boot_saved = 1;
    }

    for (i = 0; i < sizeof k_a7_rates / sizeof k_a7_rates[0]; i++)
        if (k_a7_rates[i].mhz == (uint16_t)mhz) break;
    if (i == sizeof k_a7_rates / sizeof k_a7_rates[0]) {
        con_puts("cpu-clk: "); con_putdec((uint32_t)mhz);
        con_puts(" MHz not reachable from GPLL0 (needs the A7 PLL + an RPM "
                 "voltage vote); refused\n");
        return -1;
    }

    /* A pending config means someone else is mid-update; do not stack on it. */
    if (mmio_read(A7_CMD_RCGR) & CMD_DIRTY_MASK) {
        con_puts("cpu-clk: config dirty, refusing\n");
        return -1;
    }

    /* VOLTAGE ORDERING (see cpu_volt_a7.c). Going UP, the rail must rise
     * BEFORE the clock or the core is briefly underv olted at the new speed;
     * going DOWN, the clock must drop first for the same reason in reverse.
     * Getting this backwards is the classic way to make an otherwise correct
     * DVFS implementation fail intermittently and only under load. */
    int now_mhz = pwr_cpu_mhz();
    int going_up = (now_mhz < 0) || (mhz > now_mhz);
    if (going_up) cpu_volt_set_for_mhz(mhz);

    cfg = mmio_read(A7_CFG_RCGR);
    cfg &= ~(CFG_DIV_MASK | CFG_SRC_MASK);
    cfg |= (uint32_t)(k_a7_rates[i].doubled - 1u) & CFG_DIV_MASK;
    cfg |= (A7_SRC_GPLL0 << CFG_SRC_SHIFT) & CFG_SRC_MASK;
    mmio_write(A7_CFG_RCGR, cfg);
    __asm__ volatile("dsb sy" ::: "memory");

    if (a7_rcg_update() < 0) {
        con_puts("cpu-clk: RCG update stuck\n");
        return -1;
    }

    if (!going_up) cpu_volt_set_for_mhz(mhz);

    int now = pwr_cpu_mhz();
    bdiag_puts("cpu-clk: -> "); bdiag_putdec((uint32_t)k_a7_rates[i].mhz);
    bdiag_puts(" MHz (readback "); bdiag_putdec((uint32_t)(now < 0 ? 0 : now));
    bdiag_puts(", cfg "); bdiag_puthex(mmio_read(A7_CFG_RCGR)); bdiag_puts(")\n");
    return now;
}

#endif /* PLAT_SOC_MSM8909 */
