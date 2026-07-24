/* ============================================================================
 *  tuya/compat/freertos/semphr.h - FreeRTOS mutexes via the REAL T5 tkl_mutex API.
 *
 *  The firmware uses these as lock/unlock pairs (the notif-store mutex, the shared-
 *  I2C mutex) guarding data shared between the net task (a real tkl thread) and the
 *  UI loop - they MUST be real. tkl_mutex is created already-unlocked; take/give map
 *  to lock/unlock. The firmware takes them with portMAX_DELAY (plain blocking lock).
 *
 *  NOTE: tkl_mutex is a plain (non-recursive) mutex. The firmware uses these as
 *  flat lock/unlock guards (no nested same-thread re-lock), so non-recursive is
 *  correct; if a recursive use surfaces later, revisit.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated per-include).
 * ========================================================================== */
#pragma once
#include "FreeRTOS.h"

extern "C" {
#include "tkl_mutex.h"
}

typedef void * SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    TKL_MUTEX_HANDLE m = nullptr;
    if (tkl_mutex_create_init(&m) != OPRT_OK) return nullptr;
    return (SemaphoreHandle_t)m;
}
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)         { return xSemaphoreCreateMutex(); }
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return xSemaphoreCreateMutex(); }

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t) {
    if (h) tkl_mutex_lock((TKL_MUTEX_HANDLE)h);
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    if (h) tkl_mutex_unlock((TKL_MUTEX_HANDLE)h);
    return pdTRUE;
}
static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t h, TickType_t) { return xSemaphoreTake(h, 0); }
static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t h)             { return xSemaphoreGive(h); }
static inline void       vSemaphoreDelete(SemaphoreHandle_t h) { if (h) tkl_mutex_release((TKL_MUTEX_HANDLE)h); }
