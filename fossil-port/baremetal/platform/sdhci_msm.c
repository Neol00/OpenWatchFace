/* sdhci_msm.c — READ-ONLY eMMC access via the standard SDHCI host controller
 * (qcom,sdhci-msm — hc_mem @ 0x7824900 on both watches' boot device).
 *
 * DESIGN: the same "reuse what aboot set up" philosophy as fb_splash.c. aboot
 * just finished reading the boot image off this controller, so it hands over
 * with clocks on, the bus tuned, and the card selected in transfer state. We
 * program NONE of that — no clock config, no card init, no mode switching.
 * The driver only issues single-block READ commands (CMD17) with polled PIO,
 * every wait bounded, so:
 *   - a controller aboot did NOT leave alive fails cleanly in ms (no hang),
 *   - the card's contents cannot be modified by anything in this file — there
 *     is deliberately no write path AT ALL until the port has a tested one.
 *
 * WHY: storage is the last big unblocked Milestone-3 item. Read-only gets us
 * the GPT partition map (needed to ever place persistent data safely) and lets
 * later code read assets/config from disk, all at zero risk to the stock OS.
 *
 * SOURCES: SDHCI is a JEDEC/SD-standard register file (offsets below are the
 * universal ones); base + reg layout verbatim from the DTB node sdhci@7824900
 * (reg-names hc_mem/core_mem/cmdq_mem). Gen 4's boot sdhci is at the same
 * hc_mem address (msm8909 sdhc1).
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

#include <string.h>

#define SDHC_BASE          0x07824900u      /* hc_mem: standard SDHCI regs */

/* Standard SDHCI host-controller registers */
#define SDHCI_BLOCK_SIZE   0x04u            /* 16-bit */
#define SDHCI_BLOCK_COUNT  0x06u            /* 16-bit */
#define SDHCI_ARGUMENT     0x08u
#define SDHCI_XFER_MODE    0x0Cu            /* 16-bit */
#define SDHCI_COMMAND      0x0Eu            /* 16-bit */
#define SDHCI_RESPONSE0    0x10u
#define SDHCI_BUFFER       0x20u
#define SDHCI_PRESENT      0x24u
#define SDHCI_INT_STATUS   0x30u
#define SDHCI_INT_ENABLE   0x34u

#define PRESENT_CMD_INHIBIT   (1u << 0)
#define PRESENT_DAT_INHIBIT   (1u << 1)

#define INT_CMD_COMPLETE   (1u << 0)
#define INT_XFER_COMPLETE  (1u << 1)
#define INT_BUF_RD_READY   (1u << 5)
#define INT_ERROR          (1u << 15)

#define XFER_MODE_READ     (1u << 4)        /* data direction: card -> host */
#define INT_BUF_WR_READY   (1u << 4)
#define MMC_WRITE_SINGLE   24u

/* COMMAND register: [13:8] index, bit5 data-present, bit4 index-check,
 * bit3 CRC-check, [1:0] response type (2 = 48-bit). */
#define CMD_R1(idx)        ((uint16_t)(((idx) << 8) | 0x1Au))
#define CMD_R1_DATA(idx)   ((uint16_t)(((idx) << 8) | 0x3Au))

#define MMC_READ_SINGLE    17u
#define EMMC_BLOCK         512u

