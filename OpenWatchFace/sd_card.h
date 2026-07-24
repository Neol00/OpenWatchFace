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

#if BOARD_HAS_SD_TUYA
/* ===== Tuya T5: microSD on 4-bit SDIO via the TuyaOpen SDK (tkl_fs DEV_SDCARD) ==========
 * The SDK's tkl_fs_mount("/sdcard", DEV_SDCARD) brings up the SDIO bus + card + FATFS in one
 * call; tuya/compat/SDCard.h wraps that as an fs::FS (the SAME File/dir bridge FFat uses) plus
 * card-detect (IO8) and capacity (bk_sd_card_get_card_size). This file just adds the firmware's
 * board-neutral surface + lazy-mount / retry / absence-latch policy on top, so storage_fs.h,
 * the Files app, the notif archive, batt-health and the sleep log all use the card unchanged. */
#include "tuya/compat/SDCard.h"

#ifndef CARD_NONE
enum { CARD_NONE = 0, CARD_MMC = 1, CARD_SD = 2, CARD_SDHC = 3, CARD_UNKNOWN = 4 };
#endif

static bool    s_sd_mounted = false;
static bool    s_sd_absent  = false;   // latched after several failed attempts with no card
static uint8_t s_sd_tries   = 0;

/* Lazily mount the card. Cheap CD probe first (no SDIO traffic when the slot is empty), then
 * the SDK mount. Latches "absent" after a few failed tries so we don't retry-spam the bus; a
 * later insertion clears the latch (CD reads inserted again) via sd_retry_tick. */
static bool sd_mount(void) {
  if (s_sd_mounted) {
    if (SDCard.healthy()) return true;        // still good
    SDCard.end(); s_sd_mounted = false;       // card pulled -> drop the mount
  }
  if (s_sd_absent) return false;
  if (!SDCardFS::cardInserted()) { s_sd_absent = true; return false; }  // empty slot
  if (SDCard.begin()) { s_sd_mounted = true; s_sd_tries = 0; return true; }
  if (++s_sd_tries >= 3) s_sd_absent = true;  // card present but won't mount -> stop trying
  return false;
}

static bool      sd_present(void)          { return s_sd_mounted && SDCard.healthy(); }
static fs::FS   &sd_fs(void)               { return (fs::FS &)SDCard; }
static int       sd_card_type(void)        { return s_sd_mounted ? CARD_SDHC : CARD_NONE; }
static uint64_t  sd_card_size_bytes(void)  { return SDCard.totalBytes(); }
static uint64_t  sd_total_bytes(void)      { return SDCard.totalBytes(); }
/* tkl_fs has no df, so "used" is ESTIMATED by summing every file's size (SDCard.usedBytes()
 * walks the tree). Approximate (ignores FAT slack) but good enough for the About screen. It's
 * O(files) + opens each file, so only call it where a one-shot read is fine (the About open). */
static uint64_t  sd_used_bytes(void)       { return s_sd_mounted ? SDCard.usedBytes() : 0; }
static inline void sd_set_bus_ready(void)  {}   // SDIO is independent of the display bus on the T5

/* Non-blocking background retry: re-arm the absence latch when a card is (re)inserted, so
 * inserting one mid-session starts using it without a reboot. Throttled by the caller's cadence. */
static void sd_retry_tick(uint32_t /*now_ms*/) {
  if (s_sd_mounted) return;
  bool in = SDCardFS::cardInserted();
  if (in && s_sd_absent) { s_sd_absent = false; s_sd_tries = 0; }  // card back -> allow mounts
  if (!in) { s_sd_absent = true; return; }                         // no card -> stay latched
  sd_mount();                                                      // card present -> try mounting
}

static bool sd_format(void) { return false; }   // SDK doesn't expose a format primitive here

/* RAII bus-hold + display-drain shim: the T5 SDIO is on its OWN controller (not the display
 * QSPI bus), so there's no cross-bus contention to arbitrate — these are no-ops. Provided so
 * the shared callers (sd_card.h consumers) compile unchanged. */
