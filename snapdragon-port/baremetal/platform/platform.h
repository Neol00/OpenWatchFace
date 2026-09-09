/* platform.h — selects the board header and declares the tiny platform API
 * every snapdragon-port target implements. Build sets exactly one PLAT_BOARD_*. */
#pragma once
#include <stdint.h>

#if defined(PLAT_BOARD_QEMU_VIRT)
#include "../boards/qemu_virt.h"
#elif defined(PLAT_BOARD_FOSSIL_GEN4)
#include "../boards/fossil_gen4.h"
#elif defined(PLAT_BOARD_FOSSIL_GEN6)
#include "../boards/fossil_gen6.h"
#elif defined(PLAT_BOARD_TICWATCH_S2)
/* Built with PLAT_BOARD_TICWATCH_C2 defined as well (same SoC, same drivers);
 * the S2 header includes the C2's and overrides the panel geometry. */
#include "../boards/ticwatch_s2.h"
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
uint32_t ramlog_written(void);   /* bytes logged so far (cursor space) */

/* usb_ci.c / usb_phy_msm.c / gcc_usb.c — CDC-ACM log console over USB */
int  gcc_usb_hs_up(void);
int  usb_dev_reinit(void);   /* usb_ci.c: full controller/PHY bring-up again (after a system collapse) */
int  usb_ulpi_write(uint8_t val, uint8_t reg);
int  usb_ulpi_read(uint8_t reg, uint8_t *out);
int  usb_phy_init_seq(void);
void usb_phy_qusb_por(void);
void gcc_usb_qusb2_phy_reset(void);
void usb_diag(uint32_t *portsc, uint32_t *usbsts, uint32_t *vid, int *cfg);
void usb_phy_lowpower(int on);
void usb_irq_arm(int on);             /* usb_ci.c (-DUSB_IRQ_WAKE): controller IRQ as a wake source */        /* usb_ci.c: PORTSC.PHCD for a cable-less sleep */
/* sleep_floor.c (-DSLEEP_FLOOR): RPM active-set trimming + measured ladder */
void sleep_floor_census(const char *tag);
void sleep_floor_enter(int cable);
void sleep_floor_exit(void);
int  usb_phy_force_vbus_valid(void);
int  usb_dev_init(void);
void usb_poll(void);
int  usb_is_configured(void);  /* cheap flag read; no ULPI traffic */
int  usb_vbus_valid(void);     /* OTGSC.BSV live VBUS sense; -1 if controller off */

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
int      gcc_blsp_qup_i2c_up(uint32_t cmd_rcgr, uint32_t cbcr, const char *name);
int      rpm_ldo_on(uint32_t id, uint32_t uv, uint32_t ma);   /* wcnss.c: vote a PM8916 LDO on */
void     sensor_scan(void);                                   /* sensor_scan.c: -DSENSOR_SCAN */
/* spm_8909.c / cpu_pc8909.c / scm.c: msm8909w CPU power collapse (sleep rung 2) */
void     spm_init(void);
int      spm_ready(void);
void     spm_cpu0_mode(int mode);    /* 0 wfi, 1 standalone pc, 2 pc (RPM notify) */
void     spm_l2_mode(int mode);      /* 0 ret, 1 gdhs, 2 pc */
uint32_t spm_cpu0_sts(void);
uint32_t spm_l2_sts(void);
int      scm_set_warmboot_addr(uint32_t addr);
int      scm_set_boot_addr(uint32_t addr, uint32_t flags);   /* SVC_BOOT/BOOT_ADDR {flags, addr} */
void     smp_cpu1_test_poll(void);   /* smp_8909.c: -DSMP_TEST second-core bring-up */
void     timer_delay_us(uint32_t us);
int      scm_terminate_pc(uint32_t l2_flag);
void     cpu_pc8909_init(void);
void     cpu_pc8909_prev_report(void);
void     cpu_pc8909_mark(uint32_t v);
void     cpu_pc8909_trace(uint32_t v);   /* flash-committed text breadcrumb (IRQs on only) */
void     ramlog_prev_tail(uint32_t n);
int      cpu_pc8909_ready(void);
int      cpu_pc8909_sleep(uint64_t wake_ticks);
void     cpu_pc8909_report(void);
/* imu_lsm6ds3.c: LSM6DS3 on bit-banged SPI gpio8-11 (Gen 4 / C2), hardware pedometer */
int      lsm6ds3_init(void);
int      lsm6ds3_present(void);
void     lsm6ds3_steps_start(void);
void     lsm6ds3_accel_on(void);
void     lsm6ds3_stop(void);
uint32_t lsm6ds3_steps_poll(void);
uint32_t lsm6ds3_steps_total(void);
void     lsm6ds3_steps_set_total(uint32_t t);
int      lsm6ds3_accel_read(int16_t xyz[3]);
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
void     mdp3_kick(const void *buf);          /* start DMA_P on a pre-cleaned buffer, no wait */
int      mdp3_wait(uint32_t timeout_ms);      /* retire the last kick (0 ok)                 */
uint32_t mdp3_frame_bytes(void);
/* smp_8909.c: frame push served by CPU1 (0 = not available, flush synchronously) */
int      smp_flush_available(void);
void     smp_flush_post(const void *buf);     /* hand a cleaned buffer to CPU1 and return   */
int      smp_flush_wait(void);                /* block until CPU1 retired it; <0 = timeout  */
void    *fb_ptr2(void);                       /* fb_mdp3.c: the second (back) framebuffer   */
void     fb_flush_buf(const void *buf);       /* clean + push `buf`; async when CPU1 is up  */
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
void poweroff_now(void);          /* PMIC shutdown; wake = power key or USB */
void pon_ps_hold_warm(void);
void ramlog_clean_to_ddr(void);
void pon_crumb_write(uint8_t v); uint8_t pon_crumb_read(void); void pon_crumb_report(void);      /* PS_HOLD -> warm reset so breadcrumbs survive a wdog bite */
void pon_ps_hold_hard(void);      /* restore the PMIC default */
void reboot_to_bootloader(void);  /* warm reset -> aboot fastboot */
void reboot_to_recovery(void);    /* warm reset -> recovery */
void deadman_arm(uint32_t timeout_ms);  /* auto-reboot-to-fastboot if un-kicked */
void deadman_kick(void);          /* prove liveness; restart the countdown */
void net_keepalive(void);         /* wlan_net.c: wdog + dead-man + usb console, for blocking network waits */
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
void rtc_probe_report(void);              /* read-only ownership + state line at boot */
int  rtc_alarm_arm(uint32_t secs);        /* wake in `secs` (counter-relative) */
int  rtc_alarm_disarm(void);
void rtc_alarm_test_start(void);          /* arm 10 s + report when the IRQ lands */
void rtc_alarm_test_poll(void);
int  pmic_irq_alarm_init(void);           /* pmic_irq.c: route periph 0x61 irq 1 */

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
int      mpm_arm_pmic_wake(uint64_t wake_cntpct);   /* 1 = pin 62 latched; 0 = do NOT collapse. wake_cntpct: absolute QTimer tick for a timed wake, 0 = none */
void     mpm_ipc_irq_arm(void);   /* GIC 203 = the RPM's wake interrupt, edge-rising (vendor msm_mpm_irq) */
void     mpm_ipc_irq_disarm(void);
extern volatile uint32_t g_mpm_irq_n, g_mpm_irq_storm, g_mpm_last_sts0, g_mpm_last_sts1;
void     gic_set_edge(unsigned id, int edge);
void     gic_handoff_report(void);
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
/* dark resume: see suspend_msm.c */
#define PLAT_WAKE_NONE   0
#define PLAT_WAKE_BUTTON 1
#define PLAT_WAKE_TIMER  2
void plat_suspend_keep_dark(int on);   /* 1: a timer wake returns with the panel still off */
int  plat_suspend_last_cause(void);    /* PLAT_WAKE_* of the last plat_suspend() */
void plat_display_on(void);
int      pah8011_force_off(void);          /* hr_pah8011.c: sensor fully off, any state */
/* uart_bt.c / bt_wcn3990.c — Gen 6 Bluetooth (WCN3990 on blsp2_uart2) */
int      bt_uart_init(void);
void     bt_uart_putc(uint8_t c);
void     bt_uart_write(const uint8_t *b, uint32_t n);
int      bt_uart_try_putc(uint8_t c);              /* 1 sent, 0 not ready (no spin) */
int      bt_uart_getc(void);                       /* byte, or -1 if none */
int      bt_uart_read(uint8_t *buf, uint32_t max, uint32_t timeout_ms);
int      bt_uart_loopback_test(void);              /* 0 = TX+RX path proven */
int      bt_uart_set_baud(uint32_t baud);          /* 115200/460800/921600/3M/3.2M */
void     bt_hex2(uint8_t v);                       /* two-digit hex byte dump */
void     bt_wcn3990_power(int on);
void     bt_wcn3990_probe(void);
int      bt_wcn3990_bringup(void);         /* power + firmware, for the BLE-enable path */
int      bt_wcn3990_ready(void);
const uint8_t *bt_wcn3990_bdaddr(void);    /* controller's own address (6 B, LSB first) */
uint32_t bt_wcn3990_link_baud(void);
void     bt_fw_set_nvm_baud(uint8_t code); /* 0 = 115200, 0x11 = 3.2 Mbps */
/* bt_fw.c — firmware files in the `bluetooth` FAT16 partition */
int      bt_fat_init(void);
int      bt_fat_find(uint32_t dir_clus, const char *name11, uint32_t *clus, uint32_t *size);
int      bt_fat_stream(uint32_t clus, uint32_t size,
                       int (*sink)(const uint8_t *b, uint32_t n, void *arg), void *arg);
