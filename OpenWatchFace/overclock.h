/* ============================================================================
 *  overclock.h — EXPERIMENTAL CPU overclock past the stock ceiling.
 *                S3: past 240 MHz (PLL/2).   C6: past 160 MHz (SPLL/3).
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
 *  WHAT IT DOES ("small monitored overshoot"): raises the BBPLL/SPLL so the
 *  CPU exceeds its stock ceiling. Memory is NOT pre-divided, so flash/PSRAM ride
 *  up with the PLL. A compute (+PSRAM where present) canary runs right after the
 *  bump; on any miscompare it AUTO-REVERTS to stock and flags it.
 *
 *  HOW (all via IDF's own inline clk_ll_* primitives + a ROM regi2c write, so the
 *  register recipe is exact, not reconstructed). BOTH chips' BBPLL uses the SAME
 *  feedback-divider formula  PLL = 40 * (div7_0 + 4)  ->  div7_0 = PLL/40 - 4:
 *    S3:  CPU = PLL/2.  260 -> PLL 520 -> div7_0 9 ; 280 -> 560 -> 10 ; 300 -> 11
 *    C6:  CPU = SPLL/3. 173 -> SPLL 520 -> 9 ; 187 -> 560 -> 10 ; 200 -> 600 -> 11
 *  We override ONLY the BBPLL div7_0 via a ROM regi2c write (the bootloader already
 *  set every other field for 480) and keep the chip's stock CPU divider. The whole
 *  cache-off window runs from IRAM.
 *
 *  Per-chip notes:
 *    S3 — single CPUPERIOD_SEL drives CPU *and* APB together (no separate APB
 *         divider), so APB rides above 80 MHz and WiFi/BLE drop. Recoverable: boot
 *         is always stock, so a reboot restores USB + radios.
 *    C6 — separate PCR dividers for AHB/APB/MSPI exist, BUT they only step in coarse
 *         ratios (AHB ÷12/24/48; APB ÷1/2/4), so a non-integer SPLL bump can't be
 *         pulled back to exactly 80 MHz — APB/flash still ride up ~8-25% and the
 *         radio is still expected to drop. Single RISC-V core, less voltage margin
 *         than the S3, so the stable window is narrower. The CPU switch uses the
 *         C6's PCR registers (NOT the S3's SYSTEM_CPU_PER_CONF_REG).
 *
 *  Higher clock wants more core voltage, so we force the core dbias to OVERCLOCK_MV
 *  for the duration (overrides the undervolt table in core_voltage.h). The mV->code
 *  conversion is board-aware (S3 regi2c grid vs C6 PMU grid).
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "board.h"          // BOARD_WS_* selection (S3 vs C6 register recipe)
#include "core_voltage.h"   // core_selftest_compute, core_set_dig_dbias, dbias codes
#include "clocks.h"         // g_pll_override_mhz (truth for the on-screen readout)

/* === the knobs ============================================================ */
#define OVERCLOCK_ENABLE       0      // 0 = no overclock at all; 1 = show the "Overclock" button in Settings>Power.
                                      // OC is BUTTON-triggered (never at boot), so the watch always boots stock
                                      // with working USB -> always flashable. If a value kills USB (OC raises the
                                      // PLL, which moves the USB 48 MHz clock off-spec; ~300 MHz breaks it,
                                      // ~260 survives), just REBOOT to get USB back. No ROM download mode needed.
#define OVERCLOCK_TARGET_MHZ   260    // desired CPU MHz. ANY value works — it snaps to an achievable PLL step
                                      // and the Power screen reports the REAL result. Memory rides up with it;
                                      // the canary reverts if it can't keep up. Per chip (PLL = 40*(div7_0+4)):
                                      //   S3 (CPU=PLL/2):  260->PLL520, 280->560, 300->600. Floor 240, cap ~380.
                                      //   C6 (CPU=SPLL/3): 173->SPLL520, 187->560, 200->600. Floor 160, cap ~253.
                                      // Pick a value valid for the board you're flashing; it snaps to the grid.
#define OVERCLOCK_MV           1150   // core voltage held during OC, in mV (tune it!). Board-aware: the mV is
                                      // converted to the right dbias code per chip (S3 regi2c grid / C6 PMU grid).
                                      // S3: 1250 = stock-240 voltage. C6: stock HP-active ≈1100 mV (code 25); the
                                      // C6 grid tops out ~1150 mV (code 27), so a high value clamps to that max.
                                      // Higher isn't automatically more stable — sweep this like the undervolt table.
                                      // A higher clock can't need LESS voltage, so a canary fail at a value that
                                      // worked higher up was an under-volt, not real instability. Watch [oc].

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
 * ROM calls (esp_rom_regi2c_write / esp_rom_delay_us). pll_mhz multiple of 40.
 * The BBPLL feedback-divider write is IDENTICAL on both chips (same regi2c block,
 * same PLL = 40*(div7_0+4) formula); only the CPU-divider re-lock differs. */
