/* platform.h — selects the board header and declares the tiny platform API
 * every fossil-port target implements. Build sets exactly one PLAT_BOARD_*. */
#pragma once
#include <stdint.h>

#if defined(PLAT_BOARD_QEMU_VIRT)
#include "../boards/qemu_virt.h"
#elif defined(PLAT_BOARD_FOSSIL_GEN4)
#include "../boards/fossil_gen4.h"
#elif defined(PLAT_BOARD_FOSSIL_GEN6)
#include "../boards/fossil_gen6.h"
#elif defined(PLAT_BOARD_TICWATCH_C2)
#include "../boards/ticwatch_c2.h"
#else
#error "platform.h: define PLAT_BOARD_QEMU_VIRT, PLAT_BOARD_FOSSIL_GEN4, \
PLAT_BOARD_FOSSIL_GEN6 or PLAT_BOARD_TICWATCH_C2"
#endif

/* Both Fossil watches share the MSM driver model (SPMI, UARTDM, GIC-400,
 * msm reboot registers). Code that is msm-generic guards on this; code that is
 * specific to one watch's peripherals still guards on the exact board. */

/* ---- middle tier: the Snapdragon Wear 2100 (msm8909w / APQ8009W) ----------
 * The Fossil Gen 4 and the Mobvoi TicWatch C2 are the SAME SoC from different
 * vendors, so a large majority of the Gen 4's bring-up is not Fossil work at
 * all — it is msm8909w work that happened to be written for a Fossil. The A7
 * clock RCG, the CPR/SMPS voltage path, MDP3, the 28nm DSI host, and the MMU
 * map are identical silicon on both watches.
 *
 * Guarding those on PLAT_BOARD_FOSSIL_GEN4 was correct while the Gen 4 was the
 * only 8909 in the tree, and became wrong the moment a second one arrived: the
 * alternative is `|| defined(PLAT_BOARD_TICWATCH_C2)` appended to a dozen files
 * and again for every watch after it. So there are three tiers now, and the
 * rule for choosing between them is what a driver actually depends on:
 *
 *   PLAT_SOC_MSM       — the MSM driver model (SPMI, GIC-400, msm reboot).
 *                        Everything from the Gen 4 to the Gen 6's ARM64 A53.
 *   PLAT_SOC_MSM8909   — this SoC's registers. Same addresses, same bitfields,
 *                        same fuse rows, regardless of whose watch it is.
 *   PLAT_BOARD_*       — this WATCH's parts: panel model and its init sequence,
 *                        touch controller, DDR carve-outs, pin assignments.
 *
 * Panel and touch stay at board tier deliberately. The Gen 4 is an AUO h139
 * with a Raydium controller; the C2 is a different panel and an ITE IT7260, on
 * the same bus at a different address. Sharing the bus is not sharing a driver. */
/* The tiers themselves are derived in plat_soc_tier.h — a dependency-free
 * header, so the compat layer can see them too. See the note there. */
#include "plat_soc_tier.h"

static inline void mmio_write(uintptr_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }
static inline uint32_t mmio_read(uintptr_t addr) { return *(volatile uint32_t *)addr; }

/* uart_<type>.c */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint32_t v);
void uart_putdec(uint32_t v);

/* ramlog.c — post-mortem ring buffer in .ramlog (survives warm reboot) */
void ramlog_init(void);          /* keeps previous contents if magic is valid */
void ramlog_putc(char c);
int  ramlog_had_previous(void);  /* nonzero if a prior boot's log was found */
uint32_t ramlog_read(uint32_t *cursor, char *out, uint32_t max); /* stream out */

/* usb_ci.c / usb_phy_msm.c / gcc_usb.c — CDC-ACM log console over USB */
int  gcc_usb_hs_up(void);
int  usb_ulpi_write(uint8_t val, uint8_t reg);
int  usb_ulpi_read(uint8_t reg, uint8_t *out);
int  usb_phy_init_seq(void);
void usb_phy_qusb_por(void);
void gcc_usb_qusb2_phy_reset(void);
void usb_diag(uint32_t *portsc, uint32_t *usbsts, uint32_t *vid, int *cfg);
int  usb_phy_force_vbus_valid(void);
int  usb_dev_init(void);
void usb_poll(void);
int  usb_is_configured(void);  /* cheap flag read; no ULPI traffic */

