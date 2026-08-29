/* ============================================================================
 *  media_codecs.h — still-image decoders for the Gallery (and anything else
 *  that needs a picture off the card).
 *
 *  WHY THIS EXISTS. The Gallery used to understand exactly one thing: the
 *  baseline JPEG an OV sensor hands us. Anything a user copied onto the card
 *  from a phone or a PC — a PNG screenshot, a BMP export, a GIF — simply did
 *  not appear. This header adds those, decoding straight to the RGB565 the UI
 *  draws, with the same "downscale while decoding" discipline the JPEG path
 *  already had: a 12-megapixel PNG must never need a 12-megapixel buffer.
 *
 *  FORMATS
 *    JPEG  baseline, via the ESP32 core's esp_jpeg (tjpgd). Progressive JPEGs
 *          are NOT decodable by tjpgd and are reported as such.
 *    PNG   8/16-bit and sub-byte depths, greyscale / RGB / palette / alpha,
 *          streamed through the ROM's miniz inflate. Adam7-interlaced files
 *          are reported, not decoded (they need the whole raster resident).
 *    BMP   1/4/8-bit palette, 16-bit 555 & BITFIELDS, 24-bit, 32-bit, either
 *          row order. RLE and embedded-JPEG/PNG variants are reported.
 *    GIF   87a/89a, interlaced or not, transparency and all three disposal
 *          methods — the decoder here is a full animation engine because the
 *          video player drives it frame by frame (see media_video.h); the
 *          still path just takes frame 0.
 *
 *  MEMORY. Everything large lives in PSRAM and is handed back to the caller,
 *  who frees it with heap_caps_free(). Decoding is ROW-STREAMED: at most a
 *  couple of source rows, a 32 KB inflate window and the (already downscaled)
 *  destination are resident, so source size is bounded by the card, not RAM.
 *  The one exception is GIF, which is defined in terms of a persistent canvas
 *  and therefore keeps one at logical-screen size.
 *
 *  DOWNSCALING is a box filter (average of each step x step source block), not
 *  point sampling: thumbnails of detailed photos stay readable instead of
 *  aliasing into noise. JPEG gets the same effect for free from tjpgd's
 *  1/2, 1/4 and 1/8 DCT scaling.
 *
 *  PORTABILITY. Everything above the "filesystem bindings" section at the
 *  bottom is plain C on top of two callbacks (read + seek) and MC_MALLOC, so
 *  the format logic builds and runs on a host for testing (-DMC_HOST_TEST).
 *  Only the bottom section knows about Arduino, FS or esp_jpeg.
 *
 *  Header-only. INCLUDE BEFORE media_video.h and app_gallery.h.
 * ========================================================================== */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

/* ---- allocation ------------------------------------------------------------
 * Pictures are big and short-lived: PSRAM, always. A failed allocation is a
 * normal outcome here (a 100-megapixel PNG is a thing that exists), so every
 * caller checks instead of assuming. */
#ifdef MC_HOST_TEST
#define MC_MALLOC(n) malloc((size_t)(n))
#define MC_FREE(p)   free(p)
#else
#include "esp_heap_caps.h"
#define MC_MALLOC(n) heap_caps_malloc((size_t)(n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MC_FREE(p)   heap_caps_free(p)
#endif

/* Never allocate a destination bigger than this many pixels (2 bytes each).
 * The photo view wants roughly screen-sized output; this is the backstop that
 * keeps a panoramic source from asking for megabytes anyway. */
#define MC_MAX_OUT_PX (700u * 1024u)

/* Recognised container/codec identities (sniffed from magic bytes). */
enum {
  MC_FMT_UNKNOWN = 0,
  MC_FMT_JPEG,
  MC_FMT_PNG,
  MC_FMT_BMP,
  MC_FMT_GIF,
  MC_FMT_WEBP,      /* recognised, not decodable here */
  MC_FMT_AVI,
  MC_FMT_MP4,       /* MP4/MOV — recognised, not decodable here */
  MC_FMT_TIFF,      /* recognised, not decodable here */
};

static const char *mc_fmt_name(int fmt) {
  switch (fmt) {
    case MC_FMT_JPEG: return "JPEG";
    case MC_FMT_PNG:  return "PNG";
    case MC_FMT_BMP:  return "BMP";
    case MC_FMT_GIF:  return "GIF";
    case MC_FMT_WEBP: return "WebP";
    case MC_FMT_AVI:  return "AVI";
    case MC_FMT_MP4:  return "MP4";
    case MC_FMT_TIFF: return "TIFF";
    default:          return "file";
  }
}

/* Magic-byte sniff over the first bytes of a file. Extensions lie (a ".jpg"
 * saved by a phone screenshot tool is routinely a PNG), so the decoders are
 * chosen by content; extensions only decide what the Gallery LISTS. */
static int mc_sniff(const uint8_t *b, int n) {
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return MC_FMT_JPEG;
  if (n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8))            return MC_FMT_PNG;
  if (n >= 2 && b[0] == 'B' && b[1] == 'M')                    return MC_FMT_BMP;
  if (n >= 6 && !memcmp(b, "GIF8", 4))                         return MC_FMT_GIF;
  if (n >= 12 && !memcmp(b, "RIFF", 4)) {
    if (!memcmp(b + 8, "WEBP", 4)) return MC_FMT_WEBP;
    if (!memcmp(b + 8, "AVI ", 4)) return MC_FMT_AVI;
  }
  if (n >= 12 && !memcmp(b + 4, "ftyp", 4))                    return MC_FMT_MP4;
  if (n >= 8 && !memcmp(b, "\x00\x00\x00\x14moov", 8))         return MC_FMT_MP4;
  if (n >= 4 && (!memcmp(b, "II*\x00", 4) || !memcmp(b, "MM\x00*", 4)))
    return MC_FMT_TIFF;
  return MC_FMT_UNKNOWN;
}

/* ============================ byte source ===============================
 * Two callbacks is all any of the decoders need. The Gallery binds these to
 * an Arduino File at the bottom of this header; the host test binds them to
 * a FILE*. */
typedef struct mc_src_s {
  int  (*read)(struct mc_src_s *s, uint8_t *dst, int n);  /* bytes actually read */
  bool (*seek)(struct mc_src_s *s, uint32_t pos);
  uint32_t size;
  void    *ctx;
} mc_src_t;

/* Buffered view over a source. GIF in particular is byte-at-a-time; going
 * through the VFS for each one would be pathologically slow on an SD card. */
#define MC_BUF_SZ 1024
typedef struct {
  mc_src_t *src;
  uint8_t   buf[MC_BUF_SZ];
  int       len, pos;      /* valid bytes in buf, and our cursor into them */
  uint32_t  abs;           /* file offset of buf[0] */
  bool      eof;
} mc_br_t;

static void mc_br_init(mc_br_t *b, mc_src_t *s) {
  b->src = s; b->len = b->pos = 0; b->abs = 0; b->eof = false;
}
static uint32_t mc_br_pos(const mc_br_t *b) { return b->abs + (uint32_t)b->pos; }

static bool mc_br_fill(mc_br_t *b) {
  if (b->pos < b->len) return true;
  b->abs += (uint32_t)b->len;
  b->pos  = 0;
  b->len  = b->src->read(b->src, b->buf, MC_BUF_SZ);
  if (b->len <= 0) { b->len = 0; b->eof = true; return false; }
  return true;
}

static int mc_br_u8(mc_br_t *b) {
  if (!mc_br_fill(b)) return -1;
  return b->buf[b->pos++];
}

/* Reads n bytes; returns how many it got. Bulk reads bypass the buffer so a
 * megabyte of IDAT does not get copied twice. */
static int mc_br_read(mc_br_t *b, uint8_t *dst, int n) {
  int got = 0;
  while (got < n) {
    if (b->pos < b->len) {                      /* drain the window first */
      int avail = b->len - b->pos;
      int take  = (n - got < avail) ? (n - got) : avail;
      memcpy(dst + got, b->buf + b->pos, (size_t)take);
      b->pos += take;
      got    += take;
      continue;
    }
    if (n - got >= MC_BUF_SZ) {                 /* straight through */
      b->abs += (uint32_t)b->len;               /* window consumed and dropped */
      b->len  = b->pos = 0;
      int r = b->src->read(b->src, dst + got, n - got);
      if (r <= 0) { b->eof = true; break; }
      b->abs += (uint32_t)r;
      got += r;
      continue;
    }
    if (!mc_br_fill(b)) break;
  }
  return got;
}

