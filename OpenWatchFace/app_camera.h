/* ============================================================================
 *  app_camera.h — "Camera" sub-app: the viewfinder — photos and video.
 *
 *  Present ONLY on boards that declare BOARD_HAS_CAMERA (the ESP32-S3-Touch-LCD-2
 *  with an OV5640 on its 24-pin header). On every other board this file compiles to
 *  nothing AND its menu tile is excluded, so a watch with no sensor never shows an
 *  app that could only fail — same model as the IMU-gated Fitness/Sleep tiles.
 *
 *  This app is the CAPTURE side only: a full-bleed live RGB565 preview with the
 *  mode-dial shutters (white disc = photo, red disc = video; a horizontal swipe
 *  spins them into place), zoom/brightness/contrast/resolution chips, and the
 *  MJPEG-AVI recorder (camera_rec.h). BROWSING lives in the standalone Gallery
 *  app (app_gallery.h) — the topbar's picture chip jumps straight there.
 *
 *  WHERE CAPTURES GO. store_fs() (storage_fs.h) — the microSD when one is
 *  mounted, else the on-flash FFat partition. Photos are DCIM/IMG_<n>.JPG,
 *  videos DCIM/VID_<n>.AVI, each `n` a zero-padded counter chosen by scanning
 *  the directory, so names sort chronologically and never collide even though
 *  the watch may have no valid wall-clock time set. /DCIM is exactly where the
 *  Gallery's "Camera" section looks.
 *
 *  MEMORY. Preview frames and the captured JPEG live in PSRAM (the board has
 *  8 MB), and the camera driver is deinitialised on exit so an idle watch pays
 *  neither the framebuffer nor the sensor's power draw.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE AFTER app_menu.h (screen shell +
 *  nav_open/nav_back), camera_dev.h (the sensor wrapper), storage_fs.h
 *  (store_fs/store_available) and app_gallery.h (the topbar chip's target).
 *  app_open_camera is forward-declared in app_menu.h so the tile can dispatch. */
#pragma once
#if BOARD_HAS_CAMERA

#include <lvgl.h>
#include <FS.h>
#include <math.h>         // sqrtf - pinch finger separation; sinf/cosf - mode dial
#include "camera_dev.h"
#include "esp_heap_caps.h"

#define CAM_DIR       "/DCIM"
#define CAM_MAX_SHOTS 999      // IMG_000..IMG_999 — the name counter's range

#include "camera_rec.h"   // MJPEG-AVI recorder (needs CAM_DIR + camera_dev.h)

static void app_open_camera(void);
static void app_camera_on_close(void);   // used by the Gallery-app jump below
static void cam_update_zoom_label(void);
static void cam_toolbar_refresh(void);
static void cam_rebuild_async(void *unused);
static void cam_rec_btn_refresh(void);
static void cam_set_status(const char *msg, uint32_t color);

/* ---- overlay widgets (rebuilt on every open) ---- */
static lv_obj_t *s_cam_zoom_lbl = nullptr;  // "1.5x" readout in the toolbar
static lv_obj_t *s_cam_res_lbl  = nullptr;  // active resolution readout (top-right)
static lv_obj_t *s_cam_bri_btn  = nullptr;  // per-setting toolbar buttons
static lv_obj_t *s_cam_con_btn  = nullptr;
static lv_obj_t *s_cam_sat_btn  = nullptr;
static lv_obj_t *s_cam_res_btn  = nullptr;

/* ---- the two shutters + the mode dial -------------------------------------
 * PHOTO (white disc) and VIDEO (red disc) are two shutter buttons sitting on
 * opposite ends of an INVISIBLE CIRCLE whose top is the classic shutter spot
 * above the toolbar. Only the active one is visible — the other waits at the
 * circle's bottom, below the screen edge. A horizontal swipe on the viewfinder
 * (or across the shutter itself) spins the circle 180°: the active disc arcs
 * down one side and off-screen while the other arcs up the opposite side into
 * place. s_cam_mode_angle is the dial's angle in degrees (0 = photo up,
 * 180 = video up); it is the lv_anim variable, so it doubles as the anim's
 * identity for lv_anim_del on teardown. */
#define CAM_SHUTTER_D 56
static lv_obj_t *s_cam_shutter  = nullptr;  // photo shutter (white)
static lv_obj_t *s_cam_rec_btn  = nullptr;  // video shutter (red) — tap = rec toggle
static bool      s_cam_mode_video = false;  // which shutter is "up" (persists)
static int32_t   s_cam_mode_angle = 0;      // dial angle, animated on switch
static int       s_cam_circ_cx = 0, s_cam_circ_cy = 0, s_cam_circ_r = 0;
/* True for the whole capture-and-save of a still. The dial refuses to spin
 * while it is set (and while cam_rec_active(), which covers recording AND the
 * background AVI finalise) — a stray swipe must never yank the controls out
 * from under an in-flight capture. Also doubles as the shutter's re-entry
 * guard. */
static bool      s_cam_busy = false;

/* ---- pinch-to-zoom sampler state ----
 * s_cam_pinch_ref is the finger separation (in px) at the moment the gesture
 * started, or 0 when no pinch is in progress. Sampled from the preview timer
 * rather than an LVGL gesture, because LVGL's indev only carries ONE point — the
 * second finger has to come straight from the touch driver. */
static int32_t s_cam_pinch_ref  = 0;
static int     s_cam_pinch_zoom0 = CAM_ZOOM_MIN;   // zoom factor when the pinch began

/* ---- live-preview state ---- */
static lv_obj_t   *s_cam_prev_img   = nullptr;  // the LVGL image showing the preview
static lv_timer_t *s_cam_prev_timer = nullptr;  // drives the preview refresh
static lv_img_dsc_t s_cam_prev_dsc;             // descriptor pointing at the frame below
static uint8_t    *s_cam_prev_buf   = nullptr;  // our OWN copy of the latest frame
static lv_obj_t   *s_cam_status     = nullptr;  // one-line status/toast under the buttons

/* Fill an image descriptor for a raw RGB565 bitmap.
 *
 * LVGL 9 requires TWO header fields that LVGL 8 did not: `magic` (the descriptor is
 * rejected outright without LV_IMAGE_HEADER_MAGIC) and `stride` (bytes per row —
 * left at 0 the renderer computes nothing and draws garbage). Both are easy to
 * forget because the v8 compat map keeps the old lv_img_* call names working, so
 * set them in ONE place that every image here goes through. */
static void cam_fill_dsc(lv_img_dsc_t *d, const uint8_t *data, int w, int h) {
  memset(d, 0, sizeof(*d));
  d->header.magic  = LV_IMAGE_HEADER_MAGIC;
  d->header.cf     = LV_COLOR_FORMAT_RGB565;
  d->header.w      = w;
  d->header.h      = h;
  d->header.stride = w * 2;              // RGB565 = 2 bytes per pixel
  d->data          = data;
  d->data_size     = (size_t)w * h * 2;
}

/* ============================ file helpers ============================== */

/* Strip any directory part from a name the FS gave us. Backends differ: some
 * File::name()s are bare ("IMG_007.JPG"), others full paths ("/DCIM/IMG_007.JPG"),
 * so normalise before parsing or comparing. */
static const char *cam_basename(const char *nm) {
  const char *slash = strrchr(nm, '/');
  return slash ? slash + 1 : nm;
}

