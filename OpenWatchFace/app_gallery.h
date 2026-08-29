/* ============================================================================
 *  app_gallery.h — "Gallery": browse photos and videos on the watch's storage.
 *
 *  A STANDALONE app, deliberately not tied to the camera: it exists on any
 *  board with PSRAM and a JPEG decoder, so a watch with no sensor can still
 *  carry pictures — drop files on the SD card or the flash FAT partition and
 *  they show up here.
 *
 *  WHAT IT LISTS. Photos (JPEG, PNG, BMP, static GIF) and videos (MJPEG-AVI,
 *  raw MJPEG streams, animated GIF), from FOUR places, presented as LOCATION
 *  SECTIONS in one grid:
 *    - /DCIM on the SD card    \  "Camera" — whatever an on-device camera
 *    - /DCIM on FFat           /  wrote (the Camera app's own output dirs)
 *    - the SD card's root         "SD card" — files the user copied on
 *    - the FFat root              "Internal" — files pushed to flash
 *  Sub-directories are NOT recursed: /DCIM is the one well-known convention,
 *  and a full tree walk over a slow shared SPI bus could take seconds.
 *
 *  Formats the watch CANNOT decode (WebP, HEIC, MP4/H.264, ...) are still
 *  listed, as a placeholder tile naming the format: a file that silently
 *  vanishes looks like a broken card, whereas "WebP not supported" tells the
 *  user exactly what to convert. The same tile appears for a supported file
 *  that failed to decode, carrying the decoder's own reason.
 *
 *  DECODING lives in media_codecs.h (stills) and media_video.h (containers);
 *  this file is the UI. Everything is decoded straight to RGB565, downscaled
 *  during decode, so a 12-megapixel import costs a screen-sized buffer, not a
 *  12-megapixel one.
 *
 *  VIEWS. The grid (cover-cropped square thumbnails, play badge on videos,
 *  newest-name-first per section) → tap a photo for the swipe pager (animated
 *  prev/next, slider zoom + one-finger pan) → tap a video for the player
 *  (paced by the file's own frame rate, per-frame for GIFs; play/pause doubles
 *  as replay). Delete is two-tap-confirm everywhere.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER app_menu.h (screen
 *  shell + nav) and storage_fs.h (sd_fs/sd_mount/FFat/ffat_mount), BEFORE
 *  app_camera.h (whose gallery chip jumps here). app_open_gallery /
 *  app_gallery_on_close are forward-declared in app_menu.h for the tile and
 *  the leave-app hooks.
 * ========================================================================== */
#pragma once

#ifndef OWF_HAS_GALLERY
#if BOARD_HAS_PSRAM && !BOARD_PLATFORM_TUYA && __has_include("jpeg_decoder.h")
#define OWF_HAS_GALLERY 1
#else
#define OWF_HAS_GALLERY 0
#endif
#endif

#if OWF_HAS_GALLERY

#include <lvgl.h>
#include <ctype.h>
#include "esp_heap_caps.h"
#include "media_codecs.h"     // stills: JPEG / PNG / BMP / GIF -> RGB565
#include "media_video.h"      // containers: AVI / MJPEG / animated GIF

#define GAL_DCIM     "/DCIM"

/* ---- the media list --------------------------------------------------------
 * One flat array, grouped by section in scan order. Names are basenames; the
 * owning filesystem and directory are carried per item so the three views can
 * open the file wherever it lives. */
#define GAL_LIST_MAX 30       /* newest listed items; beyond it the caption
                               * says how many more exist (decode + PSRAM cap) */
#define GAL_SRC_CAM   0       /* /DCIM (either fs) — "Camera" section */
#define GAL_SRC_SD    1       /* SD card root */
#define GAL_SRC_FLASH 2       /* FFat root */

#define GAL_KIND_PHOTO 0
#define GAL_KIND_VIDEO 1
#define GAL_KIND_OTHER 2      /* recognised extension, not decodable here */

static char    s_gal_names[GAL_LIST_MAX][40];
static uint8_t s_gal_src[GAL_LIST_MAX];      // GAL_SRC_* (the section)
static uint8_t s_gal_onsd[GAL_LIST_MAX];     // 1 = lives on SD, 0 = on FFat
static uint8_t s_gal_kind[GAL_LIST_MAX];     // GAL_KIND_*
static const char *s_gal_err[GAL_LIST_MAX];  // why a tile is a placeholder
static int     s_gal_list_n = 0;
static int     s_gal_extra  = 0;             // media files beyond the cap

/* ---- view state ---- */
static int   s_gal_idx        = 0;           // selected item (grid index)
static bool  s_gal_photo_view = false;       // sub-view: the swipe pager
static bool  s_gal_video_view = false;       // sub-view: the video player
static bool  s_gal_confirm_del = false;

static lv_obj_t *s_gal_status = nullptr;     // caption / toast line

/* grid: one decoded thumbnail + descriptor per listed item */
static uint8_t     *s_gal_thumb_buf[GAL_LIST_MAX];
static lv_img_dsc_t s_gal_thumb_dsc[GAL_LIST_MAX];