static inline void hc_w32(uint32_t off, uint32_t v) { mmio_write(SDHC_BASE + off, v); }
static inline uint32_t hc_r32(uint32_t off)         { return mmio_read(SDHC_BASE + off); }
/* 16-bit registers, accessed via their containing word (keeps mmio 32-bit). */
static inline void hc_w16(uint32_t off, uint16_t v)
{
    uint32_t word = hc_r32(off & ~3u);
    unsigned sh = (off & 2u) * 8u;
    word = (word & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    hc_w32(off & ~3u, word);
}

static int s_emmc_ok = 0;
static int s_slow_mode;   /* STORAGE27: dropped to 400 kHz after a data CRC */
static uint32_t s_rca;               /* card's relative address, found at wake */
static int emmc_wake(void);

/* Reset the controller's CMD+DAT state machines (SW_RESET byte @0x2F, bits
 * 1|2, self-clearing) — the kernel does this after every command error; a
 * latched error state can poison every later command. */
static void hc_reset_lines(void)
{
    uint32_t w = hc_r32(0x2Cu);
    hc_w32(0x2Cu, (w & 0x00FFFFFFu) | (0x06u << 24));
    uint32_t t0 = timer_ms();
    while (hc_r32(0x2Cu) & (0x06u << 24)) {
        if ((uint32_t)(timer_ms() - t0) > 20u) break;
    }
}

static uint32_t s_last_err;   /* INT_STATUS at last failure (0 = silent timeout) */
uint32_t emmc_last_error(void) { return s_last_err; }

/* WHERE the init ladder failed, as a display color (STORAGE16 — the first
 * boot with live clocks showed the old fixes never really ran; localize):
 * BLUE wake probes | CYAN CMD0 | YELLOW CMD1 loop | ORANGE CMD2/3/7 |
 * WHITE post-init verify | 0 = never reached command stage */
static uint32_t s_where;
uint32_t emmc_fail_where(void) { return s_where; }

static int hc_wait(uint32_t mask, uint32_t timeout_ms)
{
    uint32_t t0 = timer_ms();
    for (;;) {
        uint32_t st = hc_r32(SDHCI_INT_STATUS);
        if (st == 0xFFFFFFFFu) { s_last_err = st; return -1; } /* unclocked */
        if (st & INT_ERROR) { s_last_err = st; hc_w32(SDHCI_INT_STATUS, st); return -1; }
        if (st & mask)      { hc_w32(SDHCI_INT_STATUS, st & mask); return 0; }
        if ((uint32_t)(timer_ms() - t0) > timeout_ms) { s_last_err = 0; return -1; }
    }
}

/* CORE_VENDOR_SPEC and friends (kernel sdhci_msm_mci_offset, relative to the
 * hc_mem base — see sdhci-msm.c: readl(host->ioaddr + core_vendor_spec)). */
#define CORE_VENDOR_SPEC        0x10Cu
#define CORE_VENDOR_SPEC3       0x1B0u
#define CORE_DLL_CONFIG         0x100u
#define VENDOR_SPEC_POR_VAL     0xA9Cu    /* CORE_VENDOR_SPEC_POR_VAL, verbatim */
#define VS_IO_PAD_PWR_SWITCH_EN (1u << 15)
#define VS_IO_PAD_PWR_SWITCH    (1u << 16)   /* set = 1.8 V pads (REQ_IO_LOW) */
#define CORE_PWRSAVE_DLL        (1u << 3)

#define SDHCI_CLOCK_CTRL   0x2Cu            /* 16-bit */
#define CLK_INT_EN         (1u << 0)
#define CLK_INT_STABLE     (1u << 1)
#define CLK_SD_EN          (1u << 2)

/* THE CARD CLOCK, kernel-shaped (sdhci-msm __sdhci_msm_set_clock +
 * msm_hc_select_default). The msm core does NOT use the standard SDHCI
 * divider — the card clock IS the GCC sdcc1_apps RCG — but the rate must NOT
 * be changed underneath a running clock: the kernel writes SDHCI_CLOCK_CONTROL
 * to ZERO first, re-asserts the free-running-MCLK mux selection, changes the
 * GCC rate, and only then re-enables. (Note the kernel's own comment in
 * msm_hc_select_default: "Make sure above writes impacting free running MCLK
 * are completed before changing the clk_rate at GCC.")
 *
 * We used to call gcc_sdcc1_set_rate() live, mid-session, with SD_CLK_EN set
 * — including the 400 kHz -> 25 MHz switch right before the first data read. */
static void emmc_set_clock(int ident)
{
    /* 1. kill the card clock entirely (CLOCK_CONTROL = 0) */
    hc_w16(0x2Cu, 0u);

    /* 2. free-running MCLK select + no HC_SELECT_IN override, 1.8 V pads.
     *    Re-asserted on every rate change exactly like msm_hc_select_default. */
    uint32_t vs = hc_r32(CORE_VENDOR_SPEC);
    vs &= ~(3u << 8);            /* CORE_HC_MCLK_SEL_MASK  */
    vs |=  (2u << 8);            /* CORE_HC_MCLK_SEL_DFLT  */
    vs &= ~(1u << 18);           /* CORE_HC_SELECT_IN_EN   */
    vs &= ~(7u << 19);           /* CORE_HC_SELECT_IN_MASK */
    vs |= VS_IO_PAD_PWR_SWITCH_EN | VS_IO_PAD_PWR_SWITCH;
    hc_w32(CORE_VENDOR_SPEC, vs);
    __asm__ volatile("dsb sy" ::: "memory");   /* the kernel's wmb() */

    /* 3. now it is safe to move the GCC root */
    gcc_sdcc1_set_rate(ident);

    /* 4. bring the clock back up: internal enable -> wait stable -> SD enable,
     *    divider left at 0 (bypassed) as sdhci-msm requires. */
    hc_w16(0x2Cu, (uint16_t)CLK_INT_EN);
    uint32_t t0 = timer_ms();
    while (!(hc_r32(0x2Cu) & CLK_INT_STABLE)) {
        if ((uint32_t)(timer_ms() - t0) > 20u) break;
    }
    hc_w16(0x2Cu, (uint16_t)(CLK_INT_EN | CLK_SD_EN));
    timer_delay_ms(1);
}

/* Probe: is the controller alive as aboot left it? Never touches the card. */
int emmc_init(void)
{
#if defined(PLAT_BOARD_FOSSIL_GEN6)
    /* GCC first (2026-08-04, THE STORAGE2 fix): aboot gates the SDCC1 core
     * clock at handoff; commands into the dead domain hard-reset the SoC
     * ~seconds later. Same class, same cure as gcc_mdss/gcc_blsp. */
    if (gcc_sdcc1_up() < 0) {
        con_puts("emmc: SDCC1 clocks refused\n");
        s_emmc_ok = 0;
        return -1;
    }
#endif
#if defined(PLAT_BOARD_FOSSIL_GEN6)
    /* STORAGE34 — KERNEL PROBE ORDER. sdhci_msm_probe writes VENDOR_SPEC's
     * POR value and sets HC_MODE_EN + FF_CLK_SW_RST_DIS *before* the core ever
     * issues a RESET_ALL (that happens later, inside sdhci_add_host). We had
     * it backwards: RESET_ALL ran first, i.e. with FF_CLK_SW_RST_DIS still
     * CLEAR, so the reset also took down the free-running MCLK the msm core
     * needs — the exact thing that bit exists to prevent.
     *   1. CORE_VENDOR_SPEC (hc+0x10C) = POR value 0xA9C
     *   2. CORE_HC_MODE (core_mem 0x07824000 + 0x78) = HC_MODE_EN
     *   3. CORE_HC_MODE |= FF_CLK_SW_RST_DIS (bit 13)
     * NB the POR value is 0xA9C, not the 0xA1C we carried since STORAGE17 —
     * bit 7 was missing (CORE_VENDOR_SPEC_POR_VAL in sdhci-msm.c). */
    mmio_write(SDHC_BASE + CORE_VENDOR_SPEC, VENDOR_SPEC_POR_VAL);
    mmio_write(0x07824000u + 0x78u, 0x1u);
    mmio_write(0x07824000u + 0x78u, mmio_read(0x07824000u + 0x78u) | (1u << 13));
    __asm__ volatile("dsb sy" ::: "memory");

    /* STORAGE21 — FULL CONTROLLER RESET (RESET_ALL, 0x2F bit0): wipe every
     * inherited scrap of aboot's configuration (xfer-mode residue, boot
     * access, torn queue state) and rebuild from zero with the complete
     * init below. Now correctly fenced by FF_CLK_SW_RST_DIS above. */
    {
        uint32_t w = hc_r32(0x2Cu);
        hc_w32(0x2Cu, (w & 0x00FFFFFFu) | (0x01u << 24));
        uint32_t t0 = timer_ms();
        while (hc_r32(0x2Cu) & (0x01u << 24)) {
            if ((uint32_t)(timer_ms() - t0) > 50u) break;
        }
        bdiag_puts("emmc: RESET_ALL done\n");
        /* STORAGE22: RESET_ALL also wipes TIMEOUT_CONTROL (byte 0x2E) to 0 =
         * data transfers expire near-instantly (the blue-stair DATA timeout
         * on the first CMD17 after a successful identification). Kernel
         * programs the maximum; so do we. */
        uint32_t tw = hc_r32(0x2Cu);
        hc_w32(0x2Cu, (tw & 0xFF00FFFFu) | (0x0Eu << 16));
    }

    /* Re-assert the vendor configuration AFTER the reset (RESET_ALL is
     * specified over the standard register file, but the msm vendor block
     * sits behind the same reset domain — cheap insurance, and it is what
     * downstream's sdhci_msm_reset_and_restore does):
     * POR value + free-running-MCLK select + 1.8 V pads.
     * PAD_PWR_SWITCH_EN (bit15) / IO_PAD_PWR_SWITCH (bit16) = the kernel's
     * REQ_IO_LOW branch in sdhci_msm_handle_pwr_irq; eMMC VCCQ here is the
     * fixed 1.8 V rail (DT vdd-io-voltage-level = 1800000/1800000). */
    mmio_write(SDHC_BASE + CORE_VENDOR_SPEC,
               VENDOR_SPEC_POR_VAL | VS_IO_PAD_PWR_SWITCH_EN | VS_IO_PAD_PWR_SWITCH);
    /* msm_hc_select_default() also clears CORE_PWRSAVE_DLL in VENDOR_SPEC3
     * for every non-HS400 mode — we never did. Left set, the DLL is allowed
     * to power-collapse under a mode that has no tuned DLL to come back to. */
    {
        uint32_t v3 = mmio_read(SDHC_BASE + CORE_VENDOR_SPEC3);
        mmio_write(SDHC_BASE + CORE_VENDOR_SPEC3, v3 & ~CORE_PWRSAVE_DLL);
    }
    /* STORAGE26 — DATA CRC on every legacy-speed read (readout: bit 21):
     * the CDR (clock-data-recovery) input capture is for TUNED high-speed
     * modes; with the DLL unlocked it garbles reads. Kernel disable recipe
     * for non-tuned modes (sdhci_msm_cdr_enable(false)): CORE_DLL_CONFIG
     * (hc+0x100): clear CDR_EN (bit17), set CDR_EXT_EN (bit19). */
    {
        uint32_t dc = mmio_read(SDHC_BASE + CORE_DLL_CONFIG);
        dc &= ~(1u << 17);
        dc |= (1u << 19);
        mmio_write(SDHC_BASE + CORE_DLL_CONFIG, dc);
    }
    /* STORAGE30 — SDC1 PAD CONTROL (TLMM+0x10A000, pinctrl-sdm429w.c
     * SDC_QDSD_PINGROUP): drive data=10mA(4)@0 cmd=10mA(4)@3 clk=16mA(7)@6,
     * pulls data=up(3)@9 cmd=up(3)@11 clk=none(0)@13 rclk=down(1)@15 — the
     * kernel's ACTIVE state. If the pads sat in a parked/sleep state (weak
     * drive, pulls off), open-drain ident responses corrupt (the CMD2 CRC)
     * and marginal edges eat later responses (the CMD7 silence). */
    {
        uint32_t pad_old = mmio_read(0x0110A000u);
        mmio_write(0x0110A000u,
                   4u | (4u << 3) | (7u << 6) | (3u << 9) | (3u << 11) |
                   (0u << 13) | (1u << 15));
        bdiag_puts("emmc: sdc1 pads "); bdiag_puthex(pad_old);
        bdiag_puts(" -> "); bdiag_puthex(mmio_read(0x0110A000u)); bdiag_puts("\n");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    bdiag_puts("emmc: HC_MODE_EN + VENDOR_SPEC POR + 1.8V pads + CDR off\n");

    /* STORAGE18 — the MSM PWRCTL handshake (kernel sdhci_msm_pwr_irq done
     * by hand). The msm core gates the command engine behind its own power
     * state machine: a write to the standard POWER_CONTROL register raises a
     * REQUEST in CORE_PWRCTL_STATUS (core_mem+0xDC) that software must CLEAR
     * (+0xE4) and ACK with success bits in CORE_PWRCTL_CTL (+0xE8). Nobody
     * ever acked a bus-on for this session -> engine held off -> CMD0
     * silently never completes (the CYAN+magenta verdict). */
    {
        volatile uint32_t *core = (volatile uint32_t *)0x07824000u;
        core[0xE0u / 4u] = 0xFu;                       /* PWRCTL_MASK: all */
        /* Raise BUS_ON + voltage via the standard power register (byte 0x29).
         * STORAGE34: this was 0x0F = SD_BUS_POWER | voltage-select 0b111 =
         * 3.3 V, which makes the core raise REQ_IO_HIGH — the OPPOSITE of the
         * 1.8 V pad switch we then force in VENDOR_SPEC, and the opposite of
         * the fixed 1.8 V vdd-io rail this eMMC actually has. Correct value is
         * voltage-select 0b101 (1.8 V) | power-on = 0x0B, so the pwr_irq
         * request we ack is REQ_IO_LOW and host, core and pads finally agree. */
        uint32_t w = hc_r32(0x28u);
        hc_w32(0x28u, (w & ~0x0000FF00u) | (0x0Bu << 8));
        for (unsigned round = 0; round < 4; round++) {
            uint32_t t0 = timer_ms(), st = 0;
            while ((st = core[0xDCu / 4u] & 0xFu) == 0u) {
                if ((uint32_t)(timer_ms() - t0) > 50u) break;
            }
            if (!st) break;
            core[0xE4u / 4u] = st;                     /* PWRCTL_CLEAR */
            uint32_t ack = 0;
            if (st & 0x3u) ack |= 0x1u;                /* BUS_* -> BUS_SUCCESS */
            if (st & 0xCu) ack |= 0x4u;                /* IO_*  -> IO_SUCCESS  */
            core[0xE8u / 4u] = ack;                    /* PWRCTL_CTL */
            bdiag_puts("emmc: pwrctl req "); bdiag_puthex(st);
            bdiag_puts(" acked\n");
            timer_delay_ms(2);
        }
    }
#endif
    uint32_t present = hc_r32(SDHCI_PRESENT);
    if (present == 0xFFFFFFFFu || present == 0u) {
        bdiag_puts("emmc: controller not alive (present=");
        bdiag_puthex(present); bdiag_puts(")\n");
        s_emmc_ok = 0;
        return -1;
    }
    /* STORAGE1 lesson candidate: aboot may GATE the SD clock after its last
     * read (internal clock/divider retained, SD_CLK_EN dropped) — then every
     * command times out. Re-assert the clock enables; harmless if already on. */
    {
        uint32_t cc = hc_r32(SDHCI_CLOCK_CTRL & ~3u);
        uint16_t clk = (uint16_t)(cc & 0xFFFFu);       /* 0x2C is word-aligned low half */
        if (!(clk & CLK_INT_EN) || !(clk & CLK_SD_EN)) {
            hc_w16(SDHCI_CLOCK_CTRL, (uint16_t)(clk | CLK_INT_EN));
            uint32_t t0 = timer_ms();
            while (!(hc_r32(SDHCI_CLOCK_CTRL & ~3u) & CLK_INT_STABLE)) {
                if ((uint32_t)(timer_ms() - t0) > 20u) break;
            }
            clk = (uint16_t)(hc_r32(SDHCI_CLOCK_CTRL & ~3u) & 0xFFFFu);
            hc_w16(SDHCI_CLOCK_CTRL, (uint16_t)(clk | CLK_INT_EN | CLK_SD_EN));
            bdiag_puts("emmc: SD clock re-enabled\n");
        }
    }
    /* Make sure status bits reach INT_STATUS (polling needs INT_ENABLE set;
     * the separate SIGNAL_ENABLE stays as-is so no IRQ line is raised). */
    hc_w32(SDHCI_INT_ENABLE, 0xFFFFFFFFu);
    hc_w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);             /* clear stale, W1C */
    /* STORAGE12: PROACTIVE CMD+DAT state-machine reset. aboot's shutdown can
     * leave the engines latched busy (CMD/DAT INHIBIT stuck) — then every
     * command times out before it is even issued, regardless of clocks. The
     * kernel resets lines around errors; we start from a clean engine. */
    /* STORAGE14, THE MAGENTA ANSWER (2026-08-06): this controller has a CQE
     * (Command Queue Engine, cmdq_mem @ +0x500 = 0x07824E00 per the dumped
     * DT) and the boot chain drives the eMMC through it. While CQCFG.EN is
     * set the LEGACY command register is IGNORED — commands vanish with no
     * completion and no error, which is precisely the magenta class that
     * survived clocks, resets and card-detect. Standard CQHCI teardown:
     * halt (CQCTL.HALT, wait) then disable (CQCFG.EN=0). */
    {
        volatile uint32_t *cq = (volatile uint32_t *)0x07824E00u;
        uint32_t cfg = cq[0x08u / 4u];
        if (cfg != 0xFFFFFFFFu && (cfg & 1u)) {
            bdiag_puts("emmc: CQE enabled - halting\n");
            cq[0x0Cu / 4u] |= 1u;                     /* CQCTL.HALT */
            uint32_t t0 = timer_ms();
            while (!(cq[0x0Cu / 4u] & 1u)) {
                if ((uint32_t)(timer_ms() - t0) > 20u) break;
            }
            timer_delay_ms(1);
            cq[0x08u / 4u] = cfg & ~1u;               /* CQCFG.EN = 0 */
            __asm__ volatile("dsb sy" ::: "memory");
            bdiag_puts("emmc: CQE disabled (legacy mode)\n");
        }
    }
    hc_reset_lines();
    /* STORAGE13: an SDHCI host does not execute commands while it believes
     * NO CARD is inserted — and eMMC has no card-detect pin. If the handoff
     * left PRESENT.card-inserted (bit 16) clear, every command sits unissued
     * forever with no error: EXACTLY the magenta class. Force card-present
     * via the CD test bits (HOST_CONTROL[7]=CD signal from test reg,
     * HOST_CONTROL[6]=test level "inserted") — the same trick the kernel's
     * broken-card-detection quirk uses. */
    if (!(hc_r32(SDHCI_PRESENT) & (1u << 16))) {
        uint32_t w = hc_r32(0x28u);
        hc_w32(0x28u, w | 0xC0u);          /* HOST_CONTROL low byte bits 6|7 */
        bdiag_puts("emmc: card-present FORCED (CD test bits)\n");
    }
    bdiag_puts("emmc: present2="); bdiag_puthex(hc_r32(SDHCI_PRESENT)); bdiag_puts("\n");
    if (emmc_wake() < 0) { s_emmc_ok = 0; return -1; }
    s_emmc_ok = 1;
    bdiag_puts("emmc: present="); bdiag_puthex(present); bdiag_puts("\n");
    return 0;
}

#define R1_STATE(r)      (((r) >> 9) & 0xFu)   /* 3=stby 4=tran 5=data */

/* No-data command with explicit SDHCI response-type code:
 * 0x00 none | 0x02 R3/R4 (48, no CRC) | 0x09 R2 (136) | 0x1A R1 | 0x1B R1b */
static int emmc_cmd_rt(uint8_t idx, uint32_t arg, uint16_t rt, uint32_t *resp)
{
    /* STORAGE34 — SPEC BUG, and a hard blocker for the whole init ladder.
     * The SDHCI spec (and Linux sdhci_send_command) waits on Command Inhibit
     * (DAT) ONLY for commands that actually use the DAT line:
     *
     *     if (sdhci_data_line_cmd(cmd))        // cmd->data || MMC_RSP_BUSY
     *             mask |= SDHCI_DATA_INHIBIT;
     *
     * We waited on it for EVERY command. DAT_INHIBIT also tracks the card
     * pulling DAT0 low, so a card the boot chain left asserting busy (or torn
     * mid-CQE-transfer, which is exactly how aboot/WearOS can hand it over)
     * pins that bit high forever -> CMD0 itself never issues, returns
     * 0xDEAD0001, and every later "fix" was tested on a bus that never got a
     * single command out. Non-busy, non-data commands must only wait on
     * Command Inhibit (CMD). */
    uint32_t mask = PRESENT_CMD_INHIBIT;
    if (rt == 0x1Bu) mask |= PRESENT_DAT_INHIBIT;      /* R1b uses DAT0 busy */
    uint32_t t0 = timer_ms();
    while (hc_r32(SDHCI_PRESENT) & mask) {
        if ((uint32_t)(timer_ms() - t0) > 100) {
            s_last_err = 0xDEAD0001u;      /* inhibit stuck: engine latched busy */
            return -1;
        }
    }
    hc_w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    hc_w32(SDHCI_ARGUMENT, arg);
    /* STORAGE25: XFER_MODE (0x0C) and COMMAND (0x0E) share one word, and a
     * 16-bit RMW of either re-writes the COMMAND half — WHICH TRIGGERS the
     * stale command. Single atomic 32-bit write, like the kernel's shadow
     * machinery. XFER_MODE = 0 for non-data commands. */
    hc_w32(0x0Cu, (((uint32_t)idx << 8) | rt) << 16);
    if (hc_wait(INT_CMD_COMPLETE, 250) < 0) { hc_reset_lines(); return -1; }
    if (rt == 0x1Bu) {                                  /* R1b: wait busy */
        t0 = timer_ms();
        while (hc_r32(SDHCI_PRESENT) & PRESENT_DAT_INHIBIT) {
            if ((uint32_t)(timer_ms() - t0) > 500) { hc_reset_lines(); return -1; }
        }
    }
    if (resp) *resp = hc_r32(SDHCI_RESPONSE0);
    return 0;
}

static int emmc_cmd(uint8_t idx, uint32_t arg, int busy, uint32_t *resp)
{
    return emmc_cmd_rt(idx, arg, busy ? 0x1Bu : 0x1Au, resp);
}

/* FULL RE-IDENTIFICATION (2026-08-04, STORAGE9 verdict: not even CMD5/CMD13
 * answers — aboot doesn't just sleep the card, it leaves it needing a cold
 * init). Standard eMMC bring-up, LK/kernel-shaped:
 *   clock -> identification rate (<=400 kHz)
 *   CMD0 GO_IDLE
 *   CMD1 loop (OCR 0x40FF8080: sector-mode + voltage window) until ready
 *   CMD2 ALL_SEND_CID (R2)
 *   CMD3 SET_RELATIVE_ADDR rca=1 (host assigns, R1)
 *   CMD7 SELECT (R1b) -> transfer state
 *   clock -> a conservative transfer rate
 * Leaves the bus 1-bit backward-compat mode: slow but universally correct;
 * width/speed upgrades are a later optimization. */
static int emmc_full_init(void)
{
    uint32_t r = 0;

    /* STORAGE11: rate changes go through the GCC RCG — the msm core ignores
     * the standard SDHCI divider (STORAGE10's magenta: dead card clock). The
     * standard POWER register is also deliberately NOT touched: on msm a
     * power-reg write raises an internal PWR_IRQ the hardware waits on. */
    emmc_set_clock(1);                        /* 400 kHz identification */
    timer_delay_ms(2);

#if defined(PLAT_BOARD_FOSSIL_GEN6)
    /* STORAGE19 — CARD POWER. CMD0 completes but the card never answers
     * CMD1: the engine works, the CARD is electrically absent. Its VCC is
     * PM660 L19 (DT vdd-supply phandle 0x96 -> rpm-regulator-ldoa19; LDO
     * qpnp base 0x4000+18*0x100 = 0x5200, sid 1 like L13). Read EN_CTL
     * (0x5246 bit7); if the handoff left it OFF, attempt the enable —
     * first-ever RPM-owned-regulator write on this port (calculated risk:
     * voltage config untouched, only the enable bit; haptics writes on the
     * same sid work). */
    {
        uint8_t en = 0;
        if (spmi_read8(1, 0x5246u, &en) == 0) {
            bdiag_puts("emmc: L19 EN_CTL="); bdiag_puthex(en); bdiag_puts("\n");
            if (!(en & 0x80u)) {
                spmi_write8(1, 0x5246u, (uint8_t)(en | 0x80u));
                timer_delay_ms(10);           /* LDO soft-start */
                bdiag_puts("emmc: L19 enable attempted\n");
            }
        }
    }
#endif

    /* STORAGE21: NO L19 power-cycle EVER — the display shares that rail
     * (STORAGE20 went black). Two attempts differ only in the reset spell:
     * attempt 2 prepends GO_PRE_IDLE (CMD0 arg 0xF0F0F0F0), the JEDEC exit
     * from boot-access states that a plain GO_IDLE does not clear. */
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        s_where = 0x00FFFFu;                            /* CYAN: CMD0 */
        if (attempt == 1) {
            bdiag_puts("emmc: retry with GO_PRE_IDLE\n");
            emmc_cmd_rt(0u, 0xF0F0F0F0u, 0x00u, 0);
            timer_delay_ms(2);
        }
        if (emmc_cmd_rt(0u, 0u, 0x00u, 0) < 0) return -1;   /* GO_IDLE */
        timer_delay_ms(2);

        s_where = 0xFFFF00u;                            /* YELLOW: CMD1 loop */
        uint32_t t0 = timer_ms();
        int ready = 0;
        for (;;) {                                      /* CMD1 until ready */
            if (emmc_cmd_rt(1u, 0x40FF8080u, 0x02u, &r) < 0) break;
            if (r & 0x80000000u) { ready = 1; break; }
            if ((uint32_t)(timer_ms() - t0) > 1500u) break;
            timer_delay_ms(10);
        }
        if (ready) break;
        if (attempt == 1) { bdiag_puts("emmc: CMD1 never ready, last r="); bdiag_puthex(r); bdiag_puts("\n"); return -1; }
    }
    bdiag_puts("emmc: OCR="); bdiag_puthex(r); bdiag_puts("\n");
    s_where = 0xFF8000u;                                /* ORANGE: CMD2/3/7 */
    /* CMD2/CMD3 responses arrive in the ident phase's OPEN-DRAIN mode and
     * are the first CRC-checked responses — tolerate them unchecked (0x01 =
     * 136-bit no-CRC, 0x02 = 48-bit no-CRC); the push-pull CMD13 verify
     * below is fully checked. */
    if (emmc_cmd_rt(2u, 0u, 0x01u, 0) < 0) {
        con_puts("emmc: CMD2 fail e="); con_puthex(s_last_err); con_puts("\n");
        return -1;
    }
    bdiag_puts("emmc: CID0="); bdiag_puthex(hc_r32(SDHCI_RESPONSE0));
    bdiag_puts(" CID1="); bdiag_puthex(hc_r32(SDHCI_RESPONSE0 + 4u)); bdiag_puts("\n");
    s_rca = 1u;
    if (emmc_cmd_rt(3u, s_rca << 16, 0x02u, &r) < 0) {
        con_puts("emmc: CMD3 fail e="); con_puthex(s_last_err); con_puts("\n");
        return -1;
    }
    bdiag_puts("emmc: CMD3 r="); bdiag_puthex(r); bdiag_puts("\n");
    if (emmc_cmd_rt(7u, s_rca << 16, 0x1Bu, 0) < 0) {
        con_puts("emmc: CMD7 fail e="); con_puthex(s_last_err);
        bdiag_puts(" present="); bdiag_puthex(hc_r32(SDHCI_PRESENT)); bdiag_puts("\n");
        return -1;
    }
    bdiag_puts("emmc: CMD7 ok\n");

    emmc_set_clock(0);                        /* 25 MHz transfer */
    timer_delay_ms(1);

    s_where = 0xFFFFFFu;                                /* WHITE: verify */
    if (emmc_cmd_rt(13u, s_rca << 16, 0x1Au, &r) < 0) {
        con_puts("emmc: CMD13 verify fail e="); con_puthex(s_last_err); con_puts("\n");
        return -1;
    }
    bdiag_puts("emmc: CMD13 r="); bdiag_puthex(r); bdiag_puts("\n");
    if (R1_STATE(r) != 4u) return -1;
    bdiag_puts("emmc: FULL re-init OK (tran, rca 1)\n");
    return 0;
}

