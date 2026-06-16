/* ============================================================================
 *  board_sleep.h — deep-sleep / wake-source abstraction (board-neutral API).
 *
 *  BOTH boards use real DEEP sleep: CPU off, RAM lost, every wake is a full
 *  reboot (setup() re-runs). What differs is HOW a user brings it back:
 *
 *    - S3-2.06 (BOARD_WAKE_USE_EXT0=1): GPIO0 (BOOT) is an RTC-IO, so EXT0 wakes
 *      it on a press (cause ESP_SLEEP_WAKEUP_EXT0). The press pull is configured
 *      via rtc_gpio_* and HELD across sleep. The RTC timer also wakes it for
 *      background notification checks.
 *
 *    - C6-1.47 (BOARD_WAKE_USE_EXT0=0): the silicon only allows GPIO0..7 as a
 *      deep-sleep wake pin (SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK = BIT0..7),
 *      and both the touch INT (GPIO21) and BOOT button (GPIO9) are OUTSIDE it —
 *      so no GPIO wake can be armed (trying GPIO9 throws "invalid deep sleep
 *      wakeup IO"). Instead the user brings it back with the hardware RST button:
 *      RST pulls the chip's reset line, which from deep sleep is a clean cold
 *      boot — exactly what a normal wake already is here. So on the C6 we arm NO
 *      button wake (RST handles it in hardware); only the RTC timer is armed.
 *
 *  API:
 *    board_wake_release_button()  — boot: undo the deep-sleep pull hold (EXT0 only).
 *    board_wake_arm_button()      — pre-sleep: arm the button wake (EXT0 only; the
 *                                   C6 relies on the hardware RST button instead).
 *    board_enter_sleep()          — DO the deep sleep. Never returns (cold boot
 *                                   on wake). Returns bool for API symmetry.
 *    board_woke_from_button()     — true if THIS boot was a button/EXT0 wake.
 *    board_woke_from_timer()      — true if THIS boot was an RTC-timer wake.
 *
 *  The timer-wake arming (esp_sleep_enable_timer_wakeup) is board-neutral and
 *  stays in sleep_power.h.
 * ========================================================================== */
#pragma once
#include <esp_sleep.h>
#include <driver/gpio.h>
#if BOARD_WAKE_USE_EXT0
#include <driver/rtc_io.h>
#endif

/* Pre-sleep peripheral isolation. C6-1.47 ONLY (no PMU to cut rails): in deep
 * sleep the SoC releases its GPIOs to their default (floating) state unless they
 * are explicitly held. On this board the PWM backlight enable (LCD_BL) and the
 * LCD/touch RESET lines then FLOAT — a floating backlight-enable lets the
 * backlight driver sit partially ON, drawing milliamps the whole time the watch
 * is "off" (the observed 10-15%/period drain). Drive them to their OFF/quiescent
 * level and LATCH it with gpio_hold so the level survives deep sleep, the same
 * trick haptics_prepare_sleep()/audio use for their pins.
 *
 * The whole body is gated on the C6 board macro, so it can NEVER affect the
 * S3-2.06 (which cuts its rails through the AXP2101 PMU instead). Since the watch
 * is revived only by the hardware RST button (a cold boot that re-inits the
 * panel), holding the controllers in reset while asleep is correct. */
static inline void board_isolate_peripherals_for_sleep(void) {
#if defined(LCD_BL) && BOARD_HAS_BACKLIGHT_PWM && !BOARD_HAS_PMU_AXP2101
  // Backlight OFF: active-high enable -> drive LOW, then latch through sleep.
  gpio_hold_dis((gpio_num_t)LCD_BL);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);
  gpio_hold_en((gpio_num_t)LCD_BL);

  // Hold the LCD + touch controllers in reset (active-LOW) so they sit quiescent
  // instead of floating and self-powering off their reset/data lines.
#ifdef LCD_RESET
  gpio_hold_dis((gpio_num_t)LCD_RESET);
  pinMode(LCD_RESET, OUTPUT);
  digitalWrite(LCD_RESET, LOW);
  gpio_hold_en((gpio_num_t)LCD_RESET);