int      bt_fw_report(void);
int      bt_fw_download(void);             /* blocking: patch + NVM (~20 s at 115200) */
/* stepped download: one segment per call so the UI and usb_poll keep running */
struct bt_file { uint32_t clus, off, left, total; };
int      bt_file_open(uint32_t dir_clus, const char *name11, struct bt_file *f);
int      bt_file_read(struct bt_file *f, uint8_t *buf, uint32_t max);
int      bt_dl_start(void);
int      bt_dl_step(void);                 /* 0 busy, 1 done, <0 failed */
int      bt_probe_step(void);              /* whole bring-up, stepped; same returns */
void     tlmm_irq_mask(int mask);          /* tlmm_irq.c: mask the GPIO summary IRQ (suspend) */
int      touch_set_sleep(int sleep);       /* touch_ft.c: 1 = hibernate, 0 = reset awake */

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
/* chg_smb231.c — external SMB231 charger + STC3117 gauge on BLSP1 QUP4
 * (TicWatch C2/S2, PLAT_CHG_SMB231). pmic_fg.c consults these first. */
int  smb231_ready(void);        /* 1 once the part answered on the bus (probes lazily) */
int  smb231_usb_present(void);  /* 1/0, -1 unknown */
int  smb231_charging(void);     /* 1/0, -1 unknown */
int  smb231_batt_ma(void); int smb231_soc_x512(void);
#ifndef PLAT_BATT_MAH
#define PLAT_BATT_MAH 400   /* TicWatch C2 cell */
#endif      /* + = discharging; -32768 unknown */
void smb231_log_state(void);    /* raw status bytes, no newline */
void smb231_charger_suspend(int on); /* SUSP pin: 1 = run from battery with VBUS present */

