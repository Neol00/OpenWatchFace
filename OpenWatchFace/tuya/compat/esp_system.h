/* ============================================================================
 *  tuya/compat/esp_system.h - reset/restart + heap-stat API, backed by the REAL
 *  T5 system calls (tkl_system_reset / tkl_system_get_reset_reason).
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino routes <esp_system.h>
 *  here). esp_restart() reboots for real; esp_reset_reason() maps the T5 reset
 *  reason onto the ESP enum the firmware's boot-cause banner reads.
 * ========================================================================== */
#pragma once
#include <cstdint>

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_system.h"
#include "tkl_memory.h"   // tkl_system_get_free_heap_size
}

typedef enum {
    ESP_RST_UNKNOWN = 0, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,
    ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT,
    ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO,
} esp_reset_reason_t;

/* Map the T5 reset reason onto the ESP enum the firmware branches on. The T5
 * TUYA_RESET_REASON_E names vary; cover the ones the banner cares about (deep-sleep
 * wake vs power-on vs watchdog) and fall back to POWERON. */
static inline esp_reset_reason_t esp_reset_reason(void) {
    char *desc = nullptr;
    TUYA_RESET_REASON_E r = tkl_system_get_reset_reason(&desc);
    switch ((int)r) {
        case TUYA_RESET_REASON_DEEPSLEEP: return ESP_RST_DEEPSLEEP;
        case TUYA_RESET_REASON_HW_WDOG:
        case TUYA_RESET_REASON_SW_WDOG:   return ESP_RST_TASK_WDT;
        case TUYA_RESET_REASON_SOFTWARE:  return ESP_RST_SW;
        case TUYA_RESET_REASON_BROWNOUT:  return ESP_RST_BROWNOUT;
        case TUYA_RESET_REASON_EXTERNAL:  return ESP_RST_EXT;
        case TUYA_RESET_REASON_FAULT:
        case TUYA_RESET_REASON_CRASH:
        case TUYA_RESET_REASON_FATAL:     return ESP_RST_PANIC;
        case TUYA_RESET_REASON_POWERON:   return ESP_RST_POWERON;
        default:                          return ESP_RST_POWERON;
    }
}

static inline uint32_t esp_get_free_heap_size(void)         { return (uint32_t)tkl_system_get_free_heap_size(); }
static inline uint32_t esp_get_minimum_free_heap_size(void) { return (uint32_t)tkl_system_get_free_heap_size(); }
static inline void     esp_restart(void)                    { tkl_system_reset(); }
