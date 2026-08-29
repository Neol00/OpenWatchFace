/* ============================================================================
 *  settings_store.h — persistent user settings (NVS), get/set/apply + load.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER the hardware objects
 *  (co5300) and watch_base.h, because:
 *    - settings_apply_brightness() drives board_display_set_brightness()
 *    - settings_apply_cpu_mhz()    calls setCpuFrequencyMhz()
 *    - settings_set_wifi_enabled() touches WiFi
 *    - settings_load() reads s_check_interval_min (defined in watch_base.h)
 *  Owns the Preferences handle `prefs` used by the WiFi/notification stores too,
 *  so it must come before them.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/Preferences.h"
#else
#include <Preferences.h>
#endif

/* Stored in flash via the Preferences library — survives reboots/power-off.
 * The app menu calls settings_get/set_brightness(); this module owns the store
 * and applies brightness to the panel. */
static Preferences prefs;
static uint8_t s_brightness = 204;        // 0-255; ~80%

/* Forward decl: applying a brightness means the panel is at full, so any active
 * auto-dim state is now stale. Defined below with the auto-dim helpers. */
static bool s_dimmed;
static uint8_t s_panel_applied = 0;       // last brightness actually written to the panel (0 = unknown)
static void settings_apply_brightness(uint8_t b) {
  s_brightness = b;
  s_dimmed = false;                       // panel is now at full -> not dimmed
  s_panel_applied = b;                    // keep the dim dedup in sync with this direct write
  board_display_set_brightness(b);        // panel command (CO5300) or PWM backlight
}

/* Called by the app menu's brightness slider (live preview + persist).
 *
 * The NVS write is DEBOUNCED. Applying brightness is instant (a panel command), but persisting is
 * a flash write — and on the Tuya KV store (tal_kv_set) that's tens of ms and BLOCKING. Writing on
 * every slider step made the slider visibly lag on the T5 (ESP32's NVS is RAM-cached, so it never
 * showed there). So we apply LIVE every step and commit the settled value to NVS later, from
 * settings_brightness_commit_tick() in loop() (or settings_brightness_commit_now() before sleep). */
static bool     s_bright_dirty    = false;
static uint8_t  s_bright_pending  = 0;
static uint32_t s_bright_dirty_ms = 0;

static uint8_t settings_get_brightness(void) { return s_brightness; }
static void settings_set_brightness(uint8_t b) {
  settings_apply_brightness(b);           // live, instant (panel command)
  s_bright_pending  = b;                  // defer the flash write until the slider settles
  s_bright_dirty    = true;
  s_bright_dirty_ms = millis();
}

/* Commit the deferred brightness to NVS once it's held steady briefly. Call from loop(). */
static void settings_brightness_commit_tick(uint32_t ms) {
  if (s_bright_dirty && (uint32_t)(ms - s_bright_dirty_ms) >= 700) {
    prefs.putUChar("bright", s_bright_pending);
    s_bright_dirty = false;
  }
}

/* Flush a pending brightness immediately (call before sleep/power-off so a just-set value isn't
 * lost inside the debounce window). Cheap no-op when nothing is pending. */
static void settings_brightness_commit_now(void) {
  if (s_bright_dirty) {
    prefs.putUChar("bright", s_bright_pending);
    s_bright_dirty = false;
  }
}

/* ---- Auto-dim on idle (battery only) ---------------------------------------
 * After a few seconds of no activity (no touch, no notification) the panel dims
 * to save power; any activity restores it. The dim is applied to the PANEL ONLY —
 * it never changes s_brightness — so restoring is exact and the user's chosen
 * brightness is untouched. Toggle + dim level persist in NVS.
 *   - s_autodim_on    : feature enabled (default on)
 *   - s_autodim_pct   : how dim, as a PERCENT of the user's brightness (default 25%)
 *   - s_dimmed        : are we currently dimmed? (RAM only) */
static bool    s_autodim_on  = true;
static uint8_t s_autodim_pct = 25;        // 5..90 % of the user's brightness
// s_dimmed is declared up by settings_apply_brightness (which clears it).

static bool    settings_get_autodim(void)     { return s_autodim_on; }
static uint8_t settings_get_autodim_pct(void) { return s_autodim_pct; }

/* Push the panel to the dimmed level (a fraction of the user's brightness, floored
 * so it never goes fully dark) or back to full — WITHOUT touching s_brightness.
 *
 * Debounce on the ACTUAL APPLIED PANEL LEVEL, not on the s_dimmed bool. The old code
 * returned early when `dim == s_dimmed` — but s_dimmed could fall out of sync with the real
 * panel register (e.g. settings_apply_brightness clears s_dimmed on a wake/brightness change
 * while a separate path had dimmed the panel). Then an un-dim (settings_dim_set(false) with
 * s_dimmed already false) wrote NOTHING and the panel stayed dim until the brightness slider
 * forced an unconditional write — exactly the observed "touch won't undim, but adjusting
 * brightness does". Keying off the last value we actually wrote makes the un-dim self-correct:
 * if the panel is dim, we restore it no matter what the bool says. */
static void settings_dim_set(bool dim) {
  uint8_t target;
  if (dim) {
    uint16_t d = (uint16_t)s_brightness * s_autodim_pct / 100;
    if (d < 8) d = 8;                     // keep it visibly on, never black
    target = (uint8_t)d;
  } else {
    target = s_brightness;                // restore the user's exact brightness
  }
  s_dimmed = dim;
  if (target == s_panel_applied) return;  // already at the right level -> no redundant write
  s_panel_applied = target;
  board_display_set_brightness(target);   // panel only; s_brightness preserved
}