/* The counter in IMG_<n>.JPG, or -1 if the name isn't one of ours. This is the
 * SINGLE definition of "is this a photo we manage" — counting, ordering and
 * next-name allocation all go through it, so they can never disagree about which
 * files are in the set (a stray .JPG dropped in /DCIM by hand is ignored by all
 * three rather than counted by one and skipped by another). */
static int cam_photo_seq(const char *nm) {
  int v;
  return (sscanf(cam_basename(nm), "IMG_%d.JPG", &v) == 1) ? v : -1;
}

/* Pick the next free IMG_<n>.JPG. Scans for the highest existing number and adds
 * one, so deleting older photos never causes a name collision with a survivor. */
static bool cam_next_path(char *out, size_t out_sz) {
  if (!store_available()) return false;
  if (!store_fs().exists(CAM_DIR)) store_fs().mkdir(CAM_DIR);

  int highest = -1;
  File dir = store_fs().open(CAM_DIR);
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory()) {
        int v = cam_photo_seq(f.name());
        if (v > highest) highest = v;
      }
      f.close();
    }
    dir.close();
  }
  if (highest >= CAM_MAX_SHOTS) return false;    // counter exhausted
  snprintf(out, out_sz, CAM_DIR "/IMG_%03d.JPG", highest + 1);
  return true;
}

/* ============================ preview ================================== */

static void cam_free_preview_buf(void) {
  if (s_cam_prev_buf) { heap_caps_free(s_cam_prev_buf); s_cam_prev_buf = nullptr; }
}

/* ---- preview timing probe --------------------------------------------------
 * Accumulates where each preview tick's time goes and prints a one-line summary
 * every ~2 s. The gap between the printed stage costs and the tick-to-tick
 * period is LVGL's render + flush of the invalidated preview — the one stage
 * that can't be timed from inside the tick. Costs a few counters per tick; the
 * print itself is one line every 2 s, so it stays in the shipped build. */
static uint32_t s_cam_dbg_ticks    = 0;   // ticks that actually showed a frame
static uint32_t s_cam_dbg_fbget_us = 0;   // waiting on esp_camera_fb_get
static uint32_t s_cam_dbg_copy_us  = 0;   // memcpy + byte-swap
static uint32_t s_cam_dbg_win_ms   = 0;   // start of the current 2 s window

/* Stop the preview timer and release the camera. Called when leaving the Camera
 * tab, leaving the app, or before a capture rebuild — anywhere the live view must
 * not keep running against a destroyed widget. */
static void cam_stop_preview(void) {
  /* A recording cannot outlive its frame source: leaving the camera view (tab
   * switch, app exit, any rebuild) ends the session — the writer task drains
   * what's buffered and finalises the file in the background. */
  cam_rec_stop();
  if (s_cam_prev_timer) { lv_timer_del(s_cam_prev_timer); s_cam_prev_timer = nullptr; }
  s_cam_prev_img = nullptr;
  cam_free_preview_buf();
  /* reset the timing probe so the next session's first line isn't averaged
   * across the gap since the last one */
  s_cam_dbg_ticks = s_cam_dbg_fbget_us = s_cam_dbg_copy_us = 0;
  s_cam_dbg_win_ms = 0;
}

/* Pull one frame and show it. Runs on the LVGL thread via lv_timer.
 *
 * The frame is COPIED into our own PSRAM buffer and the driver's framebuffer is
 * returned immediately. Handing LVGL the driver's buffer directly would mean
 * either holding the only framebuffer until the next tick (starving the sensor) or
 * letting LVGL read memory the driver may already be overwriting. */
/* ---- pinch-to-zoom sampler -------------------------------------------------
 * Called once per preview tick. Reads the touch controller DIRECTLY (not through
 * LVGL) because LVGL's pointer indev carries a single point by design — the second
 * finger is only visible at the driver level.
 *
 * THIS MAY DO NOTHING ON THIS HARDWARE, BY DESIGN. The CST816 family is documented
 * as single-touch: its register map defines one X/Y pair, and only some dies
 * populate a second slot. cst816_read2() returns false unless the controller
 * genuinely reports two distinct, in-range points, so on a single-touch die this
 * function is a cheap no-op every tick and the +/- chips remain the way to zoom.
 * If the die DOES report two fingers, pinch starts working with no code change —
 * and the first successful read latches cst816_has_multitouch() so the fact is
 * discoverable (it is logged once, below).
 *
 * Gesture model: remember the finger separation when the second finger lands, then
 * scale the zoom by how far that separation has changed. Absolute, not
 * incremental, so drift can't accumulate over a long pinch.
 *
 * i2c_lock() is held for the read: the QMI8658 shares this bus. */
static void cam_pinch_tick(void) {
#if BOARD_TOUCH_CST816
  int32_t x1, y1, x2, y2;
  i2c_lock();
  bool two = cst816_read2(&x1, &y1, &x2, &y2);
  i2c_unlock();

  if (!two) { s_cam_pinch_ref = 0; return; }      // lifted / single-touch panel

  if (!s_cst_multitouch_seen) {                    // first ever 2-finger report
    s_cst_multitouch_seen = true;
    USBSerial.println("[touch] CST816 reports 2 points - pinch enabled");
  }

  int32_t dx = x2 - x1, dy = y2 - y1;
  int32_t dist = (int32_t)sqrtf((float)(dx * dx + dy * dy));
  if (dist < 8) return;                            // too close to be meaningful

  if (s_cam_pinch_ref == 0) {                      // gesture just started
    s_cam_pinch_ref  = dist;
    s_cam_pinch_zoom0 = cam_zoom_get();
    return;
  }

  /* zoom scales with the ratio of current to initial separation. */
  int target = (int)((int64_t)s_cam_pinch_zoom0 * dist / s_cam_pinch_ref);
  cam_zoom_set(target, cam_preview_w(), cam_preview_h());
  cam_update_zoom_label();
#endif
}

