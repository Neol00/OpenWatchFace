/* ============================================================================
 *  sleep_power.h — deep sleep + the AXP2101 peripheral-rail safety probe.
 *
 *  Everything around powering the watch down and back up:
 *    - the runtime per-rail safety probe (which AXP2101 LDOs can be cut in sleep
 *      without deadlocking the I2C bus / losing the RTC), its NVS journal, and the
 *      Power-screen accessors (rail_* — forward-declared in the .ino for app_power.h);
 *    - rails_restore() (boot) / rails_cut_for_sleep() (pre-sleep);
 *    - the one-shot SD-rail diagnostic (guarded by SD_RAIL_DIAG, default 0);
 *    - the deep-sleep entry paths (enter_deep_sleep / arm_wakes_and_sleep /
 *      rearm_and_deep_sleep) and the light background-check path
 *      (background_check_has_new) that a timer wake runs without the display.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER:
 *    - the hardware objects (board_power.h, gfx, rtc, Wire, USBSerial) and the
 *      config macros (BOOT_BTN_GPIO, SLEEP_INTENT_MAGIC, rtc_sleep_intent,
 *      IIC_SDA/SCL) defined at the top of the .ino;
 *    - settings_store.h (s_checks_enabled, s_check_interval_min via watch_base.h),
 *      timer_store.h, haptics.h, power_model.h (calib_note_*), ble_provision.h
 *      (ble_end), sd_card.h (SDMMC pins) and notif_net.h (notif_fetch_raw,
 *      rtc_last_notif_id) — all the things the sleep paths call.
 *  The .ino forward-declares the rail_* accessors (for app_power.h, included
 *  earlier); this header defines them.
 *
 *  NOTE: rounder_event_cb (the CO5300 even-alignment LVGL callback) deliberately
 *  does NOT live here — it's a display callback and stays with my_disp_flush /
 *  my_touchpad_read in the .ino.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/esp_sleep.h"
#include "tuya/compat/driver/rtc_io.h"
#include "tuya/compat/esp_system.h"
#else
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp_system.h>
#endif

/* ===================== runtime peripheral-rail safety probe ==================
 * We can't trust the schematic for which AXP2101 rail powers what, and the ONE
 * rail we must never cut is whichever feeds the shared I2C pull-ups (cutting it
 * deadlocks the wake — you can't re-enable a rail over the bus it powers). So the
 * watch probes each candidate rail at runtime: disable it, check the PMU + RTC
 * still ACK on I2C, then re-enable. Rails that pass are marked SAFE — meaning
 * "won't deadlock the wake" — but that is NOT enough on its own: a rail can pass
 * the I2C test yet still power the DISPLAY, so cutting it leaves a blank screen on
 * wake. So actually cutting a rail is OPT-IN PER RAIL (s_rail_cut, default OFF):
 * you enable SAFE rails one at a time from Settings>Power and confirm the screen
 * comes back after a real sleep/wake. Nothing is cut until you say so.
 *
 * Crash-safe probe: the rail being probed is journaled to NVS *before* it's
 * disabled, so if a probe freezes the bus, the next boot (after a cold power-cycle
 * restores defaults) sees the journal and marks that rail UNSAFE. Only peripheral
 * LDOs are candidates — never DCDC1 (ESP32) or the RTC supply.
 *
 * Recovery if you enable a bad rail (blank on wake): cold power-cycle (PWR off,
 * then on) -> the screen works again -> turn on Caffeine (so it won't sleep) ->
 * Settings>Power -> toggle that rail back off. */
/* The rail list (RAIL_* / RAIL_COUNT), rail_name/rail_set/rail_is_on and the
 * board's RAIL_DEFAULT_CUT seed all live in board_power.h now — the probe/cut
 * POLICY below is board-neutral, the rail PRIMITIVES are the board's. */
enum { RAIL_UNKNOWN = 0, RAIL_SAFE = 1, RAIL_UNSAFE = 2 };
static uint8_t s_rail_state[RAIL_COUNT];   // probe verdict (UNKNOWN/SAFE/UNSAFE)
static uint8_t s_rail_cut[RAIL_COUNT];     // user opt-in: actually cut this rail in sleep?

/* Bumping RAILCUT_VER re-seeds RAIL_DEFAULT_CUT onto an existing device once;
 * after that your Settings>Power toggles win. (v2: the S3-2.06 rail list grew
 * the experimental AUX rails — DC2..CPUSLDO — so re-seed to the wider array;
 * the old 6-byte NVS blobs wouldn't load into the new RAIL_COUNT anyway.) */
#define RAILCUT_VER 2

static bool i2c_acks(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}
/* Bus is "alive" only if BOTH the PMU and the RTC still answer — so a rail that
 * powers either the I2C pull-ups OR the RTC counts as unsafe (cutting it would
 * deadlock the wake or lose the clock). */
static bool i2c_bus_alive(void) {
  return board_power_acks() && i2c_acks(BOARD_RTC_I2C_ADDR);
}

