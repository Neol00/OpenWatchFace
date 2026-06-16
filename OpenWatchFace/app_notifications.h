/* ============================================================================
 *  app_notifications.h — Notifications sub-app (stored-history list + reader).
 *
 *  Split out of app_menu.h to keep each sub-app self-contained as the UI grows.
 *  This is a header-only module compiled into the single .ino translation unit,
 *  so it shares the same statics as the rest of the menu code — it just has to be
 *  INCLUDED AFTER:
 *    - app_menu.h          (screen shell app_scr / app_screen_begin + nav stack)
 *    - notif_store.h       (the flash store: s_notifs / s_notif_count / ...)
 *    - notif_archive_sd.h  (the SD archive: s_na_view / na_load_view / ...)
 *    - watch_base.h        (store_lock / store_unlock) and the FONT_* macros.
 *
 *  Two sources, picked at open time:
 *    - SD ARCHIVE when a card holds entries  -> the FULL history (newest-first),
 *      bigger bodies, PAGED at NA_PAGE_SIZE (100) records per page with Newer/Older
 *      buttons so the in-memory view (and the rendered row count) never exceeds one
 *      page.
 *    - FLASH STORE otherwise                 -> the newest 32 (always one page).
 *  The list is vertical; tapping a row opens it FULL-SCREEN so a long message can
 *  be read/scrolled; BOOT returns to the list (on the SAME page). Each row has an X
 *  to dismiss just that one (removed from BOTH stores); "Clear" empties everything.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

static void app_open_notifications(void);    // fwd (rebuilt after each dismiss)
static void app_open_notif_detail(void);     // fwd (full-screen reader; screen_fn)

/* ANCS two-way dismiss (defined in ble_ancs.h, included later in the .ino). Lets a
 * watch-side dismiss also clear the notification on the iPhone. Best-effort: no-ops
 * when disconnected / no live UID. Forward-declared so the dismiss handlers below
 * can call them regardless of include order. */
static void ancs_dismiss_id(uint64_t id);
static void ancs_dismiss_all(void);

/* Screen rebuilds MUST be deferred out of the click event. A row/X/Clear handler
 * runs as an event on a child of app_scr; rebuilding (open detail, or rebuild the
 * list after a dismiss) calls app_screen_begin(), which does lv_obj_del(app_scr) —
 * deleting the object tree whose event is still being dispatched. Doing that inside
 * the event is LVGL undefined behavior (stale input/render state -> e.g. the reader
 * showing the wrong item). lv_async_call runs these in the next lv_timer_handler,
 * after the event has fully unwound and the tree is stable. */
static void notif_open_detail_async(void *p) { (void)p; nav_open(app_open_notif_detail); }
static void notif_rebuild_async(void *p) {
  (void)p;
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  app_open_notifications();   // in-place rebuild (nav stack untouched)
  watchface_refresh_bell();   // a dismiss/clear may have changed the unread count
}

/* Per-category icons + tints live in notif_icons.h (MDI font, built-in fallback);
 * use notif_icon_apply() / notif_icon_color() from there. */
static bool notif_cat_is_call(uint8_t cat) { return cat == NCAT_CALL; }

/* Which store the currently-shown list came from (decided in app_open_notifications). */
enum NotifSrc { NSRC_FLASH, NSRC_SD };
static NotifSrc s_notif_src = NSRC_FLASH;

/* Current page of the SD-archive list (0 = newest NA_PAGE_SIZE records). Only the
 * SD source pages; the flash store (<= 32) is always a single page. Reset to 0 each
 * time the app is opened fresh (see nav into app_open_notifications), preserved
 * across in-place rebuilds (dismiss) so a dismiss doesn't bounce you to page 0. */
static uint16_t s_notif_page = 0;
static void notif_reset_page(void) { s_notif_page = 0; }   // called on a fresh open from the menu

/* The reader renders from these buffers, captured at tap time, so it never has to
 * touch the (cross-core) stores while building — and it works the same whether the
 * item came from flash or SD. Sized for the larger SD limits. */
