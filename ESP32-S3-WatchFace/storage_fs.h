/* ============================================================================
 *  storage_fs.h — shared "SD card if present, else on-flash FAT" filesystem pick.
 *
 *  The board has TWO writable filesystems:
 *    - SD_MMC : a removable microSD card (when one is inserted + mounts).
 *    - FFat   : a ~24 MB on-flash FAT partition (label "ffat"), ALWAYS present.
 *  Every persistent file the watch writes — WiFi credentials, the battery-health
 *  trend log, the notification archive — should PREFER the SD card (removable,
 *  effectively unlimited) but TRANSPARENTLY fall back to the flash partition when
 *  no card is present, so nothing depends on a card being inserted.
 *
 *  This header centralizes that choice so all three stores share one implementation
 *  (it used to live in notif_archive_sd.h and was duplicated/hardcoded to SD_MMC in
 *  the others):
 *    store_fs()        -> the active fs::FS (SD if mounted, else FFat).
 *    store_on_sd()     -> true if SD is the active backend right now.
 *    store_available() -> true if EITHER backend is usable (so a write can proceed).
 *    ffat_mount()      -> lazily mount (and one-time format) the flash partition.
 *
 *  SD takes priority and is re-checked on each call (via sd_mount()), so inserting a
 *  card mid-session resumes using it; FFat is mounted lazily the first time it's the
 *  fallback. Header-only, in the .ino TU. INCLUDE AFTER sd_card.h (sd_mount/SD_MMC)
 *  and BEFORE the stores that use it (batt_health_sd.h, wifi_store.h,
 *  notif_archive_sd.h).
 * ========================================================================== */
#pragma once
#include <FS.h>
#include <FFat.h>
#include "sd_card.h"

static bool s_ffat_tried = false;   // have we attempted to mount FFat this boot?
static bool s_ffat_ok    = false;   // is FFat mounted and usable?

/* Lazily mount the on-flash FAT partition. format-on-fail = true: a fresh device has
 * a blank ffat partition; format it once so the first write succeeds. Label "ffat"
 * matches partitions.csv. Mounted at most once per boot (result latched). */
static bool ffat_mount(void) {
  if (s_ffat_tried) return s_ffat_ok;
  s_ffat_tried = true;
  s_ffat_ok = FFat.begin(true, "/ffat", 10, "ffat");
  USBSerial.printf("[ffat] mount %s\n", s_ffat_ok ? "OK (flash storage ready)" : "FAILED");
  return s_ffat_ok;
}

/* True if the SD card is the active backend right now (mounted). When false but FFat
 * is up, the flash partition is the backend. Re-checked each call so a card inserted
 * mid-session is picked up. */
static bool store_on_sd(void) { return sd_mount(); }

/* The active filesystem: SD card if mounted, else the flash FAT partition. Callers
 * use store_available() first (or check FILE handles) since FFat must mount. */
static fs::FS &store_fs(void) {
  if (store_on_sd()) return (fs::FS &)SD_MMC;
  return (fs::FS &)FFat;
}

/* Is a writable backend usable at all? True if SD is mounted OR the flash partition
 * mounts. SD is preferred; FFat is the always-available fallback. */
static bool store_available(void) { return store_on_sd() || ffat_mount(); }