/* timer.c — ARM architected timer (polled; IRQ mode arrives with FreeRTOS) */
uint32_t timer_freq_hz(void);
uint64_t timer_ticks(void);
uint32_t timer_us32(void);   /* free-running us, wraps ~71 min; use as deltas */
uint32_t timer_ms(void);
void     timer_delay_ms(uint32_t ms);

/* mmu.c — identity map + caches (MUST run before LVGL/newlib: unaligned
 * accesses fault while the MMU is off) */
void mmu_enable_flat(void);

/* gic.c — GICv2 */
void gic_init(void);
void gic_enable_irq(unsigned id, uint8_t priority);
void gic_disable_irq(unsigned id);

/* irq.c — dispatch table (tick PPI 27 is claimed internally) */
void irq_register(unsigned id, void (*fn)(void *), void *arg);

/* fb_*.c — framebuffer (ramfb on QEMU; MDP3/DSI on the watch later).
 * Returns an XRGB8888 buffer of w*h pixels, or NULL. */
void *fb_init(uint32_t w, uint32_t h);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_bpp(void);
void    *fb_ptr(void);   /* raw framebuffer base (gfx_text.c) */       /* bytes per pixel the source pipe is set to */
void     fb_flush_all(void); /* cache clean + MDP kickoff (command-mode panel) */
void     fb_flush_region(const void *addr, uint32_t len); /* cache clean only */
void     fb_idle_refresh(uint32_t max_age_ms); /* re-kick if panel unfed (drift) */
void     fb_perf_loop_tick(uint32_t body_ms); /* PERF_BARS: loop-rate sample */
void     fb_dbg_mark(uint32_t idx, uint32_t xrgb); /* 3 big center trace blocks */
void     fb_dbg_byte(uint32_t row, uint32_t val);  /* 8 bit-blocks, center band */
void     fb_text_dump(const char *s); /* 8x8 font text straight into the fb */
void     fb_trace(uint32_t xrgb); /* VISUAL_TRACE: paint+kick a milestone color */
int      gcc_blsp_qup4_up(void);  /* gen6: enable touch-I2C QUP clocks first */
void     tlmm_cfg(uint32_t pin, uint32_t func, uint32_t pull, uint32_t drv_ma,
                  int output);    /* gen6 TLMM pin mux (tlmm.c) */
void     tlmm_out(uint32_t pin, int hi);
int      tlmm_in(uint32_t pin);   /* GPIO_IN_OUT bit0 — live pin level */
void     tlmm_touch_setup(void);  /* mux i2c4 pins; reset line held high */
void     tlmm_touch_reset_pulse(void); /* fallback hard reset pulse */
/* DCS command layer. The msm8909w watches and the Gen 6 have DIFFERENT DSI
 * hosts and therefore different command APIs: the Gen 6 (dsi_dcs.c) takes
 * cmd + one param, msm8909w (msm_dsi.c) takes cmd + a parameter BUFFER,
 * because the AUO h139's on-command table contains 4-byte long writes
 * (2A/2B/30/31). Do not merge them. */
#if defined(PLAT_SOC_MSM8909)
int      dsi_dcs_write(uint8_t cmd, const uint8_t *params, uint32_t len);
#else
int      dsi_dcs_write(uint8_t cmd, uint8_t param, int has_param);
#endif
int      dsi_dcs_lwrite(const uint8_t *payload, uint32_t len);
int      dsi_dcs_repin_window(void);      /* restore CASET/PASET (drift fix) */
int      dsi_dcs_repin_full(void);        /* + partial area/mode (error path) */
int      dsi_dcs_set_brightness(uint8_t level);  /* panel DCS 0x51 */

/* --- msm8909w display: MDP3 DMA_P + the aboot splash takeover -------------
 * This SoC's display block is MDP3, not the Gen 6's MDP5: there is no
 * SSPP/mixer/CTL topology, just one DMA engine (DMA_P) that pushes a frame
 * out of DDR to a command-mode DSI panel when DMA_P_START is written. Shared
 * by the Fossil Gen 4 and the TicWatch C2 — same silicon, same registers. */
