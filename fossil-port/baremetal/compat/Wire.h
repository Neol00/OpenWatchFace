/* Wire.h — Arduino TwoWire (I2C) on the fossil-port QUP driver (msm_i2c.c).
 *
 * REAL implementation (Milestone 3): transactions are buffered Arduino-style
 * and executed as single QUP FIFO transfers on endTransmission()/requestFrom().
 * The bus is selectable per TwoWire instance; the default is the board's
 * PLAT_I2C_DEFAULT_BASE (on the Gen 6: BLSP2 QUP1, the only bus the published
 * DTB shows populated — NFC@0x28). Errors follow the Arduino contract:
 * endTransmission() 0 = ok, 2 = NACK on address/data, 4 = other error.
 *
 * Limits: one QUP FIFO transfer per transaction — 32 data bytes out, 32 in
 * (WIRE_BUFFER). Every sensor this firmware talks to fits comfortably.
 */
#pragma once
#include <Arduino.h>
#include <cstdint>
#include <cstddef>

extern "C" {
int i2c_bus_init(uintptr_t base);
int i2c_bus_xfer(uintptr_t base, uint8_t addr, const uint8_t *wbuf, uint32_t wlen,
                 uint8_t *rbuf, uint32_t rlen);
}

#ifndef WIRE_BUFFER
#define WIRE_BUFFER 32
#endif

class TwoWire {
public:
    explicit TwoWire(uintptr_t base = 0) : _base(base) {}

    bool begin() {
        if (!_base) _base = WIRE_DEFAULT_BASE;
        if (!_base) return false;        /* board has no I2C bus configured */
        _inited = (i2c_bus_init(_base) == 0);
        return _inited;
    }
    bool begin(int /*sda*/, int /*scl*/) { return begin(); }  /* pins are muxed by aboot */
    bool begin(uint8_t) { return begin(); }                   /* slave mode: unsupported */
    void end() { _inited = false; }
    void setClock(uint32_t) {}      /* fixed 400 kHz (PLAT_I2C_BUS_HZ) */
    bool setSDA(int) { return true; }
    bool setSCL(int) { return true; }

    void beginTransmission(uint8_t a) { _addr = a; _txlen = 0; _txerr = 0; }
    size_t write(uint8_t b) {
        if (_txlen >= WIRE_BUFFER) { _txerr = 1; return 0; }
        _tx[_txlen++] = b;
        return 1;
    }
    size_t write(const uint8_t *b, size_t n) {
        size_t i;
        for (i = 0; i < n; i++) if (!write(b[i])) break;
        return i;
    }
    uint8_t endTransmission(bool /*stop*/ = true) {
        if (!_inited && !begin()) return 4;
        if (_txerr) return 1;                       /* data too long */
        int rc = i2c_bus_xfer(_base, _addr, _tx, _txlen, nullptr, 0);
        return rc == 0 ? 0 : (rc == -2 ? 2 : 4);
    }

    size_t requestFrom(uint8_t addr, size_t n) {
        _rxlen = _rxpos = 0;
        if (n > WIRE_BUFFER) n = WIRE_BUFFER;
        if (!_inited && !begin()) return 0;
        if (i2c_bus_xfer(_base, addr, nullptr, 0, _rx, (uint32_t)n) == 0)
            _rxlen = n;
        return _rxlen;
    }
    size_t requestFrom(int addr, int n) {
        return requestFrom((uint8_t)addr, (size_t)(n < 0 ? 0 : n));
    }
    int available() { return (int)(_rxlen - _rxpos); }
    int read() { return _rxpos < _rxlen ? _rx[_rxpos++] : -1; }
    int peek() { return _rxpos < _rxlen ? _rx[_rxpos] : -1; }

    static const uintptr_t WIRE_DEFAULT_BASE;

private:
    uintptr_t _base;
    bool     _inited = false;
    uint8_t  _addr = 0, _txerr = 0;
    uint8_t  _tx[WIRE_BUFFER];
    uint8_t  _rx[WIRE_BUFFER];
    size_t   _txlen = 0, _rxlen = 0, _rxpos = 0;
};
extern TwoWire Wire;
