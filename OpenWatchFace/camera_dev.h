/* ============================================================================
 *  camera_dev.h — DVP camera sensor (esp32-camera) wrapper
 *
 *  Board-neutral surface over Espressif's esp32-camera driver for boards with
 *  BOARD_HAS_CAMERA (the ESP32-S3-Touch-LCD-2's 24-pin header, OV5640 fitted).
 *  On a board WITHOUT a camera the whole file compiles to nothing and the Camera
 *  app is excluded with it — same "feature absent = no code, no call-site changes"
 *  model as the rest of the BOARD_HAS_* flags.
 *
 *    cam_begin()        - power up + configure the sensor (idempotent, lazy).
 *    cam_end()          - release the driver + its PSRAM framebuffers.
 *    cam_preview_get()  - grab one RGB565 frame for the live preview.
 *    cam_preview_done() - hand that frame back (MUST pair with every get).
 *    cam_capture_jpeg() - take a still: reconfigures to JPEG, grabs, restores.
 *    cam_available()    - is the sensor actually there and initialised?
 *
 *  WHY TWO PIXEL FORMATS. The driver's pixel format is fixed at init, so a single
 *  config cannot serve both jobs well:
 *    - PREVIEW wants RGB565 at a small frame size, because LVGL can blit those
 *      bytes straight to the screen with no decode step (exactly what the vendor
 *      demo does). Decoding JPEG every frame would be far too slow for a preview.
 *    - A SAVED PHOTO wants JPEG: a full-resolution RGB565 still would be many MB
 *      and would fill the card in a handful of shots, whereas the OV5640's
 *      hardware JPEG encoder gives a few hundred KB at much higher resolution.
 *  So cam_capture_jpeg() DEINITS and re-inits the driver in JPEG mode for the
 *  shot, then the caller returns to preview mode. That re-init costs ~300 ms,
 *  which is a perfectly acceptable shutter delay and keeps the preview fast.
 *
 *  PSRAM IS REQUIRED. Framebuffers live in PSRAM (CAMERA_FB_IN_PSRAM); a
 *  full-resolution JPEG framebuffer cannot fit in internal SRAM. The static
 *  assert below enforces that a board declaring a camera also declares PSRAM.
 *
 *  THREADING: everything here runs on the loop/LVGL thread. The sensor's SCCB bus
 *  is its OWN 2-wire bus (CAM_PIN_SIOD/SIOC), NOT the shared touch/IMU I2C — so
 *  none of this takes i2c_lock(), and it must not, or it would serialise against
 *  an unrelated bus.
 * ========================================================================== */
#pragma once
#if BOARD_HAS_CAMERA

#include "esp_camera.h"
#include "esp_heap_caps.h"   // heap_caps_malloc — the still is copied into PSRAM here
#include "driver/gpio.h"     // gpio_set_* — parking the sensor's PWDN pin in cam_end
#include "img_converters.h"  // fmt2jpg — software JPEG encode for the instant-shutter path

#if !BOARD_HAS_PSRAM
#error "board config: BOARD_HAS_CAMERA requires BOARD_HAS_PSRAM (framebuffers live in PSRAM)"
#endif

/* Legacy fixed-preview constants. The preview size is now DERIVED from the chosen
 * aspect (cam_preview_framesize()/cam_preview_w()/_h() below); these remain only as
 * the 1:1 fallback values and must not be used to size the live feed. */
#define CAM_PREVIEW_W        240
#define CAM_PREVIEW_H        320

/* Still-photo size. FRAMESIZE_UXGA (1600x1200) is the largest the OV5640 does over
 * DVP that the S3 reliably sustains; JPEG quality 10 (lower = better on this
 * sensor family) lands around 200-400 KB per shot. */
#define CAM_STILL_FRAMESIZE  FRAMESIZE_UXGA
#define CAM_STILL_QUALITY    10

/* ---- selectable still resolutions, grouped by ASPECT RATIO ----------------
 * The UI offers an aspect first, then only the resolutions belonging to it, so a
 * 4:3 choice can never silently hand back a 16:9 frame. Every entry below is a
 * size the    genuinely supports (the framesize enum's own list); the sensor
 * array is 4:3, so the 16:9 and 1:1 entries are sensor-side crops of it.
 *
 * Ordered LARGEST FIRST within each group: the common intent is "best quality",
 * so the default lands at the top of the list. */
typedef struct { const char *name; framesize_t fs; uint16_t w, h; } cam_res_t;

/* 4:3 — the sensor's native shape, so these use the full width of the array. */
static const cam_res_t CAM_RES_4_3[] = {
  { "2592x1944 (5MP)", FRAMESIZE_QSXGA, 2592, 1944 },
  { "2048x1536 (3MP)", FRAMESIZE_QXGA,  2048, 1536 },
  { "1600x1200 (2MP)", FRAMESIZE_UXGA,  1600, 1200 },
  { "1024x768",        FRAMESIZE_XGA,   1024,  768 },
  { "800x600",         FRAMESIZE_SVGA,   800,  600 },
  { "640x480",         FRAMESIZE_VGA,    640,  480 },
  /* NATIVE-CLASS: the preview's own frame size. A shot displays pixel-perfect
   * on the 240-wide panel, costs ~10-20 KB of card, and — because it equals the
   * 4:3 preview frame — always captures via the instant-shutter fast path in
   * cam_capture_jpeg: no driver re-init, no shutter delay. (A true-portrait
   * 240x320 variant via sensor windowing was tried and abandoned: the custom
   * readout ran ~5x slower than the native mode and glitched — see
   * cam_preview_w.) */
  { "320x240 (native)", FRAMESIZE_QVGA,  320,  240 },
};
/* 16:9 — a letterboxed crop of the 4:3 array. */
static const cam_res_t CAM_RES_16_9[] = {
  { "1920x1080 (FHD)", FRAMESIZE_FHD,   1920, 1080 },
  { "1280x720 (HD)",   FRAMESIZE_HD,    1280,  720 },
};
/* 1:1 — square, for a watch-face-shaped shot. Only the two NATIVE square
 * framesizes: larger squares were tried as center-crops of 4:3 stills and
 * produced no better output, so they're gone.
 *
 * 240x240 LEADS because it is the DEFAULT (index 0 is also where
 * cam_aspect_set's reset lands): it equals the 1:1 preview frame, so the
 * shutter is INSTANT (fmt2jpg of the live frame — no driver re-init), and its
 * quality-90 software encode actually produces files a few KB LARGER than the
 * 320x320 sensor-encoded JPEGs, whose hardware encoder compresses much harder.
 * 320x320 buys pixels at the cost of the ~600 ms re-init shutter. */