#endif
#ifdef TP_RESET
  gpio_hold_dis((gpio_num_t)TP_RESET);
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  gpio_hold_en((gpio_num_t)TP_RESET);
#endif

  // Keep the per-pin holds active through deep sleep. On SoCs that can hold a
  // SINGLE IO in deep sleep (e.g. the C6), each gpio_hold_en() above already
  // persists on its own and the global enable is COMPILED OUT of the driver
  // (declared only #if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP), so gate the
  // call on that macro to keep the build working on both families.
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();   // older SoCs: one global switch arms all holds
#endif
#endif
}

/* Boot counterpart: release the holds placed above so the panel/touch init can
 * drive these pins again. A C6 RST press is a full chip reset that clears holds
 * on its own, but an RTC-TIMER deep-sleep wake is NOT a reset — the holds survive
 * it and would keep the backlight/controllers pinned OFF, blocking gfx->begin().
 * So release them unconditionally early in setup(). C6-gated; no-op on the S3. */
static inline void board_release_sleep_isolation(void) {
#if defined(LCD_BL) && BOARD_HAS_BACKLIGHT_PWM && !BOARD_HAS_PMU_AXP2101
  gpio_hold_dis((gpio_num_t)LCD_BL);
#ifdef LCD_RESET
  gpio_hold_dis((gpio_num_t)LCD_RESET);
#endif
#ifdef TP_RESET
  gpio_hold_dis((gpio_num_t)TP_RESET);
#endif
#endif
}

/* Boot: release the pull-hold placed on BOOT_BTN_GPIO before sleeping (EXT0
 * boards), so it reads as a normal input again. No-op where no hold was set. */
static void board_wake_release_button(void) {
#if BOARD_WAKE_USE_EXT0
  rtc_gpio_hold_dis((gpio_num_t)BOOT_BTN_GPIO);
  rtc_gpio_deinit((gpio_num_t)BOOT_BTN_GPIO);
#endif
}

/* Pre-sleep: arm a BOOT press (active-LOW) as the deep-sleep wake source. Only
 * EXT0 boards (S3) can do this — the C6's button isn't on a wake-capable pin, so
 * it relies on the hardware RST button (a reset = cold boot from deep sleep). */
static void board_wake_arm_button(void) {
#if BOARD_WAKE_USE_EXT0
  rtc_gpio_init((gpio_num_t)BOOT_BTN_GPIO);
  rtc_gpio_set_direction((gpio_num_t)BOOT_BTN_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)BOOT_BTN_GPIO);              // idle HIGH
  rtc_gpio_pulldown_dis((gpio_num_t)BOOT_BTN_GPIO);
  rtc_gpio_hold_en((gpio_num_t)BOOT_BTN_GPIO);                // keep pull in sleep
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BTN_GPIO, 0); // wake on LOW (press)
#endif
  // C6: no GPIO wake armed — the hardware RST button cold-boots it from sleep.
}

/* Was THIS boot woken by a button press? On EXT0 boards that's the EXT0 cause.
 * On the C6 the RST button produces a normal cold-boot reset (NOT a sleep wake
 * cause), so there is no "button wake" cause to report — return false and let the
 * normal cold-boot path run (which is what an RST press is). */
static bool board_woke_from_button(void) {
#if BOARD_WAKE_USE_EXT0
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
#else
  return false;
#endif
}

/* Was THIS boot woken by the RTC timer (the scheduled background-check tick)? */
static bool board_woke_from_timer(void) {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

/* Enter deep sleep. Never returns — the chip powers down and the next wake (EXT0
 * press, RTC timer, or an RST-button cold boot on the C6) re-runs setup(). The
 * bool return is for API symmetry / future light-sleep boards; here it's
 * unreachable. */
static bool board_enter_sleep(void) {
  board_isolate_peripherals_for_sleep();  // C6: latch backlight/reset lines OFF (no-op on S3)
  esp_deep_sleep_start();            // does not return
  return false;                      // unreachable
}
