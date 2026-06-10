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

#define HAPTICS_MOTOR_GPIO   18      // MOTOR net (Q1 MMBT3904 driver), active HIGH
#define HAPTICS_ACTIVE_HIGH   1

/* Pulse/gap shape (ms). Tuned to feel like an insistent fast heartbeat. */
#define H_DOT_MS        70
#define H_DASH_MS      170
#define H_GAP_MS        70           // silence between symbols
#define H_LOOP_PAUSE_MS 520          // silence before a looping pattern repeats

/* The alarm rhythm requested for the timer: a quick double-tap heartbeat. */
#define HAPTICS_HEARTBEAT "-.-.---.-.---.-.-"

static const char *h_pat      = nullptr;  // active pattern (null = none)
static bool        h_loop     = false;
static uint16_t    h_idx      = 0;
static bool        h_phase_on = false;    // currently mid-ON of a symbol?
static uint32_t    h_deadline = 0;        // ms when the current phase ends
static bool        h_pulse    = false;    // a one-shot pulse is active
static uint32_t    h_pulse_off= 0;

static inline void h_motor(bool on) {
  digitalWrite(HAPTICS_MOTOR_GPIO,
               (HAPTICS_ACTIVE_HIGH ? on : !on) ? HIGH : LOW);
}

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
  h_pulse = false;
  h_motor(false);
}

/* One-shot buzz of `ms` (e.g. a 15-20 ms tick of UI feedback). Overrides any
 * running pattern. */
static void haptics_pulse(uint16_t ms) {
  h_pat = nullptr;                  // a pulse and a pattern are mutually exclusive
  h_pulse = true;
  h_pulse_off = millis() + ms;
  h_motor(true);
}

/* Start a dot/dash pattern. loop=true repeats it (with a pause) until stopped. */
static void haptics_play(const char *pattern, bool loop) {
  h_pulse = false;
  h_pat = pattern; h_loop = loop;
  h_idx = 0; h_phase_on = false; h_deadline = 0;   // fire on the next tick
}

static inline bool haptics_active(void) { return h_pat != nullptr || h_pulse; }

/* Advance the motor state machine. Call once per loop with millis(). */
static void haptics_tick(uint32_t now) {
  // One-shot pulse takes priority and is self-contained.
  if (h_pulse) {
    if ((int32_t)(now - h_pulse_off) >= 0) { h_motor(false); h_pulse = false; }
    return;
  }
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
