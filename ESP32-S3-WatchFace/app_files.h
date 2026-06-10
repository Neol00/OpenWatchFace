/* ============================================================================
 *  app_files.h — "Files" sub-app: a small on-device file manager for developers.
 *
 *  Browses the two filesystems this board exposes, presented at the root as two
 *  folders:
 *    - Flash (FFat)  : the ~24 MB on-flash FAT partition (always present). This is
 *                      where the notification archive / logs live with no SD card.
 *    - SD Card       : the microSD card, when one is mounted (hidden otherwise).
 *  Inside a volume you can walk directories, see each file's size, VIEW small text
 *  files on-screen (read-only, truncated), and DELETE a file (with a confirm). A
 *  header shows used / total space for the current volume.
 *
 *  Header-only, compiled into the .ino TU, sharing the menu statics. INCLUDE AFTER
 *  app_menu.h (screen shell + nav_open/back), sd_card.h (SD_MMC + sd_mount) and
 *  notif_archive_sd.h (which includes FFat + mounts it via ffat_mount). Registered
 *  as a menu tile in app_menu.h (app_open_files forward-declared there).
 *
 *  THREADING: everything here runs on the loop/LVGL thread. Filesystem reads are
 *  synchronous and small (a directory listing, an <=8 KB text head), so they don't
 *  noticeably stall the UI. We never touch the SD/FFat objects off this thread.
 * ========================================================================== */
#pragma once
#include <lvgl.h>
#include <FS.h>
#include <FFat.h>
#include <SD_MMC.h>
#include "esp_heap_caps.h"   // heap_caps_malloc — read big files into PSRAM, not SRAM

/* ---- navigation state (which volume + directory we're looking at) ----
 * s_fm_vol: -1 = at the volume-chooser root (list Flash + SD); 0 = Flash; 1 = SD.
 * s_fm_path: the current directory WITHIN that volume, always starting with '/'.
 * A capture buffer holds the file picked for view/delete so the deferred rebuild
 * (after a delete) and the viewer screen don't read freed list state. */
static int  s_fm_vol = -1;
static char s_fm_path[256] = "/";
static char s_fm_view_path[256] = "";   // file currently open in the viewer
static char s_fm_pending_del[256] = ""; // file awaiting delete confirmation

static void app_open_files(void);            // fwd (rebuilt after a delete)
static void app_open_file_view(void);        // fwd (full-screen text viewer; screen_fn)

/* The fs::FS for the active volume. Only valid when s_fm_vol is 0 or 1. */
static fs::FS &fm_fs(void) {
  return (s_fm_vol == 1) ? (fs::FS &)SD_MMC : (fs::FS &)FFat;
}

/* Human-readable byte size into `out` (e.g. "1.4 MB", "812 B"). */
static void fm_fmt_size(uint64_t b, char *out, size_t n) {
  if (b < 1024ULL)            snprintf(out, n, "%u B", (unsigned)b);
  else if (b < 1024ULL*1024)  snprintf(out, n, "%.1f KB", b / 1024.0);
  else if (b < 1024ULL*1024*1024) snprintf(out, n, "%.1f MB", b / (1024.0*1024));
  else                        snprintf(out, n, "%.1f GB", b / (1024.0*1024*1024));
}

/* Is this filename one we'll let the viewer open as text? (Small allowlist by
 * extension; everything else is shown as a non-viewable file with size only.) */
static bool fm_is_texty(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) return false;
  const char *ext = dot + 1;
  return !strcasecmp(ext, "txt") || !strcasecmp(ext, "csv") ||
         !strcasecmp(ext, "log") || !strcasecmp(ext, "json") ||
         !strcasecmp(ext, "ini") || !strcasecmp(ext, "md") ||
         !strcasecmp(ext, "cfg") || !strcasecmp(ext, "conf");
}

/* Deferred rebuild (same LVGL-UB-avoidance as the notifications app: never delete
 * app_scr from inside the click event that triggered the rebuild). */
static void fm_rebuild_async(void *p) {
  (void)p;
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  app_open_files();
}
static void fm_open_view_async(void *p) { (void)p; nav_open(app_open_file_view); }