void bt_hci_drop(void);                          /* bt_hci.c: forget channels after a PAS shutdown */
int  cpu_clk_sleep_enter(void);                  /* cpu_clk_a7.c: cluster clock -> XO for the sleep */
int  cpu_clk_sleep_exit(void);
void logfile_flush(void);                        /* logfile.c */
void gcc_mdss_sleep(int on);                     /* gate/ungate MDSS branches (GDSC + PLL kept) */
void gcc_sdcc1_sleep(int on);                    /* gate/ungate eMMC clocks */
void gcc_blsp_sleep(int on);                     /* gate/ungate running QUP core clocks */
void spm_cpu1_mode(int mode);                    /* spm_8909.c: CPU1's SAW */
int  smp_park_extra_cores(void);   /* smp_8909.c v180: cpu2/cpu3 into TZ power collapse */
int  spm_cpu1_ready(void); void spm_cpu_mode_n(unsigned cpu, int mode); int spm_cpu_ready_n(unsigned cpu);
uint32_t spm_cpu1_sts(void);
int  scm_set_warmboot_addr_cpu1(uint32_t addr);
int  scm_mc_boot_available(void);                /* TZ offers SCM_BOOT_ADDR_MC */
int  scm_set_warmboot_addr_mc_all(uint32_t addr);/* one warm-boot entry for every core */
extern void pc_warm_entry(void);                 /* smp_entry.S: MPIDR dispatch -> cpu_pc_resume / smp_secondary_entry */
int  smp_cpu1_pc_request(void);
int  smp_cpu1_pc_request_mode(uint32_t mode);      /* 1 = SMC, 2 = SPM+WFI without TZ */
int  smp_cpu1_wake(void);                        /* SGI, then cold boot if it stays down */                  /* smp_8909.c: collapse CPU1 for the sleep */
void gicd_save(void);  void gicd_restore(void);   /* gic.c: distributor across a cluster power-down */
void spm_l2_mode_noen(int mode); void spm_l2_mode_noen_noslp(int mode); int spm_l2_set_vdd(uint32_t vlevel); void spm_l2_mode_kernel_pc(void); void spm_l2_mode_kernel_gdhs(void); void spm_l2_ctl_zero(void); uint32_t spm_l2_ctl(void); int spm_l2_ready(void);
uint32_t mpm_status_word(unsigned idx); void mpm_status_clear(void);
extern uint32_t g_cpu_pc_l2_off;
void mpm_report(void);
int  sys_pc8909_init(void);                      /* sys_pc8909.c: sleep-set votes + MPM probe */
void rpm_master_stats_line(const char *tag);
int  sys_pc8909_prepare(uint64_t deadline_ms);   /* arm wake (pmic pin + vMPM timer), save GIC, L2 -> pc; 1 = go */
void sys_pc8909_finish(void);                    /* undo + census line */
void cpu_pc8909_state_line(const char *tag);   /* cpu_pc8909.c: register census around a collapse */
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
int emmc_write_window_boot(uint32_t lba, uint32_t nblocks); /* OTA only: bootimg_write.c */
/* bootimg_write.c -- over-the-air install of a verified Android boot image
 * into the `boot` partition. 0 ok; <0 = nothing written (or verify failed). */
