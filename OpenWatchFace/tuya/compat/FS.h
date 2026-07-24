/* ============================================================================
 *  tuya/compat/FS.h - ESP32-style Arduino fs::FS / fs::File over the REAL T5 FS
 *  (tkl_fopen/fread/fwrite/fseek + tkl_dir_* + tkl_fs_*).
 *
 *  The firmware uses the ESP32 Arduino FS API: `fs::File f = fs.open("/x", FILE_WRITE);
 *  f.println(...); f.read(buf,n); if (f) {...}; f.close();` plus directory iteration
 *  (openNextFile/isDirectory/name) for the Files app. The TuyaOpen core ships a
 *  DIFFERENT, incompatible FS class (raw TUYA_FILE handles), so we provide the
 *  fs:: namespace ourselves, backed by the tkl_fs primitives. File derives from Print
 *  so print/println/printf/write(buf,n) come for free.
 *
 *  Paths: each FS carries a mountpoint prefix (e.g. "/ffat"); the firmware passes
 *  root-relative paths ("/wifi.csv") which we join to that prefix before tkl_fopen.
 *  FFat.h binds an FS to the internal-flash mount; sd_card.h points sd_fs() here too.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated per-include in the firmware).
 * ========================================================================== */
#pragma once
#include <Arduino.h>     // Print, String
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdio>

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_fs.h"
}

/* Arduino open-mode tokens (the firmware passes these to open()). */
#ifndef FILE_READ
#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"
#endif

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

namespace fs {

class File : public Print {
public:
    File() {}
    File(TUYA_FILE f, const String &path, bool isDir = false, TUYA_DIR dir = nullptr)
        : _f(f), _dir(dir), _path(path), _isDir(isDir) {}

    /* ---- Print: implement write(); print/println/printf are inherited -------- */
    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t *buf, size_t n) override {
        if (!_f) return 0;
        int w = tkl_fwrite((void *)buf, (int)n, _f);
        return w > 0 ? (size_t)w : 0;
    }
    using Print::write;

    /* printf is absent from this core's Print - provide it (line-buffered). */
    int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char stackbuf[160];
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
        va_end(ap);
        if (n < 0) return 0;
        if ((size_t)n < sizeof(stackbuf)) { write((const uint8_t *)stackbuf, (size_t)n); return n; }
        char *heap = (char *)malloc((size_t)n + 1);
        if (!heap) { write((const uint8_t *)stackbuf, sizeof(stackbuf) - 1); return (int)sizeof(stackbuf) - 1; }
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)n + 1, fmt, ap);
        va_end(ap);
        write((const uint8_t *)heap, (size_t)n);
        free(heap);
        return n;
    }

    /* ---- read ---------------------------------------------------------------- */
    int read() { uint8_t b; return (read(&b, 1) == 1) ? b : -1; }
    int read(uint8_t *buf, size_t n) {
        if (!_f) return -1;
        int r = tkl_fread((void *)buf, (int)n, _f);
        return r;
    }
    size_t readBytes(char *buf, size_t n) { int r = read((uint8_t *)buf, n); return r > 0 ? (size_t)r : 0; }
    /* Read up to len bytes, stopping after `terminator` (which is consumed but NOT
     * stored) or at EOF. Mirrors Arduino Stream::readBytesUntil - used by the CSV
     * readers (notif archive, sleep log). Byte-at-a-time (files are small lines). */
    size_t readBytesUntil(char terminator, char *buf, size_t len) {
        size_t i = 0; int c;
        while (i < len && (c = read()) >= 0) {
            if ((char)c == terminator) break;
            buf[i++] = (char)c;
        }
        return i;
    }
    /* Stream read-timeout: irrelevant for file reads (they never block), so a no-op.
     * The firmware calls setTimeout(0) so EOF returns immediately - already our behavior. */
    void setTimeout(uint32_t /*ms*/) {}
    int available() {
        if (!_f) return 0;
        return tkl_feof(_f) ? 0 : 1;   // coarse: nonzero until EOF (enough for the firmware's while(available) loops)
    }
    int peek() { return -1; }   /* not used by the firmware on files */

    String readStringUntil(char terminator) {
        String s;
        int c;
        while ((c = read()) >= 0) {
            if ((char)c == terminator) break;
            s += (char)c;
        }
        return s;
    }

    /* ---- positioning --------------------------------------------------------- */
    bool seek(uint32_t pos, SeekMode mode = SeekSet) {
        if (!_f) return false;
        return tkl_fseek(_f, (int64_t)pos, (int)mode) == 0;
    }
    size_t position() { return _f ? (size_t)tkl_ftell(_f) : 0; }
    size_t size() {
        if (_path.length() == 0) return 0;
        int sz = tkl_fgetsize(_path.c_str());
        return sz > 0 ? (size_t)sz : 0;
    }

    void flush() override { if (_f) tkl_fflush(_f); }
    void close() {
        if (_f)   { tkl_fclose(_f);   _f = nullptr; }
        if (_dir) { tkl_dir_close(_dir); _dir = nullptr; }
    }

    const char *name() { return _path.c_str(); }
    const char *path() { return _path.c_str(); }
    bool isDirectory() { return _isDir; }
    operator bool() const { return _f != nullptr || _dir != nullptr; }

    /* ---- directory iteration (Files app) ------------------------------------- */
    File openNextFile(const char *mode = FILE_READ);