static void rails_load_state(void) {
  if (prefs.getBytes("railstate", s_rail_state, RAIL_COUNT) != RAIL_COUNT)
    memset(s_rail_state, RAIL_UNKNOWN, RAIL_COUNT);
  // Cut opt-ins: seed the validated default set on a fresh device OR once after a
  // RAILCUT_VER bump; otherwise honor the saved per-rail toggles.
  if (prefs.getUChar("railcutver", 0) != RAILCUT_VER) {
    memcpy(s_rail_cut, RAIL_DEFAULT_CUT, RAIL_COUNT);
    prefs.putBytes("railcut", s_rail_cut, RAIL_COUNT);
    prefs.putUChar("railcutver", RAILCUT_VER);
  } else if (prefs.getBytes("railcut", s_rail_cut, RAIL_COUNT) != RAIL_COUNT) {
    memcpy(s_rail_cut, RAIL_DEFAULT_CUT, RAIL_COUNT);
  }
}
static void rails_save_state(void) {
  prefs.putBytes("railstate", s_rail_state, RAIL_COUNT);
  prefs.putBytes("railcut", s_rail_cut, RAIL_COUNT);
}

/* ---- accessors for the Power screen ---- */
static uint8_t rail_state_count(void) { return RAIL_COUNT; }
static uint8_t rail_state_raw(uint8_t i) { return i < RAIL_COUNT ? s_rail_state[i] : RAIL_UNKNOWN; }
static bool    rail_cut_get(uint8_t i) { return i < RAIL_COUNT && s_rail_cut[i]; }
static void    rail_cut_set(uint8_t i, bool on) {
  if (i >= RAIL_COUNT) return;
  s_rail_cut[i] = on ? 1 : 0;
  rails_save_state();
}
static const char *rail_verdict_str(uint8_t i) {
  switch (i < RAIL_COUNT ? s_rail_state[i] : RAIL_UNKNOWN) {
    case RAIL_SAFE:   return "bus-safe";       // passed the I2C test (can be opted-in)
    case RAIL_UNSAFE: return "kept on";        // powers I2C/RTC -> never cut
    default:          return "untested";
  }
}
static uint8_t rails_cut_count(void) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < RAIL_COUNT; i++) if (s_rail_cut[i]) n++;
  return n;
}

/* Bring EVERY peripheral rail back up at boot — BEFORE the RTC read + display init.
 * Unconditional (not just opted-in rails) so the watch always recovers to full
 * power no matter what a prior sleep — or prior firmware — left disabled: a blank
 * screen after a reflash is just the previous cut still latched in the AXP2101.
 * Generous settle so the AMOLED rail is stable before gfx->begin(). */
static void rails_restore(void) {
  if (!board_power_ok()) return;
  // Bring rails up ONE AT A TIME with a GENEROUS gap between them. The failure with
  // ALL rails cut (PMU then reads begin()=0, RTC + touch dead) is NOT a topology
  // deadlock — every rail recovers fine when cut individually. It's COMBINED INRUSH:
  // ALDO1/2/4 are slow to ramp, and bringing several up close together (or slamming
  // them all on at once) sags VSYS into a brownout that knocks the AXP2101's own I2C
  // offline, cascading to everything on the shared bus. So restore is SEQUENCED with
  // real settle per rail, and we only enable rails that are actually off (no
  // gratuitous off->on cycling that would stack extra inrush). Each enable is
  // retried since the AXP2101 can NAK a write right at boot.
  // The AXP2101 reports a rail's enable bit as 1 even when its LDO output stayed
  // collapsed after a deep-sleep cut (proven: PMU answers, rails all read "on", yet
  // the RTC + touch on them are dead). So we can't trust the readback and skip them.
  // FORCE each rail OFF->ON to actually restart the regulator — but do it ONE RAIL
  // AT A TIME with a short settle, so the slow ALDO1/2/4 don't stack inrush and sag
  // VSYS (which earlier knocked the PMU's I2C offline when all were cycled together).
  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
#ifdef RAIL_NEVER_CUT_MASK
    // Never-cut rails (ALDO1) were never disabled in sleep, so they're already up —
    // don't cycle them (wastes time + adds needless inrush). Skip straight past.
    if (RAIL_NEVER_CUT_MASK & (1u << i)) continue;
#endif
#ifdef RAIL_AUX_MASK
    // EXPERIMENTAL aux rails (DC2..CPUSLDO, no load on the schematic): only touch one
    // if the user opted IN to cutting it (then it's "managed": on awake, off asleep).
    // Un-opted aux rails keep whatever state the PMU's power-on defaults gave them —
    // force-enabling an inductor-less buck here could sag VSYS for nothing.
    if ((RAIL_AUX_MASK & (1u << i)) && !s_rail_cut[i]) continue;
#endif
    rail_set(i, false);                          // force regulator off (readback may lie)
    delay(2);
    for (int t = 0; t < 5 && !rail_is_on(i); t++) { rail_set(i, true); delay(4); }  // back on (retry NAKs)
    delay(18);                   // per-rail settle: let this LDO ramp + VSYS recover
                                 // before the next rail's inrush — NEVER all at once.
  }
  delay(60);                     // final settle so the AMOLED/RTC/touch rails are
                                 // stable before board_clock_begin() + gfx->begin().
}

