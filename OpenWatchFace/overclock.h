/* ============================================================================
 *  overclock.h — EXPERIMENTAL CPU overclock past the 240 MHz stock ceiling.
 *
 *  ##########################################################################
 *  #   WARNING.  This may brick your hardware. The FIRST flash with         #
 *  #   OVERCLOCK_ENABLE=1 may hang or scramble flash/NVS. RECOVERY:         #
 *  #    1) set OVERCLOCK_ENABLE back to 0 below,                            #
 *  #    2) hold BOOT, tap RST (ROM download mode ignores the app),          #
 *  #    3) re-flash. A cold power-cycle also restores stock clocks.         #
 *  #   Not a brick — but treat every bump as "might need a reflash".        #
 *  ##########################################################################
 *
 *  WHAT IT DOES ("small monitored overshoot"): raises the BBPLL so the
 *  CPU exceeds 240 MHz. Memory is NOT pre-divided, so flash/PSRAM ride up with
 *  the PLL (~+8% at 260 MHz). A compute + PSRAM canary runs right after the bump;
 *  on any miscompare it AUTO-REVERTS to stock 480/240 and flags it.
 *
 *  HOW (all via IDF's own inline clk_ll_* primitives, so the register recipe is
 *  exact, not reconstructed):
 *    target CPU -> PLL = CPU*2 (we keep the /2 CPU divider).
 *    BBPLL freq = 40 * (div7_0 + 4)  ->  div7_0 = PLL/40 - 4
 *       260 -> PLL 520 -> div7_0 9 ;  280 -> 560 -> 10 ;  300 -> 600 -> 11
 *    Override ONLY the BBPLL div7_0 via a ROM regi2c write (the bootloader already
 *    set every other field for 480). The ENTIRE cache-off window runs from IRAM.
 *
 *  Higher clock wants more core voltage, so we force dig_dbias to OVERCLOCK_MV
 *  for the duration (overrides the undervolt table in core_voltage.h).
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "core_voltage.h"   // core_selftest_compute, core_set_dig_dbias, dbias codes
#include "clocks.h"         // g_pll_override_mhz (truth for the on-screen readout)

/* === the knobs ============================================================ */
#define OVERCLOCK_ENABLE       0      // 0 = no overclock at all; 1 = show the "Overclock" button in Settings>Power.
                                      // OC is BUTTON-triggered (never at boot), so the watch always boots stock
                                      // with working USB -> always flashable. If a value kills USB (OC raises the
                                      // PLL, which moves the USB 48 MHz clock off-spec; ~300 MHz breaks it,
                                      // ~260 survives), just REBOOT to get USB back. No ROM download mode needed.
#define OVERCLOCK_TARGET_MHZ   260    // desired CPU MHz. ANY value works — it snaps to the nearest 20 MHz
                                      // step (CPU = PLL/2) and the Power screen reports the REAL result.
                                      // Memory rides up with it; the canary reverts if it can't keep up.
                                      // 260->PLL520, 280->560, 300->600. Floor 240, ceiling ~380.
#define OVERCLOCK_MV           1250   // core voltage held during OC, in mV (tune it!). 1250 = stock-240 voltage.
                                      // Higher isn't automatically more stable — sweep this like the undervolt table.
                                      // NOTE: testing 280 @ the SAME 1160 mV that 300 runs stable at — a higher
                                      // clock can't need LESS voltage, so if 280 fails the canary here it was an
                                      // under-volt last time, not real instability. Watch the [oc] serial line.

/* Published state (read by the Power screen). Always defined so callers compile
 * regardless of the flag. */
static uint16_t s_oc_cpu_mhz = 0;     // actual CPU MHz after a successful bump (0 = stock/off)
static bool     s_oc_failed  = false; // a bump was tried but the canary failed -> reverted

#if OVERCLOCK_ENABLE
/* Heavy/private SDK headers are pulled in ONLY when the feature is enabled, so
 * the default (disabled) build stays identical to the known-good baseline. */
