/* ============================================================================
 *  camera_rec.h — MJPEG-AVI video recording for the Camera app.
 *
 *  Present only on BOARD_HAS_CAMERA boards (compiles to nothing elsewhere).
 *  INCLUDE AFTER camera_dev.h and storage_fs.h; app_camera.h provides the UI.
 *
 *  ARCHITECTURE — two cores, one ring, one file:
 *    - The PREVIEW PIPELINE IS UNTOUCHED. The sensor stays in RGB565 preview
 *      mode, so the viewfinder keeps its full frame rate while recording. A
 *      JPEG-mode recorder was rejected: the sensor outputs ONE format at a
 *      time, and JPEG frames would cost a ~20 ms software decode per DISPLAYED
 *      frame — gutting the live view to single digits.
 *    - cam_preview_tick (LVGL thread, core 1) additionally copies each raw
 *      big-endian frame into a PSRAM RING BUFFER (cam_rec_feed) — ~5 ms per
 *      frame, the only cost the viewfinder pays.
 *    - A WRITER TASK PINNED TO CORE 0 drains the ring: software-encodes each
 *      frame to JPEG (fmt2jpg — the same encoder as the instant shutter) and
 *      appends it to an AVI on the store. Encoding is the throughput limit
 *      (~15-20 fps at 240x240); when the ring is full the producer simply
 *      skips that frame, so a slow card or a hard scene degrades RECORDED fps,
 *      never the live view and never correctness.
 *
 *  WHY THERE IS NO "PARALLEL FLUSH": one SPI bus, one card, one file — a
 *  second writer would just contend on the same wire (which the display also
 *  shares, see sd_bus_lock in the .ino). The two-core split puts the genuinely
 *  parallelisable work (JPEG encoding) on the otherwise-idle core instead.
 *
 *  CONTAINER: MJPEG in AVI — every frame an independent JPEG, the container
 *  every player understands (VLC, phones, browsers). No audio: no mic on this
 *  board. The header is written with placeholder timing at start and PATCHED
 *  at stop with the MEASURED average frame rate, so a session that recorded at
 *  13.7 fps plays back at 13.7 fps — variable encode speed never skews time.
 * ========================================================================== */
#pragma once
#if BOARD_HAS_CAMERA

#include <FS.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"           // esp_timer_get_time — session timing
#include "freertos/FreeRTOS.h"   // writer task + delays
#include "freertos/task.h"
#if __has_include("freertos/idf_additions.h")
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCore (IDF 5.x home)
#endif

/* ---- tuning ---- */
#define REC_RING_SLOTS   6        /* raw frames buffered: 6 x 115 KB = 690 KB PSRAM */
#define REC_JPEG_QUALITY 80       /* fmt2jpg 0..100; 80 ~= 6-8 KB/frame at 240x240 */
#define REC_MAX_FRAMES   27000    /* index cap (16 B/frame): ~25 min at 18 fps */

/* ---- session state ----
 * SPSC ring: the LVGL thread produces (head), the writer task consumes (tail).
 * Slot dims are FIXED per session (taken from the preview at start); a frame
 * of any other size (aspect changed mid-recording) is simply not enqueued. */
static uint8_t *s_rec_ring[REC_RING_SLOTS];
static volatile int s_rec_head = 0, s_rec_tail = 0;
static int s_rec_w = 0, s_rec_h = 0;
static volatile bool s_rec_active = false;    /* session open (UI state) */
static volatile bool s_rec_stop_req = false;  /* producer stopped; task drains + finalises */
static TaskHandle_t s_rec_task_h = nullptr;

static File      s_rec_file;                 /* written ONLY by the writer task */
static uint8_t  *s_rec_index = nullptr;      /* idx1 entries, built as we go     */
static uint32_t  s_rec_frames = 0;
static uint32_t  s_rec_movi_bytes = 0;       /* bytes inside the movi LIST after its 4cc */
static int64_t   s_rec_t0_us = 0, s_rec_t1_us = 0;
static char      s_rec_path[128];

