/* compat/Preferences.h — Arduino NVS (Preferences) shim.
 *
 * Backed by an in-process key/value map (namespace-qualified). Values set in a
 * session read back correctly; they do NOT yet persist across restarts — that
 * becomes a file-backed store later (see maixcam-port/README.md). Enough for the
 * UI to run with live settings.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <map>
#include "Arduino.h"   // String

inline std::map<std::string, std::string> &prefs_store() {
    static std::map<std::string, std::string> m;
    return m;
}

class Preferences {
public:
    bool begin(const char *name, bool readOnly = false) {
        ns_ = name ? name : ""; ro_ = readOnly; return true;
    }
    void end() { ns_.clear(); }
    bool clear() {
        auto &m = prefs_store();
        for (auto it = m.begin(); it != m.end();)
            it = (it->first.rfind(ns_ + "/", 0) == 0) ? m.erase(it) : std::next(it);
        return true;
    }
    bool remove(const char *key) { return prefs_store().erase(k(key)) > 0; }
    bool isKey(const char *key)  { return prefs_store().count(k(key)) > 0; }

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
        prefs_store()[k(key)] = v.str(); return v.length();
    }
    size_t putString(const char *key, const char *v) { return putString(key, String(v)); }
    String getString(const char *key, const String &d = String()) {
        auto &m = prefs_store(); auto it = m.find(k(key));
        return it == m.end() ? d : String(it->second);
    }

    /* ---- bytes ---- */
    size_t putBytes(const char *key, const void *buf, size_t len) {
        prefs_store()[k(key)] = std::string((const char *)buf, len); return len;
    }
    size_t getBytesLength(const char *key) {
        auto &m = prefs_store(); auto it = m.find(k(key));
        return it == m.end() ? 0 : it->second.size();
    }
    size_t getBytes(const char *key, void *buf, size_t maxLen) {
        auto &m = prefs_store(); auto it = m.find(k(key));
        if (it == m.end()) return 0;
        size_t n = it->second.size() < maxLen ? it->second.size() : maxLen;
        memcpy(buf, it->second.data(), n); return n;
    }

private:
    std::string k(const char *key) const { return ns_ + "/" + (key ? key : ""); }
    size_t putRaw(const char *key, const void *p, size_t n) {
        prefs_store()[k(key)] = std::string((const char *)p, n); return n;
    }
    template <typename T> T getRaw(const char *key, T d) {
        auto &m = prefs_store(); auto it = m.find(k(key));
        if (it == m.end() || it->second.size() != sizeof(T)) return d;
        T v; memcpy(&v, it->second.data(), sizeof(T)); return v;
    }
    std::string ns_;
    bool ro_ = false;
};