/* photo view: up to 3 decoded pages (prev/current/next photo) for the pager */
static uint8_t     *s_gal_page_buf[3];
static lv_img_dsc_t s_gal_page_dsc[3];
static lv_obj_t    *s_gal_page_img[3];
static int          s_gal_cell_idx[3];
static int          s_gal_cells  = 0;
static int          s_gal_center = 0;
static lv_obj_t    *s_gal_pager  = nullptr;
static int          s_gal_view_h = 0;
static int          s_gal_zoom_pct = 100;    // 100..400 (%)
static int          s_gal_pan_x = 0, s_gal_pan_y = 0;

/* video player: one demuxer handle plus the LVGL bits it drives */
static mv_t        s_vid;
static bool        s_vid_open    = false;
static bool        s_vid_playing = false;
static uint32_t    s_vid_ms      = 0;        // playback position, ms
static lv_img_dsc_t s_vid_dsc;
static lv_obj_t   *s_vid_img      = nullptr;
static lv_obj_t   *s_vid_play_btn = nullptr;
static lv_timer_t *s_vid_timer    = nullptr;
static uint32_t    s_vid_cap_s    = ~0u;

static void app_open_gallery(void);
static void app_open_gallery_grid(void);
static void app_open_gallery_photo(void);
static void app_open_gallery_video(void);
static void gal_vid_close(void);
static void gal_vid_set_playing(bool p);

/* ============================ helpers ================================== */

static void gal_fill_dsc(lv_img_dsc_t *d, const uint8_t *data, int w, int h) {
  memset(d, 0, sizeof(*d));
  d->header.magic  = LV_IMAGE_HEADER_MAGIC;   // LVGL 9 rejects the dsc without it
  d->header.cf     = LV_COLOR_FORMAT_RGB565;
  d->header.w      = w;
  d->header.h      = h;
  d->header.stride = w * 2;                   // and draws garbage without this
  d->data          = data;
  d->data_size     = (size_t)w * h * 2;
}

static void gal_set_status(const char *msg, uint32_t color) {
  if (!s_gal_status) return;
  lv_label_set_text(s_gal_status, msg);
  lv_obj_set_style_text_color(s_gal_status, lv_color_hex(color), 0);
}

static const char *gal_basename(const char *nm) {
  const char *slash = strrchr(nm, '/');
  return slash ? slash + 1 : nm;
}

/* The file's extension in upper case, for placeholder tiles ("WEBP"). */
static void gal_ext_upper(const char *bn, char *out, size_t out_sz) {
  const char *dot = strrchr(bn, '.');
  size_t i = 0;
  if (dot) {
    for (const char *p = dot + 1; *p && i + 1 < out_sz; p++)
      out[i++] = (char)toupper((unsigned char)*p);
  }
  out[i] = 0;
}

/* Media kind by extension, case-insensitive:
 *   GAL_KIND_PHOTO / GAL_KIND_VIDEO / GAL_KIND_OTHER, or -1 for "not media".
 * Extensions only decide what gets LISTED and which viewer opens; the actual
 * decoder is chosen from the file's magic bytes, so a mislabelled file still
 * shows up correctly.
 *
 * "._foo.jpg" AppleDouble junk (macOS write-through-SMB droppings on cards
 * that visited a Mac) is excluded — they are resource forks, not JPEGs. */
static int gal_media_kind(const char *bn) {
  if (bn[0] == '.') return -1;
  const char *dot = strrchr(bn, '.');
  if (!dot) return -1;
  static const char *photo[] = { ".jpg", ".jpeg", ".jpe", ".jfif", ".png",
                                 ".bmp", ".dib" };
  static const char *video[] = { ".avi", ".mjpg", ".mjpeg", ".mjpa" };
  /* Recognisable media we cannot decode: listed, but as a labelled placeholder
   * rather than silently dropped. */
  static const char *other[] = { ".webp", ".heic", ".heif", ".avif", ".tif",
                                 ".tiff", ".mp4", ".mov", ".m4v", ".mkv",
                                 ".webm", ".3gp", ".raw", ".dng", ".svg" };
  for (unsigned i = 0; i < sizeof(photo) / sizeof(photo[0]); i++)
    if (!strcasecmp(dot, photo[i])) return GAL_KIND_PHOTO;
  for (unsigned i = 0; i < sizeof(video) / sizeof(video[0]); i++)
    if (!strcasecmp(dot, video[i])) return GAL_KIND_VIDEO;
  for (unsigned i = 0; i < sizeof(other) / sizeof(other[0]); i++)
    if (!strcasecmp(dot, other[i])) return GAL_KIND_OTHER;
  if (!strcasecmp(dot, ".gif")) return GAL_KIND_PHOTO;   /* refined at scan */
  return -1;
}

/* The filesystem / full path of a listed item. */
static fs::FS &gal_item_fs(int i) {
  return s_gal_onsd[i] ? sd_fs() : (fs::FS &)FFat;
}
static void gal_item_path(int i, char *out, size_t out_sz) {
  snprintf(out, out_sz, "%s/%s",
           (s_gal_src[i] == GAL_SRC_CAM) ? GAL_DCIM : "", s_gal_names[i]);
}

/* Scan ONE directory into the list. Called once per source, in section order,
 * so the array stays grouped; within the section entries are kept sorted by
 * name DESCENDING (our IMG_/VID_ names are zero-padded, so lexicographic is
 * chronological — newest first; foreign names get reverse-alphabetical, which
 * at least keeps an imported set together and stable). */
