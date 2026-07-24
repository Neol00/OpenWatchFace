/* compat/esp32-hal-cpu.h — CPU frequency API stubbed (fixed on a Linux host). */
#pragma once
#include <cstdint>

static inline uint32_t getCpuFrequencyMhz(void)        { return 1000; }   /* SG2002 ~1 GHz */
static inline bool     setCpuFrequencyMhz(uint32_t)    { return false; }
static inline uint32_t getXtalFrequencyMhz(void)       { return 40; }
static inline uint32_t getApbFrequency(void)           { return 80000000; }
