/* ============================================================================
 *  tuya/owf_tuya_port.h - TuyaOpen (T5-E1) platform bring-up shim
 *
 *  Home for the small surface that lets OpenWatchFace run on the Tuya T5-E1 under
 *  the Arduino-TuyaOpen core. The SDK owns LVGL (display + capacitive touch + tick
 *  + the LVGL handler task) via its lv_vendor layer; this header wraps that so the
 *  .ino's BOARD_PLATFORM_TUYA branches stay tiny and SDK-header-free at the call
 *  site. As later subsystem phases land (RTC, IMU, battery, audio, SD, BLE) this is
 *  where the tkl_* / Wire re-bus shims go - add each only when its module is enabled.
 *
 *  Bring-up sequence (see OpenWatchFace.ino setup()/loop()):
 *    owf_tuya_lvgl_begin()  -> board_register_hardware() + lv_vendor_init()
 *                              (creates the LVGL display + touch indev + tick).
 *    ...firmware builds the whole UI single-threaded (no lock needed yet)...
 *    owf_tuya_lvgl_start()  -> lv_vendor_start() (starts the SDK's LVGL task).
 *  After start(), ONLY the vendor task may touch LVGL: loop() must not call
 *  lv_task_handler(), and any off-task widget mutation must take the display lock
 *  (owf_tuya_disp_lock()/unlock()).
 *
 *  This header is included ONLY from the BOARD_PLATFORM_TUYA build (guarded in the
 *  .ino), so it may reference SDK headers directly.
 * ========================================================================== */
#ifndef OWF_TUYA_PORT_H
#define OWF_TUYA_PORT_H
#pragma once

#if BOARD_PLATFORM_TUYA

#include <time.h>   // time() (AON-RTC-backed) — suspend timed-wake deadline

/* NOTE: we do NOT include lv_vendor.h here. The firmware runs its OWN LVGL v9.5
 * (tuya/owf_tuya_lvgl_own.h includes <lvgl.h>); pulling in lv_vendor.h would drag the
 * SDK's vendor LVGL v8 headers into the same TU -> two conflicting LVGL versions. This
 * header now only does panel/touch registration + power/sleep/backlight (all via the
 * lower-level tdl_disp and tkl HAL APIs, which are LVGL-independent). */

/* ---- Direct CO5300 panel + CST92xx touch registration --------------------
 * The Arduino TUYA_T5AI_BOARD variant's board_register_hardware() registers the WRONG
 * display (ILI9488 RGB, for a 3.5" expansion module) - there is no Arduino board
 * variant for this Waveshare CO5300 QSPI round panel. So we register the CORRECT
 * panel + touch OURSELVES, porting the native board config from the cloned upstream:
 *   TuyaOpen/boards/T5AI/WAVESHARE_T5AI_TOUCH_AMOLED_1_75/board_com_api.c
 * Every value below (466x466, x_offset 6, QSPI0 @80MHz, RST=29, touch CST92xx RST=42
 * on I2C0 P20/P21 mirror_x/y) is taken verbatim from that authoritative file. The
 * register symbols live in libtuyaos.a (verified). Name "display" matches DISPLAY_NAME
 * (Kconfig default) so lv_vendor_init("display") binds to what we register here. */
extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_gpio.h"                 // power-latch hold pin
#include <driver/gpio.h>              // Beken bk_gpio_register_lowpower_keep_status (drive the
                                      // GPIO19 power latch HIGH through the deep-sleep entry;
                                      // toolchain uses 1-byte enums, layout verified)
#include "tal_kv.h"                   // deep-sleep pending flag (survives the controlled reboot)
#include "tkl_system.h"               // tkl_system_reset / tkl_system_get_reset_reason
#include "tkl_thread.h"               // tkl_thread_create (the LV second-core parker task)
#include "tal_sleep.h"                // (legacy include; the light-sleep idle mode itself is removed)
#include "tdd_disp_co5300.h"          // tdd_disp_qspi_co5300_register, CO5300_BL/WRITE_REG regs
#include "tdd_display_qspi.h"         // tdd_disp_qspi_send_cmd + DISP_QSPI_BASE_CFG_T (raw panel cmds)
#include "tdd_tp_cst92xx.h"           // tdd_tp_i2c_cst92xx_register
#include "tdl_display_driver.h"       // tdl_disp_custom_backlight_register
#include "tdl_display_manage.h"       // tdl_disp_find_dev / tdl_disp_set_brightness (runtime brightness)
}

/* Our own CO5300 driver — needed so owf_tuya_set_backlight() can route brightness through it when
 * OWF_T5_OWN_PANEL=1. Self-contained + #pragma once (compiles to nothing when the toggle is 0, and
 * is a no-op re-include when owf_tuya_lvgl_own.h pulls it again). MUST be outside the extern "C". */
#include "owf_tuya_co5300_qspi.h"

/* ---- Soft power latch (CRITICAL on battery) -------------------------------
 * The board has a soft power-latch: the PWR button momentarily applies power, and
 * firmware must immediately drive the KEEP-ALIVE pin (GPIO19, active HIGH) to latch
 * the supply ON - otherwise releasing PWR drops power. On USB, VBUS keeps it alive,
 * which is why USB "just works" and unplugging it kills the device. From the native
 * board config (WAVESHARE_T5AI_TOUCH_AMOLED_1_75/board_com_api.c): PWR_EN=GPIO19
 * HIGH = on, LOW = power-off. Driving it LOW is the deliberate power-off (see
 * owf_tuya_power_off). MUST be called as the FIRST thing in setup(). */
#define OWF_T5_PWR_EN_PIN   TUYA_GPIO_NUM_19

static inline void owf_tuya_power_latch_on(void) {
  TUYA_GPIO_BASE_CFG_T cfg;
  cfg.mode   = TUYA_GPIO_PUSH_PULL;
  cfg.direct = TUYA_GPIO_OUTPUT;
  cfg.level  = TUYA_GPIO_LEVEL_HIGH;   // assert keep-alive -> supply latched on
  tkl_gpio_init(OWF_T5_PWR_EN_PIN, &cfg);
}

/* Deliberate power-off: drop the keep-alive latch (device powers down unless USB is in). */
static inline void owf_tuya_power_off(void) {
  tkl_gpio_write(OWF_T5_PWR_EN_PIN, TUYA_GPIO_LEVEL_LOW);
}

/* (The old light-sleep idle mode — owf_tuya_enter_light_sleep / tal_cpu_set_lp_mode — is
 * REMOVED along with the whole panel-off keep-running sleep state; T5 sleep is the real
 * deep sleep below, nothing else.) */

/* ---- REAL deep sleep (opt-in, OWF_T5_DEEP_SLEEP) --------------------------------------
 * This powers the SoC down; waking is a reboot into a fresh setup(). Wake sources: User button (GPIO12),
 * PWR button (GPIO18), optional AON-RTC timer — all active-LOW presses.
 *
 * VENDOR MECHANISM (disassembled tkl_cpu_sleep_mode_set in libtuyaos_adapter.a): the tkl
 * "deep sleep" SERIALIZES the wake-source table to flash @0x7dc000, busy-waits ~2s, then
 * HARD-RESETS; the next boot's early init (tkl_sleep_param_check_and_set, called in main()
 * BEFORE the app) replays the sources and enters the real sleep from that minimal boot. Its
 * fatal flaw for this board: only the wake sources ride the flash blob — the GPIO19 power-
 * latch keep-status (RAM, system core) does not, so the entry's GPIO shutdown discards the
 * latch pin and on battery the watch FULLY POWERS OFF (RTC lost). USB masks it via VBUS->D3.
 * We therefore reimplement the same reboot-into-quiet-boot shape OURSELVES (two phases below)
 * so the keep-status can be re-registered on the boot that actually enters the sleep.
 *
 * TRADE-OFF: while deep-asleep the chip is OFF, so alarms/timers do NOT fire until a wake
 * (the RTC timed wake below covers scheduled alarms/checks). */
