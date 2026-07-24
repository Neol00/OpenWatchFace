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
}

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

/* ---- SUSPEND sleep (resume-style; the shipping sleep mode) -----------------------------
 * ROOT CAUSE of the battery deaths (round 14, proven by the user's cable experiment): mode-2
 * deep sleep SURVIVES on battery — but its wake is a reboot through the boot ROM, which
 * releases GPIO19 before setup() can re-latch. Q3's gate rises through R24 (10k) in
 * microseconds and the battery rail dies AT THE WAKE PRESS. No firmware can fix a window in
 * which firmware isn't running; the hardware fix is a cap on Q3's gate node (~100uF -> ~1.4s
 * hold), not available right now. owf_tuya_deep_sleep_try above is KEPT for the day that cap
 * is fitted — do not call it for battery sleep until then.
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

  Serial.print("[susp] suspend: pm low-voltage (mode 1); waiting for User/PWR press");
  if (wake_sec > 0) { Serial.print(" or timed wake in (s): "); Serial.print(wake_sec); }
  Serial.println();
  Serial.flush();
  // Deadline off the AON-RTC-backed wall clock (time() == bk_rtc_gettimeofday), NOT millis():
  // the FreeRTOS tick may be suppressed/compensated across low-voltage spans.
  time_t deadline = (wake_sec > 0) ? time(NULL) + (time_t)wake_sec : (time_t)0;
  bk_pm_ap_sleep_mode_set(1);              // PM_MODE_LOW_VOLTAGE vote; engages from idle
  for (;;) {
    delay(100);                            // block the loop task -> idle -> PM may drop low-voltage
    if (digitalRead(12) == LOW || digitalRead(18) == LOW) break;   // press on either button = wake
    if (deadline && time(NULL) >= deadline) {
      Serial.println("[susp] timed deadline reached -> resuming (dark)");
      return false;                        // timed wake; caller handles housekeeping
    }
  }
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
  uint8_t zero = 0x00;
  owf_tuya_co5300_cmd(CO5300_BL, &zero, 1);   // 0x51 = 0 -> kill AMOLED pixel emission (true black)
  owf_tuya_co5300_cmd(0x28, NULL, 0);         // display OFF
  owf_tuya_co5300_cmd(0x10, NULL, 0);         // sleep IN (lowest panel power)
}
static inline void owf_tuya_display_wake(void) {
  uint8_t full = 0xFF;
  owf_tuya_co5300_cmd(0x11, NULL, 0);         // sleep OUT
  delay(120);                                 // MIPI spec: ~120ms after sleep-out before display-on
  owf_tuya_co5300_cmd(0x29, NULL, 0);         // display ON
  owf_tuya_co5300_cmd(CO5300_BL, &full, 1);   // 0x51 = 0xFF -> restore full brightness
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