void display_bus_drain(void);   // defined in the .ino (no-op on Tuya)
struct SdBusHold { SdBusHold() {} ~SdBusHold() {} };

#elif !BOARD_HAS_SD_MMC && !BOARD_HAS_SD_SPI
/* No SD slot on this board. Same API, never mounts; every SD consumer already
 * degrades to its flash (FFat) fallback when sd_mount() is false. sd_fs() must
 * still return a reference (it's only dereferenced when sd_mount() is true, so
 * it's never actually used here) — point it at FFat to keep the type valid. */
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/FFat.h"
#else
#include <FFat.h>
#endif
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
 * old behavior) made a PRESENT card report "no card" all session. With ~8 tries spaced
 * SD_RETRY_MS apart that's a generous multi-second window for a slow card/rail to come
 * up — yet for a truly absent card the cost is just that many quick failed begin()s
 * (each capped by command_timeout_ms so it can't freeze the UI), then silence (retries
 * stop once latched). */
#define SD_MAX_TRIES  8

/* Spacing between background retries. A failed mount on the SPI backend still costs a
 * few hundred ms even with command_timeout_ms capped (it issues several init commands),
 * so leave a clear gap between attempts — the loop renders UI frames in between rather
 * than chaining the hitches back-to-back. ~3 s across all SD_MAX_TRIES is still a
 * generous window for a slow card/rail to come up. */
#define SD_RETRY_MS  1500

/* ========================================================================== *
 *  SDMMC backend (S3-2.06) — dedicated 1-bit bus via the Arduino SD_MMC lib.  *
 *  UNCHANGED from the original; only reorganized under this #if.              *
 * ========================================================================== */
#if BOARD_HAS_SD_MMC
#include <SD_MMC.h>
#define SD_DEV  SD_MMC

/* Init at a conservative clock. A lower probe-friendly clock makes the first
 * handshake far more dependable on early boot / a marginal rail. */
#ifndef SD_MMC_FREQ
#define SD_MMC_FREQ  SDMMC_FREQ_DEFAULT   // ~20 MHz; well within spec, robust at init
#endif

/* ---- Bus WIDTH (1-bit vs 4-bit) ------------------------------------------
 * 4-bit moves 4 data lines per clock instead of 1, so reads/writes are up to ~4x
 * faster — a real win for loading images / larger files off the card. It needs the
 * board to actually WIRE all four data lines (D1/D2/D3 in addition to D0): a board
 * that only breaks out D0 (e.g. the S3-2.06) MUST stay 1-bit, or the extra lines
 * float and the card mis-clocks. So 4-bit is opt-in PER BOARD: the header sets
 * BOARD_HAS_SD_MMC_4BIT=1 AND defines SDMMC_D1/D2/D3. Default 0 keeps every existing
 * board on the proven 1-bit path unchanged.
 *
 * SD_MMC.begin()'s 2nd arg is `mode1bit`: true = 1-bit, false = 4-bit. setPins has a
 * 3-arg (1-bit: clk,cmd,d0) and a 6-arg (4-bit: clk,cmd,d0,d1,d2,d3) form. */
#if BOARD_HAS_SD_MMC_4BIT   /* defaulted 0 centrally in board.h */
#define SD_MMC_MODE_1BIT  false   // 4-bit data bus
static inline bool sd_mmc_set_pins(void) {
  return SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA, SDMMC_D1, SDMMC_D2, SDMMC_D3);
}
#else
#define SD_MMC_MODE_1BIT  true    // 1-bit data bus (D0 only)
static inline bool sd_mmc_set_pins(void) {
  return SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
}
#endif