static void cam_preview_tick(lv_timer_t *t) {
  (void)t;
  if (!s_cam_prev_img) return;
  uint32_t dbg_t0 = micros();
  camera_fb_t *fb = cam_preview_get();
  if (!fb) return;
  uint32_t dbg_t1 = micros();

  size_t need = (size_t)fb->width * fb->height * 2;
  if (fb->format == PIXFORMAT_RGB565 && fb->len >= need) {
    /* Recording tap: hand the RAW (big-endian) frame to the recorder ring
     * BEFORE the display's byte-swap copy — the encoder wants sensor order.
     * ~5 ms memcpy when recording, nothing at all when not. */
    cam_rec_feed(fb->buf, (int)fb->width, (int)fb->height);
    if (!s_cam_prev_buf) {
      s_cam_prev_buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_cam_prev_buf) {
      /* FUSED copy + byte-swap, one pass.
       *
       * The sensor emits BIG-endian RGB565, but LVGL's in-memory format here is
       * LITTLE-endian (the flush path byte-swaps on its way to the panel — see
       * lv_draw_sw_rgb565_swap in my_disp_flush). Without the swap the preview
       * renders with badly wrong colours — and not obviously so on a test
       * pattern: 0x0000/0xFFFF are swap-invariant, so black and white look right
       * while every real colour is wrong.
       *
       * Fused rather than memcpy + lv_draw_sw_rgb565_swap because both source and
       * destination live in PSRAM: the two-pass version reads AND writes the whole
       * frame twice over the slow external bus, and the timing probe measured it
       * at ~15 ms/frame — a third of the whole frame budget. One pass (read a
       * 32-bit word, swap the bytes of both pixels in registers, write it) halves
       * that traffic. `need` is w*h*2 with even w, so it's always 4-aligned. */
      const uint32_t *src = (const uint32_t *)fb->buf;
      uint32_t       *dst = (uint32_t *)s_cam_prev_buf;
      for (size_t i = 0, nw = need / 4; i < nw; i++) {
        uint32_t v = src[i];
        dst[i] = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
      }

      cam_fill_dsc(&s_cam_prev_dsc, s_cam_prev_buf, fb->width, fb->height);
      lv_img_set_src(s_cam_prev_img, &s_cam_prev_dsc);

      /* 1:1 CENTER-CROP BLIT — never any scaling.
       *
       * LVGL's software transform (the old cover-upscale of the 320-wide frame
       * onto the 240-wide panel) was the single biggest cost in the preview
       * loop, ~70 ms/frame measured. Instead: show the frame at zoom 256 (LVGL's
       * no-transform fast path) and let the sized image object + centered inner
       * align CROP it to the panel — clipping is free, scaling is not. The
       * viewfinder shows the central 240 columns of the 320-wide frame (75% of
       * the photo's width — MORE than the 56% the old cover-crop showed) at
       * full sensor speed; the height letterboxes behind the dark overlay bars.
       *
       * The aspect crop composes with it: vis_h trims 16:9 to its true band
       * first, then the width crop applies the same way. */
      if (fb->width > 0 && fb->height > 0) {
        int vis_h = fb->height * cam_preview_crop_pct() / 100;   // aspect band
        if (vis_h < 1) vis_h = 1;
        int shown_w = (fb->width > LCD_WIDTH)  ? LCD_WIDTH  : fb->width;
        int shown_h = (vis_h     > LCD_HEIGHT) ? LCD_HEIGHT : vis_h;
        /* The object is the crop rectangle; the source centres inside it, so
         * everything outside is clipped away rather than displayed. */
        lv_obj_set_size(s_cam_prev_img, shown_w, shown_h);
        lv_image_set_inner_align(s_cam_prev_img, LV_IMAGE_ALIGN_CENTER);
        lv_img_set_zoom(s_cam_prev_img, 256);      // 1:1 — no transform, ever
      }
      lv_obj_invalidate(s_cam_prev_img);

      /* ---- timing probe bookkeeping ---- */
      s_cam_dbg_ticks++;
      s_cam_dbg_fbget_us += (uint32_t)(dbg_t1 - dbg_t0);
      s_cam_dbg_copy_us  += (uint32_t)(micros() - dbg_t1);
      uint32_t now_ms = millis();
      if (s_cam_dbg_win_ms == 0) s_cam_dbg_win_ms = now_ms;
      if (now_ms - s_cam_dbg_win_ms >= 2000) {
        uint32_t n = s_cam_dbg_ticks ? s_cam_dbg_ticks : 1;
        uint32_t fps10 = n * 10000 / (now_ms - s_cam_dbg_win_ms);   // fps x10
        USBSerial.printf("[cam] %lu.%lu fps | fb_get %lu us | copy+swap %lu us (avg/frame)\n",
                         (unsigned long)(fps10 / 10), (unsigned long)(fps10 % 10),
                         (unsigned long)(s_cam_dbg_fbget_us / n),
                         (unsigned long)(s_cam_dbg_copy_us / n));
        s_cam_dbg_ticks = s_cam_dbg_fbget_us = s_cam_dbg_copy_us = 0;
        s_cam_dbg_win_ms = now_ms;
      }
    }
  }
  cam_preview_done(fb);

  /* REC readout: tick the timer once a second while recording, and flip to a
   * "saved" toast when the writer task finishes finalising in the background. */
  static bool     s_rec_was     = false;
  static uint32_t s_rec_shown_s = ~0u;
  bool rec_now = cam_rec_active();
  if (rec_now) {
    uint32_t s = cam_rec_seconds();
    if (s != s_rec_shown_s) {
      char t[24];
      snprintf(t, sizeof(t), LV_SYMBOL_VIDEO " REC %lu:%02lu",
               (unsigned long)(s / 60), (unsigned long)(s % 60));
      cam_set_status(t, 0xFF4040);
      s_rec_shown_s = s;
    }
  } else if (s_rec_was) {
    cam_set_status("Video saved", 0x32D74B);
    s_rec_shown_s = ~0u;
    cam_rec_btn_refresh();     // stop request completed -> icon back to record
  }
  s_rec_was = rec_now;

  cam_pinch_tick();      // sample the second finger on the same cadence
}

/* ============================ actions ================================== */

static void cam_set_status(const char *msg, uint32_t color) {
  if (!s_cam_status) return;
  lv_label_set_text(s_cam_status, msg);
  lv_obj_set_style_text_color(s_cam_status, lv_color_hex(color), 0);
  /* Hide the whole panel when there is no message: an empty translucent slab is
   * pure chrome on a viewfinder. (Gallery keeps its own always-visible caption —
   * it passes a non-empty string every time.) */
  if (msg && msg[0]) lv_obj_clear_flag(s_cam_status, LV_OBJ_FLAG_HIDDEN);
  else               lv_obj_add_flag(s_cam_status, LV_OBJ_FLAG_HIDDEN);
}

/* Shutter. Grabs a JPEG and writes it to the store. Runs inside the s_cam_busy
 * latch (see cam_shutter_cb below), so every early return here still clears it. */
static void cam_shutter_do(void) {
  if (!store_available()) { cam_set_status("No storage", 0xFF6060); return; }

  /* While recording, only the INSTANT still path is allowed (it just encodes a
   * live frame). A re-init still (320x320) would tear down the very driver the
   * recording is fed from. */
  if (cam_rec_active()) {
    const cam_res_t *r = cam_res();
    if (r->w != cam_preview_w() || r->h != cam_preview_h()) {
      cam_set_status("Stop recording first", 0xFFD60A);
      return;
    }
  }

  cam_set_status("Capturing...", 0xFFFFFF);
  lv_refr_now(nullptr);            // paint the message BEFORE the blocking capture

  uint8_t *jpg = nullptr;
  size_t   len = 0;
  if (!cam_capture_jpeg(&jpg, &len)) { cam_set_status("Capture failed", 0xFF6060); return; }

  char path[128];
  if (!cam_next_path(path, sizeof(path))) {
    heap_caps_free(jpg);
    cam_set_status("Storage full", 0xFF6060);
    return;
  }

  File f = store_fs().open(path, FILE_WRITE);
  if (!f) { heap_caps_free(jpg); cam_set_status("Write failed", 0xFF6060); return; }
  size_t wrote = f.write(jpg, len);
  f.close();
  heap_caps_free(jpg);

  if (wrote != len) { cam_set_status("Write failed", 0xFF6060); return; }

  char msg[64];
  snprintf(msg, sizeof(msg), "Saved %s (%uK)", strrchr(path, '/') + 1,
           (unsigned)(len / 1024));
  cam_set_status(msg, 0x32D74B);
}

static void cam_shutter_cb(lv_event_t *e) {
  (void)e;
  if (s_cam_busy) return;               // a capture is already in flight
  s_cam_busy = true;
  cam_shutter_do();
  s_cam_busy = false;
}

