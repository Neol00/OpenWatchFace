/* gen4_stubs.c — the Gen 4's not-yet-ported subsystems, as no-ops.
 *
 * The Gen 6 reached a booting firmware by stubbing every peripheral first and
 * filling drivers in one at a time; this file is the Gen 4's version of that
 * boundary. Each stub stands for a real Gen 6 driver that has not been carried
 * across yet, and each is written to FAIL SAFELY — returning "no device" rather
 * than pretending to work, so the firmware above takes its own absent-hardware
 * path instead of hanging or writing into nothing.
 *
 * Delete an entry here the moment its real driver lands; the linker will then
 * pick the driver up instead. Nothing in this file touches hardware.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

#include <stddef.h>

/* --- storage: NO LONGER STUBBED (2026-08-30) ----------------------------
 * eMMC (sdhci_msm.c + gcc_sdcc.c), the userdata region map (storage_gen6.c),
 * the NVS Preferences backend (nvs_store.c), FatFs and the ramlog mirror file
 * (logfile.c) are the Gen 6's files built as-is; the board header declares
 * PLAT_HAVE_EMMC_STORAGE plus the four things that differ on this SoC (pad
 * register value, card-VDD LDO, no CQE, no ICE). */

/* --- power/thermal census (platform/pwr_diag.c on the Gen 6) -------------
 * TSENS is readable on this SoC but not yet ported, so the die temperature
 * stays an honest -9999.
 *
 * CPU LOAD IS NOT A STUB ANY MORE. It used to return -1, which the Power app
 * clamps to 0 — so both msm8909w watches displayed a permanent 0% CPU that
 * looked like a broken readout rather than a missing one. The measurement the
 * Gen 6 uses needs nothing SoC-specific: it is the share of wall-clock time
 * the app loop spends in its own body, which arduino_main.cpp already hands us
 * once per iteration. pwr_diag.c cannot simply be compiled here because the
 * rest of that file is Gen 6 clock/TSENS/PLL code, so the census — and only
 * the census — is reproduced below.
 *
 * DEFINITION: busy% = (loop-body time MINUS time slept inside it) / window.
 *
 * The subtraction is the whole point. A first cut without it reported a
 * pegged 86% on an idle watch, which is exactly what you get by measuring
 * wall-clock time around a loop() that spends most of itself inside delay():
 * the sleep is real elapsed time, so it lands in the body measurement, but it
 * is emphatically not compute. g_pwr_sleep_ms (compat/arduino_glue.cpp) is a
 * monotonic counter of exactly that, and the Gen 6 has always subtracted it —
 * this build simply had not.
 *
 * The display transfer is subtracted too. msm_mdp3.c now keeps g_fb_wait_us
 * (microseconds spun waiting for DMA_P_DONE), which is the panel's transfer
 * time rather than the CPU's work — the second reason an idle watch read ~86%.
 *
 * What REMAINS counted as busy, deliberately: the per-frame cache clean. On a
 * 360x360 XRGB8888 frame that is ~518 KB of maintenance per flush, and it is
 * real CPU work, so hiding it would turn a measurement into a flattering
 * fiction. If this still reads high while idle, that number is the truth and
 * it points at partial-update rendering as the fix, not at the census. */
#define PWR_WIN_MS  2000u      /* census window; matches the app's refresh */

extern volatile uint32_t g_pwr_sleep_ms;   /* arduino_glue.cpp delay() */
extern volatile uint32_t g_fb_wait_us;     /* msm_mdp3.c DMA_P wait   */

static uint32_t s_pwr_busy_ms;
static uint32_t s_pwr_loops;
static uint32_t s_pwr_win_start;
static uint32_t s_pwr_sleep_base;
static uint32_t s_pwr_wait_base;
static uint32_t s_pwr_last_pct;

