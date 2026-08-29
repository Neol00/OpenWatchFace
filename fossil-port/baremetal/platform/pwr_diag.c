/* pwr_diag.c — Gen 6 power/thermal/clock census (2026-08-06).
 *
 * GOAL: validate every power-related data source on hardware in ONE image,
 * before any of it is wired into the OWF power app or its fail-safes. The
 * fuel gauge was disabled wholesale on 2026-08-03 (board_power.h,
 * OWF_FOSSIL_FG_DISABLED) after junk readings and a suspected XPU reset —
 * but those reads predate the SPMI arbiter v2 APID-table fix in spmi_arb.c
 * ("channel 0 for everything"), which is exactly the kind of bug that
 * produces plausible junk. So: re-measure, raw values on the log, decide
 * from evidence.
 *
 * Prints one PWR line every 10 s with RAW register values alongside every
 * decoded number, so a wrong decode is visible instead of silently wrong.
 *
 * SAFETY SHAPE:
 *   - First SPMI/FG access is DEFERRED until 12 s after boot, and announced
 *     with a flushed marker line the loop iteration BEFORE it happens: if
 *     touching the FG really does trip a TZ/XPU hard reset, the log ends
 *     with "pwr: arming first FG read" and that is a definitive verdict.
 *   - TSENS and APCS reads are plain always-on-domain MMIO reads (thermal
 *     sensor + CPU clock hardware, alive while the CPU runs). TSENS is
 *     checked for "enabled" in SROT_CTRL before any TM read.
 *   - Everything is read-only. No PMIC writes, no clock writes.
 *
 * SOURCES (hoki 4.14 kernel + dumped DTB, sda429-hoki-decompiled.dts):
 *   FG/charger register map + scaling: see pmic_fg.c (fg-reg.h/smb-reg.h).
 *   tsens@4a8000 "qcom,msm8937-tsens": SROT 0x4a8000, TM 0x4a9000
 *     (drivers/thermal/qcom/tsens2xxx.c):
 *     SROT_CTRL +0x4 bit0 = tsens enabled
 *     TM_Sn_STATUS = TM + 0xa0 + 4*n, [11:0] = last temp, DECI-degC,
 *     12-bit two's complement; bit21 = valid. msm8937 family: 11 sensors.
 *   qcom,clock-cpu@b011050 "qcom,cpu-clock-sdm":
 *     apcs-c1-rcg-base 0xb011050 (CMD +0, CFG +4: src [10:8], div [4:0],
 *     freq = src / ((div+1)/2)), apcs_pll 0xb016000 (MODE +0, L_VAL +4;
 *     pll = L * 19.2 MHz). Decode is best-effort — raw CFG/L printed too.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

extern volatile uint32_t g_pwr_sleep_ms;   /* arduino_glue.cpp delay() */
extern volatile uint32_t g_fb_wait_us;     /* fb_splash.c fb_kick waits */

/* LVGL render census — fed by compat/owf_fossil_lvgl.h's display event hooks.
 * Defined here (a C file that is always linked) so both the C++ header and
 * this printer can reach them. See platform.h for what each one means. */
volatile uint32_t g_lv_refr_n, g_lv_render_n, g_lv_refr_us, g_lv_flush_us, g_lv_px;
volatile uint32_t g_fb_cache_us, g_touch_us, g_touch_n;

#define PWR_FIRST_MS     12000u   /* leave boot + a few census windows alone */
#define PWR_PERIOD_MS    10000u

/* ---- TSENS (tsens v1.4, msm8937 family) ----------------------------------
 * FIRST CUT WAS WRONG (pwr1: tsens=X,X,...): the 0xa0/bit21/deci-C layout is
 * the tsens2xxx generation. This SoC is tsens1xxx + data_tsens14xx (vendor
 * drivers/thermal/tsens1xxx.c, tsens.h):
 *   TSENS_SN_STATUS_ADDR_8937 = 0x44  -> Sn_STATUS = TM + 0x44 + 4n
 *   valid = bit 14, temp = [9:0], and it is a raw ADC CODE, not a degree.
 * Conversion needs the per-sensor factory calibration from the QFPROM
 * (tsens_calib.c calibrate_8937, eeprom reg = 0xa4000):
 *   words at +0x1D8 +0x1DC +0x210 +0x214 +0x230
 *   mode = word2 & 7:  3 = two-point (30 C + 120 C), 2 = one-point, else none
 *   p1/p2 fuse fields per sensor (masks below, kernel-verbatim), then
 *   p = (base + p) << 2
 *   slope = (p2-p1)*1000/90   (default 3200 if not two-point)
 *   offset = p1*1000 - 30*slope
 *   degc  = (code*1000 - offset) / slope       (code_to_degc)
 */