/* Force the panel back to full brightness, unconditionally re-asserting the register even if
 * our bookkeeping already thinks it's full. Called on fresh user activity (touch / BOOT) to
 * SELF-HEAL a desync: the brightness command shares the QSPI bus with the LVGL flush, so a
 * single write can be dropped, leaving the panel stuck dim while s_panel_applied wrongly reads
 * "full" — which the deduped settings_dim_set(false) would then never correct. This bypasses
 * the dedup so any dropped un-dim is fixed by the next tap/press. */
static void settings_undim_force(void) {
  s_dimmed = false;
  s_panel_applied = s_brightness;
  board_display_set_brightness(s_brightness);
}
static bool settings_is_dimmed(void) { return s_dimmed; }

static void settings_set_autodim(bool on) {
  s_autodim_on = on;
  prefs.putBool("autodim", on);
  if (!on) settings_dim_set(false);       // turning it off immediately un-dims
}
static void settings_set_autodim_pct(uint8_t pct) {
  if (pct < 5)  pct = 5;
  if (pct > 90) pct = 90;
  s_autodim_pct = pct;
  prefs.putUChar("autodimp", pct);
  // If we're currently dimmed, re-apply at the new level so the change is visible.
  if (s_dimmed) { s_dimmed = false; settings_dim_set(true); }
}

/* Dim while on USB power too? Default OFF: historically the watch never auto-dims
 * while plugged in (the assumption being you're at a desk / programming). But when
 * you're just charging — e.g. overnight — a screen that keeps lighting up is the
 * annoyance, so this opt-in lets the idle dim apply on USB as well. It ONLY affects
 * dimming: the watch still never deep-sleeps on USB (so it stays reachable while
 * charging), and CAFFEINE still always wins (its whole job is to keep the screen
 * on, so it's never dimmed regardless of this toggle). */
static bool s_dim_on_usb = false;
static bool settings_get_dim_on_usb(void) { return s_dim_on_usb; }
static void settings_set_dim_on_usb(bool on) {
  s_dim_on_usb = on;
  prefs.putBool("dimusb", on);
}

/* Show the little voltage readout in the watch-face top-right corner? Default ON.
 * Purely cosmetic — the value is still read for the battery widget regardless; this
 * only controls whether that corner label is visible. Persisted to NVS. Applied
 * live by watchface_apply_volt_visible() (forward-declared in watch_base.h). */
static bool s_show_volt = true;
static bool settings_get_show_volt(void) { return s_show_volt; }
static void settings_set_show_volt(bool on) {
  s_show_volt = on;
  prefs.putBool("showvolt", on);
  watchface_apply_volt_visible();         // reflect on the face immediately
}

/* Show the weather widget (icon + temperature) on the watch face, under the main
 * dial? Default OFF. When ON, the weekday/date row shifts DOWN to make room for the
 * widget in the middle of the face (see watchface.h). Purely a layout choice — the
 * weather data is fetched regardless; this only controls the on-face widget.
 * Persisted to NVS. Applied live by watchface_apply_weather_visible()
 * (forward-declared in watch_base.h alongside the other watchface_apply_* hooks). */
static bool s_show_weather = false;
static bool settings_get_show_weather(void) { return s_show_weather; }
static void settings_set_show_weather(bool on) {
  s_show_weather = on;
  prefs.putBool("showwx", on);
  watchface_apply_weather_visible();      // reflect on the face immediately
}

/* Swap the WiFi and BLE indicators on the watch face? Default OFF.
 *   OFF (default): WiFi lives in the right stat column, BLE glyph in the top-right tray.
 *   ON           : WiFi glyph moves to the top-right tray, BLE takes the right column.
 * Purely a layout choice — both indicators still reflect their real state regardless;
 * this only controls WHERE each is drawn. Persisted to NVS. Applied live by
 * watchface_apply_indicator_layout() (forward-declared in watch_base.h). */
static bool s_swap_wifi_ble = false;
static bool settings_get_swap_wifi_ble(void) { return s_swap_wifi_ble; }
static void settings_set_swap_wifi_ble(bool on) {
  s_swap_wifi_ble = on;
  prefs.putBool("swapwb", on);
  watchface_apply_indicator_layout();     // reflect on the face immediately
}

/* ---- UI accent color (personalization) -------------------------------------
 * One RGB value (0xRRGGBB) used for every accent in the UI: section headers,
 * graph lines, sliders, switches, the freq highlight and the pull-down shade.
 * Stored as a plain uint32 (no LVGL type here so this header stays UI-agnostic);
 * the UI wraps it in lv_color_hex(). Screens that rebuild on open pick it up for
 * free; the always-resident shade is restyled live via quick_shade_restyle(). */
static uint32_t s_accent = 0x00B0FF;      // default: the blue we started with
static uint32_t settings_get_accent(void) { return s_accent; }
static void settings_set_accent(uint32_t rgb) {
  s_accent = rgb & 0xFFFFFF;
  prefs.putUInt("accent", s_accent);      // persist to NVS (survives reboot)
}

/* Lighten an 0xRRGGBB color by `amt`/255 toward white (integer-only, so it works
 * without LVGL). Used to derive the softer header tint from the base accent so a
 * single knob keeps the two shades coordinated for ANY chosen color. */