static const cam_res_t CAM_RES_1_1[] = {
  { "240x240",         FRAMESIZE_240X240, 240, 240 },
  { "320x320",         FRAMESIZE_320X320, 320, 320 },
};

typedef struct { const char *name; const cam_res_t *list; uint8_t count; } cam_aspect_t;
static const cam_aspect_t CAM_ASPECTS[] = {
  { "4:3",  CAM_RES_4_3,  (uint8_t)(sizeof(CAM_RES_4_3)  / sizeof(CAM_RES_4_3[0]))  },
  { "16:9", CAM_RES_16_9, (uint8_t)(sizeof(CAM_RES_16_9) / sizeof(CAM_RES_16_9[0])) },
  { "1:1",  CAM_RES_1_1,  (uint8_t)(sizeof(CAM_RES_1_1)  / sizeof(CAM_RES_1_1[0]))  },
};
#define CAM_ASPECT_COUNT ((int)(sizeof(CAM_ASPECTS) / sizeof(CAM_ASPECTS[0])))

/* Current selection: default 1:1 @ 240x240. The 1:1 preview (240x240) sits in
 * the sensor's FAST clock bracket — measured 19+ fps against 4:3's boosted ~15
 * (see cam_boost_preview_clock) — and 240x240 stills equal that preview frame,
 * so the DEFAULT shutter is instant (see the CAM_RES_1_1 comment). */
static int s_cam_aspect_idx = 2;
static int s_cam_res_idx    = 0;

/* ---- PREVIEW frame size, derived from the chosen aspect -------------------
 * The preview must SHOW THE SHAPE YOU WILL GET, or the aspect setting is a lie:
 * framing a 16:9 shot through a square viewfinder means the saved photo contains
 * things you never saw (and vice versa for the crop). So the preview framesize
 * follows the selected aspect rather than being fixed at 240x240.
 *
 * WHY 16:9 IS NOT A NATIVE PREVIEW SIZE. The framesize enum's only true 16:9
 * entries are 1280x720 and larger. An RGB565 preview at 1280x720 is 1.8 MB PER
 * FRAME to copy and byte-swap every tick — far too heavy for a ~10 fps live view.
 * So 16:9 previews at the cheap 4:3 size and the DISPLAY crops it to 16:9 (see
 * cam_preview_crop_h() and its use in cam_preview_tick). The user still frames the
 * correct shape; only the intermediate buffer is 4:3. The SAVED photo is a real
 * 16:9 frame either way, because the still is captured at the chosen framesize.
 *
 * RGB565 preview frames are uncompressed, so the size is also the PSRAM cost per
 * frame (w*h*2): 320x240 = 150 KB, 240x240 = 115 KB. Both are well inside budget. */
static framesize_t cam_preview_framesize(void) {
  switch (s_cam_aspect_idx) {
    case 2:  return FRAMESIZE_240X240;   /* 1:1 — native square */
    case 1:                              /* 16:9 — preview 4:3, cropped on display */
    default: return FRAMESIZE_QVGA;      /* 4:3 — 320x240 */
  }
}
/* The nominal preview dimensions for the current aspect — always the DRIVER'S
 * OWN mode shapes. A portrait 240x320 sensor output via set_res_raw was tried
 * (it would have blitted full-bleed 1:1): the byte count matches QVGA so the
 * capture layer accepted it, but the sensor's custom-window readout ran ~5x
 * SLOWER than the driver's native QVGA timing (fb_get 25 ms -> 119 ms, measured)
 * and produced intermittently truncated frames (cam_hal FB-SIZE errors). Native
 * driver modes are the only readouts with proven-good timing, so the preview
 * stays in them and the DISPLAY achieves 1:1 by center-cropping the landscape
 * frame instead (see cam_preview_tick) — zero transform, full sensor speed. */
static int cam_preview_w(void) { return (s_cam_aspect_idx == 2) ? 240 : 320; }
static int cam_preview_h(void) { return 240; }

/* Fraction of the preview frame's HEIGHT to actually show, as a percentage.
 * 100 for 4:3 and 1:1 (the frame already is the target shape); for 16:9 the 4:3
 * frame is letterbox-cropped to 9/16 of its width — i.e. 3/4 * 9/16 * ... which
 * works out to 75% of the height of a 4:3 frame. Applied by the display, so the
 * viewfinder shows exactly the shape the photo will have. */
static int cam_preview_crop_pct(void) {
  if (s_cam_aspect_idx != 1) return 100;          /* 4:3 and 1:1 need no crop */
  /* A 4:3 frame (w = 4h/3) shown as 16:9 keeps height = w * 9/16 = (4h/3)*9/16
   * = 0.75h -> 75% of the frame height. */
  return 75;
}

static const cam_aspect_t *cam_aspect(void) { return &CAM_ASPECTS[s_cam_aspect_idx]; }
static const cam_res_t *cam_res(void) {
  const cam_aspect_t *a = cam_aspect();
  int i = s_cam_res_idx;
  if (i >= a->count) i = 0;
  return &a->list[i];
}
/* Selecting an aspect resets the resolution to that group's largest, because the
 * old index refers to a different list and would otherwise pick an unrelated size. */
static void cam_aspect_set(int idx) {
  if (idx < 0 || idx >= CAM_ASPECT_COUNT) return;
  s_cam_aspect_idx = idx;
  s_cam_res_idx    = 0;
}
static void cam_res_set(int idx) {
  if (idx < 0 || idx >= cam_aspect()->count) return;
  s_cam_res_idx = idx;
}

static bool s_cam_ready = false;      // driver initialised right now
static bool s_cam_absent = false;     // probe failed once -> don't retry every open
static pixformat_t s_cam_fmt = PIXFORMAT_RGB565;   // format the driver is CURRENTLY in

