/* ============================================================================
 *  sd_card.h — shared lazy microSD mount (one SD_MMC.begin for the whole sketch).
 *
 *  The board wires the card for 1-bit SDMMC (SDMMC_CLK/CMD/DATA in pin_config.h).
 *  Both the WiFi-credential CSV (wifi_store.h) and the battery-health trend log
 *  (batt_health_sd.h) share this single mount, so the card is begun at most once
 *  per boot. Absence is latched so we never retry-spam the bus when no card is in.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE BEFORE wifi_store.h and
 *  batt_health_sd.h. Needs the SDMMC_* pins from pin_config.h (included earlier).
 * ========================================================================== */
#pragma once
#include <SD_MMC.h>

static bool    s_sd_mounted   = false;
static bool    s_sd_absent    = false;  // latched ONLY after several failed attempts
static uint8_t s_sd_tries     = 0;      // failed mount attempts so far this boot

/* How many mount attempts may fail before we give up and latch "absent". The first
 * couple happen very early in setup() (wifi_nets_load / na_seed_total), BEFORE the
 * PMU rails are restored, so they fail on an unpowered card; the rest are the loop's
 * background retries (every SD_RETRY_MS). Latching "absent" on the first failure (the
 * old behavior) made a PRESENT card report "no card" all session. With ~12 tries and
 * a 400 ms retry spacing that's a generous ~4-5 s window for a slow card/rail to come
 * up — yet for a truly absent card the cost is just that many quick failed begin()s,
 * then silence (retries stop once latched). */
#define SD_MAX_TRIES  8

/* Init at a conservative clock. The default BOARD_MAX_SDMMC_FREQ can be too fast for
 * a reliable first handshake on early boot / marginal rail; a lower probe-friendly
 * clock makes mounting far more dependable. (Throughput here is tiny — CSV logs.) */
#ifndef SD_MMC_FREQ
#define SD_MMC_FREQ  SDMMC_FREQ_DEFAULT   // ~20 MHz; well within spec, robust at init
#endif

/* Mount the card if needed. Returns true if mounted (or already mounted). Safe to
 * call repeatedly: it retries a present-but-not-yet-ready card and only latches
 * "absent" after SD_MAX_TRIES genuine failures. */
static bool sd_mount(void) {
  if (s_sd_mounted) return true;
  if (s_sd_absent)  return false;

  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  // 1-bit mode (this board), don't format on failure, conservative clock.
  bool ok = SD_MMC.begin("/sdcard", true, false, SD_MMC_FREQ);
  if (ok && SD_MMC.cardType() != CARD_NONE) {
    s_sd_mounted = true;
    USBSerial.println("[sd] card mounted");
    return true;
  }

  // Failed this attempt. end() so the next try starts the peripheral clean.
  SD_MMC.end();
  if (++s_sd_tries >= SD_MAX_TRIES) {
    s_sd_absent = true;                  // give up only after repeated failures
    USBSerial.printf("[sd] no card after %u tries -> SD features disabled "
                     "(flash fallbacks still work)\n", (unsigned)s_sd_tries);
  } else {
    USBSerial.printf("[sd] mount attempt %u failed; will retry\n", (unsigned)s_sd_tries);
  }
  return false;
}

static bool sd_present(void) { return s_sd_mounted; }

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