#if defined(PLAT_SOC_MSM8909)
struct mdp3_splash_cfg {
    uint32_t config;    /* DMA_P_CONFIG as aboot left it        */
    uint32_t addr;      /* DMA_P_IBUF_ADDR (aboot's own buffer) */
    uint32_t w, h;      /* DMA_P_SIZE                           */
    uint32_t stride;    /* DMA_P_IBUF_Y_STRIDE, bytes           */
    uint32_t format;    /* CONFIG[26:25] 0=RGB888 1=RGB565 2=XRGB8888 */
    uint32_t out_sel;   /* CONFIG[20:19] 1 = DSI command mode   */
};
int      mdp3_splash_probe(struct mdp3_splash_cfg *out);
void     mdp3_dma_config(const void *buf, uint32_t w, uint32_t h);
void     mdp3_dma_repoint(const void *buf);
uint32_t mdp3_dma_stride(void);
int      mdp3_flush(const void *buf);
void     dsi_init(void);
int      panel_on(void);
int      panel_off(void);
int      panel_full_init(void);   /* dsi_init() + panel_on(): blind bring-up */
#endif

uint32_t fb_last_kick_err(void); /* 0 ok, 1 pingpong timeout, 2 DSI timeout */
uint32_t fb_error_classes(void); /* accumulated DSI errors: b0 ack b1 to b2 phy b3 fifo */

/* gcc_mdss.c — Gen 6 MDSS power domain + clocks. MUST be up before ANY
 * MDP/DSI register access (unclocked MSM blocks hang the AHB bus). */
#define GCC_MDSS_ST_GDSC_WAS_ON  (1u << 0)  /* aboot left the domain powered  */
#define GCC_MDSS_ST_GDSC_ON      (1u << 1)  /* domain is powered now          */
#define GCC_MDSS_ST_CORE_CLKS    (1u << 2)  /* ahb/axi/mdp/vsync all ticking  */
#define GCC_MDSS_ST_DSI_CLKS     (1u << 3)  /* esc0/byte0/pclk0 all ticking
                                             * (clear => the 12nm DSI PLL is
                                             * dead and needs a full bring-up) */
#define GCC_MDSS_ST_ESC0_CLK     (1u << 4)  /* esc0 alone (XO-sourced): clear
                                             * means a GCC-side fault, not PLL */
#define GCC_MDSS_ST_LINK_CLKS    (1u << 5)  /* byte0+pclk0 (DSI-PLL-sourced)  */
uint32_t gcc_mdss_up(void);
uint32_t gcc_mdss_status(void);
uint32_t gcc_mdss_dsi_clks_retry(void);  /* after the DSI PLL is relocked */

/* dsi_pll_12nm.c — re-lock the bootloader-configured DSI PLL (Gen 6). */
#define DSI_PLL_ST_LK_PROGRAMMED (1u << 0)  /* SYS_CTRL bit7: LK left PHY set up */
#define DSI_PLL_ST_LOCKED        (1u << 1)  /* STAT0 reports lock               */
uint32_t dsi_pll_12nm_relock(void);
uint32_t dsi_pll_12nm_program(void);  /* full from-scratch config + enable */
int dsi_pll_12nm_status_locked(void);
void dsi_phy_12nm_config(void);      /* full PHY replay (dumped panel timings) */
void dsi_host_12nm_reenable(void);   /* re-assert DSI ctrl enable/clock gates */
void dsi_host_12nm_trigger_setup(void); /* TRIG_CTRL: TE_SEL + sw dma trigger */
void gcc_mdss_set_mdp_cfg(uint32_t cfg); /* retune mdp_clk_src (arm sweep) */

/* DSI arm-sweep knobs (fb_splash.c owns them; dsi_dcs.c reads them). */
extern volatile uint32_t g_dsi_no_repin;   /* 1 = skip the per-frame re-pin  */
extern volatile uint32_t g_dsi_dcs_drain;  /* 1 = wait for the lane to idle  */
/* HS_TX timeout the host setup installs. A global because the recovery path
 * re-runs dsi_host_12nm_trigger_setup() constantly, which silently overwrote
 * (and voided) the round-2 sweep's attempts to vary this register. */
extern volatile uint32_t g_dsi_hstmr;
extern volatile uint32_t g_dsi_te_on;       /* panel TE (DCS 0x35) requested */
extern volatile uint32_t g_dsi_drain_timeouts; /* DCS drain hit its 10 ms cap */
extern volatile uint32_t g_dsi_repin_full_n;/* repin_full every N kicks, 0=off */
extern volatile uint32_t g_dsi_partial_on;  /* send DCS 0x12 in repin_full    */
int dsi_dcs_set_tear(int on);               /* DCS 0x35 / 0x34               */
void dsi_host_12nm_sw_reset(void);   /* controller reset (init + error recovery) */