#define TSENS_SROT_BASE  0x004A8000u
#define TSENS_TM_BASE    0x004A9000u
#define TSENS_QFPROM     0x000A4000u
#define TSENS_SROT_CTRL  0x0004u
#define TSENS_TM_STATUS0 0x0044u     /* TSENS_SN_STATUS_ADDR_8937 */
#define TSENS_NSENSORS   11u

static int32_t s_ts_slope[TSENS_NSENSORS];
static int32_t s_ts_offset[TSENS_NSENSORS];
static uint32_t s_ts_mode = 0xFFu;   /* 0xFF = not yet calibrated */

static void tsens_calibrate(void)
{
    /* Kernel-verbatim fuse unpack (tsens_calib.c calibrate_8937). */
    static const uint32_t p1_msk[TSENS_NSENSORS] =
        { 0x000001F8, 0x001F8000, 0xF8000000, 0x00001F80, 0x01F80000,
          0x00003F00, 0x03F00000, 0x0000003F, 0x0003F000, 0x0000003F,
          0x0003F000 };
    static const uint8_t p1_sh[TSENS_NSENSORS] =
        { 3, 15, 27, 7, 19, 8, 20, 0, 12, 0, 12 };
    static const uint8_t p1_word[TSENS_NSENSORS] =
        { 2, 2, 2, 3, 3, 0, 0, 1, 1, 4, 4 };
    static const uint32_t p2_msk[TSENS_NSENSORS] =
        { 0x00007E00, 0x07E00000, 0x0000007E, 0x0007E000, 0x7E000000,
          0x000FC000, 0xFC000000, 0x00000FC0, 0x00FC0000, 0x00000FC0,
          0x00FC0000 };
    static const uint8_t p2_sh[TSENS_NSENSORS] =
        { 9, 21, 1, 13, 25, 14, 26, 6, 18, 6, 18 };
    static const uint8_t p2_word[TSENS_NSENSORS] =
        { 2, 2, 3, 3, 3, 0, 0, 1, 1, 4, 4 };

    uint32_t q[5];
    q[0] = mmio_read(TSENS_QFPROM + 0x1D8u);
    q[1] = mmio_read(TSENS_QFPROM + 0x1DCu);
    q[2] = mmio_read(TSENS_QFPROM + 0x210u);
    q[3] = mmio_read(TSENS_QFPROM + 0x214u);
    q[4] = mmio_read(TSENS_QFPROM + 0x230u);
    s_ts_mode = q[2] & 7u;               /* 3 = two-pt, 2 = one-pt */

    uint32_t base0 = q[0] & 0xFFu;
    uint32_t base1 = (q[1] & 0xFF000000u) >> 24;

    for (unsigned i = 0; i < TSENS_NSENSORS; i++) {
        uint32_t p1 = 500u, p2 = 780u;   /* kernel defaults, no-cal case */
        if (s_ts_mode == 3u || s_ts_mode == 2u) {
            p1 = (q[p1_word[i]] & p1_msk[i]) >> p1_sh[i];
            if (i == 2u)                 /* S2_P1 bit 5 lives in word 3 bit 0 */
                p1 |= (q[3] & 1u) << 5;
            p1 = (base0 + p1) << 2;
        }
        if (s_ts_mode == 3u) {
            p2 = (q[p2_word[i]] & p2_msk[i]) >> p2_sh[i];
            p2 = (base1 + p2) << 2;
        }
        s_ts_slope[i]  = (s_ts_mode == 3u)
                       ? ((int32_t)(p2 - p1) * 1000) / (120 - 30)
                       : 3200;           /* SLOPE_DEFAULT */
        if (s_ts_slope[i] <= 0) s_ts_slope[i] = 3200;
        s_ts_offset[i] = (int32_t)p1 * 1000 - 30 * s_ts_slope[i];
    }
    bdiag_puts("pwr: tsens calibrated, mode="); bdiag_putdec(s_ts_mode);
    bdiag_puts(" base0="); bdiag_putdec(base0);
    bdiag_puts(" base1="); bdiag_putdec(base1); bdiag_puts("\n");
}