extern "C" {
OPERATE_RET tkl_cpu_sleep_mode_set(BOOL_T enable, TUYA_CPU_SLEEP_MODE_E mode);
OPERATE_RET tkl_wakeup_source_set(const TUYA_WAKEUP_SOURCE_BASE_CFG_T *param);
/* Direct PM entry, bypassing tkl's flash-blob + hard-reset dance (see owf_tuya_deep_sleep_try).
 * bk_pm_ap_sleep_mode_set(mode) sends PM mailbox cmd 11 to the system core (mode 2 = deep
 * sleep) and waits ~100ms; pm_debug_ctrl(8) mirrors the vendor's own entry sequence. Both
 * symbols verified present in the linked vendor libs (libdriver.a / libbk_pm.a). */
int bk_pm_ap_sleep_mode_set(unsigned int sleep_mode);
int pm_debug_ctrl(unsigned int debug_en_value);
/* OWF LV QUERY (vendor patch, ap pwr_clk.c -> mailbox -> cp low_pwr_core.c): reads the
 * system core's low-voltage vote state. Types: 0x10 = missing-votes mask low 32 (bits =
 * pm_sleep_module_name_e indices still NOT asleep), 0x11 = mask high bits, 0x12/0x13 =
 * cumulative count of idle passes that engaged low-voltage / fell back to full-power WFI. */
int owf_pm_ap_get_cp_data(uint32_t type, uint32_t *data);
/* The application's own low-voltage consent vote (module 12 = PM_SLEEP_MODULE_NAME_APP).
 * THE missing piece of suspend: the mailbox forensics showed every other required module
 * already votes asleep (miss:0x1000, eng:0) — the chip idled at FULL POWER (mA-class,
 * ~6h battery) purely because nobody cast this vote after bk_pm_ap_sleep_mode_set(1).
 * Tuya's own tkl_cpu_sleep_mode_set(TUYA_CPU_SLEEP) does exactly mode-set + this vote. */
int bk_pm_module_vote_sleep_ctrl(uint32_t module, uint32_t sleep_state, uint32_t sleep_time);
/* SYSTEM-CORE wake arming (ap pwr_clk.c -> PM_WAKEUP_CONFIG_CMD mailbox -> cp0
 * low_pwr_core_gpio_wakeup_config): registers the pin as a wake source ON THE CORE THAT
 * ACTUALLY OWNS THE PADS DURING LOW-VOLTAGE SLEEP. tkl_wakeup_source_set() only arms the
 * AP-side GPIO driver — meaningless once LV engages (proven: LV slept, no button woke it,
 * RST-only recovery). cp0's wake ISR exits LV and mailbox-notifies the AP awake. */
int bk_pm_ap_gpio_wakeup_source_config(uint32_t sleep_mode, uint32_t wakeup_source, void *gpio_cfg);
/* VENDOR CORE PARK (libbk7258_ap.a sys_pm_hal.c — full 0x21c-byte function verified present
 * in the shipped Arduino archive by disassembly). The ONLY safe way for an AP core to raise
 * its 'parked' bit in AON r3: if cp0's sleep vote (r3 bit3) is up, it masks this core's
 * interrupts + systick, raises this core's enter-wfi bit, WFIs until cp0's mailbox wake
 * kick, then restores everything and re-kicks the sibling core. If cp0 is not voting it is
 * just a plain WFI that returns on the next interrupt. Reached in stock firmware from
 * tickless idle (verified chain: bk_pm_suppress_ticks_and_sleep -> pm_management ->
 * pm_enter_normal_sleep -> sys_drv_enter_normal_sleep -> here) — but the park branch never
 * executes in this image (its FIXED_ADDR counters stay 0), so we call it directly. */
void sys_hal_enter_normal_sleep(uint32_t peri_clk);
}
typedef struct { uint16_t gpio_id; uint16_t int_type; } owf_pm_gpio_wakeup_cfg_t;
#define OWF_PM_SLEEP_MODULE_APP 12u

/* DESIGN HISTORY (do not repeat): every RESET-BASED entry is unfixable on battery. The
 * round-8 verdict log proved that an unattended reset (vendor's flash-blob dance or our own
 * controlled reboot) loses the battery supply BEFORE the next boot's first instruction — the
 * GPIO19 latch does not survive a reset with no finger on PWR and no VBUS. Hence the only
 * viable shape is the NO-RESET entry in owf_tuya_deep_sleep_try below: quiesce, register
 * wake sources + GPIO19 keep-status (RAM tables stay valid — nothing resets), send the PM
 * deep-sleep command directly. The kv keys below remain only so boots can clear leftovers
 * written by the older reboot-based firmware. */
#define OWF_T5_DS_PEND_KEY "owf_ds_pend"   // legacy (reboot-based design); cleared at boot
#define OWF_T5_DS_MARK_KEY "owf_ds_mark"   // legacy breadcrumb; cleared at boot
#define OWF_T5_DS_WAKE_KEY "owf_ds_wake"   // armed just before a PWR-wake deep sleep; found at
                                           // boot = this boot IS the deep-sleep wake (PWR press).
                                           // Value: the pre-sleep rtc_last_notif_id (8 bytes).

/* wake_sec > 0 also arms a periodic/one-shot RTC TIMED wake (the AON RTC stays powered in deep
 * sleep) so the watch can self-wake for housekeeping or a scheduled alarm; 0 = button-only.
 *
 * NO-RESET ENTRY (the only shape that can work on battery — PROVEN by the round-8 verdict
 * log: with the reboot-based design the pending flag survived a battery sleep attempt into a
 * POWERON boot, i.e. the battery supply dies during ANY unattended reset, before the next
 * boot's first instruction. So reset-based entries — the vendor's flash-blob dance AND our
 * two-phase reboot — are unfixable on battery, full stop). Instead: the CALLER quiesces the
 * radios (ble_end + WiFi off; LVGL stops because it runs on the blocked loop thread), we
 * register the wake sources + the GPIO19 keep-status (RAM tables, valid at entry since
 * nothing resets), and send the PM deep-sleep command directly. The earlier direct-entry
 * attempt crash-rebooted with BLE/WiFi LIVE under it — this quiesced retry is the remaining
 * difference. Returns false if it did not engage (caller falls back to light sleep). */