/* ---- little-endian emit helpers (AVI is a little-endian RIFF format) ---- */
static size_t avi_p32(uint8_t *b, size_t o, uint32_t v) {
  b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8);
  b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24);
  return o + 4;
}
static size_t avi_p16(uint8_t *b, size_t o, uint16_t v) {
  b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8);
  return o + 2;
}
static size_t avi_p4cc(uint8_t *b, size_t o, const char *s) {
  memcpy(b + o, s, 4);
  return o + 4;
}

/* ---- the 224-byte AVI header ------------------------------------------------
 * Fixed layout, so the finalise step can patch by ABSOLUTE OFFSET:
 *   0   RIFF <riff_size> AVI                       riff_size      @ 4
 *   12  LIST 192 hdrl
 *   24    avih 56                                  usPerFrame     @ 32
 *                                                  totalFrames    @ 48
 *   88    LIST 116 strl
 *   100     strh 56 ('vids'/'MJPG')                dwScale        @ 128
 *                                                  dwRate         @ 132
 *                                                  dwLength       @ 140
 *   164     strf 40 (BITMAPINFOHEADER, 'MJPG')
 *   212 LIST <movi_size> movi                      movi_size      @ 216
 *   224 first '00dc' frame chunk
 * After the last frame: idx1 (entry offsets relative to the 'movi' 4cc, first
 * chunk = 4 — the convention every mainstream player accepts). */
#define REC_HDR_BYTES 224
static void cam_rec_write_header(uint32_t us_per_frame, uint32_t frames,
                                 uint32_t movi_bytes) {
  uint8_t h[REC_HDR_BYTES];
  memset(h, 0, sizeof(h));
  size_t o = 0;
  o = avi_p4cc(h, o, "RIFF");
  o = avi_p32(h, o, 4 + (8 + 4 + movi_bytes) + 200 /* hdrl block */ +
                    (s_rec_frames ? 8 + 16 * frames : 0));  /* file - 8 (patched) */
  o = avi_p4cc(h, o, "AVI ");
  o = avi_p4cc(h, o, "LIST"); o = avi_p32(h, o, 192); o = avi_p4cc(h, o, "hdrl");
  o = avi_p4cc(h, o, "avih"); o = avi_p32(h, o, 56);
  o = avi_p32(h, o, us_per_frame);            /* dwMicroSecPerFrame  @32 */
  o = avi_p32(h, o, 500000);                  /* dwMaxBytesPerSec (loose) */
  o = avi_p32(h, o, 0);                       /* padding */
  o = avi_p32(h, o, 0x10);                    /* AVIF_HASINDEX */
  o = avi_p32(h, o, frames);                  /* dwTotalFrames       @48 */
  o = avi_p32(h, o, 0);                       /* initial frames */
  o = avi_p32(h, o, 1);                       /* one stream */
  o = avi_p32(h, o, 128 * 1024);              /* suggested buffer */
  o = avi_p32(h, o, (uint32_t)s_rec_w);
  o = avi_p32(h, o, (uint32_t)s_rec_h);
  o += 16;                                    /* reserved[4] */
  o = avi_p4cc(h, o, "LIST"); o = avi_p32(h, o, 116); o = avi_p4cc(h, o, "strl");
  o = avi_p4cc(h, o, "strh"); o = avi_p32(h, o, 56);
  o = avi_p4cc(h, o, "vids");
  o = avi_p4cc(h, o, "MJPG");
  o = avi_p32(h, o, 0);                       /* flags */
  o = avi_p16(h, o, 0); o = avi_p16(h, o, 0); /* priority, language */
  o = avi_p32(h, o, 0);                       /* initial frames */
  o = avi_p32(h, o, us_per_frame);            /* dwScale             @128 */
  o = avi_p32(h, o, 1000000);                 /* dwRate              @132 */
  o = avi_p32(h, o, 0);                       /* start */
  o = avi_p32(h, o, frames);                  /* dwLength            @140 */
  o = avi_p32(h, o, 128 * 1024);              /* suggested buffer */
  o = avi_p32(h, o, 0xFFFFFFFF);              /* quality: default */
  o = avi_p32(h, o, 0);                       /* sample size */
  o = avi_p16(h, o, 0); o = avi_p16(h, o, 0); /* rcFrame l,t */
  o = avi_p16(h, o, (uint16_t)s_rec_w);
  o = avi_p16(h, o, (uint16_t)s_rec_h);
  o = avi_p4cc(h, o, "strf"); o = avi_p32(h, o, 40);
  o = avi_p32(h, o, 40);                      /* biSize */
  o = avi_p32(h, o, (uint32_t)s_rec_w);
  o = avi_p32(h, o, (uint32_t)s_rec_h);
  o = avi_p16(h, o, 1);                       /* planes */
  o = avi_p16(h, o, 24);                      /* bit count */
  o = avi_p4cc(h, o, "MJPG");                 /* biCompression */
  o = avi_p32(h, o, (uint32_t)(s_rec_w * s_rec_h * 3));
  o = avi_p32(h, o, 0); o = avi_p32(h, o, 0); /* pels/meter */
  o = avi_p32(h, o, 0); o = avi_p32(h, o, 0); /* clr used/important */
  o = avi_p4cc(h, o, "LIST");
  o = avi_p32(h, o, 4 + movi_bytes);          /* movi_size           @216 */
  o = avi_p4cc(h, o, "movi");
  s_rec_file.write(h, sizeof(h));
}

