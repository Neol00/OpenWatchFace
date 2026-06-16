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
 *  app_menu.h (screen shell + nav_open/back), sd_card.h (sd_fs + sd_mount) and
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
#include "sd_card.h"         // board-neutral SD: sd_mount/sd_fs/sd_total_bytes (not SD_MMC)
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

/* ---- MOVE state ----
 * When the user taps a file's move button we remember WHERE the file lives (source
 * volume + full path + its base name) and enter "destination-picker" mode. In that
 * mode the browser is reused to navigate ANY folder on EITHER volume; a "Move here"
 * bar drops the file into the currently-shown directory. Cleared when the move
 * completes or is cancelled. s_fm_move_active != 0 means we're picking a destination. */
static bool s_fm_move_active   = false;       // true while choosing a destination
static int  s_fm_move_src_vol  = -1;          // volume the source file lives on (0/1)
static char s_fm_move_src[256] = "";          // full source path within its volume
static char s_fm_move_base[128] = "";         // the file's base name (kept on the dest)
static char s_fm_move_status[96] = "";        // last move result, shown briefly
static lv_obj_t *s_fm_move_abar = nullptr;    // narrow move-mode bottom action bar (raised on top of the list)

static void app_open_files(void);            // fwd (rebuilt after a delete)
static void app_open_file_view(void);        // fwd (full-screen text viewer; screen_fn)
static bool fm_name_at_index(int idx, char *out, size_t n, bool *isdir_out);  // fwd (used by move handlers above its defn)

/* The fs::FS for the active volume. Only valid when s_fm_vol is 0 or 1. */
static fs::FS &fm_fs(void) {
  return (s_fm_vol == 1) ? sd_fs() : (fs::FS &)FFat;
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

/* ---- MOVE: copy across (possibly different) filesystems, then delete source ----
 * FFat and the SD are SEPARATE filesystems, so a plain rename() can't move between
 * them; even within one volume we use copy+delete for one uniform code path. Returns
 * true only if the whole copy succeeded AND the source was removed. The byte buffer
 * lives in PSRAM (internal SRAM is tight). */
static fs::FS &fm_fs_for_vol(int vol) { return (vol == 1) ? sd_fs() : (fs::FS &)FFat; }

static bool fm_copy_file(fs::FS &src_fs, const char *src,
                         fs::FS &dst_fs, const char *dst) {
  if (src_fs.exists(dst) || dst_fs.exists(dst)) { /* fall through; we overwrite */ }
  File in = src_fs.open(src, FILE_READ);
  if (!in) return false;
  File out = dst_fs.open(dst, FILE_WRITE);       // truncates/creates
  if (!out) { in.close(); return false; }
  const size_t CHUNK = 4096;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { in.close(); out.close(); return false; }
  bool ok = true;
  for (;;) {
    size_t n = in.read(buf, CHUNK);
    if (n == 0) break;
    if (out.write(buf, n) != n) { ok = false; break; }
  }
  heap_caps_free(buf);
  out.close();
  in.close();
  return ok;
}

/* Drop the captured source file into directory `dst_dir` on the currently-browsed
 * volume (s_fm_vol). Copies then deletes the original. Sets s_fm_move_status. */
static void fm_do_move_into(const char *dst_dir) {
  char dst[256];
  if (!strcmp(dst_dir, "/")) snprintf(dst, sizeof(dst), "/%s", s_fm_move_base);
  else                       snprintf(dst, sizeof(dst), "%s/%s", dst_dir, s_fm_move_base);

  // No-op guard: same volume + identical path -> nothing to do.
  if (s_fm_move_src_vol == s_fm_vol && !strcmp(dst, s_fm_move_src)) {
    snprintf(s_fm_move_status, sizeof(s_fm_move_status), "Already there");
    s_fm_move_active = false;
    return;
  }

  fs::FS &src_fs = fm_fs_for_vol(s_fm_move_src_vol);
  fs::FS &dst_fs = fm_fs_for_vol(s_fm_vol);
  bool ok = fm_copy_file(src_fs, s_fm_move_src, dst_fs, dst);
  if (ok) {
    src_fs.remove(s_fm_move_src);                // original gone only after a good copy
    snprintf(s_fm_move_status, sizeof(s_fm_move_status), "Moved %s", s_fm_move_base);
  } else {
    dst_fs.remove(dst);                          // clean up a partial copy
    snprintf(s_fm_move_status, sizeof(s_fm_move_status), "Move failed");
  }
  s_fm_move_active = false;
}

/* The move button on a file row: capture the source (volume + path + base name) and
 * enter destination-picker mode, starting at the volume chooser so you can drop it
 * anywhere on either volume. */
static void fm_move_request_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  char name[128];
  if (!fm_name_at_index(idx, name, sizeof(name), nullptr)) return;
  s_fm_move_src_vol = s_fm_vol;
  fm_join(s_fm_move_src, sizeof(s_fm_move_src), s_fm_path, name);
  strncpy(s_fm_move_base, name, sizeof(s_fm_move_base) - 1);
  s_fm_move_base[sizeof(s_fm_move_base) - 1] = '\0';
  s_fm_move_active = true;
  s_fm_move_status[0] = '\0';
  s_fm_vol = -1;                                 // start the picker at the volume chooser
  strcpy(s_fm_path, "/");
  lv_async_call(fm_rebuild_async, nullptr);
}