static inline bool owf_tuya_deep_sleep_try(uint32_t wake_sec) {
  // RTC timed wake (optional).
  if (wake_sec > 0) {
    TUYA_WAKEUP_SOURCE_BASE_CFG_T wr;
    memset(&wr, 0, sizeof(wr));
    wr.source = TUYA_WAKEUP_SOURCE_RTC;                      // AON RTC survives deep sleep
    wr.wakeup_para.rtc_param.RTC_num = TUYA_RTC_NUM_0;
    wr.wakeup_para.rtc_param.mode    = TUYA_RTC_MODE_ONCE;   // one-shot; re-armed each sleep
    wr.wakeup_para.rtc_param.ms      = wake_sec * 1000u;
    wr.wakeup_para.rtc_param.cb      = NULL;
    if (tkl_wakeup_source_set(&wr) != OPRT_OK)
      Serial.println("[deep] RTC timed-wake NOT accepted");
    else { Serial.print("[deep] timed wake armed (s): "); Serial.println(wake_sec); }
  }

  // Button wakes: User (GPIO12) + PWR (GPIO18), active-LOW. tkl-only — NEVER add raw bk_gpio
  // level-interrupt arming (watchdog storm), and NEVER register GPIO19 as a wake source (its
  // driven->input transition at entry reads as its own trigger and drops the latch).
  TUYA_WAKEUP_SOURCE_BASE_CFG_T wk;
  memset(&wk, 0, sizeof(wk));
  wk.source = TUYA_WAKEUP_SOURCE_GPIO;
  wk.wakeup_para.gpio_param.gpio_num = TUYA_GPIO_NUM_12;
  wk.wakeup_para.gpio_param.level    = TUYA_GPIO_WAKEUP_LOW;
  if (tkl_wakeup_source_set(&wk) != OPRT_OK)
    Serial.println("[deep] User-button wakeup-source set failed");
  TUYA_WAKEUP_SOURCE_BASE_CFG_T wp;
  memset(&wp, 0, sizeof(wp));
  wp.source = TUYA_WAKEUP_SOURCE_GPIO;
  wp.wakeup_para.gpio_param.gpio_num = TUYA_GPIO_NUM_18;
  wp.wakeup_para.gpio_param.level    = TUYA_GPIO_WAKEUP_LOW;
  if (tkl_wakeup_source_set(&wp) != OPRT_OK)
    Serial.println("[deep] PWR-button wakeup-source set failed");

  // GPIO19 keep-status: the entry must keep DRIVING the power latch HIGH or battery power
  // collapses when the entry's GPIO shutdown discards the pin.
  gpio_config_t keep;
  keep.io_mode   = GPIO_OUTPUT_ENABLE;
  keep.pull_mode = GPIO_PULL_UP_EN;
  keep.func_mode = GPIO_SECOND_FUNC_DISABLE;
  if (bk_gpio_register_lowpower_keep_status(GPIO_19, &keep) != BK_OK)
    Serial.println("[deep] GPIO19 keep-status register FAILED (battery sleep may power off)");

  Serial.println("[deep] no-reset entry: pm sleep mode 2 ...");
  Serial.flush();
  pm_debug_ctrl(8);
  __asm volatile("dsb sy\n\tisb sy" ::: "memory");
  bk_pm_ap_sleep_mode_set(2);                  // PM_MODE_DEEP_SLEEP; engages from idle
  delay(5000);                                 // block loop task -> idle -> power-down here

  // Still running: did not engage. Restore low-voltage idle; the caller relights the panel
  // and the watch just stays awake (there is no light-sleep fallback state any more).
  bk_pm_ap_sleep_mode_set(1);
  Serial.println("[deep] did NOT engage -> staying awake");
  return false;
}

/* ---- REAL deep sleep, PWR-quick-press wake (OWF_T5_DS_QUICK) ---------------------------
 * The schematic (T5-E1-Touch-AMOLED-1.75_schematic.pdf) shows the PWR button (Key2) pulls
 * Q3's gate low through D5 IN HARDWARE — so a PWR press both wakes the chip (GPIO18/SYS_OUT,
 * via D4) and holds the battery rail through the wake reboot until firmware re-latches
 * GPIO19. With the vendor patch that re-latches in entry_main() (both cores, marked "OWF DS"
 * in the WSL t5_os tree), the required hold is only ROM+bootloader time — a quick press.
 *
 * RAIL SAFETY MODEL (updated for the CP "OWF DS" retention patches): the CP core now arms
 * hardware GPIO retention for GPIO19 at every deep-sleep entry (pm_enter_deep_sleep) and
 * engages the AON pad latch, so the battery rail is held IN HARDWARE from sleep entry,
 * through the wake reboot, until boot's gpio_retention_sync re-drives the pin. That makes
 * ANY wake source electrically safe — which is why the USER button (GPIO12, no path to
 * Q3's gate) can be a wake source at all. Without those CP patches a USER wake dies
 * mid-boot; PWR-hold is then the only survivable wake.
 * HARD RULES this entry still enforces:
 *   - RTC timer wakes stay DISARMED until retention is proven on hardware — a failed
 *     unattended wake is a silent power-off (earlier suspends leave RTC armed, so it is
 *     explicitly unregistered here; tkl_wakeup_source_clear only edits tkl's table).
 *   - GPIO19 keep-status must keep driving the latch through the sleep itself.
 *   - Enter only with both buttons released: wakes are LEVEL-low triggers, so entering
 *     with one still held would wake (and reboot) instantly.
 * Wake-cause: OWF_T5_DS_WAKE_KEY is written before entry; the next boot finding it knows it
 * is the deep-sleep wake (there is no other way — the wake is a plain reboot). The value
 * carries rtc_last_notif_id so the notification dedup survives the RAM loss.
 * On success this NEVER RETURNS (the wake is a reboot into setup()). Returns false if the
 * PM did not engage — caller falls back to suspend for this pass. */
extern "C" {
OPERATE_RET tkl_wakeup_source_clear(const TUYA_WAKEUP_SOURCE_BASE_CFG_T *param);
bk_err_t bk_gpio_unregister_wakeup_source(gpio_id_t gpio_id);
/* Cancel a previously registered AON-RTC PM wake (mirrors the adapter's own
 * _bk_rtc_wakeup_unregister: period_cnt = 0 unregisters). Layout of pm_ap_rtc_info_t is
 * taken from the platform's driver/pm_ap_core.h (tick, cnt, callback, param). */
typedef struct { unsigned int period_tick; unsigned int period_cnt;
                 void *callback; void *param_p; } owf_pm_ap_rtc_info_t;
int bk_pm_ap_rtc_regsiter_wakeup(unsigned int sleep_mode, void *info);   /* [sic] vendor name */
}

