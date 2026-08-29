/* FFat.h — Arduino FFat API over the Gen 6 userdata FFAT region (FatFs).
 * begin() lazily brings up the whole storage stack (eMMC -> GPT -> window ->
 * superblock -> mount, formatting the region on first use). */
#pragma once
#include "FS.h"
class FFatFS : public fs::FS {
public:
    bool begin(bool format_on_fail = false, const char * = "/ffat",
               uint8_t = 10, const char * = "ffat");
    void end();
    bool format();
    size_t totalBytes();
    size_t usedBytes();
    size_t freeBytes();
};
extern FFatFS FFat;
