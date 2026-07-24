/* compat/freertos/semphr.h — FreeRTOS mutexes as real std::recursive_mutex.
 *
 * The firmware uses these as lock/unlock pairs (store_lock / i2c_lock) guarding
 * data shared between the net task (now a real thread) and the UI loop. They MUST
 * be real on this port. Recursive so nested locks in the same thread can't deadlock.
 * (The firmware takes them with portMAX_DELAY — a plain blocking lock.) */
#pragma once
#include "FreeRTOS.h"
#include <mutex>

typedef void * SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)          { return (SemaphoreHandle_t)new std::recursive_mutex(); }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)         { return xSemaphoreCreateMutex(); }
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return xSemaphoreCreateMutex(); }

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t) {
    if (h) ((std::recursive_mutex *)h)->lock();
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    if (h) ((std::recursive_mutex *)h)->unlock();
    return pdTRUE;
}
static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t h, TickType_t) { return xSemaphoreTake(h, 0); }
static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t h)             { return xSemaphoreGive(h); }
static inline void       vSemaphoreDelete(SemaphoreHandle_t h) { if (h) delete (std::recursive_mutex *)h; }