static inline bool owf_tuya_deep_sleep_quick(uint64_t notif_id_persist) {
  // Wait for BOTH buttons released (level-low wakes would fire instantly — the USER
  // double-tap sleep gesture reaches here with GPIO12 still held), bounded like the ESP path.
  { uint32_t t0 = millis();
    while ((digitalRead(18) == LOW || digitalRead(12) == LOW) &&
           (uint32_t)(millis() - t0) < 3000) delay(10);
    delay(50); }

  // Disarm the RTC wake left by earlier suspends. UNATTENDED wakes stay off until the
  // GPIO19 hardware retention (CP "OWF DS" patches) is proven: if retention failed, a
  // timer wake with nobody near the watch = silent power-off. Button wakes are the same
  // experiment the user performs anyway, so both buttons are armed below.
  owf_pm_ap_rtc_info_t rtc_off;
  memset(&rtc_off, 0, sizeof(rtc_off));                    // period_cnt 0 = unregister
  bk_pm_ap_rtc_regsiter_wakeup(2 /*PM_MODE_DEEP_SLEEP*/, &rtc_off);
  bk_pm_ap_rtc_regsiter_wakeup(1 /*PM_MODE_LOW_VOLTAGE*/, &rtc_off);
  TUYA_WAKEUP_SOURCE_BASE_CFG_T clr;
  memset(&clr, 0, sizeof(clr));
  clr.source = TUYA_WAKEUP_SOURCE_RTC;
  tkl_wakeup_source_clear(&clr);

  // Arm the PWR button (GPIO18) wake ONLY, active-LOW. USER (GPIO12) stays DISARMED in
  // mode 2: the pads are NOT held through the wake reset (proven on battery), so a USER
  // wake has no finger on PWR holding Q3's gate — the rail dies mid-boot and the watch
  // cold powers-off. Only a ~1s PWR press bridges the gap until entry_main re-latches
  // GPIO19. (Super deep sleep, which DOES hold the pads, refuses GPIO12/18 as wake pins
  // entirely — tried 2026-08-12, wedged the watch until RST.)
  TUYA_WAKEUP_SOURCE_BASE_CFG_T wp;
  memset(&wp, 0, sizeof(wp));
  wp.source = TUYA_WAKEUP_SOURCE_GPIO;
  wp.wakeup_para.gpio_param.gpio_num = TUYA_GPIO_NUM_18;
  wp.wakeup_para.gpio_param.level    = TUYA_GPIO_WAKEUP_LOW;
  if (tkl_wakeup_source_set(&wp) != OPRT_OK) {
    Serial.println("[deep] PWR wakeup-source set FAILED -> not sleeping");
    return false;                                          // no wake source = would brick
  }
  bk_gpio_unregister_wakeup_source(GPIO_12);               // strip any suspend-era USER arm

  // Keep driving the GPIO19 latch through the sleep (same as the suspend entry).
  gpio_config_t keep;
  keep.io_mode   = GPIO_OUTPUT_ENABLE;
  keep.pull_mode = GPIO_PULL_UP_EN;
  keep.func_mode = GPIO_SECOND_FUNC_DISABLE;
  if (bk_gpio_register_lowpower_keep_status(GPIO_19, &keep) != BK_OK) {
    Serial.println("[deep] GPIO19 keep-status FAILED -> not sleeping (battery would die)");
    return false;
  }

  // Wake marker + notif-id carry-over (RAM does not survive; kv does).
  tal_kv_set((const char *)OWF_T5_DS_WAKE_KEY, (uint8_t *)&notif_id_persist,
             sizeof(notif_id_persist));

  // GPIO19 RETENTION, armed DIRECTLY from here as well (AON PMU r0 is shared memory):
  // gpio_retention_bitmap = r0 bits[12:19], slot 1 of the CP's GPIO_RETENTION_MAP
  // (= GPIO_19 with the CP "OWF DS" patch) is bit 13. The CP's pm_enter_deep_sleep is
  // *supposed* to arm this too — writing it here makes the arming independent of which
  // CP path actually executes the sleep. The analog commit (r25 magic + gpio_sleep
  // latch) still happens inside sys_hal_enter_deep_sleep (CP patch). Diagnostics are
  // printed so a serial capture shows exactly what was armed and what survived.
  {
    volatile uint32_t *r0  = (volatile uint32_t *)0x44000000u;   // AON PMU r0
    volatile uint32_t *r7b = (volatile uint32_t *)0x440001ECu;   // AON PMU r7b (0x7b<<2)
    uint32_t before = *r0;
    *r0 = before | (1u << 13);                                   // retention slot1 = HIGH
    Serial.print("[deep] AON r0 before/after arm: 0x"); Serial.print(before, HEX);
    Serial.print(" / 0x"); Serial.print(*r0, HEX);
    Serial.print("  r7b: 0x"); Serial.println(*r7b, HEX);
  }

  Serial.println("[deep] entering deep sleep (mode 2); wake = PWR press (~1s)");
  Serial.flush();
  pm_debug_ctrl(8);
  __asm volatile("dsb sy\n\tisb sy" ::: "memory");
  // Mode 2 (normal deep sleep), NOT mode 3. Super deep sleep was tried (2026-08-12) and
  // hard-wedged the watch on battery: the GPIO19 retention latch held the rail on in
  // hardware, but super deep does NOT accept GPIO12/18 as wake pins on this part — chip
  // off, rail latched, no button wake and no button power-off; only RST or a battery
  // pull recovers. Mode 2 keeps the known-working behavior: pads are NOT held through
  // the wake reset, so the wake press must be PWR held ~1s until entry_main re-latches
  // GPIO19 (a shorter press cuts the rail = cold power-off, recoverable).
  bk_pm_ap_sleep_mode_set(2);                  // PM_MODE_DEEP_SLEEP; engages from idle
  delay(5000);                                 // block loop task -> idle -> power-down here

  // Still running: did not engage. Clean the marker so the NEXT boot isn't misread as a
  // deep-sleep wake, restore low-voltage idle, let the caller fall back to suspend.
  tal_kv_del((const char *)OWF_T5_DS_WAKE_KEY);
  bk_pm_ap_sleep_mode_set(1);
  Serial.println("[deep] did NOT engage -> falling back to suspend");
  return false;
}

/* Wake-cause readback for the boot after a deep sleep (set by owf_tuya_deep_sleep_boot_check). */
static bool     s_owf_t5_ds_wake = false;
static uint64_t s_owf_t5_ds_saved_nid = 0;
static inline bool     owf_tuya_woke_from_deep_sleep(void) { return s_owf_t5_ds_wake; }
static inline uint64_t owf_tuya_ds_saved_notif_id(void)    { return s_owf_t5_ds_saved_nid; }

/* ---- SUSPEND sleep (resume-style; the fallback / scheduled-wake sleep mode) ------------
 * ROOT CAUSE of the battery deaths (round 14, proven by the user's cable experiment): mode-2
 * deep sleep SURVIVES on battery — but its wake is a reboot through the boot ROM, which
 * releases GPIO19 before setup() can re-latch. Q3's gate rises through R24 (10k) in
 * microseconds and the battery rail dies AT THE WAKE PRESS.
 * RESOLVED (schematic trace + "OWF DS" vendor patch): the PWR button hardware-holds Q3's
 * gate through D5 for as long as it is pressed, and the vendor patch re-latches GPIO19 in
 * entry_main() — so a quick PWR press bridges the window. That is exactly (and only) what
 * owf_tuya_deep_sleep_quick above implements; any OTHER wake (USER button, RTC timer) still
 * dies mid-reboot, which is why suspend remains the mode whenever something must self-fire.
 * (owf_tuya_deep_sleep_try stays as the multi-wake-source variant for the day a ~100uF cap
 * on Q3's gate makes unattended wakes survivable.)
 *
 * So instead: SUSPEND. Vote PM low-voltage sleep (mode 1) and block right here polling the
 * buttons. Nothing ever resets, GPIO19 never stops being driven, RAM and the whole UI stay
 * intact; a button press simply makes this function return and the caller relights the panel.
 * The wake sources are still registered so the PM can exit low-voltage when it does engage.
 *
 * TIMED WAKE (wake_sec > 0): unlike the reboot-style deep-sleep wake (which releases the
 * GPIO19 latch and killed battery power), a suspend wake RESUMES IN PLACE — nothing resets —
 * so a timed self-wake is safe here. An AON-RTC one-shot wake source lets the PM exit
 * low-voltage at the deadline, and the poll loop checks the deadline against time() (backed
 * by the AON RTC, so immune to any tick distortion under low-voltage sleep). Returns TRUE
 * for a button wake (caller relights), FALSE when the timed deadline expired (caller runs
 * its dark housekeeping — background check / alarm — and decides whether to relight or
 * re-suspend). wake_sec == 0 = button-only, blocks indefinitely. */