/* Join s_fm_path + child into dst, collapsing the root so we never get "//x". */
static void fm_join(char *dst, size_t n, const char *dir, const char *child) {
  if (!strcmp(dir, "/")) snprintf(dst, n, "/%s", child);
  else                   snprintf(dst, n, "%s/%s", dir, child);
}

/* Drop the last path component of s_fm_path (go up one directory). At a volume root
 * ("/") there's nothing to drop -> caller handles leaving the volume. */
static void fm_path_up(void) {
  char *slash = strrchr(s_fm_path, '/');
  if (!slash) { strcpy(s_fm_path, "/"); return; }
  if (slash == s_fm_path) s_fm_path[1] = '\0';   // keep the leading '/'
  else                    *slash = '\0';
}

/* ---- delete confirm ---- */
static void fm_del_confirm_cb(lv_event_t *e) {
  (void)e;
  if (s_fm_pending_del[0]) {
    fm_fs().remove(s_fm_pending_del);
    s_fm_pending_del[0] = '\0';
  }
  lv_async_call(fm_rebuild_async, nullptr);   // back to the listing, minus the file
}
static void fm_del_cancel_cb(lv_event_t *e) {
  (void)e;
  s_fm_pending_del[0] = '\0';
  lv_async_call(fm_rebuild_async, nullptr);
}

/* ---- index-based entry resolution (NO per-row heap allocation) ----
 * Rows carry only their ordinal position (an int in user_data). At click time we
 * re-open the current directory and walk to that index to get the entry's name —
 * exactly the order app_open_files() listed them in (same dir, same enumeration), so
 * the index is stable for the life of the on-screen list. This removes every strdup/
 * free + DELETE-callback the rows used to need (the source of the heap corruption).
 * Returns false if the index is out of range. */
static bool fm_name_at_index(int idx, char *out, size_t n, bool *isdir_out) {
  File dir = fm_fs().open(s_fm_path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }
  int i = 0;
  bool found = false;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (i == idx) {
      const char *full = entry.name();
      const char *base = strrchr(full, '/');
      base = base ? base + 1 : full;
      strncpy(out, base, n - 1);
      out[n - 1] = '\0';
      if (isdir_out) *isdir_out = entry.isDirectory();
      entry.close();
      found = true;
      break;
    }
    entry.close();
    i++;
  }
  dir.close();
  return found;
}

/* The X on a file row: resolve the row index to a path and rebuild into a confirm. */
static void fm_del_request_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  char name[128];
  if (!fm_name_at_index(idx, name, sizeof(name), nullptr)) return;
  fm_join(s_fm_pending_del, sizeof(s_fm_pending_del), s_fm_path, name);
  lv_async_call(fm_rebuild_async, nullptr);
}

/* ---- row tap handlers ---- */

/* Tap the volume-chooser entries at the root. */
static void fm_pick_vol_cb(lv_event_t *e) {
  int vol = (int)(intptr_t)lv_event_get_user_data(e);
  // Make sure the chosen volume is actually mounted before we browse it: FFat mounts
  // lazily (ffat_mount, from storage_fs.h) and may not be up yet if no archive
  // write has happened this boot; SD is mounted via sd_mount(). Bail if either fails.
  if (vol == 0) { if (!ffat_mount()) return; }
  else          { if (!sd_mount())   return; }
  s_fm_vol = vol;
  strcpy(s_fm_path, "/");
  lv_async_call(fm_rebuild_async, nullptr);
}

/* Tap a directory row: resolve the index to a name and descend into it. */
static void fm_enter_dir_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  char name[128];
  if (!fm_name_at_index(idx, name, sizeof(name), nullptr)) return;
  char next[256];
  fm_join(next, sizeof(next), s_fm_path, name);
  strncpy(s_fm_path, next, sizeof(s_fm_path) - 1);
  s_fm_path[sizeof(s_fm_path) - 1] = '\0';
  lv_async_call(fm_rebuild_async, nullptr);
}