/* Returns deci-degC; *valid = 0 means the sensor had no fresh sample. */
static int tsens_read_dc(unsigned n, int *valid)
{
    uint32_t st = mmio_read(TSENS_TM_BASE + TSENS_TM_STATUS0 + 4u * n);
    int32_t code = (int32_t)(st & 0x3FFu);   /* [9:0] raw ADC code */
    *valid = (st >> 14) & 1u;                /* TSENS_SN_STATUS_VALID */
    return (int)((code * 1000 - s_ts_offset[n]) * 10 / s_ts_slope[n]);
}

/* ---- APCS CPU clock ------------------------------------------------------
 * Kernel truth (clk-cpu-sdm.c + clk-pll.c + clk-regmap-mux-div.c, hoki 4.14):
 * the C1 RCG mux parents are src 0 = XO 19.2M, src 4 = GPLL0_AO 800 MHz,
 * src 5 = the dedicated APCS HF PLL at 0xb016000. div field is a HALF-integer
 * divider: freq = parent * 2 / (div + 1), so div=1 is /1, div=3 is /2.
 * THE BOOTLOADER LEAVES US ON GPLL0: cfg=0x401 = src4 div1 = 800 MHz. The HF
 * PLL is programmed (L=54, 1036.8 MHz) but NOT selected. The kernel's own
 * safe-switch dance (cpucc_notifier_cb): park the mux on GPLL0, reprogram the
 * PLL, switch to src 5.
 * VOLTAGE: the DT fmax table (sdm429.dtsi qcom,speed*-bin-v0-c1) puts 0-960M
 * AND 1305.6M all in vdd corner 1 (SVS) — the SAME rail level the bootloader
 * already established for 800 MHz operation. So any frequency <= 1305.6 MHz
 * is safe WITHOUT touching the APC regulator. Above that (1497.6M = corner 2)
 * needs an SPM/CPR rail raise first — NOT implemented; hard-capped below. */
#define APCS_C1_RCG_CMD  0x0B011050u   /* bit0 = UPDATE, self-clears */
#define APCS_C1_RCG_CFG  0x0B011054u
#define APCS_PLL_MODE    0x0B016000u   /* bit0 OUTCTRL, bit1 BYPASSNL, bit2 RESET_N */
#define APCS_PLL_L       0x0B016004u
#define APCS_PLL_M       0x0B016008u
#define APCS_PLL_N       0x0B01600Cu
#define APCS_PLL_STATUS  0x0B01601Cu   /* bit16 = locked */
#define APCS_SPM_BASE    0x0B011200u   /* spm_c1_base */
#define APCS_SPM_EVENT   0x50u         /* L2_SPM_FORCE_EVENT_EN, event bit 4 */

/* ---- CPU load (this core, our loop) --------------------------------------*/
static uint32_t s_busy_ms;      /* summed loop-body time in this window */
static uint32_t s_loops;        /* app-loop iterations in this window */
static uint32_t s_win_start;
static uint32_t s_armed;        /* 0 = waiting, 1 = marker printed, 2 = live */
static uint32_t s_last_cpu_pct; /* last census window's real compute share */

/* ===== App-facing accessors (OWF power app, 2026-08-07) ====================
 * Everything below was validated on hardware by the PWR census before being
 * exposed here — see the pwr1/pwr2 log analysis. */

/* Real CPU usage of this core, 0-100: last census window's compute share
 * (loop body minus sleep minus display-wait). */
int pwr_cpu_pct(void) { return (int)s_last_cpu_pct; }

/* SoC die temperature in deci-degC: hottest currently-valid TSENS channel
 * (the die's hot spot is the number that matters). -9999 if none valid. */
int pwr_soc_temp_dc(void)
{
    if (!(mmio_read(TSENS_SROT_BASE + TSENS_SROT_CTRL) & 1u)) return -9999;
    if (s_ts_mode == 0xFFu) tsens_calibrate();
    int best = -9999;
    for (unsigned n = 0; n < TSENS_NSENSORS; n++) {
        int valid, v = tsens_read_dc(n, &valid);
        if (valid && v > best) best = v;
    }
    return best;
}

