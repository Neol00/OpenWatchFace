/* ============================================================================
 *  media_video.h — the Gallery's video reader: one handle, three containers.
 *
 *  WHAT CHANGED AND WHY. The player used to understand exactly one file: the
 *  AVI camera_rec.h writes, recognised by its fixed 224-byte header and read
 *  by walking '00dc' chunks from a hard-coded offset. Anything else — an AVI
 *  from a phone, a dashcam, ffmpeg, or one of our own files that grew an extra
 *  header chunk — was rejected at open. This file replaces that with a real
 *  (if small) demuxer, so "video" means what a user expects it to mean.
 *
 *  CONTAINERS
 *    AVI    Properly parsed: RIFF walk, hdrl/avih for timing, per-stream strh
 *           /strf to find the VIDEO track, then movi walked chunk by chunk
 *           with audio, JUNK, and 'rec ' groupings skipped. Frame rate comes
 *           from the file (avih, or strh's rate/scale). Our own recordings are
 *           just one well-behaved case of this.
 *    MJPEG  A bare stream of JPEGs (.mjpg/.mjpeg, what a lot of IP cameras and
 *           ffmpeg's -c:v mjpeg -f mjpeg produce): frames are found by their
 *           SOI/EOI markers. No timing in the format, so 10 fps is assumed.
 *    GIF    Animated GIFs are played like video — same transport, same play/
 *           pause, per-frame delays honoured (media_codecs.h does the work).
 *
 *  CODEC SCOPE. Every frame must be a JPEG (or a GIF frame). An ESP32-S3 has
 *  no hardware video decoder and no headroom for a software H.264/HEVC/VP9
 *  one, so those files are refused BY NAME ("H.264 not supported") instead of
 *  failing mysteriously — the point is that the user learns what to convert.
 *
 *  MEMORY. One grow-only compressed-frame buffer and one RGB565 frame buffer,
 *  both PSRAM, reused for the whole session. Decoding is scaled down during
 *  the JPEG decode, so a 1080p MJPEG plays on a 240-pixel screen without ever
 *  materialising a 1080p frame.
 *
 *  Header-only. INCLUDE AFTER media_codecs.h, BEFORE app_gallery.h.
 * ========================================================================== */
#pragma once

#include "media_codecs.h"
#include <FS.h>

enum { MV_NONE = 0, MV_AVI, MV_MJPEG, MV_GIF };

#define MV_MAX_FRAME (2u * 1024u * 1024u)   /* sanity cap on one coded frame */

typedef struct {
  int   kind;
  bool  open;
  mc_file_src_t src;

  int      w, h;            /* source frame size, if the container states it */
  uint32_t frames;          /* total frames, 0 = unknown                     */
  uint32_t cur;             /* frames delivered so far                       */
  uint32_t uspf;            /* nominal frame period                          */
  uint32_t delay_ms;        /* period of the frame just delivered            */

  /* AVI */
  uint32_t movi_pos, movi_end;
  int      vstream;

  /* GIF */
  mc_gif_t gif;
  bool     gif_open;
  int      gif_step;

  /* frame buffers (grow-only, reused every frame) */
  uint8_t  *jpg;  size_t jpg_cap;
  uint16_t *rgb;  size_t rgb_cap;
  int       fw, fh;         /* dimensions of the frame in `rgb`              */

  int tw, th;               /* the view we are decoding for                  */
} mv_t;

/* ---- helpers -------------------------------------------------------------- */

static uint32_t mv_r32(const uint8_t *b) { return mc_le32(b); }

/* Is this four-cc a video codec we can actually play? Anything JPEG-shaped is
 * fine; the rest we name so the message is useful. */