static char    s_detail_title[NA_TITLE_MAX];
static char    s_detail_body[NA_BODY_MAX];
static uint8_t s_detail_cat = NCAT_GENERIC;   // category of the item open in the reader

/* Callbacks carry the ROW INDEX (small int) in user_data, not the id: ids are full
 * 64-bit values (from strtoull) and uintptr_t is only 32-bit on the ESP32-S3, so
 * passing an id through user_data would truncate it. The list is rebuilt on every
 * change, so the index is valid at click time; we resolve it under store_lock. */
static void notif_entry_cb(lv_event_t *e) {
  uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  uint64_t dismissed_id = 0;             // captured so we can also clear it on the phone
  store_lock();                          // shared with the core-0 network task (and SD)
  if (s_notif_src == NSRC_SD) {
    if (idx < s_na_view_count) {
      uint64_t id = s_na_view[idx].id;
      dismissed_id = id;
      na_remove(id);                     // drop from the SD archive
      for (uint8_t i = 0; i < s_notif_count; i++)   // and from the flash cache if mirrored
        if (s_notifs[i].id == id) { notif_store_remove(i); break; }
    }
  } else {
    if (idx < s_notif_count) { dismissed_id = s_notifs[idx].id; notif_store_remove((uint8_t)idx); }
  }
  store_unlock();
  // Two-way sync: also dismiss it on the iPhone (best-effort; no-op if disconnected
  // or it's an iPhone-side item from a previous session whose live UID we lack).
  if (dismissed_id) ancs_dismiss_id(dismissed_id);
  lv_async_call(notif_rebuild_async, nullptr);   // rebuild AFTER the event unwinds
}

static void notif_clear_all_cb(lv_event_t *e) {
  (void)e;
  // Two-way sync: dismiss everything we know a live UID for on the iPhone too. Do
  // this BEFORE wiping the local store (it reads the session UID map, not the store,
  // so order doesn't strictly matter, but it's clearer). Best-effort if connected.
  ancs_dismiss_all();
  store_lock();
  notif_store_clear();
  na_clear();                  // wipe the SD archive too (no-op without a card)
  store_unlock();
  s_notif_page = 0;            // archive is empty now -> back to the first page
  lv_async_call(notif_rebuild_async, nullptr);
}

/* Page flip: -1 (newer/Prev) or +1 (older/Next). The list rebuild (which reloads
 * the page under the lock) is deferred like every other rebuild so it runs after
 * the click event unwinds. user_data carries the direction. */
static void notif_page_cb(lv_event_t *e) {
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  if (dir < 0 && s_notif_page > 0) s_notif_page--;
  else if (dir > 0)                s_notif_page++;   // app_open_notifications clamps overshoot
  lv_async_call(notif_rebuild_async, nullptr);
}

/* Tap a row (anywhere but the X) -> capture its full text and open the reader. */
static void notif_open_cb(lv_event_t *e) {
  uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  store_lock();
  s_detail_cat = NCAT_GENERIC;
  if (s_notif_src == NSRC_SD) {
    if (idx < s_na_view_count) {
      uint64_t id = s_na_view[idx].id;
      s_detail_cat = s_na_view[idx].cat;
      strncpy(s_detail_title, s_na_view[idx].title, sizeof(s_detail_title) - 1);
      s_detail_title[sizeof(s_detail_title) - 1] = '\0';
      na_read_body(id, s_detail_body, sizeof(s_detail_body));  // full body from SD
      if (!s_na_view[idx].read) {        // skip the O(n) file rewrite if already read
        na_mark_read(id);                // opening it clears it from the bell count
        s_na_view[idx].read = true;      // keep the in-memory view in sync
        for (uint8_t i = 0; i < s_notif_count; i++)   // mirror in the flash cache if present
          if (s_notifs[i].id == id) { notif_store_mark_read(id); break; }
      }
    } else { s_detail_title[0] = '\0'; s_detail_body[0] = '\0'; }
  } else {
    if (idx < s_notif_count) {
      s_detail_cat = s_notifs[idx].cat;
      strncpy(s_detail_title, s_notifs[idx].title, sizeof(s_detail_title) - 1);
      s_detail_title[sizeof(s_detail_title) - 1] = '\0';
      strncpy(s_detail_body, s_notifs[idx].body, sizeof(s_detail_body) - 1);
      s_detail_body[sizeof(s_detail_body) - 1] = '\0';
      notif_store_mark_read(s_notifs[idx].id);   // opening it clears it from the bell count
    } else { s_detail_title[0] = '\0'; s_detail_body[0] = '\0'; }
  }
  store_unlock();
  lv_async_call(notif_open_detail_async, nullptr);   // open AFTER the event unwinds
}

