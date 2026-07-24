/* ============================================================================
 *  ble_notif_flags_tuya.h - shared BLE-notification -> loop handoff flags for the
 *  Tuya build, plus the ancs_* public accessors the .ino/app/sleep code calls.
 *
 *  On the ESP32 build these flags and accessors live inside ble_ancs.h. The Tuya
 *  Phase A doesn't have ANCS yet, but its Gadgetbridge server
 *  (ble_gadgetbridge_tuya.h) feeds the SAME store and needs the SAME handoff
 *  flags, and the loop/apps unconditionally call the ancs_* accessors. So this
 *  small header owns them for Tuya:
 *    - the store-changed flags (s_ancs_ui_dirty / s_ancs_removed / added_total),
 *    - the take-once accessors the loop polls (ancs_take_ui_dirty / _removed),
 *    - the incoming-call accessors (no live-call UI without ANCS -> inert),
 *    - dismiss / background-check (no iPhone link in Phase A -> no-op).
 *
 *  PHASE B (active): the .ino defines BLE_TUYA_HAVE_ANCS before ble_tuya.h, and
 *  ble_ancs_tuya.h (included after the notif store) owns the real versions of the
 *  dismiss / incoming / background-check accessors; the flags + take accessors
 *  here stay (ANCS and Gadgetbridge share them, just as ble_gadgetbridge.h reuses
 *  ble_ancs.h's flags on the ESP32 build).
 *
 *  INCLUDE AFTER notif_net.h (s_pop_*) and BEFORE ble_gadgetbridge_tuya.h.
 * ========================================================================== */
#pragma once
#include <Arduino.h>

/* Store-changed handoff (set by a BLE notification source on the NimBLE task,
 * consumed by the loop). Names kept identical to ble_ancs.h so the .ino loop and
 * ble_gadgetbridge_tuya.h are byte-for-byte the same as the ESP32 path. */
static volatile bool     s_ancs_ui_dirty    = false;   // a new notification was stored -> pop card + bell
static volatile bool     s_ancs_removed     = false;   // a notification was removed -> refresh bell/list
static volatile uint32_t s_ancs_added_total = 0;       // monotonic add counter (timer-wake check)

/* Loop polls these once per iteration (LVGL-thread safe). */
static bool ancs_take_ui_dirty(void) { if (!s_ancs_ui_dirty) return false; s_ancs_ui_dirty = false; return true; }
static bool ancs_take_removed(void)  { if (!s_ancs_removed)  return false; s_ancs_removed  = false; return true; }

/* Live incoming-call accessors - inert in Phase A (no ANCS, so no live call). */
#ifndef BLE_TUYA_HAVE_ANCS
static bool        ancs_take_incoming_dirty(void) { return false; }
static bool        ancs_incoming_active(void)     { return false; }
static const char *ancs_incoming_who(void)        { return ""; }

/* Dismiss-on-phone (ANCS PerformAction) - no iPhone link in Phase A. The local
 * store removal still happens in the caller; these just skip the phone echo. */
static void ancs_dismiss_id(uint64_t /*id*/) {}
static void ancs_dismiss_all(void) {}

/* Deep-sleep timer-wake background fetch - ANCS-only; nothing to do in Phase A.
 * (Gadgetbridge/Android delivers only while the loop runs and a phone app is
 * connected, not on the NimBLE-less wake path.) */
static bool ancs_background_check(uint32_t /*budget_ms*/) { return false; }
#endif