#include "hal/clk_tree_ll.h"          // clk_ll_* inline primitives + SYSTEM_* regs + REG_SET_FIELD
#include "soc/regi2c_bbpll.h"         // I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DIV_7_0
#include "esp_rom_regi2c.h"           // esp_rom_regi2c_write — ROM-resident, SAFE with cache off
#include "esp_private/cache_utils.h"  // spi_flash_disable/enable_interrupts_caches_and_other_cpu
#include "esp_rom_sys.h"              // esp_rom_delay_us
#include "esp_heap_caps.h"            // heap_caps_malloc (PSRAM canary)
extern "C" void ets_update_cpu_frequency(uint32_t ticks_per_us);  // keep delay()/LVGL timing correct

/* Persistent stage marker in RTC slow memory — survives a RESET (not a full
 * power-cycle). If the switch hangs hard, tap RST (keep power): the next boot
 * reads this, reports WHERE it died, and boots stock so the device is usable. */
RTC_NOINIT_ATTR static uint32_t s_oc_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_oc_rtc_stage;
#define OC_RTC_MAGIC   0xC10C0CEAu
#define OC_STAGE_DONE  100
static uint8_t s_oc_died_stage = 0;   // >0 = a prior attempt hung here (shown on Power screen)

/* The PLL retune. IRAM + cache-off-safe: only inline clk_ll_*, MMIO writes, and
 * ROM calls (esp_rom_regi2c_write / esp_rom_delay_us). pll_mhz multiple of 40. */
static void IRAM_ATTR overclock_switch_pll(uint16_t pll_mhz) {
  clk_ll_cpu_set_src(SOC_CPU_CLK_SRC_XTAL);                 // park CPU on the 40 MHz XTAL
  // Override ONLY the BBPLL feedback divider (bootloader set every other field
  // for 480; IDF uses identical charge-pump/current across 320-480). ROM write =
  // safe with the cache off.
  esp_rom_regi2c_write(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DIV_7_0,
                       (uint8_t)((pll_mhz / 40) - 4));      // retune VCO to pll_mhz
  REG_SET_FIELD(SYSTEM_CPU_PER_CONF_REG, SYSTEM_PLL_FREQ_SEL, 1);  // dividers stay "480-class"
  esp_rom_delay_us(100);                                    // let the PLL relock
  clk_ll_cpu_set_freq_mhz_from_pll(240);                    // CPUPERIOD_SEL=2 -> CPU = PLL/2
  clk_ll_cpu_set_src(SOC_CPU_CLK_SRC_PLL);                  // back onto the (now faster) PLL
}

/* THE cache-off critical section — the WHOLE thing must be IRAM, because every
 * instruction here executes with the flash cache disabled. (My earlier bug: this
 * code lived in flash via overclock_apply(), so the CPU crashed trying to fetch
 * it after the cache went off. That was the second black screen.) */
static void IRAM_ATTR overclock_do_switch(uint16_t pll_mhz, uint16_t cpu_mhz) {
  spi_flash_disable_interrupts_caches_and_other_cpu();
  s_oc_rtc_stage = 3;                                       // in cache-off window, about to switch
  overclock_switch_pll(pll_mhz);
  s_oc_rtc_stage = 4;                                       // switch returned, PLL relocked
  ets_update_cpu_frequency(cpu_mhz);                        // fix tick math for delay()/LVGL
  spi_flash_enable_interrupts_caches_and_other_cpu();
}

/* ---- canary buffers / golden ---- */
static uint32_t  s_oc_golden = 0;
static uint32_t *s_oc_psram  = nullptr;
static const uint32_t OC_PSRAM_WORDS = 16384;               // 64 KB of PSRAM exercised

static bool oc_psram_ok(void) {
  if (!s_oc_psram) return true;                              // no PSRAM -> nothing to check
  for (uint32_t i = 0; i < OC_PSRAM_WORDS; i++) s_oc_psram[i] = i * 2654435761u;
  for (uint32_t i = 0; i < OC_PSRAM_WORDS; i++)
    if (s_oc_psram[i] != i * 2654435761u) return false;     // a bad read = PSRAM unstable at this clock
  return true;
}

/* Called at BOOT — does NOT overclock. Booting always stays at stock 240 MHz, so
 * USB always enumerates and the device is ALWAYS flashable normally (no ROM
 * download mode). If a previous button-triggered attempt hung mid-switch, the RTC
 * stage marker survived the reset -> report where it died, then mark idle. */
