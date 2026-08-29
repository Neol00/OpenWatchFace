/* owf_arduino_extra.cpp — Arduino/AVR number-format helpers ArduinoCore-API's
 * String.cpp links against but newlib doesn't provide (dtostrf/ltoa/ultoa/itoa). */
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" char *dtostrf(double val, signed char width, unsigned char prec, char *s) {
    char fmt[24];
    snprintf(fmt, sizeof(fmt), "%%%d.%uf", (int)width, (unsigned)prec);
    sprintf(s, fmt, val);
    return s;
}

static char *u_to_base(unsigned long v, char *s, int base, int neg) {
    char tmp[33]; int i = 0;
    if (base < 2 || base > 16) base = 10;
    do { int d = v % base; tmp[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10); v /= base; } while (v);
    char *p = s;
    if (neg) *p++ = '-';
    while (i) *p++ = tmp[--i];
    *p = '\0';
    return s;
}
extern "C" char *ltoa(long v, char *s, int base) {
    if (base == 10 && v < 0) return u_to_base((unsigned long)(-v), s, 10, 1);
    return u_to_base((unsigned long)v, s, base, 0);
}
extern "C" char *ultoa(unsigned long v, char *s, int base) { return u_to_base(v, s, base, 0); }
extern "C" char *itoa(int v, char *s, int base) { return ltoa((long)v, s, base); }
extern "C" char *utoa(unsigned int v, char *s, int base) { return ultoa((unsigned long)v, s, base); }