/* ---- live image controls (survive the preview<->still re-init) -------------
 * The esp32-camera driver resets the sensor whenever the pixel format changes, so
 * every one of these must be re-applied after each init or a photo would come out
 * with different orientation/brightness than the preview that framed it. They are
 * plain module state, re-pushed by cam_apply_controls().
 *
 * ORIENTATION. vflip/hmirror are SENSOR-side (the chip flips its own readout),
 * which is free — no CPU, no extra buffer — unlike rotating the frame in
 * software. If a future board mounts its module differently, change these
 * defaults (or call cam_flip_set) rather than touching the app.
 *
 * NO FLIP by default. Earlier revisions forced vflip=1 on the theory that the
 * module sat inverted on the header (Waveshare's factory demo does the same for
 * ITS display init) — but with that flip genuinely reaching the sensor (the
 * forced double-write in cam_apply_controls guarantees the register write), the
 * live view came out UPSIDE DOWN. On this module, as this firmware orients the
 * panel, the raw readout is already right-side-up, so vflip=1 was the very
 * inversion it claimed to fix. The vendor demo is not authoritative here: its
 * MADCTL/panel orientation differs from ours, so its flip corrects a rotation
 * we don't have.
 *
 * The raw readout on THIS board is upright but MIRRORED (real-world right came
 * out as screen left), so the correction is hmirror alone. For a future module
 * mounted differently: upside down AND mirrored needs vflip=1 alone; rotated a
 * full 180 (upside down, text still reading left-to-right) needs both. */
static int s_cam_vflip   = 0;   // readout is already upright on this board
static int s_cam_hmirror = 1;   // un-mirror: raw feed swaps left/right
/* Brightness / contrast / saturation / sharpness: -8..+8 (0 = neutral). The
 * driver's own setters stop at +-3 (+-4 for saturation) — the extended range is
 * reached by writing the SAME registers the driver's tables target, with the
 * per-level step extrapolated past the table's end (see cam_apply_controls). */
static int s_cam_bright   = 0;
static int s_cam_contrast = 0;
static int s_cam_sat      = 0;
static int s_cam_sharp    = 0;
/* Denoise strength 0 (off) .. 8. 4 was the old hardcoded default. */
static int s_cam_denoise  = 4;
/* Exposure compensation (AE target level), the sensor's native -5..+5. */
static int s_cam_ev       = 0;
/* White balance preset: 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home. */
static int s_cam_wb       = 0;
/* Special effect: 0 none, 1 negative, 2 B&W, 3 red, 4 green, 5 blue, 6 sepia. */
static int s_cam_effect   = 0;
/* Max auto-gain ("ISO limit"): -1 = driver default (untouched), 0..6 = the
 * gainceiling_t steps 2x..128x. Lower = darker dark shots but far less noise. */
static int s_cam_gainceil = -1;

#define CAM_LEVEL_MIN  (-8)
#define CAM_LEVEL_MAX  ( 8)

static int cam_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Mains frequency for the flicker (banding) filter — 1 = 50 Hz (Europe/Asia),
 * 0 = 60 Hz (Americas). Indoor lighting pulses at twice the mains rate; rows
 * exposed under different phases of that pulse come out brighter/darker, which
 * is exactly the "horizontal lines, worst in dim light" artifact. The sensor's
 * banding filter quantises exposure to whole flicker periods to cancel it, but
 * it must know WHICH frequency — its auto-detect is unreliable in low light
 * (the very condition where banding is strongest), so it is forced manual. */
#ifndef CAM_BAND_50HZ
#define CAM_BAND_50HZ 1
#endif

/* Preview PCLK boost (cam_boost_preview_clock): doubling the sensor PLL bought
 * preview fps, but it runs the OV5640's analog readout at 2x its configured
 * operating point — a prime suspect for the fine VERTICAL column noise seen in
 * photos (strong in the dark, faint but present in light). Photos are captured
 * from the boosted stream, so it poisons stills too. Disabled while that is
 * being verified; set 1 to restore the fast preview. */
#ifndef CAM_PCLK_BOOST
#define CAM_PCLK_BOOST 0
#endif

/* Instant shutter (software-encode the live RGB565 preview frame when the still
 * size equals the preview size): fast, but RGB565 is 5-6-5 BIT colour — dim
 * gradients quantise into stripes of near-identical colours (banding), which the
 * sensor's own 8-bit JPEG pipeline never shows. Quality won: stills now always
 * go through the sensor JPEG path (~300 ms shutter). Set 1 to trade colour
 * depth for the instant shutter again. */
#ifndef CAM_INSTANT_SHUTTER
#define CAM_INSTANT_SHUTTER 0
#endif

/* Push every control to the sensor. Safe to call when the driver is down (no-op).
 * Each setter is null-checked: the function-pointer table is per-sensor and a
 * model that lacks one leaves it null rather than stubbing it. */
static bool cam_apply_zoom(int out_w, int out_h);   /* defined below; used by cam_init_mode */
static bool cam_preview_restart(void);              /* defined below; used by cam_zoom_set  */