/* WAKE THE CARD (2026-08-04, THE STORAGE8-STAIRS verdict: first CMD17 fails
 * CLEANLY on a healthy clocked controller = the CARD refuses). LK bootloaders
 * end with mmc_put_card_to_sleep(): CMD7(deselect) + CMD5(sleep) — a sleeping
 * card ignores everything except the wake sequence. Standard wake:
 *   CMD5 arg RCA<<16 bit15=0  (SLEEP_AWAKE -> awake, R1b)
 *   CMD7 arg RCA<<16          (SELECT back into transfer state, R1b)
 *   CMD13 SEND_STATUS         (verify state = tran(4))
 * RCA is whatever aboot assigned (LK uses small integers) — probe 1..4. */
#define MMC_SLEEP_AWAKE   5u
#define MMC_SELECT_CARD   7u
#define MMC_SEND_STATUS  13u

static int emmc_wake(void)
{
    /* STORAGE28 — NO WAKE SHORTCUT. The CMD13 probe was "succeeding": the
     * card is still initialized from ABOOT — at 8-BIT bus width and HS
     * timing. Adopting it while the controller reads 1-bit garbles every
     * data block at ANY clock (the un-killable bit-21 CRC). Always cold-init:
     * GO_IDLE drops the card to 1-bit backward-compat, matching the
     * controller. */
    return emmc_full_init();

    s_where = 0x0000FFu;                    /* BLUE: wake probes */
    for (uint32_t rca = 1; rca <= 4; rca++) {
        uint32_t st;
        if (emmc_cmd(MMC_SEND_STATUS, rca << 16, 0, &st) == 0) {
            s_rca = rca;
            if (R1_STATE(st) == 4u) { bdiag_puts("emmc: card awake (tran)\n"); return 0; }
            if (R1_STATE(st) == 3u) {                   /* standby: select */
                if (emmc_cmd(MMC_SELECT_CARD, rca << 16, 1, 0) == 0 &&
                    emmc_cmd(MMC_SEND_STATUS, rca << 16, 0, &st) == 0 &&
                    R1_STATE(st) == 4u) {
                    bdiag_puts("emmc: card selected -> tran\n");
                    return 0;
                }
            }
        } else {
            /* no response to status: asleep? blind wake, then select */
            if (emmc_cmd(MMC_SLEEP_AWAKE, rca << 16, 1, 0) == 0 &&
                emmc_cmd(MMC_SELECT_CARD, rca << 16, 1, 0) == 0 &&
                emmc_cmd(MMC_SEND_STATUS, rca << 16, 0, &st) == 0 &&
                R1_STATE(st) == 4u) {
                s_rca = rca;
                bdiag_puts("emmc: card WOKEN (rca "); bdiag_putdec(rca);
                bdiag_puts(")\n");
                return 0;
            }
        }
    }
    con_puts("emmc: no card responded to wake - full re-init\n");
    /* STORAGE10 escalation: aboot left the card beyond any wake shortcut —
     * cold-init it like a first power-up. */
    return emmc_full_init();
}

