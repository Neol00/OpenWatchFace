/* ============================================================================
 *  compat/Arduino.h — Arduino core shim for the MaixCam-Pro (Linux) port.
 *
 *  Placed FIRST on the include path so the firmware's `#include <Arduino.h>`
 *  resolves here instead of to a real Arduino core. Provides the Arduino API
 *  surface the firmware uses (timing, GPIO no-ops, math, Serial, String) on
 *  plain Linux. Definitions that need MaixCDK live in arduino_shim.cpp.
 *
 *  This is intentionally minimal and grows as the build surfaces new symbols.
 * ========================================================================== */
#ifndef ARDUINO_H_COMPAT
#define ARDUINO_H_COMPAT

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <algorithm>
#include <sys/time.h>   // settimeofday/gettimeofday (used by board_clock.h)

/* ---- Basic Arduino typedefs / constants ---------------------------------- */
typedef uint8_t  byte;
typedef bool     boolean;
typedef unsigned int word;

#ifndef HIGH
#define HIGH 1
#define LOW  0
#endif
#define INPUT        0x0
#define OUTPUT       0x1
#define INPUT_PULLUP 0x2
#define INPUT_PULLDOWN 0x3
#define LSBFIRST 0
#define MSBFIRST 1
#define RISING   1
#define FALLING  2
#define CHANGE   3

/* Section/placement attributes are no-ops on Linux (no RTC RAM / IRAM / PSRAM). */
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define RTC_RODATA_ATTR
#define RTC_IRAM_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR

/* PROGMEM / flash-string helpers are no-ops on Linux (unified address space). */
#define PROGMEM
#define PGM_P const char *
#define PSTR(s) (s)
#define F(s)    (s)
#ifndef pgm_read_byte
#define pgm_read_byte(addr)  (*(const uint8_t  *)(addr))
#define pgm_read_word(addr)  (*(const uint16_t *)(addr))
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#define pgm_read_ptr(addr)   (*(void * const *)(addr))
#endif
#define memcpy_P  memcpy
#define strcpy_P  strcpy
#define strlen_P  strlen
#define strcmp_P  strcmp

