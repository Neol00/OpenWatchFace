/* ============================================================================
 *  compat/FS.h — Arduino fs::FS / fs::File over the Linux filesystem.
 *
 *  The firmware writes all its persistent files (settings CSV, WiFi creds, notif
 *  archive, sleep log, …) through the Arduino fs::FS API, normally backed by an
 *  on-flash FAT partition (FFat) or an SD card. Here those map to real files under
 *  a data root on the host (default $HOME/.openwatchface, override with
 *  $OPENWATCH_DATA_DIR). Paths are used as-is under that root (e.g. "/wifi.csv").
 * ========================================================================== */
#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <string>
#include <memory>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "Arduino.h"   // String

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

namespace fs {

inline const std::string &data_root() {
    static std::string root = [] {
        const char *e = getenv("OPENWATCH_DATA_DIR");
        if (e && *e) return std::string(e);
        const char *h = getenv("HOME");
        return std::string(h ? h : ".") + "/.openwatchface";
    }();
    return root;
}
inline std::string host_path(const char *p) {
    std::string s = p ? p : "";
    if (!s.empty() && s[0] != '/') s = "/" + s;
    return data_root() + s;
}

class File {
public:
    File() {}
    File(FILE *f, const std::string &name) : f_(f, &fclose), name_(name) {}
    File(DIR *d, const std::string &name) : d_(d, &closedir), name_(name), dir_(true) {}

    explicit operator bool() const { return f_ != nullptr || d_ != nullptr; }

    size_t write(uint8_t b)                 { return f_ ? fwrite(&b, 1, 1, f_.get()) : 0; }
    size_t write(const uint8_t *p, size_t n){ return f_ ? fwrite(p, 1, n, f_.get()) : 0; }

    size_t print(const char *s)   { return f_ && s ? fwrite(s, 1, strlen(s), f_.get()) : 0; }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t print(char c)          { return write((uint8_t)c); }
    size_t print(int v)           { return f_ ? fprintf(f_.get(), "%d", v) : 0; }
    size_t print(unsigned v)      { return f_ ? fprintf(f_.get(), "%u", v) : 0; }
    size_t print(long v)          { return f_ ? fprintf(f_.get(), "%ld", v) : 0; }
    size_t print(unsigned long v) { return f_ ? fprintf(f_.get(), "%lu", v) : 0; }
    size_t print(double v)        { return f_ ? fprintf(f_.get(), "%f", v) : 0; }

    size_t println(const char *s)   { return print(s) + print("\n"); }
    size_t println(const String &s) { return println(s.c_str()); }
    size_t println()                { return print("\n"); }
    template <class T> size_t println(T v) { return print(v) + print("\n"); }

    int  read()                  { return f_ ? fgetc(f_.get()) : -1; }
    int  read(uint8_t *buf, size_t n) { return f_ ? (int)fread(buf, 1, n, f_.get()) : -1; }

    int  printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!f_) return 0;
        va_list ap; va_start(ap, fmt);
        int r = vfprintf(f_.get(), fmt, ap);
        va_end(ap);
        return r;
    }
    /* Read up to a delimiter (consumed, not stored). Arduino Stream semantics. */
    String readStringUntil(char term) {
        std::string out;
        if (!f_) return String();
        int ch;
        while ((ch = fgetc(f_.get())) != EOF && (char)ch != term) out += (char)ch;
        return String(out);
    }
    int readBytesUntil(char term, char *buf, size_t len) {
        if (!f_) return 0;
        size_t i = 0; int ch;
        while (i < len && (ch = fgetc(f_.get())) != EOF && (char)ch != term) buf[i++] = (char)ch;
        return (int)i;
    }
    void setTimeout(uint32_t) {}   /* no-op: local files never block */
    int  available()             { if (!f_) return 0; long c = ftell(f_.get()); fseek(f_.get(), 0, SEEK_END);
                                   long e = ftell(f_.get()); fseek(f_.get(), c, SEEK_SET); return (int)(e - c); }
    size_t size()                { if (!f_) return 0; long c = ftell(f_.get()); fseek(f_.get(), 0, SEEK_END);
                                   long e = ftell(f_.get()); fseek(f_.get(), c, SEEK_SET); return (size_t)e; }
    bool   seek(uint32_t pos)    { return f_ ? fseek(f_.get(), pos, SEEK_SET) == 0 : false; }
    size_t position()            { return f_ ? (size_t)ftell(f_.get()) : 0; }
    void   flush()               { if (f_) fflush(f_.get()); }
    void   close()               { f_.reset(); d_.reset(); }

    const char *name() const     { return name_.c_str(); }
    const char *path() const     { return name_.c_str(); }
    bool   isDirectory() const   { return dir_; }
    File   openNextFile(const char * = "r");

private:
    std::shared_ptr<FILE> f_;
    std::shared_ptr<DIR>  d_;
    std::string name_;
    bool dir_ = false;
};

class FS {
public:
    File open(const char *path, const char *mode = "r", bool = false) {
        std::string hp = host_path(path);
        struct stat st;
        if (stat(hp.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *d = opendir(hp.c_str());
            return d ? File(d, path) : File();
        }
        FILE *f = fopen(hp.c_str(), mode);
        return f ? File(f, path) : File();
    }
    bool exists(const char *path) { struct stat st; return stat(host_path(path).c_str(), &st) == 0; }
    bool remove(const char *path) { return ::remove(host_path(path).c_str()) == 0; }
    bool mkdir(const char *path)  { return ::mkdir(host_path(path).c_str(), 0755) == 0; }
    bool rmdir(const char *path)  { return ::rmdir(host_path(path).c_str()) == 0; }
    bool rename(const char *a, const char *b) { return ::rename(host_path(a).c_str(), host_path(b).c_str()) == 0; }
};

inline File File::openNextFile(const char *) {
    if (!d_) return File();
    struct dirent *de;
    while ((de = readdir(d_.get())) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        std::string child = name_ + "/" + de->d_name;
        FS fs; return fs.open(child.c_str(), "r");
    }
    return File();
}

} // namespace fs

/* The Arduino-ESP32 core exposes File/FS unqualified at global scope; the firmware
 * uses `File f = ...` and `fs::FS&` interchangeably. Mirror that. Qualify with the
 * GLOBAL ::fs (a TU doing `using namespace maix` also sees maix::fs → ambiguous). */
using ::fs::File;
using ::fs::FS;
