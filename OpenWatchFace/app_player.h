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
  // buzz comes from the global click hook now
  // NO optimistic state flip: that could desync the button if the command didn't
  // take or the watch's assumed state was already wrong. Instead we trust the phone
  // — its real PlaybackInfo update (which AMS pushes when playback actually changes)
  // drives the button. The periodic re-sync (player_request_sync) also corrects any
  // missed update within a couple seconds, so the button always reflects reality.
}

/* Transport button diameters (reference px; UI_PX scales them per panel).
 * Mid-narrow (S3-1.64) enlarges them: UI_PX scales to 68% there, which left
 * PREV/NEXT at ~45 px and PLAY at ~57 px — usable but small, as reported. The
 * bumped references give ~60 px and ~79 px instead. The row still fits easily:
 * 2*88 + 116 + two 22 px gaps = 336 reference px -> ~229 px on the 280 px panel.
 * Every other board keeps the original 66/84, unchanged. */
/* SUBREF, not MIDNARROW: these are REFERENCE px that UI_PX then scales down, so
 * the question is "is this panel narrower than the reference" — not how many
 * launcher columns it takes. The S3-Touch-LCD-2 (240 wide) needs the same bump for
 * the same reason the S3-1.64 did: at the plain 66/84 references its scaled
 * transport buttons come out too small to hit comfortably. */
#if BOARD_SCREEN_SUBREF
#define PL_BTN_SIDE 88
#define PL_BTN_PLAY 116
#else
#define PL_BTN_SIDE 66
#define PL_BTN_PLAY 84
#endif

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
  lv_obj_set_style_text_font(l, &UI_FONT(20), 0);
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
#elif BOARD_SCREEN_ROUND_SMALL
  // SMALL ROUND face: this screen is a TOP-ANCHORED flex column, so the transport
  // row does not have a position of its own — it lands wherever the stack above it
  // ends. On the C2 that stack measured ~299 px starting at y=82, putting the row at
  // y 287..381 on a 360 px panel: the reported "play and skip buttons half off the
  // bottom". The buttons themselves are the right size (UI_PX(84)/UI_PX(66) = 73/57
  // px), so nothing here shrinks them — the fix is to reclaim the ~45 px of vertical
  // slack that the stack above spends, and start it higher:
  //     column top   UI_PX(84) -> UI_PX(70)          -10 px
  //     pad_all      UI_PX(10) -> UI_PX(6)            -8 px
  //     pad_row      UI_PX(8)  -> UI_PX(3)           -20 px (5 gaps)
  //     header glyph UI_FONT(40) -> UI_FONT(30)      -12 px
  //     glyph pad_top 8 -> 2, state pad_top 4 -> 0    -10 px
  // That lands the row at y 229..319 with the play button bottom at ~309, where the
  // inscribed circle is still ~251 px wide against the row's 225 px.
  lv_obj_set_height(col, (int)screenHeight - UI_PX(70) - UI_PX(24));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(70));
#else
  lv_obj_set_height(col, (int)screenHeight - UI_PX(84) - UI_PX(8));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(84));
#endif
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_set_style_pad_all(col, UI_PX(6), 0);
#else
  lv_obj_set_style_pad_all(col, UI_PX(10), 0);
#endif
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(col);  // narrow-panel: shift scrollbar toward the edge (no-op on S3)
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_set_style_pad_row(col, UI_PX(3), 0);
#else
  lv_obj_set_style_pad_row(col, UI_PX(8), 0);
#endif

  // Big music glyph up top.
  lv_obj_t *ic = lv_label_create(col);
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_set_style_text_font(ic, &UI_FONT(30), 0);
#else
  lv_obj_set_style_text_font(ic, &UI_FONT(40), 0);
#endif
  lv_obj_set_style_text_color(ic, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(ic, LV_SYMBOL_AUDIO);
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_set_style_pad_top(ic, 2, 0);
#else
  lv_obj_set_style_pad_top(ic, 8, 0);
#endif

  pl_title_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(pl_title_lbl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(pl_title_lbl, lv_color_white(), 0);
  lv_obj_set_width(pl_title_lbl, LV_PCT(96));
  lv_obj_set_style_text_align(pl_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
  ui_label_single_line(pl_title_lbl);

  pl_artist_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(pl_artist_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(pl_artist_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_width(pl_artist_lbl, LV_PCT(96));
  lv_obj_set_style_text_align(pl_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
  ui_label_single_line(pl_artist_lbl);

  pl_album_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(pl_album_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(pl_album_lbl, lv_color_hex(0x888888), 0);
  lv_obj_set_width(pl_album_lbl, LV_PCT(96));
  lv_obj_set_style_text_align(pl_album_lbl, LV_TEXT_ALIGN_CENTER, 0);
  ui_label_single_line(pl_album_lbl);

  pl_state_lbl = lv_label_create(col);
  lv_obj_set_style_text_font(pl_state_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(pl_state_lbl, lv_color_hex(ui_accent_soft_hex()), 0);
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_set_style_pad_top(pl_state_lbl, 0, 0);
#else
  lv_obj_set_style_pad_top(pl_state_lbl, 4, 0);
#endif

  // Transport row: prev / play-pause (bigger) / next. Height must clear the TALLEST
  // button (the 84px play button) plus the top gap, or the button clips at the bottom.
  lv_obj_t *ctl = lv_obj_create(col);
  lv_obj_remove_style_all(ctl);
  lv_obj_set_width(ctl, LV_PCT(100));
#if BOARD_SCREEN_ROUND_SMALL
  // Headroom trimmed 24 -> 20 and the row's own top pad 16 -> 8 (below), so the
  // box still fully contains the tallest button (UI_PX(84) = 73 px) instead of
  // reserving slack this panel cannot afford: 7 + 73 = 80 <= UI_PX(104) = 90.
  lv_obj_set_height(ctl, UI_PX(PL_BTN_PLAY + 20));
#else
  lv_obj_set_height(ctl, UI_PX(PL_BTN_PLAY + 24));  // tallest button + headroom so nothing clips
#endif
  lv_obj_clear_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ctl, UI_PX(22), 0);
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_set_style_pad_top(ctl, UI_PX(8), 0);
#else
  lv_obj_set_style_pad_top(ctl, UI_PX(16), 0);
#endif

  pl_make_btn(ctl, LV_SYMBOL_PREV, PCMD_PREV, UI_PX(PL_BTN_SIDE), 0x2A2A2A);
  pl_play_lbl = pl_make_btn(ctl, LV_SYMBOL_PLAY, PCMD_TOGGLE, UI_PX(PL_BTN_PLAY), ui_accent_hex());
  pl_make_btn(ctl, LV_SYMBOL_NEXT, PCMD_NEXT, UI_PX(PL_BTN_SIDE), 0x2A2A2A);

  pl_refresh();
  pl_timer = lv_timer_create(pl_timer_cb, 400, nullptr);   // pick up live updates
  lv_obj_add_event_cb(app_scr, pl_cleanup_cb, LV_EVENT_DELETE, nullptr);
}