/* reboot_msm.c — SoC reset + reboot-to-bootloader + button-less watchdog.
 * Fossil target only (msm8909 registers); the watchdog is the sole guaranteed
 * recovery path on a watch with no button force-reset. See reboot_msm.c. */
#if defined(PLAT_SOC_MSM)
void reboot_now(void);            /* warm reset -> stock boot chain */
void reboot_to_bootloader(void);  /* warm reset -> aboot fastboot */
void reboot_to_recovery(void);    /* warm reset -> recovery */
void deadman_arm(uint32_t timeout_ms);  /* auto-reboot-to-fastboot if un-kicked */
void deadman_kick(void);          /* prove liveness; restart the countdown */
void deadman_disarm(void);        /* stop the watchdog once a build is trusted */

/* msm_i2c.c — BLSP QUP v2 I2C master (polled, FIFO mode, bounded timeouts).
 * Explicit-base API serves any of the SoC's QUPs; returns 0 ok, -1 error,
 * -2 NACK. Boards with a known touch bus also get the legacy i2c_* wrappers
 * (see msm_i2c.c). */
int i2c_bus_init(uintptr_t base);
int i2c_bus_xfer(uintptr_t base, uint8_t addr, const uint8_t *wbuf, uint32_t wlen,
                 uint8_t *rbuf, uint32_t rlen);
/* Pure-write completion counters (see the WRITE DRAIN note in msm_i2c.c):
 * total writes, and how many reached the old code's reset point with the
 * output not yet done / the wire still active — i.e. would have been cut. */
extern uint32_t g_i2c_wr_total, g_i2c_wr_early, g_i2c_wr_busy;

/* spmi_arb.c — SPMI PMIC-arbiter master (polled, bounded). Shared by every
 * PMIC peripheral driver (vibrator/haptics, RTC, PON...). len is 1..8 bytes.
 * All return 0 on ack, -1 on error/timeout — never hang. */
int spmi_write(uint8_t sid, uint16_t addr, const uint8_t *buf, unsigned len);
int spmi_read(uint8_t sid, uint16_t addr, uint8_t *buf, unsigned len);
int spmi_write8(uint8_t sid, uint16_t addr, uint8_t val);
int spmi_read8(uint8_t sid, uint16_t addr, uint8_t *val);
/* Ownership probe, READ-ONLY: answers "would a write here be denied?" from the
 * arbiter's own table instead of by attempting one. The RTC counter write cost
 * a flash cycle and a TZ reset to learn that the hard way — use this first. */
int spmi_apid_of(uint8_t sid, uint16_t addr);   /* arb channel, -1 unmapped */
int spmi_owner_ee(uint8_t sid, uint16_t addr);  /* owning EE, -1 unmapped */
int spmi_writable(uint8_t sid, uint16_t addr);  /* 1 yes, 0 denied, -1 unmapped */

/* pmic_rtc.c — PMIC RTC (PM660 on the Gen 6; pm8941-class register map).
 * Seconds since the RTC's own epoch (it simply counts from whenever it was
 * zeroed; the stock OS stores UTC in it, so treat it as Unix time). */
int rtc_read_epoch(uint32_t *sec);        /* 0 ok, -1 SPMI error */
int rtc_write_epoch(uint32_t sec);        /* 0 ok, -1 error (may be RO) */
/* Alarm block (0x61xx) — the always-on wake source deep sleep wants. Separate
 * peripheral from the counter, so it may be writable even though the counter
 * is secure-owned. Both of these are read-only probes; neither arms anything. */
int rtc_alarm_read(uint32_t *match, uint8_t *ctrl);  /* 0 ok, -1 SPMI error */
int rtc_alarm_writable(void);             /* 1 armable, 0 denied, -1 unmapped */

/* pmic_fg.c — PM660 fuel gauge + SMB2 charger status (Gen 6; -1 stubs on the
 * Gen 4, whose PM8916 VM-BMS gauge is a different, unported block). */
/* irq.c: 1 = the FreeRTOS tick interrupt is live, so WFI always wakes within
 * one tick. Wait loops must WFI when set and spin when clear (see the
 * 100%-duty bug note in irq.c). */
extern volatile uint32_t g_tick_armed;

/* irq.c: tickless suspend window. tick_park() re-points the CNTV comparator at
 * an absolute deadline instead of the next 1 ms boundary, so a sleeping core
 * takes ONE wakeup instead of one per millisecond; tick_unpark() restores the
 * 1 kHz cadence and returns the number of ticks the scheduler never saw (feed
 * them to xTaskCatchUpTicks). Prerequisite for every deeper idle state: the
 * cheapest system level needs 1.25 ms of residency, system-pc needs 5.3 ms. */
