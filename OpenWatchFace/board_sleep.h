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

#ifndef SLEEP_LOG   /* sleep_power.h may not have been included yet */
#  if defined(OWF_SLEEP_VERBOSE)
#    define SLEEP_LOG(...) USBSerial.printf(__VA_ARGS__)
#  else
#    define SLEEP_LOG(...) do {} while (0)
#  endif
#endif
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/esp_sleep.h"     // maps deep sleep onto tkl_cpu_sleep_mode_set + tkl_wakeup
#include "tuya/compat/driver/gpio.h"   // gpio_hold_* no-ops (T5 retains pads / wake via tkl_wakeup)
#if BOARD_WAKE_USE_EXT0
#include "tuya/compat/driver/rtc_io.h"
#endif
#else
#include <esp_sleep.h>
#include <driver/gpio.h>
#if BOARD_WAKE_USE_EXT0
#include <driver/rtc_io.h>
#endif
#endif

/* ---- Soft power latch (BOARD_PWR_LATCH_GPIO) -------------------------------
 * Boards whose battery rail flows through a P-FET that only the PWR key (held)
 * or this GPIO keeps enabled (S3-Touch-LCD-1.69: SYS_EN). Three duties:
 *   - board_power_latch_on(): assert the keep-alive. Called FIRST THING in
 *     setup() — before it, battery power lasts only as long as the key is held
 *     (on USB, VBUS hides the issue). Also releases a surviving sleep hold, so
 *     a deep-sleep wake (not a reset — holds persist) can re-own the pin.
 *   - board_isolate_peripherals_for_sleep() below HOLDS the latch HIGH through
 *     deep sleep — without the hold the pad floats when the CPU sleeps and the
 *     supply collapses mid-sleep.
 *   - board_power_latch_off(): TRUE power-off — drop the latch and hold it LOW.
 *     On battery the supply collapses (never returns); on USB, VBUS feeds the
 *     LDO around the latch, so the caller falls through to its deep-sleep path.
 *     The dropped flag stops the isolation block from re-asserting the latch on
 *     that fall-through (board_enter_sleep runs right after). */
#ifdef BOARD_PWR_LATCH_GPIO
static bool s_pwr_latch_dropped = false;   // set by board_power_latch_off()
static inline void board_power_latch_on(void) {
  gpio_hold_dis((gpio_num_t)BOARD_PWR_LATCH_GPIO);
  pinMode(BOARD_PWR_LATCH_GPIO, OUTPUT);
  digitalWrite(BOARD_PWR_LATCH_GPIO, HIGH);
  s_pwr_latch_dropped = false;
}
static inline void board_power_latch_off(void) {
  gpio_hold_dis((gpio_num_t)BOARD_PWR_LATCH_GPIO);
  pinMode(BOARD_PWR_LATCH_GPIO, OUTPUT);
  digitalWrite(BOARD_PWR_LATCH_GPIO, LOW);
  gpio_hold_en((gpio_num_t)BOARD_PWR_LATCH_GPIO);   // keep LOW if USB carries us into deep sleep
  s_pwr_latch_dropped = true;
}
#endif

/* Pre-sleep peripheral isolation. Two per-board blocks:
 *
 * FT3168 + AXP2101 boards: put the touch controller into its lowest RECOVERABLE
 * power mode (+ latch TP_RESET where one exists) — it sits on the never-cut
 * ALDO1 rail, so the PMU rail-cut can't reach it. Which mode depends on whether
 * the board has a reset line to wake it with; see the block comment inside.
 *
 * C6-1.47 (no PMU to cut rails): in deep
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
#if BOARD_DISPLAY_EPD_GDEQ031T10
  /* T-Deck Pro: latch every peripheral enable LOW through deep sleep. Digital
   * (non-RTC) pads float once the CPU sleeps; a floating modem-boost EN drifts
   * high and powers the A7682E mid-sleep — red STATUS LED on, battery drain on
   * a "sleeping" watch. board_tdeck_rails_off() at boot releases these holds. */
  board_tdeck_rails_hold_for_sleep();
