/* compat/freertos/FreeRTOS.h — minimal FreeRTOS shim for the Linux/Maix port.
 * Net + BLE tasks are deferred, so the firmware is effectively single-threaded
 * here; mutexes are no-ops and task/delay map to the Arduino shim. */
#pragma once
#include <cstdint>
#include <cstddef>

typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void *   TaskHandle_t;
typedef void *   QueueHandle_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY        ((TickType_t)0xffffffff)
#define configMINIMAL_STACK_SIZE 768
#define tskIDLE_PRIORITY     0
#define portTICK_PERIOD_MS   1
#define pdMS_TO_TICKS(ms)    ((TickType_t)(ms))

static inline void vTaskDelay(TickType_t ticks) { extern void delay(uint32_t); delay((uint32_t)ticks); }
static inline TickType_t xTaskGetTickCount(void) { extern uint32_t millis(void); return millis(); }