static void cam_apply_controls(void) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  /* FORCE the orientation writes. The sensor driver CACHES the current value in
   * s->status.vflip / .hmirror and several drivers (the OV5640's included) return
   * early when the requested value equals the cached one — without touching the
   * register. After esp_camera_init() the cache can already read 1 while the chip's
   * actual register is 0, so a plain set_vflip(s, 1) is silently dropped and the
   * feed stays upside down. That is exactly the "flip never applied" symptom.
   *
   * Writing the OPPOSITE value first guarantees a real register write on the second
   * call, because the cache is then genuinely different. The intermediate value is
   * never displayed: both writes land within one I2C burst, long before the next
   * frame is read out. */
  if (s->set_vflip) {
    s->set_vflip(s, s_cam_vflip ? 0 : 1);     // make the cache differ...
    s->set_vflip(s, s_cam_vflip);             // ...so THIS one really writes
  }
  if (s->set_hmirror) {
    s->set_hmirror(s, s_cam_hmirror ? 0 : 1);
    s->set_hmirror(s, s_cam_hmirror);
  }
  /* Driver setters first, clamped to their NATIVE ranges (brightness/contrast
   * +-3, saturation +-4 via its colour-matrix table, ae_level +-5) — this keeps
   * the driver's status cache coherent. The raw register writes further down
   * then push brightness/contrast/sharpness past the tables' ends. */
  if (s->set_brightness) s->set_brightness(s, cam_clampi(s_cam_bright, -3, 3));
  if (s->set_contrast)   s->set_contrast(s, cam_clampi(s_cam_contrast, -3, 3));
  if (s->set_saturation) s->set_saturation(s, cam_clampi(s_cam_sat, -4, 4));
  if (s->set_ae_level)   s->set_ae_level(s, s_cam_ev);
  if (s->set_wb_mode)    s->set_wb_mode(s, s_cam_wb);
  if (s->set_special_effect) s->set_special_effect(s, s_cam_effect);
  if (s_cam_gainceil >= 0 && s->set_gainceiling)
    s->set_gainceiling(s, (gainceiling_t)s_cam_gainceil);

  /* ---- line/band suppression (the "visible lines in the dark" fixes) ----
   * 1) Flicker banding: force the banding filter ON and MANUAL at the mains
   *    frequency (see CAM_BAND_50HZ above) — the auto-detect picks wrong in
   *    exactly the dim scenes where the bands are strongest.
   *      0x3A00[5] : banding filter function enable
   *      0x3C01[7] : 1 = manual band selection (auto detect off)
   *      0x3C00[2] : 1 = 50 Hz, 0 = 60 Hz
   * 2) Sensor-side denoise (where the driver exposes it): knocks down the
   *    high-gain row noise that reads as faint horizontal streaks in low
   *    light. Moderate level — heavy denoise smears texture. */
  if (s->set_reg) {
    s->set_reg(s, 0x3A00, 0x20, 0x20);
    s->set_reg(s, 0x3C01, 0x80, 0x80);
    s->set_reg(s, 0x3C00, 0x04, CAM_BAND_50HZ ? 0x04 : 0x00);
    /* 3) Black-level calibration every frame (0x4005[1]). By default BLC only
     *    recalibrates when gain crosses thresholds; in the dark the gain pegs
     *    at max and per-COLUMN amp offsets drift uncorrected — showing as
     *    vertical stripes and a lifted purple noise floor. */
    s->set_reg(s, 0x4005, 0x02, 0x02);
  }
  if (s->set_denoise) s->set_denoise(s, s_cam_denoise);

  /* ---- extended-range image controls (OV5640 SDE / CIP registers) ----------
   * These write the same registers the driver's own +-3 tables end at, with the
   * per-level step continued out to +-8. Runs AFTER the driver setters so the
   * raw values win; after set_denoise because both touch 0x5308 (masked here). */
  if (s->set_reg) {
    /* Brightness: SDE Y offset. Driver step is 0x10/level (0x5587 magnitude,
     * 0x5588[3] = negative). +-8 -> +-0x80. */
    int y = (s_cam_bright < 0 ? -s_cam_bright : s_cam_bright) * 0x10;
    if (y > 0xFF) y = 0xFF;
    s->set_reg(s, 0x5587, 0xFF, y);
    s->set_reg(s, 0x5588, 0x08, s_cam_bright < 0 ? 0x08 : 0x00);
    /* Contrast: SDE Y gain, 0x20 = 1.0x, driver step 8/level. -8 clamps just
     * above zero (near-flat grey), +8 = 3x (crushed, deliberate). */
    s->set_reg(s, 0x5586, 0xFF, cam_clampi(0x20 + s_cam_contrast * 8, 0x02, 0x60));
    /* Sharpness: CIP edge-enhancement thresholds, manual mode (0x5308[6]=0 —
     * the driver's own +-3 path does the same). Same 8/level step. */
    {
      int m2 = cam_clampi((s_cam_sharp + 3) * 8, 0x00, 0xFF);
      int m1 = cam_clampi(m2 + 1, 0x01, 0xFF);
      s->set_reg(s, 0x5308, 0x40, 0x00);
      s->set_reg(s, 0x5300, 0xFF, 0x10);  s->set_reg(s, 0x5301, 0xFF, 0x10);
      s->set_reg(s, 0x5302, 0xFF, m1);    s->set_reg(s, 0x5303, 0xFF, m2);
      s->set_reg(s, 0x5309, 0xFF, 0x10);  s->set_reg(s, 0x530A, 0xFF, 0x10);
      s->set_reg(s, 0x530B, 0xFF, 0x04);  s->set_reg(s, 0x530C, 0xFF, 0x06);
    }
    /* Saturation past the colour-matrix table's +-4: an SDE UV gain layered on
     * top (0x40 = 1x, so +8 reaches ~2.5x chroma, -8 fully grey). Only while no
     * special effect is active — the effects own 0x5580/0x5583/0x5584. */
    if (s_cam_effect == 0) {
      if (s_cam_sat > 4 || s_cam_sat < -4) {
        int extra = (s_cam_sat > 0) ? s_cam_sat - 4 : s_cam_sat + 4;
        int uv = cam_clampi(0x40 + extra * 0x18, 0x00, 0xFF);
        s->set_reg(s, 0x5583, 0xFF, uv);
        s->set_reg(s, 0x5584, 0xFF, uv);
        s->set_reg(s, 0x5580, 0x02, 0x02);   /* SDE saturation enable */
        s->set_reg(s, 0x5588, 0x40, 0x40);   /* manual UV adjust */
      } else {
        s->set_reg(s, 0x5588, 0x40, 0x00);   /* back to colour-matrix only */
      }
    }
  }

  /* 4) The ISP correction chain, explicitly ON (null-checked per sensor):
   *    bad/white-pixel cancel (hot pixels at high gain), raw gamma (spreads
   *    the sensor's dynamic range before quantisation — smoother dark
   *    gradients), lens shading correction (evens out corner falloff and the
   *    glow around bright lights), and DCW scaling. */
  if (s->set_bpc)     s->set_bpc(s, 1);
  if (s->set_wpc)     s->set_wpc(s, 1);
  if (s->set_raw_gma) s->set_raw_gma(s, 1);
  if (s->set_lenc)    s->set_lenc(s, 1);
  if (s->set_dcw)     s->set_dcw(s, 1);
}

/* Fill a camera_config_t with this board's pin map. `fmt`/`fs`/`q` pick the
 * mode; `fbs` the buffer count (see cam_init_mode's callers). */
