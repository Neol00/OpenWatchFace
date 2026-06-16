/* ============================================================================
 *  sd_card.h — shared lazy microSD mount (one begin() for the whole sketch).
 *
 *  The watch's removable storage. Two bus types, picked by the board header:
 *    - BOARD_HAS_SD_MMC (S3-2.06): 1-bit SDMMC (SDMMC_CLK/CMD/DATA), via SD_MMC.
 *    - BOARD_HAS_SD_SPI (C6-1.47): SD over SPI (SD_SPI_SCK/MISO/MOSI/CS), via SD.
 *  All consumers go through the board-neutral surface:
 *    sd_mount()      -> true if the card is mounted (lazily mounts, retries).
 *    sd_present()    -> true if currently mounted.
 *    sd_fs()         -> the active fs::FS for the card (storage_fs.h hands this
 *                       out as the SD backend; never name SD_MMC/SD elsewhere).
 *    sd_card_type()  -> the CARD_* type code (app_settings.h "card info").
 *    sd_card_size_bytes() / sd_total_bytes() / sd_used_bytes()
 *    sd_retry_tick() -> non-blocking background mount retry.
 *
 *  The WiFi-credential CSV (wifi_store.h), the battery-health log (batt_health_sd.h)
 *  and the notification archive (notif_archive_sd.h) share this single mount, so the
 *  card is begun at most once per boot. Absence is latched so we never retry-spam the
 *  bus when no card is in.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE BEFORE storage_fs.h and the
 *  stores. Needs the SD pins from the board header (included earlier).
 * ========================================================================== */
#pragma once

#if !BOARD_HAS_SD_MMC && !BOARD_HAS_SD_SPI
/* No SD slot on this board. Same API, never mounts; every SD consumer already
 * degrades to its flash (FFat) fallback when sd_mount() is false. sd_fs() must
 * still return a reference (it's only dereferenced when sd_mount() is true, so
 * it's never actually used here) — point it at FFat to keep the type valid. */
#include <FFat.h>
/* CARD_* type codes (from the SD libs we don't include here) so a consumer's
 * card-type switch still COMPILES on a no-SD board — it's dead code there, since
 * sd_present() is always false. Values mirror sdmmc_types.h. */
#ifndef CARD_NONE
enum { CARD_NONE = 0, CARD_MMC = 1, CARD_SD = 2, CARD_SDHC = 3, CARD_UNKNOWN = 4 };
#endif
static bool      sd_mount(void)            { return false; }
static bool      sd_present(void)          { return false; }
static fs::FS   &sd_fs(void)               { return (fs::FS &)FFat; }
static int       sd_card_type(void)        { return 0; /* CARD_NONE */ }
static uint64_t  sd_card_size_bytes(void)  { return 0; }
static uint64_t  sd_total_bytes(void)      { return 0; }
static uint64_t  sd_used_bytes(void)       { return 0; }
static void      sd_retry_tick(uint32_t)   {}
static inline void sd_set_bus_ready(void)  {}   // no-op (no SD slot)
static bool      sd_format(void)           { return false; }

#else  /* a real SD slot — SDMMC or SPI -------------------------------------- */

#if BOARD_HAS_SD_MMC
#include <SD_MMC.h>
#define SD_DEV  SD_MMC          // the fs object for this bus
#else
#include <SPI.h>
#include <SD.h>
#define SD_DEV  SD
#endif

static bool    s_sd_mounted   = false;
static bool    s_sd_absent    = false;  // latched ONLY after several failed attempts
static uint8_t s_sd_tries     = 0;      // failed mount attempts so far this boot

/* Set true by the .ino right after gfx->begin(). On a board where the SD shares the
 * display's SPI bus (BOARD_HAS_SD_SPI), the SD must not be mounted until the display
 * has initialized the bus — so sd_mount() waits on this. (Always-true effect on
 * SDMMC boards, which have a dedicated bus; the .ino sets it regardless.) */
static bool    s_gfx_ready    = false;
static inline void sd_set_bus_ready(void) { s_gfx_ready = true; }