/* ---- writer task (core 0) -------------------------------------------------
 * Drains the ring: encode -> append '00dc' chunk -> index entry. On stop it
 * finishes whatever is still buffered, writes idx1, patches the header with
 * the measured timing, closes the file and frees everything. Every SD write
 * goes through the store's own bus arbitration (the display shares the SPI
 * host), so cross-core safety comes from the existing sd_bus_lock machinery. */
static void cam_rec_task(void *arg) {
  (void)arg;
  const size_t frame_bytes = (size_t)s_rec_w * s_rec_h * 2;

  while (true) {
    if (s_rec_tail != s_rec_head) {
      uint8_t *jpg = nullptr;
      size_t   jl  = 0;
      /* Raw big-endian sensor RGB565 — exactly fmt2jpg's expectation (same as
       * the instant shutter). */
      bool ok = fmt2jpg(s_rec_ring[s_rec_tail], frame_bytes,
                        (uint16_t)s_rec_w, (uint16_t)s_rec_h,
                        PIXFORMAT_RGB565, REC_JPEG_QUALITY, &jpg, &jl);
      s_rec_tail = (s_rec_tail + 1) % REC_RING_SLOTS;   /* slot free for producer */

      if (ok && jpg && s_rec_frames < REC_MAX_FRAMES) {
        uint8_t ch[8];
        size_t o = 0;
        o = avi_p4cc(ch, o, "00dc");
        o = avi_p32(ch, o, (uint32_t)jl);
        s_rec_file.write(ch, 8);
        s_rec_file.write(jpg, jl);
        uint32_t padded = (uint32_t)jl;
        if (jl & 1) { uint8_t z = 0; s_rec_file.write(&z, 1); padded++; }

        /* idx1 entry: chunk offset relative to the 'movi' 4cc (first = 4). */
        uint8_t *ix = s_rec_index + (size_t)s_rec_frames * 16;
        size_t io = 0;
        io = avi_p4cc(ix, io, "00dc");
        io = avi_p32(ix, io, 0x10);                       /* keyframe */
        io = avi_p32(ix, io, 4 + s_rec_movi_bytes);
        io = avi_p32(ix, io, (uint32_t)jl);

        s_rec_movi_bytes += 8 + padded;
        s_rec_frames++;
        s_rec_t1_us = esp_timer_get_time();
      }
      if (jpg) free(jpg);                    /* fmt2jpg allocates via malloc */
      if (s_rec_frames >= REC_MAX_FRAMES) s_rec_stop_req = true;
    } else if (s_rec_stop_req) {
      break;                                 /* producer stopped and ring drained */
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));          /* ring empty: wait for frames */
    }
  }

  /* ---- finalise ---- */
  if (s_rec_frames > 0) {
    uint8_t ih[8];
    size_t o = 0;
    o = avi_p4cc(ih, o, "idx1");
    o = avi_p32(ih, o, s_rec_frames * 16);
    s_rec_file.write(ih, 8);
    s_rec_file.write(s_rec_index, (size_t)s_rec_frames * 16);

    /* Patch the header with MEASURED timing: average us/frame across the whole
     * session, so drops and encode jitter never skew playback speed. */
    int64_t dur = s_rec_t1_us - s_rec_t0_us;
    uint32_t uspf = (s_rec_frames > 1 && dur > 0)
                      ? (uint32_t)(dur / (int64_t)s_rec_frames) : 66667;
    uint32_t fsz = REC_HDR_BYTES + s_rec_movi_bytes - 4 + 8 + s_rec_frames * 16;
    uint8_t p[4];
    s_rec_file.seek(4);   avi_p32(p, 0, fsz);                 s_rec_file.write(p, 4);
    s_rec_file.seek(32);  avi_p32(p, 0, uspf);                s_rec_file.write(p, 4);
    s_rec_file.seek(48);  avi_p32(p, 0, s_rec_frames);        s_rec_file.write(p, 4);
    s_rec_file.seek(128); avi_p32(p, 0, uspf);                s_rec_file.write(p, 4);
    s_rec_file.seek(140); avi_p32(p, 0, s_rec_frames);        s_rec_file.write(p, 4);
    s_rec_file.seek(216); avi_p32(p, 0, 4 + s_rec_movi_bytes); s_rec_file.write(p, 4);
    s_rec_file.close();
    USBSerial.printf("[rec] saved %s: %lu frames, %lu.%lu fps avg\n", s_rec_path,
                     (unsigned long)s_rec_frames,
                     (unsigned long)(uspf ? 10000000 / uspf / 10 : 0),
                     (unsigned long)(uspf ? (10000000 / uspf) % 10 : 0));
  } else {
    s_rec_file.close();
    store_fs().remove(s_rec_path);           /* zero frames: no point keeping it */
  }

  for (int i = 0; i < REC_RING_SLOTS; i++) {
    if (s_rec_ring[i]) { heap_caps_free(s_rec_ring[i]); s_rec_ring[i] = nullptr; }
  }
  if (s_rec_index) { heap_caps_free(s_rec_index); s_rec_index = nullptr; }

  s_rec_active = false;                      /* LAST: the UI polls this */
  s_rec_task_h = nullptr;
  vTaskDelete(nullptr);
}

