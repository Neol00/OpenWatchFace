/* ============================================================================
 *  crown_nav.h — rotating crown -> scrolling + the quick-shade (BOARD_HAS_CROWN).
 *
 *  Compiled out ENTIRELY when the flag is 0, exactly like button_nav.h: boards
 *  without a crown carry no code and no runtime cost, and the .ino call sites
 *  are #if-gated too.
 *
 *  WHAT THE CROWN DOES HERE:
 *    - On the watchface: rolling DOWN pulls the quick-shade down, the same
 *      thing the touch boards' pull-down gesture does. Rolling UP on the bare
 *      watchface does nothing (there is nothing above it).
 *    - With the shade open: rolling UP closes it again, so the gesture is
 *      symmetrical and you never have to reach for BOOT.
 *    - Anywhere else (an app, the launcher, a settings list): the crown
 *      SCROLLS whatever is scrollable on screen.
 *
 *  WHY IT SCROLLS THE OBJECT AND DOES NOT USE AN LVGL ENCODER INDEV. An
 *  encoder indev moves FOCUS between widgets and only scrolls as a side effect
 *  of focusing something off-screen. That is the right model for a device with
 *  no touch, and the wrong one here: this watch has a working touchscreen, so
 *  the crown is a second way to move the SAME view, not a replacement pointer.
 *  Registering an encoder indev would also mean every app suddenly grows focus
 *  rings and a focus order nobody designed. Scrolling the container directly
 *  keeps the crown additive -- it changes nothing about how any app looks or
 *  how touch behaves.
 *
 *  Threading: everything runs on the loop thread alongside lv_task_handler, so
 *  touching LVGL objects here is safe -- same rule as button_nav.h.
 *
 *  Include from the .ino AFTER quick_shade.h and app_menu.h (uses
 *  quick_shade_open/close/is_open and app_menu_is_open).
 * ========================================================================== */
#pragma once

#if BOARD_HAS_CROWN

#ifndef CROWN_SCROLL_PX_PER_CNT
#define CROWN_SCROLL_PX_PER_CNT 6
#endif
#ifndef CROWN_SHADE_OPEN_CNT
#define CROWN_SHADE_OPEN_CNT 12
#endif
#ifndef CROWN_IDLE_RESET_MS
#define CROWN_IDLE_RESET_MS 400
#endif

/* Progress toward the watchface "roll down to open the shade" gesture. Kept as
 * an accumulator rather than firing on the first count so a knock or a stray
 * count while the watch is in a sleeve cannot yank the shade down. */
static int      cn_gesture = 0;
static uint32_t cn_last_ms = 0;

/* Find what the crown should scroll: the deepest object that is scrollable AND
 * actually has somewhere to go vertically.
 *
 * "Actually has somewhere to go" is the important half. Nearly every LVGL
 * container is created with LV_OBJ_FLAG_SCROLLABLE whether or not its content
 * overflows, so testing the flag alone finds a page's outermost box and then
 * scrolls it by zero, which feels like the crown is broken. lv_obj_get_scroll_
 * top/bottom report the pixels available above and below the current position,
 * so a container with room in EITHER direction is a real target and everything
 * else is skipped.
 *
 * Depth-first, children before parents: a list inside a page should win over
 * the page that contains it. */
static lv_obj_t *cn_find_scrollable(lv_obj_t *o) {
  if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return nullptr;
  /* The quick-shade is parked OFF-SCREEN rather than hidden when closed (only
   * its scrim gets LV_OBJ_FLAG_HIDDEN), so without this it is a live candidate
   * sitting above every app on the top layer -- the crown would scroll a panel
   * the user cannot see. */
  if (o == qs_panel || o == qs_scrim) return nullptr;
  uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *r = cn_find_scrollable(lv_obj_get_child(o, i));
    if (r) return r;
  }
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) &&
      (lv_obj_get_scroll_top(o) > 0 || lv_obj_get_scroll_bottom(o) > 0))
    return o;
  return nullptr;
}