static camera_config_t cam_make_config(pixformat_t fmt, framesize_t fs, int q, int fbs) {
  camera_config_t c = {};
  c.pin_pwdn      = CAM_PIN_PWDN;
  c.pin_reset     = CAM_PIN_RESET;
  c.pin_xclk      = CAM_PIN_XCLK;
  c.pin_sccb_sda  = CAM_PIN_SIOD;
  c.pin_sccb_scl  = CAM_PIN_SIOC;
  c.pin_d7        = CAM_PIN_D7;
  c.pin_d6        = CAM_PIN_D6;
  c.pin_d5        = CAM_PIN_D5;
  c.pin_d4        = CAM_PIN_D4;
  c.pin_d3        = CAM_PIN_D3;
  c.pin_d2        = CAM_PIN_D2;
  c.pin_d1        = CAM_PIN_D1;
  c.pin_d0        = CAM_PIN_D0;
  c.pin_vsync     = CAM_PIN_VSYNC;
  c.pin_href      = CAM_PIN_HREF;
  c.pin_pclk      = CAM_PIN_PCLK;
  c.xclk_freq_hz  = CAM_XCLK_HZ;
  c.ledc_timer    = LEDC_TIMER_0;
  c.ledc_channel  = LEDC_CHANNEL_0;
  c.pixel_format  = fmt;
  c.frame_size    = fs;
  c.jpeg_quality  = q;
  /* PREVIEW runs DOUBLE-buffered with GRAB_LATEST: the DVP engine free-runs into
   * one buffer while the UI reads the other, so esp_camera_fb_get() hands back a
   * finished frame instead of BLOCKING the LVGL thread for a full capture. With
   * the old fb_count=1 + GRAB_WHEN_EMPTY, capture could only START after the UI
   * returned the sole buffer, then fb_get sat waiting for that capture to finish
   * — every tick paid a whole frame period doing nothing, which is a large part
   * of the "single-digit fps" preview. The second QVGA RGB565 buffer costs 150 KB
   * of PSRAM — nothing against 8 MB.
   *
   * STILLS keep the single-buffer config: a UXGA JPEG buffer is big, the mode
   * lives only for one shot, and the warm-up-frame discard in cam_capture_jpeg
   * already handles freshness there. */
  /* Buffer count comes from the caller: the PREVIEW runs 2 (capture overlaps
   * consumption; three was tried and measured WORSE — the always-armed DVP burns
   * PSRAM bandwidth on frames that mostly get discarded), stills run 1 (a UXGA
   * RGB565 frame is 3.8 MB — two of them would eat half the PSRAM for a single
   * shot). GRAB_LATEST only makes sense with a spare buffer. */
  c.fb_count      = fbs;
  c.grab_mode     = (fbs > 1) ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
  c.fb_location   = CAMERA_FB_IN_PSRAM;
  return c;
}

/* (Re)initialise the driver in a given mode. Returns true on success. */
static bool cam_init_mode(pixformat_t fmt, framesize_t fs, int q, int fbs) {
  if (s_cam_ready) { esp_camera_deinit(); s_cam_ready = false; }
  camera_config_t cfg = cam_make_config(fmt, fs, q, fbs);
  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    USBSerial.printf("[cam] init failed (0x%X)\n", (unsigned)err);
    return false;
  }
  s_cam_ready = true;
  s_cam_fmt   = fmt;

  /* Orientation + image controls are re-applied on EVERY init, because the driver
   * resets the sensor when the mode changes (preview <-> still), which would
   * otherwise drop them mid-session and make a photo come out flipped or flat
   * relative to the preview that framed it. */
  cam_apply_controls();

  /* One-line sanity dump: is the fitted chip really the OV5640 all the raw
   * register writes assume (PID 0x5640), and do those writes actually land?
   * Read back straight from the sensor, not the driver cache. */
  {
    sensor_t *s = esp_camera_sensor_get();
    if (s && s->get_reg) {
      USBSerial.printf("[cam] sensor PID=0x%04X VER=0x%02X MID=%02X%02X | "
                       "3A00=%02X 3C00=%02X 3C01=%02X 4005=%02X 3036=%02X | "
                       "gainceil=%02X%02X isp 5000=%02X 5001=%02X\n",
                       s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL,
                       s->get_reg(s, 0x3A00, 0xFF), s->get_reg(s, 0x3C00, 0xFF),
                       s->get_reg(s, 0x3C01, 0xFF), s->get_reg(s, 0x4005, 0xFF),
                       s->get_reg(s, 0x3036, 0xFF),
                       s->get_reg(s, 0x3A18, 0xFF), s->get_reg(s, 0x3A19, 0xFF),
                       s->get_reg(s, 0x5000, 0xFF), s->get_reg(s, 0x5001, 0xFF));
    }
  }
  return true;
}

/* ---- zoom (real sensor windowing, not upscaling) --------------------------
 * The OV5640's full array is 2592x1944. set_res_raw() picks WHICH RECTANGLE of
 * that array is read out and what size it is scaled to on the way out. Reading a
 * smaller centred rectangle into the same output size is a true optical-region
 * zoom: the detail is real, because those are real sensor pixels, not an
 * interpolated blow-up of an already-downscaled frame.
 *
 * Zoom is expressed in 1/100ths (100 = 1.0x) so the UI can step it without float
 * state. 4.0x is the ceiling: beyond that the read window gets close to the output
 * resolution and there is no detail left to gain, only noise and a narrower field.
 *
 * NOTE this is applied to the PREVIEW pipeline. cam_capture_jpeg() re-inits the
 * driver for the still and re-applies the same factor, so what you framed is what
 * you get. */
#define CAM_SENSOR_W   2592          /* OV5640 full active array */
#define CAM_SENSOR_H   1944
#define CAM_ZOOM_MIN   100           /* 1.00x — full field of view */
/* 3.00x ceiling, not 4.00x. The limit is the sensor's BINNED readout: binning
 * needs the read window to cover 2x the output in both axes, and the portrait
 * preview's base window (1458x1944 for a 240x320 output) hits that bound at
 * exactly 3.04x. Past it the sensor would have to leave the binned timing
 * regime its PLL is programmed for — the mismatch that wedged the DVP (see
 * cam_apply_zoom). 3x of true optical-region zoom is the honest maximum. */
#define CAM_ZOOM_MAX   300
#define CAM_ZOOM_STEP   25           /* 0.25x per button press */

static int s_cam_zoom = CAM_ZOOM_MIN;   /* current factor, in 1/100ths */

/* Apply the current zoom to a given output size. Returns false if the sensor has
 * no windowing support (then the caller simply keeps the full field). */
