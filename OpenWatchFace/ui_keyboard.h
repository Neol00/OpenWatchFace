/* ============================================================================
 *  ui_keyboard.h — on-screen QWERTY for boards with no physical keyboard.
 *
 *  Exists so a WiFi password (and, later, any other short string) can be typed
 *  on the watch itself instead of only being pushed in over BLE provisioning.
 *
 *  WHY NOT lv_keyboard: LVGL's stock keyboard is laid out for a phone-shaped
 *  rectangle. Its bottom row carries the mode switch, cursor arrows and OK at
 *  the extreme left/right edges — exactly the pixels a ROUND panel clips away,
 *  which on the Fossil/TicWatch bezels makes "OK" and "1#" unreachable. So this
 *  is a plain button matrix with a watch-shaped map: no cursor arrows, and the
 *  bottom row is three wide keys (mode / space / done) whose hit areas stay well
 *  inside the circle. It is also narrower per row than the stock 10-wide map on
 *  the rows that matter, so a 240 px panel still gets ~24 px keys.
 *
 *  Modes: lowercase, uppercase (caps-lock style — the shift key stays engaged
 *  until pressed again, so what you see on the keys is always what you get),
 *  numbers/common symbols, and a second symbol page for the rest of ASCII.
 *  Everything a WPA2 passphrase can contain is reachable.
 *
 *  The text is shown IN THE CLEAR. On a watch screen you cannot correct a typo
 *  you cannot see, and a 1-inch panel held at arm's length is not a
 *  shoulder-surfing surface. Same call the T-Deck Pro prompt already made.
 *
 *  Header-only, part of the .ino translation unit. INCLUDE AFTER app_menu.h:
 *  it parents onto app_scr and borrows nav_back_intercept so the physical BOOT
 *  button closes the keyboard before it pops the screen.
 *
 *  Usage:
 *      owf_kb_open("Password for \"net\"", "", 64, on_done, user_ptr);
 *  on_done(text, user) is called from the LVGL event context with the typed
 *  string; the keyboard is already closed by then. Cancel calls nothing.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

/* Boards that have a real keyboard don't want a fake one over the top of it. */
#define OWF_HAS_OSK (!BOARD_HAS_KEYBOARD_TCA8418)

#if OWF_HAS_OSK

#define OWF_KB_TEXT_MAX 96

#if BOARD_SCREEN_ROUND
/* ---- round-panel geometry -------------------------------------------------
 * A round display is square in memory with the corners simply not there, so
 * anything placed by "inset N px from the right edge" lands outside the glass
 * near the top. Measured on the first cut of this keyboard: the Cancel X was
 * fully invisible on the Gen 4 (416 px) and half-cut on the C2 (360) and the
 * S2 (400) -- at the row it sat on, the circle spans only about +-50 px from
 * centre and the button had been put at +117..+174.
 *
 * So ask the circle instead of guessing an inset. For a panel of radius R, the
 * usable half-width at row y is sqrt(R^2 - (R-y)^2). In the TOP half the
 * narrowest row an object covers is its TOP edge, so that is what everything
 * up here is fitted against. */
static int kb_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}
static int kb_half_w(int y)
{
    int R = (int)screenWidth / 2;
    int dy = R - y; if (dy < 0) dy = -dy;
    int v = R * R - dy * dy;
    return v <= 0 ? 0 : kb_isqrt(v);
}
#endif

typedef void (*owf_kb_done_fn)(const char *text, void *user);

static lv_obj_t      *s_kb_box  = nullptr;   // full-screen modal
static lv_obj_t      *s_kb_ta   = nullptr;   // the one-line textarea
static lv_obj_t      *s_kb_mtx  = nullptr;   // the key matrix
static owf_kb_done_fn s_kb_done = nullptr;
static void          *s_kb_user = nullptr;
static bool         (*s_kb_prev_back)(void) = nullptr;   // restored on close
static uint8_t        s_kb_mode = 0;         // 0 lower, 1 upper, 2 num, 3 sym

/* ---- key maps -------------------------------------------------------------
 * Row 4 is always {mode, space, done} so the three keys people hit most often
 * are the three biggest targets and none of them sits in a clipped corner. */