/* WHERE TO LOOK -- and this is what made the first version scroll nothing at
 * all. Apps and the launcher are NOT children of the active screen: app_menu.h
 * builds them with lv_obj_create(lv_layer_top()), so the whole UI the crown is
 * supposed to scroll lives on the TOP LAYER while lv_screen_active() holds
 * only the watchface underneath. Searching the active screen therefore found
 * nothing in every app, every time, and the crown looked dead there while the
 * shade (handled before this branch) worked fine.
 *
 * Top layer first because it is what is actually in front of the user; the
 * active screen is the fallback for a board or a future view that puts
 * scrollable content there instead. */
static lv_obj_t *cn_scroll_target(void) {
  lv_obj_t *t = cn_find_scrollable(lv_layer_top());
  if (!t) t = cn_find_scrollable(lv_screen_active());
  return t;
}

/* Poll the crown and act on it. Returns true if the crown did something, so
 * the caller can treat it as user activity (undim, restart the idle timer) —
 * the same contract button_nav_poll() has. */
static bool crown_nav_poll(uint32_t ms) {
  crown_poll();                       /* driver-side I2C read; self-rate-limited */
  int d = crown_take_delta();

  /* A pause abandons a half-finished shade gesture, so counts from a turn
   * thirty seconds ago cannot combine with a fresh nudge to open the shade. */
  if (d == 0) {
    if (cn_gesture && (uint32_t)(ms - cn_last_ms) > CROWN_IDLE_RESET_MS)
      cn_gesture = 0;
    return false;
  }
  cn_last_ms = ms;

  /* An alarm owns the screen; the crown must not scroll behind it. */
  if (g_alarm_active) return false;

  /* Sign convention: POSITIVE = rolling "down" (toward the wearer), the
   * direction that pulls the shade down and scrolls a list downward. If the
   * watch turns out to report the opposite sense, flip it once in the DRIVER
   * (-DPLAT_CROWN_INVERT) rather than here — that keeps every consumer of
   * crown_take_delta() consistent instead of only this one. */
  if (quick_shade_is_open()) {
    cn_gesture = 0;
    if (d < 0) { quick_shade_close(); return true; }
    return false;                     /* rolling further down inside the shade: no-op */
  }

  if (!app_menu_is_open()) {
    /* Bare watchface: accumulate downward motion until the gesture is clearly
     * deliberate, then drop the shade. Upward motion cancels rather than
     * subtracts, so a wobble cannot creep the counter upward over time. */
    if (d > 0) {
      cn_gesture += d;
      if (cn_gesture >= CROWN_SHADE_OPEN_CNT) {
        cn_gesture = 0;
        quick_shade_open();
        return true;
      }
    } else {
      cn_gesture = 0;
    }
    return false;
  }

  /* In the launcher or an app: scroll whatever can scroll. */
  cn_gesture = 0;
  lv_obj_t *tgt = cn_scroll_target();
  if (!tgt) return false;

  /* Scrolling DOWN means the content moves UP, hence the negation.
   *
   * NOT ANIMATED, and that is a fix rather than a preference. The crown
   * delivers counts every ~15 ms, and lv_obj_scroll_by(..., LV_ANIM_ON)
   * animates from the CURRENT position to current+delta -- so each new count
   * replaced the running animation and threw away however much of the previous
   * step had not been played yet. A steady turn therefore moved far less than
   * px_per_count x counts, which is why it felt stuck on the lowest
   * sensitivity. Applying the delta immediately loses nothing, and at 66
   * updates a second it reads as smooth motion anyway -- the same way a finger
   * drag does, which is also unanimated.
   *
   * Bounded, too: with no touch release to snap back, an unbounded scroll_by
   * would let the crown push the view into the elastic over-scroll zone and
   * leave it parked there. */
  lv_obj_scroll_by_bounded(tgt, 0, -d * CROWN_SCROLL_PX_PER_CNT, LV_ANIM_OFF);
  return true;
}

#endif /* BOARD_HAS_CROWN */