/* Read one 512-byte block at `lba` (sector addressing — every eMMC big enough
 * to be in a watch is high-capacity). 0 ok, -1 error. */
int emmc_read_block(uint32_t lba, void *dst)
{
    uint8_t *p = (uint8_t *)dst;
    uint32_t t0;

    if (!s_emmc_ok) return -1;

    /* Wait for a quiet bus (aboot's last op long done, but be correct). */
    t0 = timer_ms();
    while (hc_r32(SDHCI_PRESENT) & (PRESENT_CMD_INHIBIT | PRESENT_DAT_INHIBIT)) {
        if ((uint32_t)(timer_ms() - t0) > 100) return -1;
    }

    hc_w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    hc_w16(SDHCI_BLOCK_SIZE, EMMC_BLOCK);
    hc_w16(SDHCI_BLOCK_COUNT, 1);
    hc_w32(SDHCI_ARGUMENT, lba);
    /* atomic XFER_MODE+COMMAND (see STORAGE25 note in emmc_cmd_rt) */
    hc_w32(0x0Cu, ((uint32_t)CMD_R1_DATA(MMC_READ_SINGLE) << 16) | XFER_MODE_READ);

    if (hc_wait(INT_CMD_COMPLETE, 100) < 0) { hc_reset_lines(); return -1; }
    if (hc_wait(INT_BUF_RD_READY, 250) < 0) {
        hc_reset_lines();
        /* STORAGE27: data CRC at 25 MHz -> drop to the ident clock for the
         * rest of the session and retry once. Slow storage beats none, and
         * the stairs verdict tells us whether rate was the whole story. */
        if (!s_slow_mode && (s_last_err & ((1u << 21) | (1u << 17)))) {
            s_slow_mode = 1;
            bdiag_puts("emmc: data CRC - dropping to 400 kHz\n");
            emmc_set_clock(1);
            timer_delay_ms(1);
            return emmc_read_block(lba, dst);
        }
        return -1;
    }

    for (unsigned i = 0; i < EMMC_BLOCK; i += 4) {
        uint32_t w = hc_r32(SDHCI_BUFFER);
        p[i+0] = (uint8_t)w; p[i+1] = (uint8_t)(w >> 8);
        p[i+2] = (uint8_t)(w >> 16); p[i+3] = (uint8_t)(w >> 24);
    }
    return hc_wait(INT_XFER_COMPLETE, 100);
}

