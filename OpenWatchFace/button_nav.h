/* ============================================================================
 *  button_nav.h — physical UP/DOWN/SELECT navigation for touch-less boards
 *  (BOARD_HAS_NAV_BUTTONS). Compiled out ENTIRELY when the flag is 0: the real
 *  code below is preprocessed away and the .ino call sites are #if-gated too,
 *  so touch boards carry zero code and zero runtime cost from this file.
 *
 *  Scope (first slice — the quick-shade only):
 *    - Watchface: DOWN opens the quick-shade (brightness is the #1 "right now"
 *      control, same reasoning as the touch boards' pull-down gesture).
 *    - Inside the shade a HIGHLIGHT (outline) marks the focused item:
 *        0 = brightness slider (the DEFAULT on open), 1 = caffeine, 2 = mute.
 *      UP/DOWN move the highlight; SELECT activates:
 *        * slider  -> toggles EDIT mode (outline turns accent): UP/DOWN now
 *          adjust brightness live (hold = auto-repeat); SELECT or BOOT leaves
 *          edit mode (BOOT is intercepted via button_nav_handle_boot()).
 *        * toggles -> synthesizes LV_EVENT_CLICKED (same path as a tap).
 *    - Close: UP while the highlight is already at the top (the slider, not in
 *      edit mode), or BOOT (the .ino's existing shade-close path).
 *
 *  Threading: everything runs on the loop thread (same thread as
 *  lv_task_handler), so touching LVGL objects here is safe — the same pattern
 *  the BOOT-button handler uses.
 *
 *  Include from the .ino AFTER quick_shade.h (reaches into its qs_* statics —
 *  header-only, same TU) and after app_menu.h (app_menu_is_open).
 * ========================================================================== */
#pragma once

#if BOARD_HAS_NAV_BUTTONS

#define BN_STEP_PCT      5     // brightness change per UP/DOWN press in edit mode
#define BN_DEBOUNCE_US   1500  // press-edge confirm (same trick as the BOOT key)
#define BN_RPT_DELAY_MS  400   // hold this long before auto-repeat kicks in...
#define BN_RPT_EVERY_MS  125   // ...then repeat at this rate (edit mode only)

/* Per-button debounce/repeat state. */
typedef struct { uint8_t pin; bool down; uint32_t t_down, t_rpt; } bn_btn_t;
static bn_btn_t bn_up  = { BTN_UP_GPIO,     false, 0, 0 };
static bn_btn_t bn_dn  = { BTN_DOWN_GPIO,   false, 0, 0 };
static bn_btn_t bn_sel = { BTN_SELECT_GPIO, false, 0, 0 };

static int  bn_focus = 0;           // focused shade item: 0=slider 1=caffeine 2=mute
static bool bn_edit  = false;       // slider edit mode (UP/DOWN adjust brightness)
static bool bn_shown = false;       // focus outline currently applied to a widget

static inline bool bn_raw(uint8_t pin) {
#if BTN_ACTIVE_LOW
  return digitalRead(pin) == LOW;
#else
  return digitalRead(pin) == HIGH;
#endif
}

static inline lv_obj_t *bn_obj(int i) {
  return (i == 0) ? qs_slider : (i == 1) ? qs_caf_btn : qs_mute_btn;
}

/* Paint the focus highlight: clear the outline everywhere, then outline the
 * focused widget — WHITE = focused, ACCENT = slider edit mode (adjusting). */
static void bn_style_apply(void) {
  for (int i = 0; i < 3; i++) {
    lv_obj_t *o = bn_obj(i);
    if (o) lv_obj_set_style_outline_width(o, 0, 0);
  }
  lv_obj_t *f = bn_obj(bn_focus);
  if (f) {
    lv_obj_set_style_outline_width(f, UI_PX(4), 0);
    lv_obj_set_style_outline_pad(f, UI_PX(4), 0);
    lv_obj_set_style_outline_color(f,
        bn_edit ? lv_color_hex(ui_accent_hex()) : lv_color_white(), 0);
  }
  bn_shown = true;
}

static void bn_style_clear(void) {
  for (int i = 0; i < 3; i++) {
    lv_obj_t *o = bn_obj(i);
    if (o) lv_obj_set_style_outline_width(o, 0, 0);
  }
  bn_shown = false;
  bn_focus = 0;
  bn_edit  = false;
}

/* Nudge the brightness slider by ±BN_STEP_PCT and apply live — the button
 * equivalent of dragging the knob (mirrors qs_slider_cb's non-Tuya path: label
 * + backlight track every step; nav-button boards are all ESP, no QSPI-write
 * contention concern here). */
