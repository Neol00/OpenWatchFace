/* compat/esp_freertos_hooks.h — idle-hook registration stub (used by cpu_usage.h). */
#pragma once
typedef bool (*esp_freertos_idle_cb_t)(void);
static inline int esp_register_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t, unsigned) { return 0; }
static inline int esp_register_freertos_idle_hook(esp_freertos_idle_cb_t) { return 0; }
static inline void esp_deregister_freertos_idle_hook_for_cpu(esp_freertos_idle_cb_t, unsigned) {}
static inline void esp_deregister_freertos_idle_hook(esp_freertos_idle_cb_t) {}