static bool mv_codec_is_mjpeg(const uint8_t *cc) {
  /* All JPEG-in-a-container spellings in the wild. Note "MPEG" is NOT here —
   * that is a different codec entirely. */
  static const char *ok[] = { "MJPG", "mjpg", "MJPEG", "JPEG", "jpeg", "MJPA",
                              "AVRn", "dmb1", "jpgl", "JPGL", "MJLS" };
  for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
    if (!strncmp((const char *)cc, ok[i], 4)) return true;
  return false;
}
static const char *mv_codec_name(const uint8_t *cc) {
  if (!strncmp((const char *)cc, "H264", 4) || !strncmp((const char *)cc, "h264", 4) ||
      !strncmp((const char *)cc, "avc1", 4) || !strncmp((const char *)cc, "X264", 4))
    return "H.264";
  if (!strncmp((const char *)cc, "HEVC", 4) || !strncmp((const char *)cc, "hvc1", 4))
    return "H.265";
  if (!strncmp((const char *)cc, "XVID", 4) || !strncmp((const char *)cc, "xvid", 4) ||
      !strncmp((const char *)cc, "DIVX", 4) || !strncmp((const char *)cc, "FMP4", 4))
    return "MPEG-4";
  if (!strncmp((const char *)cc, "VP80", 4) || !strncmp((const char *)cc, "VP90", 4))
    return "VP8/9";
  if (!strncmp((const char *)cc, "mpg1", 4) || !strncmp((const char *)cc, "MPG1", 4) ||
      !strncmp((const char *)cc, "mpg2", 4) || !strncmp((const char *)cc, "MPG2", 4))
    return "MPEG-1/2";
  if (!strncmp((const char *)cc, "cvid", 4) || !strncmp((const char *)cc, "CVID", 4))
    return "Cinepak";
  return NULL;
}

/* Clear every field EXCEPT the embedded File. (Arduino's File owns a
 * shared_ptr; memset-ing over one silently corrupts its refcount, so this
 * struct is never bulk-cleared.) */
static void mv_reset(mv_t *v) {
  v->kind = MV_NONE;
  v->open = false;
  v->src.base.read = nullptr;
  v->src.base.seek = nullptr;
  v->src.base.size = 0;
  v->src.base.ctx  = nullptr;
  v->w = v->h = 0;
  v->frames = v->cur = 0;
  v->uspf = 66667;
  v->delay_ms = 0;
  v->movi_pos = v->movi_end = 0;
  v->vstream = -1;
  v->gif_open = false;
  v->gif_step = 1;
  v->jpg = nullptr; v->jpg_cap = 0;
  v->rgb = nullptr; v->rgb_cap = 0;
  v->fw = v->fh = 0;
  v->tw = v->th = 1;
}

static void mv_close(mv_t *v) {
  if (v->gif_open) { mc_gif_close(&v->gif); v->gif_open = false; }
  if (v->open)     { mc_file_close(&v->src); v->open = false; }
  if (v->jpg)      { MC_FREE(v->jpg); v->jpg = nullptr; }
  if (v->rgb)      { MC_FREE(v->rgb); v->rgb = nullptr; }
  v->jpg_cap = v->rgb_cap = 0;
  v->kind = MV_NONE;
}

/* ============================== AVI ====================================== */

/* Parse hdrl + locate movi. Everything is a RIFF chunk: 4cc, 32-bit size,
 * payload, padded to even. LIST chunks carry a further 4cc naming the group. */