private:
    TUYA_FILE _f   = nullptr;
    TUYA_DIR  _dir = nullptr;
    String    _path;
    bool      _isDir = false;
};

class FS {
public:
    FS() {}
    explicit FS(const String &mountpoint) : _mp(mountpoint) {}

    void _setMount(const String &mp) { _mp = mp; }

    File open(const char *path, const char *mode = FILE_READ) {
        String full = _join(path);
        /* A directory open: the firmware opens a dir then calls openNextFile(). Detect
         * via tkl_dir_open succeeding; otherwise it's a regular file via tkl_fopen. */
        TUYA_DIR dir = nullptr;
        if (strcmp(mode, FILE_READ) == 0 && tkl_dir_open(full.c_str(), &dir) == 0 && dir) {
            return File(nullptr, full, /*isDir=*/true, dir);
        }
        TUYA_FILE f = tkl_fopen(full.c_str(), mode);
        if (!f) return File();
        return File(f, full, false, nullptr);
    }
    File open(const String &path, const char *mode = FILE_READ) { return open(path.c_str(), mode); }

    bool exists(const char *path) {
        BOOL_T ex = FALSE;
        return tkl_fs_is_exist(_join(path).c_str(), &ex) == 0 && ex;
    }
    bool exists(const String &path) { return exists(path.c_str()); }
    bool remove(const char *path) { return tkl_fs_remove(_join(path).c_str()) == 0; }
    bool remove(const String &path) { return remove(path.c_str()); }
    bool rename(const char *a, const char *b) { return tkl_fs_rename(_join(a).c_str(), _join(b).c_str()) == 0; }
    bool mkdir(const char *path) { return tkl_fs_mkdir(_join(path).c_str()) == 0; }

    uint64_t totalBytes() { return 0; }   /* tkl_fs has no df; About screen shows 0 (cosmetic) */
    uint64_t usedBytes()  { return 0; }

    const String &mountpoint() const { return _mp; }

protected:
    String _join(const char *path) const {
        if (!path || !path[0]) return _mp;
        if (_mp.length() == 0) return String(path);
        if (path[0] == '/') return _mp + path;
        return _mp + "/" + path;
    }
    String _mp;
};

/* openNextFile needs FS::_join semantics for the child path; implemented out-of-line
 * so File can build full child paths the same way. */
inline File File::openNextFile(const char *mode) {
    if (!_dir) return File();
    TUYA_FILEINFO info = nullptr;
    if (tkl_dir_read(_dir, &info) != 0 || !info) return File();
    const char *cname = nullptr;
    if (tkl_dir_name(info, &cname) != 0 || !cname) return File();
    BOOL_T isdir = FALSE;
    tkl_dir_is_directory(info, &isdir);
    String child = _path + "/" + cname;
    if (isdir) return File(nullptr, child, true, nullptr);
    TUYA_FILE f = tkl_fopen(child.c_str(), mode);
    return File(f, child, false, nullptr);
}

} // namespace fs

using fs::File;
using fs::FS;
/* SeekMode/SeekSet/etc. are already at global scope (above). */
