/* compat/freertos/task.h — FreeRTOS tasks as real (detached) std::threads.
 *
 * The firmware's net task (notif_net.h) drives WiFi + notification fetch; on this
 * port it must actually run for WiFi to connect. Audio tasks are gated off on the
 * Maix board, so in practice only the net task is spawned. Thread-safety relies on
 * the real mutexes in semphr.h. */
#pragma once
#include "FreeRTOS.h"
#include <thread>

typedef void (*TaskFunction_t)(void *);

static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *, uint32_t,
                                     void *param, UBaseType_t, TaskHandle_t *h) {
    try { std::thread(fn, param).detach(); }
    catch (...) { if (h) { *h = nullptr; } return pdFAIL; }
    if (h) { *h = (TaskHandle_t)1; }
    return pdPASS;
}
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t depth,
                                                 void *param, UBaseType_t prio, TaskHandle_t *h, BaseType_t) {
    return xTaskCreate(fn, name, depth, param, prio, h);   // no core affinity on Linux
}
static inline void vTaskDelete(TaskHandle_t) {}            /* tasks here run forever */
static inline void vTaskDelayUntil(TickType_t *, TickType_t) {}
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
