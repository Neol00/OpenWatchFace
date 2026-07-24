/* ============================================================================
 *  compat/arduino_shim.cpp — definitions for the Arduino core shim.
 *
 *  Does NOT include MaixCDK headers (those bring `using namespace std`, which
 *  collides with Arduino.h's free `map()`/`std::map`). The MaixCDK-backed time
 *  functions live in arduino_time_maix.cpp instead.
 * ========================================================================== */
#include "Arduino.h"
#include <cstdarg>
#include <cctype>
#include "owf_maix_hooks.h"   // g_owf_boot_level (driven by main.cpp's User-button poll)

/* ---- Math ----------------------------------------------------------------- */
long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    if (in_max == in_min) return out_min;
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
long random(long howbig)               { return howbig <= 0 ? 0 : (long)(::random() % howbig); }
long random(long howsmall, long howbig){ return howsmall >= howbig ? howsmall
                                                : howsmall + random(howbig - howsmall); }
void randomSeed(unsigned long seed)    { ::srandom((unsigned)seed); }

/* ---- GPIO / ADC / PWM — no hardware pins on Linux ------------------------ */
/* BOOT button level, driven from the MaixCam-Pro User button in main.cpp.
 * The firmware only digitalRead()s the BOOT pin on this platform, so returning
 * this for every pin is fine. 1 = released (HIGH), 0 = pressed (LOW). */
volatile int g_owf_boot_level = 1;

void pinMode(uint8_t, uint8_t)      {}
void digitalWrite(uint8_t, uint8_t) {}
int  digitalRead(uint8_t)           { return g_owf_boot_level; }
int  analogRead(uint8_t)            { return 0; }
void analogWrite(uint8_t, int)      {}

/* ---- String members that aren't inline ----------------------------------- */
bool String::equalsIgnoreCase(const String &o) const
{
    if (s_.size() != o.s_.size()) return false;
    for (size_t i = 0; i < s_.size(); ++i)
        if (tolower((unsigned char)s_[i]) != tolower((unsigned char)o.s_[i])) return false;
    return true;
}
String String::substring(unsigned from, unsigned to) const
{
    if (from > s_.size()) from = s_.size();
    if (to   > s_.size()) to   = s_.size();
    if (to < from) std::swap(from, to);
    return String(s_.substr(from, to - from));
}
bool String::endsWith(const String &p) const
{
    return p.s_.size() <= s_.size() &&
           s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0;
}
void String::trim()
{
    size_t b = s_.find_first_not_of(" \t\r\n");
    size_t e = s_.find_last_not_of(" \t\r\n");
    s_ = (b == std::string::npos) ? std::string() : s_.substr(b, e - b + 1);
}
void String::toLowerCase() { for (auto &c : s_) c = (char)tolower((unsigned char)c); }
void String::toUpperCase() { for (auto &c : s_) c = (char)toupper((unsigned char)c); }
void String::replace(const String &from, const String &to)
{
    if (from.s_.empty()) return;
    size_t p = 0;
    while ((p = s_.find(from.s_, p)) != std::string::npos) {
        s_.replace(p, from.s_.size(), to.s_);
        p += to.s_.size();
    }
}

/* ---- Serial -> stdout ----------------------------------------------------
 * USBSerial is defined by the firmware itself (`HWCDC USBSerial;` in the .ino,
 * HWCDC == HardwareSerial via compat/HWCDC.h), so we only provide Serial here. */
HardwareSerial Serial;

static size_t put(const char *s) { return s ? fputs(s, stdout), strlen(s) : 0; }
static const char *fmt_for(int base) {
    switch (base) { case HEX: return "%x"; case OCT: return "%o"; default: return "%d"; }
}

size_t HardwareSerial::print(const char *s)        { return put(s); }
size_t HardwareSerial::print(const String &s)      { return put(s.c_str()); }
size_t HardwareSerial::print(char c)               { putchar(c); return 1; }
size_t HardwareSerial::print(int v, int base)      { return printf(fmt_for(base), v); }
size_t HardwareSerial::print(unsigned v, int base) { return printf(base == HEX ? "%x" : "%u", v); }
size_t HardwareSerial::print(long v, int base)     { return printf(base == HEX ? "%lx" : "%ld", v); }
size_t HardwareSerial::print(unsigned long v, int base) { return printf(base == HEX ? "%lx" : "%lu", v); }
size_t HardwareSerial::print(double v, int digits) { return printf("%.*f", digits, v); }

size_t HardwareSerial::println(const char *s)      { size_t n = put(s); putchar('\n'); return n + 1; }
size_t HardwareSerial::println(const String &s)    { return println(s.c_str()); }
size_t HardwareSerial::println(char c)             { putchar(c); putchar('\n'); return 2; }
size_t HardwareSerial::println(int v, int base)    { size_t n = print(v, base); putchar('\n'); return n + 1; }
size_t HardwareSerial::println(unsigned v, int base){ size_t n = print(v, base); putchar('\n'); return n + 1; }
size_t HardwareSerial::println(long v, int base)   { size_t n = print(v, base); putchar('\n'); return n + 1; }
size_t HardwareSerial::println(unsigned long v, int base){ size_t n = print(v, base); putchar('\n'); return n + 1; }
size_t HardwareSerial::println(double v, int digits){ size_t n = print(v, digits); putchar('\n'); return n + 1; }
size_t HardwareSerial::println(void)               { putchar('\n'); return 1; }

int HardwareSerial::printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* ---- Arduino-ESP32 time helpers / ESP object ----------------------------- */
#include <ctime>
bool getLocalTime(struct tm *info, uint32_t /*ms*/)
{
    time_t now = ::time(nullptr);   /* ::time — `using namespace maix` also has maix::time */
    localtime_r(&now, info);
    return info->tm_year + 1900 >= 2024;   // "synced" once the clock is plausibly set
}
void configTzTime(const char *tz, const char *, const char *, const char *)
{
    if (tz) { setenv("TZ", tz, 1); tzset(); }
}
void configTime(long, int, const char *, const char *, const char *) {}
float temperatureRead(void) { return 0.0f; }   // no host die-temp sensor

EspClass ESP;

/* ---- Single definitions for the globals declared `extern` in the stubs ---- */
#include "Wire.h"
#include "WiFi.h"
#include "FFat.h"
TwoWire  Wire;
TwoWire  Wire1;
WiFiClass WiFi;
FFatFS   FFat;