static bool mv_avi_parse(mv_t *v, const char **err, const char **warn) {
  mc_br_t br;
  mc_br_init(&br, &v->src.base);

  uint8_t hdr[12];
  if (mc_br_read(&br, hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) ||
      memcmp(hdr + 8, "AVI ", 4)) {
    *err = "Not an AVI";
    return false;
  }

  uint32_t file_end = v->src.base.size;
  uint32_t pos = 12;
  int stream_i = 0;              /* streams seen so far, in declaration order */
  uint32_t strh_scale = 0, strh_rate = 0, strh_len = 0;
  uint8_t  handler[4] = { 0, 0, 0, 0 };
  bool     found_video = false;
  const char *bad_codec = NULL;

  v->vstream = -1;
  v->movi_pos = v->movi_end = 0;

  while (pos + 8 <= file_end) {
    uint8_t ch[12];
    if (!mc_br_seek(&br, pos) || mc_br_read(&br, ch, 8) != 8) break;
    uint32_t sz = mv_r32(ch + 4);
    uint32_t body = pos + 8;

    if (!memcmp(ch, "LIST", 4)) {
      if (mc_br_read(&br, ch + 8, 4) != 4) break;
      if (!memcmp(ch + 8, "movi", 4)) {
        /* The frame data itself: note where it is and step OVER it. Walking
         * into it here would mean reading every chunk header in the file. */
        v->movi_pos = body + 4;
        v->movi_end = body + (sz ? sz : (file_end - body));
        if (v->movi_end > file_end) v->movi_end = file_end;
        if (found_video) break;             /* hdrl normally precedes movi */
        pos = body + sz + (sz & 1);
        continue;
      }
      pos = body + 4;                       /* hdrl / strl / INFO: descend */
      continue;
    }

    if (!memcmp(ch, "avih", 4)) {
      uint8_t a[40];
      if (mc_br_read(&br, a, 40) == 40) {
        v->uspf   = mv_r32(a);
        v->frames = mv_r32(a + 16);
        v->w      = (int)mv_r32(a + 32);
        v->h      = (int)mv_r32(a + 36);
      }
    } else if (!memcmp(ch, "strh", 4)) {
      uint8_t s[40];
      if (mc_br_read(&br, s, 40) == 40) {
        if (!memcmp(s, "vids", 4) && !found_video) {
          memcpy(handler, s + 4, 4);
          strh_scale = mv_r32(s + 20);
          strh_rate  = mv_r32(s + 24);
          strh_len   = mv_r32(s + 32);
          v->vstream = stream_i;
          found_video = true;
        }
        stream_i++;
      }
    } else if (!memcmp(ch, "strf", 4) && found_video && v->vstream == stream_i - 1) {
      uint8_t f[40];
      if (sz >= 40 && mc_br_read(&br, f, 40) == 40) {
        if (v->w <= 0) v->w = (int)mv_r32(f + 4);
        if (v->h <= 0) v->h = (int)(int32_t)mv_r32(f + 8);
        /* biCompression is the authoritative codec id for video streams. */
        if (!mv_codec_is_mjpeg(f + 16) && !mv_codec_is_mjpeg(handler)) {
          const char *nm = mv_codec_name(f + 16);
          if (!nm) nm = mv_codec_name(handler);
          bad_codec = nm;
        }
      }
    }

    pos = body + sz + (sz & 1);             /* chunks are 2-byte aligned */
  }

  if (!v->movi_pos) { *err = "Damaged AVI"; return false; }
  if (!found_video) { *err = "No video track"; return false; }

  /* Prefer the stream's own rate/scale — it is the exact rational frame rate,
   * where avih's microseconds is rounded. */
  if (strh_rate > 0 && strh_scale > 0) {
    uint64_t us = (uint64_t)strh_scale * 1000000ULL / strh_rate;
    if (us >= 1000 && us <= 2000000) v->uspf = (uint32_t)us;
  }
  if (v->uspf < 10000 || v->uspf > 2000000) v->uspf = 66667;   /* 0.5..100 fps */
  if (v->frames == 0) v->frames = strh_len;
  if (v->h < 0) v->h = -v->h;

  /* A wrong-codec AVI is only fatal if the frames really are not JPEGs; the
   * fourcc is advisory (plenty of muxers write junk there), so the first
   * frame gets the final word in mv_next(). Remember the name for the error. */
  if (bad_codec && warn) {
    static char msg[40];
    snprintf(msg, sizeof(msg), "%s video not supported", bad_codec);
    *warn = msg;
  }
  return true;
}

/* Next video chunk in movi. Returns its length (bytes now sitting in v->jpg)
 * or 0 at end of stream. */