static void gal_scan_dir(fs::FS &f, const char *dirpath, uint8_t src, uint8_t onsd) {
  File dir = f.open(dirpath);
  if (!dir || !dir.isDirectory()) return;

  const int sec_start = s_gal_list_n;          // this section's insertion range
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    if (!e.isDirectory()) {
      const char *bn = gal_basename(e.name());
      int k = gal_media_kind(bn);
      if (k >= 0) {
        /* A GIF is a picture or a video depending on its frame count — the
         * only kind we cannot tell from the name. The check reads a few
         * hundred bytes of block headers, no pixels. */
        const char *dotg = strrchr(bn, '.');
        if (dotg && !strcasecmp(dotg, ".gif")) {
          char gp[160];
          snprintf(gp, sizeof(gp), "%s/%s",
                   (src == GAL_SRC_CAM) ? GAL_DCIM : "", bn);
          if (mv_gif_is_animated(f, gp)) k = GAL_KIND_VIDEO;
        }
        int pos = s_gal_list_n;                // insertion point, descending
        while (pos > sec_start && strcasecmp(s_gal_names[pos - 1], bn) < 0) pos--;
        if (pos < GAL_LIST_MAX) {
          if (s_gal_list_n == GAL_LIST_MAX) s_gal_extra++;   // the tail falls off
          else s_gal_list_n++;
          for (int i = s_gal_list_n - 1; i > pos; i--) {
            memcpy(s_gal_names[i], s_gal_names[i - 1], sizeof(s_gal_names[0]));
            s_gal_src[i]  = s_gal_src[i - 1];
            s_gal_onsd[i] = s_gal_onsd[i - 1];
            s_gal_kind[i] = s_gal_kind[i - 1];
          }
          snprintf(s_gal_names[pos], sizeof(s_gal_names[0]), "%s", bn);
          s_gal_src[pos]  = src;
          s_gal_onsd[pos] = onsd;
          s_gal_kind[pos] = (uint8_t)k;
        } else {
          s_gal_extra++;                       // sorts below everything listed
        }
      }
    }
    e.close();
  }
  dir.close();
}

/* Build the whole list: both /DCIM dirs first (the "Camera" section), then the
 * SD root, then the FFat root. Each backend is scanned only if it is actually
 * mountable right now, so a pulled card just makes its sections disappear. */
static void gal_build_list(void) {
  s_gal_list_n = 0;
  s_gal_extra  = 0;
  for (int i = 0; i < GAL_LIST_MAX; i++) s_gal_err[i] = nullptr;
  bool sd = sd_mount();
  bool ff = ffat_mount();
  if (sd) gal_scan_dir(sd_fs(),        GAL_DCIM, GAL_SRC_CAM,   1);
  if (ff) gal_scan_dir((fs::FS &)FFat, GAL_DCIM, GAL_SRC_CAM,   0);
  if (sd) gal_scan_dir(sd_fs(),        "/",      GAL_SRC_SD,    1);
  if (ff) gal_scan_dir((fs::FS &)FFat, "/",      GAL_SRC_FLASH, 0);
}

/* ============================ decode =================================== */

static void gal_free_bufs(void) {
  for (int i = 0; i < GAL_LIST_MAX; i++) {
    if (s_gal_thumb_buf[i]) { heap_caps_free(s_gal_thumb_buf[i]); s_gal_thumb_buf[i] = nullptr; }
  }
  for (int i = 0; i < 3; i++) {
    if (s_gal_page_buf[i]) { heap_caps_free(s_gal_page_buf[i]); s_gal_page_buf[i] = nullptr; }
    s_gal_page_img[i] = nullptr;
  }
  s_gal_pager = nullptr;
  s_gal_cells = 0;
}

/* Decode item `i` for display in a tw x th box. Photos and videos differ only
 * in which decoder produces the pixels — a video's "still" is its first frame.
 * Returns false with *err set to a showable reason. */
static bool gal_decode_item(int i, int tw, int th, bool cover,
                            uint8_t **buf, lv_img_dsc_t *dsc, const char **err) {
  char path[160];
  gal_item_path(i, path, sizeof(path));
  uint16_t *px = nullptr;
  int w = 0, h = 0;
  bool ok = (s_gal_kind[i] == GAL_KIND_VIDEO)
              ? mv_thumb(gal_item_fs(i), path, tw, th, cover, &px, &w, &h, err)
              : mc_decode_still(gal_item_fs(i), path, tw, th, cover, &px, &w, &h, err);
  if (!ok) return false;
  *buf = (uint8_t *)px;
  gal_fill_dsc(dsc, *buf, w, h);
  return true;
}

/* ============================ video playback ============================
 * media_video.h owns the demuxing and the frame buffer; everything here is
 * pacing and chrome. Playback runs off an LVGL timer whose period follows the
 * file's own frame timing — which for a GIF changes frame by frame. */

/* Show the frame media_video.h just decoded, CONTAIN-fit in the player cell. */
static void gal_vid_show_frame(void) {
  if (!s_vid_img || s_vid.fw <= 0 || s_vid.fh <= 0) return;
  gal_fill_dsc(&s_vid_dsc, (const uint8_t *)s_vid.rgb, s_vid.fw, s_vid.fh);
  lv_img_set_src(s_vid_img, &s_vid_dsc);
  int zx = (LCD_WIDTH    * 256) / s_vid.fw;
  int zy = (s_gal_view_h * 256) / s_vid.fh;
  int z  = (zx < zy) ? zx : zy;
  if (z > 256) z = 256;
  lv_img_set_zoom(s_vid_img, z > 0 ? z : 1);
  lv_obj_set_pos(s_vid_img, (LCD_WIDTH    - s_vid.fw * z / 256) / 2,
                            (s_gal_view_h - s_vid.fh * z / 256) / 2);
  lv_obj_invalidate(s_vid_img);
}