static bool sd_mount(void) {
  if (s_sd_mounted) return true;
  if (s_sd_absent)  return false;
  sd_mmc_set_pins();
  bool ok = SD_MMC.begin("/sdcard", SD_MMC_MODE_1BIT, false, SD_MMC_FREQ);  // no auto-format
  if (ok && SD_DEV.cardType() != 0) {
    s_sd_mounted = true;
    USBSerial.printf("[sd] card mounted (%s)\n", SD_MMC_MODE_1BIT ? "1-bit" : "4-bit");
    return true;
  }
  SD_DEV.end();  // dedicated bus -> safe to end and retry clean
  if (++s_sd_tries >= SD_MAX_TRIES) {
    s_sd_absent = true;
    USBSerial.printf("[sd] no card after %u tries -> SD features disabled "
                     "(flash fallbacks still work)\n", (unsigned)s_sd_tries);
  } else {
    USBSerial.printf("[sd] mount attempt %u failed; will retry\n", (unsigned)s_sd_tries);
  }
  return false;
}
static bool      sd_present(void)         { return s_sd_mounted; }
static fs::FS   &sd_fs(void)              { return (fs::FS &)SD_DEV; }
static bool sd_format(void) {
  s_sd_mounted = false; s_sd_absent = false; s_sd_tries = 0;
  sd_mmc_set_pins();   // self-contained: format may run before a successful mount
  bool ok = SD_MMC.begin("/sdcard", SD_MMC_MODE_1BIT, true, SD_MMC_FREQ);  // format-on-empty TRUE
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
#undef SD_DEV

/* ========================================================================== *
 *  SD-over-SPI backend (C6-1.47) — IDF sdspi as a SECOND device on the host   *
 *  the DISPLAY already owns (Arduino_ESP32SPIDMA called spi_bus_initialize).  *
 *                                                                             *
 *  WHY NOT Arduino SD.begin(cs, SPI, ...): that goes through Arduino SPIClass *
 *  which would spi_bus_initialize() the FSPI host a SECOND time -> the IDF    *
 *  aborts (ESP_ERR_INVALID_STATE) and the chip reboots. Instead we attach via *
 *  sdspi_host_init_device()/esp_vfs_fat_sdspi_mount() onto the SAME host id,  *
 *  which only does spi_bus_add_device() (LCD = device 0, SD = device 1). The  *
 *  IDF spi_master then arbitrates the two devices per-transaction.            *
 *                                                                             *
 *  The card is mounted into the VFS at /sd (POSIX fopen), then wrapped in an  *
 *  Arduino fs::FS via VFSImpl so sd_fs() and every existing consumer          *
 *  (sd_fs().open(), the Files app, wifi/notif/health stores) are UNCHANGED.   *
 * ========================================================================== */
#elif BOARD_HAS_SD_SPI
#include <FFat.h>                       // pulls in the fs::FS / VFSImpl machinery
#include "vfs_api.h"                    // VFSImpl: maps an fs::FS onto a VFS mount path
#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

/* CARD_* type codes for the About screen's card-type switch. The Arduino <SD.h>
 * (which defined these) is no longer pulled in on this IDF path, so define them
 * here. Values mirror sdmmc_types.h / the Arduino SD lib. */
#ifndef CARD_NONE
enum { CARD_NONE = 0, CARD_MMC = 1, CARD_SD = 2, CARD_SDHC = 3, CARD_UNKNOWN = 4 };
#endif

/* Defined in the .ino TU: drains any in-flight async display-flush DMA so the SPI
 * bus is idle before the SD (2nd device on the shared host) issues a transaction.
 * Forward-declared here because sd_card.h is #included before that definition. */
void display_bus_drain(void);
/* SD<->display HYBRID bus arbitration (defined in the .ino). lock() drains the in-flight
 * display tile then puts the display in SYNC-ONLY mode (no async DMA) for the duration of the
 * SD op; unlock() restores async when the (nested) count returns to 0. Every SD op is bracketed
 * via SdBusHold below — so while the SD card uses the shared host, the display never has a
 * queued DMA in flight, which is what corrupted the panel. */
void sd_bus_lock(void);
void sd_bus_unlock(void);

#ifndef SD_SPI_FREQ_KHZ
#define SD_SPI_FREQ_KHZ  20000          // 20 MHz SD clock once mounted (init is slower internally)
#endif
#define SD_MOUNT_POINT  "/sd"

static sdmmc_card_t   *s_sd_card   = nullptr;
static sdspi_dev_handle_t s_sd_dev = 0;

/* ---- Display-bus FENCE around every SD transaction (C6 shared-host fix) ------------
 * The SD card is a 2nd device on the SAME SPI host the display owns. The display's async
 * DMA flush queues a tile and returns with the panel CS still held LOW (driven by RAW GPIO
 * registers in the GFX lib, OUTSIDE the IDF's per-device CS management) until the transfer-
 * done ISR raises it. If an SD transaction hits the bus in that window, BOTH devices' CS are
 * low at once -> the panel's address-window/RAM state is corrupted = persistent garbled lines
 * that survive leaving the screen. Per-call drains in app code were whack-a-mole; instead we
 * fence at the ONE chokepoint every SD access funnels through: the VFS file/fs impl. Each
 * read/write/seek/open/exists first calls display_bus_drain() (waitAsync), which both finishes
 * the in-flight DMA AND lets its post_cb raise the panel CS, so the bus is truly idle. These
 * all run on the loop task (same task as the flush), so once drained no new flush can queue
 * mid-call. No-op cost when no DMA is pending. */
/* RAII: hold the SD<->display bus lock for the duration of one SD op. lock() drains any
 * in-flight display DMA + marks the bus SD-owned; ~unlock() releases. */
struct SdBusHold { SdBusHold() { sd_bus_lock(); } ~SdBusHold() { sd_bus_unlock(); } };

/* A FileImpl DECORATOR: holds the real VFS file and brackets every operation that issues an SD
 * SPI transaction with the bus lock (drain display DMA + block display flushes for the op),
 * then delegates. Wrapping (not subclassing) VFSFileImpl keeps the real VFS/FATFS logic intact. */
class FencedFileImpl : public fs::FileImpl {
  fs::FileImplPtr _inner;
public:
  explicit FencedFileImpl(fs::FileImplPtr inner) : _inner(inner) {}
  size_t write(const uint8_t *b, size_t n) override { SdBusHold h; return _inner->write(b, n); }
  size_t read(uint8_t *b, size_t n) override        { SdBusHold h; return _inner->read(b, n); }
  void   flush() override                           { SdBusHold h; _inner->flush(); }
  bool   seek(uint32_t p, SeekMode m) override      { SdBusHold h; return _inner->seek(p, m); }
  size_t position() const override                  { return _inner->position(); }
  size_t size() const override                      { return _inner->size(); }
  bool   setBufferSize(size_t n) override           { return _inner->setBufferSize(n); }
  void   close() override                           { SdBusHold h; _inner->close(); }
  time_t getLastWrite() override                    { return _inner->getLastWrite(); }
  const char *path() const override                 { return _inner->path(); }
  const char *name() const override                 { return _inner->name(); }
  boolean isDirectory(void) override                { return _inner->isDirectory(); }
  fs::FileImplPtr openNextFile(const char *mode) override {
    SdBusHold h;
    fs::FileImplPtr nf = _inner->openNextFile(mode);
    return nf ? std::make_shared<FencedFileImpl>(nf) : nf;
  }
  boolean seekDir(long position) override           { SdBusHold h; return _inner->seekDir(position); }
  String getNextFileName(void) override             { SdBusHold h; return _inner->getNextFileName(); }
  String getNextFileName(bool *isDir) override      { SdBusHold h; return _inner->getNextFileName(isDir); }
  void   rewindDirectory(void) override             { SdBusHold h; _inner->rewindDirectory(); }
  operator bool() override                          { return (bool)*_inner; }
};

/* VFSImpl subclass: brackets the directory-level ops with the bus lock, and wraps every opened
 * file in the fencing decorator. Delegates to the real VFSImpl::open (keeps path/dir-create/lock
 * logic) and just decorates the result. */
class FencedVFSImpl : public VFSImpl {
public:
  FileImplPtr open(const char *path, const char *mode, const bool create) override {
    SdBusHold h;
    FileImplPtr f = VFSImpl::open(path, mode, create);
    return f ? std::make_shared<FencedFileImpl>(f) : f;
  }
  bool exists(const char *path) override { SdBusHold h; return VFSImpl::exists(path); }
  bool rename(const char *a, const char *b) override { SdBusHold h; return VFSImpl::rename(a, b); }
  bool remove(const char *path) override { SdBusHold h; return VFSImpl::remove(path); }
  bool mkdir(const char *path)  override { SdBusHold h; return VFSImpl::mkdir(path); }
  bool rmdir(const char *path)  override { SdBusHold h; return VFSImpl::rmdir(path); }
};

/* Arduino fs::FS view of the /sd VFS mount. Built once; its impl's mountpoint is set
 * to /sd so a consumer's relative path ("/wifi.csv") resolves under the card. Uses the
 * FENCED impl so every SD transaction drains the display DMA first (see above). */
class SdSpiFS : public fs::FS {
public:
  SdSpiFS() : fs::FS(fs::FSImplPtr(new FencedVFSImpl())) { _impl->mountpoint(SD_MOUNT_POINT); }
};
static SdSpiFS s_sd_fs;

/* The host the display bus initialized. On the C6 Arduino FSPI == SPI2_HOST. */
#define SD_SPI_HOST_ID  ((spi_host_device_t)SPI2_HOST)

/* Core of the mount. format_if_failed mirrors the old format_if_empty arg: FALSE for
 * a normal mount (never silently wipe a Windows-FAT32 card the IDF can't read), TRUE
 * only from sd_format() (a deliberate, confirmed user action). */
static bool sd_spi_try_mount(bool format_if_failed) {
  if (!s_gfx_ready) return false;       // display owns spi_bus_initialize; wait for it

  // Hold the bus for the WHOLE mount (drain alone leaves a window where a flush from
  // another context could start mid-mount): drains the LCD DMA, blocks concurrent
  // flushes, and forces sync-only until this function returns.
  SdBusHold bus;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SD_SPI_HOST_ID;           // attach to the ALREADY-initialized host (no re-init)
  host.max_freq_khz = SD_SPI_FREQ_KHZ;
  // Cap how long a SINGLE init command may block. With the default (0 -> the SD
  // layer's ~1s+), a SILENT/ABSENT card makes esp_vfs_fat_sdspi_mount() spin in
  // sdmmc_init_ocr()'s ACMD41 loop for ~3.3s PER ATTEMPT — all on the loop task, so
  // the whole UI freezes until every retry is exhausted. A short timeout makes a
  // failed attempt return fast (a real card answers ACMD41 almost immediately, well
  // within this), so the loop keeps rendering between the spaced-out retries below.
  host.command_timeout_ms = 250;

  // NOTE: no on-chip-LDO (LDO_VO4) power-up call here. The IDF sd_pwr_ctrl /
  // esp_ldo_regulator helpers are NOT compiled into this Arduino core's prebuilt C6
  // libs (link error: undefined reference to sd_pwr_ctrl_new_on_chip_ldo), and the
  // previous working Arduino SD.begin() path had no LDO call either — so the rail is
  // in its power-on default and the card is reachable without it. If a card ever comes
  // up SILENT (cardType=0) specifically because of VDD, this is the first place to
  // revisit (would need the LDO component enabled in the core build).
  host.pwr_ctrl_handle = nullptr;

  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id = SD_SPI_HOST_ID;
  slot.gpio_cs = (gpio_num_t)SD_SPI_CS;

  esp_vfs_fat_mount_config_t mcfg = {};
  mcfg.format_if_mount_failed = format_if_failed;
  mcfg.max_files = 5;
  mcfg.allocation_unit_size = 16 * 1024;

  esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_sd_card);
  if (err != ESP_OK) {
    // ESP_FAIL = card responded but FATFS couldn't mount (format/filesystem problem);
    // other = card never responded (bus/signal/CS) — mirror the old diagnostic split.
    USBSerial.printf("[sd] mount failed: %s -> %s\n", esp_err_to_name(err),
                     (err == ESP_FAIL) ? "card RESPONDS (filesystem/format problem)"
                                       : "card SILENT (bus/signal/CS problem, not format)");
    s_sd_card = nullptr;
    return false;
  }
  return true;
}