void pwr_diag_poll(uint32_t body_ms)
{
    uint32_t now = timer_ms();
    uint32_t win, slept, busy;

    if (s_pwr_win_start == 0u) {
        s_pwr_win_start  = now;
        s_pwr_sleep_base = g_pwr_sleep_ms;
        s_pwr_wait_base  = g_fb_wait_us;
        return;
    }

    s_pwr_busy_ms += body_ms;
    s_pwr_loops++;

    win = now - s_pwr_win_start;
    if (win >= PWR_WIN_MS) {
        /* Unsigned subtraction, so the counter wrapping costs one window
         * rather than producing a nonsense figure. */
        slept = g_pwr_sleep_ms - s_pwr_sleep_base;
        /* Display transfer: the CPU is spinning on the panel, not computing. */
        slept += (g_fb_wait_us - s_pwr_wait_base) / 1000u;
        busy  = (s_pwr_busy_ms > slept) ? (s_pwr_busy_ms - slept) : 0u;

        uint32_t pct = (busy * 100u) / win;
        s_pwr_last_pct = pct > 100u ? 100u : pct;
        /* LOAD CENSUS (2026-09-04): one line per ~10 s so the idle cost can
         * be attributed. loops = app-loop passes, touch = indev read time,
         * spmi = arbiter transactions (button/battery/charger polls). */
        {
            extern volatile uint32_t g_touch_us, g_touch_n, g_spmi_n;
            extern volatile uint64_t g_spmi_ticks;
            static uint32_t acc_win, acc_busy, acc_slept, acc_loops, n_win;
            static uint32_t t_us0, t_n0, sp_n0; static uint64_t sp_t0;
            acc_win += win; acc_busy += busy; acc_slept += slept; acc_loops += s_pwr_loops; n_win++;
            if (n_win >= 5u) {
#if defined(LOG_VERBOSE)
                uint32_t f = timer_freq_hz() / 1000000u; if (!f) f = 1u;
                con_puts("load: cpu="); con_putdec(acc_win ? acc_busy * 100u / acc_win : 0u);
                con_puts("% busy="); con_putdec(acc_busy); con_puts("ms slept="); con_putdec(acc_slept);
                con_puts("ms of "); con_putdec(acc_win); con_puts("ms loops="); con_putdec(acc_loops);
                con_puts(" touch="); con_putdec(g_touch_n - t_n0); con_puts("x/"); con_putdec((g_touch_us - t_us0) / 1000u);
                con_puts("ms spmi="); con_putdec(g_spmi_n - sp_n0); con_puts("x/");
                con_putdec((uint32_t)((g_spmi_ticks - sp_t0) / f / 1000u)); con_puts("ms\n");
#endif
                t_us0 = g_touch_us; t_n0 = g_touch_n; sp_n0 = g_spmi_n; sp_t0 = g_spmi_ticks;
                acc_win = acc_busy = acc_slept = acc_loops = n_win = 0u;
            }
            s_pwr_loops = 0u;
        }

        s_pwr_busy_ms    = 0u;
        s_pwr_win_start  = now;
        s_pwr_sleep_base = g_pwr_sleep_ms;
        s_pwr_wait_base  = g_fb_wait_us;
    }
}

int pwr_cpu_pct(void)     { return (int)s_pwr_last_pct; }
/* pwr_soc_temp_dc() is REAL on this SoC now — platform/tsens_8909.c. */
/* pwr_cpu_mhz / cpu_clk_set_mhz are REAL on this watch — platform/cpu_clk_a7.c. */

/* --- LVGL / touch census counters ---------------------------------------
 * Real storage, not stubs: compat/owf_fossil_lvgl.h WRITES these on every
 * refresh regardless of board, and only the Gen 6's pwr_diag.c reads them.
 * They cost 32 bytes and keep the instrumentation honest if a reader lands. */
volatile uint32_t g_lv_refr_n, g_lv_render_n, g_lv_refr_us, g_lv_flush_us;
volatile uint32_t g_lv_px, g_fb_cache_us, g_touch_us, g_touch_n;
volatile uint32_t g_fb_wait_us;   /* us spun waiting for DMA_P_DONE */


/* --- USB CDC-ACM log console: NO LONGER STUBBED --------------------------
 * These three stubs are gone. The claim that used to sit here -- that the
 * msm8909w carries a plain ULPI SNPS PHY and so needed a port rather than a
 * recompile -- was a guess, and the device trees contradict it: both
 * firefish and skipjack describe usb@78d9000 with phy-type <3>
 * (QUSB_ULPI_PHY), a "phy_csr" reg at 0x6c000 and a phy_csr_clk, which is
 * the QUSB2 signature and is field-for-field the Gen 6's node. The real
 * drivers (gcc_usb.c, usb_phy_msm.c, usb_ci.c) are now built for every board
 * that defines PLAT_HAVE_USB_CDC, this SoC included. */

#endif /* PLAT_SOC_MSM8909 */
