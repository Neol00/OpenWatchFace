/* Preferences.h — Arduino Preferences (ESP32 NVS) API backed by the REAL
 * gen6 key-value store (platform/nvs_store.c: RAM working set, CRC'd
 * ping-pong slots in the userdata NVS region — the "extended NVS").
 *
 * Persistence contract: puts mutate the RAM store immediately; end() commits
 * to eMMC (atomic slot flip). Matches OWF's begin/put/end usage. If storage
 * never came up (no eMMC / no userdata), every call degrades to the old
 * volatile-stub behavior automatically (nvs_* fail soft, getters return the
 * caller's defaults).
 */
#pragma once
#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include <cstring>

extern "C" {
    int nvs_load(void);
    int nvs_commit(void);
    int nvs_put(const char *ns, const char *key, unsigned char type,
                const void *data, uint32_t len);
    int nvs_get(const char *ns, const char *key, void *out, uint32_t cap);
    int nvs_erase(const char *ns, const char *key);
    int nvs_erase_ns(const char *ns);
    int nvs_haskey(const char *ns, const char *key);
}

class Preferences {
    char _ns[16] = {0};
    bool _began = false;

    template <typename T> size_t putT(const char *k, const T &v) {
        return nvs_put(_ns, k, 1, &v, sizeof(T)) < 0 ? 0 : sizeof(T);
    }
    template <typename T> T getT(const char *k, T d) {
        T v;
        return nvs_get(_ns, k, &v, sizeof(T)) == (int)sizeof(T) ? v : d;
    }
public:
    bool begin(const char *ns, bool = false) {
        strncpy(_ns, ns ? ns : "", sizeof(_ns) - 1);
        _ns[sizeof(_ns) - 1] = 0;
        _began = true;
        nvs_load();
        return true;
    }
    void end() { if (_began) nvs_commit(); _began = false; }
    bool clear() { return nvs_erase_ns(_ns) == 0; }
    bool remove(const char *k) { return nvs_erase(_ns, k) == 0; }
    bool isKey(const char *k) { return nvs_haskey(_ns, k) != 0; }

    size_t putChar(const char *k, int8_t v)     { return putT(k, v); }
    size_t putUChar(const char *k, uint8_t v)   { return putT(k, v); }
    size_t putShort(const char *k, int16_t v)   { return putT(k, v); }
    size_t putUShort(const char *k, uint16_t v) { return putT(k, v); }
    size_t putInt(const char *k, int32_t v)     { return putT(k, v); }
    size_t putUInt(const char *k, uint32_t v)   { return putT(k, v); }
    size_t putLong(const char *k, int32_t v)    { return putT(k, v); }
    size_t putULong(const char *k, uint32_t v)  { return putT(k, v); }
    size_t putULong64(const char *k, uint64_t v){ return putT(k, v); }
    size_t putFloat(const char *k, float v)     { return putT(k, v); }
    size_t putDouble(const char *k, double v)   { return putT(k, v); }
    size_t putBool(const char *k, bool v)       { uint8_t b = v ? 1 : 0; return putT(k, b); }
    size_t putString(const char *k, const char *s) {
        if (!s) return 0;
        uint32_t n = (uint32_t)strlen(s);
        return nvs_put(_ns, k, 2, s, n) < 0 ? 0 : n;
    }
    size_t putString(const char *k, const String &s) { return putString(k, s.c_str()); }
    size_t putBytes(const char *k, const void *p, size_t n) {
        return nvs_put(_ns, k, 3, p, (uint32_t)n) < 0 ? 0 : n;
    }

    int8_t   getChar(const char *k, int8_t d = 0)      { return getT(k, d); }
    uint8_t  getUChar(const char *k, uint8_t d = 0)    { return getT(k, d); }
    int16_t  getShort(const char *k, int16_t d = 0)    { return getT(k, d); }
    uint16_t getUShort(const char *k, uint16_t d = 0)  { return getT(k, d); }
    int32_t  getInt(const char *k, int32_t d = 0)      { return getT(k, d); }
    uint32_t getUInt(const char *k, uint32_t d = 0)    { return getT(k, d); }
    int32_t  getLong(const char *k, int32_t d = 0)     { return getT(k, d); }
    uint32_t getULong(const char *k, uint32_t d = 0)   { return getT(k, d); }
    uint64_t getULong64(const char *k, uint64_t d = 0) { return getT(k, d); }
    float    getFloat(const char *k, float d = 0)      { return getT(k, d); }
    double   getDouble(const char *k, double d = 0)    { return getT(k, d); }
    bool     getBool(const char *k, bool d = false) {
        uint8_t b; return nvs_get(_ns, k, &b, 1) == 1 ? b != 0 : d;
    }
    String getString(const char *k, const String &d = String()) {
        int n = nvs_get(_ns, k, nullptr, 0);
        if (n < 0 || n > 4096) return d;
        char buf[4097];
        nvs_get(_ns, k, buf, (uint32_t)n);
        buf[n] = 0;
        return String(buf);
    }
    size_t getString(const char *k, char *out, size_t cap) {
        if (!out || !cap) return 0;
        int n = nvs_get(_ns, k, out, (uint32_t)cap - 1u);
        if (n < 0) { out[0] = 0; return 0; }
        size_t take = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        out[take] = 0;
        return take;
    }
    size_t getBytesLength(const char *k) {
        int n = nvs_get(_ns, k, nullptr, 0);
        return n < 0 ? 0 : (size_t)n;
    }
    size_t getBytes(const char *k, void *out, size_t cap) {
        int n = nvs_get(_ns, k, out, (uint32_t)cap);
        if (n < 0) return 0;
        return (size_t)n < cap ? (size_t)n : cap;
    }
};