#endif
#if BOARD_TOUCH_FT3168 && BOARD_HAS_PMU_AXP2101
  /* S3-2.06: the FT3168 lives on ALDO1 — the ONE rail that can never be cut in
   * sleep (it also feeds the RTC + I2C pull-ups) — so unlike every other
   * peripheral it is NOT powered down by rails_cut_for_sleep(). Left alone it
   * keeps scanning in MONITOR mode all night (mA-scale: the S3-2.06 sleep-drain
   * offender). Two-step shutdown:
   *   1) a low-power mode command (reg 0xA5) — WHICH one depends on the board's
   *      ability to wake the chip again; see the mode selection immediately below.
   *   2) drive TP_RESET LOW and LATCH it through deep sleep, so the controller
   *      is pinned in reset instead of floating (a floating reset into a powered
   *      chip leaks and can even restart its scan engine) — only where such a
   *      line exists. */
  /* WHICH low-power mode depends on whether we can WAKE the chip afterwards.
   *
   *   HIBERNATE (0x03) is the deepest (~uA) but it is a ONE-WAY door: the FT3x68
   *   stops clocking its I2C slave, so it does NOT answer further transactions.
   *   The ONLY exit is a hardware reset PULSE on TP_RESET.
   *
   *   MONITOR (0x01) is the gesture/low-power scan mode: much lower than ACTIVE,
   *   and critically the chip STAYS ADDRESSABLE, so it comes back on its own /
   *   on the next I2C access with no reset line needed.
   *
   * So a board with TP_RESET hibernates (2.06 — board_release_sleep_isolation()
   * pulses it on wake), and a board WITHOUT one (S3-1.8) must NOT hibernate: it
   * would sleep fine but wake with a permanently dead touch panel (display OK,
   * touch gone — observed). It uses MONITOR instead and accepts the higher idle
   * draw, because there is no way to revive a hibernated chip without the pin. */
#ifdef TP_RESET
  const uint8_t ft_pmode = 0x03;          // hibernate; woken by the reset pulse
  const char   *ft_pname = "hibernate";
#else
  const uint8_t ft_pmode = 0x01;          // monitor; self-recoverable over I2C
  const char   *ft_pname = "monitor (no TP_RESET to wake a hibernate)";
#endif
  /* VERIFIED write: the ACK was previously discarded — a silent NAK meant the
   * controller kept scanning in ACTIVE mode all night (mA on ALDO1). Retry a few
   * times and LOG the outcome so a failed shutdown is visible on the wire. */
  uint8_t ft_rc = 255;
  for (int t = 0; t < 3; t++) {
    Wire.beginTransmission(FT3168_DEVICE_ADDRESS);
    Wire.write(0xA5);               // PMODE register
    Wire.write(ft_pmode);
    ft_rc = Wire.endTransmission();
    if (ft_rc == 0) break;
    delay(5);
  }
  USBSerial.printf("[sleep] FT3168 %s %s (i2c rc=%u)\n",
                   ft_pname, ft_rc == 0 ? "ACKed" : "FAILED", (unsigned)ft_rc);
  delay(2);
  /* Hold TP_RESET HIGH (INACTIVE) through sleep — NOT low. Reset is active-LOW and
   * OVERRIDES hibernate: a chip with reset asserted isn't hibernating, it's held in
   * its reset state (POR + internal regulator active, current unspecified — the
   * suspected residual ALDO1 drain). Held HIGH the line can't float AND the chip
   * stays in the ~uA hibernate it just entered. Wake-up needs a real LOW->HIGH
   * reset PULSE now — board_release_sleep_isolation() provides it.
   *
   * #ifdef TP_RESET: the S3-1.8 has the same FT3168 on the same never-cut rail but
   * breaks out NO touch reset line — so there is nothing to latch here, and (see
   * the mode selection above) it was put into MONITOR rather than HIBERNATE
   * precisely because it has no way to be pulsed back to life. */
