/* ============================================================================
 *  esp_clk_stubs.h - no-op stand-ins for the ESP-only clock/voltage modules.
 *
 *  Included instead of clocks.h / overclock.h / core_voltage.h on the Maix build.
 *  Those modules poke SoC registers (soc, esp_private, regi2c) that don't
 *  exist on a Linux host; CPU frequency/voltage is fixed and not ours to tune.
 *  The Power app and boot path call only this small public surface.
 *  Signatures mirror the originals.
 * ========================================================================== */
#pragma once
#include <cstddef>
#include <cstdint>

/* clocks.h - read-only clock-tree dump (diagnostic). */
static inline void clocks_dump(const char * /*tag*/) {}
static inline void clocks_format(char *buf, size_t n) { if (buf && n) buf[0] = 0; }

/* overclock.h - experimental CPU overclock (default OFF on ESP too). */
static inline void overclock_apply(void) {}
static inline void overclock_check_recovery(void) {}

/* core_voltage.h - experimental core undervolt self-test (default OFF on ESP too). */
static inline void core_selftest_capture_golden(void) {}
static inline bool core_selftest_ok(void) { return true; }
static inline void core_apply_for_mhz(uint16_t /*mhz*/) {}
static inline uint32_t core_get_dig_dbias(void)   { return 0; }   // dig_dbias trim register value
static inline uint16_t core_dbias_to_mv(uint32_t) { return 0; }   // -> approx mV (unknown on Linux)
static const bool s_core_unstable = false;                        // never reverted (no undervolt here)