/* ===================== SD-rail diagnostic (one-shot) ========================
 * Find WHICH AXP2101 peripheral rail powers the microSD card, so we can keep
 * cutting every OTHER rail in sleep but never the SD one. Method: bring all rails
 * up and confirm the card mounts; then disable rails ONE AT A TIME and re-mount —
 * the rail whose removal breaks the mount is the SD's supply. Prints a verdict per
 * rail + a summary line. Set SD_RAIL_DIAG to 1 (top of the .ino), flash, read the
 * serial log, then set it back to 0. (Leaves all rails ON so the watch runs.) */
#ifndef SD_RAIL_DIAG
#define SD_RAIL_DIAG 0
#endif
/* SDMMC + AXP2101-rail specific: only meaningful on a board with both. Uses raw
 * SD_MMC (bypassing sd_card.h's latch) for a fresh mount per rail-test. */
#if SD_RAIL_DIAG && BOARD_HAS_SD_MMC
/* Fresh mount attempt for the diagnostic (bypasses sd_card.h's latch/retry state). */
static bool sd_diag_try_mount(void) {
  SD_MMC.end();
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  bool ok = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);
  ok = ok && (SD_MMC.cardType() != CARD_NONE);
  SD_MMC.end();                 // leave the peripheral clean for the next test
  return ok;
}
static void sd_rail_diag(void) {
  if (!board_power_ok()) { USBSerial.println("[sddiag] no PMU -> skip"); return; }
  USBSerial.println("[sddiag] ==== SD power-rail diagnostic ====");

  // 1) All rails ON -> baseline. If this fails, it's NOT a rail (pins/socket/card).
  // (Aux rails are excluded throughout: no load on the schematic, so they can't be
  // the SD supply, and force-enabling an unloaded buck is needless risk.)
#ifndef RAIL_AUX_MASK
#define RAIL_AUX_MASK 0u
#endif
  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
    if (RAIL_AUX_MASK & (1u << i)) continue;
    rail_set(i, true); delay(8);
  }
  delay(120);                   // let everything settle + the card power up
  bool base = sd_diag_try_mount();
  USBSerial.printf("[sddiag] all rails ON -> mount %s\n", base ? "OK" : "FAILED");
  if (!base) {
    USBSerial.println("[sddiag] card does NOT mount even with all rails on ->");
    USBSerial.println("[sddiag]   the cause is NOT a cut rail (check socket/pins/card).");
    return;
  }

  // 2) Drop each rail in turn; if the mount breaks, that rail feeds the SD card.
  //    SKIP rails the probe already marked UNSAFE — those power the I2C pull-ups or
  //    RTC, so cutting one here could prevent us re-enabling it (bus deadlock). They
  //    can't be the SD rail in any case (we never cut them in sleep).
  int sd_rail = -1;
  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
    if (RAIL_AUX_MASK & (1u << i)) continue;    // no load -> can't be the SD rail
    if (s_rail_state[i] == RAIL_UNSAFE) {
      USBSerial.printf("[sddiag]   %s skipped (UNSAFE: powers I2C/RTC)\n", rail_name(i));
      continue;
    }
    rail_set(i, false); delay(120);             // cut just this rail, settle
    bool ok = sd_diag_try_mount();
    rail_set(i, true);  delay(60);              // restore it before the next test
    USBSerial.printf("[sddiag]   %s OFF -> mount %s%s\n",
                     rail_name(i), ok ? "OK" : "FAILED",
                     ok ? "" : "   <== SD card is on this rail");
    if (!ok && sd_rail < 0) sd_rail = i;
  }

  // Make sure everything is back ON before normal boot continues.
  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
    if (RAIL_AUX_MASK & (1u << i)) continue;
    rail_set(i, true); delay(8);
  }
  delay(80);

  if (sd_rail >= 0)
    USBSerial.printf("[sddiag] RESULT: SD card is powered by %s — exclude it from the cut set.\n",
                     rail_name(sd_rail));
  else
    USBSerial.println("[sddiag] RESULT: no single rail broke the mount (SD may be on an "
                      "always-on rail; cutting any one is safe).");
  USBSerial.println("[sddiag] ==== end (all rails left ON) ====");
}
#endif  // SD_RAIL_DIAG && BOARD_HAS_SD_MMC

/* Cut the opted-in rails right before deep sleep — but only ones the probe also
 * verified bus-safe, so a seeded default can never cut a rail this unit hasn't
 * cleared (e.g. before the probe has run, or on different silicon). */