/* psci.c — PSCI over SMC. The DTB routes the whole LPM ladder through PSCI
 * (arm,psci-1.0 / method=smc / qcom,use-psci), so every state below WFI is
 * requested from the secure world. Only cpu-pc, perf-l2-pc and system-pc are
 * power-down (qcom,is-reset) and need warm-boot code; the rest return in
 * place. g_psci_suspend_ok is set by psci_report() after it has PROVEN the
 * SMC path reaches a real PSCI implementation — never call CPU_SUSPEND
 * without checking it. */
uint32_t psci_version(void);
int32_t  psci_features(uint32_t fn);
int32_t  psci_cpu_suspend(uint32_t power_state, uint32_t entry, uint32_t ctx);
int32_t  psci_cpu_on(uint32_t target, uint32_t entry, uint32_t ctx);
int32_t  psci_affinity_info(uint32_t target, uint32_t level);
int32_t  psci_system_suspend(uint32_t entry, uint32_t ctx);
void     psci_report(void);
extern uint32_t g_psci_suspend_ok;
extern uint32_t g_psci_sys_suspend_ok;   /* SYSTEM_SUSPEND is implemented */
extern uint32_t g_psci_cpu_on_ok;        /* CPU_ON is implemented         */
extern uint32_t g_psci_cpu_on_landed;    /* cpu1 reached OUR entry point   */
extern uint32_t g_psci_state;        /* power_state suspend passes to PSCI */
extern volatile uint32_t g_psci_fail_n;
extern volatile int32_t  g_psci_last_err;

void     tick_park(uint64_t deadline_ticks);
uint32_t tick_unpark(void);
/* Re-arm the tick after a CPU power-down took CNTV_CTL/CVAL and the banked
 * PPI enable with it. CNTVCT itself is always-on and keeps counting. */
void     tick_rearm(void);
void     tick_stop(void);   /* silence the tick before a power-down */

/* gic.c — restore the per-CPU GIC half after a power-down. The distributor
 * (SPI enables/priorities) is in the always-on domain and survives; the CPU
 * interface does not, and without it nothing is ever presented again. */
void     gic_cpu_resume(void);
int      gic_is_pending(unsigned id);  /* distributor view; survives collapse */

/* cpu_pc.c / cpu_suspend.S — CPU power collapse with warm boot. This is the
 * first state that actually switches the core off, and the gateway to
 * system-ret / system-pc (both need cpu-pc as their child state).
 * cpu_pc_sleep() returns 1 if the core really collapsed, 0 if PSCI declined. */
int      cpu_pc_sleep(void);
void     cpu_pc_report(void);
void     cpu_pc_selftest_run(void);
/* Does TZ deliver control to an entry point WE choose? Tested on cpu1 via
 * CPU_ON, so it costs no risk to our own core. */
void     cpu_pc_cpu_on_test(void);
/* The last IMEM breadcrumb the previous (dead) boot left behind. Call early. */
void     cpu_pc_prev_mark_report(void);
/* PSCI_SYSTEM_SUSPEND: suspend-to-RAM where the FIRMWARE owns the collapse. */
int      cpu_pc_system_suspend(void);

/* mpm.c — MSM Power Manager, the always-on block that must wake a collapsed
 * core. READ-ONLY until the GIC->pin table is known; mpm_dump() before and
 * after a sleep identifies the pin empirically (the STATUS bit that moves). */
void     mpm_report(void);
void     mpm_dump(const char *tag);
uint32_t mpm_status(unsigned word);
int      mpm_arm_pmic_wake(void);   /* 1 = pin 62 latched; 0 = do NOT collapse */
void     mpm_disarm_pmic_wake(void);
extern volatile uint32_t g_mpm_irq_n, g_mpm_last_sts0, g_mpm_last_sts1;
extern volatile uint32_t g_mpm_ram_live;  /* 0 = vMPM writes do not stick */
extern uint32_t g_cpu_pc_selftest_ok;
extern uint32_t g_cpu_pc_resumes;   /* bumped by the assembly resume path */
extern uint32_t g_cpu_pc_attempts, g_cpu_pc_declined, g_cpu_pc_ok;
extern uint32_t g_cpu_pc_last_ms;
extern int32_t  g_cpu_pc_last_rc;