static const char * const kb_map_lower[] = {
  "q","w","e","r","t","y","u","i","o","p","\n",
  "a","s","d","f","g","h","j","k","l","\n",
  LV_SYMBOL_UP,"z","x","c","v","b","n","m",LV_SYMBOL_BACKSPACE,"\n",
  "?123"," ",LV_SYMBOL_OK,""
};
static const char * const kb_map_upper[] = {
  "Q","W","E","R","T","Y","U","I","O","P","\n",
  "A","S","D","F","G","H","J","K","L","\n",
  LV_SYMBOL_DOWN,"Z","X","C","V","B","N","M",LV_SYMBOL_BACKSPACE,"\n",
  "?123"," ",LV_SYMBOL_OK,""
};
static const char * const kb_map_num[] = {
  "1","2","3","4","5","6","7","8","9","0","\n",
  "@","#","$","_","&","-","+","(",")","/","\n",
  "=\\<","*","\"","'",":",";","!","?",LV_SYMBOL_BACKSPACE,"\n",
  "abc"," ",LV_SYMBOL_OK,""
};
static const char * const kb_map_sym[] = {
  "~","`","|","^","=","{","}","[","]","\\","\n",
  "<",">","%",".",",","\"","'",":",";","/","\n",
  "123","!","?","@","#","$","&","*",LV_SYMBOL_BACKSPACE,"\n",
  "abc"," ",LV_SYMBOL_OK,""
};

static void kb_apply_map(uint8_t mode) {
  const char * const *m = kb_map_lower;
  if (mode == 1) m = kb_map_upper;
  else if (mode == 2) m = kb_map_num;
  else if (mode == 3) m = kb_map_sym;
  s_kb_mode = mode;
  lv_buttonmatrix_set_map(s_kb_mtx, m);
  /* Widen the bottom row by name rather than by index: the maps have different
   * key counts per row, and a hard-coded index silently widens the wrong key
   * the first time a map is edited. Walk until get_button_text() runs out. */
  for (uint32_t i = 0; i < 64; i++) {
    const char *t = lv_buttonmatrix_get_button_text(s_kb_mtx, i);
    if (!t) break;
    if (!strcmp(t, " "))
      lv_buttonmatrix_set_button_ctrl(s_kb_mtx, i, LV_BUTTONMATRIX_CTRL_WIDTH_5);
    else if (!strcmp(t, "?123") || !strcmp(t, "abc"))
      lv_buttonmatrix_set_button_ctrl(s_kb_mtx, i, LV_BUTTONMATRIX_CTRL_WIDTH_3);
    else if (!strcmp(t, LV_SYMBOL_OK))
      lv_buttonmatrix_set_button_ctrl(s_kb_mtx, i, LV_BUTTONMATRIX_CTRL_WIDTH_3);
  }
}

static void owf_kb_close(void) {
  if (!s_kb_box) return;
  nav_back_intercept = s_kb_prev_back;
  lv_obj_t *box = s_kb_box;
  s_kb_box = nullptr; s_kb_ta = nullptr; s_kb_mtx = nullptr;   // clear FIRST: the
  lv_obj_del(box);                                            // delete cb re-enters
}

/* The parent screen is torn down on Back or on any rebuild — never dangle. */
static void kb_deleted_cb(lv_event_t *e) {
  if (lv_event_get_target(e) == s_kb_box) {
    nav_back_intercept = s_kb_prev_back;
    s_kb_box = nullptr; s_kb_ta = nullptr; s_kb_mtx = nullptr;
  }
}
static bool kb_back_intercept(void) {
  if (!s_kb_box) return false;
  owf_kb_close();
  return true;                            // BOOT closed the keyboard, not the app
}

