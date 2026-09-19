/* ============================================================================
 *  swipe_back.h — edge-swipe "back": drag from the LEFT edge to the right to go
 *  back one level, the same as a single BOOT tap (sub-app -> previous screen ->
 *  menu -> watch face). Stops at the watch face (a swipe there does nothing).
 *
 *  How it works: the pointer indev's read callback is WRAPPED (sb_install(), from
 *  the loop until it finds the indev — works for every platform's own indev). A
 *  touch that STARTS in the left edge band is held back from LVGL until it is
 *  clear what it is:
 *    - moves right (mostly horizontal)  -> it's a back swipe; LVGL never sees it
 *    - moves another way / is held      -> handed to LVGL (scroll, long press)
 *    - lifted quickly                   -> delivered to LVGL as a normal tap
 *  so buttons and pagers near the edge keep working.
 *
 *  While dragging, the current screen follows the finger and what is behind it
 *  shows through: the menu (root sub-app), the watch face (menu / flashlight), or
 *  a black backdrop when the previous screen does not exist yet (deeper screens
 *  are rebuilt on back — see nav_back()). Released past the threshold (or flicked)
 *  it slides off, the back action runs, and a rebuilt previous screen slides in
 *  from the left. Otherwise it springs back.
 *
 *  Threading: everything runs inside lv_timer_handler (indev read / anim / async
 *  callbacks), i.e. on the LVGL thread. The animations drive a dummy variable and
 *  re-resolve the target screen each frame, so a screen deleted mid-animation
 *  (BOOT press, app self-refresh) can never leave a dangling pointer or a stuck
 *  animation.
 *
 *  Include after app_menu.h, quick_shade.h, app_timer.h and button_actions.h.
 * ========================================================================== */
#pragma once

#define SB_EDGE_PCT      14    // left edge band that starts a swipe (% of width)
#define SB_SLOP_PX       UI_PX(12)  // movement that decides swipe vs. pass-through
#define SB_HOLD_MS       180   // undecided this long -> hand the press to LVGL
#define SB_COMMIT_PCT    35    // released beyond this (% of width) -> go back
#define SB_FLICK_PCT     12    // ...or beyond this with a fast flick
#define SB_FLICK_PX_MS   6     // flick speed, in px per 10 ms
#define SB_OUT_MS        150   // slide-off duration
#define SB_IN_MS         200   // previous screen slide-in duration
#define SB_SPRING_MS     160   // cancelled swipe spring-back duration

enum { SB_IDLE, SB_PENDING, SB_PASS, SB_DRAG, SB_TAP_REL, SB_SWALLOW };
enum { SB_UNDER_FACE, SB_UNDER_MENU, SB_UNDER_BACKDROP };

static lv_indev_read_cb_t sb_orig = nullptr;   // the platform's real touch read
static uint8_t  sb_state = SB_IDLE;
static bool     sb_busy  = false;              // drag or animation in progress
static bool     sb_on_app = false;             // dragging app_scr (else menu_scr)
static uint8_t  sb_under = SB_UNDER_FACE;
static lv_obj_t *sb_backdrop = nullptr;
static lv_obj_t *sb_arrow    = nullptr;       // "back" arrow shown behind the dragged screen
static int32_t  sb_x0, sb_y0, sb_dx, sb_px, sb_vel;   // vel: px per 10 ms
static uint32_t sb_t0, sb_pt;
static int      sb_anim_var;                   // dummy anim target (see header)

static inline int32_t sb_w(void) { return lv_display_get_horizontal_resolution(NULL); }

static lv_obj_t *sb_target(void) { return sb_on_app ? app_scr : menu_scr; }

static void sb_set_x(int32_t x) {
  lv_obj_t *t = sb_target();
  if (t) lv_obj_set_style_translate_x(t, x, 0);
}

/* Is (x,y) inside the left edge band? On a round panel the band follows the
 * bezel, so the top and bottom of the circle count too. */
