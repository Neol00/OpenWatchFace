/* spmi_arb.c — SPMI PMIC-arbiter master (polled, bounded, no IRQ).
 *
 * Extracted from pmic_vib.c so more than one PMIC peripheral driver can share
 * it (vibrator/haptics, RTC, PON, fuel gauge...). Same design rules as the
 * vibrator driver it came from: WRITE/READ are polled with a hard iteration
 * bound so a wrong base address or a dead arbiter can NEVER hang the boot —
 * callers get -1 and carry on.
 *
 * SOURCES (verbatim from the stock DTBs of both watches, and the vendor
 * spmi-pmic-arb driver):
 *   qcom,spmi@200f000, reg-names core/chnls/obsrvr/intr/cnfg
 *     core   = 0x200f000 len 0x1000
 *     chnls  = 0x2400000 len 0x400000     <- writes go through here
 *     obsrvr = 0x2c00000 len 0x400000     <- reads go through here
 *   arb channel 0, EE 0 (qcom,pmic-arb-channel = 0)
 * Identical addresses on the Gen 4 (msm8909/PM8916) and Gen 6 (sdm429w/PM660).
 *
 * The command word layout follows the vendor driver (kept in the same shape so
 * it can be diffed against kernel source):
 *   cmd = (opc << 27) | ((sid & 0xf) << 20) | (addr << 4) | (byte_count - 1)
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#define SPMI_CHNLS_BASE   0x02400000u
#define SPMI_OBSRVR_BASE  0x02C00000u
#define SPMI_CHNL_STRIDE  0x8000u          /* per-channel spacing */

#define SPMI_CH_CMD       0x00u
#define SPMI_CH_CONFIG    0x04u
#define SPMI_CH_STATUS    0x08u
#define SPMI_CH_WDATA0    0x10u
#define SPMI_CH_WDATA1    0x14u
#define SPMI_CH_RDATA0    0x18u
#define SPMI_CH_RDATA1    0x1Cu

/* opcodes (PMIC_ARB_CMD_OP_*) */
#define SPMI_OP_EXT_WRITEL 0x00u           /* extended register write, long */
#define SPMI_OP_EXT_READL  0x01u           /* extended register read, long */

/* status bits */
#define SPMI_ST_DONE       (1u << 0)
#define SPMI_ST_FAILURE    (1u << 1)
#define SPMI_ST_DENIED     (1u << 2)
#define SPMI_ST_DROPPED    (1u << 3)

#define SPMI_POLL_ITERS    100000u

/* ---- traffic census (2026-08-07) -----------------------------------------
 * Hunting the steady 11-12% idle CPU. The LV census cleared rendering (~0% at
 * idle), touch (INT-gated, 57 us/read idle) and the D-cache flush (~0%),
 * leaving ~11% unexplained inside loop(). Every PMIC access funnels through
 * this file, and OWF's loop polls the boot button through it EVERY ITERATION:
 * digitalRead(FOSSIL_PIN_KPDPWR) -> pon_kpdpwr_pressed() -> spmi_read8(), each
 * one a polled bus transaction. That is the remaining suspect with the right
 * signature (constant, independent of UI activity).
 *
 * Ticks are accumulated raw and converted once at print time — a division per
 * transaction would perturb the very thing being measured. */
volatile uint32_t g_spmi_n;
volatile uint64_t g_spmi_ticks;

/* ---- arbiter v2 PPID -> channel (APID) mapping ---------------------------
 * THE FIRST-BOOT LESSON (no buzz despite running code): on arbiter v2 — which
 * this SoC has — a peripheral is NOT reachable through an arbitrary channel.
 * Each PPID ((sid << 8) | (addr >> 8)) is assigned a channel by the boot
 * firmware, published in a table in the arbiter core region:
 *     core + 0x800 + 4*n : entry for channel n; PPID in bits [19:8], 0 = unused
 * and channel n's registers live at (chnls|obsrvr) + 0x8000*n (EE0).
 * Using channel 0 for everything (the old code) silently targets whatever
 * peripheral happens to be first in the table — writes complete as DENIED or
 * hit the wrong block, so the motor never moved.
 * Mirrors pmic_arb_find_apid()/pmic_arb_offset_v2() in the vendor
 * drivers/spmi/spmi-pmic-arb.c. */
#define SPMI_CORE_BASE     0x0200F000u
#define SPMI_APID_TABLE    0x800u
#define SPMI_MAX_APID      128u     /* chnls region 0x400000 / 0x8000 */