static void bn_adjust(int d) {
  if (!qs_slider) return;
  int pct = (int)lv_slider_get_value(qs_slider) + d;
  if (pct < QS_BRIGHT_MIN_PCT) pct = QS_BRIGHT_MIN_PCT;
  if (pct > 100) pct = 100;
  lv_slider_set_value(qs_slider, pct, LV_ANIM_OFF);
  if (qs_val) lv_label_set_text_fmt(qs_val, "%d%%", pct);
  settings_set_brightness(qs_pct_to_raw(pct));
}

/* UP pressed (or auto-repeated). */
static void bn_on_up(void) {
  if (!quick_shade_is_open()) return;          // (menus later — watchface: no-op)
  if (bn_edit)             { bn_adjust(+BN_STEP_PCT); return; }
  if (bn_focus == 0)       { bn_style_clear(); quick_shade_close(); return; }  // top -> close
  bn_focus--;
  bn_style_apply();
}

/* DOWN pressed (or auto-repeated). */
static void bn_on_down(void) {
  if (!quick_shade_is_open()) {
    // Watchface shortcut: DOWN pulls the shade down (only when no menu is open,
    // so a later menu-nav layer can claim DOWN there without a conflict).
    if (!app_menu_is_open()) {
      bn_focus = 0; bn_edit = false;           // default selection = the slider
      quick_shade_open();
      bn_style_apply();
    }
    return;
  }
  if (bn_edit)        { bn_adjust(-BN_STEP_PCT); return; }
  if (bn_focus < 2)   { bn_focus++; bn_style_apply(); }
  else                bn_style_apply();        // bottom: stay (also heals a touch-opened shade)
}

/* SELECT pressed. */
static void bn_on_select(void) {
  if (!quick_shade_is_open()) return;
  if (bn_focus == 0) {
    bn_edit = !bn_edit;                        // enter/leave brightness edit mode
    bn_style_apply();
  } else {
    lv_obj_t *b = bn_obj(bn_focus);
    if (b) lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);   // same path as a tap
    bn_style_apply();                          // make sure the highlight is visible
  }
}

/* One debounced press-edge detector with hold-to-repeat. `repeats` enables the
 * auto-repeat (UP/DOWN only, and only useful in edit mode — but repeating focus
 * moves is harmless since focus clamps, so it's gated at the call site). */
static bool bn_poll_btn(bn_btn_t *b, uint32_t ms, bool repeats) {
  bool raw = bn_raw(b->pin);
  if (raw && !b->down) {
    delayMicroseconds(BN_DEBOUNCE_US);         // same inline confirm as the BOOT key
    if (!bn_raw(b->pin)) return false;         // bounce -> ignore
    b->down = true; b->t_down = ms; b->t_rpt = ms;
    return true;                               // fresh press edge
  }
  if (!raw && b->down) b->down = false;        // release
  else if (raw && b->down && repeats &&
           (ms - b->t_down) >= BN_RPT_DELAY_MS &&
           (ms - b->t_rpt)  >= BN_RPT_EVERY_MS) {
    b->t_rpt = ms;
    return true;                               // auto-repeat fire
  }
  return false;
}

/* BOOT hook, called from the .ino's BOOT single-tap dispatch BEFORE the
 * shade-close branch: in slider edit mode BOOT only DESELECTS (leaves edit
 * mode) and is consumed; otherwise not consumed, so BOOT falls through to its
 * normal behavior (close the shade / menu back). */
static bool button_nav_handle_boot(void) {
  if (quick_shade_is_open() && bn_edit) {
    bn_edit = false;
    bn_style_apply();
    return true;
  }
  return false;
}

/* Poll all three buttons once per loop iteration. Returns true if anything
 * fired (a press edge or a repeat) so the caller can refresh the idle/dim
 * timers exactly like touch/BOOT activity. Auto-repeat is enabled only while
 * adjusting the slider — everywhere else a hold is a single action. */
static bool button_nav_poll(uint32_t ms) {
  // Shade got closed behind our back (scrim tap, force-close by an alarm, BOOT):
  // drop the stale highlight so the next open starts fresh at the slider.
  if (!quick_shade_is_open() && bn_shown) bn_style_clear();

  bool act = false;
  if (bn_poll_btn(&bn_up,  ms, bn_edit)) { bn_on_up();     act = true; }
  if (bn_poll_btn(&bn_dn,  ms, bn_edit)) { bn_on_down();   act = true; }
  if (bn_poll_btn(&bn_sel, ms, false))   { bn_on_select(); act = true; }
  return act;
}

#endif /* BOARD_HAS_NAV_BUTTONS */