static bool sb_in_edge(int32_t x, int32_t y) {
  int32_t w = sb_w();
  int32_t left = 0;
#if BOARD_SCREEN_ROUND
  int32_t r = w / 2, dy = y - r;
  if (dy < -r || dy > r) return false;
  left = r - (int32_t)sqrtf((float)(r * r - dy * dy));
#endif
  return x >= 0 && x - left < w * SB_EDGE_PCT / 100;
}

static bool sb_can_start(void) {
  return app_menu_is_open() && !quick_shade_is_open() && !g_alarm_active;
}

/* The same action as a single BOOT tap (minus alarm/shade, which never start a
 * swipe). */
static void sb_back_action(void) {
  if (flashlight_is_on()) flashlight_off();
  else                    app_menu_back();
}

static void sb_backdrop_del(void) {
  if (sb_backdrop) { lv_obj_delete(sb_backdrop); sb_backdrop = nullptr; }
  if (sb_arrow)    { lv_obj_delete(sb_arrow);    sb_arrow    = nullptr; }
}

static void sb_anim_exec(void *var, int32_t v) { (void)var; sb_set_x(v); }

static void sb_anim_start(int32_t from, int32_t to, uint32_t ms, lv_anim_completed_cb_t done) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &sb_anim_var);
  lv_anim_set_exec_cb(&a, sb_anim_exec);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_completed_cb(&a, done);
  lv_anim_start(&a);
}

static void sb_done(lv_anim_t *a) {
  (void)a;
  sb_set_x(0);
  sb_backdrop_del();
  sb_busy = false;
}

/* Runs outside the anim timer's iteration: this is where screens get deleted
 * and rebuilt. */
static void sb_commit_async(void *p) {
  (void)p;
  sb_set_x(0);                    // same frame as the back action: no flash
  sb_back_action();
  if (app_scr && sb_under != SB_UNDER_MENU) {
    // A (re)built screen: slide it in from the left over the backdrop.
    sb_on_app = true;
    sb_set_x(-sb_w());
    sb_anim_start(-sb_w(), 0, SB_IN_MS, sb_done);
  } else {
    sb_done(nullptr);
  }
}

static void sb_out_done(lv_anim_t *a) { (void)a; lv_async_call(sb_commit_async, nullptr); }

static void sb_cancel_done(lv_anim_t *a) {
  // Put the menu back behind the still-open app.
  if (sb_under == SB_UNDER_MENU && app_scr && menu_scr)
    lv_obj_add_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);
  sb_done(a);
}