static uint32_t mv_avi_next_chunk(mv_t *v) {
  for (;;) {
    uint32_t pos = (uint32_t)v->src.f.position();
    if (pos + 8 > v->movi_end) return 0;

    uint8_t ch[8];
    if (v->src.f.read(ch, 8) != 8) return 0;
    uint32_t sz = mv_r32(ch + 4);

    if (!memcmp(ch, "LIST", 4)) {           /* 'rec ' grouping: step inside */
      uint8_t t[4];
      if (v->src.f.read(t, 4) != 4) return 0;
      continue;
    }
    if (sz > MV_MAX_FRAME) return 0;        /* nonsense length: stop cleanly */

    /* '##dc' / '##db' where ## is the stream number in ASCII. */
    bool is_video = (ch[0] >= '0' && ch[0] <= '9' && ch[1] >= '0' && ch[1] <= '9') &&
                    ((ch[2] == 'd' && (ch[3] == 'c' || ch[3] == 'b')) ||
                     (ch[2] == 'D' && (ch[3] == 'C' || ch[3] == 'B')));
    int strno = (ch[0] - '0') * 10 + (ch[1] - '0');
    if (is_video && (v->vstream < 0 || strno == v->vstream)) {
      if (sz == 0) {                        /* dropped frame: skip, keep going */
        continue;
      }
      if (sz > v->jpg_cap) {
        if (v->jpg) MC_FREE(v->jpg);
        v->jpg = (uint8_t *)MC_MALLOC(sz);
        v->jpg_cap = v->jpg ? sz : 0;
        if (!v->jpg) return 0;
      }
      if (v->src.f.read(v->jpg, sz) != (int)sz) return 0;
      if (sz & 1) v->src.f.seek(v->src.f.position() + 1);
      return sz;
    }

    /* Audio, index, junk, another video stream: skip the payload. */
    if (!v->src.f.seek((uint32_t)v->src.f.position() + sz + (sz & 1))) return 0;
  }
}

/* ============================= raw MJPEG ================================ */

/* Copy the next SOI..EOI run into v->jpg. Returns its length, or 0 at EOF.
 * Deliberately simple: an EOI byte pair inside entropy-coded data is not a
 * thing a compliant encoder emits, and a false split only costs one frame. */
static uint32_t mv_mjpeg_next(mv_t *v) {
  uint32_t len = 0;
  int prev = -1;
  bool in_frame = false;

  for (;;) {
    uint8_t chunk[512];
    int got = v->src.f.read(chunk, sizeof(chunk));
    if (got <= 0) return 0;
    for (int i = 0; i < got; i++) {
      int b = chunk[i];
      if (!in_frame) {
        if (prev == 0xFF && b == 0xD8) {    /* SOI */
          in_frame = true;
          len = 0;
          if (len + 2 > v->jpg_cap) {
            size_t want = 64 * 1024;
            if (v->jpg) MC_FREE(v->jpg);
            v->jpg = (uint8_t *)MC_MALLOC(want);
            v->jpg_cap = v->jpg ? want : 0;
            if (!v->jpg) return 0;
          }
          v->jpg[len++] = 0xFF;
          v->jpg[len++] = 0xD8;
        }
        prev = b;
        continue;
      }
      if (len + 1 > v->jpg_cap) {           /* grow: frames vary a lot in size */
        size_t want = v->jpg_cap * 2;
        if (want > MV_MAX_FRAME) return 0;
        uint8_t *n = (uint8_t *)MC_MALLOC(want);
        if (!n) return 0;
        memcpy(n, v->jpg, len);
        MC_FREE(v->jpg);
        v->jpg = n;
        v->jpg_cap = want;
      }
      v->jpg[len++] = (uint8_t)b;
      if (prev == 0xFF && b == 0xD9) {      /* EOI: frame complete */
        /* Rewind the reader to just after this frame. */
        v->src.f.seek(v->src.f.position() - (uint32_t)(got - 1 - i));
        return len;
      }
      prev = b;
    }
  }
}

/* ============================ JPEG frame -> RGB565 ======================= */

/* Decode v->jpg (len bytes) into v->rgb, scaled to roughly the player view.
 * The buffer is grow-only: after the first frame of a normal video, every
 * later frame reuses it exactly. */