static bool cam_apply_zoom(int out_w, int out_h) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !s->set_res_raw) return false;
  if (s_cam_zoom < CAM_ZOOM_MIN) s_cam_zoom = CAM_ZOOM_MIN;
  if (s_cam_zoom > CAM_ZOOM_MAX) s_cam_zoom = CAM_ZOOM_MAX;

  /* BASE WINDOW: the largest centred rectangle of the OUTPUT'S aspect that fits
   * the array. The window must match the output shape or the ISP scaler maps one
   * aspect onto another and the image squishes — which is also how the PORTRAIT
   * preview works at all: a 240x320 output reads a 1458x1944 (3:4) window, so
   * the panel gets exactly the cover-crop it used to compute in software, only
   * now the sensor does it for free. (This also fixes zoomed 16:9 stills, which
   * previously scaled a 4:3 window into a 16:9 output — anamorphic squish.) */
  int base_w = CAM_SENSOR_W, base_h = CAM_SENSOR_H;
  if ((int64_t)out_w * CAM_SENSOR_H < (int64_t)out_h * CAM_SENSOR_W) {
    base_w = (int)((int64_t)CAM_SENSOR_H * out_w / out_h) & ~1;  /* taller than 4:3: full height */
  } else {
    base_h = (int)((int64_t)CAM_SENSOR_W * out_h / out_w) & ~1;  /* wider than 4:3: full width  */
  }

  /* Centred read window: shrink the base rectangle by the zoom factor.
   *
   * EVERY EDGE MUST BE EVEN — this is a correctness requirement, not tidiness.
   * The OV5640 is a BAYER sensor: its colour filter is a 2x2 mosaic (RGGB) and the
   * demosaic assumes the readout starts on that mosaic's phase. Starting one pixel
   * across (odd sx) or one line down (odd sy) shifts the phase, so R and B swap and
   * the WHOLE FRAME comes out with a magenta/purple cast.
   *
   * That is exactly the "every second zoom step is purple" symptom: with a 0.25x
   * step, sx/sy land odd on alternating steps —
   *     1.00x sx=0   sy=0    even -> correct colour
   *     1.25x sx=259 sy=194  ODD  -> purple
   *     1.50x sx=432 sy=324  even -> correct colour
   *     1.75x sx=555 sy=417  ODD  -> purple
   * Rounding the origin DOWN and the size down to even keeps the mosaic phase at
   * every factor. Width/height are made even too: an odd window height feeds the
   * scaler a half mosaic row, which shows as a coloured fringe on the last line. */
  int win_w = (base_w * 100 / s_cam_zoom) & ~1;
  int win_h = (base_h * 100 / s_cam_zoom) & ~1;
  int sx    = ((CAM_SENSOR_W - win_w) / 2) & ~1;
  int sy    = ((CAM_SENSOR_H - win_h) / 2) & ~1;

  /* TIMING TOTALS AND BINNING MUST MATCH THE DRIVER'S MODE — this is what makes
   * zoom SAFE, not a nicety. set_res_raw writes HTS/VTS (the per-line and
   * per-frame clock totals, blanking included) VERBATIM from its totalX/totalY
   * arguments. An earlier revision passed the ARRAY size (2592x1944) as the
   * totals with binning off: that is ~2x the pixel-periods per frame of the
   * driver's own binned-QVGA timing (HTS 2844-200=2644, VTS 1968/2=984), driven
   * by a PLL the driver had set FOR the binned mode. The sensor's frame rate
   * collapsed and the DVP glitched out of sync the moment zoom was touched —
   * cam_hal spat FB-OVF then a zero-length FB-SIZE frame, and fb_get settled at
   * ~5x its normal wait, unrecoverably (zooming back out re-wrote the same bad
   * totals). So: keep binning ON whenever the window still covers 2x the output
   * (true for the whole 1-4x range at preview sizes — the scaler discards the
   * detail binning would lose, so nothing is given up), and write the driver's
   * own 4:3 totals for whichever readout mode we're in (esp32-camera ov5640.c
   * ratio_table 4:3: tx=2844, ty=1968; binned modes write tx-200, ty/2). The
   * sensor then keeps the frame rate of the mode the PLL was programmed for. */
  bool binning = (win_w >= out_w * 2) && (win_h >= out_h * 2);
  int tot_x = binning ? (2844 - 200) : 2844;
  int tot_y = binning ? (1968 / 2)   : 1968;
  return s->set_res_raw(s, sx, sy, sx + win_w - 1, sy + win_h - 1,
                        0, 0, tot_x, tot_y,
                        out_w, out_h, true /* scale */, binning) == 0;
}

static int  cam_zoom_get(void) { return s_cam_zoom; }
static void cam_zoom_set(int z, int out_w, int out_h) {
  if (z < CAM_ZOOM_MIN) z = CAM_ZOOM_MIN;
  if (z > CAM_ZOOM_MAX) z = CAM_ZOOM_MAX;
  if (z == s_cam_zoom) return;
  s_cam_zoom = z;

  /* BACK TO 1.0x = FULL DRIVER RE-INIT, not a full-field window write.
   *
   * A custom set_res_raw readout runs ~4-5x slower than the driver's native
   * mode on this sensor (measured: fb_get ~25 ms native vs ~95-130 ms windowed)
   * — acceptable while actually zoomed, since it's the only way to get a real
   * optical-region crop, but a full-field WINDOW at 1.0x is just the native
   * view at a fraction of the frame rate. Worse, writing the window registers
   * mid-stream tears the in-flight frame (the cam_hal FB-SIZE 122880/0 errors
   * seen on zoom transitions) and there is no register write that returns the
   * sensor to its native readout. So 1.0x tears the driver down and re-inits:
   * ~300 ms once, clean vsync from the first frame, full speed restored. The
   * zoom-out path used to leave the preview wedged at the slow rate until the
   * app was reopened — this is what actually restores it. */
  if (z <= CAM_ZOOM_MIN && s_cam_fmt == PIXFORMAT_RGB565) {
    cam_preview_restart();
  } else {
    cam_apply_zoom(out_w, out_h);
  }
}

/* Image-control get/set. Every setter stores the value and re-runs the FULL
 * apply: the extended-range register writes must land in a fixed order relative
 * to the driver setters (and to each other), so there is exactly one path that
 * touches the sensor — cam_apply_controls(). No-op while the driver is down;
 * the stored value is pushed on the next init. */
static int  cam_bright_get(void)   { return s_cam_bright; }
static int  cam_contrast_get(void) { return s_cam_contrast; }
static int  cam_sat_get(void)      { return s_cam_sat; }
static int  cam_sharp_get(void)    { return s_cam_sharp; }
static int  cam_denoise_get(void)  { return s_cam_denoise; }
static int  cam_ev_get(void)       { return s_cam_ev; }
static int  cam_wb_get(void)       { return s_cam_wb; }
static int  cam_effect_get(void)   { return s_cam_effect; }
static int  cam_gainceil_get(void) { return s_cam_gainceil; }