/* ---- producer hook: called from cam_preview_tick with the RAW frame -------
 * Copies the frame into the ring if recording and a slot is free; a full ring
 * (encoder busy) just skips — the viewfinder never waits on the recorder. */
static void cam_rec_feed(const uint8_t *buf, int w, int h) {
  if (!s_rec_active || s_rec_stop_req) return;
  if (w != s_rec_w || h != s_rec_h) return;        /* session dims only */
  int next = (s_rec_head + 1) % REC_RING_SLOTS;
  if (next == s_rec_tail) return;                  /* ring full: drop this frame */
  memcpy(s_rec_ring[s_rec_head], buf, (size_t)w * h * 2);
  s_rec_head = next;
}

static bool cam_rec_active(void) { return s_rec_active; }
static uint32_t cam_rec_seconds(void) {
  return s_rec_active ? (uint32_t)((esp_timer_get_time() - s_rec_t0_us) / 1000000)
                      : 0;
}

/* Pick the next free VID_<n>.AVI (same scheme as the photos' IMG counter). */
static bool cam_rec_next_path(char *out, size_t out_sz) {
  if (!store_available()) return false;
  if (!store_fs().exists(CAM_DIR)) store_fs().mkdir(CAM_DIR);
  int highest = -1;
  File dir = store_fs().open(CAM_DIR);
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      int v;
      const char *nm = strrchr(f.name(), '/');
      nm = nm ? nm + 1 : f.name();
      if (!f.isDirectory() && sscanf(nm, "VID_%d.AVI", &v) == 1 && v > highest)
        highest = v;
      f.close();
    }
    dir.close();
  }
  if (highest >= 999) return false;
  snprintf(out, out_sz, CAM_DIR "/VID_%03d.AVI", highest + 1);
  return true;
}

