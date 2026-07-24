/* ============================================================================
 *  tuya/compat/freertos/task.h - FreeRTOS tasks via the REAL T5 tkl_thread API.
 *
 *  The firmware spawns a net task (notif_net.h) for WiFi + notification fetch and
 *  (on boards that have them) audio tasks. These must really run, so xTaskCreate*
 *  maps onto tkl_thread_create. The T5 has no per-core pinning exposed here, so
 *  xTaskCreatePinnedToCore drops the affinity arg (the SMP scheduler places it).
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated per-include).
 * ========================================================================== */
#pragma once
#include "FreeRTOS.h"

extern "C" {
#include "tkl_thread.h"
}

typedef void (*TaskFunction_t)(void *);

static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words,
                                     void *param, UBaseType_t prio, TaskHandle_t *h) {
    TKL_THREAD_HANDLE th = nullptr;
    /* FreeRTOS stack depth is in WORDS; tkl wants BYTES. 4 bytes/word on the T5 (ARM). */
    OPERATE_RET rt = tkl_thread_create(&th, name, (unsigned)stack_words * 4u,
                                       (unsigned)prio, (THREAD_FUNC_T)fn, param);
    if (h) *h = (TaskHandle_t)th;
    return (rt == OPRT_OK) ? pdPASS : pdFAIL;
}

static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_words,
                                                 void *param, UBaseType_t prio, TaskHandle_t *h, BaseType_t /*core*/) {
    return xTaskCreate(fn, name, stack_words, param, prio, h);   // no exposed core affinity on the T5
}

static inline void vTaskDelete(TaskHandle_t h) {
    /* NULL means "delete the calling task" in FreeRTOS; tkl_thread_release needs a
     * handle. The firmware's task fns call vTaskDelete(NULL) at their end to self-
     * terminate - for a tkl thread, returning from the function ends it, so a NULL
     * delete is a safe no-op. A non-NULL handle releases that thread. */
    if (h) tkl_thread_release((TKL_THREAD_HANDLE)h);
}
static inline void vTaskDelayUntil(TickType_t *, TickType_t) {}
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h) {
    UINT_T wm = 0;
    if (h) tkl_thread_get_watermark((TKL_THREAD_HANDLE)h, &wm);
    return (UBaseType_t)wm;
}