/* suspend_msm.c (gen6) — blocking suspend: panel off, tickless chunked sleep
 * until a button (kpdpwr/resin), touch INT, or the armed timer deadline;
 * resumes in place. Backs esp_deep_sleep_start(). */
void plat_suspend(void);
void plat_suspend_set_timer_us(unsigned long long us);

/* sleep_stats.c (gen6) — READ-ONLY sleep accounting. These counters are how a
 * sleep experiment is proved rather than inferred: rpm apss numshutdowns rises
 * when APSS really power-collapses, and the MPM2 counter keeps time while the
 * ARM architected timer is dead. */
void     sleep_stats_report(void);   /* one-shot feasibility probe at boot */
void     sleep_stats_line(void);     /* compact counters, diff across a sleep */
uint32_t mpm_sleep_counter(void);    /* always-on 32768 Hz counter */
uint32_t rpm_apss_shutdowns(void);   /* APSS power-collapse count */

/* pwr_diag.c — 10 s PWR census line: FG/charger + TSENS + CPU clock + load.
 * Call once per app-loop iteration with that iteration's body time. */
void pwr_diag_poll(uint32_t body_ms);

/* LVGL render census (compat/owf_fossil_lvgl.h feeds these; pwr_diag prints a
 * 10 s delta line). The point: pwr_diag's cpu% says HOW MUCH the loop computes
 * but not WHAT. These split the idle load into refresh cycles, cycles that
 * actually rendered, pixels redrawn, and time in the cache flush — enough to
 * tell "LVGL is redrawing too much" from "LVGL is fine, the cost is elsewhere",
 * which guessing from the source could not. Monotonic; read as deltas. */
extern volatile uint32_t g_lv_refr_n;    /* refresh cycles (incl. empty ones) */
extern volatile uint32_t g_lv_render_n;  /* cycles that actually rendered      */
extern volatile uint32_t g_lv_refr_us;   /* us inside refresh (render+flush)   */
extern volatile uint32_t g_lv_flush_us;  /* us inside fb_flush_all (D-cache)   */
extern volatile uint32_t g_lv_px;        /* pixels handed to the flush cb      */
extern volatile uint32_t g_fb_cache_us;  /* us in the 519 KB D-cache clean     */
extern volatile uint32_t g_touch_us;     /* us inside the LVGL indev read      */
extern volatile uint32_t g_touch_n;      /* indev reads (runs even when idle)  */
/* spmi_arb.c — every PMIC access funnels through here, including OWF's
 * per-loop-iteration boot-button poll (digitalRead -> pon_kpdpwr_pressed). */
extern volatile uint32_t g_spmi_n;
extern volatile uint64_t g_spmi_ticks;
int  pwr_cpu_pct(void);      /* this core's real usage %, -1 unknown */
int  pwr_soc_temp_dc(void);  /* hottest TSENS channel, deci-C; -9999 err */
int  pwr_cpu_mhz(void);      /* live CPU clock, MHz; -1 unknown */
int  cpu_clk_set_mhz(int mhz); /* set CPU clock (kernel RCG/PLL dance); result MHz, <0 fail */

int fg_batt_percent(void);   /* 0-100, -1 on error */
int fg_batt_mv(void);        /* mV, -1 on error */
int fg_batt_ma(void);        /* mA, + = discharging; -32768 on error */
int fg_batt_temp_dc(void);   /* deci-degC; -9999 on error */
int chg_usb_present(void);   /* 1/0, -1 on error */
int chg_charging(void);      /* 1/0, -1 on error */

/* bootmark.c — boot-progress breadcrumbs in IMEM (0x08600800), survive a warm
 * reset and the next kernel's boot. Read from a rooted Linux on the watch:
 *   devmem 0x08600800  -> 0x4F574642 ("OWFB") if our code ran at all
 *   devmem 0x08600804  -> highest BOOTMARK_* stage reached
 *   devmem 0x08600808 ... 0x08600814 -> aux values (fb address, geometry, rc) */
void bootmark(uint32_t stage);
void bootmark_aux(unsigned idx, uint32_t val);