static bool sd_mount(void) {
  if (s_sd_mounted) return true;
  if (s_sd_absent)  return false;
  if (!s_gfx_ready) return false;       // not a failed attempt — bus just isn't up yet

  if (sd_spi_try_mount(false)) {
    s_sd_mounted = true;
    USBSerial.printf("[sd] card mounted (%lluMB)\n",
                     ((uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size) / (1024ULL * 1024ULL));
    return true;
  }
  if (++s_sd_tries >= SD_MAX_TRIES) {
    s_sd_absent = true;
    USBSerial.printf("[sd] no card after %u tries -> SD features disabled "
                     "(flash fallbacks still work)\n", (unsigned)s_sd_tries);
  } else {
    USBSerial.printf("[sd] mount attempt %u failed; will retry\n", (unsigned)s_sd_tries);
  }
  return false;
}

static bool      sd_present(void)         { return s_sd_mounted; }
static fs::FS   &sd_fs(void)              { return s_sd_fs; }

static bool sd_format(void) {
  s_sd_tries = 0; s_sd_absent = false;
  // Unmount first if we're mounted (format_if_mount_failed only kicks in on a FAILED
  // mount, so to reformat a mountable card we must drop it then remount with format).
  if (s_sd_mounted) {
    SdBusHold bus;   // lock + drain + sync-only across the unmount's SD transactions
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_sd_card);
    s_sd_card = nullptr;
    s_sd_mounted = false;
  }
  if (sd_spi_try_mount(true)) {
    s_sd_mounted = true;
    USBSerial.println("[sd] card formatted + mounted");
    return true;
  }
  USBSerial.println("[sd] format failed (no card responding, or f_mkfs error)");
  return false;
}