/* Caption: "VID_007.AVI  0:04 / 0:12" (the total is dropped when the container
 * does not state one, as with a bare MJPEG stream). Rewritten only when the
 * shown second changes, never while the delete confirm is armed. */
static void gal_vid_update_caption(void) {
  if (!s_vid_open || s_gal_confirm_del) return;
  uint32_t cs = s_vid_ms / 1000;
  uint32_t ts = mv_duration_s(&s_vid);
  if (cs == s_vid_cap_s) return;
  s_vid_cap_s = cs;
  char cap[96];
  if (ts > 0)
    snprintf(cap, sizeof(cap), "%s   %lu:%02lu / %lu:%02lu",
             s_gal_names[s_gal_idx],
             (unsigned long)(cs / 60), (unsigned long)(cs % 60),
             (unsigned long)(ts / 60), (unsigned long)(ts % 60));
  else
    snprintf(cap, sizeof(cap), "%s   %lu:%02lu",
             s_gal_names[s_gal_idx],
             (unsigned long)(cs / 60), (unsigned long)(cs % 60));
  gal_set_status(cap, 0x999999);
}

/* Advance playback one frame. End of stream (or a damaged tail) rewinds to the
 * first frame and pauses, so the play button naturally means "replay". */
static void gal_vid_step(void) {
  if (!s_vid_open) return;
  int r = mv_next(&s_vid);
  if (r == 1) {
    gal_vid_show_frame();
    s_vid_ms += s_vid.delay_ms;
    if (s_vid_timer) {                       /* GIFs re-time on every frame */
      uint32_t p = s_vid.delay_ms < 20 ? 20 : s_vid.delay_ms;
      lv_timer_set_period(s_vid_timer, p);
    }
  } else if (r == 0) {
    mv_rewind(&s_vid);
    s_vid_ms = 0;
    gal_vid_set_playing(false);
  }
  /* r < 0: one undecodable frame — keep the previous picture and carry on. */
  gal_vid_update_caption();
}

static void gal_vid_timer_cb(lv_timer_t *t) {
  (void)t;
  if (s_vid_playing) gal_vid_step();
}

/* Single owner of the player's file handle, timer and buffers. Safe from any
 * teardown path, any number of times. */
static void gal_vid_close(void) {
  if (s_vid_timer) { lv_timer_del(s_vid_timer); s_vid_timer = nullptr; }
  if (s_vid_open)  { mv_close(&s_vid); s_vid_open = false; }
  s_vid_playing = false;
  s_vid_ms      = 0;
  s_vid_img      = nullptr;
  s_vid_play_btn = nullptr;
}

/* ============================ navigation ================================ */

static void gal_rebuild_async(void *unused) {
  (void)unused;
  app_open_gallery();
}

static void gal_to_grid(void) {
  s_gal_photo_view  = false;
  s_gal_video_view  = false;
  s_gal_confirm_del = false;
  nav_back_intercept = nullptr;
  lv_async_call(gal_rebuild_async, nullptr);
}
static void gal_to_grid_cb(lv_event_t *e) { (void)e; gal_to_grid(); }
static bool gal_back_intercept(void) {
  if (!s_gal_photo_view && !s_gal_video_view) return false;
  gal_to_grid();
  return true;
}

/* Grid thumbnail tapped: photos open the pager, videos the player, and an
 * undecodable file just explains itself in the caption. */
static void gal_thumb_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= s_gal_list_n) return;
  if (s_gal_kind[idx] == GAL_KIND_OTHER || s_gal_err[idx]) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: %s", s_gal_names[idx],
             s_gal_err[idx] ? s_gal_err[idx] : "Not supported");
    gal_set_status(msg, 0xFFD60A);
    return;
  }
  s_gal_idx         = idx;
  s_gal_video_view  = s_gal_kind[idx] == GAL_KIND_VIDEO;
  s_gal_photo_view  = !s_gal_video_view;
  s_gal_confirm_del = false;
  lv_async_call(gal_rebuild_async, nullptr);
}

/* Delete the shown item (photo view and player share this): first tap arms
 * the confirm, second does it. */
static void gal_del_cb(lv_event_t *e) {
  (void)e;
  if (s_gal_idx < 0 || s_gal_idx >= s_gal_list_n) return;
  if (!s_gal_confirm_del) {
    s_gal_confirm_del = true;
    if (s_gal_video_view) gal_vid_set_playing(false);   // freeze while deciding
    gal_set_status("Tap delete again to confirm", 0xFFD60A);
    return;
  }
  char path[160];
  gal_item_path(s_gal_idx, path, sizeof(path));
  fs::FS &f = gal_item_fs(s_gal_idx);
  gal_vid_close();                    // release OUR handle before removing
  f.remove(path);
  gal_to_grid();
}

/* ---- photo-view zoom + pan (slider 1..4x; drag pans while zoomed; the
 * pager's swipe is disabled while zoomed so one finger means one thing) ---- */