/* Tap a viewable text file: resolve the index to its full path and open the viewer. */
static void fm_open_file_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  char name[128];
  if (!fm_name_at_index(idx, name, sizeof(name), nullptr)) return;
  fm_join(s_fm_view_path, sizeof(s_fm_view_path), s_fm_path, name);
  lv_async_call(fm_open_view_async, nullptr);
}

/* The "up / back" row: leave the current dir, or the volume if at its root. */
static void fm_up_cb(lv_event_t *e) {
  (void)e;
  if (!strcmp(s_fm_path, "/")) s_fm_vol = -1;   // at volume root -> back to chooser
  else                         fm_path_up();
  lv_async_call(fm_rebuild_async, nullptr);
}

/* ---- list-row builders (match the notifications app's row look) ---- */

/* One tappable row: an icon, a name, an optional right-aligned size, and (for
 * deletable files) an X. `tap_ud` is a plain integer index (cast through void*); NO
 * heap allocation is involved, so rows need no free/DELETE bookkeeping. Pass nullptr
 * tap for static rows (the SD "(none)" placeholder). */
static lv_obj_t *fm_row(lv_obj_t *list, const char *symbol, uint32_t icon_rgb,
                        const char *name, const char *size, lv_event_cb_t tap,
                        void *tap_ud) {
  lv_obj_t *row = lv_obj_create(list);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, 330);
  lv_obj_set_height(row, 52);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 12, 0);
  lv_obj_set_style_pad_all(row, 10, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  if (tap) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, tap, LV_EVENT_CLICKED, tap_ud);
  }

  lv_obj_t *ic = lv_label_create(row);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ic, lv_color_hex(ui_deco_hex(icon_rgb)), 0);
  lv_label_set_text(ic, symbol);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *nm = lv_label_create(row);
  lv_obj_set_style_text_font(nm, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_label_set_text(nm, name);
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nm, size ? 180 : 250);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 34, 0);

  if (size) {
    lv_obj_t *sz = lv_label_create(row);
    lv_obj_set_style_text_font(sz, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(sz, lv_color_hex(0x888888), 0);
    lv_label_set_text(sz, size);
    lv_obj_align(sz, LV_ALIGN_RIGHT_MID, 0, 0);
  }
  return row;
}

/* Add the small round X delete button to a file row (its own click; doesn't bubble).
 * Carries the row's index (no allocation); the handler resolves it to a path. */
