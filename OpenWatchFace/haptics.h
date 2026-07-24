/* ============================================================================
 *  haptics.h — vibration motor driver (non-blocking).
 *
 *  The board's vibration motor sits on GPIO18, switched by an MMBT3904 NPN
 *  transistor (schematic ref Q1) — so it's a plain active-HIGH on/off motor, not
 *  an I2C haptic chip. We drive it with digitalWrite and shape buzzes purely in
 *  software.
 *
 *  Everything is non-blocking: start a one-shot pulse or a repeating dot/dash
 *  pattern, then call haptics_tick(millis()) every loop to advance it. Nothing
 *  here ever calls delay(), so the UI and buttons stay responsive while it buzzes.
 *
 *  Pattern strings use '.' = short pulse (dot) and '-' = long pulse (dash); any
 *  other char is a beat of silence. The alarm uses HAPTICS_HEARTBEAT.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "driver/gpio.h"             // gpio_hold_* (keep the motor OFF through deep sleep)

/* HAPTICS_MOTOR_GPIO / HAPTICS_ACTIVE_HIGH come from the board header. */

/* ---- vibration INTENSITY (not yet implemented) -----------------------------
 * The motor currently runs FULL strength (plain on/off) — too aggressive for a small
 * coin ERM, but the softening approach is still TBD. PWM is the right idea, but on the
 * Tuya T5 the motor is on IO14, which is NOT a hardware-PWM pin (T5 PWM is only on
 * 18/24/32/34/36/19/8/9/25/33/35/37, so analogWrite() no-ops), and the first attempt
 * (software PWM from a tkl_timer ISR) BROKE haptics entirely — the prebuilt tkl_timer
 * period/min-interval semantics aren't knowable from source and the timer never fired,
 * leaving IO14 stuck LOW. Options to revisit, verified on real hardware first:
 *   (a) confirm tkl_timer actually fires at a usable rate, then re-add the ISR PWM;
 *   (b) move the motor signal to a real PWM pin and use analogWrite (0-100 duty);
 *   (c) hardware: lower the motor's supply / add series R so it spins weaker.
 * HAPTICS_INTENSITY_PCT is kept as the future knob but is currently UNUSED. */
#ifndef HAPTICS_INTENSITY_PCT
#define HAPTICS_INTENSITY_PCT 100   // reserved; not wired up yet (full strength for now)
#endif

#if !BOARD_HAS_HAPTICS
/* No motor on this board — same API, all no-ops, so call sites don't change. */
static void haptics_init(void) {}
static void haptics_prepare_sleep(void) {}
static void haptics_stop(void) {}
static void haptics_pulse(uint16_t) {}
static void haptics_play(const char *, bool) {}
static inline bool haptics_active(void) { return false; }
static void haptics_tick(uint32_t) {}
#define HAPTICS_HEARTBEAT ""
/* Also defined here (not just in the has-haptics branch) because app headers reference
 * them UNCONDITIONALLY — HAPTICS_CLICK_MS in app_timer.h's steppers, HAPTICS_NO_BUZZ_FLAG
 * in app_menu.h's tile. Harmless on a no-motor board: haptics_pulse() is a no-op above, and
 * the flag is just an LVGL user-flag that nothing reads. Keeps those files board-agnostic. */
#ifndef HAPTICS_CLICK_MS
#define HAPTICS_CLICK_MS 6
#endif
#define HAPTICS_NO_BUZZ_FLAG  LV_OBJ_FLAG_USER_1
#else

/* Pulse/gap shape (ms) for the alarm/timer pattern. Kept SHORT: the motor is full
 * on/off (amplitude PWM isn't working yet — see the INTENSITY note above), so the only
 * way to soften a buzz is to make each pulse brief — a coin ERM barely spins up in
 * ~45-100 ms, so it feels like a gentle tap rather than a hard sustained hit. Combined
 * with the pattern + long loop pause below, this gives a calm "lub-DUB ... rest" heartbeat
 * instead of the old fast drum roll. Raise DOT/DASH for a firmer buzz, lower for fainter. */
#define H_DOT_MS         45          // "lub" — the soft first beat
#define H_DASH_MS       100          // "DUB" — the firmer second beat
#define H_GAP_MS         45          // gap between the two beats of one heartbeat
#define H_LOOP_PAUSE_MS 800          // the long REST between heartbeats (the big gap you want)

/* Length of the global per-click UI tick (the buzz every CONTROL gives via the
 * indev-level hook in the .ino) — now the SINGLE source of UI haptic feedback (the
 * scattered per-call haptics_pulse() values were all removed in favour of this). Set
 * as LOW as still registers: a coin ERM has spin-up inertia, so below ~5-6 ms it may
 * not move at all. 6 is the practical floor; drop to 5/4 to test if you still feel it,
 * raise if it's too faint. ONE number tunes every app's buzz now. */
#ifndef HAPTICS_CLICK_MS
#define HAPTICS_CLICK_MS 6
#endif

/* The global hook buzzes on switches/buttons/checkables but NOT on app-launcher tiles
 * (which are lv_btn too). Tag those exceptions with this flag so the hook skips them —
 * see menu_build_tile(). Uses an LVGL user flag (free for app use). */
#define HAPTICS_NO_BUZZ_FLAG  LV_OBJ_FLAG_USER_1

/* The alarm/timer rhythm: a real heartbeat — "lub-DUB", then a long rest, repeating.
 * ".-" = dot(lub) then dash(DUB); H_GAP_MS separates them, then H_LOOP_PAUSE_MS gives the
 * ~800 ms silence before the next heartbeat. (Was "-.-.---.-.---.-.-" — 17 symbols at 70 ms
 * gaps = a continuous fast drum roll with no space; this is calm and clearly a heartbeat.) */