#ifdef TP_RESET
  gpio_hold_dis((gpio_num_t)TP_RESET);
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, HIGH);
  gpio_hold_en((gpio_num_t)TP_RESET);
#endif
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();        // S3: arm the global switch so the hold survives deep sleep
#endif
#endif
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

#ifdef BOARD_PWR_LATCH_GPIO
  /* Keep the battery keep-alive (SYS_EN) asserted through deep sleep — a
   * released pad floats and the latch NPN turns off, collapsing the supply
   * mid-sleep on battery. Skipped when a power-off just dropped the latch on
   * purpose (its own LOW hold is already in place). Non-RTC pad, so it rides
   * the global deep-sleep hold switch. */
  if (!s_pwr_latch_dropped) {
    gpio_hold_dis((gpio_num_t)BOARD_PWR_LATCH_GPIO);
    pinMode(BOARD_PWR_LATCH_GPIO, OUTPUT);
    digitalWrite(BOARD_PWR_LATCH_GPIO, HIGH);
    gpio_hold_en((gpio_num_t)BOARD_PWR_LATCH_GPIO);
  }
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();
#endif
#endif

#if BOARD_HAS_CAMERA && defined(CAM_PIN_PWDN) && (CAM_PIN_PWDN >= 0)
  /* Camera boards (S3-Touch-LCD-2): the OV5640 module hangs off the always-on
   * 3V3 rail — there is no PMU and no load switch, so the ONLY off-switch it
   * has is its PWDN pin (active HIGH = hardware power-down, ~µA). The esp_camera
   * driver only manages that pin while it is initialised; outside the Camera
   * app the GPIO floats, and a floating PWDN leaves the sensor's regulators
   * energised — measured as the S3-LCD-2's fast sleep/off drain. Drive it HIGH
   * and LATCH it through deep sleep, every sleep, camera used this boot or not. */
  gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, HIGH);
  gpio_hold_en((gpio_num_t)CAM_PIN_PWDN);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
  gpio_deep_sleep_hold_en();
#endif
#endif
}

/* Boot counterpart: release the holds placed above so the panel/touch init can
 * drive these pins again. A C6 RST press is a full chip reset that clears holds
 * on its own, but an RTC-TIMER deep-sleep wake is NOT a reset — the holds survive
 * it and would keep the backlight/controllers pinned OFF, blocking gfx->begin().
 * So release them unconditionally early in setup(). On the S3-2.06 this also
 * releases the TP_RESET hold and gives the hibernated FT3168 its wake-up reset
 * edge (LOW-held all sleep -> driven HIGH here). */
