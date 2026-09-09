/* sleep_quiesce.c — the AP-side blocks that keep running INSIDE a collapsed
 * window on the msm8909w watches (2026-09-09).
 *
 * WHY A SECOND FILE NEXT TO sleep_floor.c: everything sleep_floor.c votes for
 * goes into the RPM's ACTIVE set, and the active set stops mattering the moment
 * the RPM applies the APSS SLEEP set — which is exactly what SYS_PC_8909 now
 * gets it to do (6-8 mA measured, on par with stock). What the RPM's sleep set
 * does NOT cover is anything the AP owns in its own registers, because the RPM
 * never sees it:
 *
 *   - GCC_APCS_GPLL_ENA_VOTE still holds the bootloader's votes for GPLL1 and
 *     GPLL2 (912 MHz). Nothing in this firmware consumes either — the cluster,
 *     MDP and the buses all run off GPLL0 — but a voted PLL stays locked and
 *     spinning through the collapse. Same for GCC_APCS_CLOCK_BRANCH_ENA_VOTE's
 *     crypto/crypto-AXI/crypto-AHB/PRNG-AHB branches: the bootloader used the
 *     crypto engine for image authentication and never released them.
 *   - The USB PHY's 480 MHz PLL runs whenever PORTSC.PHCD is clear, cable or
 *     no cable. PORTSC.PHCD stops it; the controller's register block stays
 *     clocked, which is the whole point (see below).
 *
 * All of it is restored at wake before anything is re-enabled, so a sleep is
 * still transparent to every driver. Deliberately NOT here: the MDSS GDSC.
 * Collapsing that power domain loses the MDP/DSI register state and would need
 * a full DSI re-init on wake rather than the DCS 0x11/0x29 the resume does now.
 *
 * ALSO DELIBERATELY NOT HERE, AND THE REASON v205 REBOOTED THE INSTANT IT SLEPT
 * (2026-09-09): gating the four USB GCC branches with gcc_usb_sleep(). THE LOG
 * CONSOLE IS THAT USB CONTROLLER. Everything after this point still prints --
 * this file's own summary, the sys-pc census, "sys-pc: armed" -- and every one
 * of those con_flush()/usb_poll() calls is an MMIO access into a block whose
 * AHB clock has just been stopped. An unclocked slave is a NoC error, and the
 * 8909's NOCERR goes to TrustZone, which drops PS_HOLD: an instant reset that
 * looks exactly like "it reboots when it sleeps". Same failure shape as the
 * unvoted PRNG in section 26 of the findings. The clocks could only be gated
 * after the LAST print, inside the collapse path itself, and the branch clocks
 * are worth far less than the PHY PLL that PORTSC.PHCD already stops -- so we
 * simply do not gate them. gcc_usb_sleep() stays in gcc_usb.c, unused, with
 * this warning attached to it.
 *
 * Enabled with -DSLEEP_QUIESCE. Prints one line per sleep with the PLL mode
 * registers either side of the vote, so "did GPLL1 actually stop" is a fact in
 * the log rather than an assumption. */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909) && defined(SLEEP_QUIESCE)

#define GCC_APCS_GPLL_ENA_VOTE   0x45000u
#define GCC_APCS_BRANCH_ENA_VOTE 0x45004u
#define GPLL_VOTE_GPLL1  (1u << 1)
#define GPLL_VOTE_GPLL2  (1u << 3)
/* GPLL0 (bit 0) and BIMC (bit 2) are NEVER touched: the cluster is running at
 * 400 MHz on GPLL0 when it collapses and TZ warm-boots it back onto the same
 * mux, and BIMC is the DDR. */
#define BRANCH_VOTE_CRYPTO_AHB (1u << 0)
#define BRANCH_VOTE_CRYPTO_AXI (1u << 1)
#define BRANCH_VOTE_CRYPTO     (1u << 2)
#define BRANCH_VOTE_PRNG_AHB   (1u << 8)
#define BRANCH_SLEEP_MASK (BRANCH_VOTE_CRYPTO_AHB | BRANCH_VOTE_CRYPTO_AXI | \
                           BRANCH_VOTE_CRYPTO | BRANCH_VOTE_PRNG_AHB)
#define GPLL1_MODE 0x20000u
#define GPLL2_MODE 0x25000u

static uint32_t s_gpll_vote0, s_branch_vote0;
static uint8_t  s_applied, s_usb_down;

/* Bisect switches: -DSLEEP_QUIESCE_NO_PLL / -DSLEEP_QUIESCE_NO_USB drop one
 * half each, so "which of the two did it" never costs a guess. */
#if defined(SLEEP_QUIESCE_NO_PLL)
#define QUIESCE_PLL 0
#else
#define QUIESCE_PLL 1
#endif
#if defined(SLEEP_QUIESCE_NO_USB)
#define QUIESCE_USB 0
#else
#define QUIESCE_USB 1
#endif

void sleep_quiesce_enter(int cable_live)
{
    s_gpll_vote0   = mmio_read(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE);
    s_branch_vote0 = mmio_read(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE);
    if (QUIESCE_PLL) {
        mmio_write(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE,
                   s_gpll_vote0 & ~(GPLL_VOTE_GPLL1 | GPLL_VOTE_GPLL2));
        mmio_write(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE,
                   s_branch_vote0 & ~BRANCH_SLEEP_MASK);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    s_applied = 1;

    /* Report BEFORE the USB step: this line goes out over that same controller,
     * and after PORTSC.PHCD the PHY is no longer moving bytes onto the wire. */
    con_puts("quiesce: gpll vote "); con_puthex(s_gpll_vote0);
    con_puts(" -> "); con_puthex(mmio_read(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE));
    con_puts(" branch "); con_puthex(s_branch_vote0);
    con_puts(" -> "); con_puthex(mmio_read(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE));
    con_puts(" gpll1="); con_puthex(mmio_read(PLAT_GCC_BASE + GPLL1_MODE));
    con_puts(" gpll2="); con_puthex(mmio_read(PLAT_GCC_BASE + GPLL2_MODE));

    /* USB PHY: only with no cable. With one attached the console is the whole
     * point of the sleep log, and usb_dev_reinit() puts it back on wake. */
    s_usb_down = 0;
    if (QUIESCE_USB && !cable_live && chg_usb_present() != 1) { s_usb_down = 1; }
    con_puts(s_usb_down ? " usb-phy=lp\n" : " usb-phy=on (cable)\n");
    con_flush();
    if (s_usb_down) usb_phy_lowpower(1);   /* after the last byte is flushed */
}

void sleep_quiesce_exit(void)
{
    if (!s_applied) return;
    s_applied = 0;
    if (s_usb_down) { usb_phy_lowpower(0); s_usb_down = 0; }
    /* Votes back BEFORE any branch is re-enabled: a branch whose PLL is still
     * unvoted comes up unclocked and its CBCR never clears CLK_OFF. */
    if (QUIESCE_PLL) {
        mmio_write(PLAT_GCC_BASE + GCC_APCS_GPLL_ENA_VOTE, s_gpll_vote0);
        mmio_write(PLAT_GCC_BASE + GCC_APCS_BRANCH_ENA_VOTE, s_branch_vote0);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    timer_delay_us(100u);            /* PLL relock before anything consumes it */
}

#endif  /* PLAT_SOC_MSM8909 && SLEEP_QUIESCE */
