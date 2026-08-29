/* FS.h — Arduino fs::FS / fs::File over FatFs (fs_glue.cpp), Gen 6 eMMC.
 * Was a dead stub until 2026-08-04; the API surface is kept identical so
 * every OWF call site compiles unchanged. File is a refcounted handle to a
 * pooled FileImpl (Arduino Files are copied by value all over OWF); the
 * last handle closes the underlying FatFs object. */
#pragma once
#include <Arduino.h>
#include <cstddef>
#include <cstdint>

namespace fs {

struct FileImpl;

class File {
    FileImpl *_i = nullptr;
public:
    File() {}
    explicit File(FileImpl *i);
    File(const File &o);
    File &operator=(const File &o);
    ~File();

    operator bool() const;
    size_t read(uint8_t *buf, size_t n);
    int read();
    size_t write(const uint8_t *buf, size_t n);
    size_t write(uint8_t c);
    int available();
    size_t size();
    bool seek(uint32_t pos);
    size_t position();
    void flush();
    void setTimeout(unsigned long) {}
    void close();
    const char *name();
    const char *path();
    bool isDirectory();
    File openNextFile();

    String readString();
    String readStringUntil(char term);
    size_t readBytes(char *buf, size_t n)   { return read((uint8_t *)buf, n); }
    size_t readBytes(uint8_t *buf, size_t n){ return read(buf, n); }
    size_t readBytesUntil(char term, char *buf, size_t n);
    size_t readBytesUntil(char term, uint8_t *buf, size_t n)
    { return readBytesUntil(term, (char *)buf, n); }

    int print(const char *s);
    int print(const String &s) { return print(s.c_str()); }
    int println(const char *s) { int n = print(s); n += print("\r\n"); return n; }
    int println(const String &s) { return println(s.c_str()); }
    int println() { return print("\r\n"); }
    int printf(const char *fmt, ...);
};

class FS {
public:
    File open(const char *path, const char *mode = "r");
    File open(const String &path, const char *mode = "r")
    { return open(path.c_str(), mode); }
    bool exists(const char *path);
    bool exists(const String &path) { return exists(path.c_str()); }
    bool remove(const char *path);
    bool remove(const String &path) { return remove(path.c_str()); }
    bool rename(const char *from, const char *to);
    bool mkdir(const char *path);
    bool rmdir(const char *path);
    size_t totalBytes();
    size_t usedBytes();
};

} // namespace fs
using fs::FS;
using fs::File;
#define FILE_READ  "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
