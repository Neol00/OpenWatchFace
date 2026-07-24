/* ============================================================================
 *  tuya/compat/SDCard.h - removable microSD on the T5 (4-bit SDIO), as an fs::FS.
 *
 *  The T5-E1 board has a microSD slot on the BK7258's 4-bit SDIO bus
 *  (CLK=IO2 CMD=IO3 D0-D3=IO4/5/10/11, card-detect=IO8 active-LOW). The TuyaOpen SDK
 *  brings the card up entirely inside tkl_fs_mount(path, DEV_SDCARD): it configures the
 *  SDIO pins, initializes the card, and mounts FATFS at the given path. So mounting is a
 *  single call; we expose it as an fs::FS (see tuya/compat/FS.h) bound to "/sdcard", the
 *  SAME bridge the on-flash FFat uses — every File/dir op then routes through tkl_fs_*.
 *
 *  Presence + capacity come from the BK SD-card driver (bk_sd_card_get_card_state /
 *  _get_card_size), and the schematic's card-detect line (IO8) gives a cheap "is a card
 *  physically inserted?" probe so we don't hammer the SDIO bus mounting when the slot is
 *  empty. sd_card.h drives the lazy-mount / retry / absence-latch policy on top of this.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated per-include in the firmware).
 * ========================================================================== */
#pragma once
#include "FS.h"

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_fs.h"
#include "tkl_gpio.h"
/* BK SD-card driver: presence/state + capacity. Declared directly (the bk_private headers
 * aren't on the Arduino include path); symbols link from libdriver.a. */
typedef enum {
  SDC_IDLE = 0, SDC_READY, SDC_IDENT, SDC_STANDBY, SDC_TRANSFER,
  SDC_SENDING, SDC_RECEIVING, SDC_PROGRAMMING, SDC_DISCONNECTED, SDC_ERROR = 0xff,
} owf_sd_state_t;                       /* mirrors the SDK's sd_card_state_t */
owf_sd_state_t bk_sd_card_get_card_state(void);
uint32_t       bk_sd_card_get_card_size(void);   /* capacity in 512-byte SECTORS (== FATFS sector
                                                  * count / USB-MSC block count; see disk_io.c,
                                                  * usbd_msc.c). bytes = size * 512. */
}

/* Card-detect (CD) on IO8, active-LOW (a seated card pulls it to GND). */
#define OWF_T5_SD_CD_PIN   TUYA_GPIO_NUM_8
#define OWF_T5_SD_MOUNT    "/sdcard"

class SDCardFS : public fs::FS {
public:
  SDCardFS() : fs::FS(String(OWF_T5_SD_MOUNT)) {}

  /* Is a card physically in the slot? Reads the CD line (active-LOW). Cheap — no SDIO
   * traffic — so sd_card.h can gate mount attempts on it. Re-inits the pin each call so
   * the read survives the CPU light-sleep that can gate a once-configured GPIO clock. */
  static bool cardInserted(void) {
    TUYA_GPIO_BASE_CFG_T in = {};
    in.mode   = TUYA_GPIO_PULLUP;        // CD floats high (no card), card pulls it low
    in.direct = TUYA_GPIO_INPUT;
    tkl_gpio_init(OWF_T5_SD_CD_PIN, &in);
    TUYA_GPIO_LEVEL_E lvl = TUYA_GPIO_LEVEL_HIGH;
    tkl_gpio_read(OWF_T5_SD_CD_PIN, &lvl);
    return (lvl == TUYA_GPIO_LEVEL_LOW);
  }

  /* Mount the card's FATFS at /sdcard. tkl_fs_mount(DEV_SDCARD) does the SDIO + card init
   * internally. Returns true once the mount root is reachable. Idempotent: a second mount
   * of an already-mounted path is treated as success. */
  bool begin(void) {
    if (_mounted) return true;
    if (!cardInserted()) return false;            // empty slot -> don't touch the SDIO bus
    int rt = tkl_fs_mount(OWF_T5_SD_MOUNT, DEV_SDCARD);
    if (rt != 0) {
      BOOL_T ex = FALSE;                          // ambiguous rt -> probe the mount root
      if (!(tkl_fs_is_exist(OWF_T5_SD_MOUNT, &ex) == 0 && ex)) return false;
    }
    _mounted = true;
    return true;
  }

  void end(void) {
    if (!_mounted) return;
    tkl_fs_unmount(OWF_T5_SD_MOUNT);
    _mounted = false;
  }

  bool mounted(void) const { return _mounted; }

  /* The card is usable iff it's mounted and NOT in a hard fault state. Only DISCONNECTED
   * (card yanked) and ERROR mean trouble — IDLE / STANDBY / READY / TRANSFER are all NORMAL
   * states (an idle mounted card resting between accesses reports IDLE/STANDBY, which must
   * NOT be treated as "no card"; that was the About-screen "no SD" bug while it worked fine
   * elsewhere). The card-detect line (cardInserted) is the real "is it physically there". */
  bool healthy(void) const {
    if (!_mounted) return false;
    owf_sd_state_t s = bk_sd_card_get_card_state();
    return (s != SDC_DISCONNECTED && s != SDC_ERROR);
  }

  /* Capacity in bytes. bk_sd_card_get_card_size() returns the 512-byte sector count. */
  uint64_t totalBytes(void) const {
    if (!_mounted) return 0;
    return (uint64_t)bk_sd_card_get_card_size() * 512ULL;
  }

  /* USED bytes — estimated by SUMMING every file's size (tkl_fs has no df/free-space API).
   * Walks the tree from the mount root via the fs:: dir bridge. This is an APPROXIMATION:
   * it counts file content, not FAT cluster slack or directory-entry overhead, so it slightly
   * under-reports vs the FAT's true allocation — fine for the About screen's "X of Y used".
   * Bounded recursion (OWF_SD_USED_MAXDEPTH) so a pathological tree can't blow the stack; one
   * dir handle open at a time per level. Call sparingly (it's O(files), opens each file to
   * read its size) — the About screen reads it once on open. */
  uint64_t usedBytes(void) {
    if (!_mounted) return 0;
    return walkSize(String(OWF_T5_SD_MOUNT), 0);   // start at the absolute mount root
  }

private:
  #define OWF_SD_USED_MAXDEPTH 8
  /* Sum file sizes under the ABSOLUTE directory `full` recursively, using the raw tkl_dir_*
   * primitives on full paths (no prefix join/strip games). Skips "." / ".." so it can't loop. */
  uint64_t walkSize(const String &full, int depth) {
    if (depth > OWF_SD_USED_MAXDEPTH) return 0;
    TUYA_DIR dir = nullptr;
    if (tkl_dir_open(full.c_str(), &dir) != 0 || !dir) return 0;
    uint64_t sum = 0;
    for (;;) {
      TUYA_FILEINFO info = nullptr;
      if (tkl_dir_read(dir, &info) != 0 || !info) break;     // end of directory
      const char *name = nullptr;
      if (tkl_dir_name(info, &name) != 0 || !name) continue;
      if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        continue;                                            // skip "." and ".."
      String child = full + "/" + name;
      BOOL_T isdir = FALSE;
      tkl_dir_is_directory(info, &isdir);
      if (isdir) sum += walkSize(child, depth + 1);
      else {
        int sz = tkl_fgetsize(child.c_str());
        if (sz > 0) sum += (uint64_t)sz;
      }
    }
    tkl_dir_close(dir);
    return sum;
  }

public:

private:
  bool _mounted = false;
};

static SDCardFS SDCard;
