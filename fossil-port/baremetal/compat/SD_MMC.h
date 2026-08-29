/* SD_MMC.h — SDMMC STUB (no SD on the sealed Gen 6; eMMC storage is Phase 5). */
#pragma once
#include "FS.h"
class SDMMCFS : public fs::FS {
public:
    bool begin(const char* = "/sdcard", bool = false) { return false; }
    bool setPins(int,int,int) { return false; }
    bool setPins(int,int,int,int,int,int) { return false; }
    void end() {}
    int cardType() { return 0; }   /* CARD_NONE */
    uint64_t cardSize() { return 0; }
    uint64_t totalBytes() { return 0; }
    uint64_t usedBytes() { return 0; }
};
extern SDMMCFS SD_MMC;
#define CARD_NONE 0