/* "Move here" bar (destination-picker mode): drop the file into the current dir. */
static void fm_move_here_cb(lv_event_t *e) {
  (void)e;
  fm_do_move_into(s_fm_path);
  // After a move, land in the destination directory so the user sees the result.
  lv_async_call(fm_rebuild_async, nullptr);
}
/* ---- SD format (manual rescue for a card that won't mount) ----
 * Triggered ONLY by tapping the greyed "SD Card (none)" row and confirming. The
 * card is reformatted to a FAT volume the ESP32 can mount — destructive, hence
 * the confirm. s_fm_format_pending gates the confirm dialog (like the delete one). */
static bool s_fm_format_pending = false;
static void fm_fmt_confirm_cb(lv_event_t *e) {
  (void)e;
  s_fm_format_pending = false;
  bool ok = sd_format();                       // DESTRUCTIVE; user confirmed
  if (ok) s_fm_vol = 1;                         // jump straight into the new SD volume
  if (ok) strcpy(s_fm_path, "/");
  lv_async_call(fm_rebuild_async, nullptr);
}
static void fm_fmt_cancel_cb(lv_event_t *e) {
  (void)e;
  s_fm_format_pending = false;
  lv_async_call(fm_rebuild_async, nullptr);
}
/* Tap handler on the greyed SD row -> raise the format prompt. */
static void fm_sd_format_request_cb(lv_event_t *e) {
  (void)e;
  s_fm_format_pending = true;
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

/* BOOT-back interceptor (registered while the Files app is open, see app_open_files).
 * Navigates WITHIN the app first so the hardware BOOT button replaces the old on-
 * screen "up/volumes" row: a press goes up one directory, or from a volume root back
 * to the volumes chooser. Only when ALREADY at the volumes chooser (s_fm_vol < 0) do
 * we return false, letting app_menu_back() leave Files for the menu. */
static bool fm_boot_back(void) {
  // A move in progress: BOOT is the ONLY way to cancel it (there is no on-screen
  // Cancel button), so it cancels from ANY depth — not just the volume chooser —
  // rather than navigating up. This keeps the cancel affordance consistent with the
  // always-present "<BOOT" hint while picking a destination.
  if (s_fm_move_active) {
    s_fm_move_active = false;
    s_fm_move_status[0] = '\0';
    lv_async_call(fm_rebuild_async, nullptr);
    return true;
  }
  if (s_fm_vol < 0) {
    return false;                               // normal: BOOT leaves the app
  }
  if (!strcmp(s_fm_path, "/")) s_fm_vol = -1;   // volume root -> back to chooser
  else                         fm_path_up();     // else up one directory
  lv_async_call(fm_rebuild_async, nullptr);
  return true;                                  // consumed
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
  lv_obj_set_width(row, UI_PX(330));
#if BOARD_SCREEN_NARROW
  // Tall enough for a name line on top + a row of big buttons underneath.
  lv_obj_set_height(row, size ? UI_PX(180) : UI_PX(96));
#else
  lv_obj_set_height(row, UI_PX(52));
#endif
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, UI_PX(12), 0);
  lv_obj_set_style_pad_all(row, UI_PX(10), 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  if (tap) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, tap, LV_EVENT_CLICKED, tap_ud);
  }

  lv_obj_t *ic = lv_label_create(row);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ic, lv_color_hex(ui_deco_hex(icon_rgb)), 0);
  lv_label_set_text(ic, symbol);