/* Small cache so the table is scanned once per distinct PPID. */
#define APID_CACHE_SLOTS   8u
static uint16_t s_ppid_cache[APID_CACHE_SLOTS];
static uint16_t s_apid_cache[APID_CACHE_SLOTS];
static unsigned s_cache_used;

static int spmi_apid_for(uint16_t ppid)
{
    unsigned i;
    for (i = 0; i < s_cache_used; i++)
        if (s_ppid_cache[i] == ppid) return (int)s_apid_cache[i];

    for (i = 0; i < SPMI_MAX_APID; i++) {
        uint32_t e = mmio_read(SPMI_CORE_BASE + SPMI_APID_TABLE + 4u * i);
        if (!e) continue;
        if (((e >> 8) & 0xFFFu) == ppid) {
            if (s_cache_used < APID_CACHE_SLOTS) {
                s_ppid_cache[s_cache_used] = ppid;
                s_apid_cache[s_cache_used] = (uint16_t)i;
                s_cache_used++;
            }
            return (int)i;
        }
    }
    return -1;
}

/* ---- ownership probe (READ-ONLY) -----------------------------------------
 * WHY THIS EXISTS: on this unit the RTC counter write (pmic_rtc.c) tripped an
 * SPMI ownership violation and the secure world answered with an instant TZ
 * reset — a whole flash cycle spent to learn one bit. Any future "can I write
 * this PMIC peripheral?" question must therefore be answered WITHOUT writing.
 *
 * It can be. The arbiter publishes, per APID, which execution environment owns
 * write access, in the cnfg region (reg-names ...\0cnfg = 0x200a000 len
 * 0x2100). Mirrors SPMI_OWNERSHIP_TABLE_REG()/SPMI_OWNERSHIP_PERIPH2OWNER()
 * in the vendor drivers/spmi/spmi-pmic-arb.c:
 *     cnfg + 0x700 + 4*apid, owner EE in bits [2:0]
 * We are EE 0 (DTB: qcom,ee = <0>). owner == 0 means a write from here is
 * permitted by the arbiter; anything else means the write would be DENIED and
 * may escalate to a secure-side reset. Reads go through the observer path and
 * are unaffected either way. */
#define SPMI_CNFG_BASE     0x0200A000u
#define SPMI_OWNER_TABLE   0x0700u
#define SPMI_OUR_EE        0u

/* APID that serves this (sid, addr), or -1 if the peripheral is not mapped. */
int spmi_apid_of(uint8_t sid, uint16_t addr)
{
    return spmi_apid_for((uint16_t)(((sid & 0xFu) << 8) | (addr >> 8)));
}

/* Owning EE for this peripheral, or -1 if it is not in the arbiter table. */
int spmi_owner_ee(uint8_t sid, uint16_t addr)
{
    int apid = spmi_apid_of(sid, addr);
    if (apid < 0) return -1;
    return (int)(mmio_read(SPMI_CNFG_BASE + SPMI_OWNER_TABLE
                           + 4u * (uint32_t)apid) & 0x7u);
}

/* 1 = a write from EE0 is permitted, 0 = it would be denied, -1 = unmapped. */
int spmi_writable(uint8_t sid, uint16_t addr)
{
    int ee = spmi_owner_ee(sid, addr);
    if (ee < 0) return -1;
    return (ee == (int)SPMI_OUR_EE) ? 1 : 0;
}

static inline uint32_t spmi_cmd(uint32_t opc, uint8_t sid, uint16_t addr,
                                unsigned len)
{
    /* v2 command word: the channel already encodes sid+periph, so only the
     * low byte of the register address goes in the command. Keeping sid and
     * the full address here as well matches the vendor driver's fmt_cmd_v2
     * (the arbiter ignores the redundant bits). */
    return (opc << 27)
         | ((uint32_t)(sid & 0xFu) << 20)
         | ((uint32_t)addr << 4)
         | ((uint32_t)(len - 1u) & 0x7u);
}

/* ---- RE-ENTRANCY (2026-08-07) --------------------------------------------
 * THE HANG: pmic_irq.c's interrupt handler clears the PON latch with an SPMI
 * write. Task context uses SPMI constantly — the suspend loop's button poll,
 * the fuel-gauge census, the RTC. Nothing here was mutually exclusive, so an
 * interrupt arriving mid-transaction started a SECOND transaction on the same
 * arbiter while the first was still in flight. Both then polled a STATUS
 * register describing the other's command. The observed symptom was the watch
 * going quiet after a wake and never coming back: every later PMIC access
 * spins to its 100000-iteration bound (~ms each) and returns -1, so the main
 * loop crawls, usb_poll() stops being reached, and the log simply stops.
 *
 * A transaction is ~7 us (measured: us/spmi=7), so masking interrupts for its
 * duration costs nothing anyone can perceive and makes the handler safe. This
 * is the single-core bare-metal equivalent of the vendor driver's
 * pmic_arb->lock. */