static void cam_bright_set(int v)   { s_cam_bright   = cam_clampi(v, CAM_LEVEL_MIN, CAM_LEVEL_MAX); cam_apply_controls(); }
static void cam_contrast_set(int v) { s_cam_contrast = cam_clampi(v, CAM_LEVEL_MIN, CAM_LEVEL_MAX); cam_apply_controls(); }
static void cam_sat_set(int v)      { s_cam_sat      = cam_clampi(v, CAM_LEVEL_MIN, CAM_LEVEL_MAX); cam_apply_controls(); }
static void cam_sharp_set(int v)    { s_cam_sharp    = cam_clampi(v, CAM_LEVEL_MIN, CAM_LEVEL_MAX); cam_apply_controls(); }
static void cam_denoise_set(int v)  { s_cam_denoise  = cam_clampi(v, 0, 8);  cam_apply_controls(); }
static void cam_ev_set(int v)       { s_cam_ev       = cam_clampi(v, -5, 5); cam_apply_controls(); }
static void cam_wb_set(int v)       { s_cam_wb       = cam_clampi(v, 0, 4);  cam_apply_controls(); }
static void cam_effect_set(int v)   { s_cam_effect   = cam_clampi(v, 0, 6);  cam_apply_controls(); }
static void cam_gainceil_set(int v) { s_cam_gainceil = cam_clampi(v, -1, 6); cam_apply_controls(); }

/* Orientation, exposed so a future board (or a user preference) can correct a
 * differently-mounted module without editing the driver. */
static void cam_flip_set(int vflip, int hmirror) {
  s_cam_vflip = vflip ? 1 : 0;
  s_cam_hmirror = hmirror ? 1 : 0;
  cam_apply_controls();
}

/* ---- preview PCLK boost --------------------------------------------------
 * THE SENSOR'S OWN FRAME RATE IS THE PREVIEW'S FLOOR. The probe showed 7.3 fps
 * (137 ms/frame) in EVERY pipeline configuration — every CPU-side optimisation
 * just moved slack into fb_get's wait. esp32-camera's OV5640 driver programs
 * very conservative PLL settings for non-JPEG (RGB565) modes — its JPEG modes
 * run 10-20x higher sysclk multipliers — because classic-ESP32 I2S capture
 * couldn't keep up. The S3's LCD_CAM DVP has no such problem at these rates.
 *
 * WHICH BRACKET IS SLOW (ov5640.c set_framesize, non-JPEG):
 *   > HVGA          : set_pll(mult 10, ..., pclk_div 2)  -> rate ~2.5
 *   >= QVGA..HVGA   : set_pll(mult  8, ..., pclk_div 4)  -> rate ~2.0  <- SLOWEST
 *   < QVGA          : set_pll(mult 20, ..., pclk_div 8)  -> rate ~2.5
 * The QVGA preview lands in the slowest clock class of the whole driver — which
 * is why a 240x240 (1:1) preview measured 19+ fps on this very pipeline while
 * QVGA sat at 7.3. (An earlier revision of this boost halved the 0x3035 system
 * divider — a NO-OP for QVGA, whose bracket already uses divider 1. The
 * multiplier is the register that actually matters.)
 *
 * So: DOUBLE the PLL multiplier (0x3036) after the preview init — PCLK and the
 * frame rate double with it, and AEC's max exposure shortens along with the
 * frame (a bonus against motion blur). Applies to BOTH preview brackets:
 *   mult  8 -> 16  (QVGA-class 4:3/16:9: 7.3 -> ~15 fps, verified on hardware)
 *   mult 20 -> 40  (sub-QVGA 1:1: the stock ~20-25 fps sensor rate was the wall
 *                   past the display's ~30 fps ceiling; doubled, the pipeline
 *                   becomes the limit instead of the sensor)
 * The gate (2..20, i.e. only known driver-written preview values) keeps unknown
 * states untouched. PREVIEW ONLY — stills re-init into untouched driver timing.
 * Delete the call in cam_begin to revert. */
static void cam_boost_preview_clock(void) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !s->get_reg || !s->set_reg) return;
  int mult = s->get_reg(s, 0x3036, 0xFF);
  /* Only values the driver's non-JPEG brackets actually write (8/10/20);
   * anything else is an unknown state we must not double blindly. */
  if (mult < 2 || mult > 20) return;
  int nm = mult * 2;
  s->set_reg(s, 0x3036, 0xFF, nm);

  /* RESCALE THE BANDING FILTER for the new clock. The filter counts EXPOSURE
   * LINES per mains half-cycle; doubling the PLL doubles the row rate, so the
   * driver-computed step words are now HALF the truth — the filter quantises
   * exposure to the wrong period and the flicker bands it exists to cancel
   * come back as horizontal lines (worst in dim light, where exposure is
   * long). Step words (lines / half-cycle) DOUBLE with the row rate; the
   * max-band counts (how many steps fit in a frame — VTS is unchanged) HALVE.
   *   0x3A08/09: B50 step   0x3A0A/0B: B60 step
   *   0x3A0E:    B50 max    0x3A0D:    B60 max */
  int b50 = ((s->get_reg(s, 0x3A08, 0xFF) << 8) | s->get_reg(s, 0x3A09, 0xFF)) * 2;
  int b60 = ((s->get_reg(s, 0x3A0A, 0xFF) << 8) | s->get_reg(s, 0x3A0B, 0xFF)) * 2;
  if (b50 > 0x3FF) b50 = 0x3FF;                 // field width guard
  if (b60 > 0x3FF) b60 = 0x3FF;
  s->set_reg(s, 0x3A08, 0xFF, (b50 >> 8) & 0xFF);
  s->set_reg(s, 0x3A09, 0xFF,  b50       & 0xFF);
  s->set_reg(s, 0x3A0A, 0xFF, (b60 >> 8) & 0xFF);
  s->set_reg(s, 0x3A0B, 0xFF,  b60       & 0xFF);
  int mb50 = s->get_reg(s, 0x3A0E, 0xFF) / 2; if (mb50 < 1) mb50 = 1;
  int mb60 = s->get_reg(s, 0x3A0D, 0xFF) / 2; if (mb60 < 1) mb60 = 1;
  s->set_reg(s, 0x3A0E, 0xFF, mb50);
  s->set_reg(s, 0x3A0D, 0xFF, mb60);

  USBSerial.printf("[cam] pclk boost: PLL mult %d -> %d (band steps 50Hz=%d 60Hz=%d)\n",
                   mult, nm, b50, b60);
}

/* Bring the camera up in PREVIEW (RGB565) mode. Idempotent and lazy: safe to call
 * on every app open. Latches absence so a board with an empty header (or a loose
 * ribbon) doesn't pay a failed init every time. */