#define BOOTMARK_START      1u   /* startup.S entry (written in asm) */
#define BOOTMARK_RELOCATED  2u   /* self-relocation + bss clear done, in C */
#define BOOTMARK_WDOG_OFF   3u   /* APPS watchdog disarmed */
#define BOOTMARK_VIB        4u   /* vib_init/vib_buzz returned */
#define BOOTMARK_MMU        5u   /* MMU + caches on */
#define BOOTMARK_GIC        6u   /* GIC + IRQ up */
#define BOOTMARK_FB         7u   /* fb_init returned (aux0 = buffer address) */
#define BOOTMARK_SCHED      8u   /* about to start the FreeRTOS scheduler */
#define BOOTMARK_APP        9u   /* app task entered (setup() about to run) */
#define BOOTMARK_LVGL      10u   /* LVGL display created */
#define BOOTMARK_LOOP      11u   /* first loop() iteration reached */

/* msm_wdog.c — the APPS hardware watchdog aboot leaves ARMED (bark 11 s).
 * MUST be disabled first thing in main() or every boot warm-resets into the
 * stock OS at ~15 s (observed on hardware). Our own dead-man then owns
 * recovery. */
void wdog_disable(void);
void wdog_extend(uint32_t sec);   /* preferred: keep the backstop, widen it.
                                     CLAMPED to 31 s: the hardware register is
                                     20-bit and TRUNCATES silently above that */
void wdog_pet(void);
void wdog_stage(unsigned stage);  /* boot-progress reporting by reboot timing;
                                     stage->seconds table lives in msm_wdog.c */

/* sdhci_msm.c — READ-ONLY eMMC via the SDHCI controller aboot left running.
 * No write path exists by design (untested storage writes could damage the
 * stock install). 512-byte blocks, sector addressing, bounded polls. */
int gcc_sdcc1_up(void);                                /* SDCC1 GCC clocks */
int gcc_sdcc1_set_rate(int ident);                     /* 1=400kHz 0=25MHz */
uint32_t emmc_last_error(void);                        /* INT_STATUS at fail */
uint32_t emmc_fail_where(void);                        /* init-ladder position */
int emmc_init(void);                                   /* probe; 0 ok */
int emmc_read_block(uint32_t lba, void *dst);          /* one 512B block */
int emmc_read(uint32_t lba, uint32_t nblocks, void *dst);
int emmc_gpt_find(const char *name, uint32_t *out_lba, uint32_t *out_nblk);
int emmc_gpt_find_largest(uint32_t *out_lba, uint32_t *out_nblk);
int emmc_write_window(uint32_t lba, uint32_t nblocks); /* arm ONCE per boot */
int emmc_write_block(uint32_t lba, const void *src);   /* fenced, window only */
int emmc_write(uint32_t lba, uint32_t nblocks, const void *src);

/* storage_gen6.c — userdata-partition storage core (region ids: 0=blackbox,
 * 1=nvs, 2=ffat). storage_init is idempotent-ish via storage_ok(). */
int      storage_init(void);
int      storage_ok(void);
uint32_t storage_region_lba(unsigned id);
uint32_t storage_region_nblk(unsigned id);
void     blackbox_flush(void);      /* rate-limited; call from the app loop */

/* nvs_store.c — Preferences backend (all return <0 / 0 on error/miss) */
int nvs_load(void);
int nvs_commit(void);
int nvs_put(const char *ns, const char *key, uint8_t type,
            const void *data, uint32_t len);
int nvs_get(const char *ns, const char *key, void *out, uint32_t cap);
int nvs_erase(const char *ns, const char *key);
int nvs_erase_ns(const char *ns);
int nvs_haskey(const char *ns, const char *key);

/* pmic_pon.c — qpnp-power-on real-time button state (both watches).
 * 1 = held, 0 = released, -1 = SPMI error. */
/* pmic_irq.c — PMIC interrupts (buttons now, RTC alarm next) routed through
 * the SPMI arbiter to GIC SPI 190 / INTID 222. This is the always-on wake
 * source a powered-down core needs: cpu-level "pc" carries
 * qcom,use-broadcast-timer precisely because CNTV dies with the core.
 * Added alongside the existing poll — g_pmic_irq_n == 0 means the chain is
 * not working and the poll is still doing the waking. */
int pmic_irq_init(void);
extern volatile uint32_t g_pmic_irq_n, g_pmic_irq_kpdpwr, g_pmic_irq_resin;
extern volatile uint32_t g_pmic_irq_wake, g_pmic_irq_spurious, g_pmic_irq_stuck;

int pon_kpdpwr_pressed(void);    /* power / crown button */
int pon_resin_pressed(void);     /* second pusher */

/* pmic_vib.c — SPMI + vibration motor. THE sign-of-life channel during bring-up:
 * it is the only output that needs neither the display stack nor a UART pad, so
 * a buzz proves our code is executing when everything else is still dark. */
