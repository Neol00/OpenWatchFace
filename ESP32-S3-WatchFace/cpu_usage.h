/* ============================================================================
 *  cpu_usage.h — lightweight, power-safe per-core CPU usage estimate.
 *
 *  The Arduino-ESP32 build does NOT enable configGENERATE_RUN_TIME_STATS, so the
 *  usual vTaskGetRunTimeStats() path isn't available. Instead we hang a FreeRTOS
 *  IDLE HOOK on each core: it fires while that core's idle task runs and we just
 *  bump a counter. Crucially the hook RETURNS TRUE, so it's called at most once
 *  per FreeRTOS tick (~1 ms) and the core is still free to enter its low-power
 *  WFI between ticks — measuring usage this way costs no extra power (important on
 *  a battery watch; a busy-spin counter would peg both cores at 100%).
 *
 *  Usage = 100 * (elapsed_ticks - idle_ticks_for_that_core) / elapsed_ticks.
 *  A fully idle core logs ~1 idle-hook call per tick, so its usage reads ~0%; a
 *  core doing real work runs its idle task less, so fewer idle calls -> higher %.
 *
 *  Call cpu_usage_init() ONCE after the scheduler is up (e.g. end of setup()).
 *  Then cpu_usage_sample() periodically (it latches a 0..100 % per core since the
 *  previous sample) and read it back with cpu_usage_pct(core).
 *
 *  Header-only, compiled into the .ino TU. No dependencies beyond ESP-IDF/FreeRTOS.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"

/* Free-running idle-hook counters, one per core. volatile: written from the idle
 * hook (which runs in the idle task context on each core), read from the UI. A
 * uint32_t increment is atomic on the S3, so no lock is needed for a coherent read. */
static volatile uint32_t s_cpu_idle_cnt[2] = {0, 0};

/* Last sample's idle-count + tick snapshot, and the latched usage (0..100). */
static uint32_t s_cpu_idle_prev[2] = {0, 0};
static uint32_t s_cpu_tick_prev    = 0;
static uint8_t  s_cpu_pct[2]       = {0, 0};
static bool     s_cpu_inited       = false;

static bool cpu_idle_hook_core0(void) { s_cpu_idle_cnt[0]++; return true; }
static bool cpu_idle_hook_core1(void) { s_cpu_idle_cnt[1]++; return true; }

/* Register the per-core idle hooks. Safe to call once the scheduler is running. */
static void cpu_usage_init(void) {
  if (s_cpu_inited) return;
  esp_register_freertos_idle_hook_for_cpu(cpu_idle_hook_core0, 0);
  esp_register_freertos_idle_hook_for_cpu(cpu_idle_hook_core1, 1);
  s_cpu_idle_prev[0] = s_cpu_idle_cnt[0];
  s_cpu_idle_prev[1] = s_cpu_idle_cnt[1];
  s_cpu_tick_prev    = xTaskGetTickCount();
  s_cpu_inited       = true;
}

/* Latch each core's busy% over the window since the previous call. Cheap; call it
 * from a UI timer. A window of at least a few hundred ms gives a stable read. */
static void cpu_usage_sample(void) {
  if (!s_cpu_inited) return;
  uint32_t now_tick = xTaskGetTickCount();
  uint32_t elapsed  = now_tick - s_cpu_tick_prev;        // wrap-safe (unsigned)
  if (elapsed == 0) return;                              // no time passed yet

  for (uint8_t c = 0; c < 2; c++) {
    uint32_t cur  = s_cpu_idle_cnt[c];
    uint32_t idle = cur - s_cpu_idle_prev[c];            // idle hooks this window
    s_cpu_idle_prev[c] = cur;
    // An idle core fires ~1 idle hook per elapsed tick. Clamp: idle can momentarily
    // exceed elapsed (hook vs tick race), and busy can't be negative.
    if (idle > elapsed) idle = elapsed;
    uint32_t busy = elapsed - idle;
    s_cpu_pct[c] = (uint8_t)((busy * 100u) / elapsed);
  }
  s_cpu_tick_prev = now_tick;
}

/* Reset the measurement window to "now" WITHOUT latching a percentage. Call this
 * when a viewer (the Power app) opens, so its first sample a moment later reflects
 * only recent activity rather than the whole idle period since boot/last visit. */
static void cpu_usage_reset_window(void) {
  if (!s_cpu_inited) return;
  s_cpu_idle_prev[0] = s_cpu_idle_cnt[0];
  s_cpu_idle_prev[1] = s_cpu_idle_cnt[1];
  s_cpu_tick_prev    = xTaskGetTickCount();
}

/* Last latched busy percentage (0..100) for core 0 or 1. */
static uint8_t cpu_usage_pct(uint8_t core) {
  return (core < 2) ? s_cpu_pct[core] : 0;
}