/* How many mount attempts may fail before we give up and latch "absent". The first
 * couple happen very early in setup() (wifi_nets_load / na_seed_total), BEFORE the
 * PMU rails are restored, so they fail on an unpowered card; the rest are the loop's
 * background retries (every SD_RETRY_MS). Latching "absent" on the first failure (the
 * old behavior) made a PRESENT card report "no card" all session. With ~8 tries and
 * a 400 ms retry spacing that's a generous ~3 s window for a slow card/rail to come
 * up — yet for a truly absent card the cost is just that many quick failed begin()s,
 * then silence (retries stop once latched). */
#define SD_MAX_TRIES  8

#if BOARD_HAS_SD_MMC
/* Init at a conservative clock. A lower probe-friendly clock makes the first
 * handshake far more dependable on early boot / a marginal rail. (Throughput
 * here is tiny — CSV logs.) */
#ifndef SD_MMC_FREQ
#define SD_MMC_FREQ  SDMMC_FREQ_DEFAULT   // ~20 MHz; well within spec, robust at init
#endif
#endif

/* Mount the card if needed. Returns true if mounted (or already mounted). Safe to
 * call repeatedly: it retries a present-but-not-yet-ready card and only latches
 * "absent" after SD_MAX_TRIES genuine failures. */
static bool sd_mount(void) {
  if (s_sd_mounted) return true;
  if (s_sd_absent)  return false;

#if BOARD_HAS_SD_MMC
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  // 1-bit mode (this board), don't format on failure, conservative clock.
  bool ok = SD_MMC.begin("/sdcard", true, false, SD_MMC_FREQ);
#else
  // SPI card sharing the LCD's SPI bus (C6-1.47). The DISPLAY owns the bus setup:
  // its Arduino_HWSPI does SPI.begin() with MISO (LCD_MISO) in gfx->begin(). We must
  // NOT do our own SPI.begin() — a second begin() while the display is initialized
  // fights it. So we only mount AFTER the display bus is up, and never SPI.end()/
  // SD.end() (that would tear down the shared bus and kill the display).
  if (!s_gfx_ready) {
    // Don't count this as a failed attempt — the bus just isn't up yet.
    return false;   // wait until gfx->begin() has set up the shared SPI bus
  }
  // Mount at a conservative 4 MHz (SD-init safe). format_if_empty=FALSE on purpose:
  // the ESP32 FATFS reports FR_NO_FILESYSTEM for a card Windows wrote as STANDARD
  // FAT32 (the layout it rejects) — even though that card is full of the user's
  // files. Auto-formatting on that error would SILENTLY WIPE a good card, so we
  // never do it here. Reformatting is a deliberate, confirmed action in the Files
  // app instead (sd_format()). (Args: ssPin, spi, freq, mountpoint, max_files,
  // format_if_empty.)
  bool ok = SD.begin(SD_SPI_CS, SPI, 4000000, "/sd", 5, false);
  if (!ok) {
    // Split the failure WITHOUT needing Core Debug Level: cardType()/cardSize()
    // are populated by the card-level init (CMD0/CMD8/ACMD41) even when the
    // filesystem mount fails. So:
    //   cardType != NONE  -> the card RESPONDS on the bus; failure is the
    //                        FILESYSTEM (format) — and format_if_empty should have
    //                        reformatted it; if it still fails, f_mkfs failed.
    //   cardType == NONE  -> the card NEVER RESPONDED — a BUS/SIGNAL/CS problem
    //                        (wiring, pull-up, clock, or the shared display bus),
    //                        NOT a format issue.
    int ct = SD.cardType();
    uint64_t sz = SD.cardSize();
    USBSerial.printf("[sd] mount failed: cardType=%d size=%lluMB -> %s\n",
                     ct, sz / (1024ULL * 1024ULL),
                     ct != 0 ? "card RESPONDS (filesystem/format problem)"
                             : "card SILENT (bus/signal/CS problem, not format)");
  }
#endif
  if (ok && SD_DEV.cardType() != 0 /* CARD_NONE */) {
    s_sd_mounted = true;
    USBSerial.println("[sd] card mounted");
    return true;
  }

#if BOARD_HAS_SD_MMC
  // Failed this attempt. end() so the next try starts the peripheral clean.
  // (SDMMC has its own dedicated bus, so ending it is safe.)
  SD_DEV.end();
#else
  // SPI path: do NOT end() — that would tear down the shared display bus. The SD
  // library leaves its CS deselected on a failed begin, so a retry is fine.
#endif
  if (++s_sd_tries >= SD_MAX_TRIES) {
    s_sd_absent = true;                  // give up only after repeated failures
    USBSerial.printf("[sd] no card after %u tries -> SD features disabled "
                     "(flash fallbacks still work)\n", (unsigned)s_sd_tries);
  } else {
    USBSerial.printf("[sd] mount attempt %u failed; will retry\n", (unsigned)s_sd_tries);
  }
  return false;
}