/* ---- Math helpers --------------------------------------------------------
 * Arduino traditionally defines min/max/abs/constrain as function-like MACROS,
 * but those break libstdc++ headers (<valarray>, <map>, …) which use min/max/abs
 * as member/function names. Provide them as inline templates instead — same call
 * syntax for the firmware, no preprocessor collateral damage. (Undef first in case
 * a prior include defined the macros.) */
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef abs
#undef abs
#endif
#ifdef constrain
#undef constrain
#endif
template <class T, class U> inline auto min(T a, U b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template <class T, class U> inline auto max(T a, U b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
template <class T> inline T abs(T x) { return x < 0 ? -x : x; }
template <class T, class L, class H> inline T constrain(T x, L lo, H hi) { return x < lo ? lo : (x > hi ? hi : x); }
#define bitRead(v, b)   (((v) >> (b)) & 0x1)
#define bitSet(v, b)    ((v) |= (1UL << (b)))
#define bitClear(v, b)  ((v) &= ~(1UL << (b)))
#define bitWrite(v, b, x) ((x) ? bitSet(v, b) : bitClear(v, b))

long  map(long x, long in_min, long in_max, long out_min, long out_max);
long  random(long howbig);
long  random(long howsmall, long howbig);
void  randomSeed(unsigned long seed);

/* ---- Time ---------------------------------------------------------------- */
uint32_t millis(void);
uint32_t micros(void);
void     delay(uint32_t ms);
void     delayMicroseconds(uint32_t us);
void     yield(void);

/* ---- GPIO / ADC / PWM — no-ops on Linux (no MCU pins) -------------------- */
void     pinMode(uint8_t pin, uint8_t mode);
void     digitalWrite(uint8_t pin, uint8_t val);
int      digitalRead(uint8_t pin);
int      analogRead(uint8_t pin);
void     analogWrite(uint8_t pin, int val);

/* ---- String: minimal Arduino String over std::string --------------------- */
class String {
public:
    String() {}
    String(const char *s)        : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    String(char c)               : s_(1, c) {}
    String(int v)                { s_ = std::to_string(v); }
    String(unsigned v)           { s_ = std::to_string(v); }
    String(long v)               { s_ = std::to_string(v); }
    String(unsigned long v)      { s_ = std::to_string(v); }
    String(float v)              { s_ = std::to_string(v); }
    String(double v)             { s_ = std::to_string(v); }

    const char *c_str() const          { return s_.c_str(); }
    unsigned    length() const         { return (unsigned)s_.size(); }
    bool        isEmpty() const        { return s_.empty(); }
    char        charAt(unsigned i) const { return i < s_.size() ? s_[i] : 0; }
    char        operator[](unsigned i) const { return charAt(i); }

    String &operator+=(const String &o) { s_ += o.s_; return *this; }
    String &operator+=(const char *o)   { s_ += (o ? o : ""); return *this; }
    String &operator+=(char c)          { s_ += c; return *this; }
    String  operator+(const String &o) const { return String(s_ + o.s_); }
    String  operator+(const char *o)   const { return String(s_ + (o ? o : "")); }

    bool operator==(const String &o) const { return s_ == o.s_; }
    bool operator==(const char *o)   const { return s_ == (o ? o : ""); }
    bool operator!=(const String &o) const { return s_ != o.s_; }
    bool operator!=(const char *o)   const { return s_ != (o ? o : ""); }
    bool operator<(const String &o)  const { return s_ < o.s_; }

    bool     equals(const String &o) const      { return s_ == o.s_; }
    bool     equalsIgnoreCase(const String &o) const;
    int      indexOf(char c) const                { auto p = s_.find(c); return p == std::string::npos ? -1 : (int)p; }
    int      indexOf(const String &o) const       { auto p = s_.find(o.s_); return p == std::string::npos ? -1 : (int)p; }
    int      indexOf(const char *o) const         { auto p = s_.find(o ? o : ""); return p == std::string::npos ? -1 : (int)p; }
    int      indexOf(char c, unsigned from) const { auto p = s_.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    int      indexOf(const String &o, unsigned from) const { auto p = s_.find(o.s_, from); return p == std::string::npos ? -1 : (int)p; }
    int      indexOf(const char *o, unsigned from) const   { auto p = s_.find(o ? o : "", from); return p == std::string::npos ? -1 : (int)p; }
    int      lastIndexOf(char c) const            { auto p = s_.rfind(c); return p == std::string::npos ? -1 : (int)p; }
    String   substring(unsigned from) const       { return from <= s_.size() ? String(s_.substr(from)) : String(); }
    String   substring(unsigned from, unsigned to) const;
    bool     startsWith(const String &p) const    { return s_.rfind(p.s_, 0) == 0; }
    bool     endsWith(const String &p) const;
    void     trim();
    void     toLowerCase();
    void     toUpperCase();
    void     replace(const String &from, const String &to);
    int      toInt() const   { return (int)strtol(s_.c_str(), nullptr, 10); }
    float    toFloat() const { return strtof(s_.c_str(), nullptr); }

    const std::string &str() const { return s_; }

private:
    std::string s_;
};

inline String operator+(const char *a, const String &b) { return String(a) + b; }

/* ---- Print / Stream — Serial & USBSerial -------------------------------- */
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class HardwareSerial {
public:
    void begin(unsigned long) {}
    void begin(unsigned long, int) {}
    void end() {}
    operator bool() const { return true; }

    size_t print(const char *s);
    size_t print(const String &s);
    size_t print(char c);
    size_t print(int v, int base = DEC);
    size_t print(unsigned v, int base = DEC);
    size_t print(long v, int base = DEC);
    size_t print(unsigned long v, int base = DEC);
    size_t print(double v, int digits = 2);

    size_t println(const char *s);
    size_t println(const String &s);
    size_t println(char c);
    size_t println(int v, int base = DEC);
    size_t println(unsigned v, int base = DEC);
    size_t println(long v, int base = DEC);
    size_t println(unsigned long v, int base = DEC);
    size_t println(double v, int digits = 2);
    size_t println(void);

    int    printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void   flush() {}
    int    available() { return 0; }
    int    read() { return -1; }
    void   setDebugOutput(bool) {}
};

extern HardwareSerial Serial;
extern HardwareSerial USBSerial;

/* ---- Arduino-ESP32 time helpers (system clock on Linux) ------------------ */
struct tm;
bool getLocalTime(struct tm *info, uint32_t ms = 5000);
void configTzTime(const char *tz, const char *s1, const char *s2 = nullptr, const char *s3 = nullptr);
void configTime(long gmtOffset_sec, int daylightOffset_sec,
                const char *s1, const char *s2 = nullptr, const char *s3 = nullptr);

/* S3 on-die temperature sensor — no host equivalent. */
float temperatureRead(void);

/* ---- ESP / system info object (Arduino's Esp.h) ------------------------- */
#define ESP_ARDUINO_VERSION_MAJOR 3
#define ESP_ARDUINO_VERSION_MINOR 3
#define ESP_ARDUINO_VERSION_PATCH 0

class EspClass {
public:
    const char *getChipModel()      { return "SG2002 (RISC-V)"; }
    uint8_t     getChipRevision()   { return 1; }
    uint8_t     getChipCores()      { return 1; }
    uint32_t    getCpuFreqMHz()     { return 1000; }
    uint32_t    getFreeHeap()       { return 16u * 1024 * 1024; }
    uint32_t    getHeapSize()       { return 64u * 1024 * 1024; }
    uint32_t    getMinFreeHeap()    { return 16u * 1024 * 1024; }
    uint32_t    getMaxAllocHeap()   { return 16u * 1024 * 1024; }
    uint32_t    getFreePsram()      { return 0; }
    uint32_t    getPsramSize()      { return 0; }
    uint32_t    getFlashChipSize()  { return 16u * 1024 * 1024; }
    uint64_t    getEfuseMac()       { return 0x5849414d0002ULL; }
    void        restart()           { extern void exit(int); exit(0); }
};
extern EspClass ESP;

/* The real Arduino-ESP32 core exposes the CPU-frequency helpers via Arduino.h
 * (esp32-hal-cpu.h); mirror that so settings_store.h etc. find setCpuFrequencyMhz. */
#include "esp32-hal-cpu.h"

#endif /* ARDUINO_H_COMPAT */