/* Live CPU clock in MHz, decoded from the APCS RCG + HF PLL.
 * FIXED 2026-08-07: the first decode assumed the mux was always on the PLL —
 * it is not. src 4 = GPLL0 800 MHz (the bootloader's parking spot), src 5 =
 * the HF PLL, and the div field is half-integer (freq = parent*2/(div+1)). */
int pwr_cpu_mhz(void)
{
    uint32_t cfg = mmio_read(APCS_C1_RCG_CFG);
    uint32_t src = (cfg >> 8) & 7u, div = cfg & 0x1Fu;
    uint32_t parent_khz;
    if (src == 5u)      parent_khz = (mmio_read(APCS_PLL_L) & 0x3FFu) * 19200u;
    else if (src == 4u) parent_khz = 800000u;
    else                return 19;             /* parked on XO */
    if (div == 0u) div = 1u;
    return (int)(parent_khz * 2u / (div + 1u) / 1000u);
}

/* Set the CPU clock. Kernel-verbatim sequence (clk-cpu-sdm.c set_rate path):
 *   1. park the mux on the safe source, GPLL0/1 = 800 MHz (cpucc_notifier_cb
 *      PRE_RATE_CHANGE) — the core keeps running while the PLL is dead;
 *   2. reprogram the HF PLL: drop OUTCTRL|RESET_N|BYPASSNL, write L (M=0 N=1,
 *      clk_pll_hf_set_rate), then the sr2 enable: BYPASSNL, 10us, RESET_N,
 *      50us, poll STATUS bit16 lock, OUTCTRL (clk_pll_sr2_enable);
 *   3. switch the mux to the PLL (src 5, div 1).
 * SPM force-event (SPM+0x50 bit4) is held while the PLL is off, mirroring
 * spm_event() in clk-pll.c, so a low-power state can't sample a dead PLL.
 * Requested MHz snaps DOWN to the L grid (19.2 MHz). 800 and 400 come from
 * GPLL0 dividers — no PLL involved. HARD CAP 1306 MHz: everything <= 1305.6M
 * is vdd corner 1 in the DT fmax table (same rail as boot); higher needs a
 * CPR rail raise we don't have. Returns the resulting MHz, <0 on failure. */