/* LV SECOND-CORE PARKER: low-voltage needs BOTH AP cores parked at the vendor WFI safe
 * point at the same instant — each core raises its own AON r3 enter-wfi bit there and cp0
 * samples both during its 3ms vote window. Writing those bits from software instead is
 * FATAL: cp0 then clock-gates a core mid-instruction and it never comes back (proven twice
 * on hardware, RST-only recovery). The suspend loop below parks the core it runs on; this
 * always-ready task is scheduled onto the other core by the 2-core SMP scheduler and parks
 * it the same way. Outside suspend it just sleeps. */
static volatile uint32_t s_owf_park_active = 0;
static void owf_lv_parker_task(void *arg) {
  (void)arg;
  for (;;) {
    if (s_owf_park_active) sys_hal_enter_normal_sleep(0); // parks when cp0's vote is up, else plain WFI
    else tkl_system_sleep(100);
  }
}
static void owf_lv_parker_start(void) {
  static TKL_THREAD_HANDLE s_parker = NULL;
  if (s_parker == NULL &&
      tkl_thread_create(&s_parker, "owf_park", 2048, 1, owf_lv_parker_task, NULL) != OPRT_OK) {
    s_parker = NULL;
    Serial.println("[susp] parker task create FAILED (second core cannot park -> no LV)");
  }
}