static void gal_apply_zoom_pan(void) {
  lv_obj_t *img = s_gal_page_img[s_gal_center];
  if (!img || !s_gal_pager) return;
  const lv_img_dsc_t *d = &s_gal_page_dsc[s_gal_center];
  if (d->header.w == 0 || d->header.h == 0) return;

  const int cw = LCD_WIDTH, ch = s_gal_view_h;
  int zfit = (cw * 256) / d->header.w;
  int zf2  = (ch * 256) / d->header.h;
  if (zf2 < zfit) zfit = zf2;              // CONTAIN
  if (zfit > 256) zfit = 256;              // never upscale the base view
  int z = zfit * s_gal_zoom_pct / 100;
  lv_img_set_zoom(img, z > 0 ? z : 1);

  const int dw = d->header.w * z / 256, dh = d->header.h * z / 256;
  int x, y;
  if (dw <= cw) { x = (cw - dw) / 2; s_gal_pan_x = x; }
  else {
    if (s_gal_pan_x > 0)       s_gal_pan_x = 0;
    if (s_gal_pan_x < cw - dw) s_gal_pan_x = cw - dw;
    x = s_gal_pan_x;
  }
  if (dh <= ch) { y = (ch - dh) / 2; s_gal_pan_y = y; }
  else {
    if (s_gal_pan_y > 0)       s_gal_pan_y = 0;
    if (s_gal_pan_y < ch - dh) s_gal_pan_y = ch - dh;
    y = s_gal_pan_y;
  }
  lv_obj_set_pos(img, x, y);
}

static void gal_zoom_slider_cb(lv_event_t *e) {
  lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
  s_gal_zoom_pct = (int)lv_slider_get_value(sl);
  if (s_gal_pager) {
    if (s_gal_zoom_pct > 100) lv_obj_clear_flag(s_gal_pager, LV_OBJ_FLAG_SCROLLABLE);
    else                      lv_obj_add_flag(s_gal_pager, LV_OBJ_FLAG_SCROLLABLE);
  }
  gal_apply_zoom_pan();
}

static void gal_pan_cb(lv_event_t *e) {
  (void)e;
  if (s_gal_zoom_pct <= 100) return;
  lv_indev_t *ind = lv_indev_active();
  if (!ind) return;
  lv_point_t v;
  lv_indev_get_vect(ind, &v);
  if (v.x == 0 && v.y == 0) return;
  s_gal_pan_x += v.x;
  s_gal_pan_y += v.y;
  gal_apply_zoom_pan();
}

/* Swipe landed on a neighbour page: adopt its item and rebuild there. */
static void gal_pager_end_cb(lv_event_t *e) {
  (void)e;
  if (!s_gal_pager || s_gal_cells <= 0 || s_gal_zoom_pct > 100) return;
  int p = (lv_obj_get_scroll_x(s_gal_pager) + LCD_WIDTH / 2) / LCD_WIDTH;
  if (p < 0) p = 0;
  if (p >= s_gal_cells) p = s_gal_cells - 1;
  if (s_gal_cell_idx[p] != s_gal_idx) {
    s_gal_idx = s_gal_cell_idx[p];
    s_gal_confirm_del = false;
    lv_async_call(gal_rebuild_async, nullptr);
  }
}

/* The nearest PHOTO neighbours of the current photo, list-order. Photos are
 * not contiguous (sections interleave videos), so scan, don't assume. */
static bool gal_is_photo(int j) {
  return s_gal_kind[j] == GAL_KIND_PHOTO && !s_gal_err[j];
}
static int gal_prev_photo(int from) {
  for (int j = from - 1; j >= 0; j--) if (gal_is_photo(j)) return j;
  return -1;
}
static int gal_next_photo(int from) {
  for (int j = from + 1; j < s_gal_list_n; j++) if (gal_is_photo(j)) return j;
  return -1;
}

/* ============================ the views ================================ */

/* One flat button for the bottom rows. */
static lv_obj_t *gal_button(lv_obj_t *parent, const char *txt, uint32_t accent,
                            lv_event_cb_t cb, void *user) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_radius(b, UI_PX(12), 0);
  lv_obj_set_style_pad_hor(b, UI_PX(12), 0);
  lv_obj_set_style_pad_ver(b, UI_PX(8), 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(accent), 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}

/* ================= GRID =================
 * Location-sectioned 3-column grid of square cover-cropped thumbnails. */