static bool cam_begin(void) {
  if (s_cam_ready && s_cam_fmt == PIXFORMAT_RGB565) return true;
  if (s_cam_absent) return false;
  if (!cam_init_mode(PIXFORMAT_RGB565, cam_preview_framesize(), 12, 2)) {
    if (!s_cam_ready) s_cam_absent = true;   // hard failure -> stop retrying
    return false;
  }
  /* Re-assert the zoom window for the preview's output size — the init above
   * reset the sensor, so a non-1.0x zoom would silently snap back to full field.
   * At exactly 1.0x, deliberately NO set_res_raw: the driver's untouched native
   * mode is the fast, proven readout (see cam_preview_w above). */
  if (s_cam_zoom > CAM_ZOOM_MIN) cam_apply_zoom(cam_preview_w(), cam_preview_h());

  /* Double the sensor's RGB565 frame rate (see cam_boost_preview_clock). Runs
   * after the zoom window so it applies to whatever readout is active. */
#if CAM_PCLK_BOOST
  cam_boost_preview_clock();
#endif
  return true;
}

/* Restart the preview in the CURRENT aspect's frame size.
 *
 * Needed because the driver's frame size is fixed at init: changing the aspect
 * only changes what the NEXT init asks for, so without this the viewfinder keeps
 * showing the old shape until the app is reopened — which is exactly the "changing
 * the resolution does nothing" symptom. Tearing down and re-initing is the only
 * way to change frame size in esp32-camera; it costs ~300 ms, paid once per
 * aspect change rather than per frame. */
static bool cam_preview_restart(void) {
  if (s_cam_absent) return false;
  if (s_cam_ready) { esp_camera_deinit(); s_cam_ready = false; }
  return cam_begin();
}

/* Release the driver and its PSRAM framebuffers. Called when the app closes so a
 * camera left idle costs neither the ~115 KB buffer nor the sensor's draw. */
static void cam_end(void) {
  if (!s_cam_ready) return;
  esp_camera_deinit();
  s_cam_ready = false;
#if defined(CAM_PIN_PWDN) && (CAM_PIN_PWDN >= 0)
  /* esp_camera_deinit() releases the driver but leaves PWDN LOW — the sensor
   * keeps idling on the always-on 3V3 rail (no PMU / load switch on this
   * board). Re-assert hardware power-down (~µA) until the next cam_begin(),
   * whose esp_camera_init() takes the pin back and powers the sensor up. */
  gpio_set_direction((gpio_num_t)CAM_PIN_PWDN, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)CAM_PIN_PWDN, 1);
#endif
}

static bool cam_available(void) { return s_cam_ready || !s_cam_absent; }

/* One RGB565 preview frame, or nullptr. EVERY non-null return MUST be released
 * with cam_preview_done() or the single framebuffer is never recycled and the
 * next grab blocks forever. */
static camera_fb_t *cam_preview_get(void) {
  if (!s_cam_ready || s_cam_fmt != PIXFORMAT_RGB565) return nullptr;
  return esp_camera_fb_get();
}

static void cam_preview_done(camera_fb_t *fb) {
  if (fb) esp_camera_fb_return(fb);
}

/* Take a full-resolution JPEG still.
 *
 * Switches the driver to JPEG mode, grabs one frame, copies it into a caller-owned
 * PSRAM buffer, then returns the driver to preview mode. The COPY matters: the
 * framebuffer must be handed back before re-init, so the caller cannot be left
 * holding driver-owned memory across that boundary.
 *
 * On success returns a heap_caps_malloc'd buffer in *out (caller frees with
 * heap_caps_free) and its length in *out_len. Returns false and touches nothing
 * on failure. The camera is left in preview mode either way, so the live view
 * resumes even after a failed shot. */
static bool cam_capture_jpeg(uint8_t **out, size_t *out_len) {
  if (!out || !out_len) return false;
  *out = nullptr; *out_len = 0;
  if (s_cam_absent) return false;

  const cam_res_t *res = cam_res();      // the user's chosen aspect + resolution

  /* ---- INSTANT-SHUTTER FAST PATH ------------------------------------------
   * When the chosen still size EQUALS the live preview's frame size (the default
   * 4:3 @ 320x240, and 1:1 @ 240x240), there is nothing the JPEG re-init would
   * add: the sensor is already delivering exactly those pixels. So take the next
   * live frame and JPEG-encode it in SOFTWARE instead — no deinit/reinit (the
   * ~300 ms shutter delay disappears), no auto-exposure re-convergence, and the
   * preview never even stops. The zoom window is already applied to the preview
   * pipeline, so framing is untouched.
   *
   * fmt2jpg's quality is JPEG-standard 0..100 (HIGHER = better) — the OPPOSITE
   * convention of the sensor encoder's CAM_STILL_QUALITY (lower = better); do
   * not feed one to the other. It encodes straight from the sensor's BIG-endian
   * RGB565 (the raw fb, BEFORE the preview's little-endian swap) and allocates
   * the output buffer itself, which the caller frees exactly like the copy made
   * by the slow path below. */
#if CAM_INSTANT_SHUTTER
  if (s_cam_ready && s_cam_fmt == PIXFORMAT_RGB565) {
    camera_fb_t *fb = cam_preview_get();
    if (fb) {
      if (fb->width == res->w && fb->height == res->h &&
          fb->format == PIXFORMAT_RGB565) {
        bool ok = fmt2jpg(fb->buf, fb->len, fb->width, fb->height,
                          PIXFORMAT_RGB565, 90, out, out_len);
        cam_preview_done(fb);
        return ok;
      }
      cam_preview_done(fb);              // size differs -> normal capture below
    }
  }
#endif

  if (!cam_init_mode(PIXFORMAT_JPEG, res->fs, CAM_STILL_QUALITY, 1)) {
    cam_begin();                       // try to restore preview even if the shot failed
    return false;
  }
  /* Re-apply the zoom at the STILL's output size so the photo captures the same
   * field the preview framed. Without this a zoomed-in preview would silently
   * produce a full-field photo — the framing you saw would be a lie. */
  if (s_cam_zoom > CAM_ZOOM_MIN) cam_apply_zoom(res->w, res->h);   /* chosen size */

  /* Discard one frame: the first after a mode switch is captured while the sensor's
   * auto-exposure/white-balance are still converging from the old mode, so it comes
   * out visibly darker or colour-shifted. The second frame is the good one. */
  camera_fb_t *warm = esp_camera_fb_get();
  if (warm) esp_camera_fb_return(warm);

  bool ok = false;
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb && fb->len > 0) {
    uint8_t *buf = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) {
      memcpy(buf, fb->buf, fb->len);
      *out = buf;
      *out_len = fb->len;
      ok = true;
    } else {
      USBSerial.println("[cam] out of PSRAM for the still");
    }
  }
  if (fb) esp_camera_fb_return(fb);    // hand it back BEFORE re-init

  cam_begin();                          // back to preview mode
  return ok;
}

#endif  /* BOARD_HAS_CAMERA */