int emmc_read(uint32_t lba, uint32_t nblocks, void *dst)
{
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < nblocks; i++) {
        if (emmc_read_block(lba + i, p + (uint32_t)i * EMMC_BLOCK) < 0)
            return -1;
    }
    return 0;
}

/* ---- WRITE path (2026-08-04, the blackbox pivot) --------------------------
 * CMD24 single-block PIO write — the mirror of emmc_read_block. GUARDED by a
 * write WINDOW: every write outside [s_win_lba, s_win_lba+s_win_n) is refused
 * at this lowest level, and the window can only be armed ONCE per boot (a
 * stray second arming attempt disarms writes permanently). The window is
 * armed from the GPT lookup of the designated log/data partition — writes to
 * boot chain partitions are therefore structurally impossible, not merely
 * avoided. */
static uint32_t s_win_lba, s_win_n, s_win_state;   /* 0=unarmed 1=armed 2=locked-out */

int emmc_write_window(uint32_t lba, uint32_t nblocks)
{
    if (s_win_state != 0u || nblocks == 0u) {
        s_win_state = 2u;                 /* second arming: lock out writes */
        bdiag_puts("emmc: write window LOCKED OUT\n");
        return -1;
    }
    s_win_lba = lba; s_win_n = nblocks; s_win_state = 1u;
    bdiag_puts("emmc: write window armed @"); bdiag_puthex(lba);
    bdiag_puts(" +"); bdiag_putdec(nblocks); bdiag_puts("\n");
    return 0;
}