static inline uint32_t ui_lighten_hex(uint32_t c, uint8_t amt) {
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r += ((255 - r) * amt) / 255;
  g += ((255 - g) * amt) / 255;
  b += ((255 - b) * amt) / 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static inline uint32_t ui_accent_hex(void)      { return s_accent; }              // base accent
static inline uint32_t ui_accent_soft_hex(void) { return ui_lighten_hex(s_accent, 60); } // header tint

/* ---- Monochrome-accent mode (personalization) ------------------------------
 * When ON, every DECORATIVE color in the UI (the otherwise-varied category icon
 * tints, graph lines, the green pager buttons, the music pink, etc.) collapses to
 * the single accent color, for a plain, uniform look. Structural colors — dark
 * backgrounds, greys and white text — are left alone so contrast/readability are
 * unchanged. Decorative call sites pass their normal color through ui_deco_hex():
 * it returns the accent in mono mode, else the original color, so a single flag
 * recolors the whole UI without each screen knowing about the mode. Persisted. */
#if BOARD_DISPLAY_EPD_GDEQ031T10
/* 1-bit e-paper: mono-accent is FORCED ON. The varied decorative tints don't
 * survive the black/white threshold (several land on the white side and simply
 * vanish), while the accent blue thresholds to solid black — so mono mode is
 * the only rendering that keeps every icon visible. The setter is inert and
 * the Appearance app hides the toggle, so nothing can turn it off. */
static const bool s_mono_accent = true;
static bool settings_get_mono_accent(void) { return true; }
static void settings_set_mono_accent(bool on) { (void)on; }
#else
static bool s_mono_accent = false;
static bool settings_get_mono_accent(void) { return s_mono_accent; }
static void settings_set_mono_accent(bool on) {
  s_mono_accent = on;
  prefs.putBool("monoacc", on);
}
#endif

/* Map a DECORATIVE color through the mono-accent mode. Pass the color a site would
 * normally use; get the accent back when mono mode is on, else the original. Use
 * ONLY for decorative accents (icon tints, graph/slider colors, colored buttons) —
 * never for backgrounds, greys or white, which must stay put for readability. */
static inline uint32_t ui_deco_hex(uint32_t normal) {
  return s_mono_accent ? s_accent : normal;
}

/* Darken an 0xRRGGBB color by `amt`/255 toward black (integer-only). Mirror of
 * ui_lighten_hex; used to derive a DIM accent for "inactive" decorative states. */
static inline uint32_t ui_darken_hex(uint32_t c, uint8_t amt) {
  uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r -= (r * amt) / 255;
  g -= (g * amt) / 255;
  b -= (b * amt) / 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Decorative color for an INACTIVE/dim state (e.g. a disconnected indicator). In
 * mono mode returns a strongly-dimmed accent (so it reads as "the accent, but off");
 * otherwise the site's normal dim color. Pairs with ui_deco_hex for the bright state. */
static inline uint32_t ui_deco_dim_hex(uint32_t normal) {
#if BOARD_DISPLAY_EPD_GDEQ031T10
  /* 1-bit inverted e-paper: any darkened "dim" color falls below the luminance
   * threshold and renders as WHITE — i.e. invisible (the missing disconnected
   * WiFi/BLE icons). There is no "dim" on a 1-bit panel; visible beats subtle,
   * so dim states use the full accent (which thresholds to black ink). State
   * still reads from the adjacent ok/off text. */
  (void)normal;
  return s_accent;
#else
  return s_mono_accent ? ui_darken_hex(s_accent, 200) : normal;
#endif
}

/* ---- Mute (persisted) ----
 * Silences any device sound (alarm tone, future UI sounds) while STILL allowing
 * vibration. Sound paths must check settings_get_mute() before making noise. */
static bool s_mute = false;
static bool settings_get_mute(void) { return s_mute; }
static void settings_set_mute(bool m) { s_mute = m; prefs.putBool("mute", m); }

/* ---- Caffeine / keep-awake (session only, NOT persisted) ----
 * While on, the idle-sleep timeout never fires, so the watch stays awake and
 * timers/stopwatch run to the exact second. Deliberately resets to OFF on every
 * boot so a crash/reboot can't strand the watch awake draining the battery. */
static bool s_caffeine = false;
static bool caffeine_get(void) { return s_caffeine; }
static void caffeine_set(bool e) {
  s_caffeine = e;
  prefs.putBool("caffeine", e);   // remember across reboots/reflashes
}

/* Notification check interval (minutes), persisted. The value itself
 * (s_check_interval_min) lives in watch_base.h. Clamped to the range the Power
 * app's stepper exposes (2..20 min): shorter than 2 wakes too aggressively for the
 * battery, longer than 20 is the cap the UI offers. */
#define CHECK_INTERVAL_MIN_MIN  2
#define CHECK_INTERVAL_MIN_MAX  20
static uint16_t settings_get_check_interval(void) { return s_check_interval_min; }
static void settings_set_check_interval(uint16_t m) {
  if (m < CHECK_INTERVAL_MIN_MIN) m = CHECK_INTERVAL_MIN_MIN;
  if (m > CHECK_INTERVAL_MIN_MAX) m = CHECK_INTERVAL_MIN_MAX;
  s_check_interval_min = m;
  prefs.putUShort("checkmin", m);
}

/* Background-check enable toggle (Settings can flip it off for true power-off). */
static bool s_checks_enabled = true;
static bool settings_get_checks_enabled(void) { return s_checks_enabled; }
static void settings_set_checks_enabled(bool en) {
  s_checks_enabled = en;
  prefs.putBool("checks", en);
}

/* ---- Sleep mode (sleep-quality tracking + Do-Not-Disturb) ------------------
 * When ON the watch is in an overnight tracking session:
 *   - the IMU keeps running through deep sleep (handed to the ULP/LP core, same as
 *     the Fitness step counter) and each periodic wake logs a movement sample to a
 *     CSV on the SD/FFat store, so we can derive sleep quality afterwards;
 *   - Do-Not-Disturb: NOTIFICATIONS must NOT wake the screen (so the watch can't be
 *     bumped awake while you sleep on it and then drain the battery), and no notif
 *     popup cards appear. ALARMS + TIMERS still ring normally — those go through a
 *     separate wake branch (timer_is_due/almclk_is_due) untouched by this flag.
 * Persisted (NVS) so it survives the deep-sleep reboot each periodic wake does, and
 * read in settings_load() before the wake dispatch decides whether to wake the
 * screen for a found notification. Mutually exclusive with the Fitness step counter
 * (both own the IMU/ULP) — the Sleep app refuses to start if counting is running and
 * vice-versa; see app_sleep.h / app_fitness.h. Default OFF. */
static bool s_sleep_mode = false;
static bool settings_get_sleep_mode(void) { return s_sleep_mode; }
static void settings_set_sleep_mode(bool en) {
  s_sleep_mode = en;
  prefs.putBool("sleepmode", en);
}

/* Radio enable toggles, persisted. When WiFi is OFF the watch never
 * brings up the STA radio, so no notification fetches happen — fully silent and
 * lowest-power. BLE is the (future) short-lived credential-sharing channel; the
 * flag gates whether it may advertise at all. Default: WiFi on, BLE off. */
static bool s_wifi_enabled = true;
static bool s_ble_enabled  = false;
static bool settings_get_wifi_enabled(void) { return s_wifi_enabled; }
static void settings_set_wifi_enabled(bool en) {
  s_wifi_enabled = en;
  prefs.putBool("wifien", en);
  if (!en) {                       // going dark: tear down the radio immediately
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}
static bool settings_get_ble_enabled(void) { return s_ble_enabled; }
static void settings_set_ble_enabled(bool en) {
  s_ble_enabled = en;
  prefs.putBool("bleen", en);
  // NOTE: the BLE stack itself isn't wired up yet — this only persists intent so
  // the future provisioning code can honor it. No radio is started here.
}

/* ---- Radio TX power (router-style range knob) ----
 * Lower TX power = lower current during every radio burst (WiFi TX peaks ~300 mA
 * at full power, well under half that at the low tiers) at the cost of range. A
 * small apartment is fine on the low tiers; Max = stock behavior. Five shared
 * tiers, persisted as an index.
 *
 * Apply points (the drivers reset power on init, so set-and-forget won't stick):
 *   - WiFi: settings_apply_wifi_txp() after every successful connect
 *     (wifi_connect in notif_net.h) — esp_wifi_set_max_tx_power needs the driver
 *     STARTED, and each fetch restarts it.
 *   - BLE:  settings_apply_ble_txp() right after BLEDevice::init() (ble_begin),
 *     gated on the controller actually being up so a stray call can't fault. */
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/esp_wifi.h"   // TX-power no-op (no T5 max-tx-power setter)
#include "tuya/compat/esp_bt.h"     // ESP_PWR_LVL_* enum + no-op BLE power (BLE off in P0)
#else
#include <esp_wifi.h>   // esp_wifi_set_max_tx_power
#include <esp_bt.h>     // esp_ble_tx_power_set / esp_bt_controller_get_status
#endif

#define RADIO_TXP_COUNT 7
static const char  *RADIO_TXP_NAMES[RADIO_TXP_COUNT] = { "Min", "VLow", "Low", "Mid", "High", "VHigh", "Max" };
static const int8_t WIFI_TXP_QDBM[RADIO_TXP_COUNT]   = { 8, 20, 28, 44, 60, 72, 80 };  // 0.25 dBm units
static const int8_t WIFI_TXP_DBM[RADIO_TXP_COUNT]    = { 2, 5, 7, 11, 15, 18, 20 };    // UI labels
/* BLE ladder spread low on purpose: the phone is on the same body, not across the
 * room. Min = the controller's hardware floor — works desk-distance, can get
 * marginal wrist-to-pocket THROUGH the body (link shows as lag/reconnects, not
 * silence; just cycle up a tier). VHigh (+9) was the old stock maximum; Max (+20)
 * is the controller's ceiling for long range at a real battery cost. WiFi has no
 * headroom in either direction: its Min (8 quarter-dBm = 2 dBm) and Max (80 = 20
 * dBm) are already the API floor and ceiling.
 *
 * The ESP_PWR_LVL_* enum differs by radio (the C6 floor is -15, the S3 is -24),
 * so the BLE tier mapping + its dBm labels come from the board header. */
static const esp_power_level_t BLE_TXP_LVL[RADIO_TXP_COUNT] = BOARD_BLE_TXP_LVL;
static const int8_t BLE_TXP_DBM[RADIO_TXP_COUNT]     = BOARD_BLE_TXP_DBM;

static uint8_t s_wifi_txp = RADIO_TXP_COUNT - 1;   // default Max = stock (20 dBm)
static uint8_t s_ble_txp  = RADIO_TXP_COUNT - 2;   // default VHigh = stock (+9); Max
                                                   // (+20) is opt-in, it costs real mA

static void settings_apply_wifi_txp(void) {
  // No-op (harmless error) unless the WiFi driver is started; called after each
  // connect so the setting survives the driver's off/on cycles between fetches.
  esp_wifi_set_max_tx_power(WIFI_TXP_QDBM[s_wifi_txp]);
}
static void settings_apply_ble_txp(void) {
  if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) return;
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, BLE_TXP_LVL[s_ble_txp]);  // connections
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     BLE_TXP_LVL[s_ble_txp]);  // advertising
}

