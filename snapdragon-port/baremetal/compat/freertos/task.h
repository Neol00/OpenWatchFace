#pragma once
#include "../../freertos/include/task.h"
/* xTaskCreatePinnedToCore: single-core here -> drop the affinity arg. */
static inline BaseType_t xTaskCreatePinnedToCore(
        TaskFunction_t fn, const char* name, const uint32_t stack,
        void* param, UBaseType_t prio, TaskHandle_t* handle, BaseType_t /*core*/) {
    return xTaskCreate(fn, name, (configSTACK_DEPTH_TYPE)stack, param, prio, handle);
}
static inline BaseType_t xPortGetCoreID(void) { return 0; }