static void rails_cut_for_sleep(void) {
  if (!board_power_ok()) { USBSerial.println("[sleep] PMU not ok -> NO rails cut!"); return; }
  uint8_t ncut = 0;
  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
#ifdef RAIL_NEVER_CUT_MASK
    // Hard guard: some rails MUST stay powered through deep sleep no matter what the
    // cut set says. On the S3-2.06 ALDO1 feeds the RTC VDD / shared-I2C bus — cutting
    // it leaves the RTC + touch unrecoverable on wake (RTC not found / FT3168 init
    // fail), needing a cold reboot. So it can never be cut, via default set OR the
    // Power app's per-rail toggles.
    if (RAIL_NEVER_CUT_MASK & (1u << i)) continue;
#endif
    if (s_rail_cut[i] && s_rail_state[i] == RAIL_SAFE) { rail_set(i, false); ncut++; }
    else if (s_rail_cut[i])
      // Opted-in but not probe-verified SAFE -> stays ON all sleep. This is the silent
      // drain case (e.g. probe verdicts lost to an NVS wipe/resize): make it loud.
      USBSerial.printf("[sleep] %s opted-in but NOT cut (verdict=%s) -> stays ON\n",
                       rail_name(i), rail_verdict_str(i));
  }
  // Dump the PMU enable bits as they will be for the whole sleep — the ground truth
  // for chasing sleep drain (anything '1' here is powered all night).
  USBSerial.printf("[sleep] rails cut: %u; enable bits at sleep entry:", ncut);
  for (uint8_t i = 0; i < RAIL_COUNT; i++)
    USBSerial.printf(" %s=%d", rail_name(i), (int)rail_is_on(i));
  USBSerial.println();
  USBSerial.flush();
}

/* Probe any not-yet-classified rails. Runs once the full UI is up (interactive
 * boots only). Stops early if a probe leaves the bus stuck — a power-cycle then
 * resumes via the NVS journal. */
static void rails_probe(void) {
  if (!board_power_ok()) return;
  rails_load_state();
  bool changed = false, stuck = false;

  // A journaled probe that didn't clear = it froze the bus last boot -> UNSAFE.
  uint8_t pj = prefs.getUChar("railprobe", 255);
  if (pj < RAIL_COUNT) {
    s_rail_state[pj] = RAIL_UNSAFE;
    rails_save_state();
    prefs.putUChar("railprobe", 255);
    changed = true;
    USBSerial.printf("[rails] %s stuck the bus last boot -> UNSAFE\n", rail_name(pj));
  }

  for (uint8_t i = 0; i < RAIL_COUNT; i++) {
    if (s_rail_state[i] != RAIL_UNKNOWN) continue;
#ifdef RAIL_NEVER_CUT_MASK
    // Don't even probe a never-cut rail (ALDO1 = RTC/I2C supply): cutting it to test
    // would wedge the bus and may leave it stuck until a cold power-cycle. Mark it
    // UNSAFE so it's permanently excluded from the cut set.
    if (RAIL_NEVER_CUT_MASK & (1u << i)) {
      s_rail_state[i] = RAIL_UNSAFE;
      rails_save_state();
      changed = true;
      USBSerial.printf("[rails] %s -> UNSAFE (never-cut: RTC/I2C supply)\n", rail_name(i));
      continue;
    }
#endif
    // Aux rails (no schematic load) are restored to their PRE-PROBE state — the PMU's
    // defaults may legitimately have one OFF, and the probe must not turn it on.
    // Classic rails were brought up by rails_restore(), so their prior state is ON.
    bool was_on = rail_is_on(i);
#ifdef RAIL_AUX_MASK
    if (!(RAIL_AUX_MASK & (1u << i))) was_on = true;
#else
    was_on = true;
#endif
    prefs.putUChar("railprobe", i);          // journal BEFORE disabling
    rail_set(i, false);
    delay(30);                               // settle
    bool alive = i2c_bus_alive();            // PMU + RTC still reachable?
    rail_set(i, was_on);                     // restore (may fail if the bus is dead)
    delay(15);
    bool recovered = i2c_bus_alive();
    s_rail_state[i] = alive ? RAIL_SAFE : RAIL_UNSAFE;
    rails_save_state();
    prefs.putUChar("railprobe", 255);
    changed = true;
    USBSerial.printf("[rails] %s -> %s\n", rail_name(i), alive ? "SAFE (will cut in sleep)" : "UNSAFE");
    if (!recovered) { stuck = true; break; } // bus dead -> needs a cold power-cycle
  }

  // If we classified anything (and the bus is still healthy), reboot into a clean
  // state: probing a rail can momentarily glitch a peripheral (e.g. the display),
  // and a fresh boot re-runs rails_restore() + a clean display init. If the bus got
  // stuck, DON'T restart (it would just reboot into the dead bus) — the watch will
  // sit until a cold power-cycle (hold PWR off, then on) restores the rail, after
  // which the next boot marks it UNSAFE and continues.
  if (changed && !stuck) {
    USBSerial.println("[rails] classification updated -> restarting clean");
    USBSerial.flush();
    esp_restart();
  }
}

/* ---- Sleep-drain forensics (PMU boards) -------------------------------------
 * The S3-2.06's battery is soldered in (no header to meter), and its gauge %
 * was profiled for the ORIGINAL smaller pack — so neither an ammeter nor the
 * percentage can be trusted to quantify sleep drain. This measures with the two
 * signals that ARE honest:
 *   - raw VBAT millivolts (PMU ADC, independent of the gauge's capacity model)
 *     sampled at sleep entry and at the next boot;
 *   - a wake counter in RTC RAM (survives deep sleep, so a WAKE LOOP — the watch
 *     silently rebooting all night, which burns ~100+ mA-seconds per pass and
 *     masquerades as "sleep drain" — shows up as a counter jump).
 * Every boot prints "[drain] ..." with the slept span, wake cause, counter and
 * mV delta; the line is also persisted to NVS ("dgn_last") and echoed on the
 * NEXT boot, so it can be read over USB after a later manual reset even though
 * the original wake happened on battery. */