static void sb_drag_begin(void) {
  sb_busy   = true;
  sb_on_app = (app_scr != nullptr);
  if (!sb_on_app) {
    sb_under = SB_UNDER_FACE;                  // menu over the watch face
  } else if (flashlight_is_on()) {
    sb_under = SB_UNDER_FACE;                  // flashlight goes straight to the face
  } else if (nav_depth == 0 && !nav_back_intercept && menu_scr) {
    sb_under = SB_UNDER_MENU;                  // root sub-app: the menu is behind it
    lv_obj_clear_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);
  } else {
    // The previous screen is rebuilt on back; show a plain backdrop meanwhile.
    sb_under = SB_UNDER_BACKDROP;
    sb_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sb_backdrop);
    lv_obj_set_size(sb_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(sb_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sb_backdrop, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sb_backdrop, LV_OBJ_FLAG_CLICKABLE);
  }
  // The "back" arrow, in every case: above whatever is behind (menu, face or
  // backdrop) but below the screen being dragged off.
  sb_arrow = lv_label_create(lv_layer_top());
  lv_obj_set_style_text_font(sb_arrow, &MENU_HINT_FONT, 0);
  lv_obj_set_style_text_color(sb_arrow, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_bg_color(sb_arrow, lv_color_black(), 0);   // readable over the menu/face
  lv_obj_set_style_bg_opa(sb_arrow, LV_OPA_70, 0);
  lv_obj_set_style_radius(sb_arrow, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_all(sb_arrow, UI_PX(8), 0);
  lv_label_set_text(sb_arrow, LV_SYMBOL_LEFT);
  lv_obj_align(sb_arrow, LV_ALIGN_LEFT_MID, UI_PX(16), 0);
  lv_obj_clear_flag(sb_arrow, LV_OBJ_FLAG_CLICKABLE);
  if (sb_target()) lv_obj_move_foreground(sb_target());
}

static void sb_drag_move(int32_t x, uint32_t now) {
  int32_t dx = x - sb_x0;
  if (dx < 0) dx = 0;
  if (now != sb_pt) sb_vel = (x - sb_px) * 10 / (int32_t)(now - sb_pt);
  sb_px = x; sb_pt = now;
  if (dx - sb_dx >= 2 || sb_dx - dx >= 2) { sb_dx = dx; sb_set_x(dx); }
}

static void sb_drag_end(void) {
  int32_t w = sb_w();
  bool go = sb_dx > w * SB_COMMIT_PCT / 100 ||
            (sb_dx > w * SB_FLICK_PCT / 100 && sb_vel >= SB_FLICK_PX_MS);
  if (go) sb_anim_start(sb_dx, w, SB_OUT_MS, sb_out_done);
  else    sb_anim_start(sb_dx, 0, SB_SPRING_MS, sb_cancel_done);
}

static void sb_read(lv_indev_t *indev, lv_indev_data_t *d) {
  sb_orig(indev, d);
  bool down = (d->state == LV_INDEV_STATE_PR);
  int32_t x = d->point.x, y = d->point.y;
  uint32_t now = lv_tick_get();

  switch (sb_state) {
  case SB_IDLE:
    if (!down) return;
    if (sb_busy) { sb_state = SB_SWALLOW; d->state = LV_INDEV_STATE_REL; return; }
    if (sb_can_start() && sb_in_edge(x, y)) {
      sb_state = SB_PENDING;
      sb_x0 = sb_px = x; sb_y0 = y; sb_t0 = sb_pt = now; sb_dx = 0; sb_vel = 0;
      d->state = LV_INDEV_STATE_REL;
      return;
    }
    sb_state = SB_PASS;
    return;

  case SB_PENDING: {
    if (!down) {                     // quick tap at the edge: deliver it after all
      d->state = LV_INDEV_STATE_PR;
      d->point.x = sb_x0; d->point.y = sb_y0;
      sb_state = SB_TAP_REL;
      return;
    }
    int32_t dx = x - sb_x0, dy = y - sb_y0;
    if (dy < 0) dy = -dy;
    if (dx >= SB_SLOP_PX && dx > 2 * dy && sb_can_start()) {
      sb_state = SB_DRAG;
      sb_drag_begin();
      sb_drag_move(x, now);
      d->state = LV_INDEV_STATE_REL;
      return;
    }
    if (dy > SB_SLOP_PX || dx < -SB_SLOP_PX || now - sb_t0 > SB_HOLD_MS) {
      sb_state = SB_PASS;            // not a back swipe: LVGL gets the press now
      return;
    }
    d->state = LV_INDEV_STATE_REL;
    return;
  }

  case SB_TAP_REL:                   // release half of a delivered edge tap
    d->state = LV_INDEV_STATE_REL;
    d->point.x = sb_x0; d->point.y = sb_y0;
    sb_state = down ? SB_SWALLOW : SB_IDLE;
    return;

  case SB_DRAG:
    d->state = LV_INDEV_STATE_REL;
    if (down) { sb_drag_move(x, now); return; }
    sb_state = SB_IDLE;
    sb_drag_end();
    return;

  case SB_PASS:
    if (!down) sb_state = SB_IDLE;
    return;

  case SB_SWALLOW:
    d->state = LV_INDEV_STATE_REL;
    if (!down) sb_state = SB_IDLE;
    return;
  }
}

/* Wrap the pointer indev's read callback. Called from the loop until it
 * succeeds (some platforms create their indev late); cheap once installed. */
static void sb_install(void) {
  if (sb_orig) return;
  for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
    if (lv_indev_get_type(i) != LV_INDEV_TYPE_POINTER) continue;
    lv_indev_read_cb_t cb = lv_indev_get_read_cb(i);
    if (!cb || cb == sb_read) continue;
    sb_orig = cb;
    lv_indev_set_read_cb(i, sb_read);
    return;
  }
}