#if BOARD_SCREEN_NARROW
  // C6 file rows are tall with the buttons on a BOTTOM line, so anchor the icon at
  // the TOP-LEFT (volume/dir rows have no buttons, but top-left still reads fine).
  lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
#else
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
#endif

  lv_obj_t *nm = lv_label_create(row);
  lv_obj_set_style_text_font(nm, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(nm, lv_color_white(), 0);
  lv_label_set_text(nm, name);
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
  // File rows (size != null) carry a MOVE + DELETE button on the right (each UI_PX(32)
  // at RIGHT_MID 0 and -40), so the name is narrower and the size sits LEFT of both
  // buttons (RIGHT_MID -UI_PX(84)) instead of under them. Rows without a size (the
  // volume chooser) keep the full width and no right reservation.
#if BOARD_SCREEN_NARROW
  if (size) {
    // C6 FILE rows: name across the TOP (buttons live below it). Start it close to
    // the ~20px FILE/DIR glyph (16 px) so the LONG_DOT-truncated name has room before
    // the right-aligned size label. 56% width keeps it off "1234.5 KB".
    lv_obj_set_width(nm, LV_PCT(56));
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 16, 0);
  } else {
    // C6 VOLUME rows (no size label, no buttons): the DRIVE / SD_CARD glyph is WIDER
    // than the file icon, so start the name further right (34 px) to clear it, and
    // let it run nearly full-width since nothing sits on the right.
    lv_obj_set_width(nm, LV_PCT(82));
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 34, 0);
  }
#else
  // Name is truncated (LONG_DOT) at this width; keep it narrow enough that even a
  // long size string (right-aligned left of the wider buttons) can't reach it.
  lv_obj_set_width(nm, size ? UI_PX(90) : UI_PX(250));
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, UI_PX(34), 0);
#endif

  if (size) {
    lv_obj_t *sz = lv_label_create(row);
    lv_obj_set_style_text_font(sz, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(sz, lv_color_hex(0x888888), 0);
    lv_label_set_text(sz, size);
#if BOARD_SCREEN_NARROW
    lv_obj_align(sz, LV_ALIGN_TOP_RIGHT, 0, 0);   // top-right; buttons are on the bottom line
#else
    lv_obj_align(sz, LV_ALIGN_RIGHT_MID, -UI_PX(132), 0);   // left of the wider move+delete buttons
#endif
  }
  return row;
}

/* Add the small round X delete button to a file row (its own click; doesn't bubble).
 * Carries the row's index (no allocation); the handler resolves it to a path. */