int bootimg_write(const void *img, uint32_t len, void (*progress)(uint32_t done, uint32_t total));
/* rng_msm.c -- mbedtls_hardware_poll(): entropy for mbedTLS (SoC PRNG, timer-jitter fallback) */
int emmc_write(uint32_t lba, uint32_t nblocks, const void *src);

/* storage_gen6.c — userdata-partition storage core (region ids: 0=blackbox,
 * 1=nvs, 2=ffat). storage_init is idempotent-ish via storage_ok(). */
int      storage_init(void);
int      storage_ok(void);
int      storage_foreign(void);   /* 1: userdata still holds a Wear OS volume, storage left read-only */
uint32_t storage_region_lba(unsigned id);
uint32_t storage_boot_count(void);   /* superblock boot counter (1 on first boot) */
uint32_t storage_region_nblk(unsigned id);
void     blackbox_flush(void);      /* rate-limited; call from the app loop */
void     blackbox_sync(void);       /* unconditional flush: call before a step that may reset silently */
void     blackbox_print_previous(void); /* at boot: replay the previous boot's tail from eMMC */

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
/* ---- SMEM: shared memory with the WiFi/modem co-processors ---------------
 * Read-only for now, and the base layer the WiFi stack is being built on:
 * SMD channels live inside SMEM, and the wcn36xx HAL lives inside SMD.
 * Compiled only for boards that define PLAT_SMEM_BASE. */
#define SMEM_HOST_MODEM  1u
#define SMEM_HOST_ADSP   2u
#define SMEM_HOST_WCNSS  4u                  /* remote-pid of the wcnss SMD edge */
#define SMEM_HOST_RPM    6u
int      smem_init(void);                    /* 0 = header valid and parsed */
int      smem_ok(void);
uint32_t smem_version(void);                 /* 11 = global heap, 12 = partitioned */
void    *smem_get(uint32_t id, uint32_t *size_out);  /* global item; NULL if absent */
void    *smem_get_host(uint32_t host, uint32_t id, uint32_t *size_out);
                                             /* item in the apps<->host partition */
int      smem_host_partition_present(uint32_t host);
void     smem_diag_dump(void);               /* -DSMEM_DIAG: inventory + build id */
void     smem_scan_report(void);             /* -DSMEM_SCAN: late probe, USB up  */