/* Start a recording session on the CURRENT preview. False if anything is
 * missing (no store, preview not running, PSRAM, file). */
static bool cam_rec_start(void) {
  if (s_rec_active) return true;
  if (!store_available() || !s_cam_ready || s_cam_fmt != PIXFORMAT_RGB565)
    return false;

  s_rec_w = cam_preview_w();
  s_rec_h = cam_preview_h();
  const size_t frame_bytes = (size_t)s_rec_w * s_rec_h * 2;
  for (int i = 0; i < REC_RING_SLOTS; i++) {
    s_rec_ring[i] = (uint8_t *)heap_caps_malloc(frame_bytes,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rec_ring[i]) goto fail;
  }
  s_rec_index = (uint8_t *)heap_caps_malloc((size_t)REC_MAX_FRAMES * 16,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_rec_index) goto fail;

  if (!cam_rec_next_path(s_rec_path, sizeof(s_rec_path))) goto fail;
  s_rec_file = store_fs().open(s_rec_path, FILE_WRITE);
  if (!s_rec_file) goto fail;

  s_rec_head = s_rec_tail = 0;
  s_rec_frames = 0;
  s_rec_movi_bytes = 0;
  s_rec_stop_req = false;
  s_rec_t0_us = s_rec_t1_us = esp_timer_get_time();
  cam_rec_write_header(66667 /* placeholder ~15 fps; patched at stop */, 0, 0);

  /* Writer on CORE 0 — the LVGL/preview world lives on core 1, so the encode
   * genuinely runs in parallel. Internal-RAM stack (tasks can't stack in
   * PSRAM); 8 KB is comfortable for fmt2jpg + FS. */
  s_rec_active = true;
  if (xTaskCreatePinnedToCore(cam_rec_task, "cam_rec", 8192, nullptr,
                              1 /* low prio: encode is bulk work */, &s_rec_task_h,
                              0) != pdPASS) {
    s_rec_active = false;
    s_rec_file.close();
    store_fs().remove(s_rec_path);
    goto fail;
  }
  USBSerial.printf("[rec] recording %s (%dx%d)\n", s_rec_path, s_rec_w, s_rec_h);
  return true;

fail:
  for (int i = 0; i < REC_RING_SLOTS; i++) {
    if (s_rec_ring[i]) { heap_caps_free(s_rec_ring[i]); s_rec_ring[i] = nullptr; }
  }
  if (s_rec_index) { heap_caps_free(s_rec_index); s_rec_index = nullptr; }
  return false;
}

/* Request stop. The task drains the ring, finalises and frees asynchronously;
 * s_rec_active flips false when the file is safely on the card. */
static void cam_rec_stop(void) {
  if (s_rec_active) s_rec_stop_req = true;
}

/* Synchronous stop for app teardown: the file must be finalised before the
 * app (or deep sleep) pulls the rug. Bounded wait — the drain of a full ring
 * is ~6 encodes, well under a second. */
static void cam_rec_stop_sync(void) {
  cam_rec_stop();
  for (int i = 0; i < 300 && s_rec_active; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

#endif  /* BOARD_HAS_CAMERA */