static uint8_t settings_get_wifi_txp(void) { return s_wifi_txp; }
static void settings_set_wifi_txp(uint8_t idx) {
  s_wifi_txp = idx % RADIO_TXP_COUNT;
  prefs.putUChar("wifitxp", s_wifi_txp);
  settings_apply_wifi_txp();               // live if the radio happens to be up
}
static uint8_t settings_get_ble_txp(void) { return s_ble_txp; }
static void settings_set_ble_txp(uint8_t idx) {
  s_ble_txp = idx % RADIO_TXP_COUNT;
  prefs.putUChar("bletxp", s_ble_txp);
  settings_apply_ble_txp();                // live if the controller is up
}

/* CPU frequency (MHz) while AWAKE. Lower = less power on the watch face; deep
 * sleep is unaffected (CPU powers down regardless). Only specific values per chip
 * are valid AND WiFi-capable. setCpuFrequencyMhz() applies it.
 *   - S3-2.06 (ESP32-S3): 240 / 160 / 80.
 *   - C6-1.47 (ESP32-C6): max 160 MHz (no 240); 160 / 80. */
#if BOARD_PLATFORM_FOSSIL
extern "C" int cpu_clk_set_mhz(int mhz);   // fossil-port pwr_diag.c (file scope: block-scope extern "C" won't compile)
#endif
#if BOARD_SOC_MSM8909
/* Gen 4 (msm8909w / APQ8009W): the REAL ladder, from this watch's own DTB
 * (qcom,cpufreq-table = 200000 400000 533330 800000 1094400 1267200 kHz).
 * The Gen 6's 400-1306 list below is a different SoC's and must not be shown
 * here — this part cannot reach 1306 MHz at all.
 *
 * NOTE: these are currently DISPLAY-ONLY and the CPU SPEED buttons are hidden
 * on this watch (see app_power.h). On msm8909w the A7 does not own its own
 * clocks or voltages: they belong to the RPM, a separate always-on Cortex-M3,
 * and "setting a frequency" means sending a vote over a shared-memory mailbox.
 * Until rpm-smd is ported, cpu_clk_set_mhz() returns -1 and a speed button
 * would do nothing but lie. */
