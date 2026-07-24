/* ============================================================================
 *  tuya/compat/freertos/FreeRTOS.h - FreeRTOS core types/macros for the T5.
 *
 *  The T5 kernel IS FreeRTOS, but the Arduino-TuyaOpen build does NOT expose the
 *  raw <freertos/FreeRTOS.h> include path - only the tkl_* wrappers (tkl_thread,
 *  tkl_mutex, tkl_system). So this provides the handful of FreeRTOS types/macros the
 *  firmware uses; task.h and semphr.h (siblings) map the calls onto tkl_thread /
 *  tkl_mutex. delay/tick route to tkl_system.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated per-include in the firmware).
 * ========================================================================== */
#pragma once
#include <cstdint>
#include <cstddef>

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_system.h"   // tkl_system_sleep, tkl_system_get_tick_count
}

typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void *   TaskHandle_t;
typedef void *   QueueHandle_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY            ((TickType_t)0xffffffff)
#define configMINIMAL_STACK_SIZE 768
#define tskIDLE_PRIORITY         0
#define portTICK_PERIOD_MS       1
#define pdMS_TO_TICKS(ms)        ((TickType_t)(ms))

/* 1 tick == 1 ms here (the firmware only uses these for coarse delays/timeouts). */
static inline void vTaskDelay(TickType_t ticks) { tkl_system_sleep((unsigned)ticks); }
static inline TickType_t xTaskGetTickCount(void) { return (TickType_t)tkl_system_get_tick_count(); }
