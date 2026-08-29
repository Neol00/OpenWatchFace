/* esp_system.h — reset reason + restart, on the fossil-port runtime. */
#pragma once
#include <cstdint>
#include "owf_meminfo.h"
extern "C" { void reboot_now(void); }   /* reboot_msm.c */

typedef enum {
    ESP_RST_UNKNOWN = 0, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
    ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
    ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO,
} esp_reset_reason_t;

/* No deep-sleep path yet -> always report a cold power-on boot. */
static inline esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
static inline void     esp_restart(void)                    { reboot_now(); for(;;){} }
static inline uint32_t esp_get_free_heap_size(void)         { return owf_mem_free_heap(); }
/* nano-malloc keeps no low-water mark; the FreeRTOS pool does, and it is the
 * one that actually runs out first, so report its worst case. */
static inline uint32_t esp_get_minimum_free_heap_size(void) { owf_meminfo_t m; owf_meminfo(&m); return m.rtos_min_free; }