int emmc_write_block(uint32_t lba, const void *src)
{
    const uint8_t *p = (const uint8_t *)src;
    uint32_t t0;

#if defined(PLAT_STORAGE_NOWRITE)
    /* STORAGE4 bisect: STORAGE2 (first build with LIVE eMMC after the clock
     * fix) died ~4 s in. This flag keeps every read path active but fails
     * all writes here, before any hardware is touched: boot surviving =
     * the write path is the killer; boot still dying = reads/clock. */
    (void)p; (void)t0; (void)lba;
    return -1;
#else
    if (s_win_state != 1u) return -1;
    if (lba < s_win_lba || lba >= s_win_lba + s_win_n) return -1;

    t0 = timer_ms();
    while (hc_r32(SDHCI_PRESENT) & (PRESENT_CMD_INHIBIT | PRESENT_DAT_INHIBIT)) {
        if ((uint32_t)(timer_ms() - t0) > 100) return -1;
    }

    hc_w32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    hc_w16(SDHCI_BLOCK_SIZE, EMMC_BLOCK);
    hc_w16(SDHCI_BLOCK_COUNT, 1);
    hc_w32(SDHCI_ARGUMENT, lba);
    /* atomic XFER_MODE+COMMAND, direction host->card (XFER low half 0) */
    hc_w32(0x0Cu, (uint32_t)CMD_R1_DATA(MMC_WRITE_SINGLE) << 16);

    if (hc_wait(INT_CMD_COMPLETE, 100) < 0) { hc_reset_lines(); return -1; }
    if (hc_wait(INT_BUF_WR_READY, 250) < 0) {
        hc_reset_lines();
        if (!s_slow_mode && (s_last_err & ((1u << 21) | (1u << 17)))) {
            s_slow_mode = 1;
            bdiag_puts("emmc: write CRC - dropping to 400 kHz\n");
            emmc_set_clock(1);
            timer_delay_ms(1);
            return emmc_write_block(lba, src);
        }
        return -1;
    }

    for (unsigned i = 0; i < EMMC_BLOCK; i += 4) {
        uint32_t w = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8)
                   | ((uint32_t)p[i+2] << 16) | ((uint32_t)p[i+3] << 24);
        hc_w32(SDHCI_BUFFER, w);
    }
    /* card programs the block after the transfer: allow a generous wait */
    return hc_wait(INT_XFER_COMPLETE, 500);