static inline bool owf_tuya_suspend_sleep(uint32_t wake_sec) {
  // Button wakes: User (GPIO12) + PWR (GPIO18), active-LOW — same registrations, same NEVERs
  // as the deep-sleep entry (no raw level ints, never GPIO19 as a source).
  TUYA_WAKEUP_SOURCE_BASE_CFG_T wk;
  memset(&wk, 0, sizeof(wk));
  wk.source = TUYA_WAKEUP_SOURCE_GPIO;
  wk.wakeup_para.gpio_param.gpio_num = TUYA_GPIO_NUM_12;
  wk.wakeup_para.gpio_param.level    = TUYA_GPIO_WAKEUP_LOW;
  if (tkl_wakeup_source_set(&wk) != OPRT_OK)
    Serial.println("[susp] User-button wakeup-source set failed");
  TUYA_WAKEUP_SOURCE_BASE_CFG_T wp;
  memset(&wp, 0, sizeof(wp));
  wp.source = TUYA_WAKEUP_SOURCE_GPIO;
  wp.wakeup_para.gpio_param.gpio_num = TUYA_GPIO_NUM_18;
  wp.wakeup_para.gpio_param.level    = TUYA_GPIO_WAKEUP_LOW;
  if (tkl_wakeup_source_set(&wp) != OPRT_OK)
    Serial.println("[susp] PWR-button wakeup-source set failed");

  // Timed wake: AON-RTC one-shot so the PM can exit low-voltage at the deadline (re-armed by
  // the caller on every re-suspend). The deadline itself is enforced below via time().
  if (wake_sec > 0) {
    TUYA_WAKEUP_SOURCE_BASE_CFG_T wr;
    memset(&wr, 0, sizeof(wr));
    wr.source = TUYA_WAKEUP_SOURCE_RTC;
    wr.wakeup_para.rtc_param.RTC_num = TUYA_RTC_NUM_0;
    wr.wakeup_para.rtc_param.mode    = TUYA_RTC_MODE_ONCE;
    wr.wakeup_para.rtc_param.ms      = (wake_sec > 4294967u) ? 0xFFFFFFFFu : wake_sec * 1000u;
    wr.wakeup_para.rtc_param.cb      = NULL;
    if (tkl_wakeup_source_set(&wr) != OPRT_OK)
      Serial.println("[susp] RTC timed-wake NOT accepted (button-only this sleep)");
  }

  // GPIO19 keep-status: low-voltage sleep's GPIO shutdown honors it too — keep the latch driven.
  gpio_config_t keep;
  keep.io_mode   = GPIO_OUTPUT_ENABLE;
  keep.pull_mode = GPIO_PULL_UP_EN;
  keep.func_mode = GPIO_SECOND_FUNC_DISABLE;
  if (bk_gpio_register_lowpower_keep_status(GPIO_19, &keep) != BK_OK)
    Serial.println("[susp] GPIO19 keep-status register FAILED");

  // SYSTEM-CORE wake sources — the ones that actually fire in low-voltage sleep. The tkl
  // registrations above only arm the AP-side driver (kept for the legacy poll path); once
  // LV truly engages the AP is frozen and only cp0 can wake the chip. Level-LOW so a held
  // button keeps re-waking cp0 until the AP loop gets enough run time to see it.
  owf_pm_gpio_wakeup_cfg_t g12; g12.gpio_id = 12; g12.int_type = 0; // GPIO_INT_TYPE_LOW_LEVEL
  owf_pm_gpio_wakeup_cfg_t g18; g18.gpio_id = 18; g18.int_type = 0;
  bk_pm_ap_gpio_wakeup_source_config(1 /*PM_MODE_LOW_VOLTAGE*/, 0 /*WAKEUP_SOURCE_INT_GPIO*/, &g12);
  bk_pm_ap_gpio_wakeup_source_config(1 /*PM_MODE_LOW_VOLTAGE*/, 0 /*WAKEUP_SOURCE_INT_GPIO*/, &g18);
  {
    // Proof the arming crossed the mailbox: cp0 counts every GPIO-wake registration it
    // receives (query 0x18) and every wake-ISR fire (0x19). arm >= 2 here or the buttons
    // are NOT wake sources this sleep.
    uint32_t armed = 0, fired = 0;
    owf_pm_ap_get_cp_data(0x18u, &armed);
    owf_pm_ap_get_cp_data(0x19u, &fired);
    Serial.print("[susp] cp0 gpio-wake armed:"); Serial.print(armed);
    Serial.print(" isr-fires:"); Serial.println(fired);
  }
  // Timed wake must ALSO be cp0-level: the tkl RTC wake registers for deep-sleep mode only
  // (tkl_wakeup.c), so an LV sleep would never see it. Periodic is fine — first fire ends
  // the sleep via the deadline check; unregistered again on every wake path below.
  if (wake_sec > 0) {
    owf_pm_ap_rtc_info_t lv_rtc;
    memset(&lv_rtc, 0, sizeof(lv_rtc));
    lv_rtc.period_tick = (wake_sec > 4294967u) ? 0xFFFFFFFFu : (unsigned int)(wake_sec * 1000u);
    lv_rtc.period_cnt  = 0xFFFFFFFFu;
    bk_pm_ap_rtc_regsiter_wakeup(1 /*PM_MODE_LOW_VOLTAGE*/, &lv_rtc);
  }

  Serial.print("[susp] suspend: pm low-voltage (mode 1); waiting for User/PWR press");
  if (wake_sec > 0) { Serial.print(" or timed wake in (s): "); Serial.print(wake_sec); }
  Serial.println();
  Serial.flush();
  // Deadline off the AON-RTC-backed wall clock (time() == bk_rtc_gettimeofday), NOT millis():
  // the FreeRTOS tick may be suppressed/compensated across low-voltage spans.
  time_t deadline = (wake_sec > 0) ? time(NULL) + (time_t)wake_sec : (time_t)0;
  delay(50);                               // let cp0's async shell drain the UART before LV can cut it
  bk_pm_ap_sleep_mode_set(1);              // PM_MODE_LOW_VOLTAGE vote; engages from idle
  bk_pm_module_vote_sleep_ctrl(OWF_PM_SLEEP_MODULE_APP, 1, 0);  // app consents -> LV can engage
  // CORE PARKING (the final LV gate): the system core refuses low-voltage until BOTH AP
  // cores raise their 'parked in WFI' bit in AON r3 during its 3ms vote window. NEVER write
  // those bits directly — cp0 then gates a core mid-instruction and it never recovers
  // (proven twice on hardware; RST-only). Instead both cores park through the vendor's own
  // sys_hal_enter_normal_sleep: this loop parks the core it runs on, the parker task parks
  // the other. Parked cores sit in WFI with only the mailbox wake enabled; cp0's wake ISR
  // (armed above) exits LV and kicks them back to life (CP patch re-enabled
  // bk_pm_cp_wakeup_ap_from_wfi + 150ms LV re-entry cooldown = guaranteed poll window).
  owf_lv_parker_start();
  s_owf_park_active = 1;
  uint32_t lv_eng_at_entry = 0;
  owf_pm_ap_get_cp_data(0x12u, &lv_eng_at_entry);  // LV cycles so far; delta at wake = proof
  uint32_t lv_poll = 0, lv_eng_prev = 0, lv_blk_prev = 0;
  for (;;) {
    sys_hal_enter_normal_sleep(0);         // park HERE. While cp0 isn't voting: plain WFI,
                                           // returns on the next tick (~ms) = cheap poll
                                           // pacing. When LV engages: returns only on cp0's
                                           // wake kick (button/RTC), inside the cooldown.
    if ((lv_poll % 500) == 0) delay(2);    // let idle run briefly (~every 0.5s of awake
                                           // time) so its hook can feed the task watchdog —
                                           // this loop no longer blocks on the scheduler
    // OWF LV QUERY: every >=10s of WALL time (pass counts are useless pacing — a pass can
    // be 1ms of WFI or a few µs on a pending-interrupt bailout, which is what turned the
    // last build into a log firehose), pull the system core's vote state over the mailbox
    // and print it (CP-side logs never reach this UART). eng/blk are deltas since the
    // previous line: eng==0 while suspended == the chip NEVER reached low-voltage
    // (full-power idle = the mA-class suspend drain); miss bits name the vetoing modules.
    ++lv_poll;
    static time_t s_lv_last_print = 0;
    time_t lv_now = time(NULL);
    if (lv_now - s_lv_last_print >= 10) {
      s_lv_last_print = lv_now;
      uint32_t miss_lo = 0, miss_hi = 0, eng = 0, blk = 0;
      uint32_t ps1 = 0, ps0 = 0, subpwr = 0, subwfi = 0;
      owf_pm_ap_get_cp_data(0x10u, &miss_lo);
      owf_pm_ap_get_cp_data(0x11u, &miss_hi);
      owf_pm_ap_get_cp_data(0x12u, &eng);
      owf_pm_ap_get_cp_data(0x13u, &blk);
      owf_pm_ap_get_cp_data(0x14u, &ps1);     // AP-core PSRAM allocations (0 -> LV powers PSRAM OFF!)
      owf_pm_ap_get_cp_data(0x15u, &ps0);     // system-core PSRAM allocations
      owf_pm_ap_get_cp_data(0x16u, &subpwr);  // subcore power-domain register
      owf_pm_ap_get_cp_data(0x17u, &subwfi);  // bit0 = all subcores in WFI (the second entry gate)
      Serial.print("[susp] LV eng:"); Serial.print(eng - lv_eng_prev);
      Serial.print(" blk:");          Serial.print(blk - lv_blk_prev);
      Serial.print(" miss:0x");       Serial.print(miss_hi, HEX);
      Serial.print("_");              Serial.print(miss_lo, HEX);
      Serial.print(" psram:");        Serial.print(ps1);
      Serial.print("/");              Serial.print(ps0);
      Serial.print(" subpwr:0x");     Serial.print(subpwr, HEX);
      Serial.print(" wfi:0x");        Serial.print(subwfi, HEX);
      // Handshake proof-of-life (vendor FIXED_ADDR block @CONFIG_PWR_MNG_ADDR, shared SRAM):
      // ap0/ap1 increment each time that AP core actually runs the vote-aware WFI handshake
      // (sys_hal_enter_normal_sleep with cp0's sleep vote seen); cp = system-core wake count.
      // Frozen ap counters while suspended == that core never participates == the wfi gate
      // can never pass == LV never engages.
      Serial.print(" hs:");           Serial.print(*(volatile uint32_t *)0x2809F70Cu);
      Serial.print("/");              Serial.print(*(volatile uint32_t *)0x2809F710u);
      Serial.print("/");              Serial.print(*(volatile uint32_t *)0x2809F708u);
      // park2: cpu1-branch park count (FIXED_ADDR +20) — the parker task's core. r3: raw
      // AON PMU r3 (bit3 = cp0 voting right now, bits1/2 = live enter-wfi bits). rst:
      // system-core/AP reset reasons (+24/+28) — nonzero change here = cp0 crash-rebooted
      // (the all-zero-mailbox forensics from the crash capture).
      Serial.print(" park2:");        Serial.print(*(volatile uint32_t *)0x2809F714u);
      Serial.print(" r3:0x");         Serial.print(*(volatile uint32_t *)0x4400000Cu, HEX);
      Serial.print(" rst:");          Serial.print(*(volatile uint32_t *)0x2809F718u, HEX);
      Serial.print("/");              Serial.println(*(volatile uint32_t *)0x2809F71Cu, HEX);
      lv_eng_prev = eng; lv_blk_prev = blk;
    }
    if (digitalRead(12) == LOW || digitalRead(18) == LOW) break;   // press on either button = wake
    if (deadline && time(NULL) >= deadline) {
      s_owf_park_active = 0;                                        // parker back to sleep
      bk_pm_module_vote_sleep_ctrl(OWF_PM_SLEEP_MODULE_APP, 0, 0);  // app awake again
      { owf_pm_ap_rtc_info_t lv_rtc_off; memset(&lv_rtc_off, 0, sizeof(lv_rtc_off));
        bk_pm_ap_rtc_regsiter_wakeup(1, &lv_rtc_off); }             // stop the periodic LV wake
      Serial.println("[susp] timed deadline reached -> resuming (dark)");
      return false;                        // timed wake; caller handles housekeeping
    }
  }
  s_owf_park_active = 0;                                            // parker back to sleep
  bk_pm_module_vote_sleep_ctrl(OWF_PM_SLEEP_MODULE_APP, 0, 0);      // app awake again
  { owf_pm_ap_rtc_info_t lv_rtc_off; memset(&lv_rtc_off, 0, sizeof(lv_rtc_off));
    bk_pm_ap_rtc_regsiter_wakeup(1, &lv_rtc_off); }                 // stop the periodic LV wake
  { uint32_t eng_now = 0;
    owf_pm_ap_get_cp_data(0x12u, &eng_now);
    Serial.print("[susp] woke; LV cycles this sleep: ");
    Serial.println(eng_now - lv_eng_at_entry);  // 0 = never actually reached low-voltage
    Serial.flush(); }
  // CONSUME the wake press: wait for release before returning, so loop()'s button handlers
  // never see it as a fresh tap (a User "tap" on wake would otherwise open the app menu).
  // A PWR press held >=2.5s here still powers off, matching the awake long-press behavior
  // (otherwise the hold would be invisible — loop() is blocked until we return).
  uint32_t held_ms = 0;
  while (digitalRead(12) == LOW || digitalRead(18) == LOW) {
    delay(10);
    held_ms += 10;
    if (held_ms >= 2500 && digitalRead(18) == LOW)
      owf_tuya_power_off();                // drops the GPIO19 latch (no return on battery)
  }
  // Mode 1 stays set — it's the same "low-voltage idle allowed" state the watch has always
  // run in after a non-engaging deep-sleep entry; normal operation is unaffected by it.
  Serial.println("[susp] button press -> resuming");
  return true;
}