#if BOARD_HAS_PMU_AXP2101
RTC_DATA_ATTR static uint32_t s_dgn_sleep_epoch = 0;   // when we last entered deep sleep
RTC_DATA_ATTR static uint16_t s_dgn_sleep_mv    = 0;   // VBAT at that entry
RTC_DATA_ATTR static bool     s_dgn_sleep_usb   = false; // USB/charger present at entry
RTC_DATA_ATTR static uint32_t s_dgn_wakes       = 0;   // boots since cold start

static void drain_diag_note_sleep(void) {
  s_dgn_sleep_epoch = (uint32_t)rtc_now_epoch();
  s_dgn_sleep_mv    = board_batt_voltage_mv();
  s_dgn_sleep_usb   = board_vbus_in();
  USBSerial.printf("[drain] sleeping: vbat=%umV%s wake#%lu\n",
                   (unsigned)s_dgn_sleep_mv, s_dgn_sleep_usb ? " (USB!)" : "",
                   (unsigned long)s_dgn_wakes);
}

/* Call AFTER the RTC/clock restore in setup() (rtc_now_epoch must be valid) — an
 * earlier call computed `slept` against the unrestored clock and reported garbage. */
static void drain_diag_boot_report(bool wake_timer, bool wake_button) {
  s_dgn_wakes++;
  char line[160];
  if (s_dgn_sleep_epoch != 0) {
    uint32_t now     = (uint32_t)rtc_now_epoch();
    uint32_t slept   = (now > s_dgn_sleep_epoch) ? (now - s_dgn_sleep_epoch) : 0;
    uint16_t mv      = board_batt_voltage_mv();
    bool     usb_now = board_vbus_in();
    // A VBAT sample taken with USB/charger present reads HIGH (charge voltage, not
    // the cell's rested voltage) — if either end was on USB the delta means nothing.
    if (s_dgn_sleep_usb || usb_now) {
      snprintf(line, sizeof line,
               "wake#%lu cause=%s slept=%lus vbat delta INVALID (USB at %s)",
               (unsigned long)s_dgn_wakes,
               wake_timer ? "timer" : (wake_button ? "button" : "other/reset"),
               (unsigned long)slept,
               s_dgn_sleep_usb ? (usb_now ? "entry+wake" : "entry") : "wake");
    } else {
      snprintf(line, sizeof line,
               "wake#%lu cause=%s slept=%lus vbat %u->%umV (%+d)",
               (unsigned long)s_dgn_wakes,
               wake_timer ? "timer" : (wake_button ? "button" : "other/reset"),
               (unsigned long)slept, (unsigned)s_dgn_sleep_mv, (unsigned)mv,
               (int)mv - (int)s_dgn_sleep_mv);
    }
  } else {
    snprintf(line, sizeof line, "wake#%lu cold boot (no sleep record)",
             (unsigned long)s_dgn_wakes);
  }
  USBSerial.printf("[drain] %s\n", line);
  String prev = prefs.getString("dgn_last", "");
  if (prev.length()) USBSerial.printf("[drain] previous: %s\n", prev.c_str());
  prefs.putString("dgn_last", line);
}
#else
static inline void drain_diag_note_sleep(void) {}
static inline void drain_diag_boot_report(bool, bool) {}
#endif