/* Only the GPLL0-derived rates are offered, because only they are reachable
 * without programming the A7 PLL and voting a higher voltage corner through
 * the RPM (see platform/cpu_clk_a7.c). 1094.4 and 1267.2 MHz from the DTB's
 * qcom,cpufreq-table are deliberately absent: they need corner 9, and a button
 * that cannot do what it says is worse than no button.
 *   800 = GPLL0/1   533 = GPLL0/1.5   400 = GPLL0/2   200 = GPLL0/4 */
static const uint16_t CPU_FREQS[] = { 800, 533, 400, 200 };
static uint16_t s_cpu_mhz = 800;          // the top of our ladder; the live rate
                                          // is reported separately by pwr_cpu_mhz()
#elif BOARD_PLATFORM_FOSSIL
/* Gen 6 (SDM429W): the RCG mux + HF PLL ladder (fossil-port pwr_diag.c
 * cpu_clk_set_mhz, kernel clk-cpu-sdm.c sequence). 800/400 run off GPLL0
 * (bootloader's parking source, PLL off); 960+ reprogram the dedicated APCS
 * HF PLL (L grid = 19.2 MHz). Everything here is DT vdd corner 1 — the SAME
 * rail voltage the bootloader set — so no regulator writes are involved.
 * 1305.6 MHz is the corner-1 ceiling; higher needs CPR rail control (TODO). */
static const uint16_t CPU_FREQS[] = { 1306, 1152, 960, 800, 533, 400 };
static uint16_t s_cpu_mhz = 800;          // bootloader default (GPLL0)
#elif BOARD_PLATFORM_TUYA
/* T5-E1 (BK7258): the DVFS ladder. Each speed sets the core clock + its matched
 * core voltage directly (owf_tuya_cpu_freq.h). Voltage rises with frequency. Highest-first (button row reads left=fast). */
static const uint16_t CPU_FREQS[] = { 480, 320, 240 };
static uint16_t s_cpu_mhz = 480;          // start at 480 MHz (max)
#elif defined(BOARD_WS_C6_TOUCH_LCD_147)
static const uint16_t CPU_FREQS[] = { 160, 80 };
static uint16_t s_cpu_mhz = 160;          // C6 default (its max)
#else
static const uint16_t CPU_FREQS[] = { 240, 160, 80 };
static uint16_t s_cpu_mhz = 240;          // S3 default
#endif
static const uint8_t  CPU_FREQ_COUNT = sizeof(CPU_FREQS) / sizeof(CPU_FREQS[0]);

#if BOARD_PLATFORM_TUYA
/* DPLL overclock (T5 only): a forced VCO band above the stock 480 MHz, with its matched
 * core voltage. Persisted in NVS and applied at boot — but GATED by a survived-boot flag
 * so an OC that hangs on cold start auto-disables on the next power cycle (it can't brick
 * the flash, but it could otherwise boot-loop and need a re-flash to clear). Band 0 is
 * ~544 MHz on the reference part; values are user-tunable from the Power app. */