static void fm_add_delete_x(lv_obj_t *row, int idx) {
  lv_obj_t *x = lv_btn_create(row);
#if BOARD_SCREEN_NARROW
  // C6: a big button on the BOTTOM-RIGHT of the tall row (under the file name),
  // half the row width so move + delete share the bottom line.
  lv_obj_set_size(x, LV_PCT(46), UI_PX(72));
  lv_obj_align(x, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
#else
  // S3: button as TALL as the row background so a tap can't fall through to the
  // row (which opens the file). pad_all on the row would cap an aligned child at
  // the inner box, so give it a negative pad override here to reach the edges.
  lv_obj_set_size(x, UI_PX(60), UI_PX(52));
  lv_obj_set_style_margin_all(x, -UI_PX(10), 0);   // cancel the row's UI_PX(10) pad
  lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
#endif
  lv_obj_set_style_bg_color(x, lv_color_hex(0x3A2020), 0);
  lv_obj_set_style_radius(x, UI_PX(12), 0);
  lv_obj_add_event_cb(x, fm_del_request_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
  lv_obj_t *xl = lv_label_create(x);
  lv_obj_set_style_text_font(xl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(xl, lv_color_hex(0xFF8888), 0);
  lv_label_set_text(xl, LV_SYMBOL_TRASH);
  lv_obj_center(xl);
}

/* Add the move button to a file row, LEFT of the delete X. Tapping it enters the
 * destination picker for this file (fm_move_request_cb). Carries the row index. */
static void fm_add_move_btn(lv_obj_t *row, int idx) {
  lv_obj_t *m = lv_btn_create(row);
#if BOARD_SCREEN_NARROW
  // C6: big button on the BOTTOM-LEFT of the tall row, beside the delete button.
  lv_obj_set_size(m, LV_PCT(46), UI_PX(72));
  lv_obj_align(m, LV_ALIGN_BOTTOM_LEFT, 0, 0);
#else
  // S3: full-row-height button, just left of the delete X (which is UI_PX(60) wide
  // at the right edge). Match its height + negative pad so neither leaks taps.
  lv_obj_set_size(m, UI_PX(60), UI_PX(52));
  lv_obj_set_style_margin_all(m, -UI_PX(10), 0);   // cancel the row's UI_PX(10) pad
  lv_obj_align(m, LV_ALIGN_RIGHT_MID, -UI_PX(64), 0);   // sits just left of the X
#endif
  lv_obj_set_style_bg_color(m, lv_color_hex(0x20303A), 0);
  lv_obj_set_style_radius(m, UI_PX(12), 0);
  lv_obj_add_event_cb(m, fm_move_request_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
  lv_obj_t *ml = lv_label_create(m);
  lv_obj_set_style_text_font(ml, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ml, lv_color_hex(0x88C0FF), 0);
  lv_label_set_text(ml, LV_SYMBOL_UPLOAD);   // "move/export" affordance
  lv_obj_center(ml);
}

/* ----------------------------- Delete confirm screen --------------------- */
static void fm_build_confirm(void) {
  lv_obj_t *box = lv_obj_create(app_scr);
  // Narrow panels: a taller box so the two buttons can stack vertically (side-by-
  // side UI_PX(150) buttons clip together in the narrow box). Wider too, as a % of
  // the screen rather than a fixed UI_PX width that shrinks too far.
#if BOARD_SCREEN_NARROW
  lv_obj_set_size(box, LV_PCT(92), UI_PX(360));
#else
  lv_obj_set_size(box, UI_PX(360), UI_PX(240));
#endif
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x202020), 0);
  lv_obj_set_style_radius(box, UI_PX(16), 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *q = lv_label_create(box);
  lv_obj_set_style_text_font(q, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(q, lv_color_white(), 0);
  lv_obj_set_width(q, LV_PCT(88));
  lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
  lv_label_set_text_fmt(q, "Delete this file?\n%s", s_fm_pending_del);
  lv_obj_align(q, LV_ALIGN_TOP_MID, 0, UI_PX(24));

  lv_obj_t *cancel = lv_btn_create(box);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x333333), 0);
  lv_obj_set_style_radius(cancel, UI_PX(12), 0);
  lv_obj_add_event_cb(cancel, fm_del_cancel_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *cl = lv_label_create(cancel);
  lv_obj_set_style_text_font(cl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(cl, lv_color_white(), 0);
  lv_label_set_text(cl, "Cancel");
  lv_obj_center(cl);

  lv_obj_t *del = lv_btn_create(box);
  lv_obj_set_style_bg_color(del, lv_color_hex(0x802020), 0);
  lv_obj_set_style_radius(del, UI_PX(12), 0);
  lv_obj_add_event_cb(del, fm_del_confirm_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *dl = lv_label_create(del);
  lv_obj_set_style_text_font(dl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(dl, lv_color_hex(0xFFB0B0), 0);
  lv_label_set_text(dl, LV_SYMBOL_TRASH "  Delete");
  lv_obj_center(dl);

  // Narrow: stack the two buttons in their own rows (Delete above Cancel), bigger
  // and full-width, so they don't clip together. Wide (S3): keep the original
  // side-by-side bottom-corner layout.
#if BOARD_SCREEN_NARROW
  lv_obj_set_size(del,    LV_PCT(86), UI_PX(120));
  lv_obj_set_size(cancel, LV_PCT(86), UI_PX(120));
  lv_obj_align(del,    LV_ALIGN_BOTTOM_MID, 0, UI_PX(-150));
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, UI_PX(-24));
#else
  lv_obj_set_size(cancel, UI_PX(150), UI_PX(52));
  lv_obj_set_size(del,    UI_PX(150), UI_PX(52));
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT,  UI_PX(8),  UI_PX(-16));
  lv_obj_align(del,    LV_ALIGN_BOTTOM_RIGHT, UI_PX(-8), UI_PX(-16));
#endif
}

/* Format-SD confirm dialog. Same shape as the delete confirm, stronger wording —
 * this erases the WHOLE card. Shown when s_fm_format_pending (greyed SD row tapped). */
static void fm_build_format_confirm(void) {
  lv_obj_t *box = lv_obj_create(app_scr);
  lv_obj_set_size(box, UI_PX(360), UI_PX(240));
  lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x202020), 0);
  lv_obj_set_style_radius(box, UI_PX(16), 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *q = lv_label_create(box);
  lv_obj_set_style_text_font(q, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(q, lv_color_white(), 0);
  lv_obj_set_width(q, UI_PX(320));
  lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
  lv_label_set_text(q, "Format the inserted SD card?\n");
  lv_obj_align(q, LV_ALIGN_TOP_MID, 0, UI_PX(20));

  lv_obj_t *cancel = lv_btn_create(box);
  lv_obj_set_size(cancel, UI_PX(150), UI_PX(52));
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, UI_PX(8), UI_PX(-16));
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x333333), 0);
  lv_obj_set_style_radius(cancel, UI_PX(12), 0);
  lv_obj_add_event_cb(cancel, fm_fmt_cancel_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *cl = lv_label_create(cancel);
  lv_obj_set_style_text_font(cl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(cl, lv_color_white(), 0);
  lv_label_set_text(cl, "Cancel");
  lv_obj_center(cl);

  lv_obj_t *fmt = lv_btn_create(box);
  lv_obj_set_size(fmt, UI_PX(150), UI_PX(52));
  lv_obj_align(fmt, LV_ALIGN_BOTTOM_RIGHT, UI_PX(-8), UI_PX(-16));
  lv_obj_set_style_bg_color(fmt, lv_color_hex(0x802020), 0);
  lv_obj_set_style_radius(fmt, UI_PX(12), 0);
  lv_obj_add_event_cb(fmt, fm_fmt_confirm_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *fl = lv_label_create(fmt);
  lv_obj_set_style_text_font(fl, &FONT_LABEL, 0);
  lv_obj_set_style_text_color(fl, lv_color_hex(0xFFB0B0), 0);
  lv_label_set_text(fl, "Format");
  lv_obj_center(fl);
}

/* ----------------------------- Text viewer (full-screen) ------------------ */
#define FM_VIEW_MAX  8192    // read at most this many bytes (a head, not the whole file)
static void app_open_file_view(void) {
  app_screen_begin("");   // no header; "<- BOOT" hint top-left returns to the listing

  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_width(col, LV_PCT(92));
  lv_obj_set_height(col, (int)screenHeight - UI_PX(52) - UI_PX(8));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(52));
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, UI_PX(12), 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(col);  // narrow-panel: shift scrollbar toward the edge (no-op on S3)
  lv_obj_set_style_pad_row(col, UI_PX(8), 0);
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
  lv_obj_set_size(body, LV_PCT(100), UI_PX(380));
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
/* Builds the free-space header and returns the Y of its BOTTOM edge so the caller
 * can start the list below it. Returns its actual rendered height, so when the line
 * WRAPS to 2-3 rows (long "X of Y used" on a narrow panel) the list is pushed down
 * to make room instead of the extra rows overlapping the list. */
static int fm_add_space_header(lv_obj_t *parent) {
  uint64_t total = 0, used = 0;
  if (s_fm_vol == 1) { total = sd_total_bytes(); used = sd_used_bytes(); }
  else               { total = FFat.totalBytes(); used = FFat.usedBytes(); }

  char u[24], t[24];
  fm_fmt_size(used, u, sizeof(u));
  fm_fmt_size(total, t, sizeof(t));

  lv_obj_t *hdr = lv_label_create(parent);
  lv_obj_set_style_text_font(hdr, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hdr, lv_color_hex(0x888888), 0);
  // Constrain to the screen width and WRAP, centered, so a long line (e.g. a big
  // SD card's "X of Y used") flows onto as many rows as it needs instead of
  // running off both sides. Applies on every board — the S3 just rarely needs it.
  lv_obj_set_width(hdr, LV_PCT(90));
  lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(hdr, LV_LABEL_LONG_WRAP);
  lv_label_set_text_fmt(hdr, "%s  |  %s of %s used",
                        s_fm_vol == 1 ? "SD Card" : "Flash (FFat)", u, t);
  // Below the "Files" title so the two don't overlap. The list (app_open_files)
  // starts below this header in turn. Narrow panels: the title sits lower
  // (UI_PX(72)), so drop the header to match.
#if BOARD_SCREEN_NARROW
  int hdr_top = UI_PX(106);
#else
  int hdr_top = UI_PX(74);
#endif
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, hdr_top);
  // Force a layout pass so the wrapped label's real (multi-row) height is known,
  // then report where its bottom edge actually lands.
  lv_obj_update_layout(hdr);
  return hdr_top + lv_obj_get_height(hdr);
}

/* Destination-picker banner: shown across the top while choosing where to move a
 * file. Names the file being moved, offers "Move here" (drops it into the CURRENT
 * directory — only meaningful inside a volume) and "Cancel". Returns the banner
 * height so the list below can be offset. Built on app_scr (over the title area). */
static int fm_add_move_banner(lv_obj_t *parent) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_width(bar, LV_PCT(96));
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x16242E), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bar, UI_PX(12), 0);
  lv_obj_set_style_pad_all(bar, UI_PX(8), 0);

#if BOARD_SCREEN_NARROW
  // Narrow screens: the old 84px top bar squeezed two 40px buttons under the label,
  // clipped the title above and was too small to tap. Split it — keep ONLY the
  // "Moving: ..." label up top (so the list offsets below it), and put the BIG
  // action buttons in a separate opaque bottom bar (built below) that can't be
  // overlapped by file rows.
  lv_obj_set_height(bar, LV_SIZE_CONTENT);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, UI_PX(122));   // below the narrow-panel "Files" title (was 96 -> clipped it)