/* Rebuild the screen AFTER the current event finishes.
 *
 * Never call app_open_camera() directly from a button callback: it destroys the
 * whole screen — including the very button LVGL is still dispatching this event
 * for — and LVGL goes on to touch that freed object when the callback returns.
 * lv_async_call defers the rebuild to the next LVGL tick, when nothing is mid-
 * dispatch. This is the same pattern the Files app uses (fm_rebuild_async). */
static void cam_rebuild_async(void *unused) {
  (void)unused;
  app_open_camera();
}

/* The topbar's picture chip: jump to the STANDALONE Gallery app. Deferred via
 * lv_async_call (this runs inside a button event; the transition tears down the
 * screen the button lives on). The camera is fully released first — the Gallery
 * needs the SD bus and PSRAM far more than a background sensor does — and
 * nav_open pushes us onto the nav stack, so backing out of the Gallery lands
 * right back here (app_open_camera re-inits the sensor fresh). */
#if OWF_HAS_GALLERY
static void cam_to_gallery_async(void *unused) {
  (void)unused;
  app_camera_on_close();
  nav_open(app_open_gallery);
}
static void cam_open_gallery_cb(lv_event_t *e) {
  (void)e;
  lv_async_call(cam_to_gallery_async, nullptr);
}
#endif

/* ============================ the screen =============================== */

/* ---- overlay chrome -------------------------------------------------------
 * A translucent horizontal bar pinned to an edge of the preview. Dark and
 * semi-transparent so the controls stay legible over a bright scene without
 * hiding it. LV_OBJ_FLAG_IGNORE_LAYOUT is not needed (the parent has no layout);
 * the bar sizes to its content and is aligned by the caller. */
static lv_obj_t *cam_overlay_bar(lv_obj_t *parent, lv_align_t align, int y_ofs) {
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_width(bar, LV_PCT(96));
  lv_obj_set_height(bar, LV_SIZE_CONTENT);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);      // see the scene through the chrome
  lv_obj_set_style_radius(bar, UI_PX(10), 0);
  lv_obj_set_style_pad_all(bar, UI_PX(4), 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, UI_PX(4), 0);
  lv_obj_align(bar, align, 0, y_ofs);
  return bar;
}

/* A compact round control for an overlay bar. Deliberately a fixed touch size
 * rather than UI_PX-scaled text padding: these are finger targets on a small
 * panel, so they must not shrink with the font tier. */