#endif /* PLAT_STORAGE_NOWRITE */
}

int emmc_write(uint32_t lba, uint32_t nblocks, const void *src)
{
    const uint8_t *p = (const uint8_t *)src;
    for (uint32_t i = 0; i < nblocks; i++) {
        if (emmc_write_block(lba + i, p + (uint32_t)i * EMMC_BLOCK) < 0)
            return -1;
    }
    return 0;
}

/* ---- GPT ------------------------------------------------------------------
 * LBA1 = header ("EFI PART"), entries usually from LBA2: 128-byte entries,
 * partition name at +0x38 as UTF-16LE. Android boot devices put boot /
 * userdata / misc... here — the map we need before ANY future write support. */
int emmc_gpt_find(const char *name, uint32_t *out_lba, uint32_t *out_nblk)
{
    uint8_t blk[EMMC_BLOCK];
    uint32_t entry_lba, nentries, esize;

    if (emmc_read_block(1, blk) < 0) return -1;
    if (memcmp(blk, "EFI PART", 8) != 0) return -1;

    entry_lba = (uint32_t)blk[0x48] | ((uint32_t)blk[0x49] << 8)
              | ((uint32_t)blk[0x4A] << 16) | ((uint32_t)blk[0x4B] << 24);
    nentries  = (uint32_t)blk[0x50] | ((uint32_t)blk[0x51] << 8)
              | ((uint32_t)blk[0x52] << 16) | ((uint32_t)blk[0x53] << 24);
    esize     = (uint32_t)blk[0x54] | ((uint32_t)blk[0x55] << 8)
              | ((uint32_t)blk[0x56] << 16) | ((uint32_t)blk[0x57] << 24);
    if (esize < 128 || esize > EMMC_BLOCK || nentries > 256) return -1;

    for (uint32_t i = 0; i < nentries; i++) {
        uint32_t lba = entry_lba + (i * esize) / EMMC_BLOCK;
        uint32_t off = (i * esize) % EMMC_BLOCK;
        static uint32_t cached_lba = 0; /* avoid re-reading the same sector */
        static uint8_t  eblk[EMMC_BLOCK];
        if (cached_lba != lba) {
            if (emmc_read_block(lba, eblk) < 0) return -1;
            cached_lba = lba;
        }
        const uint8_t *e = eblk + off;
        /* zero type-GUID = unused entry */
        int used = 0;
        for (int j = 0; j < 16; j++) used |= e[j];
        if (!used) continue;
        uint32_t first = (uint32_t)e[0x20] | ((uint32_t)e[0x21] << 8)
                       | ((uint32_t)e[0x22] << 16) | ((uint32_t)e[0x23] << 24);
        uint32_t last  = (uint32_t)e[0x28] | ((uint32_t)e[0x29] << 8)
                       | ((uint32_t)e[0x2A] << 16) | ((uint32_t)e[0x2B] << 24);

        /* UTF-16LE name -> ASCII compare, CASE-INSENSITIVE (STORAGE2: some
         * vendor GPTs carry "USERDATA"/mixed case) */
        const uint8_t *n16 = e + 0x38;
        unsigned j;
        for (j = 0; j < 36 && name[j]; j++) {
            uint8_t a = n16[j*2], b = (uint8_t)name[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b || n16[j*2+1] != 0) break;
        }
        if (name[j] == 0 && (j == 36 || (n16[j*2] == 0 && n16[j*2+1] == 0))) {
            if (out_lba)  *out_lba = first;
            if (out_nblk) *out_nblk = last - first + 1;
            return 0;
        }
    }
    return -1;
}

