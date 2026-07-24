/* ============================================================================
 *  tuya/compat/FFat.h - the always-present on-flash filesystem, for the T5.
 *
 *  On the ESP boards FFat is a FAT partition on internal flash; the firmware uses it
 *  as the persistence fallback (and, on a no-SD board like the T5, the ONLY file
 *  store): saved WiFi networks, alarms, the notification archive, the sleep log.
 *
 *  Here FFat is an fs::FS (see tuya/compat/FS.h) bound to the T5 internal-flash mount.
 *  begin() mounts it via tkl_fs_mount(mountpoint, DEV_INNER_FLASH) and records the
 *  mountpoint so root-relative paths ("/wifi.csv") resolve under it. The Arduino
 *  begin(formatOnFail, mountpoint, maxFiles, label) signature is accepted; only the
 *  mountpoint is meaningful on the T5 (the platform formats inner flash as needed).
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated per-include in the firmware).
 * ========================================================================== */
#pragma once
#include "FS.h"

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_fs.h"
}

class FFatFS : public fs::FS {
public:
    FFatFS() : fs::FS(String("/ffat")) {}

    /* Arduino-compatible begin(); mountpoint defaults match the firmware's call
     * FFat.begin(true, "/ffat", 10, "ffat"). */
    bool begin(bool /*formatOnFail*/ = false, const char *mountpoint = "/ffat",
               uint8_t /*maxOpenFiles*/ = 10, const char * /*label*/ = "ffat") {
        _setMount(String(mountpoint));
        int rt = tkl_fs_mount(mountpoint, DEV_INNER_FLASH);
        /* 0 == newly mounted; treat an already-mounted result as success too. The
         * firmware only needs "is the flash store usable?" - verify with a cheap
         * is_exist probe on the mount root if the mount call is ambiguous. */
        if (rt == 0) return true;
        BOOL_T ex = FALSE;
        return tkl_fs_is_exist(mountpoint, &ex) == 0;   // mount root reachable -> usable
    }
    void end() {}
    bool format() { return false; }   /* platform manages inner-flash format; not exposed */
};

static FFatFS FFat;