static void fm_add_delete_x(lv_obj_t *row, int idx) {
  lv_obj_t *x = lv_btn_create(row);
  lv_obj_set_size(x, 32, 32);
  lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(x, lv_color_hex(0x3A2020), 0);
  lv_obj_set_style_radius(x, 16, 0);
  lv_obj_add_event_cb(x, fm_del_request_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
  lv_obj_t *xl = lv_label_create(x);
  lv_obj_set_style_text_font(xl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(xl, lv_color_hex(0xFF8888), 0);
  lv_label_set_text(xl, LV_SYMBOL_TRASH);
  lv_obj_center(xl);
}

/* ----------------------------- Delete confirm screen --------------------- */
static void fm_build_confirm(void) {
  lv_obj_t *box = lv_obj_create(app_scr);
  lv_obj_set_size(box, 360, 240);
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x202020), 0);
  lv_obj_set_style_radius(box, 16, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *q = lv_label_create(box);
  lv_obj_set_style_text_font(q, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(q, lv_color_white(), 0);
  lv_obj_set_width(q, 320);
  lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
  lv_label_set_text_fmt(q, "Delete this file?\n%s", s_fm_pending_del);
  lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 24);

  lv_obj_t *cancel = lv_btn_create(box);
  lv_obj_set_size(cancel, 150, 52);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 8, -16);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x333333), 0);
  lv_obj_set_style_radius(cancel, 12, 0);
  lv_obj_add_event_cb(cancel, fm_del_cancel_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *cl = lv_label_create(cancel);
  lv_obj_set_style_text_font(cl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(cl, lv_color_white(), 0);
  lv_label_set_text(cl, "Cancel");
  lv_obj_center(cl);

  lv_obj_t *del = lv_btn_create(box);
  lv_obj_set_size(del, 150, 52);
  lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, -8, -16);
  lv_obj_set_style_bg_color(del, lv_color_hex(0x802020), 0);
  lv_obj_set_style_radius(del, 12, 0);
  lv_obj_add_event_cb(del, fm_del_confirm_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *dl = lv_label_create(del);
  lv_obj_set_style_text_font(dl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(dl, lv_color_hex(0xFFB0B0), 0);
  lv_label_set_text(dl, LV_SYMBOL_TRASH "  Delete");
  lv_obj_center(dl);
}

/* ----------------------------- Text viewer (full-screen) ------------------ */
#define FM_VIEW_MAX  8192    // read at most this many bytes (a head, not the whole file)
static void app_open_file_view(void) {
  app_screen_begin("");   // no header; "<- BOOT" hint top-left returns to the listing

  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_size(col, 372, 440);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 12, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 8, 0);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);

  const char *base = strrchr(s_fm_view_path, '/');
  base = base ? base + 1 : s_fm_view_path;
  lv_obj_t *name = lv_label_create(col);
  lv_obj_set_style_text_font(name, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(name, lv_color_hex(ui_accent_soft_hex()), 0);
  lv_obj_set_width(name, LV_PCT(100));
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_label_set_text(name, base);

  // A read-only TEXTAREA (not a plain label): it's built for large, scrollable
  // multi-line text and won't overrun a layout buffer the way an 8 KB LONG_WRAP
  // label can. It owns its own copy of the text (in PSRAM via the LVGL allocator).
  lv_obj_t *body = lv_textarea_create(col);
  lv_obj_set_size(body, LV_PCT(100), 380);
  lv_obj_set_style_text_font(body, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(body, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_textarea_set_cursor_click_pos(body, false);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICK_FOCUSABLE);   // view-only; no keyboard

  File f = fm_fs().open(s_fm_view_path, FILE_READ);
  if (!f) { lv_textarea_set_text(body, "(could not open file)"); return; }
  // Read a head into a PSRAM buffer (the watch has ~7 MB free PSRAM; internal SRAM is
  // tight and was where the heap was corrupting). +1 for the NUL terminator.
  size_t cap = FM_VIEW_MAX;
  char *buf = (char *)heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { f.close(); lv_textarea_set_text(body, "(out of memory)"); return; }
  size_t got = f.read((uint8_t *)buf, cap);
  bool truncated = (f.size() > (uint32_t)got);
  f.close();
  // Sanitize to STRICT 7-bit ASCII before handing the text to LVGL. This is the key
  // crash fix: LVGL's text engine decodes UTF-8, and a multi-byte character split at
  // the 8 KB read boundary (or any malformed/invalid byte from the CSV's escaped,
  // possibly-emoji bodies) makes its decoder read continuation bytes PAST the buffer
  // end — the internal-SRAM heap-tail overrun we were seeing. By replacing every byte
  // >=0x80 (and control bytes except tab/newline) with '.', the string is guaranteed
  // valid single-byte text the decoder can't run off the end of. Plain ASCII content
  // (the common case for these logs) is untouched.
  for (size_t i = 0; i < got; i++) {
    unsigned char c = (unsigned char)buf[i];
    if (c >= 0x80 || (c < 0x20 && c != '\t' && c != '\n' && c != '\r')) buf[i] = '.';
  }
  buf[got] = '\0';
  lv_textarea_set_text(body, got ? buf : "(empty file)");
  heap_caps_free(buf);

  if (truncated) {
    lv_obj_t *more = lv_label_create(col);
    lv_obj_set_style_text_font(more, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(more, lv_color_hex(0x888888), 0);
    lv_label_set_text(more, "--- truncated (showing first 8 KB) ---");
  }
}

/* ----------------------------- Volume free-space header ------------------- */
static void fm_add_space_header(lv_obj_t *parent) {
  uint64_t total = 0, used = 0;
  if (s_fm_vol == 1) { total = SD_MMC.totalBytes(); used = SD_MMC.usedBytes(); }
  else               { total = FFat.totalBytes();   used = FFat.usedBytes();   }

  char u[24], t[24];
  fm_fmt_size(used, u, sizeof(u));
  fm_fmt_size(total, t, sizeof(t));

  lv_obj_t *hdr = lv_label_create(parent);
  lv_obj_set_style_text_font(hdr, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(0x888888), 0);
  lv_label_set_text_fmt(hdr, "%s  |  %s of %s used",
                        s_fm_vol == 1 ? "SD Card" : "Flash (FFat)", u, t);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 56);
}

/* ----------------------------- List screen -------------------------------- */
static void app_open_files(void) {
  app_screen_begin("Files");

  // A pending delete shows the confirm prompt instead of the listing.
  if (s_fm_pending_del[0]) { fm_build_confirm(); return; }

  // The scrollable list area (below the title, room for the bottom bar).
  lv_obj_t *list = lv_obj_create(app_scr);
  lv_obj_set_size(list, 360, s_fm_vol < 0 ? 360 : 330);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, s_fm_vol < 0 ? 90 : 84);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);

  // ---- ROOT: the volume chooser (Flash always; SD when mounted) ----
  if (s_fm_vol < 0) {
    fm_row(list, LV_SYMBOL_DRIVE, 0xFF9F0A, "Flash (FFat)", nullptr,
           fm_pick_vol_cb, (void *)(intptr_t)0);
    if (sd_mount())
      fm_row(list, LV_SYMBOL_SD_CARD, 0x33A0FF, "SD Card", nullptr,
             fm_pick_vol_cb, (void *)(intptr_t)1);
    else
      fm_row(list, LV_SYMBOL_SD_CARD, 0x555555, "SD Card (none)",
             nullptr, nullptr, nullptr);
    return;
  }

  // ---- INSIDE A VOLUME: free-space header + an "up" row + the directory ----
  fm_add_space_header(app_scr);

  fm_row(list, LV_SYMBOL_LEFT, 0xAAAAAA,
         strcmp(s_fm_path, "/") ? ".. (up)" : ".. (volumes)", nullptr,
         fm_up_cb, nullptr);

  File dir = fm_fs().open(s_fm_path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    lv_obj_t *err = lv_label_create(list);
    lv_obj_set_style_text_font(err, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(err, lv_color_hex(0x888888), 0);
    lv_label_set_text(err, "(cannot read directory)");
    return;
  }

  // `idx` is the entry's ordinal in this directory enumeration; the click handlers
  // re-walk the same dir to this index (fm_name_at_index). NO per-row allocation.
  int idx = 0;
  uint16_t shown = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile(), idx++) {
    const char *full = entry.name();                 // may be a full path on some cores
    const char *base = strrchr(full, '/');
    base = base ? base + 1 : full;
    char nameCopy[128];
    strncpy(nameCopy, base, sizeof(nameCopy) - 1);
    nameCopy[sizeof(nameCopy) - 1] = '\0';
    bool isdir = entry.isDirectory();
    uint64_t fsize = isdir ? 0 : entry.size();
    entry.close();

    if (isdir) {
      fm_row(list, LV_SYMBOL_DIRECTORY, 0xFFD60A, nameCopy, nullptr,
             fm_enter_dir_cb, (void *)(intptr_t)idx);
    } else {
      char szbuf[24]; fm_fmt_size(fsize, szbuf, sizeof(szbuf));
      bool texty = fm_is_texty(nameCopy);
      lv_obj_t *row = fm_row(list, LV_SYMBOL_FILE, texty ? 0x32D74B : 0x9090A0,
                             nameCopy, szbuf,
                             texty ? fm_open_file_cb : nullptr,
                             texty ? (void *)(intptr_t)idx : nullptr);
      fm_add_delete_x(row, idx);
    }
    if (++shown >= 200) break;   // safety cap so a huge dir can't build forever
  }
  dir.close();

  if (shown == 0) {
    lv_obj_t *empty = lv_label_create(list);
    lv_obj_set_style_text_font(empty, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
    lv_label_set_text(empty, "(empty folder)");
  }
}