static void arm_wakes_and_sleep(void) {
  rtc_sleep_intent = SLEEP_INTENT_MAGIC;   // mark this as a CLEAN, intended sleep

  ble_end();                               // BLE can't run in deep sleep — tear it down + free its RAM

  // Periodic notification wake: only arm the timer if the user left background
  // checks ON (Power app toggle). With it OFF the watch sleeps with ONLY the BOOT
  // (EXT0) wake armed below — it never wakes on its own, so no battery is spent on
  // background fetches; you bring it back with a BOOT press. A RUNNING countdown
  // still arms a timer wake regardless, so the alarm always fires on time.
  bool     timer_due_wake = timer_is_active() && !timer_is_paused();
  uint32_t almc_in        = almclk_seconds_until();   // 0 = no alarm clock armed
  if (s_checks_enabled || timer_due_wake || almc_in) {
    // Wake at the SOONEST deadline: the next notification check (only if checks
    // are on), a running countdown's due time, the alarm clock's fire time.
    // (Previously a running countdown with checks OFF woke at the check interval
    // anyway; now it sleeps straight through to its due time.)
    uint64_t us = UINT64_MAX;
    if (s_checks_enabled)
      us = (uint64_t)s_check_interval_min * 60ULL * 1000000ULL;
    if (timer_due_wake) {
      uint64_t tus = (uint64_t)timer_remaining_s() * 1000000ULL;
      if (tus < us) us = tus;
    }
    if (almc_in) {
      uint64_t aus = (uint64_t)almc_in * 1000000ULL;
      if (aus < us) us = aus;
    }
    if (us < 1000000ULL) us = 1000000ULL;   // floor at 1s so we never busy-wake
    esp_sleep_enable_timer_wakeup(us);
    USBSerial.printf("[sleep] timer wake armed in %llus\n", (unsigned long long)(us / 1000000ULL));
  } else {
    USBSerial.println("[sleep] no timer wake (button only)");
  }

  board_wake_arm_button();                 // arm the button wake (EXT0 boards; C6 uses RST)

  board_clock_persist_save();              // snapshot wall-clock to NVS so a no-RTC board
                                           // (C6) can restore it after the RST-button wake
  // Persist the step total before sleep so it survives a power-off / C6 RST-button wake
  // (which wipes RTC memory). Unconditional here (ignores the throttle) since we're about to
  // lose volatile state anyway; only writes if the count actually moved since the last save.
  if (imu_steps_count() != 0) prefs.putUInt("steps", imu_steps_count());
  haptics_prepare_sleep();                 // latch motor LOW so it can't float-buzz
  audio_alarm_prepare_sleep();             // latch the codec/amp rail (GPIO46) OFF too

  // If the step counter OR a sleep-tracking session is running, hand the IMU to the RISC-V
  // ULP so it keeps sampling through deep sleep. Keep RTC_PERIPH powered (ULP + RTC GPIOs
  // alive). The SLEEP session must be checked via imu_sleep_ulp_wanted() (durable RTC flag),
  // not imu_steps_ulp_wanted() (step counter's volatile s_imu_ok), or the sleep ULP would
  // only run for the first span and be dropped on every screen-less wake after — and the rail
  // would get cut, killing the accel mid-night. When neither wants it, normal full-power-down
  // deep sleep proceeds (IMU already off).
  if (imu_steps_ulp_wanted() || imu_sleep_ulp_wanted()) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    // Arm whichever session is active — they're mutually exclusive and have fully separate
    // arm paths/blobs. Sleep is checked first (its durable flag is the authoritative one for
    // an overnight session that spans many screen-less wakes).
    if (imu_sleep_ulp_wanted()) imu_sleep_ulp_arm();
    else                        imu_steps_ulp_arm();
    // SKIP rails_cut_for_sleep(): it could drop the IMU's rail and leave the ULP reading a
    // dead chip. Higher sleep current while actively counting (acceptable for a session).
  } else {
    // No step/sleep session wants the ULP this sleep. FORCE the fully-powered-down state:
    // power the IMU accel OFF in the chip AND put RTC_PERIPH back to AUTO + halt any stale ULP.
    // Both the accel-enable bit and the RTC_PERIPH=ON pd-config PERSIST across deep sleep, so if
    // an earlier session left them on, a session-less sleep would otherwise keep them powered and
    // burn ~tens of mA every cycle — the bimodal 10-vs-50 mA drain. This makes "no session" mean
    // a genuinely clean low-power sleep regardless of prior state.
    USBSerial.println("[sleep] no IMU session -> accel off, ULP halted, RTC_PERIPH auto");
    imu_ensure_off_for_sleep();
    rails_cut_for_sleep();                  // cut the proven-safe peripheral rails, last
  }

  drain_diag_note_sleep();                 // record VBAT + epoch for the boot-side report
  board_enter_sleep();                     // deep sleep; does not return
}

/* (The old compute_wake_seconds() helper is gone: the T5 suspend loop inside
 * enter_deep_sleep() computes the same soonest-deadline min INLINE, because it also
 * needs to know WHICH source drove the deadline — alarm/timer means a lit full wake,
 * a check interval means a dark background fetch.) */

/* If the light check found a notification, this flag tells the full UI boot (ESP:
 * next setup(); T5: the loop() resume path) to pop the card for the newest stored
 * item (s_notifs[0]). The item itself is already persisted by notif_fetch_raw. */
static bool pending_notif = false;