int cpu_clk_set_mhz(int mhz)
{
    if (mhz > 1306) mhz = 1306;
    if (mhz < 100)  mhz = 100;

    /* 1: park on GPLL0 (src4). Half-int div: 1 -> /1 = 800, 2 -> /1.5 = 533
     * (the kernel's CCI rate), 3 -> /2 = 400. PLL targets park at /1. */
    uint32_t park_div = 1u, park_mhz = 800u;
    if (mhz <= 800) {
        if (mhz < 480)      { park_div = 3u; park_mhz = 400u; }
        else if (mhz < 700) { park_div = 2u; park_mhz = 533u; }
    }
    mmio_write(APCS_C1_RCG_CFG, (4u << 8) | park_div);
    mmio_write(APCS_C1_RCG_CMD, mmio_read(APCS_C1_RCG_CMD) | 1u);
    for (int i = 0; i < 500 && (mmio_read(APCS_C1_RCG_CMD) & 1u); i++) { }
    if (mmio_read(APCS_C1_RCG_CMD) & 1u) return -1;   /* mux update stuck */

    if (mhz <= 800) {                 /* GPLL0 covers it — PLL left off/idle */
        bdiag_puts("cpuclk: GPLL0 src, mhz="); bdiag_putdec(park_mhz);
        bdiag_puts("\n");
        return (int)park_mhz;
    }

    /* 2: reprogram the HF PLL while nothing consumes it. */
    uint32_t l = (uint32_t)mhz * 10u / 192u;          /* snap down to L grid */
    mmio_write(APCS_SPM_BASE + APCS_SPM_EVENT,
               mmio_read(APCS_SPM_BASE + APCS_SPM_EVENT) | (1u << 4));
    mmio_write(APCS_PLL_MODE, mmio_read(APCS_PLL_MODE) & ~7u);  /* off */
    mmio_write(APCS_PLL_L, (mmio_read(APCS_PLL_L) & ~0x3FFu) | l);
    mmio_write(APCS_PLL_M, mmio_read(APCS_PLL_M) & ~0x7FFFFu);
    mmio_write(APCS_PLL_N, (mmio_read(APCS_PLL_N) & ~0x7FFFFu) | 1u);
    mmio_write(APCS_PLL_MODE, mmio_read(APCS_PLL_MODE) | 2u);   /* BYPASSNL */
    { uint32_t t0 = timer_ms(); while (timer_ms() - t0 < 2u) { } }  /* >10us */
    mmio_write(APCS_PLL_MODE, mmio_read(APCS_PLL_MODE) | 4u);   /* RESET_N */
    { uint32_t t0 = timer_ms(); while (timer_ms() - t0 < 2u) { } }  /* >50us */
    uint32_t t0 = timer_ms();
    while (!(mmio_read(APCS_PLL_STATUS) & (1u << 16))) {
        if (timer_ms() - t0 > 10u) {              /* no lock: stay on GPLL0 */
            con_puts("cpuclk: PLL LOCK TIMEOUT, staying at 800\n");
            return -2;
        }
    }
    mmio_write(APCS_PLL_MODE, mmio_read(APCS_PLL_MODE) | 1u);   /* OUTCTRL */
    mmio_write(APCS_SPM_BASE + APCS_SPM_EVENT,
               mmio_read(APCS_SPM_BASE + APCS_SPM_EVENT) & ~(1u << 4));

    /* 3: switch the core onto the PLL. */
    mmio_write(APCS_C1_RCG_CFG, (5u << 8) | 1u);
    mmio_write(APCS_C1_RCG_CMD, mmio_read(APCS_C1_RCG_CMD) | 1u);
    for (int i = 0; i < 500 && (mmio_read(APCS_C1_RCG_CMD) & 1u); i++) { }

    int out = (int)(l * 192u / 10u);   /* L * 19.2 MHz */
    bdiag_puts("cpuclk: PLL L="); bdiag_putdec(l);
    bdiag_puts(" mhz="); bdiag_putdec((uint32_t)out); bdiag_puts("\n");
    return out;
}

