/* compat/esp_timer.h — microsecond clock via the Arduino shim's maix-backed time. */
#pragma once
#include <cstdint>

extern uint32_t micros(void);
static inline int64_t esp_timer_get_time(void) { return (int64_t)micros(); }