static void enter_deep_sleep(void) {
  USBSerial.println("[power] entering deep sleep");
  USBSerial.flush();
  settings_brightness_commit_now();   // flush any debounced brightness before we stop ticking

  // (Duty-cycle accounting for the sleep-floor learner is now driven entirely by the
  // MEASURED awake/sleep gaps in drain_update() — see power_model.h — so it no longer
  // depends on s_checks_enabled. The old estimate-based notes here were removed to avoid
  // double-counting the sleep span.)

#if !BOARD_PLATFORM_MAIX && !BOARD_PLATFORM_TUYA
  gfx->fillScreen(RGB565_BLACK);
  gfx->displayOff();                      // AMOLED into its own low-power sleep
#endif

#if BOARD_PLATFORM_TUYA
  // T5 sleep = SUSPEND (blocked-in-place low-voltage sleep; resumes on a button, never
  // reboots). The old loop()-managed panel-off state (s_tuya_screen_off + touch wake +
  // tuya_wake_to_watchface) stays removed — while suspended the loop thread is blocked in
  // here, so there is nothing for loop() to manage.
  owf_tuya_display_sleep();               // CO5300 display-off + sleep-in + brightness 0 (true black)
#if OWF_T5_DEEP_SLEEP
  // SUSPEND sleep (round 14 — see owf_tuya_suspend_sleep in owf_tuya_port.h). Mode-2 deep
  // sleep is UNUSABLE on battery: its wake is a reboot, and the boot ROM releases the GPIO19
  // power latch before setup() can re-latch — the wake press itself kills the power (proven
  // by the cable experiment). Suspend never resets anything: quiesce the radios, vote PM
  // low-voltage sleep, and BLOCK inside owf_tuya_suspend_sleep until a wake.
  //
  // PERIODIC WAKES now work in suspend: because a suspend wake RESUMES IN PLACE (no reboot,
  // GPIO19 never released), a timed self-wake is safe — it was only the reboot-style wake
  // that lost battery power. Each pass computes the soonest deadline (background-check
  // interval / running countdown / alarm clock — same schedule as the ESP boards' timer
  // wake) and suspends until a button OR that deadline. A timed wake resumes DARK:
  //   - alarm clock / countdown due  -> full wake; loop()'s handlers ring it on a lit screen;
  //   - background check             -> fetch with the screen off; new item -> full wake
  //                                     (pending_notif pops the card back in loop());
  //                                     nothing new (or sleep-mode DND) -> re-suspend.
  // BLE stays down for the whole suspended span, so ANCS (iPhone) items are not fetched by
  // dark checks — server notifications only, matching the radio quiesce below.
  ble_end();                              // radios can't ride through low-voltage — free BLE
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(150);                             // let the radio teardown settle
  for (;;) {
    // Soonest timed deadline + which source drives it (mirrors arm_wakes_and_sleep).
    bool     timer_due_wake = timer_is_active() && !timer_is_paused();
    uint32_t almc_in        = almclk_seconds_until();   // 0 = no alarm clock armed
    uint64_t check_s = s_checks_enabled ? (uint64_t)s_check_interval_min * 60ULL : UINT64_MAX;
    uint64_t timer_s = timer_due_wake   ? (uint64_t)timer_remaining_s()          : UINT64_MAX;
    uint64_t almc_s  = almc_in          ? (uint64_t)almc_in                      : UINT64_MAX;
    uint64_t soonest = check_s;
    if (timer_s < soonest) soonest = timer_s;
    if (almc_s  < soonest) soonest = almc_s;
    uint32_t wake_s = 0;                                // 0 = button-only
    if (soonest != UINT64_MAX) wake_s = (soonest < 1) ? 1 : (uint32_t)soonest;

    if (owf_tuya_suspend_sleep(wake_s)) break;          // button press -> full wake
    if (soonest == UINT64_MAX) continue;                // spurious: no deadline was armed

    // Timed wake, screen still dark. Alarm/countdown was the deadline -> full wake so
    // loop()'s almclk_fire()/timer handlers ring it on a lit screen.
    if (timer_s == soonest || almc_s == soonest) break;

    // Background notification check (the ESP boards' background_check_has_new, minus the
    // reboot): brief WiFi fetch, then back to suspend unless something new must be shown.
    if (sleep_track_active()) sleep_track_log_wake((uint32_t)rtc_now_epoch());
    String title, body; uint64_t maxId = 0;
    int count = notif_fetch_raw(title, body, maxId);    // connects WiFi itself
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    bool fresh = (count > 0 && maxId > rtc_last_notif_id);
    if (fresh) rtc_last_notif_id = maxId;               // stored either way (bell count right)
    if (fresh && !sleep_track_active()) {               // sleep-mode DND: never light the screen
      pending_notif = true;                             // newest stashed in s_pop_* by the fetch
      break;                                            // -> full wake + pop the card in loop()
    }
    USBSerial.println("[check] nothing to show; re-suspending");
  }
#endif
  // Reached on wake from suspend (or with OWF_T5_DEEP_SLEEP off). Relight and keep running;
  // the caller in loop() resets the idle timer so we don't immediately re-enter.
  USBSerial.println("[power] suspend wake -> relight");
  USBSerial.flush();
  owf_tuya_display_wake();                // CO5300 sleep-out + display-on
  settings_apply_brightness(s_brightness);
  // Restore the radios the suspend entry quiesced. On the ESP boards the wake is a
  // reboot, so setup() re-runs ble_apply_enabled(); this resume-in-place path must do
  // it itself or BLE stays down until manually toggled (bonded iPhone can't reconnect,
  // paired list reads empty). WiFi reconnects lazily on demand as everywhere else.
  ble_apply_enabled();
  return;
#else
  arm_wakes_and_sleep();                  // timer + BOOT wake; does not return
#endif
}

/* ---- Low-battery protective power-off --------------------------------------
 * Unlike enter_deep_sleep() (which arms an RTC timer to wake for background
 * checks), this is a TRUE off: NO timer wake, so the chip stays down and stops
 * draining the cell — critical on the C6, which has no PMU to enforce a cutoff
 * in hardware. The user revives it by charging the battery and pressing RST
 * (C6) / BOOT (S3, still armed below so a deliberate press can bring it back).
 * Never returns. */
