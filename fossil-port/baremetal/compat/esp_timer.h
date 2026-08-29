/* esp_timer.h — microsecond monotonic clock -> fossil-port arch timer. */
#pragma once
#include <cstdint>
extern "C" { uint64_t timer_ticks(void); uint32_t timer_freq_hz(void); }
static inline int64_t esp_timer_get_time(void) {
    return (int64_t)((timer_ticks() * 1000000ULL) / timer_freq_hz());
}
