/* compat/Wire.h — Arduino I2C (TwoWire) stub. All on-board I2C peripherals
 * (touch/RTC/PMU/IMU) are OFF on the Maix board, so these are no-ops; kept so the
 * unconditional `#include <Wire.h>` and any stray call sites compile. */
#pragma once
#include <cstdint>
#include <cstddef>

class TwoWire {
public:
    TwoWire(int = 0) {}
    bool begin(int = -1, int = -1, uint32_t = 0) { return true; }
    bool begin(uint8_t)                          { return true; }
    void end() {}
    void setClock(uint32_t) {}
    void beginTransmission(uint8_t) {}
    uint8_t endTransmission(bool = true) { return 0; }      /* 0 = success */
    size_t  write(uint8_t)        { return 1; }
    size_t  write(const uint8_t *, size_t n) { return n; }
    uint8_t requestFrom(uint8_t, uint8_t, bool = true) { return 0; }
    int     available() { return 0; }
    int     read()      { return -1; }
    int     peek()      { return -1; }
    void    flush()     {}
};

extern TwoWire Wire;
extern TwoWire Wire1;