static void enter_power_off(void) {
  USBSerial.println("[power] battery critically low -> powering off");
  USBSerial.flush();

  rtc_sleep_intent = SLEEP_INTENT_MAGIC;   // clean, intended sleep
  ble_end();

#if !BOARD_PLATFORM_MAIX && !BOARD_PLATFORM_TUYA
  if (s_display_ready) {                    // skip on the screen-less wake path
    gfx->fillScreen(RGB565_BLACK);
    gfx->displayOff();
  }
#endif

  haptics_prepare_sleep();
  audio_alarm_prepare_sleep();
  // Low battery = the one case where the IMU must NEVER stay on: force the accel off and
  // tear down any ULP state (this path used to skip it entirely — a running session would
  // keep the accel + ULP burning through the "protective" power-off on PMU-less boards).
  imu_ensure_off_for_sleep();

  // Boards WITH a PMU (S3-AXP2101): do a TRUE hardware power-off — cuts the
  // system rail entirely (deeper than deep sleep), revived by a PWR press / USB.
  // board_power_off() returns false on PMU-less boards, where we fall through to
  // the deep-sleep path below.
  if (board_power_off()) {                  // S3: does not return
    delay(200);                             // let the rail collapse
  }
#if BOARD_PLATFORM_TUYA
  // T5: TRUE power-off = drop the GPIO19 keep-alive latch. The supply collapses unless
  // USB is plugged (VBUS holds it); revived by a PWR press. Does not return on battery.
  owf_tuya_power_off();
  delay(200);                               // let the rail collapse
#endif

  // PMU-less boards (C6): deliberately arm NO timer wake — the watch must NOT
  // wake itself for background checks while the battery is empty (that would keep
  // draining it). Revived by the hardware RST button (a cold boot from sleep).
  board_wake_arm_button();                 // C6: RST only (no-op arm)
  rails_cut_for_sleep();
  drain_diag_note_sleep();                 // record VBAT + epoch for the boot-side report
  board_enter_sleep();                     // deep sleep; does not return
}

/* Re-arm the timer (and BOOT wake) and go back to deep sleep WITHOUT touching
 * the display. Used by the light-check path when there's nothing new, so the
 * screen never lights. */
static void rearm_and_deep_sleep(void) {
  USBSerial.println("[check] nothing new; back to deep sleep");
  USBSerial.flush();
  // (Duty accounting moved to drain_update()'s measured gaps — see power_model.h. The next
  // full wake measures this entire sleep span from the RTC epoch delta, so the brief check
  // wakes need no explicit note here; lumping their few awake seconds into sleep is harmless.)
  arm_wakes_and_sleep();                  // timer + BOOT wake; does not return
}

/* LIGHT background check: bring up ONLY I2C(PMU/RTC) + WiFi, check the server.
 * Returns true if there's a new notification (caller then does the full UI
 * boot). On "nothing new", re-arms the alarm and powers off — never returns,
 * so the display is never initialized (screen stays dark). */
static bool background_check_has_new(void) {
  bool pok = board_power_begin();
  Wire.setClock(400000);   // match setup(): 400 kHz Fast-mode (begin may reset it to 100 kHz)
  // Keep the charge cap + low undervoltage cutoff applied on background wakes too
  // (in case begin() reset PMU registers).
  if (pok) board_power_sleep_charge_cfg();

  // Sleep-tracking session active: log this wake's IMU movement to sleep.csv while
  // we're briefly awake. Done before the radio work so the burst sees the device at
  // rest (not jostled by anything we do). (rtc_now_epoch is valid here — RTC/NVS clock
  // is restored in board_power_begin()/clock path above.)
  if (sleep_track_active()) sleep_track_log_wake((uint32_t)rtc_now_epoch());

  String title, body; uint64_t maxId = 0;
  int count = notif_fetch_raw(title, body, maxId);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Do-Not-Disturb (sleep mode): a new notification must NOT wake the screen. Still
  // record it as the newest-seen so the bell count is right and it won't re-trigger,
  // and it's already persisted in the store by notif_fetch_raw — we just refuse the
  // full UI boot and go straight back to sleep. Alarms/timers are handled by a
  // separate wake branch before this function, so they still ring normally.
  if (sleep_track_active()) {
    if (count > 0 && maxId > rtc_last_notif_id) rtc_last_notif_id = maxId;
    rearm_and_deep_sleep();              // back to sleep, screen stays dark
    return false;                        // unreachable
  }

  if (count > 0 && maxId > rtc_last_notif_id) {
    rtc_last_notif_id = maxId;
    pending_notif = true;                  // newest stashed in s_pop_* by the fetch
    return true;                          // -> full boot + show it
  }

  // No NEW server notification — now check the iPhone over BLE (ANCS). If BLE is
  // enabled, bring it up and give iOS a window to reconnect + drain its pending
  // notifications into our store. Any new ANCS item also means "full boot to show
  // it". The ANCS parser already stashed the newest in s_pop_* + set pending_notif
  // semantics via the store, so we just flag pending_notif and boot.
  if (settings_get_ble_enabled()) {
    USBSerial.println("[wake] no server item -> checking iPhone via ANCS...");
    // ~9s budget: long enough for iOS to usually reconnect on a timer wake, short
    // enough to stay frugal. Bails early the instant something lands.
    if (ancs_background_check(9000)) {
      pending_notif = true;                // newest stashed in s_pop_* by the ANCS parser
      return true;                        // -> full boot + show it (BLE left up)
    }
  }

  rearm_and_deep_sleep();                // nothing new (server + iPhone): back to sleep
  return false;                          // unreachable
}