static bool     s_oc_enabled = false;     // is a persistent overclock active?
static uint8_t  s_oc_band    = 0;         // forced DPLL band (0 = ~544 MHz)
static uint16_t s_oc_mv      = 975;       // matched core voltage (mV)
#endif

/* Lowest CPU clock the watch ever runs at — used on the screen-less background-check
 * wake (no drawing/animation, so max speed is wasted power). 80 MHz is the slowest
 * WiFi-capable PLL clock on BOTH chips (S3 and C6); the radio still works at it, so a
 * timer-wake notification fetch is fine. The full-UI path always restores s_cpu_mhz. */
static const uint16_t CPU_FREQ_LOW = 80;

/* ===========================================================================
 *  EXPERIMENTAL — per-frequency system-rail UNDERVOLT (AXP2101 DCDC1)
 *  ---------------------------------------------------------------------------
 *  Read this before enabling. This is NOT classic core-voltage scaling: the
 *  ESP32-S3 regulates its own ~1.1V digital core with an INTERNAL LDO that
 *  Arduino cannot set. The only software voltage knob on THIS board is the
 *  AXP2101 DCDC1 buck, which feeds the chip's 3.3V VDD pins (plus flash and the
 *  3.3V peripherals). Trimming it shaves the LDO + peripheral conversion loss —
 *  modest savings (single-digit mW), real brownout risk. Lower CPU clocks pull
 *  smaller current spikes, so they tolerate a slightly lower rail before sagging
 *  into a brownout — hence one voltage per frequency.
 *
 *  SAFE WINDOW: the S3 needs 3.0-3.6V and its brownout detector sits near ~3.0V;
 *  flash + the AMOLED want a healthy 3.3V. Stay in [3000, 3300] mV on the AXP2101
 *  100mV grid (driver hard limits are 1500-3400). WiFi TX bursts are the usual
 *  brownout trigger, so this is applied ONLY on the interactive (full-UI) path,
 *  never during background WiFi checks (those stay at the stock 3.3V rail).
 *
 *  TUNING (slow + safe): the voltage is a PURE FUNCTION of the table below and is
 *  NEVER persisted on its own, so every experiment is just a reflash. Lower ONE
 *  entry by 100mV, flash, then stress it (max brightness + a WiFi fetch + busy
 *  CPU) for a while. If you ever see the on-screen "BROWNOUT" boot banner or
 *  random resets, that value is too low — raise it 100mV. Can't boot? Hold BOOT,
 *  tap RST (ROM download mode ignores the app) and reflash; a cold power-cycle
 *  also returns DCDC1 to its 3.3V default. That's why this is hard to brick.
 *
 *  DEFAULT = 3300 everywhere = STOCK (no undervolt). Flip UNDERVOLT_ENABLE to 1
 *  and edit CPU_UVOLT_MV[] (1:1 with CPU_FREQS[] above) to opt in.
 * ======================================================================== */
#define UNDERVOLT_ENABLE  0       // 0 = always stock 3.3V rail; 1 = apply table
#define UNDERVOLT_MIN_MV  3000    // hard floor; the apply fn clamps to this
#define UNDERVOLT_MAX_MV  3300    // never push the rail above the board's 3.3V

// One entry per CPU_FREQS[] (board-specific). This is the AXP2101-RAIL undervolt,
// so it's PMU-only — dead/no-op on the C6 (settings_apply_rail_for_mhz bails when
// no PMU), but it must still match CPU_FREQS[] length for the static_assert.
#if BOARD_PLATFORM_FOSSIL
// Gen 6: no AXP2101 (PM660 rails are corner-managed, untouched here). Stock
// entries only so the static_assert holds; the fossil clock path never reads this.
//   CPU MHz index:                      1306  1152   960   800   533   400
#if BOARD_SOC_MSM8909
static const uint16_t CPU_UVOLT_MV[] = { 3300, 3300, 3300, 3300 };  // 4 rates
#else
static const uint16_t CPU_UVOLT_MV[] = { 3300, 3300, 3300, 3300, 3300, 3300 };
#endif
#elif BOARD_PLATFORM_TUYA
// T5 has no AXP2101 rail to trim (this is the PMU-rail undervolt). One STOCK entry per
// CPU_FREQS[] so the static_assert holds; the real T5 core voltage is set per-frequency
// via vcorehsel in owf_tuya_cpu_freq.h, not this AXP path.
//   CPU MHz index:                      480   320   240
static const uint16_t CPU_UVOLT_MV[] = { 3300, 3300, 3300 };
#elif defined(BOARD_WS_C6_TOUCH_LCD_147)
//   CPU MHz index:                      160    80   (C6 has no PMU rail to trim)
static const uint16_t CPU_UVOLT_MV[] = { 3300, 3300 };
#else
//   CPU MHz index:                      240   160    80
static const uint16_t CPU_UVOLT_MV[] = { 3300, 3300, 3300 };  // STOCK (safe default)
//   Conservative first step to try (validate EACH before trusting it):
//   static const uint16_t CPU_UVOLT_MV[] = { 3300, 3200, 3200 };
//   Aggressive (no WiFi; expect occasional brownouts at the low edges):
//   static const uint16_t CPU_UVOLT_MV[] = { 3200, 3100, 3100 };
#endif
static_assert(sizeof(CPU_UVOLT_MV) == sizeof(CPU_FREQS),
              "CPU_UVOLT_MV[] must have exactly one entry per CPU_FREQS[] entry");