/* Fallback when the name lookup fails: the LARGEST partition. On every
 * Android GPT the multi-GiB tail partition is userdata — unambiguous on this
 * watch (4.65 GiB vs 1.5 GiB system in second place). */
int emmc_gpt_find_largest(uint32_t *out_lba, uint32_t *out_nblk)
{
    uint8_t blk[EMMC_BLOCK];
    uint32_t entry_lba, nentries, esize, best = 0, best_lba = 0, best_n = 0;

    if (emmc_read_block(1, blk) < 0) return -1;
    if (memcmp(blk, "EFI PART", 8) != 0) return -1;
    entry_lba = (uint32_t)blk[0x48] | ((uint32_t)blk[0x49] << 8)
              | ((uint32_t)blk[0x4A] << 16) | ((uint32_t)blk[0x4B] << 24);
    nentries  = (uint32_t)blk[0x50] | ((uint32_t)blk[0x51] << 8)
              | ((uint32_t)blk[0x52] << 16) | ((uint32_t)blk[0x53] << 24);
    esize     = (uint32_t)blk[0x54] | ((uint32_t)blk[0x55] << 8)
              | ((uint32_t)blk[0x56] << 16) | ((uint32_t)blk[0x57] << 24);
    if (esize < 128 || esize > EMMC_BLOCK || nentries > 256) return -1;

    uint32_t cached = 0xFFFFFFFFu;
    uint8_t eblk[EMMC_BLOCK];
    for (uint32_t i = 0; i < nentries; i++) {
        uint32_t lba = entry_lba + (i * esize) / EMMC_BLOCK;
        uint32_t off = (i * esize) % EMMC_BLOCK;
        if (cached != lba) {
            if (emmc_read_block(lba, eblk) < 0) return -1;
            cached = lba;
        }
        const uint8_t *e = eblk + off;
        int used = 0;
        for (int j2 = 0; j2 < 16; j2++) used |= e[j2];
        if (!used) continue;
        uint32_t first = (uint32_t)e[0x20] | ((uint32_t)e[0x21] << 8)
                       | ((uint32_t)e[0x22] << 16) | ((uint32_t)e[0x23] << 24);
        uint32_t last  = (uint32_t)e[0x28] | ((uint32_t)e[0x29] << 8)
                       | ((uint32_t)e[0x2A] << 16) | ((uint32_t)e[0x2B] << 24);
        uint32_t n = last - first + 1;
        if (n > best) { best = n; best_lba = first; best_n = n; }
    }
    if (!best) return -1;
    if (out_lba)  *out_lba = best_lba;
    if (out_nblk) *out_nblk = best_n;
    return 0;
}

#endif /* PLAT_SOC_MSM */