static void app_open_gallery_grid(void) {
  gal_free_bufs();
  lv_obj_t *scr = app_screen_begin("Gallery");
  nav_back_intercept = nullptr;            // grid is the app root: BOOT exits

  /* Caption under the grid: counts, or the delete-confirm toast. */
  s_gal_status = lv_label_create(scr);
  lv_obj_set_style_text_font(s_gal_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(s_gal_status, lv_color_hex(0x999999), 0);
  lv_obj_set_width(s_gal_status, LV_PCT(92));
  lv_obj_set_style_text_align(s_gal_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_gal_status, LV_LABEL_LONG_WRAP);
  lv_label_set_text(s_gal_status, "");
  lv_obj_align(s_gal_status, LV_ALIGN_BOTTOM_MID, 0, -UI_PX(8));

  gal_build_list();

  const int GAP  = UI_PX(6);
  const int gw   = LCD_WIDTH - UI_PX(16);
  const int cell = (gw - 2 * GAP) / 3;
  lv_obj_t *grid = lv_obj_create(scr);
  lv_obj_remove_style_all(grid);
  lv_obj_set_width(grid, gw);
#if BOARD_SCREEN_NARROW
  const int grid_top = UI_PX(112);         // below the wrapped title
#elif BOARD_SCREEN_SUBREF
  const int grid_top = UI_PX(88);          // title sits at 46 on this tier
#else
  const int grid_top = UI_PX(76);
#endif
  lv_obj_set_height(grid, LCD_HEIGHT - grid_top - UI_PX(30));
  lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, grid_top);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(grid, GAP, 0);
  lv_obj_set_style_pad_row(grid, GAP, 0);
  lv_obj_set_scroll_dir(grid, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);

  if (s_gal_list_n == 0) {
    lv_obj_t *empty = lv_label_create(grid);
    lv_obj_set_style_text_font(empty, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x777777), 0);
    lv_label_set_text(empty, (sd_mount() || ffat_mount())
                               ? "No photos or videos found"
                               : "No storage");
    return;
  }

  int photos = 0, vids = 0, others = 0;
  int last_src = -1;
  for (int i = 0; i < s_gal_list_n; i++) {
    /* SECTION HEADER whenever the source changes — a full-width child forces
     * the wrap-flex onto a new row, so the header naturally spans the grid. */
    if ((int)s_gal_src[i] != last_src) {
      last_src = s_gal_src[i];
      lv_obj_t *hdr = lv_label_create(grid);
      lv_obj_set_width(hdr, LV_PCT(100));
      lv_obj_set_style_text_font(hdr, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(hdr, lv_color_hex(0xAAAAAA), 0);
      lv_obj_set_style_pad_top(hdr, (i == 0) ? 0 : UI_PX(6), 0);
      lv_label_set_text(hdr, (last_src == GAL_SRC_CAM) ? "Camera" :
                             (last_src == GAL_SRC_SD)  ? "SD card" : "Internal");
    }

    /* Thumbnail: whatever the file is, decoded to cover the square cell.
     * Videos contribute their first frame. */
    bool ok = false;
    const char *err = "Not supported";
    if (s_gal_kind[i] != GAL_KIND_OTHER)
      ok = gal_decode_item(i, cell, cell, true, &s_gal_thumb_buf[i],
                           &s_gal_thumb_dsc[i], &err);
    if (!ok) s_gal_err[i] = err;

    if (s_gal_err[i] || s_gal_kind[i] == GAL_KIND_OTHER) others++;
    else if (s_gal_kind[i] == GAL_KIND_VIDEO)            vids++;
    else                                                 photos++;

    lv_obj_t *cellw = lv_btn_create(grid);
    lv_obj_set_size(cellw, cell, cell);
    lv_obj_set_style_radius(cellw, UI_PX(6), 0);
    lv_obj_set_style_clip_corner(cellw, true, 0);
    lv_obj_set_style_bg_color(cellw, lv_color_hex(ok ? 0x101010 : 0x181818), 0);
    lv_obj_set_style_bg_opa(cellw, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(cellw, 0, 0);
    lv_obj_set_style_pad_all(cellw, 0, 0);
    lv_obj_add_flag(cellw, HAPTICS_NO_BUZZ_FLAG);
    lv_obj_add_event_cb(cellw, gal_thumb_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    if (!ok) {
      /* PLACEHOLDER: the format's name, so an unsupported or broken file is
       * visibly present and says what it is instead of disappearing. */
      char ext[10];
      gal_ext_upper(s_gal_names[i], ext, sizeof(ext));
      lv_obj_t *l = lv_label_create(cellw);
      lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(0x888888), 0);
      lv_label_set_text(l, ext[0] ? ext : "?");
      lv_obj_center(l);
      lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
      continue;
    }

    lv_obj_t *im = lv_img_create(cellw);
    lv_img_set_src(im, &s_gal_thumb_dsc[i]);
    lv_image_set_antialias(im, false);
    lv_obj_center(im);
    /* COVER-crop: scale by the larger ratio; the cell clips the overflow. */
    int zx = (cell * 256) / s_gal_thumb_dsc[i].header.w;
    int zy = (cell * 256) / s_gal_thumb_dsc[i].header.h;
    int z  = (zx > zy) ? zx : zy;
    lv_img_set_zoom(im, z > 0 ? z : 1);
    lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (s_gal_kind[i] == GAL_KIND_VIDEO) {   // play badge: reads as video at a glance
      lv_obj_t *badge = lv_label_create(cellw);
      lv_obj_set_style_text_font(badge, &FONT_SMALL, 0);
      lv_obj_set_style_text_color(badge, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_bg_color(badge, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(badge, LV_OPA_50, 0);
      lv_obj_set_style_radius(badge, UI_PX(8), 0);
      lv_obj_set_style_pad_all(badge, UI_PX(4), 0);
      lv_label_set_text(badge, LV_SYMBOL_PLAY);
      lv_obj_center(badge);
      lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
  }

  char more[40] = "";
  if (s_gal_extra > 0)
    snprintf(more, sizeof(more), " (%d more not shown)", s_gal_extra);
  char skipped[24] = "";
  if (others > 0) snprintf(skipped, sizeof(skipped), ", %d other", others);
  char cap[128];
  if (vids > 0)
    snprintf(cap, sizeof(cap), "%d photo%s, %d video%s%s%s",
             photos, photos == 1 ? "" : "s", vids, vids == 1 ? "" : "s",
             skipped, more);
  else
    snprintf(cap, sizeof(cap), "%d photo%s%s%s", photos, photos == 1 ? "" : "s",
             skipped, more);
  gal_set_status(cap, 0x999999);
}

/* ================= FULL PHOTO VIEW =================
 * Snap-scrolling pager over the nearest photo neighbours (videos skipped);
 * slider zoom + one-finger pan; delete with confirm. */
static void app_open_gallery_photo(void) {
  gal_free_bufs();
  lv_obj_t *scr = app_screen_begin("");
  nav_back_intercept = gal_back_intercept;

  gal_build_list();                        // a delete may have changed the world
  if (s_gal_idx >= s_gal_list_n) s_gal_idx = s_gal_list_n - 1;
  if (s_gal_list_n == 0) { gal_to_grid(); return; }
  if (s_gal_idx < 0) s_gal_idx = 0;
  if (!gal_is_photo(s_gal_idx)) {           // landed on a video: nearest photo
    int p = gal_prev_photo(s_gal_idx);
    if (p < 0) p = gal_next_photo(s_gal_idx);
    if (p < 0) { gal_to_grid(); return; }
    s_gal_idx = p;
  }

  const int top = UI_PX(44);
  s_gal_view_h = LCD_HEIGHT - top - UI_PX(64);

  lv_obj_t *pager = lv_obj_create(scr);
  lv_obj_remove_style_all(pager);
  lv_obj_set_size(pager, LCD_WIDTH, s_gal_view_h);
  lv_obj_align(pager, LV_ALIGN_TOP_MID, 0, top);
  lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(pager, 0, 0);
  lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_add_event_cb(pager, gal_pager_end_cb, LV_EVENT_SCROLL_END, nullptr);
  s_gal_pager = pager;
  s_gal_cells = 0;

  int neighbours[3] = { gal_prev_photo(s_gal_idx), s_gal_idx,
                        gal_next_photo(s_gal_idx) };
  lv_obj_t *center_cell = nullptr;
  const char *center_err = nullptr;
  for (int k = 0; k < 3; k++) {
    int j = neighbours[k];
    if (j < 0) continue;
    int o = s_gal_cells;
    const char *err = nullptr;
    if (!gal_decode_item(j, LCD_WIDTH, s_gal_view_h, false,
                         &s_gal_page_buf[o], &s_gal_page_dsc[o], &err)) {
      if (j == s_gal_idx) center_err = err;
      continue;
    }

    lv_obj_t *cell = lv_obj_create(pager);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, LCD_WIDTH, s_gal_view_h);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *im = lv_img_create(cell);
    lv_img_set_src(im, &s_gal_page_dsc[o]);
    lv_image_set_antialias(im, false);
    lv_image_set_pivot(im, 0, 0);
    int zx = (LCD_WIDTH    * 256) / s_gal_page_dsc[o].header.w;
    int zy = (s_gal_view_h * 256) / s_gal_page_dsc[o].header.h;
    int z  = (zx < zy) ? zx : zy;
    if (z > 256) z = 256;
    lv_img_set_zoom(im, z > 0 ? z : 1);
    lv_obj_set_pos(im, (LCD_WIDTH - s_gal_page_dsc[o].header.w * z / 256) / 2,
                       (s_gal_view_h - s_gal_page_dsc[o].header.h * z / 256) / 2);

    s_gal_page_img[o] = im;
    s_gal_cell_idx[o] = j;
    if (j == s_gal_idx) {
      s_gal_center = o;
      center_cell  = cell;
      lv_obj_add_event_cb(cell, gal_pan_cb, LV_EVENT_PRESSING, nullptr);
    }
    s_gal_cells++;
  }

  lv_obj_update_layout(pager);
  if (center_cell) lv_obj_scroll_to_view(center_cell, LV_ANIM_OFF);

  /* Zoom slider, right edge. Pinch is impossible on single-touch panels, so
   * this IS the zoom control; while >100 the drag pans and swiping is off. */
  s_gal_zoom_pct = 100;
  s_gal_pan_x = s_gal_pan_y = 0;
  lv_obj_t *zsl = lv_slider_create(scr);
  lv_slider_set_range(zsl, 100, 400);
  lv_slider_set_value(zsl, 100, LV_ANIM_OFF);
  lv_obj_set_size(zsl, UI_PX(10), s_gal_view_h - UI_PX(28));
  lv_obj_align(zsl, LV_ALIGN_TOP_RIGHT, -UI_PX(6), top + UI_PX(14));
  lv_obj_set_style_bg_color(zsl, lv_color_hex(0x333333), LV_PART_MAIN);
  lv_obj_set_style_bg_color(zsl, lv_color_hex(0x888888), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(zsl, lv_color_hex(0xDDDDDD), LV_PART_KNOB);
  lv_obj_add_flag(zsl, HAPTICS_NO_BUZZ_FLAG);
  lv_obj_add_event_cb(zsl, gal_zoom_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  /* Caption: filename + photo position; doubles as the delete-confirm toast. */
  s_gal_status = lv_label_create(scr);
  lv_obj_set_style_text_font(s_gal_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(s_gal_status, lv_color_hex(0x999999), 0);
  lv_obj_set_width(s_gal_status, LV_PCT(92));
  lv_obj_set_style_text_align(s_gal_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_gal_status, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_gal_status, LV_ALIGN_BOTTOM_MID, 0, -UI_PX(38));
  if (center_err) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: %s", s_gal_names[s_gal_idx], center_err);
    gal_set_status(msg, 0xFF6060);
  } else {
    int ppos = 0, ptot = 0;
    for (int i = 0; i < s_gal_list_n; i++) {
      if (gal_is_photo(i)) { ptot++; if (i <= s_gal_idx) ppos++; }
    }
    char cap[96];
    snprintf(cap, sizeof(cap), "%s   %d / %d", s_gal_names[s_gal_idx], ppos, ptot);
    gal_set_status(cap, 0x999999);
  }

  /* Bottom row: back to the grid, delete. */
  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(92));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -UI_PX(4));
  gal_button(row, LV_SYMBOL_LIST,  0xFFFFFF, gal_to_grid_cb, nullptr);
  gal_button(row, LV_SYMBOL_TRASH, 0xFF6060, gal_del_cb, nullptr);
}

/* ================= VIDEO PLAYER ================= */

static void gal_vid_set_playing(bool p) {
  s_vid_playing = p && s_vid_open;
  if (!s_vid_play_btn) return;
  lv_obj_t *l = lv_obj_get_child(s_vid_play_btn, 0);
  if (l) lv_label_set_text(l, s_vid_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void gal_vid_play_cb(lv_event_t *e) {
  (void)e;
  if (!s_vid_open) return;
  gal_vid_set_playing(!s_vid_playing);
}

static void app_open_gallery_video(void) {
  gal_free_bufs();
  gal_vid_close();                         // a previous player session, if any
  lv_obj_t *scr = app_screen_begin("");
  nav_back_intercept = gal_back_intercept;

  gal_build_list();
  if (s_gal_idx >= s_gal_list_n) s_gal_idx = s_gal_list_n - 1;
  if (s_gal_idx < 0 || s_gal_list_n == 0 || s_gal_kind[s_gal_idx] != GAL_KIND_VIDEO) {
    gal_to_grid();
    return;
  }

  const int top = UI_PX(44);
  s_gal_view_h = LCD_HEIGHT - top - UI_PX(64);

  lv_obj_t *cellv = lv_obj_create(scr);
  lv_obj_remove_style_all(cellv);
  lv_obj_set_size(cellv, LCD_WIDTH, s_gal_view_h);
  lv_obj_align(cellv, LV_ALIGN_TOP_MID, 0, top);
  lv_obj_clear_flag(cellv, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(cellv, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(cellv, LV_OPA_COVER, 0);

  s_vid_img = lv_img_create(cellv);
  lv_image_set_antialias(s_vid_img, false);
  lv_image_set_pivot(s_vid_img, 0, 0);

  s_gal_status = lv_label_create(scr);
  lv_obj_set_style_text_font(s_gal_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(s_gal_status, lv_color_hex(0x999999), 0);
  lv_obj_set_width(s_gal_status, LV_PCT(92));
  lv_obj_set_style_text_align(s_gal_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_gal_status, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_gal_status, LV_ALIGN_BOTTOM_MID, 0, -UI_PX(38));

  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(92));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -UI_PX(4));
  gal_button(row, LV_SYMBOL_LIST, 0xFFFFFF, gal_to_grid_cb, nullptr);
  s_vid_play_btn = gal_button(row, LV_SYMBOL_PAUSE, 0xFFFFFF, gal_vid_play_cb, nullptr);
  gal_button(row, LV_SYMBOL_TRASH, 0xFF6060, gal_del_cb, nullptr);

  char path[160];
  gal_item_path(s_gal_idx, path, sizeof(path));
  const char *err = "Can't play this video";
  if (!mv_open(&s_vid, gal_item_fs(s_gal_idx), path, LCD_WIDTH, s_gal_view_h, &err)) {
    gal_vid_set_playing(false);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: %s", s_gal_names[s_gal_idx],
             err ? err : "Can't play");
    gal_set_status(msg, 0xFF6060);
    return;
  }
  s_vid_open = true;

  /* Autoplay at the file's own frame period (never faster than ~50 fps); a
   * slow decode just makes its tick late. First frame is painted right now. */
  s_vid_cap_s = ~0u;
  s_vid_ms    = 0;
  uint32_t period = s_vid.uspf / 1000;
  if (period < 20) period = 20;
  s_vid_timer = lv_timer_create(gal_vid_timer_cb, period, nullptr);
  gal_vid_set_playing(true);
  gal_vid_step();
}

/* ============================ entry / exit ============================== */

static void app_open_gallery(void) {
  gal_vid_close();                         // any previous view's leftovers
  if      (s_gal_video_view) app_open_gallery_video();
  else if (s_gal_photo_view) app_open_gallery_photo();
  else                       app_open_gallery_grid();
}

/* Leaving the app (nav_back / app_menu_close hooks): free every decoded
 * buffer and reset to the grid so the next visit starts fresh. */
static void app_gallery_on_close(void) {
  gal_vid_close();
  gal_free_bufs();
  s_gal_status      = nullptr;
  s_gal_photo_view  = false;
  s_gal_video_view  = false;
  s_gal_confirm_del = false;
  s_gal_idx         = 0;
}

#endif  /* OWF_HAS_GALLERY */