#define HAPTICS_HEARTBEAT ".-"

static const char *h_pat      = nullptr;  // active pattern (null = none)
static bool        h_loop     = false;
static uint16_t    h_idx      = 0;
static bool        h_phase_on = false;    // currently mid-ON of a symbol?
static uint32_t    h_deadline = 0;        // ms when the current phase ends

/* Drive the pin to its electrical OFF/ON level (honoring ACTIVE_HIGH). The raw
 * level write — the PWM layer below calls this from the timer ISR. */
static inline void h_motor_raw(bool on) {
  digitalWrite(HAPTICS_MOTOR_GPIO,
               (HAPTICS_ACTIVE_HIGH ? on : !on) ? HIGH : LOW);
}

/* Full-strength on/off level write. (Amplitude PWM isn't available — IO14 has no HW PWM
 * and the periodic-ISR software-PWM attempt broke haptics; strength is pulse-LENGTH only.) */
static inline void h_motor(bool on) { h_motor_raw(on); }

static void haptics_init(void) {
  gpio_hold_dis((gpio_num_t)HAPTICS_MOTOR_GPIO);   // release any hold from before sleep
  pinMode(HAPTICS_MOTOR_GPIO, OUTPUT);
  h_motor(false);
}

/* Pin the motor LOW and latch it so it can't float (and phantom-buzz / drain the
 * battery) during deep sleep. Call from the sleep path right before sleeping. */
static void haptics_prepare_sleep(void) {
  h_motor(false);
  gpio_hold_en((gpio_num_t)HAPTICS_MOTOR_GPIO);
  gpio_deep_sleep_hold_en();
}

/* Stop everything immediately (motor off). */
static void haptics_stop(void) {
  h_pat = nullptr; h_loop = false; h_phase_on = false;
  h_motor(false);
}

/* Minimum gap between UI ticks. Rapid-fire controls (slider drags, sweeping across color
 * swatches, a held stepper) call this in a STREAM; without a floor every event would fire
 * its own blocking buzz and they'd merge into a continuous vibration (and stack up delay).
 * Swallow a new pulse if it comes within this window of the last, so a fast sweep gives one
 * tick, not a sustained buzz. */
#ifndef HAPTICS_PULSE_MIN_GAP_MS
#define HAPTICS_PULSE_MIN_GAP_MS 60
#endif
static uint32_t h_pulse_last = 0;     // millis() of the last pulse start (0 = none yet)

/* One-shot buzz of `ms` — a short tick of UI feedback. SELF-CONTAINED + BLOCKING: it turns
 * the motor on, waits `ms`, and turns it off, all before returning. This is deliberate: the
 * earlier "turn on now, turn off later from the loop/a timer" design made the buzz last as
 * long as the button stayed pressed / the screen redrew (the loop — and even a HW timer —
 * couldn't reliably cut the motor while a heavy click callback blocked the core), so every
 * button buzzed for a different, far-too-long duration. Doing the whole pulse inline makes it
 * EXACTLY `ms` regardless of any redraw that happens after the callback returns. `ms` is tiny
 * (6-14 ms) so blocking the click handler that long is imperceptible. Debounced so a stream of
 * events doesn't chain many blocking pulses. Skipped while an alarm PATTERN is playing (that
 * owns the motor via the non-blocking tick engine). */
static void haptics_pulse(uint16_t ms) {
  if (h_pat) return;                // an alarm/timer pattern owns the motor — don't interrupt it
  uint32_t now = millis();
  if (h_pulse_last != 0 && (uint32_t)(now - h_pulse_last) < HAPTICS_PULSE_MIN_GAP_MS) return;
  h_pulse_last = now;
  h_motor(true);
  delay(ms);                        // brief + yields (Arduino delay); the WHOLE pulse is here
  h_motor(false);                   // motor is OFF before we return -> any later redraw can't
                                    // lengthen it. Exactly `ms`, every button, every board.
}

/* Start a dot/dash pattern. loop=true repeats it (with a pause) until stopped. */
static void haptics_play(const char *pattern, bool loop) {
  h_pat = pattern; h_loop = loop;
  h_idx = 0; h_phase_on = false; h_deadline = 0;   // fire on the next tick
}

/* A one-shot pulse is fully self-contained (blocking) now, so "active" just means a
 * pattern is playing. */
static inline bool haptics_active(void) { return h_pat != nullptr; }

/* Advance the alarm/timer PATTERN state machine. Call once per loop with millis().
 * (One-shot UI pulses are handled entirely inside haptics_pulse() and never reach here.) */
static void haptics_tick(uint32_t now) {
  if (!h_pat) return;
  if ((int32_t)(now - h_deadline) < 0) return;       // current phase still running

  if (h_phase_on) {
    // End of a symbol's ON time -> go silent for the inter-symbol gap.
    h_motor(false);
    h_phase_on = false;
    h_idx++;
    if (!h_pat[h_idx]) {                              // reached the end of the string
      if (!h_loop) { haptics_stop(); return; }
      h_idx = 0;
      h_deadline = now + H_LOOP_PAUSE_MS;             // longer pause, then repeat
    } else {
      h_deadline = now + H_GAP_MS;
    }
    return;
  }

  // Start the next symbol (skip non-dot/dash chars as a short rest).
  char c = h_pat[h_idx];
  if (c == '.' || c == '-') {
    h_motor(true);
    h_phase_on = true;
    h_deadline = now + (c == '-' ? H_DASH_MS : H_DOT_MS);
  } else {
    h_phase_on = true;                                // treat as a zero-length "on"
    h_deadline = now;                                 // so the gap logic advances idx
    h_motor(false);
  }
}

#endif  /* BOARD_HAS_HAPTICS */