static lv_obj_t *cam_chip(lv_obj_t *parent, const char *txt, uint32_t fg,
                          lv_event_cb_t cb, void *user) {
  lv_obj_t *b = lv_btn_create(parent);
  const int d = 34;                                 // absolute px: a comfortable tap
  lv_obj_set_size(b, d, d);
  lv_obj_set_style_radius(b, d / 2, 0);
  /* A LIGHTER fill than the bar it sits on, plus a hairline border. On the previous
   * near-black fill the chips vanished into the overlay and the row read as one
   * large empty band with stray glyphs in it; the contrast step is what makes each
   * chip legible AS A BUTTON. */
  lv_obj_set_style_bg_color(b, lv_color_hex(0x484848), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(0x9A9A9A), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_opa(b, LV_OPA_80, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_add_flag(b, HAPTICS_NO_BUZZ_FLAG);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}

/* A transparent, layout-only sub-group for an overlay bar. Used to bind the
 * [-] 1.0x [+] trio together: without it the bar's SPACE_EVENLY spreads all five
 * controls across the full width, stranding the zoom readout miles from its own
 * buttons. Grouped, the three sit tight and the GROUP is what gets spaced. */
static lv_obj_t *cam_group(lv_obj_t *parent, int gap_px) {
  lv_obj_t *g = lv_obj_create(parent);
  lv_obj_remove_style_all(g);
  lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(g, gap_px, 0);
  return g;
}

static void cam_update_zoom_label(void) {
  if (!s_cam_zoom_lbl) return;
  int z = cam_zoom_get();
  char t[16];
  /* Two decimals: the step is 0.25x, so "1.2x" and "1.3x" would both round from
   * 1.25 and the readout would look stuck. */
  snprintf(t, sizeof(t), "%d.%02dx", z / 100, z % 100);
  lv_label_set_text(s_cam_zoom_lbl, t);
}

/* Refresh the toolbar's live readouts in place. Called after a picker applies a
 * change — deliberately NOT a screen rebuild, which would tear down the preview
 * image and restart the camera for what is a two-label update. */
static void cam_toolbar_refresh(void) {
  cam_update_zoom_label();
  if (s_cam_res_lbl) {
    const cam_res_t *r = cam_res();
    char t[24];
    /* Just the size — the aspect is fixed (1:1) on this board and the square
     * viewfinder already says so. */
    snprintf(t, sizeof(t), "%ux%u", (unsigned)r->w, (unsigned)r->h);
    lv_label_set_text(s_cam_res_lbl, t);
  }
}

/* Zoom +/- chip. Applies to the PREVIEW output size; the still re-applies the
 * same factor at capture time so the photo matches what was framed. */
static void cam_zoom_cb(lv_event_t *e) {
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  cam_zoom_set(cam_zoom_get() + dir * CAM_ZOOM_STEP, cam_preview_w(), cam_preview_h());
  cam_update_zoom_label();
}

/* ---- setting pickers -------------------------------------------------------
 * Every setting is its OWN icon button in the bottom toolbar, and tapping one
 * opens a LIST to choose from. Deliberately NOT cycle-on-tap: with 6 resolutions
 * a cycling button makes you tap five times to see what the options even are, and
 * you can never jump straight to one. A list shows the whole range at once, marks
 * the current entry, and applies on selection.
 *
 * ONE shared picker object serves every setting — it is rebuilt per open from a
 * small descriptor (title + item strings + an apply callback), so adding another
 * setting later is a table entry and a callback, not another panel.
 *
 * The picker covers the toolbar that opened it, so like the old adjust panel it
 * carries its own close (a header X) AND hooks nav_back_intercept so BOOT
 * dismisses it instead of leaving the app. */

#define CAM_PICK_MAX 20                   /* most items any one picker offers */

typedef void (*cam_pick_apply_fn)(int idx);

static lv_obj_t        *s_cam_picker    = nullptr;   /* the open list, or null */
static cam_pick_apply_fn s_cam_pick_fn  = nullptr;
static int              s_cam_pick_n    = 0;

static void cam_picker_close(void) {
  if (s_cam_picker) { lv_obj_del(s_cam_picker); s_cam_picker = nullptr; }
  s_cam_pick_fn = nullptr;
  nav_back_intercept = nullptr;
}

static bool cam_picker_back_intercept(void) {
  if (s_cam_picker) { cam_picker_close(); return true; }
  return false;
}

static void cam_picker_close_cb(lv_event_t *e) { (void)e; cam_picker_close(); }

static void cam_picker_item_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  cam_pick_apply_fn fn = s_cam_pick_fn;     /* copy: close() clears the global */
  cam_picker_close();
  if (fn) fn(idx);
  /* Rebuilding the whole screen would tear down the preview; the toolbar labels
   * are refreshed in place instead (see cam_toolbar_refresh). */
  cam_toolbar_refresh();
}

/* Open a picker. `items` are the row labels, `cur` is highlighted as current. */
static void cam_picker_open(const char *title, const char *const *items, int n,
                            int cur, cam_pick_apply_fn apply) {
  cam_picker_close();                       /* only one open at a time */
  if (n > CAM_PICK_MAX) n = CAM_PICK_MAX;
  s_cam_pick_fn = apply;
  s_cam_pick_n  = n;

  lv_obj_t *p = lv_obj_create(app_scr);
  lv_obj_set_width(p, LV_PCT(88));
  lv_obj_set_height(p, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(p, LV_PCT(76), 0);   /* tall lists scroll */
  lv_obj_set_style_bg_color(p, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(p, lv_color_hex(0x555555), 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_radius(p, UI_PX(10), 0);
  lv_obj_set_style_pad_all(p, UI_PX(6), 0);
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(p, UI_PX(4), 0);
  lv_obj_center(p);

  /* header: title + close */
  lv_obj_t *hdr = lv_obj_create(p);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_width(hdr, LV_PCT(100));
  lv_obj_set_height(hdr, LV_SIZE_CONTENT);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_t *t = lv_label_create(hdr);
  lv_obj_set_style_text_font(t, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(0xAAAAAA), 0);
  lv_label_set_text(t, title);
  cam_chip(hdr, LV_SYMBOL_CLOSE, 0xFFFFFF, cam_picker_close_cb, nullptr);

  for (int i = 0; i < n; i++) {
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, lv_color_hex(i == cur ? 0x2A4A6A : 0x232323), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, UI_PX(6), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_ver(b, UI_PX(6), 0);
    lv_obj_set_style_pad_hor(b, UI_PX(8), 0);
    lv_obj_add_flag(b, HAPTICS_NO_BUZZ_FLAG);
    lv_obj_add_event_cb(b, cam_picker_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(i == cur ? 0xFFFFFF : 0xCCCCCC), 0);
    /* A tick marks the active entry, so the current value is obvious at a glance
     * without relying on the highlight colour alone. */
    lv_label_set_text_fmt(l, "%s%s", (i == cur) ? LV_SYMBOL_OK "  " : "", items[i]);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  }

  /* Long lists open SCROLLED TO the current value — the window stays the same
   * size (max_height above) and the -8..+8 ranges just scroll within it. The
   * layout pass must run first or every row still reports y=0. */
  lv_obj_update_layout(p);
  if (cur >= 0 && cur < n) {
    lv_obj_t *row = lv_obj_get_child(p, cur + 1);   /* child 0 is the header */
    if (row) lv_obj_scroll_to_view(row, LV_ANIM_OFF);
  }

  s_cam_picker = p;
  nav_back_intercept = cam_picker_back_intercept;
}

/* ---- the individual settings ---------------------------------------------- */

/* Brightness / contrast / saturation / sharpness share the -8..+8 scale, so
 * they share one label set (17 rows — the picker scrolls, opened AT the
 * current value). Past +-3-ish is deliberately aggressive territory. */
static const char *const CAM_LEVEL_ITEMS[] = {
  "-8", "-7", "-6", "-5", "-4", "-3", "-2", "-1", "0",
  "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8"
};
#define CAM_LEVEL_N 17

/* Exposure compensation: the sensor's native AE-target range. */
static const char *const CAM_EV_ITEMS[] = {
  "-5", "-4", "-3", "-2", "-1", "0", "+1", "+2", "+3", "+4", "+5"
};
/* White-balance presets and special effects: the OV5640's built-in sets. */
static const char *const CAM_WB_ITEMS[] = {
  "Auto", "Sunny", "Cloudy", "Office", "Home"
};
static const char *const CAM_FX_ITEMS[] = {
  "None", "Negative", "B&W", "Reddish", "Greenish", "Bluish", "Sepia"
};
/* Denoise strength (0 = off). */
static const char *const CAM_DN_ITEMS[] = {
  "Off", "1", "2", "3", "4", "5", "6", "7", "8"
};
/* Max auto-gain — an ISO ceiling. Lower = darker dark shots, far less noise. */
static const char *const CAM_ISO_ITEMS[] = {
  "Auto", "2x", "4x", "8x", "16x", "32x", "64x", "128x"
};

static void cam_apply_bright(int i)   { cam_bright_set(i + CAM_LEVEL_MIN); }
static void cam_apply_contrast(int i) { cam_contrast_set(i + CAM_LEVEL_MIN); }
static void cam_apply_sat(int i)      { cam_sat_set(i + CAM_LEVEL_MIN); }
static void cam_apply_sharp(int i)    { cam_sharp_set(i + CAM_LEVEL_MIN); }
static void cam_apply_ev(int i)       { cam_ev_set(i - 5); }
static void cam_apply_wb(int i)       { cam_wb_set(i); }
static void cam_apply_fx(int i)       { cam_effect_set(i); }
static void cam_apply_dn(int i)       { cam_denoise_set(i); }
static void cam_apply_iso(int i)      { cam_gainceil_set(i - 1); }
/* Resolution affects only the STILL (the preview stays at its per-aspect size),
 * so there is nothing to restart here — the next capture picks it up. The
 * toolbar's top-right readout is refreshed by the picker-item path. */
static void cam_apply_res(int i) { cam_res_set(i); }

static void cam_open_res(lv_event_t *e) {
  (void)e;
  /* The sizes of the CURRENT (fixed 1:1) aspect: 320x320 and the instant-shutter
   * 240x240. Still a picker rather than a cycle-tap so the choice is visible. */
  const cam_aspect_t *a = cam_aspect();
  const char *items[CAM_PICK_MAX];
  int n = a->count; if (n > CAM_PICK_MAX) n = CAM_PICK_MAX;
  for (int i = 0; i < n; i++) items[i] = a->list[i].name;
  cam_picker_open("Resolution", items, n, s_cam_res_idx, cam_apply_res);
}

/* ---- video record toggle ---------------------------------------------------
 * The RED SHUTTER is the record toggle: a plain red disc (the universal
 * camcorder record mark) that gains a white STOP glyph while recording. Stop
 * only REQUESTS the end — the writer task drains its ring and finalises the
 * AVI in the background; the preview tick flips the toast to "Video saved"
 * when the file is safely closed. */
static void cam_rec_btn_refresh(void) {
  if (!s_cam_rec_btn) return;
  lv_obj_t *l = lv_obj_get_child(s_cam_rec_btn, 0);
  if (!l) return;
  bool rec = cam_rec_active();
  lv_label_set_text(l, rec ? LV_SYMBOL_STOP : "");
  lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
}

static void cam_rec_cb(lv_event_t *e) {
  (void)e;
  if (cam_rec_active()) {
    cam_rec_stop();
    cam_set_status("Saving video...", 0xFFFFFF);
  } else if (cam_rec_start()) {
    /* the tick's REC readout takes over the toast from here */
  } else {
    cam_set_status(store_available() ? "Can't record" : "No storage", 0xFF6060);
  }
  cam_rec_btn_refresh();
}

/* ---- the mode dial ---------------------------------------------------------
 * Both shutters are positioned from ONE angle: the photo disc rides the dial
 * at s_cam_mode_angle, the video disc at the diametrically opposite point.
 * Angle 0 puts a disc at the TOP of the circle (the shutter spot); 180 at the
 * bottom, which is s_cam_circ_r below the shutter spot's mirror — chosen so
 * it lies past the screen's bottom edge and the parent clips it away. */
static void cam_mode_place(int32_t ang) {
  const float rad = (float)ang * 3.14159265f / 180.0f;
  lv_obj_t *two[2] = { s_cam_shutter, s_cam_rec_btn };
  for (int i = 0; i < 2; i++) {
    if (!two[i]) continue;
    float a = rad + (i ? 3.14159265f : 0.0f);      // video rides 180° behind
    int x = s_cam_circ_cx + (int)(s_cam_circ_r * sinf(a));
    int y = s_cam_circ_cy - (int)(s_cam_circ_r * cosf(a));
    lv_obj_set_pos(two[i], x - CAM_SHUTTER_D / 2, y - CAM_SHUTTER_D / 2);
  }
}

static void cam_mode_anim_cb(void *var, int32_t v) {
  (void)var;
  s_cam_mode_angle = v;
  cam_mode_place(v);
}

/* Spin the dial to the other mode. `dirsign` (+1/-1) picks WHICH WAY the
 * circle turns so the discs travel in the direction of the swipe. The target
 * is the nearest angle in that direction that is ≡ the new mode's rest angle
 * (mod 360) — computed this way so a swipe that lands mid-animation still
 * settles exactly on a rest point rather than accumulating drift. */
static void cam_mode_set(bool video, int dirsign) {
  if (video == s_cam_mode_video) return;
  s_cam_mode_video = video;

  int32_t from   = s_cam_mode_angle;
  int32_t target = video ? 180 : 0;
  int32_t to     = target;
  if (dirsign >= 0) { while (to <= from) to += 360; }
  else              { while (to >= from) to -= 360; }

  lv_anim_del(&s_cam_mode_angle, nullptr);   // a re-swipe supersedes the old spin
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &s_cam_mode_angle);
  lv_anim_set_exec_cb(&a, cam_mode_anim_cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, 320);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

/* Horizontal swipe anywhere on the viewfinder — the preview itself, the top
 * bar, or straight across a shutter (LV_OBJ_FLAG_GESTURE_BUBBLE is on by
 * default, so a press that starts on a button still bubbles its gesture up to
 * app_scr, where this lives). lv_indev_wait_release makes the indev drop the
 * remainder of the press, so swiping OVER a shutter never also CLICKS it. */
static void cam_mode_gesture_cb(lv_event_t *e) {
  (void)e;
  lv_indev_t *ind = lv_indev_active();
  if (!ind) return;
  lv_dir_t dir = lv_indev_get_gesture_dir(ind);
  if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;   // vertical: not ours
  lv_indev_wait_release(ind);
  if (cam_rec_active()) {
    /* The red shutter is the only way to END a recording — spinning it away
     * mid-take would strand the session, so the dial locks while recording
     * (cam_rec_active stays true through the background AVI finalise too,
     * so "Saving video..." is covered by the same lock). */
    cam_set_status("Stop recording first", 0xFFD60A);
    return;
  }
  if (s_cam_busy) {
    /* A still is mid-capture/save — the dial stays put until it lands. */
    cam_set_status("Saving photo...", 0xFFD60A);
    return;
  }
  cam_mode_set(!s_cam_mode_video, (dir == LV_DIR_RIGHT) ? 1 : -1);
}

static void cam_open_bright(lv_event_t *e) {
  (void)e;
  cam_picker_open("Brightness", CAM_LEVEL_ITEMS, CAM_LEVEL_N,
                  cam_bright_get() - CAM_LEVEL_MIN, cam_apply_bright);
}
static void cam_open_contrast(lv_event_t *e) {
  (void)e;
  cam_picker_open("Contrast", CAM_LEVEL_ITEMS, CAM_LEVEL_N,
                  cam_contrast_get() - CAM_LEVEL_MIN, cam_apply_contrast);
}
static void cam_open_sat(lv_event_t *e) {
  (void)e;
  cam_picker_open("Saturation", CAM_LEVEL_ITEMS, CAM_LEVEL_N,
                  cam_sat_get() - CAM_LEVEL_MIN, cam_apply_sat);
}
static void cam_open_sharp(lv_event_t *e) {
  (void)e;
  cam_picker_open("Sharpness", CAM_LEVEL_ITEMS, CAM_LEVEL_N,
                  cam_sharp_get() - CAM_LEVEL_MIN, cam_apply_sharp);
}
static void cam_open_ev(lv_event_t *e) {
  (void)e;
  cam_picker_open("Exposure", CAM_EV_ITEMS, 11, cam_ev_get() + 5, cam_apply_ev);
}
static void cam_open_wb(lv_event_t *e) {
  (void)e;
  cam_picker_open("White balance", CAM_WB_ITEMS, 5, cam_wb_get(), cam_apply_wb);
}
static void cam_open_fx(lv_event_t *e) {
  (void)e;
  cam_picker_open("Effect", CAM_FX_ITEMS, 7, cam_effect_get(), cam_apply_fx);
}
static void cam_open_dn(lv_event_t *e) {
  (void)e;
  cam_picker_open("Denoise", CAM_DN_ITEMS, 9, cam_denoise_get(), cam_apply_dn);
}
static void cam_open_iso(lv_event_t *e) {
  (void)e;
  cam_picker_open("ISO limit", CAM_ISO_ITEMS, 8, cam_gainceil_get() + 1, cam_apply_iso);
}

static void app_open_camera(void) {
  /* Any previous build's preview timer points at widgets about to be destroyed —
   * and so would a mid-flight mode-dial spin, so kill that too and drop the
   * shutter pointers before the tree goes down. */
  cam_stop_preview();
  lv_anim_del(&s_cam_mode_angle, nullptr);
  s_cam_shutter = nullptr;
  s_cam_rec_btn = nullptr;

  /* ================= FULL-BLEED PREVIEW =================
   * This tab deliberately does NOT use app_screen_begin(): that shell paints a
   * title and reserves a header band, and the whole point here is that the sensor
   * feed owns the entire panel with every control floating ON TOP of it. So the
   * screen is built by hand and the pieces the shell would have provided (the
   * BOOT-back hint, the tab switch) are re-created as overlays.
   *
   * Z-ORDER: children added later draw on top, so the preview image goes down
   * first and every control after it. All overlay chrome sits on translucent
   * backgrounds so it stays readable over a bright scene. */
  display_bus_drain();                    // fence async flush before retearing the tree
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  if (menu_scr) lv_obj_add_flag(menu_scr, LV_OBJ_FLAG_HIDDEN);
  nav_back_intercept = nullptr;

  app_scr = lv_obj_create(lv_layer_top());
  lv_obj_set_size(app_scr, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(app_scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(app_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(app_scr, 0, 0);
  lv_obj_set_style_pad_all(app_scr, 0, 0);
  lv_obj_set_style_radius(app_scr, 0, 0);
  lv_obj_clear_flag(app_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *scr = app_scr;

  /* ---- the live preview ----
   * Blitted 1:1 and center-cropped to the panel — never scaled (see the zoom
   * logic in cam_preview_tick for why). The height letterboxes behind the
   * overlay bars; the screen behind is already black. */
  lv_obj_t *img = lv_img_create(scr);
  lv_obj_center(img);
  /* Belt and braces: with zoom pinned at 256 LVGL never transforms, but if a
   * future tweak reintroduces one, nearest-neighbour keeps it survivable —
   * bilinear over a PSRAM source is far too expensive for a live feed. */
  lv_image_set_antialias(img, false);
  s_cam_prev_img = img;

  /* ---- top overlay: "< BOOT" hint, the Gallery-app shortcut, and the
   * resolution readout in the RIGHT CORNER. SPACE_BETWEEN (not the bar's
   * default SPACE_EVENLY) pins the hint left and the readout hard right. ---- */
  lv_obj_t *topbar = cam_overlay_bar(scr, LV_ALIGN_TOP_MID, UI_PX(6));
  lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_t *hint = lv_label_create(topbar);
  lv_obj_set_style_text_font(hint, &MENU_HINT_FONT, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0xDDDDDD), 0);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " BOOT");
#if OWF_HAS_GALLERY
  /* Review what was just shot: releases the camera and opens the Gallery app
   * (its "Camera" section is this app's /DCIM output). Backing out returns. */
  cam_chip(topbar, LV_SYMBOL_IMAGE, 0xFFFFFF, cam_open_gallery_cb, nullptr);
#endif
  /* Active capture resolution, top-right. Informational only on this board —
   * the aspect/resolution pickers are gone (see the toolbar comment below). */
  s_cam_res_lbl = lv_label_create(topbar);
  lv_obj_set_style_text_font(s_cam_res_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(s_cam_res_lbl, lv_color_hex(0xBBBBBB), 0);

  /* ---- bottom overlay: a HORIZONTALLY SCROLLABLE settings toolbar ----
   * Each setting is its own icon button rather than one gear hiding a panel, so
   * they are all one tap away. The row SCROLLS horizontally, which is what lets
   * the set grow without redesigning the bar or shrinking the buttons: anything
   * past the right edge is reachable by swiping the strip.
   *
   * Order puts the zoom trio first (most-used, and it stays visible at rest),
   * then the image settings. */
  lv_obj_t *botbar = cam_overlay_bar(scr, LV_ALIGN_BOTTOM_MID, -UI_PX(6));
  lv_obj_add_flag(botbar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(botbar, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(botbar, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_snap_x(botbar, LV_SCROLL_SNAP_NONE);
  /* START, not SPACE_EVENLY: with a scrolling strip the children must pack from
   * the left and overflow off the right edge. SPACE_EVENLY would instead squeeze
   * everything into the visible width and there would be nothing to scroll to. */
  lv_obj_set_flex_align(botbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(botbar, 6, 0);

  /* zoom: [-] 1.0x [+] kept tight as one group */
  lv_obj_t *zgrp = cam_group(botbar, 4);
  cam_chip(zgrp, "-", 0xFFFFFF, cam_zoom_cb, (void *)(intptr_t)-1);
  s_cam_zoom_lbl = lv_label_create(zgrp);
  lv_obj_set_style_text_font(s_cam_zoom_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(s_cam_zoom_lbl, lv_color_hex(0xFFFFFF), 0);
  cam_update_zoom_label();
  cam_chip(zgrp, "+", 0xFFFFFF, cam_zoom_cb, (void *)(intptr_t)1);

  /* One button per setting, each opening its own picker list.
   * Icons are chosen from LVGL's built-in symbol font (no font regeneration):
   *   brightness -> EYE_OPEN   (light/visibility)
   *   contrast   -> CHARGE     (a filled bolt: the closest to a half-black/
   *                             half-white contrast mark in the built-in set)
   *
   *   resolution -> IMAGE      (picture size — the 1:1 sizes only)
   *
   * NO aspect picker ON THIS BOARD — a deliberate removal, not an omission.
   * The 1:1 preview is the only one in the sensor's fast clock bracket;
   * 4:3/16:9 run ~40% slower (see camera_dev.h's PLL bracket notes), and
   * square IS the watch-face shape, so offering slower shapes was a trap.
   * CAM_ASPECTS/cam_aspect_set stay in camera_dev.h pinned to 1:1; one
   * cam_chip line here brings the aspect picker back for a board where the
   * trade-off differs. */
  s_cam_bri_btn = cam_chip(botbar, LV_SYMBOL_EYE_OPEN, 0xFFFFFF, cam_open_bright, nullptr);
  s_cam_con_btn = cam_chip(botbar, LV_SYMBOL_CHARGE,   0xFFFFFF, cam_open_contrast, nullptr);
  /* saturation -> TINT (the paint-drop: colour intensity). With this chip the
   * strip overflows the 240 px bar again — that is fine BY DESIGN: the bar
   * scrolls horizontally (flags set at its creation above), so growth costs a
   * swipe, not button size. */
  s_cam_sat_btn = cam_chip(botbar, LV_SYMBOL_TINT,     0xFFFFFF, cam_open_sat, nullptr);
  /* The full manual-controls strip. Placeholder glyphs from LVGL's built-in
   * symbol font until a custom MDI icon font lands (see the icon wishlist in
   * the settings notes): exposure comp, white balance, sharpness, denoise,
   * ISO limit, colour effect. The bar scrolls; growth costs a swipe. */
  cam_chip(botbar, LV_SYMBOL_PLUS,    0xFFFFFF, cam_open_ev, nullptr);
  cam_chip(botbar, LV_SYMBOL_REFRESH, 0xFFFFFF, cam_open_wb, nullptr);
  cam_chip(botbar, LV_SYMBOL_UP,      0xFFFFFF, cam_open_sharp, nullptr);
  cam_chip(botbar, LV_SYMBOL_MUTE,    0xFFFFFF, cam_open_dn, nullptr);
  cam_chip(botbar, LV_SYMBOL_BARS,    0xFFFFFF, cam_open_iso, nullptr);
  cam_chip(botbar, LV_SYMBOL_SHUFFLE, 0xFFFFFF, cam_open_fx, nullptr);
  s_cam_res_btn = cam_chip(botbar, LV_SYMBOL_IMAGE,    0xFFFFFF, cam_open_res, nullptr);
  cam_toolbar_refresh();

  /* ---- THE SHUTTERS: two discs on the mode dial, floating ABOVE the bar ----
   * Deliberately NOT children of the overlay bar. Inside it, a shutter was one
   * chip among five and read as just another setting. A camera's shutter has to
   * be the single obvious control, so it is: bigger (56 px vs 34), perfectly
   * centred, sitting clear of the dark bar with no panel behind it. There are
   * TWO — photo (white) and video (red) — riding opposite ends of the invisible
   * mode-dial circle (see cam_mode_place); only the active one is on-screen at
   * rest, the other waits below the bottom edge. They are moved BENEATH the
   * bars in z-order (see the move_to_index calls below): a disc arcing past
   * the toolbar during a spin slides BEHIND it, so the chrome always wins. */
  lv_obj_t *shutter = lv_btn_create(scr);          // PHOTO — white disc
  lv_obj_set_size(shutter, CAM_SHUTTER_D, CAM_SHUTTER_D);
  lv_obj_set_style_radius(shutter, CAM_SHUTTER_D / 2, 0);
  lv_obj_set_style_bg_color(shutter, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(shutter, LV_OPA_COVER, 0);
  /* A dark ring so the white disc still reads against a bright/blown-out scene. */
  lv_obj_set_style_border_color(shutter, lv_color_hex(0x101010), 0);
  lv_obj_set_style_border_width(shutter, 3, 0);
  lv_obj_set_style_shadow_width(shutter, 0, 0);
  lv_obj_set_style_pad_all(shutter, 0, 0);
  /* Pressed state: dim the disc so a tap is visibly acknowledged even before the
   * capture message appears (the capture itself blocks for a moment). */
  lv_obj_set_style_bg_color(shutter, lv_color_hex(0xBBBBBB), LV_STATE_PRESSED);
  lv_obj_add_flag(shutter, HAPTICS_NO_BUZZ_FLAG);
  lv_obj_add_event_cb(shutter, cam_shutter_cb, LV_EVENT_CLICKED, nullptr);
  s_cam_shutter = shutter;

  lv_obj_t *vshutter = lv_btn_create(scr);         // VIDEO — red disc
  lv_obj_set_size(vshutter, CAM_SHUTTER_D, CAM_SHUTTER_D);
  lv_obj_set_style_radius(vshutter, CAM_SHUTTER_D / 2, 0);
  lv_obj_set_style_bg_color(vshutter, lv_color_hex(0xE53935), 0);
  lv_obj_set_style_bg_opa(vshutter, LV_OPA_COVER, 0);
  /* Same dark ring, plus a WHITE inner ring the white shutter doesn't need:
   * red on a dark scene is much lower-contrast than white, and the extra ring
   * is also what makes the disc read as "record", not "error". */
  lv_obj_set_style_border_color(vshutter, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(vshutter, 3, 0);
  lv_obj_set_style_outline_color(vshutter, lv_color_hex(0x101010), 0);
  lv_obj_set_style_outline_width(vshutter, 2, 0);
  lv_obj_set_style_shadow_width(vshutter, 0, 0);
  lv_obj_set_style_pad_all(vshutter, 0, 0);
  lv_obj_set_style_bg_color(vshutter, lv_color_hex(0xB71C1C), LV_STATE_PRESSED);
  lv_obj_add_flag(vshutter, HAPTICS_NO_BUZZ_FLAG);
  lv_obj_add_event_cb(vshutter, cam_rec_cb, LV_EVENT_CLICKED, nullptr);
  /* The STOP glyph while recording lives in this label (empty when idle). */
  lv_obj_t *vlbl = lv_label_create(vshutter);
  lv_obj_set_style_text_font(vlbl, &FONT_SMALL, 0);
  lv_obj_center(vlbl);
  s_cam_rec_btn = vshutter;
  cam_rec_btn_refresh();

  /* Z-ORDER: slide both discs down to just above the preview image (index 0),
   * BELOW the bars. The bars are translucent, so this alone is presentation —
   * the parked disc is kept genuinely invisible by the radius math below —
   * but it is what makes a disc pass BEHIND the toolbar mid-spin. */
  lv_obj_move_to_index(shutter, 1);
  lv_obj_move_to_index(vshutter, 2);

  /* Dial geometry, from where the bar actually landed (needs a layout pass).
   * The circle's TOP is the classic shutter spot just above the bar. The
   * radius is computed, not fixed: the parked disc sits 2R below that spot,
   * and R is chosen so its top edge lands past LCD_HEIGHT — fully off-screen
   * (the bars are translucent, so merely "behind the bar" would still glow
   * through). UI_PX(64) is the floor so the travel arc stays generous. */
  lv_obj_update_layout(scr);
  const int rest_cy = lv_obj_get_y(botbar) - UI_PX(8) - CAM_SHUTTER_D / 2;
  int r_min = (LCD_HEIGHT + CAM_SHUTTER_D / 2 + UI_PX(6) - rest_cy + 1) / 2;
  s_cam_circ_r  = (r_min > UI_PX(64)) ? r_min : UI_PX(64);
  s_cam_circ_cx = LCD_WIDTH / 2;
  s_cam_circ_cy = rest_cy + s_cam_circ_r;
  s_cam_mode_angle = s_cam_mode_video ? 180 : 0;   // land in the remembered mode
  cam_mode_place(s_cam_mode_angle);

  /* The mode swipe listens on the whole screen: presses on the (non-clickable)
   * preview land on app_scr directly, and presses on the bars/shutters walk
   * their gesture up to it. CRITICAL: LVGL delivers a gesture to the first
   * ancestor WITHOUT the GESTURE_BUBBLE flag — with the flag left on (its
   * default) the walk runs past app_scr and lv_layer_top() off the top of the
   * tree and the event is delivered to NOTHING. Clearing it here makes
   * app_scr the walk's terminus, i.e. the one place gestures actually land. */
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(scr, cam_mode_gesture_cb, LV_EVENT_GESTURE, nullptr);

  /* ---- status/toast ----
   * Lives just BELOW THE TOP BAR, not above the bottom one. It used to sit in the
   * band directly over the bottom bar — which is exactly where the shutter now
   * floats, so its full-width translucent panel drew a second dark slab across the
   * shutter. Moving it to the top clears that band entirely.
   *
   * It is also HIDDEN when it has nothing to say (see cam_set_status): a permanently
   * present empty panel is chrome for no reason on a viewfinder. */
  s_cam_status = lv_label_create(scr);
  lv_obj_set_style_text_font(s_cam_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(s_cam_status, lv_color_hex(0xEEEEEE), 0);
  lv_obj_set_style_bg_color(s_cam_status, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_cam_status, LV_OPA_60, 0);
  lv_obj_set_style_pad_all(s_cam_status, UI_PX(4), 0);
  lv_obj_set_style_radius(s_cam_status, UI_PX(6), 0);
  lv_obj_set_width(s_cam_status, LV_PCT(86));
  lv_obj_set_style_text_align(s_cam_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_cam_status, LV_LABEL_LONG_WRAP);
  lv_label_set_text(s_cam_status, "");
  lv_obj_add_flag(s_cam_status, LV_OBJ_FLAG_HIDDEN);   // nothing to say yet
  lv_obj_align_to(s_cam_status, topbar, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(6));

  if (!cam_begin()) {
    s_cam_prev_img = nullptr;
    cam_set_status("Camera not detected", 0xFF6060);
    return;
  }

  /* Pinch-to-zoom is sampled from the same timer as the preview. The CST816D is
   * documented as a SINGLE-touch part, so this is wired defensively: the sampler
   * only acts when the controller genuinely reports two distinct points, and the
   * +/- chips above are always present as the guaranteed path. */
  s_cam_pinch_ref = 0;

  /* 33 ms -> up to ~30 fps. The timer is only a CEILING: a slow frame just delays
   * the next tick rather than backing up work, so fast sensor modes (1:1 measured
   * 19 fps pinned by the previous 50 ms period, fb_get near zero) get room to run
   * while slow modes degrade gracefully. The floor is the sensor's own frame rate
   * per mode (see cam_boost_preview_clock in camera_dev.h); the probe above
   * prints where it lands. The same timer services the pinch sampler. */
  s_cam_prev_timer = lv_timer_create(cam_preview_tick, 33, nullptr);
  cam_set_status("", 0x999999);
}


/* Leaving the app: finalise any recording, stop the preview and release the
 * camera. Called from the menu's back path (see app_menu.h) and from the
 * Gallery-app jump, so an idle watch keeps neither the sensor powered nor the
 * preview's PSRAM pinned. */
static void app_camera_on_close(void) {
  cam_rec_stop_sync();      // finalise an in-flight recording BEFORE the teardown
  cam_stop_preview();
  cam_end();
  lv_anim_del(&s_cam_mode_angle, nullptr);   // a spin must not outlive its discs
  s_cam_shutter      = nullptr;
  s_cam_status       = nullptr;
  s_cam_zoom_lbl  = nullptr;
  s_cam_res_lbl   = nullptr;
  s_cam_bri_btn   = nullptr;
  s_cam_con_btn   = nullptr;
  s_cam_sat_btn   = nullptr;
  s_cam_res_btn   = nullptr;
  s_cam_rec_btn   = nullptr;
  s_cam_picker    = nullptr;   // destroyed with app_scr
  s_cam_pick_fn   = nullptr;
  s_cam_pinch_ref = 0;
}

#endif  /* BOARD_HAS_CAMERA */
