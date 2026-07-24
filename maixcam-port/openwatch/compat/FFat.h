/* compat/FFat.h — the on-flash FAT backend, mapped to the Linux data root.
 * begin() just ensures the data directory exists; everything else is fs::FS. */
#pragma once
#include <sys/stat.h>
#include "FS.h"

/* Qualify with the GLOBAL ::fs — a TU that does `using namespace maix` also sees
 * maix::fs, which would make a bare `fs` ambiguous. */
class FFatFS : public ::fs::FS {
public:
    bool begin(bool /*formatOnFail*/ = false, const char * = "/ffat",
               uint8_t = 10, const char * = "ffat") {
        ::mkdir(::fs::data_root().c_str(), 0755);   // idempotent; ok if it already exists
        struct stat st;
        return stat(::fs::data_root().c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    void   end() {}
    size_t totalBytes() { return 24u * 1024 * 1024; }
    size_t usedBytes()  { return 0; }
    size_t freeBytes()  { return totalBytes(); }
    bool   format()     { return true; }
};

extern FFatFS FFat;