static inline void overclock_check_recovery(void) {
  if (s_oc_rtc_magic == OC_RTC_MAGIC && s_oc_rtc_stage != OC_STAGE_DONE)
    s_oc_died_stage = (uint8_t)s_oc_rtc_stage;              // a prior attempt hung here
  s_oc_rtc_magic = OC_RTC_MAGIC;
  s_oc_rtc_stage = OC_STAGE_DONE;                           // idle/clean
}

/* Apply the overclock NOW — call from the Power-menu button (NOT at boot). Snaps
 * the target, bumps, verifies with the canary, reverts on failure. If a value
 * kills USB (e.g. 300 MHz), just REBOOT: boot is stock, so USB returns and you
 * can reflash normally. Stage markers (1,2 -> flash; 3,4 -> cache-off; 5 ->
 * survived; DONE -> finished) localize a hard hang on the next boot. */
static inline void overclock_apply(void) {
  if (s_oc_cpu_mhz) return;                                 // already overclocked this session
  s_oc_failed = false;
  s_oc_died_stage = 0;
  s_oc_rtc_magic = OC_RTC_MAGIC;
  s_oc_rtc_stage = 1;                                        // entered (still in flash, cache on)

  // Snap the requested target to an achievable step and use the REAL result for
  // everything (the switch, the canary report, the readout). CPU = PLL/2 and
  // PLL = 40*(div7_0+4), so the CPU lands on a 20 MHz grid. Any value is fine.
  int div7 = ((int)OVERCLOCK_TARGET_MHZ + 10) / 20 - 4;     // div7_0 for the rounded target
  if (div7 < 8)  div7 = 8;                                   // floor: PLL 480 = stock 240 MHz
  if (div7 > 15) div7 = 15;                                  // ceiling: PLL 760 = 380 MHz (sanity cap)
  const uint16_t pll = (uint16_t)(40 * (div7 + 4));          // actual PLL after snapping
  const uint16_t cpu = (uint16_t)(pll / 2);                  // actual CPU MHz

  if (!s_oc_psram)
    s_oc_psram = (uint32_t *)heap_caps_malloc(OC_PSRAM_WORDS * 4, MALLOC_CAP_SPIRAM);
  s_oc_golden = core_selftest_compute();                    // trusted result at stock 240 MHz
  core_set_dig_dbias(core_mv_to_dbias(OVERCLOCK_MV));        // OC core voltage (tunable, mV)
  s_oc_rtc_stage = 2;                                        // about to enter the cache-off switch

  overclock_do_switch(pll, cpu);
  s_oc_rtc_stage = 5;                                        // survived the switch; cache back on

  // Split the canary so the log says WHICH check failed — compute (ALU/SRAM) vs PSRAM.
  // This makes a retest diagnostic: a clean "OC OK" means the core is stable at this
  // MHz+mV (so any later crash is something else — radio/USB/APB, not core stability),
  // while a specific "compute FAILED" / "PSRAM FAILED" means raise the voltage. Without
  // this the apply was silent and a canary revert looked identical to a real crash.
  bool compute_ok = (core_selftest_compute() == s_oc_golden);
  bool psram_ok   = oc_psram_ok();
  if (compute_ok && psram_ok) {
    s_oc_cpu_mhz       = cpu;
    g_pll_override_mhz = pll;                                // readout shows the real frequency
    USBSerial.printf("[oc] OK: %u MHz (PLL %u) @ %u mV — core stable; canary passed\n",
                     (unsigned)cpu, (unsigned)pll, (unsigned)OVERCLOCK_MV);
  } else {
    overclock_do_switch(480, 240);                          // canary failed -> revert to stock
    core_set_dig_dbias(CORE_DBIAS_STOCK);
    s_oc_failed = true;
    USBSerial.printf("[oc] REVERTED to 240: %u MHz @ %u mV failed canary (compute %s, PSRAM %s) "
                     "-> raise OVERCLOCK_MV and retry\n",
                     (unsigned)cpu, (unsigned)OVERCLOCK_MV,
                     compute_ok ? "ok" : "FAILED", psram_ok ? "ok" : "FAILED");
  }
  s_oc_rtc_stage = OC_STAGE_DONE;                            // completed cleanly
}

#else  /* OVERCLOCK_ENABLE == 0 */
static inline void overclock_check_recovery(void) {}        // no-ops; default build is unchanged
static inline void overclock_apply(void) {}
#endif