/* Push the rail voltage paired with `mhz` to DCDC1. No-op unless the feature is
 * enabled AND the PMU is present. Clamped to [MIN,MAX] and snapped to the AXP2101
 * 100mV grid (so the PMU always accepts it). The raw rail write lives in
 * board_power.h (board_set_core_rail_mv), included before this header. */
static void settings_apply_rail_for_mhz(uint16_t mhz) {
#if UNDERVOLT_ENABLE
  if (!board_power_ok()) return;                // no PMU -> never touch the rail
  uint16_t mv = UNDERVOLT_MAX_MV;               // fall back to stock if unmatched
  for (uint8_t i = 0; i < CPU_FREQ_COUNT; i++)
    if (CPU_FREQS[i] == mhz) { mv = CPU_UVOLT_MV[i]; break; }
  if (mv < UNDERVOLT_MIN_MV) mv = UNDERVOLT_MIN_MV;
  if (mv > UNDERVOLT_MAX_MV) mv = UNDERVOLT_MAX_MV;
  mv = (uint16_t)((mv / 100) * 100);            // snap to the 100mV step grid
  board_set_core_rail_mv(mv);                   // DCDC1 = ESP32-S3 main 3.3V rail
#else
  (void)mhz;
#endif
}

/* Push a CPU clock to the HARDWARE only (rail + clock + core undervolt) WITHOUT
 * touching the saved s_cpu_mhz. Use this for a TRANSIENT clamp — e.g. dropping to
 * CPU_FREQ_LOW for a screen-less background-check wake — so the user's chosen speed
 * is preserved and can be restored with settings_apply_cpu_mhz(settings_get_cpu_mhz()). */
static void settings_clock_hw(uint16_t mhz) {
#if BOARD_PLATFORM_TUYA
  // T5 (BK7258): set the core divider + its matched core voltage directly (the PM vote is
  // ignored by CP1, and the SDK's stepping switch brownouts at 320M). owf_tuya_set_cpu_mhz
  // keeps the 480 PLL + bus clock (QSPI/PSRAM domain) untouched and only scales the CPU,
  // with voltage-before-speedup / voltage-after-slowdown ordering. Safe to call at boot.
  owf_tuya_set_cpu_mhz(mhz);
#elif BOARD_PLATFORM_FOSSIL
  // Gen 6: the whole safe-switch dance (park on GPLL0 -> reprogram HF PLL ->
  // lock-wait -> switch) lives in the port (pwr_diag.c cpu_clk_set_mhz). No
  // voltage step: every CPU_FREQS[] entry is the same DT vdd corner. On PLL
  // lock failure it stays parked at 800 MHz — always a working clock.
  cpu_clk_set_mhz((int)mhz);
#else
  // Order matters when SPEEDING UP: raise the rail before the clock so the chip
  // never runs fast on a low rail. At boot the PMU isn't up yet, so this call
  // no-ops and the explicit apply after power.begin() handles the initial set.
  settings_apply_rail_for_mhz(mhz);
  setCpuFrequencyMhz(mhz);                 // takes effect immediately
  // Re-apply the REAL core undervolt AFTER the clock change: setCpuFrequencyMhz()
  // reprograms dig_dbias for the new frequency, so our override must follow it.
  // (No-op unless UNDERVOLT_CORE_ENABLE is set in core_voltage.h.) This is the
  // preferred undervolt path; the rail trick above is minor and higher-risk.
  core_apply_for_mhz(mhz);
#endif
}
static void settings_apply_cpu_mhz(uint16_t mhz) {
  s_cpu_mhz = mhz;                          // becomes the new SAVED interactive speed
  settings_clock_hw(mhz);
}
static uint16_t settings_get_cpu_mhz(void) { return s_cpu_mhz; }
static void settings_set_cpu_mhz(uint16_t mhz) {
  settings_apply_cpu_mhz(mhz);
  prefs.putUShort("cpumhz", mhz);          // persist to NVS
}

#if BOARD_PLATFORM_TUYA
/* ---- persistent DPLL overclock (T5) ----------------------------------------------
 * Storage keys in the "watch" namespace: "ocen" (bool), "ocband" (u8), "ocmv" (u16),
 * plus "ocarmed" (bool) — the survived-boot guard, set just before applying OC at boot
 * and cleared once the watch proves it booted OK. If a boot finds "ocarmed" still set,
 * the previous OC boot hung before clearing it -> we DISABLE OC for this boot.
 */
static bool     s_oc_enabled_runtime = false;   // OC actually applied this session?

static uint8_t  settings_get_oc_band(void)   { return s_oc_band; }
static uint16_t settings_get_oc_mv(void)     { return s_oc_mv; }
static bool     settings_get_oc_enabled(void){ return s_oc_enabled; }
static bool     settings_oc_is_active(void)  { return s_oc_enabled_runtime; }

/* Apply the overclock to hardware NOW (band + voltage) and mark it active this session. */
static void settings_oc_apply_hw(void) {
  owf_tuya_dpll_apply_band(s_oc_band, s_oc_mv);
  s_oc_enabled_runtime = true;
}

/* Enable + persist a DPLL overclock with the given band/voltage, and apply it now.
 * Called from the Power app once the user has confirmed band/voltage is stable. */
static void settings_set_oc(uint8_t band, uint16_t mv) {
  s_oc_band = band; s_oc_mv = mv; s_oc_enabled = true;
  prefs.putUChar ("ocband", band);
  prefs.putUShort("ocmv",   mv);
  prefs.putBool  ("ocen",   true);
  settings_oc_apply_hw();
}

