/* ============================================================================
 *  batt_health_sd.h — OPTIONAL per-cycle battery-health trend log on SD.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER power_model.h (it
 *  provides the definition of batt_health_sd_log(), which power_model.h forward-
 *  declares and calls once per learned discharge cycle).
 *
 *  PURPOSE: the authoritative health number is the running average in NVS
 *  (power_model.h) and works with NO SD card. THIS file only adds an append-only
 *  CSV trend log so you can pull lifetime per-cycle points onto a PC and graph
 *  capacity fade. If no usable card is present, every call is a graceful no-op —
 *  the NVS average is never affected.
 *
 *  The SD card is mounted LAZILY (only on the first log of a session) and left
 *  mounted; cycles are rare (hours apart) so there's no spin-up churn. 1-bit
 *  SDMMC on the board's wiring (SDMMC_CLK/CMD/DATA from pin_config.h), matching
 *  how the old notification archive mounted it.
 *
 *  FORMAT: /batt_health.csv, header written once, then one row per cycle:
 *    epoch,cycle,delta_pct,learned_mah,health_pct
 * ========================================================================== */
#pragma once
#include "sd_card.h"           // shared SD mount (sd_mount)
#include "storage_fs.h"        // SD-if-present-else-FFat backend (store_fs/store_available)

#define BATT_CSV_PATH  "/batt_health.csv"

/* Append one cycle's trend point to the CSV. Writes to SD if present, else the flash
 * FAT partition; no-op only if NEITHER mounts. Writes the header row once, when the
 * file is first created. */
static void batt_health_sd_log(uint32_t epoch, uint16_t cycle, int delta_pct,
                               float learned_mah, int health_pct) {
  if (!store_available()) return;              // no backend at all -> silently skip

  bool need_header = !store_fs().exists(BATT_CSV_PATH);
  File f = store_fs().open(BATT_CSV_PATH, FILE_APPEND);
  if (!f) return;
  if (need_header) f.println("epoch,cycle,delta_pct,learned_mah,health_pct");
  f.printf("%lu,%u,%d,%.0f,%d\n", (unsigned long)epoch, cycle, delta_pct,
           learned_mah, health_pct);
  f.close();
  USBSerial.printf("[batt] logged cycle %u: %.0f mAh (%d%%) to %s\n",
                   cycle, learned_mah, health_pct, store_on_sd() ? "SD" : "flash");
}