/* Boot-time cleanup — call FIRST THING in setup(). The reboot-based two-phase design is GONE
 * (its Phase-1 reset killed battery power before Phase 2 could run — proven by the round-8
 * verdict log), but a watch upgraded from that firmware may still carry its flags in kv;
 * clear them so they can never trigger anything, and report the old breadcrumb if present. */
static inline void owf_tuya_deep_sleep_boot_check(void) {
  uint8_t *val = NULL; size_t len = 0;
  // Deep-sleep wake detection: the PWR-quick-press entry writes OWF_T5_DS_WAKE_KEY right
  // before sleeping, so finding it here means THIS boot is the wake (the wake is a plain
  // reboot — there is no hardware wake-cause that survives it). Payload = the pre-sleep
  // rtc_last_notif_id (notification dedup across the RAM loss). Edge case: if the battery
  // died DURING the sleep, the next cold boot also finds the key — it then just takes the
  // (lit, full-UI) button-wake path, which is what a cold boot does anyway.
  // Wake-path forensics: r0 was recovered from the analog-retained r7b by the vendor's
  // sys_hal_low_power_hardware_init before we run, so bit13 still set here == the retention
  // bitmap SURVIVED the deep sleep; GPIO19 pad reg 0x32 == the pin came up driven HIGH.
  {
    volatile uint32_t *r0   = (volatile uint32_t *)0x44000000u;
    volatile uint32_t *r7b  = (volatile uint32_t *)0x440001ECu;
    volatile uint32_t *p19  = (volatile uint32_t *)(0x44000400u + 19u * 4u);
    Serial.print("[deep] boot AON r0: 0x");  Serial.print(*r0, HEX);
    Serial.print("  r7b: 0x");               Serial.print(*r7b, HEX);
    Serial.print("  gpio19: 0x");            Serial.println(*p19, HEX);
    // BATTERY FORENSICS: serial is dead on battery (the CH341A is USB-powered), so the
    // values of the PREVIOUS boot are persisted in kv and replayed here — deep-sleep/wake
    // on battery, then plug USB, reboot once, and the battery boot's numbers appear as
    // "[deep] prev-boot ...".
    uint8_t *pv = NULL; size_t pl = 0;
    if (tal_kv_get((const char *)"owf_dsdbg", &pv, &pl) == OPRT_OK) {
      if (pv && pl == 12) {
        uint32_t v[3]; memcpy(v, pv, 12);
        Serial.print("[deep] prev-boot r0: 0x"); Serial.print(v[0], HEX);
        Serial.print("  r7b: 0x");               Serial.print(v[1], HEX);
        Serial.print("  gpio19: 0x");            Serial.println(v[2], HEX);
      }
      if (pv) tal_kv_free(pv);
    }
    uint32_t cur[3] = { *r0, *r7b, *p19 };
    tal_kv_set((const char *)"owf_dsdbg", (uint8_t *)cur, sizeof(cur));
  }
  if (tal_kv_get((const char *)OWF_T5_DS_WAKE_KEY, &val, &len) == OPRT_OK) {
    s_owf_t5_ds_wake = true;
    if (val && len == sizeof(uint64_t)) memcpy(&s_owf_t5_ds_saved_nid, val, sizeof(uint64_t));
    if (val) tal_kv_free(val);
    val = NULL; len = 0;
    tal_kv_del((const char *)OWF_T5_DS_WAKE_KEY);
    Serial.println("[deep] this boot is a deep-sleep wake (PWR press)");
  }
  if (tal_kv_get((const char *)OWF_T5_DS_MARK_KEY, &val, &len) == OPRT_OK) {
    if (val) tal_kv_free(val);
    val = NULL; len = 0;
    tal_kv_del((const char *)OWF_T5_DS_MARK_KEY);
  }
  if (tal_kv_get((const char *)OWF_T5_DS_PEND_KEY, &val, &len) == OPRT_OK) {
    if (val) tal_kv_free(val);
    tal_kv_del((const char *)OWF_T5_DS_PEND_KEY);
    Serial.println("[deep] cleared stale sleep-pending flag from previous firmware");
  }
}

/* ---- Display sleep / wake (raw CO5300 register writes) --------------------
 * WHY NOT lv_vendor_stop()+tdl_disp_dev_close(): tdl_disp_dev_close() bottoms out in
 * __tdd_display_qspi_close() which is a STUB returning OPRT_NOT_SUPPORTED - it sends the
 * panel NOTHING. The AMOLED keeps emitting its last frame, so "sleep" left the screen lit.
 * And lv_vendor_stop() while the loop holds the disp lock can DEADLOCK on the LVGL task.
 *
 * Correct + simple: talk to the CO5300 directly. Its init seq ends 0x11 (sleep-out)+0x29
 * (display-on); to sleep we send the inverse 0x28 (display-off)+0x10 (sleep-in) and force
 * the brightness register 0x51 to 0x00 (the SDK's set_bl FLOORS brightness at 5, so it can
 * never blank an AMOLED - we bypass it). Wake reverses: 0x11, 0x29, 0x51=0xFF.
 *
 * tdd_disp_qspi_send_cmd(cfg,reg,data,len) only reads cfg->cmd_ramwr (0x02) and cfg->port,
 * so a tiny stack cfg is all we need; no access to the driver's static config required.
 * No LVGL task stop => no lock dance => no deadlock. The vendor task may still flush while
 * asleep, but display-off + brightness 0 means nothing emits. */
#define OWF_T5_DISPLAY_NAME      "display"
#define OWF_T5_QSPI_PORT_NUM     TUYA_QSPI_NUM_0   /* same port owf_tuya_register_panel uses */

static inline void owf_tuya_co5300_cmd(uint8_t reg, uint8_t *data, uint32_t len) {
  DISP_QSPI_BASE_CFG_T c;
  memset(&c, 0, sizeof(c));
  c.port      = OWF_T5_QSPI_PORT_NUM;   // which QSPI controller the panel is on
  c.cmd_ramwr = CO5300_WRITE_REG;       // 0x02 - the reg-write opcode the helper wraps each cmd in
  tdd_disp_qspi_send_cmd(&c, reg, data, len);
}