#else
  lv_obj_set_height(bar, UI_PX(84));
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, UI_PX(36));
#endif

  lv_obj_t *lbl = lv_label_create(bar);
  lv_obj_set_style_text_font(lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x88C0FF), 0);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_label_set_text_fmt(lbl, LV_SYMBOL_UPLOAD "  Moving: %s", s_fm_move_base);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

#if BOARD_SCREEN_NARROW
  // ---- bottom action bar: big, opaque, can't be hidden behind file rows ----
  // Only ONE action ("Move here"); cancelling is the always-present "<BOOT" key
  // (fm_boot_back cancels a move from any depth), so there's no Cancel button.
  lv_obj_t *abar = lv_obj_create(parent);
  s_fm_move_abar = abar;   // raised to the foreground after the list is built (z-order)
  lv_obj_remove_style_all(abar);
  lv_obj_set_size(abar, LV_PCT(100), UI_PX(140));
  lv_obj_align(abar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(abar, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(abar, LV_OPA_COVER, 0);   // solid black so rows behind it don't show through
  lv_obj_set_style_pad_all(abar, UI_PX(12), 0);
  lv_obj_clear_flag(abar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *here = lv_btn_create(abar);
  lv_obj_set_size(here, LV_PCT(100), UI_PX(88));
  lv_obj_align(here, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(here, UI_PX(12), 0);
  if (s_fm_vol < 0) {
    lv_obj_set_style_bg_color(here, lv_color_hex(0x2A2A2A), 0);
    lv_obj_add_state(here, LV_STATE_DISABLED);
  } else {
    lv_obj_set_style_bg_color(here, lv_color_hex(0x1E5A2E), 0);
    lv_obj_add_event_cb(here, fm_move_here_cb, LV_EVENT_CLICKED, nullptr);
  }
  lv_obj_t *hl = lv_label_create(here);
  lv_obj_set_style_text_font(hl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hl, lv_color_white(), 0);
  lv_label_set_text(hl, s_fm_vol < 0 ? "Pick a volume" : "Move here");
  lv_obj_center(hl);

  // List sits between the label banner and the bottom action bar. Report the label
  // banner's real bottom edge (it auto-sized to its content).
  lv_obj_update_layout(bar);
  return UI_PX(122) + lv_obj_get_height(bar);
#else
  // Wide screens: one centered "Move here" button in the top bar. Cancelling is the
  // always-present "<BOOT" key (no Cancel button), with a hint beside the action.
  lv_obj_t *here = lv_btn_create(bar);
  lv_obj_set_size(here, UI_PX(180), UI_PX(44));
  lv_obj_align(here, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_radius(here, UI_PX(10), 0);
  if (s_fm_vol < 0) {
    lv_obj_set_style_bg_color(here, lv_color_hex(0x2A2A2A), 0);
    lv_obj_add_state(here, LV_STATE_DISABLED);
  } else {
    lv_obj_set_style_bg_color(here, lv_color_hex(0x1E5A2E), 0);
    lv_obj_add_event_cb(here, fm_move_here_cb, LV_EVENT_CLICKED, nullptr);
  }
  lv_obj_t *hl = lv_label_create(here);
  lv_obj_set_style_text_font(hl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hl, lv_color_white(), 0);
  lv_label_set_text(hl, s_fm_vol < 0 ? "Pick a volume" : "Move here");
  lv_obj_center(hl);

  return UI_PX(36) + UI_PX(84);   // banner bottom (top offset + height)
#endif
}

/* ----------------------------- List screen -------------------------------- */
static void app_open_files(void) {
  app_screen_begin("Files");
  nav_back_intercept = fm_boot_back;   // BOOT navigates up/out within Files (no on-screen back row)
  s_fm_move_abar = nullptr;            // stale handle from the just-cleared screen

  // A pending delete / format shows its confirm prompt instead of the listing.
  if (s_fm_pending_del[0]) { fm_build_confirm(); return; }
  if (s_fm_format_pending) { fm_build_format_confirm(); return; }

  // Destination-picker mode: show the move banner across the top; it pushes the list
  // down below it. (Normal mode: list starts just below the title / space header.)
  int list_top;
  if (s_fm_move_active) {
    list_top = fm_add_move_banner(app_scr) + UI_PX(8);
  } else if (s_fm_vol < 0) {
    // Volume chooser: no free-space header, list starts just below the title.
#if BOARD_SCREEN_NARROW
    list_top = UI_PX(122);
#else
    list_top = UI_PX(90);
#endif
  } else {
    // Inside a volume: build the free-space header FIRST and start the list below
    // its REAL bottom edge, so when the header wraps to 2-3 rows the list moves
    // down to make room instead of being overlapped.
    list_top = fm_add_space_header(app_scr) + UI_PX(8);
  }

  // The scrollable list area (below the title/banner, room for the bottom bar).
  // In narrow move mode, reserve the bottom action-bar height (UI_PX(220)) so the
  // last rows aren't trapped behind the opaque bar where they can't be tapped.
  int list_bottom_reserve = UI_PX(8);
#if BOARD_SCREEN_NARROW
  if (s_fm_move_active) list_bottom_reserve = UI_PX(140) + UI_PX(8);
#endif
  lv_obj_t *list = lv_obj_create(app_scr);
  lv_obj_set_width(list, LV_PCT(92));
  lv_obj_set_height(list, (int)screenHeight - list_top - list_bottom_reserve);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(list);  // narrow-panel: shift scrollbar toward the edge (no-op on S3)
  lv_obj_set_style_pad_row(list, UI_PX(8), 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);

  // ---- ROOT: the volume chooser (Flash always; SD when mounted) ----
  if (s_fm_vol < 0) {
    fm_row(list, LV_SYMBOL_DRIVE, 0xFF9F0A, "Flash (FFat)", nullptr,
           fm_pick_vol_cb, (void *)(intptr_t)0);
    if (sd_mount())
      fm_row(list, LV_SYMBOL_SD_CARD, 0x33A0FF, "SD Card", nullptr,
             fm_pick_vol_cb, (void *)(intptr_t)1);
    else
      // Greyed "no card" row IS tappable: it opens the format prompt, so a card
      // Windows wrote in a layout the ESP32 can't mount can be reformatted on-
      // device (destructive, confirmed). If truly no card is inserted, the format
      // simply fails (no card responds) and we return to this list.
      fm_row(list, LV_SYMBOL_SD_CARD, 0x555555, "SD Card (tap to format)",
             nullptr, fm_sd_format_request_cb, nullptr);
    return;
  }

  // ---- INSIDE A VOLUME: the directory ----
  // (No on-screen "up" row anymore — the hardware BOOT button goes up/out via
  // fm_boot_back.) The free-space header was already built above (it drives
  // list_top); in move mode the banner occupies the top instead.

  // One-shot move result toast (e.g. "Moved foo.txt"). Shown as the first list row
  // after a completed/cancelled move, then cleared so it doesn't persist.
  if (s_fm_move_status[0]) {
    lv_obj_t *toast = lv_label_create(list);
    lv_obj_set_style_text_font(toast, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(toast, lv_color_hex(0x66D080), 0);
    lv_label_set_text(toast, s_fm_move_status);
    s_fm_move_status[0] = '\0';
  }

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
      // In destination-picker mode files are just context (you're choosing a folder),
      // so no move/delete buttons then — only in the normal browser.
      if (!s_fm_move_active) {
        fm_add_move_btn(row, idx);
        fm_add_delete_x(row, idx);
      }
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

  // The list was created AFTER the move-mode action bar, so it sits above it in
  // z-order and the bottom rows show through. Raise the (opaque) bar back to the
  // front so it cleanly covers anything scrolled behind it.
  if (s_fm_move_abar) lv_obj_move_foreground(s_fm_move_abar);
}
