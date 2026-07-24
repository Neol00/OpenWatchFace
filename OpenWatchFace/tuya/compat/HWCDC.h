/* ============================================================================
 *  tuya/compat/HWCDC.h - the firmware's USB-CDC serial type, for TuyaOpen.
 *
 *  On the ESP32 boards HWCDC is the native USB-CDC class and the firmware declares
 *  its own `HWCDC USBSerial;` for logging. TuyaOpen has no HWCDC; its serial is
 *  `Serial` (== _SerialUART0_, an arduino::SerialUART on UART0). Two mismatches make
 *  a plain typedef impossible:
 *    - HardwareSerial is abstract and SerialUART has no default ctor, but the
 *      firmware default-constructs `HWCDC USBSerial;`.
 *    - The firmware uses USBSerial.printf(...), but this core's Print has no printf().
 *
 *  So HWCDC is a small concrete Print subclass that forwards bytes to the core's
 *  Serial (giving all the print()/println() overloads for free) and adds printf().
 *  begin() forwards to Serial.begin so the firmware's USBSerial.begin(115200) works.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino routes "HWCDC.h" here,
 *  gated per-include).
 * ========================================================================== */
#pragma once
#include <Arduino.h>     // Serial (== _SerialUART0_), Print, String
#include <cstdarg>
#include <cstdio>

class HWCDC : public Print {
public:
    HWCDC() {}

    void begin(unsigned long baud = 115200) { Serial.begin(baud); }
    void end() { Serial.end(); }
    void flush() { Serial.flush(); }
    int  available() { return Serial.available(); }
    int  read() { return Serial.read(); }
    operator bool() { return true; }

    /* Print interface: forward the single required virtual to the core Serial; the
     * rest of Print (print/println for every type) is inherited and routes here. */
    size_t write(uint8_t b) override { return Serial.write(b); }
    size_t write(const uint8_t *buf, size_t n) override { return Serial.write(buf, n); }
    using Print::write;

    /* printf() - absent from this core's Print, but used widely by the firmware. */
    int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char stackbuf[160];
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
        va_end(ap);
        if (n < 0) return 0;
        if ((size_t)n < sizeof(stackbuf)) { write((const uint8_t *)stackbuf, (size_t)n); return n; }
        /* Long line: allocate exactly once and re-render. */
        char *heap = (char *)malloc((size_t)n + 1);
        if (!heap) { write((const uint8_t *)stackbuf, sizeof(stackbuf) - 1); return (int)sizeof(stackbuf) - 1; }
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)n + 1, fmt, ap);
        va_end(ap);
        write((const uint8_t *)heap, (size_t)n);
        free(heap);
        return n;
    }
};