/* scm.c — TrustZone calls. SMCCC/scm_call2 convention on PLAT_SCM_SMCCC
 * boards (Gen 6); elsewhere probed at first use and falls back to the legacy
 * command-buffer convention (msm8909w: Gen 4, TicWatch C2). */
int  scm_is_call_available(uint32_t svc, uint32_t cmd);  /* 1/0, <0 = query failed */
const char *scm_convention_name(void);                   /* "smccc" | "legacy" | "none" */
void scm_diag(void);
void tz_log_dump(void);
void kmsg_forensics_report(void);   /* -DKMSG_FORENSICS: previous kernel log from DDR */
void kf_tz_ring_dump(void);   /* dump the TrustZone diagnostic ring (reason for a PAS failure) */
void tz_log_tail(uint32_t n);   /* the last n bytes of that ring, one line */
void tz_boot_counters(const char *tag);   /* per-CPU warm-boot / pc entry-exit counters + reset counters (tzdbg boot/reset) */
int  scm_pas_init_image(uint32_t pas_id, uint32_t mdt_phys);   /* 0 = ok */
int  scm_pas_mem_setup(uint32_t pas_id, uint32_t base, uint32_t size);
int  scm_pas_auth_and_reset(uint32_t pas_id);
int  scm_pas_shutdown(uint32_t pas_id);
int  scm_pas_is_supported(uint32_t pas_id);   /* r1: 1 = PAS boot for this id */

/* wcnss.c — Pronto/WCNSS bring-up: firmware from the modem partition, rails
 * via the RPM, Iris XO config, PAS boot. -DWIFI_DIAG runs wcnss_boot_diag(). */
int  wcnss_boot(void);            /* 0 = WCNSS answered on SMD after PAS boot */
void wcnss_boot_diag(void);

/* smd.c — Shared Memory Driver channels (polled) + the RPM client on top. */
struct smd_chan {
    volatile uint32_t *tx, *rx;            /* info blocks: ours, theirs (word or byte layout) */
    volatile uint32_t *tx_fifo, *rx_fifo;
    uint32_t fifo_size, ipc_bit, cid, state, pkt_size;
    uint32_t byte_info;                    /* 1 = 20-byte smd_channel_info (WCNSS edge), 0 = 11-word (RPM) */
    uint32_t stream;                       /* 1 = stream channel (no packet headers; alloc flag 0x200 clear) */
};
#define SMEM_GLOBAL 0xFFFFu                /* smd_open host: items in the global heap */
int      smd_open(struct smd_chan *c, uint32_t host, uint32_t cid, uint32_t ipc_bit);
int      smd_send(struct smd_chan *c, const void *data, uint32_t len);
uint32_t smd_recv(struct smd_chan *c, void *buf, uint32_t max, uint32_t timeout_ms);
uint32_t smd_remote_state(struct smd_chan *c);   /* 2 = OPENED */
void smd_close(struct smd_chan *c);             /* our half -> CLOSED (before a PAS shutdown) */
uint32_t smd_rx_pending(struct smd_chan *c);     /* bytes waiting in the rx FIFO */
/* wcnss.c: SMD alloc table on the WCNSS edge. flags bit 0x200 = packet channel. */
int      wcnss_channel_lookup(const char *name, uint32_t *cid, uint32_t *flags);
void     wcnss_list_channels(void);
int      wcnss_fw_resident(void);                /* firmware loaded (WiFi MAC may be stopped) */
/* bt_hci.c -- Bluetooth HCI over the APPS_RIVA_BT_CMD / _ACL SMD channels */
int      bt_hci_open(void);                      /* boots WCNSS if needed; 0 = both channels open */
int      bt_hci_is_open(void);
int      bt_hci_send_cmd(const uint8_t *pkt, uint32_t len);   /* HCI command packet, no H4 byte */
int      bt_hci_send_acl(const uint8_t *pkt, uint32_t len);
void     bt_hci_set_rx(void (*evt)(const uint8_t *pkt, uint32_t len), void (*acl)(const uint8_t *pkt, uint32_t len));
void     bt_hci_poll(void);                      /* drain both channels into the rx callbacks */
uint32_t bt_hci_cmd_wait(const uint8_t *cmd, uint32_t len, uint8_t *evt, uint32_t max, uint32_t ms); /* sync: Command Complete/Status */
void     bt_hci_diag(void);                      /* raw HCI: reset, version, BD_ADDR, advertise */
/* wcn36xx.c -- WLAN data path (DXE rings) + passive scan over the HAL */
struct wlan_scan_net { char ssid[33]; uint8_t bssid[6]; uint8_t chan; int8_t rssi; uint8_t secured; };
int      wcn36xx_dxe_init(void);
uint32_t wcn36xx_rx_poll(void);
int      wcn36xx_scan(struct smd_chan *wlan, uint32_t dwell_ms, struct wlan_scan_net *out, uint32_t max);
int      wcn36xx_tx(const uint8_t bd[40], const uint8_t *frame, uint32_t len, int high);   /* high = mgmt ring */
void     wcn36xx_set_rx_handler(void (*fn)(const uint8_t *f, uint32_t len, int8_t rssi));
/* wcnss.c: one HAL request/response over WLAN_CTRL; returns reply bytes or 0 */
uint32_t wlan_hal_xfer(const void *msg, uint32_t len, uint32_t want, uint32_t *rsp, uint32_t max, uint32_t ms);
const uint8_t *wlan_mac(void);
/* wlan_sta.c -- station: authenticate, associate, WPA2-PSK handshake, keys */
int      wlan_sta_connect(const char *ssid, const char *pass, const struct wlan_scan_net *bss);
int      wlan_sta_connected(void);
int      wlan_sta_disconnect(void);
void     wlan_sta_reset(void);          /* after a HAL stop: the firmware forgot everything */
int      wlan_sta_tx_eth(const uint8_t *eth, uint32_t len);            /* Ethernet II frame out, encrypted */
int      wlan_sta_tx_probe(void);                                      /* diag: plain + encrypted dummy data frame */
void     wlan_sta_counters(uint32_t *tx, uint32_t *rx, uint32_t *rx_other);
void     wlan_sta_set_data_rx(void (*fn)(const uint8_t *eth, uint32_t len));
/* wcnss.c: the one lock every radio/network caller takes (recursive) */
void     wlan_lock(void);
void     wlan_unlock(void);
/* wlan_net.c -- lwIP over the station link. All calls take wlan_lock internally;
 * the blocking ones release it while they wait so the poll task can feed them. */
