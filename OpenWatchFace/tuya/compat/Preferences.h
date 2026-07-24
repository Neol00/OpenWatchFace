/* ============================================================================
 *  tuya/compat/Preferences.h - Arduino NVS (Preferences) backed by the REAL
 *  TuyaOpen key-value store (tal_kv), so settings PERSIST across reboots.
 *
 *  This matters on the T5: deep sleep wakes THROUGH RESET, so anything kept only in
 *  RAM (settings, alarms, the RTC time backup, timers) would be lost every wake. The
 *  maix Preferences shim is an in-memory map (no persistence); here we back the same
 *  Arduino Preferences surface with tal_kv_set/get/del so values survive power
 *  cycles and deep-sleep wakes.
 *
 *  Namespacing: Arduino Preferences scopes keys by begin(namespace); tal_kv keys are
 *  flat, so we store under "<ns>:<key>". Values are raw bytes (scalars memcpy'd,
 *  strings/bytes stored verbatim) - matches how the firmware round-trips them.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino routes <Preferences.h>
 *  here, gated per-include).
 * ========================================================================== */
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include "Arduino.h"   // String

extern "C" {
#include "tuya_cloud_types.h"
#include "tal_kv.h"
}

class Preferences {
public:
    bool begin(const char *name, bool readOnly = false) {
        ns_ = name ? name : ""; ro_ = readOnly; return true;
    }
    void end() { ns_.clear(); }

    /* tal_kv has no namespace-wide clear; the firmware uses clear() rarely. Without a
     * key enumeration API we can't wipe a whole namespace, so this is a no-op that
     * reports success - individual remove()s below DO work. (Revisit if a real
     * consumer depends on a full clear.) */
    bool clear() { return true; }
    bool remove(const char *key) { return tal_kv_del(k(key).c_str()) == OPRT_OK; }
    bool isKey(const char *key)  {
        uint8_t *v = nullptr; size_t n = 0;
        if (tal_kv_get(k(key).c_str(), &v, &n) != OPRT_OK) return false;
        tal_kv_free(v); return true;
    }

    /* ---- scalar put/get (stored as raw bytes) ---- */
    size_t putChar(const char *key, int8_t v)     { return putRaw(key, &v, sizeof v); }
    size_t putUChar(const char *key, uint8_t v)   { return putRaw(key, &v, sizeof v); }
    size_t putShort(const char *key, int16_t v)   { return putRaw(key, &v, sizeof v); }
    size_t putUShort(const char *key, uint16_t v) { return putRaw(key, &v, sizeof v); }
    size_t putInt(const char *key, int32_t v)     { return putRaw(key, &v, sizeof v); }
    size_t putUInt(const char *key, uint32_t v)   { return putRaw(key, &v, sizeof v); }
    size_t putLong(const char *key, int32_t v)    { return putRaw(key, &v, sizeof v); }
    size_t putULong(const char *key, uint32_t v)  { return putRaw(key, &v, sizeof v); }
    size_t putLong64(const char *key, int64_t v)  { return putRaw(key, &v, sizeof v); }
    size_t putULong64(const char *key, uint64_t v){ return putRaw(key, &v, sizeof v); }
    size_t putBool(const char *key, bool v)       { return putRaw(key, &v, sizeof v); }
    size_t putFloat(const char *key, float v)     { return putRaw(key, &v, sizeof v); }
    size_t putDouble(const char *key, double v)   { return putRaw(key, &v, sizeof v); }

    int8_t   getChar(const char *key, int8_t d = 0)     { return getRaw<int8_t>(key, d); }
    uint8_t  getUChar(const char *key, uint8_t d = 0)   { return getRaw<uint8_t>(key, d); }
    int16_t  getShort(const char *key, int16_t d = 0)   { return getRaw<int16_t>(key, d); }
    uint16_t getUShort(const char *key, uint16_t d = 0) { return getRaw<uint16_t>(key, d); }
    int32_t  getInt(const char *key, int32_t d = 0)     { return getRaw<int32_t>(key, d); }
    uint32_t getUInt(const char *key, uint32_t d = 0)   { return getRaw<uint32_t>(key, d); }
    int32_t  getLong(const char *key, int32_t d = 0)    { return getRaw<int32_t>(key, d); }
    uint32_t getULong(const char *key, uint32_t d = 0)  { return getRaw<uint32_t>(key, d); }
    int64_t  getLong64(const char *key, int64_t d = 0)  { return getRaw<int64_t>(key, d); }
    uint64_t getULong64(const char *key, uint64_t d = 0){ return getRaw<uint64_t>(key, d); }
    bool     getBool(const char *key, bool d = false)   { return getRaw<bool>(key, d); }
    float    getFloat(const char *key, float d = 0)     { return getRaw<float>(key, d); }
    double   getDouble(const char *key, double d = 0)   { return getRaw<double>(key, d); }

    /* ---- string ---- */
    size_t putString(const char *key, const String &v) {
        return putRaw(key, v.c_str(), v.length());
    }
    size_t putString(const char *key, const char *v) {
        return putRaw(key, v, v ? strlen(v) : 0);
    }
    String getString(const char *key, const String &d = String()) {
        uint8_t *v = nullptr; size_t n = 0;
        if (tal_kv_get(k(key).c_str(), &v, &n) != OPRT_OK) return d;
        String s((const char *)v, n);   // length-bounded (value isn't NUL-terminated)
        tal_kv_free(v);
        return s;
    }

    /* ---- bytes ---- */
    size_t putBytes(const char *key, const void *buf, size_t len) {
        return putRaw(key, buf, len);
    }
    size_t getBytesLength(const char *key) {
        uint8_t *v = nullptr; size_t n = 0;
        if (tal_kv_get(k(key).c_str(), &v, &n) != OPRT_OK) return 0;
        tal_kv_free(v); return n;
    }
    size_t getBytes(const char *key, void *buf, size_t maxLen) {
        uint8_t *v = nullptr; size_t n = 0;
        if (tal_kv_get(k(key).c_str(), &v, &n) != OPRT_OK) return 0;
        size_t cp = n < maxLen ? n : maxLen;
        memcpy(buf, v, cp); tal_kv_free(v); return cp;
    }

private:
    std::string k(const char *key) const { return ns_ + ":" + (key ? key : ""); }

    size_t putRaw(const char *key, const void *p, size_t n) {
        if (ro_) return 0;
        return tal_kv_set(k(key).c_str(), (const uint8_t *)p, n) == OPRT_OK ? n : 0;
    }
    template <typename T> T getRaw(const char *key, T d) {
        uint8_t *v = nullptr; size_t n = 0;
        if (tal_kv_get(k(key).c_str(), &v, &n) != OPRT_OK) return d;
        T out = d;
        if (n == sizeof(T)) memcpy(&out, v, sizeof(T));
        tal_kv_free(v);
        return out;
    }

    std::string ns_;
    bool ro_ = false;
};