static bool      sd_present(void)         { return s_sd_mounted; }
static fs::FS   &sd_fs(void)              { return (fs::FS &)SD_DEV; }

/* DESTRUCTIVE: reformat the inserted card to a fresh FAT volume the ESP32 can
 * mount. Used ONLY from a deliberate, confirmed user action (Files app) — never
 * automatically — because the ESP32 FATFS reports "no filesystem" for a card
 * Windows wrote as STANDARD FAT32 even though it's full of the user's files;
 * auto-formatting that would wipe good data. This is the manual rescue for a
 * card that won't mount (e.g. one formatted by the Windows GUI). Returns true if
 * the card formatted AND mounted afterwards. Clears the absent-latch so a freshly
 * formatted card is picked up. */
static bool sd_format(void) {
  s_sd_mounted = false;
  s_sd_absent  = false;
  s_sd_tries   = 0;
#if BOARD_HAS_SD_MMC
  // format_if_empty path doesn't apply to SDMMC begin the same way; reuse begin
  // with format=true (3rd arg). 1-bit mode, format-on-empty TRUE, conservative clk.
  bool ok = SD_MMC.begin("/sdcard", true, true, SD_MMC_FREQ);
#else
  if (!s_gfx_ready) return false;            // bus not up yet
  // begin() with format_if_empty=TRUE: forces a FAT volume onto a card the ESP32
  // can't otherwise mount. The user already confirmed this is destructive.
  bool ok = SD.begin(SD_SPI_CS, SPI, 4000000, "/sd", 5, true);
#endif
  if (ok && SD_DEV.cardType() != 0) {
    s_sd_mounted = true;
    USBSerial.println("[sd] card formatted + mounted");
    return true;
  }
  USBSerial.println("[sd] format failed (no card responding, or f_mkfs error)");
  return false;
}
static int       sd_card_type(void)       { return SD_DEV.cardType(); }
static uint64_t  sd_card_size_bytes(void) { return SD_DEV.cardSize(); }
static uint64_t  sd_total_bytes(void)     { return SD_DEV.totalBytes(); }
static uint64_t  sd_used_bytes(void)      { return SD_DEV.usedBytes(); }

/* Non-blocking background retry, called from the main loop with millis(). The first
 * mount attempts run during early boot before the SD rail is powered, so they fail;
 * rather than blocking boot to wait for the rail, we retry here every SD_RETRY_MS as
 * the watch runs normally. Stops as soon as the card mounts (or sd_mount latches
 * "absent" after SD_MAX_TRIES). Each failed begin() costs a little time, so we space
 * the attempts out instead of hammering every loop iteration. */
#define SD_RETRY_MS  400
static void sd_retry_tick(uint32_t now_ms) {
  if (s_sd_mounted || s_sd_absent) return;   // settled either way -> nothing to do
  static uint32_t last = 0;
  if (now_ms - last < SD_RETRY_MS) return;
  last = now_ms;
  sd_mount();                                 // one attempt; updates state + logs
}

#undef SD_DEV
#endif  /* SD slot present */