int      net_up(void);                              /* netif + DHCP, starts the poll task */
void     net_down(void);
int      net_has_ip(void);
uint32_t net_ip(void);                              /* host order, 0 if none */
int      net_wait_ip(uint32_t ms);
int      net_dns(const char *host, uint32_t *ip, uint32_t ms);
void     net_sntp_start(const char *s1, const char *s2);
int      net_time_synced(void);
void    *net_tcp_connect(uint32_t ip, uint16_t port, uint32_t ms);   /* handle or 0 */
int      net_tcp_write(void *h, const void *data, uint32_t len, uint32_t ms);
int      net_tcp_read(void *h, void *buf, uint32_t max);            /* >0 bytes, 0 none yet, <0 closed */
int      net_tcp_available(void *h);
int      net_tcp_connected(void *h);
void     net_tcp_close(void *h);
int8_t   wlan_sta_rssi(void);
/* wlan_crypto.c */
void     wpa_pmk_from_passphrase(const char *pass, const uint8_t *ssid, uint32_t ssid_len, uint8_t pmk[32]);
void     wpa_ptk(const uint8_t pmk[32], const uint8_t *aa, const uint8_t *spa, const uint8_t *anonce, const uint8_t *snonce, uint8_t ptk[48]);
void     hmac_sha1(const uint8_t *key, uint32_t klen, const uint8_t *msg, uint32_t mlen, uint8_t out[20]);
int      aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t inlen, uint8_t *out);
struct sha1 { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; };
void sha1_init(struct sha1 *c); void sha1_update(struct sha1 *c, const void *d, uint32_t n); void sha1_final(struct sha1 *c, uint8_t out[20]);
/* wcnss.c -- the radio as the app sees it (backs compat/WiFi.h on PLAT_WLAN_APP boards) */
int      wlan_up(void);        /* full bring-up: rails, PAS, NV, HAL start, DXE. 0 = ok. Idempotent. */
int      wlan_down(void);      /* light: HAL stop, firmware stays resident. */
int      wlan_power_off(void); /* full: HAL stop + PAS shutdown + rails released. */
int      wlan_idle(void);      /* sleep: HAL stop, firmware resident, AP-side WLAN votes released (the stock idle) */
int      wlan_is_up(void);
int      wlan_scan(struct wlan_scan_net *out, uint32_t max);   /* passive scan; count or <0 */
int      wlan_connect(const char *ssid, const char *pass);      /* scan for it, join, WPA2 handshake. 0 = keys installed */
int      wlan_connected(void);
int      wlan_disconnect(void);
int      rpm_smd_init(void);
int      rpm_smd_request(uint32_t set, uint32_t type, uint32_t id, const uint32_t *kv, uint32_t kv_bytes);
void     rpm_diag(void);

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
/* VERBOSE narration (bring-up step-by-step, register dumps, per-cycle
 * census lines) is compiled out unless -DLOG_VERBOSE. Errors, failures and
 * one-line milestones stay on con_puts. Use these for anything that would
 * print on a healthy boot more than once, or that only a developer reads. */