/* rot_pat9126.c — PixArt PAT9126 optical rotation sensor = the Gen 4's crown.
 * crown_take_delta() returns RAW sensor counts since the last call (signed),
 * not detents: the UI decides the counts-per-step. 0 when absent. */
int  crown_init(void);        /* 0 = present and answering, -1 = absent */
void crown_poll(void);        /* rate-limited internally; safe every loop */
int  crown_take_delta(void);  /* accumulated signed counts, and reset */
int  crown_present(void);

int  vib_init(void);              /* set drive voltage; 0 = ok, -1 = SPMI error */
void vib_set(int on);
void vib_buzz(unsigned n, uint32_t ms);   /* n pulses; polled, pre-scheduler safe */
#endif

/* console: fan out to UART + ramlog */
void con_putc(char c);
void con_flush(void);   /* push an unterminated line into the ramlog */
void con_puts(const char *s);
void con_puthex(uint32_t v);
void con_putdec(uint32_t v);

/* DEEP-SLEEP DIAGNOSTICS. The sleep work needed a lot of instrumentation —
 * the read-only feasibility probe, the PSCI/MPM/SPMI dumps, the warm-boot
 * selftest — and all of it printed on every boot and every sleep, which buried
 * the lines that actually matter in normal use. It is kept (it was expensive to
 * write and will be wanted again if cpu-pc is ever revisited) but is now silent
 * unless built with -DSLEEP_DIAG. Anything that CHANGES behaviour still runs
 * unconditionally; only the printing is gated.
 *
 * What stays visible without the flag: the one-line "suspend: ..." on entry and
 * the "suspend: woke by ..." on exit. That is the whole sleep story in two
 * lines. */
/* BRING-UP / SUBSYSTEM DIAGNOSTICS. Same idea as SLEEP_DIAG below, for the
 * chatter that was needed while a subsystem was being brought up and is noise
 * once it works. All default to SILENT; add the flag to CFLAGS_EXTRA to get the
 * detail back. Failures and faults are NEVER gated — only the running
 * commentary is.
 *   -DBOOT_DIAG  eMMC/SDHCI init, clock (gcc-*) bring-up, USB PHY + controller
 *                registers, I2C QUP version, framebuffer/panel/TLMM takeover
 *   -DLV_DIAG    the per-10 s LV / LV2 / LV3 render, touch and SPMI census
 *                (written to hunt the 12% idle-CPU bug, which is fixed)
 *   -DDSI_DIAG   the per-10 s "DSI arm=" register dump. The line still prints
 *                by itself whenever recov/underflow/drto are non-zero, so a
 *                real display regression is not hidden by silencing it. */
#if defined(BOOT_DIAG)
#  define bdiag_puts(s)   con_puts(s)
#  define bdiag_puthex(v) con_puthex(v)
#  define bdiag_putdec(v) con_putdec(v)
#  define bdiag_flush()   con_flush()
#else
#  define bdiag_puts(s)   ((void)0)
#  define bdiag_puthex(v) ((void)0)
#  define bdiag_putdec(v) ((void)0)
#  define bdiag_flush()   ((void)0)
#endif

#if defined(LV_DIAG)
#  define lvdiag_puts(s)   con_puts(s)
#  define lvdiag_puthex(v) con_puthex(v)
#  define lvdiag_putdec(v) con_putdec(v)
#  define lvdiag_flush()   con_flush()
#else
#  define lvdiag_puts(s)   ((void)0)
#  define lvdiag_puthex(v) ((void)0)
#  define lvdiag_putdec(v) ((void)0)
#  define lvdiag_flush()   ((void)0)
#endif

#if defined(SLEEP_DIAG)
#  define diag_puts(s)    con_puts(s)
#  define diag_puthex(v)  con_puthex(v)
#  define diag_putdec(v)  con_putdec(v)
#  define diag_flush()    con_flush()
#  define DIAG_ONLY(x)    x
#else
#  define diag_puts(s)    ((void)0)
#  define diag_puthex(v)  ((void)0)
#  define diag_putdec(v)  ((void)0)
#  define diag_flush()    ((void)0)
#  define DIAG_ONLY(x)    ((void)0)
#endif

extern uint32_t boot_r2;      /* r2 as received from the loader (DTB/ATAGS?) */
extern uint32_t boot_fault;   /* nonzero = a fault stub parked the CPU */