static inline void board_release_sleep_isolation(void) {
#if BOARD_TOUCH_FT3168 && BOARD_HAS_PMU_AXP2101
  /* Undo the pre-sleep touch shutdown. An EXT0/timer deep-sleep wake is NOT a
   * reset, so the TP_RESET hold survives it — release it, then give the FT3168 a
   * real LOW->HIGH reset PULSE. (The line was held HIGH through sleep so the chip
   * stayed in hibernate — see isolate above — so the wake edge must be generated
   * here: assert LOW ~5 ms, release HIGH.) The chip needs ~200 ms to boot, which
   * the display init between here and the touch begin() more than covers; if
   * begin() still misses, its existing retry path pulses TP_RESET again.
   * Skipped where no reset line is broken out (S3-1.8) — nothing was held there,
   * so there is nothing to release and no edge to generate. */
#ifdef TP_RESET
  gpio_hold_dis((gpio_num_t)TP_RESET);
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(5);
  digitalWrite(TP_RESET, HIGH);
#endif
#endif
#if defined(LCD_BL) && BOARD_HAS_BACKLIGHT_PWM && !BOARD_HAS_PMU_AXP2101
  gpio_hold_dis((gpio_num_t)LCD_BL);
#ifdef LCD_RESET
  gpio_hold_dis((gpio_num_t)LCD_RESET);
#endif
#ifdef TP_RESET
  gpio_hold_dis((gpio_num_t)TP_RESET);
#endif
#endif
#if BOARD_HAS_CAMERA && defined(CAM_PIN_PWDN) && (CAM_PIN_PWDN >= 0)
  /* Release the sleep hold but KEEP DRIVING PWDN HIGH: the sensor stays in
   * hardware power-down the whole time the watch runs — this is also what
   * parks it on a plain cold boot, where nothing else touches the pin. The
   * Camera app's esp_camera_init() takes the pin over (and powers the sensor
   * up) when — and only when — the app actually opens. */
  gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, HIGH);
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
  /* WAIT FOR THE BUTTON TO BE RELEASED before arming. The double-tap-to-sleep
   * gesture triggers on the PRESS edge of the second tap, so on a fast sleep
   * path (T-Deck Pro: no PMU rails, no radios to tear down) we reach
   * esp_deep_sleep_start() with BOOT still held LOW — and EXT0 is a LEVEL wake,
   * so the sleep "completes" and wakes in the same instant, which the user sees
   * as sleep = instant full reboot. Bounded so a stuck/shorted pin can't hang
   * the sleep path forever; the trailing delay outlasts release bounce. */
  {
    uint32_t t0 = millis();
    while (digitalRead(BOOT_BTN_GPIO) == LOW && (uint32_t)(millis() - t0) < 3000) delay(10);
    delay(50);
  }
  rtc_gpio_init((gpio_num_t)BOOT_BTN_GPIO);
  rtc_gpio_set_direction((gpio_num_t)BOOT_BTN_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)BOOT_BTN_GPIO);              // idle HIGH
  rtc_gpio_pulldown_dis((gpio_num_t)BOOT_BTN_GPIO);
  rtc_gpio_hold_en((gpio_num_t)BOOT_BTN_GPIO);                // keep pull in sleep
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BTN_GPIO, 0); // wake on LOW (press)
#elif defined(BOARD_WAKE_GPIO)
  // C6 HARDWARE MOD: the BOOT button is wired in parallel to BOARD_WAKE_GPIO (GPIO7), which
  // IS in the deep-sleep wake mask (GPIO0..7). Arm a GPIO deep-sleep wake on it, wake-on-LOW
  // (BOOT is active-LOW). A press now wakes the watch from deep sleep WITHOUT a reset, so RTC
  // memory (the step counter) survives — same as the S3's EXT0 path.
  gpio_pullup_en((gpio_num_t)BOARD_WAKE_GPIO);                // idle HIGH
  gpio_pulldown_dis((gpio_num_t)BOARD_WAKE_GPIO);
  esp_err_t e = esp_deep_sleep_enable_gpio_wakeup(1ULL << BOARD_WAKE_GPIO,
                                                  ESP_GPIO_WAKEUP_GPIO_LOW);
  SLEEP_LOG("[wake] C6 GPIO%d deep-sleep wake arm -> %s\n", BOARD_WAKE_GPIO,
                   e == ESP_OK ? "OK" : esp_err_to_name(e));
#endif
  // (No-op on a C6 without BOARD_WAKE_GPIO: relies on the hardware RST button.)
}

/* Was THIS boot woken by a button press? On EXT0 boards that's the EXT0 cause.
 * On the C6 the RST button produces a normal cold-boot reset (NOT a sleep wake
 * cause), so there is no "button wake" cause to report — return false and let the
 * normal cold-boot path run (which is what an RST press is). */
static bool board_woke_from_button(void) {
#if BOARD_PLATFORM_TUYA
  // T5: the deep-sleep wake is a plain reboot (no hardware wake cause survives it); the
  // entry leaves a kv marker instead, read by owf_tuya_deep_sleep_boot_check() in setup().
  // Only a PWR press can wake that sleep, so marker == button wake.
  return owf_tuya_woke_from_deep_sleep();
#elif BOARD_WAKE_USE_EXT0
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
#elif defined(BOARD_WAKE_GPIO)
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;   // C6 GPIO7 hardware-mod wake
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