static int sd_card_type(void) {
  if (!s_sd_card) return CARD_NONE;
  // ocr's HCS/CCS bit (bit 30) set => high-capacity (SDHC/SDXC); else standard SDSC.
  return (s_sd_card->ocr & (1u << 30)) ? CARD_SDHC : CARD_SD;
}
static uint64_t sd_card_size_bytes(void)  {
  return s_sd_card ? (uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size : 0;
}
static uint64_t sd_total_bytes(void) {
  if (!s_sd_mounted) return 0;
  uint64_t tot = 0, fre = 0;
  SdBusHold bus;   // fat_info walks the FAT = many SD transactions; hold the bus for all
  if (esp_vfs_fat_info(SD_MOUNT_POINT, &tot, &fre) != ESP_OK) return 0;
  return tot;
}
static uint64_t sd_used_bytes(void) {
  if (!s_sd_mounted) return 0;
  uint64_t tot = 0, fre = 0;
  SdBusHold bus;
  if (esp_vfs_fat_info(SD_MOUNT_POINT, &tot, &fre) != ESP_OK) return 0;
  return tot - fre;
}

#endif  /* backend select */

/* Non-blocking background retry, called from the main loop with millis(). The first
 * mount attempts run during early boot before the SD rail is powered, so they fail;
 * rather than blocking boot to wait for the rail, we retry here every SD_RETRY_MS as
 * the watch runs normally. Stops as soon as the card mounts (or sd_mount latches
 * "absent" after SD_MAX_TRIES). */
static void sd_retry_tick(uint32_t now_ms) {
  if (s_sd_mounted || s_sd_absent) return;
  static uint32_t last = 0;
  if (now_ms - last < SD_RETRY_MS) return;
  last = now_ms;
  sd_mount();
}

#endif  /* SD slot present */