#if defined(LOG_VERBOSE)
#define con_dbg(s)      con_puts(s)
#define con_dbg_hex(v)  con_puthex(v)
#define con_dbg_dec(v)  con_putdec(v)
#define con_dbg_c(c)    con_putc(c)
#define con_dbg_flush() con_flush()
#else
#define con_dbg(s)      ((void)(s))
#define con_dbg_hex(v)  ((void)(v))
#define con_dbg_dec(v)  ((void)(v))
#define con_dbg_c(c)    ((void)(c))
#define con_dbg_flush() ((void)0)
#endif
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
/* DIAGNOSTIC MACROS EVALUATE THEIR ARGUMENTS EVEN WHEN SILENT (2026-09-05).
 * The first image ever built without -DBOOT_DIAG (v79) died in eMMC init on
 * the TicWatch C2: with the flag, the bring-up code carried register READ-
 * BACKS inside its prints (present-state after forcing card detect, the RCG
 * config after enabling the SDCC branch), and ((void)0) macros dropped those
 * reads along with the text. A quiet build must issue byte-identical bus
 * traffic to a verbose one; only the ramlog text may differ. Hence
 * ((void)(v)), never ((void)0), for every diag macro below.
 *
 * BRING-UP / SUBSYSTEM DIAGNOSTICS. Same idea as SLEEP_DIAG below, for the
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
#  define bdiag_puts(s) ((void)(s))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define bdiag_puthex(v) ((void)(v))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define bdiag_putdec(v) ((void)(v))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define bdiag_flush()   ((void)0)
#endif

#if defined(LV_DIAG)
#  define lvdiag_puts(s)   con_puts(s)
#  define lvdiag_puthex(v) con_puthex(v)
#  define lvdiag_putdec(v) con_putdec(v)
#  define lvdiag_flush()   con_flush()
#else
#  define lvdiag_puts(s) ((void)(s))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define lvdiag_puthex(v) ((void)(v))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define lvdiag_putdec(v) ((void)(v))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define lvdiag_flush()   ((void)0)
#endif

#if defined(SLEEP_DIAG)
#  define diag_puts(s)    con_puts(s)
#  define diag_puthex(v)  con_puthex(v)
#  define diag_putdec(v)  con_putdec(v)
#  define diag_flush()    con_flush()
#  define DIAG_ONLY(x)    x
#else
#  define diag_puts(s) ((void)(s))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define diag_puthex(v) ((void)(v))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define diag_putdec(v) ((void)(v))   /* ARGUMENT STILL EVALUATED: read-backs stay */
#  define diag_flush()    ((void)0)
#  define DIAG_ONLY(x)    ((void)0)
#endif

extern uint32_t boot_r2;      /* r2 as received from the loader (DTB/ATAGS?) */
/* ddr_size.c -- DDR size read at boot (aboot's DTB, else SMEM RAM table,
 * else PLAT_DDR_SIZE). Reporting only: the memory map stays PLAT_DDR_*. */
uint32_t ddr_size_detect(void);   /* call once after smem_init(); prints "ddr:" */
uint32_t plat_ddr_size(void);     /* bytes; PLAT_DDR_SIZE until detected */
extern uint32_t boot_fault;   /* nonzero = a fault stub parked the CPU */