#if defined(BOARD_WS_C6_TOUCH_LCD_147)
/* ---- ESP32-C6 ---- CPU = SPLL/3 (the HS path is a FIXED /3 then a configurable
 * /{1,2,4}; we keep /1 so CPU = SPLL/3). No SYSTEM_CPU_PER_CONF_REG on the C6 —
 * the source mux + divider live in the PCR block (clk_ll_cpu_set_src writes
 * PCR.sysclk_conf.soc_clk_sel; the HS divider is PCR.cpu_freq_conf). */
static void IRAM_ATTR overclock_switch_pll(uint16_t pll_mhz) {
  clk_ll_cpu_set_src(SOC_CPU_CLK_SRC_XTAL);                 // park CPU on the 40 MHz XTAL
  // Override ONLY the SPLL feedback divider. The IDF hard-codes div7_0=8 (480) and
  // ASSERTs "fixed 480" — but that's a software policy, not a hardware lock: the
  // analog OC_DIV_7_0 field is writable exactly like the S3's. ROM write = cache-safe.
  esp_rom_regi2c_write(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DIV_7_0,
                       (uint8_t)((pll_mhz / 40) - 4));      // retune VCO to pll_mhz
  esp_rom_delay_us(100);                                    // let the SPLL relock
  clk_ll_cpu_set_hs_divider(3);                             // CPU = SPLL/3 (cpu_hs_div_num=0)
  clk_ll_cpu_set_src(SOC_CPU_CLK_SRC_PLL);                  // back onto the (now faster) SPLL
}
#else
/* ---- ESP32-S3 ---- CPU = PLL/2. */
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
#endif

/* Per-chip clock constants used by the snap math + revert below. CPU = PLL / div,
 * PLL = 40*(div7_0+4). OC_DIV7_MAX caps the SPLL high enough to be useful but below
 * the point where the relock/flash usually give out (a sanity rail, not a promise).
 *   S3: CPU=PLL/2, stock 480/240.  C6: CPU=SPLL/3, stock 480/160. */
#if defined(BOARD_WS_C6_TOUCH_LCD_147)
  #define OC_CPU_PLL_DIV   3                 // CPU = SPLL/3
  #define OC_DIV7_MAX      18                // SPLL 760 -> 253 MHz hard cap (canary still arbitrates)
#else
  #define OC_CPU_PLL_DIV   2                 // CPU = PLL/2
  #define OC_DIV7_MAX      15                // PLL 760 -> 380 MHz hard cap
#endif
#define OC_STOCK_PLL       480               // both chips: stock SPLL/BBPLL is 480
#define OC_STOCK_CPU       (OC_STOCK_PLL / OC_CPU_PLL_DIV)   // S3 240 / C6 160

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
  // everything (the switch, the canary report, the readout). PLL = 40*(div7_0+4),
  // CPU = PLL/OC_CPU_PLL_DIV (S3 /2, C6 /3), so the CPU lands on a fixed grid. Any
  // value is fine. Solve div7_0 from the requested CPU: PLL = CPU*div -> div7_0 =
  // PLL/40 - 4, rounded to the nearest 40 MHz PLL step.
  int target_pll = (int)OVERCLOCK_TARGET_MHZ * OC_CPU_PLL_DIV;
  int div7 = (target_pll + 20) / 40 - 4;                     // div7_0 for the rounded target
  if (div7 < 8)         div7 = 8;                            // floor: PLL 480 = stock (S3 240 / C6 160)
  if (div7 > OC_DIV7_MAX) div7 = OC_DIV7_MAX;                // ceiling: per-chip sanity cap
  const uint16_t pll = (uint16_t)(40 * (div7 + 4));          // actual PLL after snapping
  const uint16_t cpu = (uint16_t)(pll / OC_CPU_PLL_DIV);     // actual CPU MHz

  if (!s_oc_psram)
    s_oc_psram = (uint32_t *)heap_caps_malloc(OC_PSRAM_WORDS * 4, MALLOC_CAP_SPIRAM);
  s_oc_golden = core_selftest_compute();                    // trusted result at stock clock
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
    overclock_do_switch(OC_STOCK_PLL, OC_STOCK_CPU);        // canary failed -> revert to stock
    core_set_dig_dbias(CORE_DBIAS_STOCK);
    s_oc_failed = true;
    USBSerial.printf("[oc] REVERTED to %u: %u MHz @ %u mV failed canary (compute %s, PSRAM %s) "
                     "-> raise OVERCLOCK_MV and retry\n",
                     (unsigned)OC_STOCK_CPU, (unsigned)cpu, (unsigned)OVERCLOCK_MV,
                     compute_ok ? "ok" : "FAILED", psram_ok ? "ok" : "FAILED");
  }
  s_oc_rtc_stage = OC_STAGE_DONE;                            // completed cleanly
}

#else  /* OVERCLOCK_ENABLE == 0 */
static inline void overclock_check_recovery(void) {}        // no-ops; default build is unchanged
static inline void overclock_apply(void) {}
#endif