/* Disable the persistent overclock and return the CPU to its normal saved speed. */
static void settings_clear_oc(void) {
  s_oc_enabled = false; s_oc_enabled_runtime = false;
  prefs.putBool("ocen", false);
  prefs.putBool("ocarmed", false);
  owf_tuya_set_cpu_mhz(s_cpu_mhz);          // back to the normal divider-based speed
}

/* Boot-time OC apply with the survived-boot gate. Call AFTER settings_load(), in setup,
 * before the watch does heavy work. Returns true if OC was applied this boot. */
static bool settings_oc_boot_apply(void) {
  if (!s_oc_enabled) return false;
  if (prefs.getBool("ocarmed", false)) {
    // Previous OC boot never reached settings_oc_boot_ok() -> it hung. Disable OC now.
    prefs.putBool("ocarmed", false);
    prefs.putBool("ocen",    false);
    s_oc_enabled = false;
    return false;
  }
  prefs.putBool("ocarmed", true);           // arm: must be cleared by a healthy boot
  settings_oc_apply_hw();
  return true;
}

/* Call once the watch has booted far enough to be considered healthy (e.g. first full
 * render / a few seconds into loop). Clears the survived-boot arm so the NEXT boot keeps
 * the OC. Until this runs, a hang/reset leaves "ocarmed" set -> OC auto-disables. */
static void settings_oc_boot_ok(void) {
  if (s_oc_enabled_runtime) prefs.putBool("ocarmed", false);
}
#endif  /* BOARD_PLATFORM_TUYA */

/* Load all persisted settings from NVS into the globals above. Opens the
 * "watch" namespace (shared by the WiFi/notification stores). */
static void settings_load(void) {
#ifdef BOARD_NVS_EXT_LABEL
  // S3-2.06: Preferences live in the 1 MB "nvsext" partition (see the board
  // header + partitions.csv), keeping the 20 KB head "nvs" free for the system
  // stores (WiFi cal, NimBLE bonds). Fall back to the default partition if the
  // label is missing (e.g. flashed over an OLD partition table), so settings
  // still work instead of silently resetting every boot.
  if (!prefs.begin("watch", false, BOARD_NVS_EXT_LABEL)) {
    USBSerial.println("[nvs] nvsext partition missing -> falling back to head nvs");
    prefs.begin("watch", false);
  }
#else
  prefs.begin("watch", false);            // namespace "watch", read/write
#endif
  s_brightness        = prefs.getUChar ("bright",   s_brightness);
  /* On the e-paper board the accent load is SKIPPED: the accent stays the
   * default blue (the one color proven to threshold to solid black) and the
   * picker is hidden — a stale NVS accent from an earlier build must not
   * resurrect an invisible-icon UI. */
#if !BOARD_DISPLAY_EPD_GDEQ031T10
  s_accent            = prefs.getUInt  ("accent",   s_accent);
#endif
  s_mute              = prefs.getBool  ("mute",     s_mute);
  s_check_interval_min= prefs.getUShort("checkmin", s_check_interval_min);
  s_checks_enabled    = prefs.getBool  ("checks",   s_checks_enabled);
  s_sleep_mode        = prefs.getBool  ("sleepmode", s_sleep_mode);
  /* Snap a stale persisted clock to the compile-time default: the freq table
   * is per-board, so a value saved by an image with a DIFFERENT table (e.g.
   * the pre-clock-control fossil builds showed the ESP 240/160/80 buttons)
   * would otherwise select nothing and get clamped to whatever the apply
   * path makes of it (seen on the Gen 6: stale value -> 400 MHz, no button lit). */
  {
    uint16_t cpu_def = s_cpu_mhz;
    s_cpu_mhz = prefs.getUShort("cpumhz", cpu_def);
    bool cpu_ok = false;
    for (uint8_t i = 0; i < CPU_FREQ_COUNT; i++)
      if (CPU_FREQS[i] == s_cpu_mhz) { cpu_ok = true; break; }
    if (!cpu_ok) s_cpu_mhz = cpu_def;
  }
  s_wifi_enabled      = prefs.getBool  ("wifien",   s_wifi_enabled);
  s_ble_enabled       = prefs.getBool  ("bleen",    s_ble_enabled);
  s_wifi_txp          = prefs.getUChar ("wifitxp",  s_wifi_txp) % RADIO_TXP_COUNT;
  s_ble_txp           = prefs.getUChar ("bletxp",   s_ble_txp)  % RADIO_TXP_COUNT;
  s_autodim_on        = prefs.getBool  ("autodim",  s_autodim_on);
  s_autodim_pct       = prefs.getUChar ("autodimp", s_autodim_pct);
  s_dim_on_usb        = prefs.getBool  ("dimusb",   s_dim_on_usb);
  s_show_volt         = prefs.getBool  ("showvolt", s_show_volt);
  s_show_weather      = prefs.getBool  ("showwx",   s_show_weather);
  s_swap_wifi_ble     = prefs.getBool  ("swapwb",   s_swap_wifi_ble);
#if !BOARD_DISPLAY_EPD_GDEQ031T10   /* forced-on there; s_mono_accent is const */
  s_mono_accent       = prefs.getBool  ("monoacc",  s_mono_accent);
#endif
  s_caffeine          = prefs.getBool  ("caffeine", s_caffeine);
#if BOARD_PLATFORM_TUYA
  s_oc_enabled        = prefs.getBool  ("ocen",     s_oc_enabled);
  s_oc_band           = prefs.getUChar ("ocband",   s_oc_band);
  s_oc_mv             = prefs.getUShort("ocmv",     s_oc_mv);
#endif
}