static inline void owf_tuya_display_sleep(void) {
#if OWF_T5_OWN_PANEL
  /* PARK THE BUS FIRST. These sleep/wake commands are DIRECT tkl_qspi writes from the loop
   * thread — they bypass the push task's queue, so fired against a frame DMA in flight
   * (CS forced low, DMA running) they corrupt the transfer, the TX-done IRQ never fires,
   * and the push task wedges forever in its tx_sem wait. That wedge is invisible while
   * asleep and detonates at wake: the relight commands (direct) still work, then the first
   * queued message (brightness / LVGL flush) fills the dead task's queue and blocks the
   * loop thread -> lit-but-frozen watch, RST-only. Parking waits out any in-flight frame
   * (CS released, tx_sem consumed) and holds the task off the bus for the whole suspend;
   * direct writes are safe on a parked bus. Unpark happens at the end of display_wake. */
  if (!owf_t5_panel_park_and_wait_idle())
    Serial.println("[susp] panel park TIMEOUT before sleep (bus may not be quiet!)");
#endif
  uint8_t zero = 0x00;
  owf_tuya_co5300_cmd(CO5300_BL, &zero, 1);   // 0x51 = 0 -> kill AMOLED pixel emission (true black)
  owf_tuya_co5300_cmd(0x28, NULL, 0);         // display OFF
  owf_tuya_co5300_cmd(0x10, NULL, 0);         // sleep IN (lowest panel power)
}
static inline void owf_tuya_display_wake(void) {
#if OWF_T5_OWN_PANEL
  /* FULL controller re-init FIRST, before ANY other QSPI access — the PM cycle can gate the
   * QSPI module clock outright, and then any register touch (a command, even irq_init) hangs
   * the AHB and wedges the core silently (the breadcrumb-less frozen wake). tkl_qspi_init
   * re-votes the module clock before touching registers, then the IRQ is re-armed. Bus is
   * parked and quiet here. */
  owf_t5_panel_reinit_after_sleep();
#endif
  uint8_t full = 0xFF;
  Serial.println("[wake] qspi irq re-armed; sleep-out");
  owf_tuya_co5300_cmd(0x11, NULL, 0);         // sleep OUT
  delay(120);                                 // MIPI spec: ~120ms after sleep-out before display-on
  owf_tuya_co5300_cmd(0x29, NULL, 0);         // display ON
  Serial.println("[wake] display on; restoring brightness");
  owf_tuya_co5300_cmd(CO5300_BL, &full, 1);   // 0x51 = 0xFF -> restore full brightness
  Serial.println("[wake] brightness cmd done");
#if OWF_T5_OWN_PANEL
  /* Release the park; queued messages (incl. the caller's settings_apply_brightness
   * right after us) drain from here on. */
  owf_t5_panel_unpark();
  Serial.println("[wake] panel task unparked");
#endif
}

#define OWF_T5_LCD_RST_PIN       TUYA_GPIO_NUM_29
#define OWF_T5_LCD_QSPI_PORT     TUYA_QSPI_NUM_0
#define OWF_T5_LCD_QSPI_CLK      (80 * 1000000)
#define OWF_T5_LCD_W             466
#define OWF_T5_LCD_H             466
#define OWF_T5_LCD_X_OFFSET      6
#define OWF_T5_LCD_Y_OFFSET      0
#define OWF_T5_TP_RST_PIN        TUYA_GPIO_NUM_42
#define OWF_T5_TP_I2C_PORT       TUYA_I2C_NUM_0
#define OWF_T5_TP_SCL_PIN        TUYA_GPIO_NUM_20
#define OWF_T5_TP_SDA_PIN        TUYA_GPIO_NUM_21

/* ---- Display orientation -------------------------------------------------------------
 * Rotation is owned by our CO5300 driver (tuya/owf_tuya_co5300_qspi.h), applied in HARDWARE
 * via MADCTL — NOT here. (The SDK's display_cfg.rotation below drives the SDK's *software*
 * rotation at the tdl layer, which our LVGL PARTIAL+zero-copy path never uses, so it's left
 * at ROTATION_0; the own driver is what actually flips the panel.) Configure the flip with
 * OWF_T5_OWN_PANEL / OWF_T5_PANEL_ROTATION in board_tuya_t5_amoled_175.h.
 *
 * Touch (CST92xx) stays on the SDK and must be flipped to MATCH the hardware rotation: a 180°
 * panel flip means inverting both touch axes vs the rotation-0 base (mirror_x=mirror_y=1). */

static inline void owf_tuya_register_panel(void) {
  DISP_QSPI_DEVICE_CFG_T display_cfg;
  memset(&display_cfg, 0, sizeof(display_cfg));
  display_cfg.bl.type   = TUYA_DISP_BL_TP_CUSTOM;   // backlight via CO5300 command
  display_cfg.width     = OWF_T5_LCD_W;
  display_cfg.height    = OWF_T5_LCD_H;
  display_cfg.x_offset  = OWF_T5_LCD_X_OFFSET;
  display_cfg.y_offset  = OWF_T5_LCD_Y_OFFSET;
  display_cfg.rotation  = TUYA_DISPLAY_ROTATION_0;
  display_cfg.port      = OWF_T5_LCD_QSPI_PORT;
  display_cfg.spi_clk   = OWF_T5_LCD_QSPI_CLK;
  display_cfg.rst_pin   = OWF_T5_LCD_RST_PIN;
  display_cfg.power.pin = TUYA_GPIO_NUM_MAX;        // no separate panel-power GPIO

  if (tdd_disp_qspi_co5300_register((char *)OWF_T5_DISPLAY_NAME, &display_cfg) != OPRT_OK)
    Serial.println("[tuya] CO5300 register FAILED");
  tdl_disp_custom_backlight_register((char *)OWF_T5_DISPLAY_NAME,
                                     tdd_qspi_co5300_send_cmd_set_bl, NULL);

  TDD_TP_CST92XX_INFO_T tp = {};
  tp.rst_pin       = OWF_T5_TP_RST_PIN;
  tp.i2c_cfg.port    = OWF_T5_TP_I2C_PORT;
  tp.i2c_cfg.scl_pin = OWF_T5_TP_SCL_PIN;
  tp.i2c_cfg.sda_pin = OWF_T5_TP_SDA_PIN;
  tp.tp_cfg.x_max  = OWF_T5_LCD_W;
  tp.tp_cfg.y_max  = OWF_T5_LCD_H;
  /* Match touch to the actual display orientation. Base (rotation 0) is mirror_x=mirror_y=1
   * (from the native board config). A hardware 180° flip (own driver) inverts both axes -> 0,0. */
#if OWF_T5_OWN_PANEL && (OWF_T5_PANEL_ROTATION == 180)
  tp.tp_cfg.flags.mirror_x = 0;
  tp.tp_cfg.flags.mirror_y = 0;
#else
  tp.tp_cfg.flags.mirror_x = 1;
  tp.tp_cfg.flags.mirror_y = 1;
#endif
  tp.tp_cfg.flags.swap_xy  = 0;
  if (tdd_tp_i2c_cst92xx_register((char *)OWF_T5_DISPLAY_NAME, &tp) != OPRT_OK)
    Serial.println("[tuya] CST92xx touch register FAILED");
}

/* NOTE: LVGL bring-up (lv_init + display + indev + the loop's lv_timer_handler) now lives
 * in tuya/owf_tuya_lvgl_own.h (our own LVGL v9.5). owf_tuya_register_panel() above is called
 * from there/setup() first; there is no vendor task and no display lock any more. */

/* Backlight / panel brightness, 0..100 (the .ino converts from its 0..255 scale). */
static inline void owf_tuya_set_backlight(int pct) {
  if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
#if OWF_T5_OWN_PANEL
  // OUR driver: 0x51 brightness, serialized through the qspi task (no command-race), 0..100 ->
  // 0..255 for the panel's 8-bit DBV register. No floor-at-5, so 0 is a true blank.
  owf_t5_panel_set_brightness((uint8_t)((pct * 255) / 100));
#else
  // SDK path: tdl backlight cb (tdd_qspi_co5300_send_cmd_set_bl) — re-expands 0..100 -> 0..255
  // and floors at 5. Handle found by name once and cached.
  static TDL_DISP_HANDLE_T s_disp = NULL;
  if (!s_disp) s_disp = tdl_disp_find_dev((char *)OWF_T5_DISPLAY_NAME);
  if (s_disp) tdl_disp_set_brightness(s_disp, (uint8_t)pct);
#endif
}

#endif /* BOARD_PLATFORM_TUYA */
#endif /* OWF_TUYA_PORT_H */