static void kb_commit(void) {
  if (!s_kb_ta) return;
  char text[OWF_KB_TEXT_MAX];
  strncpy(text, lv_textarea_get_text(s_kb_ta), sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  owf_kb_done_fn fn = s_kb_done;
  void *user = s_kb_user;
  owf_kb_close();                         // closed before the callback: the callback
  if (fn) fn(text, user);                 // is free to rebuild the whole screen
}

static void kb_key_cb(lv_event_t *e) {
  if (!s_kb_mtx || !s_kb_ta) return;
  uint32_t id = lv_buttonmatrix_get_selected_button(s_kb_mtx);
  const char *t = lv_buttonmatrix_get_button_text(s_kb_mtx, id);
  if (!t) return;

  if (!strcmp(t, LV_SYMBOL_UP))        { kb_apply_map(1); return; }
  if (!strcmp(t, LV_SYMBOL_DOWN))      { kb_apply_map(0); return; }
  if (!strcmp(t, "?123"))              { kb_apply_map(2); return; }
  if (!strcmp(t, "=\\<"))              { kb_apply_map(3); return; }
  if (!strcmp(t, "123"))               { kb_apply_map(2); return; }
  if (!strcmp(t, "abc"))               { kb_apply_map(0); return; }
  if (!strcmp(t, LV_SYMBOL_BACKSPACE)) { lv_textarea_delete_char(s_kb_ta); return; }
  if (!strcmp(t, LV_SYMBOL_OK))        { kb_commit(); return; }
  lv_textarea_add_text(s_kb_ta, t);
}
static void kb_cancel_cb(lv_event_t *e) { (void)e; owf_kb_close(); }
static void kb_ta_ready_cb(lv_event_t *e) { (void)e; kb_commit(); }

/* Open the keyboard over the current sub-app screen. */
static void owf_kb_open(const char *title, const char *initial, uint16_t max_len,
                        owf_kb_done_fn done, void *user) {
  if (!app_scr) return;
  owf_kb_close();                         // only ever one
  s_kb_done = done;
  s_kb_user = user;

  s_kb_box = lv_obj_create(app_scr);
  lv_obj_add_event_cb(s_kb_box, kb_deleted_cb, LV_EVENT_DELETE, nullptr);
  lv_obj_set_size(s_kb_box, LV_PCT(100), LV_PCT(100));
  lv_obj_center(s_kb_box);
  lv_obj_set_style_bg_color(s_kb_box, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_kb_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_kb_box, 0, 0);
  lv_obj_set_style_pad_all(s_kb_box, 0, 0);
  lv_obj_set_style_radius(s_kb_box, 0, 0);
  lv_obj_clear_flag(s_kb_box, LV_OBJ_FLAG_SCROLLABLE);

  /* BOOT closes the keyboard first; the screen's own hook is put back after. */
  s_kb_prev_back = nav_back_intercept;
  nav_back_intercept = kb_back_intercept;

  /* ---- layout order: KEYS FIRST, then upward ------------------------------
   * The first cut placed the title and the entry field at predicted y values and
   * put the keys at a fixed percentage of the screen. On the C2 the prediction
   * for the field's height was too small and the keyboard covered it completely.
   *
   * The lesson is the same one the Cancel X taught: do not predict what LVGL is
   * going to measure. So the key matrix is built and positioned first, then the
   * layout is flushed and every row above it is placed against MEASURED
   * geometry -- the field sits directly above the real top edge of the keys, the
   * title directly above the real top of the field. Font metrics, padding and
   * per-board scaling can then be whatever they are without anything colliding.
   */
  s_kb_mtx = lv_buttonmatrix_create(s_kb_box);
  lv_obj_set_size(s_kb_mtx,
                  LV_PCT(BOARD_SCREEN_ROUND ? 94 : 100),
                  LV_PCT(BOARD_SCREEN_ROUND ? 54 : (BOARD_SCREEN_NARROW ? 62 : 58)));
  lv_obj_align(s_kb_mtx, LV_ALIGN_BOTTOM_MID, 0,
               BOARD_SCREEN_ROUND ? UI_PX(-22) : 0);
  lv_obj_set_style_bg_opa(s_kb_mtx, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_kb_mtx, 0, 0);
  lv_obj_set_style_pad_all(s_kb_mtx, UI_PX(2), 0);
  lv_obj_set_style_pad_row(s_kb_mtx, UI_PX(3), 0);
  lv_obj_set_style_pad_column(s_kb_mtx, UI_PX(3), 0);
  /* Key face + label. montserrat_14 on the slim panels: the scaled body font is
   * wider than a 17 px key there and every letter would render as a dot. */
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(s_kb_mtx, &lv_font_montserrat_14, LV_PART_ITEMS);
#else
  lv_obj_set_style_text_font(s_kb_mtx, &FONT_SMALL, LV_PART_ITEMS);
#endif
  lv_obj_set_style_bg_color(s_kb_mtx, lv_color_hex(0x262626), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(s_kb_mtx, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_kb_mtx, lv_color_white(), LV_PART_ITEMS);
  lv_obj_set_style_radius(s_kb_mtx, UI_PX(8), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(s_kb_mtx, lv_color_hex(0x4A4A4A),
                            (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_add_event_cb(s_kb_mtx, kb_key_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  kb_apply_map(0);                                 /* keys in place before measuring */
  lv_obj_update_layout(s_kb_box);
  const int kb_top = lv_obj_get_y(s_kb_mtx);       /* measured, not assumed */
  const int kb_cx  = (int)screenWidth / 2;

  /* ---- entry field: directly above the keys ---- */
  s_kb_ta = lv_textarea_create(s_kb_box);
  lv_textarea_set_one_line(s_kb_ta, true);
  if (max_len == 0 || max_len > OWF_KB_TEXT_MAX) max_len = OWF_KB_TEXT_MAX;
  lv_textarea_set_max_length(s_kb_ta, max_len - 1);
  lv_textarea_set_placeholder_text(s_kb_ta, "password");
  if (initial && *initial) lv_textarea_set_text(s_kb_ta, initial);
  lv_obj_set_style_text_font(s_kb_ta, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(s_kb_ta, lv_color_white(), 0);
  lv_obj_set_style_bg_color(s_kb_ta, lv_color_hex(0x101010), 0);
  lv_obj_set_style_border_width(s_kb_ta, 1, 0);
  lv_obj_set_style_border_color(s_kb_ta, lv_color_hex(0x404040), 0);
  lv_obj_add_event_cb(s_kb_ta, kb_ta_ready_cb, LV_EVENT_READY, nullptr);
#if BOARD_SCREEN_ROUND
  lv_obj_set_width(s_kb_ta, 2 * kb_half_w(kb_top) - UI_PX(20));
#else
  lv_obj_set_width(s_kb_ta, LV_PCT(92));
#endif
  lv_obj_update_layout(s_kb_box);
  int ta_y = kb_top - UI_PX(8) - lv_obj_get_height(s_kb_ta);
  if (ta_y < 0) ta_y = 0;
  lv_obj_set_pos(s_kb_ta, kb_cx - lv_obj_get_width(s_kb_ta) / 2, ta_y);

  /* ---- title row: above the field, sharing its row with the Cancel X ----
   * On a round panel the X cannot live in the corner (the corner is not there),
   * and it cannot have its own row above the title either -- that row is where
   * the circle is narrowest AND the space is now whatever the keyboard left. So
   * the two share a row, out at the chord the row actually has. */
  lv_obj_t *q = lv_label_create(s_kb_box);
  lv_obj_set_style_text_font(q, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(q, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(q, LV_LABEL_LONG_DOT);
  lv_label_set_text(q, title ? title : "");

  lv_obj_t *x = lv_btn_create(s_kb_box);
  lv_obj_set_size(x, UI_PX(56), UI_PX(44));
  lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(x, 0, 0);
  lv_obj_add_event_cb(x, kb_cancel_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *xl = lv_label_create(x);
  lv_obj_set_style_text_font(xl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(xl, lv_color_hex(0x999999), 0);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE);
  lv_obj_center(xl);

#if BOARD_SCREEN_ROUND
  { const int xw = UI_PX(56), xh = UI_PX(44);
    int row_y = ta_y - UI_PX(6) - xh;                  /* the X is the taller of the two */
    if (row_y < 0) row_y = 0;
    /* Chord at the row's TOP edge: the narrowest line it covers up here. */
    const int hw = kb_half_w(row_y);
    const int x_left = kb_cx + hw - UI_PX(6) - xw;
    lv_obj_set_pos(x, x_left, row_y);
    /* Title fills what is left of the chord to the left of the X. */
    int t_left  = kb_cx - hw + UI_PX(6);
    int t_w     = x_left - UI_PX(8) - t_left;
    if (t_w < UI_PX(80)) t_w = UI_PX(80);
    lv_obj_set_width(q, t_w);
    lv_obj_update_layout(s_kb_box);
    int t_y = row_y + (xh - lv_obj_get_height(q)) / 2;  /* centred on the X */
    if (t_y < 0) t_y = 0;
    lv_obj_set_pos(q, t_left, t_y); }
#else
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, UI_PX(-6), UI_PX(6));
  lv_obj_set_width(q, LV_PCT(88));
  lv_obj_update_layout(s_kb_box);
  { int t_y = ta_y - UI_PX(6) - lv_obj_get_height(q);
    if (t_y < UI_PX(6)) t_y = UI_PX(6);
    lv_obj_set_pos(q, kb_cx - lv_obj_get_width(q) / 2, t_y); }
#endif

}

#endif  /* OWF_HAS_OSK */