static bool mc_br_seek(mc_br_t *b, uint32_t pos) {
  if (pos >= b->abs && pos < b->abs + (uint32_t)b->len) {   /* inside the window */
    b->pos = (int)(pos - b->abs);
    return true;
  }
  if (!b->src->seek(b->src, pos)) return false;
  b->abs = pos; b->len = b->pos = 0; b->eof = false;
  return true;
}

static bool mc_br_skip(mc_br_t *b, uint32_t n) {
  if (n <= (uint32_t)(b->len - b->pos)) { b->pos += (int)n; return true; }
  return mc_br_seek(b, mc_br_pos(b) + n);
}

/* Big-endian (PNG) and little-endian (BMP/GIF/RIFF) scalar reads. */
static uint32_t mc_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint32_t mc_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t mc_le16(const uint8_t *p) {
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* ============================ downscaler ================================
 * Streaming box filter. Source rows arrive top to bottom as RGB888; each
 * step x step block of source pixels is averaged into one destination pixel.
 * `flip` writes destination rows bottom-up, which is what a normal (bottom-up)
 * BMP needs without seeking backwards through the file. */
typedef struct {
  int       sw, sh, ow, oh, step;
  bool      flip;
  uint32_t *acc;      /* ow * 3 accumulators for the block row in progress */
  uint32_t *cnt;      /* ow     source pixels folded into each accumulator */
  uint16_t *out;      /* ow * oh RGB565, handed to the caller on success   */
  int       in_row;   /* next source row index expected                    */
  int       block;    /* destination row the accumulators belong to        */
} mc_ds_t;

static void mc_ds_free_scratch(mc_ds_t *d) {
  if (d->acc) { MC_FREE(d->acc); d->acc = NULL; }
  if (d->cnt) { MC_FREE(d->cnt); d->cnt = NULL; }
}

/* `cover`  — output must at least fill a tw x th cell (grid thumbnails, which
 *            are centre-cropped by the UI): use the SMALLER reduction.
 * !cover   — output must fit inside tw x th (the full-screen viewer): use the
 *            LARGER reduction, so neither axis overshoots. */
static bool mc_ds_init(mc_ds_t *d, int sw, int sh, int tw, int th, bool cover,
                       bool flip) {
  memset(d, 0, sizeof(*d));
  if (sw <= 0 || sh <= 0) return false;
  if (tw < 1) tw = 1;
  if (th < 1) th = 1;

  int a = sw / tw, b = sh / th;
  int step;
  if (cover) step = (a < b) ? a : b;
  else {
    a = (sw + tw - 1) / tw;
    b = (sh + th - 1) / th;
    step = (a > b) ? a : b;
  }
  if (step < 1) step = 1;

  int ow = (sw + step - 1) / step, oh = (sh + step - 1) / step;
  while ((uint32_t)ow * (uint32_t)oh > MC_MAX_OUT_PX) {   /* absolute backstop */
    step++;
    ow = (sw + step - 1) / step;
    oh = (sh + step - 1) / step;
  }

  d->sw = sw; d->sh = sh; d->ow = ow; d->oh = oh; d->step = step; d->flip = flip;
  d->acc = (uint32_t *)MC_MALLOC((size_t)ow * 3 * sizeof(uint32_t));
  d->cnt = (uint32_t *)MC_MALLOC((size_t)ow * sizeof(uint32_t));
  d->out = (uint16_t *)MC_MALLOC((size_t)ow * (size_t)oh * 2);
  if (!d->acc || !d->cnt || !d->out) {
    mc_ds_free_scratch(d);
    if (d->out) { MC_FREE(d->out); d->out = NULL; }
    return false;
  }
  memset(d->acc, 0, (size_t)ow * 3 * sizeof(uint32_t));
  memset(d->cnt, 0, (size_t)ow * sizeof(uint32_t));
  memset(d->out, 0, (size_t)ow * (size_t)oh * 2);
  d->in_row = 0;
  d->block  = 0;
  return true;
}

static void mc_ds_flush(mc_ds_t *d) {
  int row = d->flip ? (d->oh - 1 - d->block) : d->block;
  if (row >= 0 && row < d->oh) {
    uint16_t *o = d->out + (size_t)row * d->ow;
    for (int i = 0; i < d->ow; i++) {
      uint32_t c = d->cnt[i] ? d->cnt[i] : 1;
      uint32_t r = d->acc[i * 3 + 0] / c;
      uint32_t g = d->acc[i * 3 + 1] / c;
      uint32_t bl = d->acc[i * 3 + 2] / c;
      o[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
    }
  }
  memset(d->acc, 0, (size_t)d->ow * 3 * sizeof(uint32_t));
  memset(d->cnt, 0, (size_t)d->ow * sizeof(uint32_t));
}

/* Fold one full source row (sw RGB888 pixels) into the destination. */
static void mc_ds_row(mc_ds_t *d, const uint8_t *rgb) {
  int blk = d->in_row / d->step;
  if (blk != d->block) { mc_ds_flush(d); d->block = blk; }
  const int step = d->step, ow = d->ow;
  for (int x = 0; x < d->sw; x++) {
    int o = x / step;
    if (o >= ow) break;
    d->acc[o * 3 + 0] += rgb[x * 3 + 0];
    d->acc[o * 3 + 1] += rgb[x * 3 + 1];
    d->acc[o * 3 + 2] += rgb[x * 3 + 2];
    d->cnt[o]++;
  }
  d->in_row++;
}

/* Flush the trailing partial block and release the scratch. `out` survives. */
static void mc_ds_finish(mc_ds_t *d) {
  mc_ds_flush(d);
  mc_ds_free_scratch(d);
}

/* ============================ inflate ===================================
 * PNG needs a zlib stream. The ESP32 mask ROM carries miniz's tinfl core, so
 * there is nothing to link and nothing to flash for it — we only supply the
 * 32 KB sliding window it decompresses through. The host test build swaps in
 * zlib behind the same three calls. */
#ifdef MC_HOST_TEST
#include <zlib.h>
#define MC_HAVE_INFLATE 1
#elif __has_include("miniz.h")
#include "miniz.h"                 /* esp_rom/include/miniz.h — the usual place */
#define MC_HAVE_INFLATE 1
#elif __has_include("rom/miniz.h")
#include "rom/miniz.h"
#define MC_HAVE_INFLATE 1
#else
#define MC_HAVE_INFLATE 0          /* no inflate: PNG says so instead of failing to build */
#endif

#define MC_INF_WND 32768

#if MC_HAVE_INFLATE

typedef struct {
  uint8_t *wnd;
  int      ofs;          /* write cursor in the ring (miniz build only) */
#ifdef MC_HOST_TEST
  z_stream z;
  bool     zinit;
#else
  tinfl_decompressor *d;
#endif
} mc_inf_t;

static bool mc_inf_init(mc_inf_t *i) {
  memset(i, 0, sizeof(*i));
  i->wnd = (uint8_t *)MC_MALLOC(MC_INF_WND);
  if (!i->wnd) return false;
#ifdef MC_HOST_TEST
  if (inflateInit(&i->z) != Z_OK) { MC_FREE(i->wnd); i->wnd = NULL; return false; }
  i->zinit = true;
#else
  i->d = (tinfl_decompressor *)MC_MALLOC(sizeof(tinfl_decompressor));
  if (!i->d) { MC_FREE(i->wnd); i->wnd = NULL; return false; }
  tinfl_init(i->d);
#endif
  return true;
}

static void mc_inf_free(mc_inf_t *i) {
#ifdef MC_HOST_TEST
  if (i->zinit) { inflateEnd(&i->z); i->zinit = false; }
#else
  if (i->d) { MC_FREE(i->d); i->d = NULL; }
#endif
  if (i->wnd) { MC_FREE(i->wnd); i->wnd = NULL; }
}

/* One decompression step. Consumes from *in (advancing it and *in_avail) and
 * reports the bytes it produced as a pointer into the window.
 * Returns 0 = stream finished, 1 = wants more input, 2 = more output pending
 * for the same input, -1 = corrupt. */
static int mc_inf_step(mc_inf_t *i, const uint8_t **in, size_t *in_avail,
                       bool more_input, uint8_t **out, size_t *out_n) {
#ifdef MC_HOST_TEST
  (void)more_input;
  i->z.next_in   = (Bytef *)*in;
  i->z.avail_in  = (uInt)*in_avail;
  i->z.next_out  = i->wnd;
  i->z.avail_out = MC_INF_WND;
  int r = inflate(&i->z, Z_NO_FLUSH);
  size_t used = *in_avail - i->z.avail_in;
  *in       += used;
  *in_avail -= used;
  *out   = i->wnd;
  *out_n = (size_t)(MC_INF_WND - i->z.avail_out);
  if (r == Z_STREAM_END) return 0;
  if (r != Z_OK && r != Z_BUF_ERROR) return -1;
  return (i->z.avail_out == 0) ? 2 : 1;
#else
  size_t isz = *in_avail;
  size_t osz = (size_t)(MC_INF_WND - i->ofs);
  int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (more_input ? TINFL_FLAG_HAS_MORE_INPUT : 0);
  tinfl_status st = tinfl_decompress(i->d, *in, &isz, i->wnd, i->wnd + i->ofs,
                                     &osz, (mz_uint32)flags);
  *in       += isz;
  *in_avail -= isz;
  *out   = i->wnd + i->ofs;
  *out_n = osz;
  i->ofs = (int)((i->ofs + (int)osz) & (MC_INF_WND - 1));
  if (st == TINFL_STATUS_DONE)              return 0;
  if (st == TINFL_STATUS_NEEDS_MORE_INPUT)  return 1;
  if (st == TINFL_STATUS_HAS_MORE_OUTPUT)   return 2;
  return -1;
#endif
}

#endif  /* MC_HAVE_INFLATE */

/* ============================== PNG ===================================== */

/* Per-row state the PNG decoder carries between inflate steps. */
typedef struct {
  int      w, h, depth, ct, channels;
  int      rw;          /* pixels in the row being assembled (pass width)    */
  size_t   stride;      /* filtered bytes per row, excluding the filter byte */
  int      fbpp;        /* filter's "bytes per pixel" distance (>= 1)        */
  uint8_t *cur, *prev;  /* two unfiltered row buffers                        */
  uint8_t *rgb;         /* w * 3 expansion scratch                           */
  size_t   fill;        /* bytes of the current row assembled so far         */
  int      filter;      /* this row's filter type                            */
  bool     have_filter;
  int      row;         /* rows finished (progressive files: total so far)   */
  uint8_t  pal[256 * 3];
  uint8_t  pal_a[256];
  int      pal_n;
  /* Adam7 */
  bool     interlaced;
  int      pass;        /* 0..6, 7 = done                                    */
  int      prow;        /* row index within the current pass                 */
  int      pass_w, pass_h;
  bool     done;
} mc_png_t;

/* Adam7 pass geometry: first pixel, then the step, in each axis. */
static const int mc_a7_x0[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const int mc_a7_y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const int mc_a7_dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const int mc_a7_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static uint8_t mc_paeth(uint8_t a, uint8_t b, uint8_t c) {
  int p = (int)a + (int)b - (int)c;
  int pa = abs(p - (int)a), pb = abs(p - (int)b), pc = abs(p - (int)c);
  if (pa <= pb && pa <= pc) return a;
  return (pb <= pc) ? b : c;
}

static void mc_png_unfilter(mc_png_t *p) {
  uint8_t *c = p->cur, *pr = p->prev;
  const int bpp = p->fbpp;
  const size_t n = p->stride;
  switch (p->filter) {
    case 0: break;
    case 1: for (size_t i = (size_t)bpp; i < n; i++) c[i] = (uint8_t)(c[i] + c[i - bpp]); break;
    case 2: for (size_t i = 0; i < n; i++) c[i] = (uint8_t)(c[i] + pr[i]); break;
    case 3:
      for (size_t i = 0; i < n; i++) {
        int left = (i >= (size_t)bpp) ? c[i - bpp] : 0;
        c[i] = (uint8_t)(c[i] + ((left + pr[i]) >> 1));
      }
      break;
    case 4:
      for (size_t i = 0; i < n; i++) {
        uint8_t left = (i >= (size_t)bpp) ? c[i - bpp] : 0;
        uint8_t up   = pr[i];
        uint8_t ul   = (i >= (size_t)bpp) ? pr[i - bpp] : 0;
        c[i] = (uint8_t)(c[i] + mc_paeth(left, up, ul));
      }
      break;
    default: break;                     /* unknown filter: leave the row as-is */
  }
}

/* Pull sample #idx (0-based, across the whole row) at the file's bit depth. */
static uint32_t mc_png_sample(const uint8_t *row, int depth, size_t idx) {
  switch (depth) {
    case 8:  return row[idx];
    case 16: return row[idx * 2];                       /* high byte is plenty */
    case 4:  return (row[idx >> 1] >> (((idx & 1) ^ 1) * 4)) & 0x0F;
    case 2:  return (row[idx >> 2] >> ((3 - (idx & 3)) * 2)) & 0x03;
    case 1:  return (row[idx >> 3] >> (7 - (idx & 7))) & 0x01;
    default: return 0;
  }
}

/* Unfiltered row -> RGB888, alpha composited over black (the UI's background,
 * so a transparent PNG icon lands looking the way it should). */
static void mc_png_expand(mc_png_t *p) {
  const int w = p->rw, d = p->depth;
  const uint8_t *s = p->cur;
  uint8_t *o = p->rgb;
  const uint32_t maxv = (d >= 8) ? 255u : (uint32_t)((1u << d) - 1u);
  for (int x = 0; x < w; x++) {
    uint32_t r, g, b, a = 255;
    switch (p->ct) {
      case 0: {                                   /* greyscale */
        uint32_t v = mc_png_sample(s, d, (size_t)x);
        if (d < 8) v = v * 255u / maxv;
        r = g = b = v;
        break;
      }
      case 2: {                                   /* truecolour */
        size_t i = (size_t)x * 3;
        r = mc_png_sample(s, d, i);
        g = mc_png_sample(s, d, i + 1);
        b = mc_png_sample(s, d, i + 2);
        break;
      }
      case 3: {                                   /* palette */
        uint32_t v = mc_png_sample(s, d, (size_t)x);
        if ((int)v >= p->pal_n) v = 0;
        r = p->pal[v * 3 + 0]; g = p->pal[v * 3 + 1]; b = p->pal[v * 3 + 2];
        a = p->pal_a[v];
        break;
      }
      case 4: {                                   /* grey + alpha */
        size_t i = (size_t)x * 2;
        uint32_t v = mc_png_sample(s, d, i);
        if (d < 8) v = v * 255u / maxv;
        r = g = b = v;
        a = mc_png_sample(s, d, i + 1);
        if (d < 8) a = a * 255u / maxv;
        break;
      }
      default: {                                  /* ct 6: RGBA */
        size_t i = (size_t)x * 4;
        r = mc_png_sample(s, d, i);
        g = mc_png_sample(s, d, i + 1);
        b = mc_png_sample(s, d, i + 2);
        a = mc_png_sample(s, d, i + 3);
        break;
      }
    }
    if (a != 255) { r = r * a / 255u; g = g * a / 255u; b = b * a / 255u; }
    o[x * 3 + 0] = (uint8_t)r;
    o[x * 3 + 1] = (uint8_t)g;
    o[x * 3 + 2] = (uint8_t)b;
  }
}

/* Row geometry for the current pass (or the whole image, non-interlaced). */
static void mc_png_row_geom(mc_png_t *p) {
  p->stride = ((size_t)p->rw * (size_t)p->channels * (size_t)p->depth + 7) / 8;
  p->fbpp   = (p->channels * p->depth + 7) / 8;
  if (p->fbpp < 1) p->fbpp = 1;
}

/* Move to the next non-empty Adam7 pass (or mark the image finished). */
static void mc_png_next_pass(mc_png_t *p) {
  while (++p->pass < 7) {
    int pw = (p->w - mc_a7_x0[p->pass] + mc_a7_dx[p->pass] - 1) / mc_a7_dx[p->pass];
    int ph = (p->h - mc_a7_y0[p->pass] + mc_a7_dy[p->pass] - 1) / mc_a7_dy[p->pass];
    if (pw > 0 && ph > 0) {
      p->pass_w = pw; p->pass_h = ph; p->prow = 0; p->rw = pw;
      mc_png_row_geom(p);
      memset(p->prev, 0, p->stride);     /* each pass filters from a zero row */
      return;
    }
  }
  p->done = true;
}

/* An interlaced pass's row goes to scattered destination pixels, so it cannot
 * go through the streaming box filter — later (finer) passes simply overwrite
 * what earlier ones put down, which is exactly how a progressive PNG is meant
 * to render, just without the intermediate redraws. */
static void mc_png_scatter(mc_png_t *p, mc_ds_t *ds) {
  int sy = mc_a7_y0[p->pass] + p->prow * mc_a7_dy[p->pass];
  int oy = sy / ds->step;
  if (oy >= ds->oh) return;
  uint16_t *o = ds->out + (size_t)oy * ds->ow;
  const int x0 = mc_a7_x0[p->pass], dx = mc_a7_dx[p->pass];
  for (int i = 0; i < p->rw; i++) {
    int ox = (x0 + i * dx) / ds->step;
    if (ox >= ds->ow) break;
    uint32_t r = p->rgb[i * 3 + 0], g = p->rgb[i * 3 + 1], b = p->rgb[i * 3 + 2];
    o[ox] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
}

/* Feed inflate output into the row assembler; emits finished rows downstream.
 * PNG rows are "filter byte + stride bytes" back to back with no alignment,
 * which is why this is a byte pump rather than a memcpy per row. */
static void mc_png_consume(mc_png_t *p, mc_ds_t *ds, const uint8_t *b, size_t n) {
  while (n) {
    if (p->done) return;                           /* trailing junk: ignore */
    if (!p->have_filter) {
      p->filter = *b++; n--;
      p->have_filter = true;
      p->fill = 0;
      continue;
    }
    size_t want = p->stride - p->fill;
    size_t take = (n < want) ? n : want;
    memcpy(p->cur + p->fill, b, take);
    p->fill += take;
    b += take; n -= take;
    if (p->fill == p->stride) {
      mc_png_unfilter(p);
      mc_png_expand(p);
      if (p->interlaced) mc_png_scatter(p, ds);
      else               mc_ds_row(ds, p->rgb);
      uint8_t *t = p->prev; p->prev = p->cur; p->cur = t;
      p->row++;
      p->have_filter = false;
      p->fill = 0;
      if (p->interlaced) {
        if (++p->prow >= p->pass_h) mc_png_next_pass(p);
      } else if (p->row >= p->h) {
        p->done = true;
      }
    }
  }
}

#if !MC_HAVE_INFLATE
/* No deflate implementation on this target: PNG stays a named, explained gap
 * rather than a build error. */
static bool mc_png_decode(mc_br_t *br, int tw, int th, bool cover,
                          uint16_t **out, int *ow, int *oh, const char **err) {
  (void)br; (void)tw; (void)th; (void)cover; (void)out; (void)ow; (void)oh;
  *err = "PNG not supported";
  return false;
}
#else
static bool mc_png_decode(mc_br_t *br, int tw, int th, bool cover,
                          uint16_t **out, int *ow, int *oh, const char **err) {
  mc_png_t p;
  mc_ds_t  ds;
  mc_inf_t inf;
  bool ok = false, started = false, inf_ok = false, ds_ok = false;
  uint8_t *inbuf = NULL;

  memset(&p, 0, sizeof(p));
  memset(&ds, 0, sizeof(ds));
  memset(&inf, 0, sizeof(inf));
  for (int i = 0; i < 256; i++) p.pal_a[i] = 255;

  uint8_t sig[8];
  if (mc_br_read(br, sig, 8) != 8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) {
    *err = "Not a PNG";
    return false;
  }

  /* Chunk walk. IHDR/PLTE/tRNS are metadata; the first IDAT switches us into
   * the streaming decode below, which keeps pulling IDATs until the zlib
   * stream ends. */
  for (;;) {
    uint8_t ch[8];
    if (mc_br_read(br, ch, 8) != 8) { *err = "Truncated PNG"; goto done; }
    uint32_t clen = mc_be32(ch);
    const uint8_t *type = ch + 4;

    if (!memcmp(type, "IHDR", 4)) {
      uint8_t ih[13];
      if (clen < 13 || mc_br_read(br, ih, 13) != 13) { *err = "Bad PNG header"; goto done; }
      mc_br_skip(br, clen - 13 + 4);
      p.w     = (int)mc_be32(ih);
      p.h     = (int)mc_be32(ih + 4);
      p.depth = ih[8];
      p.ct    = ih[9];
      if (p.w <= 0 || p.h <= 0 || p.w > 20000 || p.h > 20000) { *err = "Bad PNG size"; goto done; }
      p.interlaced = (ih[12] == 1);
      if (ih[12] > 1) { *err = "Unsupported PNG"; goto done; }
      if (ih[10] != 0 || ih[11] != 0) { *err = "Unsupported PNG"; goto done; }
      switch (p.ct) {
        case 0: p.channels = 1; break;
        case 2: p.channels = 3; break;
        case 3: p.channels = 1; break;
        case 4: p.channels = 2; break;
        case 6: p.channels = 4; break;
        default: *err = "Unsupported PNG"; goto done;
      }
      if (p.depth != 1 && p.depth != 2 && p.depth != 4 && p.depth != 8 && p.depth != 16) {
        *err = "Unsupported PNG depth"; goto done;
      }
      if (p.ct == 3 && p.depth == 16) { *err = "Unsupported PNG"; goto done; }
      if (p.ct != 3 && p.ct != 0 && p.depth < 8) { *err = "Unsupported PNG"; goto done; }
      continue;
    }

    if (!memcmp(type, "PLTE", 4)) {
      uint32_t n = clen / 3;
      if (n > 256) n = 256;
      if (mc_br_read(br, p.pal, (int)(n * 3)) != (int)(n * 3)) { *err = "Bad PNG palette"; goto done; }
      p.pal_n = (int)n;
      mc_br_skip(br, clen - n * 3 + 4);
      continue;
    }

    if (!memcmp(type, "tRNS", 4)) {
      if (p.ct == 3) {                        /* per-palette-entry alpha */
        uint32_t n = clen > 256 ? 256 : clen;
        if (mc_br_read(br, p.pal_a, (int)n) != (int)n) { *err = "Bad PNG"; goto done; }
        mc_br_skip(br, clen - n + 4);
      } else {
        mc_br_skip(br, clen + 4);             /* colour-key: rendered opaque */
      }
      continue;
    }

    if (!memcmp(type, "IDAT", 4)) {
      if (p.w == 0) { *err = "Bad PNG"; goto done; }
      if (!started) {
        /* Row geometry, the two row buffers and the destination. The buffers
         * are sized for a FULL-width row, which is also the largest any Adam7
         * pass can be. */
        p.rw = p.w;
        mc_png_row_geom(&p);
        size_t maxstride = p.stride;
        p.cur  = (uint8_t *)MC_MALLOC(maxstride);
        p.prev = (uint8_t *)MC_MALLOC(maxstride);
        p.rgb  = (uint8_t *)MC_MALLOC((size_t)p.w * 3);
        inbuf  = (uint8_t *)MC_MALLOC(4096);
        if (!p.cur || !p.prev || !p.rgb || !inbuf) { *err = "Out of memory"; goto done; }
        memset(p.prev, 0, maxstride);
        if (!mc_ds_init(&ds, p.w, p.h, tw, th, cover, false)) { *err = "Out of memory"; goto done; }
        ds_ok = true;
        if (p.interlaced) { p.pass = -1; mc_png_next_pass(&p); }
        if (!mc_inf_init(&inf)) { *err = "Out of memory"; goto done; }
        inf_ok = true;
        started = true;
      }

      /* Stream this chunk's payload through inflate. When it runs out we come
       * back around the chunk loop; the next IDAT continues the same stream.
       * HAS_MORE_INPUT stays set throughout: a truncated file then ends as
       * "wants more" (we simply stop) instead of as a hard error. */
      uint32_t left = clen;
      bool stream_done = false;
      while (left) {
        int want = (int)(left < 4096 ? left : 4096);
        int got  = mc_br_read(br, inbuf, want);
        if (got <= 0) { *err = "Truncated PNG"; goto done; }
        left -= (uint32_t)got;
        const uint8_t *ip = inbuf;
        size_t iavail = (size_t)got;
        for (;;) {
          uint8_t *op = NULL;
          size_t   on = 0;
          int st = mc_inf_step(&inf, &ip, &iavail, true, &op, &on);
          if (st < 0) { *err = "Corrupt PNG data"; goto done; }
          if (on) mc_png_consume(&p, &ds, op, on);
          if (st == 0) { stream_done = true; break; }   /* zlib stream complete */
          if (st == 1 && iavail == 0) break;            /* wants the next slice */
          /* Belt and braces: a decompressor that reports "more output" while
           * consuming nothing and producing nothing would spin here. */
          if (on == 0 && iavail == 0) break;
        }
        if (stream_done) break;
      }
      if (!mc_br_skip(br, left + 4)) { *err = "Truncated PNG"; goto done; }  /* rest + CRC */
      if (stream_done || p.done) break;
      continue;
    }

    if (!memcmp(type, "IEND", 4)) break;
    if (!mc_br_skip(br, clen + 4)) { *err = "Truncated PNG"; goto done; }
  }

  if (!started || p.row == 0) { *err = "Empty PNG"; goto done; }
  /* Interlaced rows went straight into the destination, so there is no
   * pending accumulator block to flush — flushing would paint row 0 black. */
  if (p.interlaced) mc_ds_free_scratch(&ds);
  else              mc_ds_finish(&ds);
  ds_ok = false;
  *out = ds.out;
  *ow  = ds.ow;
  *oh  = ds.oh;
  ok   = true;

done:
  if (ds_ok) { mc_ds_free_scratch(&ds); if (ds.out) MC_FREE(ds.out); }
  if (inf_ok) mc_inf_free(&inf);
  if (p.cur)  MC_FREE(p.cur);
  if (p.prev) MC_FREE(p.prev);
  if (p.rgb)  MC_FREE(p.rgb);
  if (inbuf)  MC_FREE(inbuf);
  return ok;
}
#endif  /* MC_HAVE_INFLATE */

/* ============================== BMP =====================================
 * Windows bitmaps as every desktop tool writes them. Rows are read in file
 * order (bottom-up for the usual positive-height file) and the downscaler is
 * told to emit its rows in reverse, so there is no backwards seeking. */
static int mc_bmp_mask_shift(uint32_t m) {
  int s = 0;
  if (!m) return 0;
  while (!(m & 1)) { m >>= 1; s++; }
  return s;
}
static int mc_bmp_mask_bits(uint32_t m) {
  int n = 0;
  while (m) { n += (int)(m & 1); m >>= 1; }
  return n;
}
static uint8_t mc_bmp_scale8(uint32_t v, int bits) {
  if (bits <= 0)  return 0;
  if (bits >= 8)  return (uint8_t)(v >> (bits - 8));
  return (uint8_t)((v * 255u) / ((1u << bits) - 1u));
}

static bool mc_bmp_decode(mc_br_t *br, int tw, int th, bool cover,
                          uint16_t **out, int *ow, int *oh, const char **err) {
  uint8_t fh[14];
  if (mc_br_read(br, fh, 14) != 14 || fh[0] != 'B' || fh[1] != 'M') {
    *err = "Not a BMP";
    return false;
  }
  uint32_t data_off = mc_le32(fh + 10);

  uint8_t dh[4];
  if (mc_br_read(br, dh, 4) != 4) { *err = "Bad BMP"; return false; }
  uint32_t dib = mc_le32(dh);
  if (dib < 12 || dib > 256) { *err = "Bad BMP"; return false; }

  uint8_t hdr[252];
  if (mc_br_read(br, hdr, (int)dib - 4) != (int)dib - 4) { *err = "Bad BMP"; return false; }

  int w, h, bpp;
  uint32_t comp = 0, clrused = 0;
  bool core = (dib == 12);
  if (core) {
    w   = (int)(int16_t)mc_le16(hdr);
    h   = (int)(int16_t)mc_le16(hdr + 2);
    bpp = (int)mc_le16(hdr + 6);
  } else {
    w    = (int)(int32_t)mc_le32(hdr);
    h    = (int)(int32_t)mc_le32(hdr + 4);
    bpp  = (int)mc_le16(hdr + 10);
    comp = mc_le32(hdr + 12);
    if (dib >= 36) clrused = mc_le32(hdr + 28);
  }
  bool flip = (h > 0);                       /* positive height = bottom-up */
  if (h < 0) h = -h;
  if (w <= 0 || h <= 0 || w > 20000 || h > 20000) { *err = "Bad BMP size"; return false; }
  if (comp == 1 || comp == 2) { *err = "RLE BMP unsupported"; return false; }
  if (comp == 4 || comp == 5) { *err = "Unsupported BMP"; return false; }
  if (comp != 0 && comp != 3) { *err = "Unsupported BMP"; return false; }
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
    *err = "Unsupported BMP depth";
    return false;
  }

  /* Channel masks: BITFIELDS puts them right after the header (or inside a
   * V4/V5 header); everything else uses the classic fixed layouts. */
  uint32_t mr = 0, mg = 0, mb = 0;
  if (comp == 3) {
    if (dib >= 56) { mr = mc_le32(hdr + 36); mg = mc_le32(hdr + 40); mb = mc_le32(hdr + 44); }
    else {
      uint8_t m[12];
      if (mc_br_read(br, m, 12) != 12) { *err = "Bad BMP"; return false; }
      mr = mc_le32(m); mg = mc_le32(m + 4); mb = mc_le32(m + 8);
    }
  } else if (bpp == 16) { mr = 0x7C00; mg = 0x03E0; mb = 0x001F; }
  else if (bpp == 32)   { mr = 0x00FF0000; mg = 0x0000FF00; mb = 0x000000FF; }

  uint8_t pal[256 * 3];
  int pal_n = 0;
  if (bpp <= 8) {
    pal_n = (int)(clrused ? clrused : (1u << bpp));
    if (pal_n > 256) pal_n = 256;
    int ent = core ? 3 : 4;
    for (int i = 0; i < pal_n; i++) {
      uint8_t e[4];
      if (mc_br_read(br, e, ent) != ent) { *err = "Bad BMP palette"; return false; }
      pal[i * 3 + 0] = e[2];                 /* stored BGR(A) */
      pal[i * 3 + 1] = e[1];
      pal[i * 3 + 2] = e[0];
    }
  }

  if (data_off && !mc_br_seek(br, data_off)) { *err = "Bad BMP"; return false; }

  const size_t stride = (((size_t)w * (size_t)bpp + 31) / 32) * 4;
  uint8_t *row = (uint8_t *)MC_MALLOC(stride);
  uint8_t *rgb = (uint8_t *)MC_MALLOC((size_t)w * 3);
  mc_ds_t ds;
  bool ok = false;
  if (!row || !rgb) { *err = "Out of memory"; goto done; }
  if (!mc_ds_init(&ds, w, h, tw, th, cover, flip)) { *err = "Out of memory"; goto done; }

  {
    const int rs = mc_bmp_mask_shift(mr), rb = mc_bmp_mask_bits(mr);
    const int gs = mc_bmp_mask_shift(mg), gb = mc_bmp_mask_bits(mg);
    const int bs = mc_bmp_mask_shift(mb), bb = mc_bmp_mask_bits(mb);
    for (int y = 0; y < h; y++) {
      if (mc_br_read(br, row, (int)stride) != (int)stride) break;   /* short file: keep what we have */
      for (int x = 0; x < w; x++) {
        uint8_t r, g, b;
        if (bpp <= 8) {
          uint32_t idx;
          if      (bpp == 8) idx = row[x];
          else if (bpp == 4) idx = (row[x >> 1] >> (((x & 1) ^ 1) * 4)) & 0x0F;
          else               idx = (row[x >> 3] >> (7 - (x & 7))) & 0x01;
          if ((int)idx >= pal_n) idx = 0;
          r = pal[idx * 3 + 0]; g = pal[idx * 3 + 1]; b = pal[idx * 3 + 2];
        } else if (bpp == 24) {
          r = row[x * 3 + 2]; g = row[x * 3 + 1]; b = row[x * 3 + 0];
        } else {
          uint32_t v = (bpp == 16) ? mc_le16(row + x * 2) : mc_le32(row + x * 4);
          r = mc_bmp_scale8((v & mr) >> rs, rb);
          g = mc_bmp_scale8((v & mg) >> gs, gb);
          b = mc_bmp_scale8((v & mb) >> bs, bb);
        }
        rgb[x * 3 + 0] = r; rgb[x * 3 + 1] = g; rgb[x * 3 + 2] = b;
      }
      mc_ds_row(&ds, rgb);
    }
    mc_ds_finish(&ds);
    *out = ds.out;
    *ow  = ds.ow;
    *oh  = ds.oh;
    ok   = true;
  }

done:
  if (row) MC_FREE(row);
  if (rgb) MC_FREE(rgb);
  return ok;
}

/* ============================== GIF =====================================
 * A full animation decoder, because the Gallery's video player drives GIFs
 * frame by frame through exactly this object (media_video.h). A still GIF is
 * just "open, decode frame 0, stop".
 *
 * The canvas is persistent RGB888 at logical-screen size: GIF frames are
 * patches applied to what came before, with a disposal method saying how to
 * undo them, so there is no way around keeping one. */
typedef struct {
  mc_br_t  br;
  int      lw, lh;                 /* logical screen                        */
  uint8_t  gct[256 * 3];
  int      gct_n;
  int      bg;
  uint8_t  lct[256 * 3];
  int      lct_n;
  uint8_t *canvas;                 /* lw * lh * 3                           */
  uint8_t *backup;                 /* lazy: only for disposal 3             */
  /* graphic control extension for the frame about to be drawn */
  int      delay_cs, transp, disposal;
  /* what to undo before the NEXT frame, and the region it covers */
  int      pend_disposal, px, py, pw, ph;
  int      frame;
  /* LZW scratch, allocated once */
  uint16_t *prefix;
  uint8_t  *suffix, *stack, *linebuf;
} mc_gif_t;

static void mc_gif_close(mc_gif_t *g) {
  if (g->canvas)  { MC_FREE(g->canvas);  g->canvas  = NULL; }
  if (g->backup)  { MC_FREE(g->backup);  g->backup  = NULL; }
  if (g->prefix)  { MC_FREE(g->prefix);  g->prefix  = NULL; }
  if (g->suffix)  { MC_FREE(g->suffix);  g->suffix  = NULL; }
  if (g->stack)   { MC_FREE(g->stack);   g->stack   = NULL; }
  if (g->linebuf) { MC_FREE(g->linebuf); g->linebuf = NULL; }
}

/* Bit reader over GIF's sub-block chain (length byte, that many bytes, ...,
 * terminating zero). LZW codes are packed LSB-first and straddle both byte
 * and sub-block boundaries. */
typedef struct {
  mc_br_t *br;
  int      rem;        /* bytes left in the current sub-block */
  uint32_t bits;
  int      nbits;
  bool     end;
} mc_lzw_bits_t;

static int mc_lzw_get(mc_lzw_bits_t *s, int n) {
  while (s->nbits < n) {
    if (s->rem == 0) {
      int l = mc_br_u8(s->br);
      if (l <= 0) { s->end = true; return -1; }
      s->rem = l;
    }
    int c = mc_br_u8(s->br);
    if (c < 0) { s->end = true; return -1; }
    s->rem--;
    s->bits |= (uint32_t)c << s->nbits;
    s->nbits += 8;
  }
  int v = (int)(s->bits & ((1u << n) - 1u));
  s->bits >>= n;
  s->nbits -= n;
  return v;
}

/* Consume whatever is left of the sub-block chain so the reader sits on the
 * next block header. */
static void mc_gif_drain(mc_br_t *br) {
  for (;;) {
    int l = mc_br_u8(br);
    if (l <= 0) return;
    if (!mc_br_skip(br, (uint32_t)l)) return;
  }
}

static bool mc_gif_open(mc_gif_t *g, mc_src_t *src, const char **err) {
  memset(g, 0, sizeof(*g));
  mc_br_init(&g->br, src);
  g->transp   = -1;
  g->delay_cs = 10;

  uint8_t h[13];
  if (mc_br_read(&g->br, h, 13) != 13 || memcmp(h, "GIF8", 4)) {
    *err = "Not a GIF";
    return false;
  }
  g->lw = mc_le16(h + 6);
  g->lh = mc_le16(h + 8);
  if (g->lw <= 0 || g->lh <= 0 || g->lw > 8000 || g->lh > 8000) {
    *err = "Bad GIF size";
    return false;
  }
  if (h[10] & 0x80) {
    g->gct_n = 2 << (h[10] & 7);
    if (mc_br_read(&g->br, g->gct, g->gct_n * 3) != g->gct_n * 3) {
      *err = "Bad GIF";
      return false;
    }
  }
  g->bg = h[11];

  g->canvas  = (uint8_t *)MC_MALLOC((size_t)g->lw * g->lh * 3);
  g->prefix  = (uint16_t *)MC_MALLOC(4096 * sizeof(uint16_t));
  g->suffix  = (uint8_t *)MC_MALLOC(4096);
  g->stack   = (uint8_t *)MC_MALLOC(4096);
  g->linebuf = (uint8_t *)MC_MALLOC((size_t)g->lw * 3);
  if (!g->canvas || !g->prefix || !g->suffix || !g->stack || !g->linebuf) {
    mc_gif_close(g);
    *err = "Out of memory";
    return false;
  }
  /* GIF's "background" is transparent in practice (browsers show the page
   * through it); black is this UI's page. */
  memset(g->canvas, 0, (size_t)g->lw * g->lh * 3);
  return true;
}

/* Undo the previous frame per its disposal method, before drawing the next. */
static void mc_gif_dispose(mc_gif_t *g) {
  if (g->pend_disposal == 2) {
    for (int y = g->py; y < g->py + g->ph && y < g->lh; y++) {
      uint8_t *r = g->canvas + ((size_t)y * g->lw + g->px) * 3;
      int n = g->pw;
      if (g->px + n > g->lw) n = g->lw - g->px;
      if (n > 0) memset(r, 0, (size_t)n * 3);
    }
  } else if (g->pend_disposal == 3 && g->backup) {
    memcpy(g->canvas, g->backup, (size_t)g->lw * g->lh * 3);
  }
  g->pend_disposal = 0;
}

/* Decode the next image into the canvas.
 * Returns 1 = a frame was drawn, 0 = end of stream, -1 = corrupt. */
static int mc_gif_next(mc_gif_t *g) {
  for (;;) {
    int blk = mc_br_u8(&g->br);
    if (blk < 0 || blk == 0x3B) return 0;             /* EOF or trailer */

    if (blk == 0x21) {                                /* extension */
      int label = mc_br_u8(&g->br);
      if (label < 0) return 0;
      if (label == 0xF9) {                            /* graphic control */
        int len = mc_br_u8(&g->br);
        uint8_t gce[4];
        if (len < 4 || mc_br_read(&g->br, gce, 4) != 4) return -1;
        mc_br_skip(&g->br, (uint32_t)(len - 4));
        g->disposal = (gce[0] >> 2) & 7;
        g->delay_cs = mc_le16(gce + 1);
        g->transp   = (gce[0] & 1) ? gce[3] : -1;
        mc_gif_drain(&g->br);
      } else {
        mc_gif_drain(&g->br);
      }
      continue;
    }

    if (blk != 0x2C) continue;                        /* unknown: resync */

    uint8_t id[9];
    if (mc_br_read(&g->br, id, 9) != 9) return -1;
    int fx = mc_le16(id), fy = mc_le16(id + 2);
    int fw = mc_le16(id + 4), fh = mc_le16(id + 6);
    bool interlace = (id[8] & 0x40) != 0;
    g->lct_n = 0;
    if (id[8] & 0x80) {
      g->lct_n = 2 << (id[8] & 7);
      if (mc_br_read(&g->br, g->lct, g->lct_n * 3) != g->lct_n * 3) return -1;
    }
    const uint8_t *pal = g->lct_n ? g->lct : g->gct;
    const int pal_n    = g->lct_n ? g->lct_n : g->gct_n;
    if (pal_n <= 0) return -1;

    mc_gif_dispose(g);
    if (g->disposal == 3) {                           /* snapshot for restore */
      if (!g->backup) g->backup = (uint8_t *)MC_MALLOC((size_t)g->lw * g->lh * 3);
      if (g->backup) memcpy(g->backup, g->canvas, (size_t)g->lw * g->lh * 3);
    }

    int mcs = mc_br_u8(&g->br);
    if (mcs < 2 || mcs > 11) return -1;

    /* ---- LZW ----------------------------------------------------------
     * Textbook GIF variant: codes below `clear` are single pixels, `clear`
     * resets the dictionary, `end` terminates, everything above is a
     * prefix+suffix pair built as we go. The one subtlety is the "code we
     * are about to define is the code we just read" case (KwKwK), which
     * resolves to prev's string plus prev's first pixel. */
    const int clear = 1 << mcs, end = clear + 1;
    int codesize = mcs + 1, next = end + 1, prev = -1, first = 0;
    mc_lzw_bits_t bs = { &g->br, 0, 0, 0, false };
    int x = 0, y = 0, pass = 0;
    const int irow[4] = { 0, 4, 2, 1 }, iinc[4] = { 8, 8, 4, 2 };

    for (;;) {
      int code = mc_lzw_get(&bs, codesize);
      if (code < 0 || code == end) break;
      if (code == clear) {
        codesize = mcs + 1;
        next = end + 1;
        prev = -1;
        continue;
      }
      if (prev >= 0 && code > next) break;              /* corrupt stream */

      /* Expand to a pixel run, pushed onto the stack in reverse. */
      int sp = 0, c = code;
      if (prev >= 0 && code == next) { g->stack[sp++] = (uint8_t)first; c = prev; }
      while (c > end && sp < 4095) { g->stack[sp++] = g->suffix[c]; c = g->prefix[c]; }
      if (c > end) break;                               /* runaway chain */
      first = (c < clear) ? c : 0;
      if (sp < 4096) g->stack[sp++] = (uint8_t)first;

      /* Emit. Pixels land in the frame rectangle, clipped to the canvas;
       * once the rectangle is full we keep decoding (to stay byte-aligned
       * with the sub-block chain) but stop drawing. */
      for (int i = sp - 1; i >= 0; i--) {
        if (y >= fh) break;
        uint8_t idx = g->stack[i];
        int cx = fx + x, cy = fy + y;
        if ((int)idx != g->transp && cx >= 0 && cx < g->lw && cy >= 0 && cy < g->lh) {
          const uint8_t *p = pal + (((int)idx < pal_n) ? idx : 0) * 3;
          uint8_t *o = g->canvas + ((size_t)cy * g->lw + cx) * 3;
          o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        }
        if (++x >= fw) {
          x = 0;
          if (interlace) {
            y += iinc[pass];
            while (y >= fh && pass < 3) { pass++; y = irow[pass]; }
          } else {
            y++;
          }
        }
      }

      if (prev >= 0 && next < 4096) {
        g->prefix[next] = (uint16_t)prev;
        g->suffix[next] = (uint8_t)first;      /* first pixel of THIS string */
        next++;
        if (next == (1 << codesize) && codesize < 12) codesize++;
      }
      prev = code;
    }
    /* Consume the rest of the sub-block chain so the reader lands on the next
     * block header — unless the chain already ran out under us (truncated
     * file), in which case draining would eat the byte we need. */
    if (!bs.end) mc_gif_drain(&g->br);

    g->pend_disposal = g->disposal;
    g->px = fx; g->py = fy; g->pw = fw; g->ph = fh;
    g->frame++;
    /* A graphic control extension applies to the ONE image that follows it,
     * so the per-frame state goes back to the spec defaults here. */
    g->disposal = 0;
    g->transp   = -1;
    return 1;
  }
}

/* Cheap pre-pass: how many frames, and how long the animation runs. Used to
 * decide "is this GIF a video?" and to caption the player. */
static bool mc_gif_scan(mc_src_t *src, int *frames, uint32_t *total_ms,
                        int *w, int *h) {
  mc_br_t br;
  mc_br_init(&br, src);
  uint8_t hd[13];
  if (mc_br_read(&br, hd, 13) != 13 || memcmp(hd, "GIF8", 4)) return false;
  if (w) *w = mc_le16(hd + 6);
  if (h) *h = mc_le16(hd + 8);
  if (hd[10] & 0x80) {
    int n = 2 << (hd[10] & 7);
    if (!mc_br_skip(&br, (uint32_t)n * 3)) return false;
  }
  int n = 0;
  uint32_t ms = 0, delay = 10;
  for (;;) {
    int blk = mc_br_u8(&br);
    if (blk < 0 || blk == 0x3B) break;
    if (blk == 0x21) {
      int label = mc_br_u8(&br);
      if (label < 0) break;
      if (label == 0xF9) {
        int len = mc_br_u8(&br);
        uint8_t gce[4];
        if (len < 4 || mc_br_read(&br, gce, 4) != 4) break;
        mc_br_skip(&br, (uint32_t)(len - 4));
        delay = mc_le16(gce + 1);
        mc_gif_drain(&br);
      } else {
        mc_gif_drain(&br);
      }
      continue;
    }
    if (blk != 0x2C) continue;
    uint8_t id[9];
    if (mc_br_read(&br, id, 9) != 9) break;
    if (id[8] & 0x80) {
      int c = 2 << (id[8] & 7);
      if (!mc_br_skip(&br, (uint32_t)c * 3)) break;
    }
    if (mc_br_u8(&br) < 0) break;                 /* min code size */
    mc_gif_drain(&br);
    n++;
    ms += (delay ? delay : 10) * 10;
    if (n > 100000) break;
  }
  if (frames)   *frames = n;
  if (total_ms) *total_ms = ms;
  return n > 0;
}

/* Canvas -> a fresh downscaled RGB565 buffer. */
static bool mc_gif_canvas_out(mc_gif_t *g, int tw, int th, bool cover,
                              uint16_t **out, int *ow, int *oh) {
  mc_ds_t ds;
  if (!mc_ds_init(&ds, g->lw, g->lh, tw, th, cover, false)) return false;
  for (int y = 0; y < g->lh; y++) mc_ds_row(&ds, g->canvas + (size_t)y * g->lw * 3);
  mc_ds_finish(&ds);
  *out = ds.out; *ow = ds.ow; *oh = ds.oh;
  return true;
}

/* Canvas -> an EXISTING RGB565 buffer at fixed dimensions (the video player
 * reuses one buffer for every frame, so it cannot take a new allocation each
 * time). Returns false if the buffer is not the size the canvas scales to. */
static bool mc_gif_canvas_into(mc_gif_t *g, uint16_t *dst, int dw, int dh,
                               int step) {
  if (step < 1) step = 1;
  for (int oy = 0; oy < dh; oy++) {
    for (int ox = 0; ox < dw; ox++) {
      uint32_t r = 0, gg = 0, b = 0, n = 0;
      for (int sy = oy * step; sy < (oy + 1) * step && sy < g->lh; sy++) {
        const uint8_t *srow = g->canvas + (size_t)sy * g->lw * 3;
        for (int sx = ox * step; sx < (ox + 1) * step && sx < g->lw; sx++) {
          r += srow[sx * 3 + 0]; gg += srow[sx * 3 + 1]; b += srow[sx * 3 + 2];
          n++;
        }
      }
      if (!n) n = 1;
      r /= n; gg /= n; b /= n;
      dst[(size_t)oy * dw + ox] =
          (uint16_t)(((r & 0xF8) << 8) | ((gg & 0xFC) << 3) | (b >> 3));
    }
  }
  return true;
}

/* ====================== filesystem bindings (ESP32) =====================
 * Everything above is host-testable C. Below is the Arduino/LVGL glue the
 * firmware actually calls. */
#ifndef MC_HOST_TEST

#include <FS.h>
#include "jpeg_decoder.h"

/* A byte source over an open Arduino File. */
typedef struct {
  mc_src_t base;
  File     f;
} mc_file_src_t;

static int mc_file_read(mc_src_t *s, uint8_t *dst, int n) {
  mc_file_src_t *fs = (mc_file_src_t *)s;
  return (int)fs->f.read(dst, (size_t)n);
}
static bool mc_file_seek(mc_src_t *s, uint32_t pos) {
  mc_file_src_t *fs = (mc_file_src_t *)s;
  return fs->f.seek(pos);
}
static bool mc_file_open(mc_file_src_t *s, fs::FS &fsys, const char *path) {
  s->f = fsys.open(path, FILE_READ);
  if (!s->f) return false;
  s->base.read = mc_file_read;
  s->base.seek = mc_file_seek;
  s->base.size = (uint32_t)s->f.size();
  s->base.ctx  = nullptr;
  return true;
}
static void mc_file_close(mc_file_src_t *s) { if (s->f) s->f.close(); }

/* A byte source over a memory blob (AVI frames arrive this way). */
typedef struct {
  mc_src_t       base;
  const uint8_t *p;
  uint32_t       len, pos;
} mc_mem_src_t;

static int mc_mem_read(mc_src_t *s, uint8_t *dst, int n) {
  mc_mem_src_t *m = (mc_mem_src_t *)s;
  uint32_t left = m->len - m->pos;
  if ((uint32_t)n > left) n = (int)left;
  if (n > 0) { memcpy(dst, m->p + m->pos, (size_t)n); m->pos += (uint32_t)n; }
  return n;
}
static bool mc_mem_seek(mc_src_t *s, uint32_t pos) {
  mc_mem_src_t *m = (mc_mem_src_t *)s;
  if (pos > m->len) return false;
  m->pos = pos;
  return true;
}
static void mc_mem_open(mc_mem_src_t *m, const uint8_t *p, uint32_t len) {
  m->base.read = mc_mem_read;
  m->base.seek = mc_mem_seek;
  m->base.size = len;
  m->base.ctx  = nullptr;
  m->p = p; m->len = len; m->pos = 0;
}

/* ---- JPEG (esp_jpeg / tjpgd) ---------------------------------------------
 * Used directly rather than through jpg2rgb565(): the wrapper does not
 * allocate, does not report dimensions and turns the decoder's bounds check
 * off. esp_jpeg emits little-endian RGB565 with swap off — LVGL's own order.
 * The 1/2, 1/4, 1/8 scales are applied DURING decode, so a 12 MP photo never
 * needs a 12 MP buffer. */
static bool mc_jpeg_decode_mem(uint8_t *jpg, size_t len, int tw, int th, bool cover,
                               uint16_t **out, int *ow, int *oh, const char **err) {
  esp_jpeg_image_cfg_t cfg = {};
  cfg.indata      = jpg;
  cfg.indata_size = len;
  cfg.out_format  = JPEG_IMAGE_FORMAT_RGB565;
  cfg.out_scale   = JPEG_IMAGE_SCALE_0;
  cfg.flags.swap_color_bytes = 0;

  esp_jpeg_image_output_t info = {};
  if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0) {
    *err = "Unsupported JPEG";           /* progressive, CMYK, 12-bit, ... */
    return false;
  }

  /* Pick the largest reduction that still satisfies the caller's target. */
  int want = 0;                          /* 0,1,2,3 -> 1, 1/2, 1/4, 1/8 */
  if (tw < 1) tw = 1;
  if (th < 1) th = 1;
  for (int s = 1; s <= 3; s++) {
    int w = info.width >> s, h = info.height >> s;
    bool fits = cover ? (w >= tw && h >= th) : (w >= tw || h >= th);
    if (fits) want = s; else break;
  }
  cfg.out_scale = (esp_jpeg_image_scale_t)want;

  esp_jpeg_image_output_t sz = {};
  if (esp_jpeg_get_image_info(&cfg, &sz) != ESP_OK || sz.output_len == 0) {
    *err = "Unsupported JPEG";
    return false;
  }
  uint16_t *rgb = (uint16_t *)MC_MALLOC(sz.output_len);
  if (!rgb) { *err = "Out of memory"; return false; }

  cfg.outbuf      = (uint8_t *)rgb;
  cfg.outbuf_size = sz.output_len;
  esp_jpeg_image_output_t done = {};
  if (esp_jpeg_decode(&cfg, &done) != ESP_OK) {
    MC_FREE(rgb);
    *err = "Corrupt JPEG";
    return false;
  }
  *out = rgb; *ow = done.width; *oh = done.height;
  return true;
}

/* Whole-file JPEG: tjpgd has no streaming entry point in this component, so
 * the file is read into PSRAM first. Anything absurd is refused rather than
 * thrashing the allocator. */
#define MC_JPEG_MAX_FILE (6u * 1024u * 1024u)

static bool mc_jpeg_decode_file(mc_file_src_t *s, int tw, int th, bool cover,
                                uint16_t **out, int *ow, int *oh, const char **err) {
  size_t len = (size_t)s->f.size();
  if (len == 0) { *err = "Empty file"; return false; }
  if (len > MC_JPEG_MAX_FILE) { *err = "JPEG too large"; return false; }
  uint8_t *jpg = (uint8_t *)MC_MALLOC(len);
  if (!jpg) { *err = "Out of memory"; return false; }
  s->f.seek(0);
  size_t got = s->f.read(jpg, len);
  if (got != len) { MC_FREE(jpg); *err = "Read error"; return false; }
  bool ok = mc_jpeg_decode_mem(jpg, len, tw, th, cover, out, ow, oh, err);
  MC_FREE(jpg);
  return ok;
}

/* ---- the one entry point the Gallery uses --------------------------------
 * Decode any supported still image to RGB565 in PSRAM. `cover` picks the
 * fit rule (see mc_ds_init). On failure *err is a short, showable reason. */
static bool mc_decode_still(fs::FS &fsys, const char *path, int tw, int th,
                            bool cover, uint16_t **out, int *ow, int *oh,
                            const char **err) {
  *out = nullptr;
  *err = "Can't open";
  mc_file_src_t s;
  if (!mc_file_open(&s, fsys, path)) return false;

  uint8_t magic[16];
  int n = (int)s.f.read(magic, sizeof(magic));
  s.f.seek(0);
  int fmt = mc_sniff(magic, n);

  bool ok = false;
  switch (fmt) {
    case MC_FMT_JPEG:
      ok = mc_jpeg_decode_file(&s, tw, th, cover, out, ow, oh, err);
      break;
    case MC_FMT_PNG: {
      mc_br_t br;
      mc_br_init(&br, &s.base);
      ok = mc_png_decode(&br, tw, th, cover, out, ow, oh, err);
      break;
    }
    case MC_FMT_BMP: {
      mc_br_t br;
      mc_br_init(&br, &s.base);
      ok = mc_bmp_decode(&br, tw, th, cover, out, ow, oh, err);
      break;
    }
    case MC_FMT_GIF: {
      mc_gif_t g;
      if (mc_gif_open(&g, &s.base, err)) {
        if (mc_gif_next(&g) != 1)                          *err = "Empty GIF";
        else if (!mc_gif_canvas_out(&g, tw, th, cover, out, ow, oh))
                                                           *err = "Out of memory";
        else                                               ok = true;
        mc_gif_close(&g);
      }
      break;
    }
    case MC_FMT_WEBP: *err = "WebP not supported"; break;
    case MC_FMT_TIFF: *err = "TIFF not supported"; break;
    case MC_FMT_MP4:  *err = "MP4 not supported";  break;
    default:          *err = "Unknown format";     break;
  }
  mc_file_close(&s);
  return ok;
}

/* Sniff a file's format without decoding it (used for listing decisions). */
static int mc_sniff_file(fs::FS &fsys, const char *path) {
  File f = fsys.open(path, FILE_READ);
  if (!f) return MC_FMT_UNKNOWN;
  uint8_t magic[16];
  int n = (int)f.read(magic, sizeof(magic));
  f.close();
  return mc_sniff(magic, n);
}

#endif  /* !MC_HOST_TEST */
