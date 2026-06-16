/* ============================================================================
 *  app_player.h — "Player" app: a Now Playing screen with transport controls.
 *
 *  Reads the source-agnostic player_state (fed by AMS today; HTTP/Android later)
 *  and renders the current track + play/pause / next / prev buttons. Button taps
 *  call player_send_command(), which routes to whatever source is active. The
 *  screen self-refreshes on a light lv_timer so live track/state updates appear.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER app_menu.h (screen shell)
 *  and player_state.h. Registered as a menu tile in app_menu.h.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

static lv_obj_t  *pl_title_lbl  = nullptr;
static lv_obj_t  *pl_artist_lbl = nullptr;
static lv_obj_t  *pl_album_lbl  = nullptr;
static lv_obj_t  *pl_state_lbl  = nullptr;   // "Nothing playing" / source hint
static lv_obj_t  *pl_play_lbl   = nullptr;   // glyph on the play/pause button
static lv_timer_t *pl_timer     = nullptr;

/* Repaint the labels from player_state. Cheap; called on open + by the timer when
 * player_take_dirty() reports a change. Reads the state under the lock. */
static void pl_refresh(void) {
  char title[PLAYER_STR_MAX], artist[PLAYER_STR_MAX], album[PLAYER_STR_MAX];
  PlayState st; bool have;
  player_lock();
  have = player_has_track();
  strcpy(title,  s_play_title);
  strcpy(artist, s_play_artist);
  strcpy(album,  s_play_album);
  st = s_play_state;
  player_unlock();

  if (pl_title_lbl)  lv_label_set_text(pl_title_lbl,  have && title[0]  ? title  : "");
  if (pl_artist_lbl) lv_label_set_text(pl_artist_lbl, have ? artist : "");
  if (pl_album_lbl)  lv_label_set_text(pl_album_lbl,  have ? album  : "");
  if (pl_state_lbl)
    lv_label_set_text(pl_state_lbl, have ? (st == PLAYING ? "Playing" : "Paused")
                                         : "Nothing playing");
  // Play/pause button glyph reflects state: show PAUSE while playing, PLAY otherwise.
  if (pl_play_lbl)
    lv_label_set_text(pl_play_lbl, (have && st == PLAYING) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

/* Runs every 400 ms: repaint on any state change, and every ~2 s poll the phone for
 * the true playback state so the play/pause button self-corrects if a push update was
 * missed (every 5th tick = 2 s; light enough not to spam BLE). */
static void pl_timer_cb(lv_timer_t *t) {
  (void)t;
  static uint8_t tick = 0;
  if (++tick >= 5) { tick = 0; player_request_sync(); }
  if (player_take_dirty()) pl_refresh();
}

static void pl_cleanup_cb(lv_event_t *e) {
  (void)e;
  if (pl_timer) { lv_timer_del(pl_timer); pl_timer = nullptr; }
  pl_title_lbl = pl_artist_lbl = pl_album_lbl = pl_state_lbl = pl_play_lbl = nullptr;
}

static void pl_cmd_cb(lv_event_t *e) {
  PlayerCmd cmd = (PlayerCmd)(uintptr_t)lv_event_get_user_data(e);
  player_send_command(cmd);
  haptics_pulse(10);
  // NO optimistic state flip: that could desync the button if the command didn't
  // take or the watch's assumed state was already wrong. Instead we trust the phone
  // — its real PlaybackInfo update (which AMS pushes when playback actually changes)
  // drives the button. The periodic re-sync (player_request_sync) also corrects any
  // missed update within a couple seconds, so the button always reflects reality.
}

/* One round transport button (glyph centered). Returns the label so the caller can
 * keep a handle (the play/pause one changes its glyph). */
static lv_obj_t *pl_make_btn(lv_obj_t *parent, const char *glyph, PlayerCmd cmd,
                             int size, uint32_t bg) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, pl_cmd_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)cmd);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, glyph);
  lv_obj_center(l);
  return l;
}

static void app_open_player(void) {
  app_screen_begin("Player");

  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_width(col, LV_PCT(92));     // fit any panel (was fixed 374, the S3 width)
#if BOARD_SCREEN_NARROW
  lv_obj_set_height(col, (int)screenHeight - UI_PX(124) - UI_PX(8));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(124));
#else
  lv_obj_set_height(col, (int)screenHeight - UI_PX(84) - UI_PX(8));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(84));
#endif
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, UI_PX(10), 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(col);  // narrow-panel: shift scrollbar toward the edge (no-op on S3)
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, UI_PX(8), 0);

  // Big music glyph up top.
  lv_obj_t *ic = lv_label_create(col);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(ic, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(ic, LV_SYMBOL_AUDIO);
  lv_obj_set_style_pad_top(ic, 8, 0);

  pl_title_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(pl_title_lbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(pl_title_lbl, lv_color_white(), 0);
  lv_obj_set_width(pl_title_lbl, LV_PCT(96));
  lv_obj_set_style_text_align(pl_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(pl_title_lbl, LV_LABEL_LONG_DOT);

  pl_artist_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(pl_artist_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(pl_artist_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_width(pl_artist_lbl, LV_PCT(96));
  lv_obj_set_style_text_align(pl_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(pl_artist_lbl, LV_LABEL_LONG_DOT);

  pl_album_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(pl_album_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(pl_album_lbl, lv_color_hex(0x888888), 0);
  lv_obj_set_width(pl_album_lbl, LV_PCT(96));
  lv_obj_set_style_text_align(pl_album_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(pl_album_lbl, LV_LABEL_LONG_DOT);

  pl_state_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(pl_state_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(pl_state_lbl, lv_color_hex(ui_accent_soft_hex()), 0);
  lv_obj_set_style_pad_top(pl_state_lbl, 4, 0);

  // Transport row: prev / play-pause (bigger) / next. Height must clear the TALLEST
  // button (the 84px play button) plus the top gap, or the button clips at the bottom.
  lv_obj_t *ctl = lv_obj_create(col);
  lv_obj_remove_style_all(ctl);
  lv_obj_set_width(ctl, LV_PCT(100));
  lv_obj_set_height(ctl, UI_PX(84 + 24));    // tallest button + headroom so nothing clips
  lv_obj_clear_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ctl, UI_PX(22), 0);
  lv_obj_set_style_pad_top(ctl, UI_PX(16), 0);

  pl_make_btn(ctl, LV_SYMBOL_PREV, PCMD_PREV, UI_PX(66), 0x2A2A2A);
  pl_play_lbl = pl_make_btn(ctl, LV_SYMBOL_PLAY, PCMD_TOGGLE, UI_PX(84), ui_accent_hex());
  pl_make_btn(ctl, LV_SYMBOL_NEXT, PCMD_NEXT, UI_PX(66), 0x2A2A2A);

  pl_refresh();
  pl_timer = lv_timer_create(pl_timer_cb, 400, nullptr);   // pick up live updates
  lv_obj_add_event_cb(app_scr, pl_cleanup_cb, LV_EVENT_DELETE, nullptr);
}