static inline uint32_t spmi_irq_save(void)
{
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("cpsid i" ::: "memory");
    return cpsr;
}

static inline void spmi_irq_restore(uint32_t cpsr)
{
    if (!(cpsr & 0x80u))              /* only re-enable if they were enabled */
        __asm__ volatile("cpsie i" ::: "memory");
}

/* Poll a channel's STATUS until DONE (or the bound trips). 0 ok, -1 error. */
static int spmi_wait(uintptr_t ch)
{
    for (unsigned i = 0; i < SPMI_POLL_ITERS; i++) {
        uint32_t st = mmio_read(ch + SPMI_CH_STATUS);
        if (st & SPMI_ST_DONE)
            return (st & (SPMI_ST_FAILURE | SPMI_ST_DENIED | SPMI_ST_DROPPED))
                   ? -1 : 0;
    }
    return -1;
}

/* Write 1..8 bytes to a PMIC register block. 0 ok, -1 error. */
int spmi_write(uint8_t sid, uint16_t addr, const uint8_t *buf, unsigned len)
{
    int apid = spmi_apid_for((uint16_t)(((sid & 0xFu) << 8) | (addr >> 8)));
    uintptr_t ch;
    uint32_t w[2] = { 0, 0 };
    unsigned i;

    if (apid < 0) return -1;               /* peripheral not in the arb table */
    ch = SPMI_CHNLS_BASE + (uintptr_t)apid * SPMI_CHNL_STRIDE;

    if (len < 1 || len > 8) return -1;
    for (i = 0; i < len; i++)
        w[i / 4] |= (uint32_t)buf[i] << ((i % 4) * 8);

    uint64_t t0 = timer_ticks();
    uint32_t fl = spmi_irq_save();     /* the whole command must be atomic */
    mmio_write(ch + SPMI_CH_WDATA0, w[0]);
    if (len > 4)
        mmio_write(ch + SPMI_CH_WDATA1, w[1]);
    mmio_write(ch + SPMI_CH_CMD, spmi_cmd(SPMI_OP_EXT_WRITEL, sid, addr, len));
    int wrc = spmi_wait(ch);
    spmi_irq_restore(fl);
    g_spmi_ticks += timer_ticks() - t0;
    g_spmi_n++;
    return wrc;
}

/* Read 1..8 bytes from a PMIC register block (observer region). 0 ok, -1 err. */
int spmi_read(uint8_t sid, uint16_t addr, uint8_t *buf, unsigned len)
{
    int apid = spmi_apid_for((uint16_t)(((sid & 0xFu) << 8) | (addr >> 8)));
    uintptr_t ch;
    uint32_t w[2];
    unsigned i;

    if (apid < 0) return -1;
    ch = SPMI_OBSRVR_BASE + (uintptr_t)apid * SPMI_CHNL_STRIDE;

    if (len < 1 || len > 8) return -1;

    uint64_t t0 = timer_ticks();
    /* Command AND result read are one atomic unit: an interrupt landing
     * between spmi_wait() and the RDATA reads would let the handler's own
     * transaction overwrite the result we are about to sample. */
    uint32_t fl = spmi_irq_save();
    mmio_write(ch + SPMI_CH_CMD, spmi_cmd(SPMI_OP_EXT_READL, sid, addr, len));
    int wrc = spmi_wait(ch);
    if (wrc == 0) {
        w[0] = mmio_read(ch + SPMI_CH_RDATA0);
        w[1] = (len > 4) ? mmio_read(ch + SPMI_CH_RDATA1) : 0;
    }
    spmi_irq_restore(fl);
    g_spmi_ticks += timer_ticks() - t0;
    g_spmi_n++;
    if (wrc < 0) return -1;
    for (i = 0; i < len; i++)
        buf[i] = (uint8_t)(w[i / 4] >> ((i % 4) * 8));
    return 0;
}

int spmi_write8(uint8_t sid, uint16_t addr, uint8_t val)
{
    return spmi_write(sid, addr, &val, 1);
}

int spmi_read8(uint8_t sid, uint16_t addr, uint8_t *val)
{
    return spmi_read(sid, addr, val, 1);
}

#endif /* PLAT_SOC_MSM */