static bool mv_decode_jpeg_frame(mv_t *v, uint32_t len) {
  esp_jpeg_image_cfg_t cfg = {};
  cfg.indata      = v->jpg;
  cfg.indata_size = len;
  cfg.out_format  = JPEG_IMAGE_FORMAT_RGB565;
  cfg.out_scale   = JPEG_IMAGE_SCALE_0;
  cfg.flags.swap_color_bytes = 0;

  esp_jpeg_image_output_t info = {};
  if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0) return false;

  int want = 0;
  for (int s = 1; s <= 3; s++) {
    int w = info.width >> s, h = info.height >> s;
    if (w >= v->tw || h >= v->th) want = s; else break;
  }
  cfg.out_scale = (esp_jpeg_image_scale_t)want;

  esp_jpeg_image_output_t sz = {};
  if (esp_jpeg_get_image_info(&cfg, &sz) != ESP_OK || sz.output_len == 0) return false;
  if (sz.output_len > v->rgb_cap) {
    if (v->rgb) MC_FREE(v->rgb);
    v->rgb = (uint16_t *)MC_MALLOC(sz.output_len);
    v->rgb_cap = v->rgb ? sz.output_len : 0;
    if (!v->rgb) return false;
  }
  cfg.outbuf      = (uint8_t *)v->rgb;
  cfg.outbuf_size = v->rgb_cap;
  esp_jpeg_image_output_t done = {};
  if (esp_jpeg_decode(&cfg, &done) != ESP_OK) return false;
  v->fw = done.width;
  v->fh = done.height;
  return true;
}

/* ============================== open / play ============================== */

/* Open `path` for playback, decoding for a tw x th view. On failure *err is a
 * short reason fit to show the user. */
static bool mv_open(mv_t *v, fs::FS &fsys, const char *path, int tw, int th,
                    const char **err) {
  mv_reset(v);
  v->tw = tw > 0 ? tw : 1;
  v->th = th > 0 ? th : 1;
  *err = "Can't open";

  if (!mc_file_open(&v->src, fsys, path)) return false;
  v->open = true;

  uint8_t magic[16];
  int n = (int)v->src.f.read(magic, sizeof(magic));
  v->src.f.seek(0);
  int fmt = mc_sniff(magic, n);

  if (fmt == MC_FMT_AVI) {
    const char *werr = NULL;              /* codec the header claims we can't play */
    if (!mv_avi_parse(v, err, &werr)) { mv_close(v); return false; }
    v->kind = MV_AVI;
    if (!v->src.f.seek(v->movi_pos)) { mv_close(v); *err = "Damaged AVI"; return false; }
    /* Prove the first frame really is a JPEG before promising playback. */
    uint32_t l = mv_avi_next_chunk(v);
    if (l == 0 || l < 4 || v->jpg[0] != 0xFF || v->jpg[1] != 0xD8) {
      mv_close(v);
      *err = werr ? werr : "Unsupported video codec";
      return false;
    }
    v->src.f.seek(v->movi_pos);
    *err = NULL;
    return true;
  }

  if (fmt == MC_FMT_GIF) {
    int frames = 0, gw = 0, gh = 0;
    uint32_t total_ms = 0;
    if (!mc_gif_scan(&v->src.base, &frames, &total_ms, &gw, &gh) || frames <= 0) {
      mv_close(v);
      *err = "Damaged GIF";
      return false;
    }
    v->src.f.seek(0);
    if (!mc_gif_open(&v->gif, &v->src.base, err)) { mv_close(v); return false; }
    v->gif_open = true;
    v->kind   = MV_GIF;
    v->w      = v->gif.lw;
    v->h      = v->gif.lh;
    v->frames = (uint32_t)frames;
    v->uspf   = frames ? (uint32_t)((uint64_t)total_ms * 1000ULL / frames) : 100000;
    /* One fixed output size for the whole animation (the canvas never
     * changes size), so the frame buffer is allocated exactly once. */
    int sw = (v->w + v->tw - 1) / v->tw, sh = (v->h + v->th - 1) / v->th;
    v->gif_step = (sw > sh) ? sw : sh;
    if (v->gif_step < 1) v->gif_step = 1;
    v->fw = (v->w + v->gif_step - 1) / v->gif_step;
    v->fh = (v->h + v->gif_step - 1) / v->gif_step;
    v->rgb_cap = (size_t)v->fw * v->fh * 2;
    v->rgb = (uint16_t *)MC_MALLOC(v->rgb_cap);
    if (!v->rgb) { mv_close(v); *err = "Out of memory"; return false; }
    memset(v->rgb, 0, v->rgb_cap);
    *err = NULL;
    return true;
  }

  if (fmt == MC_FMT_JPEG) {               /* bare MJPEG stream */
    v->kind = MV_MJPEG;
    v->uspf = 100000;                     /* 10 fps: the format states nothing */
    v->frames = 0;                        /* unknown without scanning it all  */
    *err = NULL;
    return true;
  }

  mv_close(v);
  if (fmt == MC_FMT_MP4)  *err = "MP4/MOV not supported";
  else if (fmt == MC_FMT_WEBP) *err = "WebP not supported";
  else *err = "Unsupported video";
  return false;
}

