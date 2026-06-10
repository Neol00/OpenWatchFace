/* ============================================================================
 *  clocks.h — read-only clock-tree observability (SAFE: no register writes).
 *
 *  Prints/formats the live CPU / PLL / divider / flash state so we can SEE the
 *  actual clocks before and during the BBPLL overclock experiment.
 *
 *  IMPORTANT: after an overclock the hardware's own freq getter UNDER-reports —
 *  the dividers still think they're on a 480 MHz PLL (PLL_FREQ_SEL only encodes
 *  480/320), so rtc_clk_cpu_freq_get_config() would say "240 / PLL 480" even
 *  though the BBPLL is really at 520/560/600. overclock.h therefore publishes the
 *  TRUE PLL frequency into g_pll_override_mhz, and we use it here to show reality.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "soc/rtc.h"          // rtc_clk_cpu_freq_get_config + rtc_cpu_freq_config_t
#include "esp32-hal-cpu.h"    // getApbFrequency()

/* Set by overclock.h to the REAL BBPLL frequency after a bump (0 = stock, use
 * the register-reported value). Lets this read-only module show the truth. */
static int g_pll_override_mhz = 0;

/* Map the CPU clock source enum to a short label for the dump. */
static inline const char *clocks_src_name(soc_cpu_clk_src_t s) {
  switch ((int)s) {
    case SOC_CPU_CLK_SRC_XTAL:    return "XTAL";
    case SOC_CPU_CLK_SRC_PLL:     return "PLL";
    case SOC_CPU_CLK_SRC_RC_FAST: return "RC_FAST";
    default:                      return "?";
  }
}

/* Write a compact one-line clock summary into `buf` for ON-SCREEN display:
 *   "Clk: 260 MHz  PLL 520/2  fl 86"  (CPU, real PLL / CPU divider, flash MHz).
 * Uses g_pll_override_mhz so it reflects the real overclocked frequency. */
static inline void clocks_format(char *buf, size_t n) {
  rtc_cpu_freq_config_t c;
  rtc_clk_cpu_freq_get_config(&c);
  unsigned regpll = (unsigned)c.source_freq_mhz;                 // what the dividers assume (480)
  unsigned pll    = g_pll_override_mhz ? (unsigned)g_pll_override_mhz : regpll;  // the truth
  unsigned cpu    = (c.div ? pll / c.div : (unsigned)c.freq_mhz);
  unsigned fl     = (unsigned)(ESP.getFlashChipSpeed() / 1000000u);
  if (g_pll_override_mhz && regpll)                              // flash tracks the PLL too
    fl = (unsigned)((uint64_t)fl * g_pll_override_mhz / regpll);
  snprintf(buf, n, "Clk: %u MHz  PLL %u/%u  fl %u", cpu, pll, (unsigned)c.div, fl);
}

/* Print one line of the current clock state to USBSerial. `tag` is a label. */
static inline void clocks_dump(const char *tag) {
  char line[80];
  clocks_format(line, sizeof(line));
  USBSerial.printf("[clk %s] %s | APB %u MHz\n",
                   tag ? tag : "", line, (unsigned)(getApbFrequency() / 1000000u));
}
