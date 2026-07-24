/* ============================================================================
 *  tuya/compat/owf_tuya_cpu_stats.h — real per-core CPU usage on the T5 (BK7258).
 *
 *  The firmware's cpu_usage.h was written for ESP-IDF: it hangs a FreeRTOS idle
 *  hook on each core (esp_register_freertos_idle_hook_for_cpu) and counts idle
 *  ticks. On the Tuya build those hook-registration calls are NO-OPs (see
 *  compat/esp_freertos_hooks.h), so the idle counters never move and every core
 *  reads a constant 100% — the bug behind "the per-core graph is always 100%".
 *
 *  The T5's FreeRTOS (SMP, configGENERATE_RUN_TIME_STATS=1, run-time clock =
 *  bk_get_tick()) instead exposes a clean numeric per-core accessor in its kernel
 *  (defined in libos_source.a, declared in the kernel's freertos additions):
 *
 *      configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimeCounterForCore(BaseType_t core);
 *
 *  It returns that core's idle task cumulative run-time counter (same clock as
 *  bk_get_tick()). Busy% over a window is then (elapsed - idleDelta)/elapsed — the
 *  SAME math as cpu_usage.h's MEASURED path, just sourced per-core from the kernel
 *  instead of from ESP idle hooks. We declare the symbol ourselves (the Arduino-
 *  TuyaOpen core doesn't ship the kernel headers, only tkl_*); it links against
 *  libos_source.a. bk_get_tick() is the matching run-time clock (libbk_system.a).
 *
 *  IMPORTANT — core count: the BK7258 is physically 3 cores, but the application
 *  FreeRTOS SMP domain that runs OUR code is 2 (CONFIG_CPU_CNT=2). The 3rd core is
 *  a separate coprocessor (CP) domain reached over mailbox/IPC; it runs vendor
 *  media/DSP firmware, has no idle task in our scheduler, and its load is not
 *  introspectable from here (the only hook, tkl_system_get_cpu_info, is a blocking
 *  5 s IPC round-trip returning a single aggregate ratio — unusable for a UI graph).
 *  So we measure the 2 cores we actually own. OWF_TUYA_CPU_CORES reflects that.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated by cpu_usage.h).
 * ========================================================================== */
#pragma once
#include <stdint.h>

/* Cores in OUR FreeRTOS SMP domain (CONFIG_CPU_CNT). The 3rd BK7258 core is the
 * coprocessor domain and is not counted here (see the header note above). */
#define OWF_TUYA_CPU_CORES 2

extern "C" {
/* T5 FreeRTOS run-time clock (libbk_system.a). Returns a 64-bit tick; FreeRTOS's
 * run-time stats clock (portGET_RUN_TIME_COUNTER_VALUE) IS bk_get_tick(), but the
 * per-task ulRunTimeCounter it accumulates is 32-bit (configRUN_TIME_COUNTER_TYPE =
 * uint32_t). So we read the 64-bit value and truncate to 32-bit ourselves, matching
 * the idle counter's width — a ratio of the two is then dimensionless and wrap-safe
 * over the UI's seconds-scale window. (Declaring the real uint64_t return type is
 * also required for the ARM ABI: a uint32_t decl would mis-read r0:r1.) */
uint64_t bk_get_tick(void);

/* Per-core idle-task cumulative run-time counter (libos_source.a). xCoreID is
 * 0..CONFIG_CPU_CNT-1. Takes the kernel lock internally; cheap and safe to poll. */
uint32_t ulTaskGetIdleRunTimeCounterForCore(int xCoreID);
}

/* 32-bit run-time clock, matching the idle counter width (see note above). */
static inline uint32_t owf_tuya_rt_tick(void) { return (uint32_t)bk_get_tick(); }

/* Latched busy% per core (0..100), updated by owf_tuya_cpu_sample(). */
static uint8_t  s_owf_cpu_pct[OWF_TUYA_CPU_CORES]       = {0, 0};
static uint32_t s_owf_cpu_idle_prev[OWF_TUYA_CPU_CORES] = {0, 0};
static uint32_t s_owf_cpu_t_prev = 0;
static bool     s_owf_cpu_inited = false;

static void owf_tuya_cpu_init(void) {
  if (s_owf_cpu_inited) return;
  for (int c = 0; c < OWF_TUYA_CPU_CORES; c++)
    s_owf_cpu_idle_prev[c] = ulTaskGetIdleRunTimeCounterForCore(c);
  s_owf_cpu_t_prev = owf_tuya_rt_tick();
  s_owf_cpu_inited = true;
}

/* Latch each core's busy% over the window since the previous call. Wrap-safe via
 * unsigned subtraction. A <1-tick window is skipped (keeps the last value). */
static void owf_tuya_cpu_sample(void) {
  if (!s_owf_cpu_inited) { owf_tuya_cpu_init(); return; }
  uint32_t now     = owf_tuya_rt_tick();
  uint32_t elapsed = now - s_owf_cpu_t_prev;          // wrap-safe
  if (elapsed == 0) return;
  for (int c = 0; c < OWF_TUYA_CPU_CORES; c++) {
    uint32_t cur  = ulTaskGetIdleRunTimeCounterForCore(c);
    uint32_t idle = cur - s_owf_cpu_idle_prev[c];     // wrap-safe
    s_owf_cpu_idle_prev[c] = cur;
    if (idle > elapsed) idle = elapsed;               // clock-domain slop -> clamp
    s_owf_cpu_pct[c] = (uint8_t)(((uint64_t)(elapsed - idle) * 100u) / elapsed);
  }
  s_owf_cpu_t_prev = now;
}

/* Reset the window to "now" WITHOUT latching, so the next sample reflects only
 * activity since the reset (called when the Power screen opens). */
static void owf_tuya_cpu_reset_window(void) {
  if (!s_owf_cpu_inited) { owf_tuya_cpu_init(); return; }
  for (int c = 0; c < OWF_TUYA_CPU_CORES; c++)
    s_owf_cpu_idle_prev[c] = ulTaskGetIdleRunTimeCounterForCore(c);
  s_owf_cpu_t_prev = owf_tuya_rt_tick();
}

static uint8_t owf_tuya_cpu_pct(int core) {
  return (core >= 0 && core < OWF_TUYA_CPU_CORES) ? s_owf_cpu_pct[core] : 0;
}
