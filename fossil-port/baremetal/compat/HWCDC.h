/* HWCDC.h — USB-CDC / UART serial for the firmware, on the fossil-port console.
 * Provides a Print subclass (printf + all print/println overloads) that writes to
 * the runtime console (con_putc), a global `Serial`, and HWCDC (used for USBSerial). */
#pragma once
#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
extern "C" { void con_putc(char c); }

class OwfSerial : public Print {
public:
    OwfSerial() {}
    void begin(unsigned long = 115200) {}
    void end() {}
    void flush() {}
    int  available() { return 0; }
    int  read() { return -1; }
    operator bool() { return true; }
    size_t write(uint8_t b) override { con_putc((char)b); return 1; }
    size_t write(const uint8_t *buf, size_t n) override { for (size_t i=0;i<n;i++) con_putc((char)buf[i]); return n; }
    using Print::write;
    int printf(const char *fmt, ...) __attribute__((format(printf,2,3))) {
        char b[192]; va_list ap; va_start(ap,fmt);
        int n = vsnprintf(b,sizeof(b),fmt,ap); va_end(ap);
        if (n<0) return 0;
        size_t k = (size_t)n < sizeof(b) ? (size_t)n : sizeof(b)-1;
        write((const uint8_t*)b,k); return (int)k;
    }
};
typedef OwfSerial HWCDC;
inline OwfSerial Serial;          /* C++17 inline var: one definition across TUs */