void pwr_diag_poll(uint32_t body_ms)
{
    uint32_t t = timer_ms();
    s_busy_ms += body_ms;
    s_loops++;                  /* one call per app-loop iteration */
    if (!s_win_start) { s_win_start = t; return; }
    if (t < PWR_FIRST_MS) return;

    /* Announce the very first FG access one loop iteration ahead, so the
     * marker is flushed (con_flush runs every loop) before the read happens.
     * If the FG read hard-resets the watch, the log ends exactly here. */
    if (s_armed == 0u) {
        bdiag_puts("pwr: arming first FG read (SPMI sid0 0x4009)\n");
        s_armed = 1u;
        return;
    }

    if ((uint32_t)(t - s_win_start) < PWR_PERIOD_MS && s_armed == 2u)
        return;

    /* ---- fuel gauge + charger (SPMI) ---- */
    int soc  = fg_batt_percent();
    int mv   = fg_batt_mv();
    int ma   = fg_batt_ma();
    int tdc  = fg_batt_temp_dc();
    uint8_t chg_raw = 0xFF, usb_raw = 0xFF;
    int chg_ok = spmi_read8(0u, 0x1006u, &chg_raw);   /* BATTERY_CHARGER_STATUS_1 */
    int usb_ok = spmi_read8(0u, 0x1310u, &usb_raw);   /* USBIN INT_RT_STS */
    if (s_armed == 1u) {
        bdiag_puts("pwr: first FG read survived\n");
        s_armed = 2u;
    }

    /* ---- one PWR line ---- */
    con_puts("PWR t=");      con_putdec(t);
    con_puts(" soc=");       con_putdec((uint32_t)soc);        /* -1 = err */
    con_puts(" vb=");        con_putdec((uint32_t)mv);         /* mV */
    con_puts(" ib=");
    if (ma == -32768) { con_puts("ERR"); }
    else {
        if (ma < 0) { con_puts("-"); con_putdec((uint32_t)-ma); }
        else con_putdec((uint32_t)ma);
    }
    /* power draw: mW = mV * mA / 1000 (+ = discharging, FG sign convention) */
    con_puts(" p_mw=");
    if (mv > 0 && ma != -32768) {
        int32_t mw = (int32_t)((int64_t)mv * ma / 1000);
        if (mw < 0) { con_puts("-"); con_putdec((uint32_t)-mw); }
        else con_putdec((uint32_t)mw);
    } else con_puts("ERR");
    con_puts(" tbat=");      con_putdec((uint32_t)tdc);        /* deci-C */
    con_puts(" chg=");       con_puthex(chg_ok < 0 ? 0xEEEEEEEEu : chg_raw);
    con_puts(" usb=");       con_puthex(usb_ok < 0 ? 0xEEEEEEEEu : usb_raw);

    /* ---- TSENS: all sensors, deci-degC, X = invalid/parked ---- */
    con_puts(" tsens=");
    if (mmio_read(TSENS_SROT_BASE + TSENS_SROT_CTRL) & 1u) {
        if (s_ts_mode == 0xFFu) tsens_calibrate();
        for (unsigned n = 0; n < TSENS_NSENSORS; n++) {
            int valid, v = tsens_read_dc(n, &valid);
            if (n) con_puts(",");
            if (!valid) con_puts("X");
            else {
                if (v < 0) { con_puts("-"); v = -v; }
                con_putdec((uint32_t)v);
            }
        }
    } else con_puts("OFF");

    /* ---- CPU clock: raw RCG CFG + PLL L, plus a best-effort MHz decode ---- */
    {
        uint32_t cfg = mmio_read(APCS_C1_RCG_CFG);
        uint32_t l   = mmio_read(APCS_PLL_L) & 0xFFu;
        con_puts(" cpucfg=");    con_puthex(cfg);
        con_puts(" pllL=");      con_putdec(l);
        con_puts(" cpu_mhz=");   con_putdec((uint32_t)pwr_cpu_mhz());
    }

    /* ---- CPU load, SPLIT (pwr1's flat "load=80%" was misleading: the loop
     * body contains OWF's own pacing delay() AND fb_kick's ~12.6 ms blocking
     * wait per frame — neither is CPU work). Three shares of wall time:
     *   cpu  = body time actually computing (body - sleep - display wait)
     *   disp = blocked in fb_kick waiting on the MDP/DSI transfer
     *   slp  = delay() calls inside the body
     * Sleep/wait totals come from monotonic counters (arduino_glue.cpp
     * g_pwr_sleep_ms, fb_splash.c g_fb_wait_us) read as per-window deltas. */
    {
        static uint32_t s_slp0, s_fbw0;
        uint32_t win  = t - s_win_start;
        uint32_t slp  = g_pwr_sleep_ms - s_slp0;
        uint32_t fbw  = (g_fb_wait_us - s_fbw0) / 1000u;
        uint32_t body = s_busy_ms;
        uint32_t cpu  = body > slp + fbw ? body - slp - fbw : 0u;
        s_slp0 = g_pwr_sleep_ms; s_fbw0 = g_fb_wait_us;
        if (win) s_last_cpu_pct = cpu * 100u / win;
        if (win) {
            con_puts(" cpu=");  con_putdec(cpu * 100u / win);
            con_puts("% disp="); con_putdec(fbw * 100u / win);
            con_puts("% slp=");  con_putdec(slp * 100u / win);
            con_puts("%");
        }
        con_puts("\n");

        /* ---- what the cpu% is actually DOING ----------------------------
         * refr  = LVGL refresh cycles this window (incl. ones with nothing
         *         to draw — a high count here with render~0 means the timer
         *         is spinning, not that the UI is expensive)
         * rend  = cycles that really rendered
         * px    = pixels redrawn (compare against 173056 = one full 416x416
         *         screen: px/173056 is "full screens per window")
         * refr_ms / flush_ms = time inside the refresh, and the share of it
         *         spent in the 519 KB D-cache clean
         * Together these split the idle 11-12% into UI work vs cache work vs
         * empty-cycle overhead. */
        {
            static uint32_t r0, d0, u0, f0, p0;
            uint32_t refr = g_lv_refr_n - r0, rend = g_lv_render_n - d0;
            uint32_t rus  = g_lv_refr_us - u0, fus = g_lv_flush_us - f0;
            uint32_t px   = g_lv_px - p0;
            r0 = g_lv_refr_n; d0 = g_lv_render_n;
            u0 = g_lv_refr_us; f0 = g_lv_flush_us; p0 = g_lv_px;

            lvdiag_puts("LV refr=");     lvdiag_putdec(refr);
            lvdiag_puts(" rend=");       lvdiag_putdec(rend);
            lvdiag_puts(" px=");         lvdiag_putdec(px);
            lvdiag_puts(" screens=");    lvdiag_putdec(px / (416u * 416u));
            lvdiag_puts(" refr_ms=");    lvdiag_putdec(rus / 1000u);
            lvdiag_puts(" flush_ms=");   lvdiag_putdec(fus / 1000u);
            if (win) {
                lvdiag_puts(" refr%=");  lvdiag_putdec((rus / 1000u) * 100u / win);
                lvdiag_puts(" flush%="); lvdiag_putdec((fus / 1000u) * 100u / win);
            }
            lvdiag_puts("\n");

            /* Second line: the two costs flush_ms was hiding, plus the input
             * path. cache_ms is real CPU work (D-cache clean of the whole
             * frame, done however small the dirty rect was); the rest of
             * flush_ms is fb_kick blocking on the DSI transfer. touch_n/ms is
             * the LVGL indev read, which runs on its own period regardless of
             * whether anything is drawn — the remaining suspect for the idle
             * 11-12%, now that the LV line has ruled out rendering. */
            static uint32_t c0v, t0v, tn0;
            uint32_t cus = g_fb_cache_us - c0v;
            uint32_t tus = g_touch_us - t0v, tn = g_touch_n - tn0;
            c0v = g_fb_cache_us; t0v = g_touch_us; tn0 = g_touch_n;

            lvdiag_puts("LV2 cache_ms=");  lvdiag_putdec(cus / 1000u);
            lvdiag_puts(" touch_n=");      lvdiag_putdec(tn);
            lvdiag_puts(" touch_ms=");     lvdiag_putdec(tus / 1000u);
            if (win) {
                lvdiag_puts(" cache%=");   lvdiag_putdec((cus / 1000u) * 100u / win);
                lvdiag_puts(" touch%=");   lvdiag_putdec((tus / 1000u) * 100u / win);
            }
            if (tn) {
                lvdiag_puts(" us/touch="); lvdiag_putdec(tus / tn);
            }
            lvdiag_puts("\n");

            /* Third line: PMIC bus traffic and the loop rate that drives it.
             * loops is counted from pwr_diag_poll() itself (called once per
             * app-loop iteration), so spmi_n/loops shows how many PMIC
             * transactions each iteration costs — OWF polls the boot button
             * over SPMI every time round. If spmi% lands near the missing
             * ~11%, that is the answer. */
            static uint64_t sp0;
            static uint32_t sn0;
            uint32_t sn  = g_spmi_n - sn0;
            uint32_t per_ms = timer_freq_hz() / 1000u;
            uint32_t sms = per_ms ? (uint32_t)((g_spmi_ticks - sp0) / per_ms) : 0u;
            uint32_t sus = per_ms ? (uint32_t)(((g_spmi_ticks - sp0) * 1000u)
                                               / per_ms) : 0u;
            sp0 = g_spmi_ticks; sn0 = g_spmi_n;

            lvdiag_puts("LV3 loops=");    lvdiag_putdec(s_loops);
            lvdiag_puts(" spmi_n=");      lvdiag_putdec(sn);
            lvdiag_puts(" spmi_ms=");     lvdiag_putdec(sms);
            if (win) { lvdiag_puts(" spmi%="); lvdiag_putdec(sms * 100u / win); }
            if (sn)  { lvdiag_puts(" us/spmi="); lvdiag_putdec(sus / sn); }
            if (s_loops) {
                lvdiag_puts(" spmi/loop="); lvdiag_putdec(sn / s_loops);
            }
            lvdiag_puts("\n");
            s_loops = 0;
        }
        s_busy_ms = 0;
        s_win_start = t;
    }
}

#else /* other boards: no-op so the shared loop code links everywhere */
void pwr_diag_poll(uint32_t body_ms) { (void)body_ms; }
int  pwr_cpu_pct(void)     { return -1; }
int  pwr_soc_temp_dc(void) { return -9999; }
int  pwr_cpu_mhz(void)     { return -1; }
int  cpu_clk_set_mhz(int mhz) { (void)mhz; return -1; }
#endif /* PLAT_BOARD_FOSSIL_GEN6 */