/* Add one notification row to the scrollable list. The row body is tappable
 * (opens the reader); the small X on the right dismisses just that one.
 * `idx` is the row's position in the current source list. `unread` draws an
 * accent-colored dot on the left and reserves a little left margin for it. */
static void notif_add_row(lv_obj_t *list, uint16_t idx, const char *title,
                          const char *body, bool unread, uint8_t cat) {
  lv_obj_t *row = lv_obj_create(list);    // plain container, made clickable below
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, 330);
  // Fixed height: roomy enough for a two-line title. The list shows the TITLE ONLY
  // (the body preview used to sit under it and overlap on short rows); the title now
  // gets the whole row and may wrap to two lines. Full body is shown in the reader.
  // Height also gives the X button a defined box to center against.
  lv_obj_set_height(row, 64);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 12, 0);
  lv_obj_set_style_pad_all(row, 12, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  // Make the whole row a tap target that opens the reader. The X button below is
  // a separate button and consumes its own clicks (events don't bubble unless
  // asked), so tapping X dismisses while tapping anywhere else opens.
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, notif_open_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

  // Left: a per-app category icon (call/message/mail/...), tinted by category. The
  // text starts to its right. An unread row also gets a small accent dot tucked at
  // the icon's top-left corner as a badge, so "unread" and "which app" are both shown.
  const int icon_w = 30;                  // reserved left column for the glyph
  const int text_x = icon_w + 6;
  lv_obj_t *ic = lv_label_create(row);
  notif_icon_apply(ic, cat);              // sets MDI glyph (or fallback symbol) + tint
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

  if (unread) {
    const int dot_d = 9;
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, dot_d, dot_d);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, icon_w - 6, -12);  // badge at the icon's upper-right
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(ui_accent_hex()), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  }

  // Text takes the middle; the X button sits on the right.
  const int text_w = 270 - text_x;

  // Title only — vertically centered, wrapping to (up to) two lines in the tall row.
  // The body preview was removed (it overlapped the title); the reader shows it in full.
  (void)body;
  lv_obj_t *t = lv_label_create(row);
  lv_obj_set_style_text_font(t, &FONT_SMALL, 0);
  // Read titles dim slightly so unread ones stand out beyond just the dot.
  lv_obj_set_style_text_color(t, unread ? lv_color_white() : lv_color_hex(0xBBBBBB), 0);
  lv_label_set_text(t, title);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, text_x, 0);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);   // long title truncates with "…" on line 2
  lv_obj_set_width(t, text_w);

  lv_obj_t *x = lv_btn_create(row);
  lv_obj_set_size(x, 36, 36);
  lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(x, lv_color_hex(0x2A2A2A), 0);   // neutral dark; glyph carries the accent
  lv_obj_set_style_radius(x, 18, 0);     // round
  lv_obj_add_event_cb(x, notif_entry_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
  lv_obj_t *xl = lv_label_create(x);
  lv_obj_set_style_text_font(xl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(xl, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE);
  lv_obj_center(xl);
}

/* ----------------------------- Reader (full-screen) -----------------------
 * Renders the title + body captured at tap time. The body wraps and the whole
 * column scrolls vertically, so an arbitrarily long message is fully readable.
 * BOOT (nav_back) returns to the list. The screen shell draws the "<- BOOT" hint
 * top-left; we keep the content clear of it. */
static void app_open_notif_detail(void) {
  app_screen_begin("");   // no header text; just the "<- BOOT" hint top-left

  // Scrollable reader column. No title header, so it starts just below the BOOT
  // hint band (which the shell draws at the very top-left) and fills the screen.
  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_size(col, 372, 440);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 14, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(col);  // narrow-panel: shift scrollbar toward the edge (no-op on S3)
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(col, 12, 0);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);   // vertical scroll only

  // CALL reader: a phone notification has no real "message" — iOS often sends an
  // empty or repetitive body, which the generic wrapped reader renders as an ugly
  // list. So render calls as a compact card: big call glyph + title, centered, no
  // body block. Everything else uses the normal title + scrollable body.
  if (notif_cat_is_call(s_detail_cat)) {
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(col, 60, 0);

    lv_obj_t *ic = lv_label_create(col);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_40, 0);   // built-in for a crisp large glyph
    lv_obj_set_style_text_color(ic, lv_color_hex(notif_icon_color(NCAT_CALL)), 0);
    lv_label_set_text(ic, LV_SYMBOL_CALL);

    lv_obj_t *th = lv_label_create(col);
    lv_obj_set_style_text_font(th, &FONT_LABEL, 0);
    lv_obj_set_style_text_color(th, lv_color_white(), 0);
    lv_obj_set_width(th, LV_PCT(100));
    lv_obj_set_style_text_align(th, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(th, LV_LABEL_LONG_WRAP);
    lv_label_set_text(th, s_detail_title[0] ? s_detail_title : "Missed Call");
    return;
  }

  lv_obj_t *th = lv_label_create(col);
  lv_obj_set_style_text_font(th, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(th, lv_color_white(), 0);
  lv_obj_set_width(th, LV_PCT(100));
  lv_label_set_long_mode(th, LV_LABEL_LONG_WRAP);
  lv_label_set_text(th, s_detail_title[0] ? s_detail_title : "(no title)");

  lv_obj_t *bd = lv_label_create(col);
  lv_obj_set_style_text_font(bd, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(bd, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_width(bd, LV_PCT(100));
  lv_label_set_long_mode(bd, LV_LABEL_LONG_WRAP);
  lv_label_set_text(bd, s_detail_body[0] ? s_detail_body : "(no content)");
}

/* ----------------------------- List screen -------------------------------- */
static void app_open_notifications(void) {
  app_screen_begin("Notifications");

  // Pick the source by card presence: with a card we read the SD archive (the
  // FULL, untruncated history, ONE page at a time — flash only mirrors the newest
  // 32). Without a card we read the flash store (always a single page). na_load_view
  // touches the card, so do it under the lock that serializes with the core-0 appender.
  store_lock();
  s_notif_src = na_available() ? NSRC_SD : NSRC_FLASH;
  uint16_t pages = 1;
  if (s_notif_src == NSRC_SD) {
    na_load_view(s_notif_page);          // sets s_na_total/s_na_unread; loads this page
    pages = na_page_count();
    // If a dismiss/clear shrank the archive so this page is now past the end, the
    // load came back empty — clamp to the new last page and load it once more.
    if (s_notif_page >= pages) { s_notif_page = pages - 1; na_load_view(s_notif_page); }
  }
  uint16_t count = (s_notif_src == NSRC_SD) ? s_na_view_count : s_notif_count;
  uint32_t total = (s_notif_src == NSRC_SD) ? s_na_total : s_notif_count;
  store_unlock();

  if (total == 0) {
    lv_obj_t *empty = lv_label_create(app_scr);
    lv_obj_set_style_text_font(empty, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
    lv_label_set_text(empty, "No notifications");
    lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    return;
  }

  bool multipage = (s_notif_src == NSRC_SD) && (pages > 1);

  // Scrollable list area, from below the title down to just above the bottom bar.
  // When paged, leave extra room at the bottom for the Prev/Next row.
  lv_obj_t *list = lv_obj_create(app_scr);
  lv_obj_set_size(list, 360, multipage ? 280 : 320);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 90);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(list);  // narrow-panel: shift scrollbar toward the edge (no-op on S3)
  lv_obj_set_style_pad_row(list, 10, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);   // vertical list — no sideways scroll

  // Rows — newest first, tap to read full-screen, X to dismiss. `i` is the row's
  // index INTO THE CURRENT PAGE (s_na_view), which is what the open/dismiss
  // callbacks resolve against, so paging needs no change to those callbacks.
  for (uint16_t i = 0; i < count; i++) {
    if (s_notif_src == NSRC_SD)
      notif_add_row(list, i, s_na_view[i].title, s_na_view[i].preview, !s_na_view[i].read, s_na_view[i].cat);
    else
      notif_add_row(list, i, s_notifs[i].title, s_notifs[i].body, !s_notifs[i].read, s_notifs[i].cat);
  }

  // Pager: "Prev   Page X/Y   Next", just above the Clear button. Prev is hidden
  // on the first page, Next on the last, so you can't flip past the ends.
  if (multipage) {
    lv_obj_t *pager = lv_obj_create(app_scr);
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, 360, 44);
    lv_obj_align(pager, LV_ALIGN_BOTTOM_MID, 0, -84);
    lv_obj_clear_flag(pager, LV_OBJ_FLAG_SCROLLABLE);

    if (s_notif_page > 0) {
      lv_obj_t *prev = lv_btn_create(pager);
      lv_obj_set_size(prev, 96, 44);
      lv_obj_align(prev, LV_ALIGN_LEFT_MID, 0, 0);
      lv_obj_set_style_bg_color(prev, lv_color_hex(0x1A2A1A), 0);
      lv_obj_set_style_radius(prev, 12, 0);
      lv_obj_add_event_cb(prev, notif_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
      lv_obj_t *pl = lv_label_create(prev);
      lv_obj_set_style_text_font(pl, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(pl, lv_color_hex(ui_deco_hex(0xAAFFAA)), 0);
      lv_label_set_text(pl, LV_SYMBOL_LEFT " Newer");
      lv_obj_center(pl);
    }

    lv_obj_t *ind = lv_label_create(pager);
    lv_obj_set_style_text_font(ind, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(ind, lv_color_hex(0x888888), 0);
    lv_label_set_text_fmt(ind, "%u/%u", (unsigned)(s_notif_page + 1), (unsigned)pages);
    lv_obj_align(ind, LV_ALIGN_CENTER, 0, 0);

    if (s_notif_page + 1 < pages) {
      lv_obj_t *next = lv_btn_create(pager);
      lv_obj_set_size(next, 96, 44);
      lv_obj_align(next, LV_ALIGN_RIGHT_MID, 0, 0);
      lv_obj_set_style_bg_color(next, lv_color_hex(0x1A2A1A), 0);
      lv_obj_set_style_radius(next, 12, 0);
      lv_obj_add_event_cb(next, notif_page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
      lv_obj_t *nl = lv_label_create(next);
      lv_obj_set_style_text_font(nl, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(nl, lv_color_hex(0xAAFFAA), 0);
      lv_label_set_text(nl, "Older " LV_SYMBOL_RIGHT);
      lv_obj_center(nl);
    }
  }

  // "Clear all" button, anchored at the bottom (where the Back button used to be).
  lv_obj_t *clr = lv_btn_create(app_scr);
  lv_obj_set_size(clr, 160, 48);
  lv_obj_align(clr, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_set_style_bg_color(clr, lv_color_hex(0x2A2A2A), 0);   // neutral dark; text carries the accent
  lv_obj_set_style_radius(clr, 12, 0);
  lv_obj_add_event_cb(clr, notif_clear_all_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *cl = lv_label_create(clr);
  lv_obj_set_style_text_font(cl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(cl, lv_color_hex(ui_accent_hex()), 0);
  lv_label_set_text(cl, LV_SYMBOL_TRASH "  Clear");
  lv_obj_center(cl);
}