/* Decode the next frame into v->rgb (v->fw x v->fh).
 * 1 = frame ready, 0 = end of stream, -1 = decode failure on this frame. */
static int mv_next(mv_t *v) {
  if (!v->open) return 0;
  v->delay_ms = v->uspf / 1000;

  if (v->kind == MV_GIF) {
    int r = mc_gif_next(&v->gif);
    if (r <= 0) return 0;
    /* A delay of 0 or 1 centisecond means "as fast as possible", which every
     * real player renders as 100 ms — matching that keeps GIFs from playing
     * at absurd speed. */
    uint32_t cs = (uint32_t)(v->gif.delay_cs < 2 ? 10 : v->gif.delay_cs);
    v->delay_ms = cs * 10;
    mc_gif_canvas_into(&v->gif, v->rgb, v->fw, v->fh, v->gif_step);
    v->cur++;
    return 1;
  }

  uint32_t len = (v->kind == MV_AVI) ? mv_avi_next_chunk(v) : mv_mjpeg_next(v);
  if (len == 0) return 0;
  if (!mv_decode_jpeg_frame(v, len)) return -1;
  v->cur++;
  return 1;
}

/* Back to frame zero. GIF has to re-read its header (the canvas is a running
 * accumulation), which is why this is not just a seek. */
static bool mv_rewind(mv_t *v) {
  if (!v->open) return false;
  v->cur = 0;
  if (v->kind == MV_GIF) {
    mc_gif_close(&v->gif);
    v->gif_open = false;
    if (!v->src.f.seek(0)) return false;
    const char *e = NULL;
    if (!mc_gif_open(&v->gif, &v->src.base, &e)) return false;
    v->gif_open = true;
    return true;
  }
  return v->src.f.seek(v->kind == MV_AVI ? v->movi_pos : 0);
}

/* Total duration in seconds, or 0 when the container does not say. */
static uint32_t mv_duration_s(const mv_t *v) {
  if (!v->frames) return 0;
  return (uint32_t)(((uint64_t)v->frames * v->uspf) / 1000000ULL);
}

/* ---- thumbnails -----------------------------------------------------------
 * First frame of a video as a standalone RGB565 buffer the caller owns. GIFs
 * go through the still decoder (same first frame, one code path). */
static bool mv_thumb(fs::FS &fsys, const char *path, int tw, int th, bool cover,
                     uint16_t **out, int *ow, int *oh, const char **err) {
  *out = nullptr;
  int fmt = mc_sniff_file(fsys, path);
  if (fmt == MC_FMT_GIF)
    return mc_decode_still(fsys, path, tw, th, cover, out, ow, oh, err);

  mv_t v;
  if (!mv_open(&v, fsys, path, tw, th, err)) return false;
  int r = mv_next(&v);
  bool ok = false;
  if (r != 1 || v.fw <= 0 || v.fh <= 0) {
    *err = "No frames";
  } else {
    size_t bytes = (size_t)v.fw * v.fh * 2;
    uint16_t *buf = (uint16_t *)MC_MALLOC(bytes);
    if (!buf) {
      *err = "Out of memory";
    } else {
      memcpy(buf, v.rgb, bytes);
      *out = buf; *ow = v.fw; *oh = v.fh;
      ok = true;
    }
  }
  mv_close(&v);
  return ok;
}

/* Does this file hold more than one frame? Used to decide whether a GIF is a
 * picture or a video; cheap (no pixels are decoded). */
static bool mv_gif_is_animated(fs::FS &fsys, const char *path) {
  mc_file_src_t s;
  if (!mc_file_open(&s, fsys, path)) return false;
  int frames = 0;
  bool ok = mc_gif_scan(&s.base, &frames, nullptr, nullptr, nullptr);
  mc_file_close(&s);
  return ok && frames > 1;
}
