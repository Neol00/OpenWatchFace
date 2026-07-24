/* ============================================================================
 *  tuya/owf_tuya_lvgl_own.h - run OUR OWN LVGL v9.5 on the T5 (not the vendor's).
 *
 *  The TuyaOpen SDK ships an LVGL v8 behind its lv_vendor_* layer. Our firmware (and
 *  all its perf patches: band-split render, async QSPI flush) is written for LVGL
 *  v9.5 - the SAME version the ESP boards use. Running the vendor v8 mis-renders the
 *  v9-authored UI (wrong fonts/styles, glitches) and can't take our patches.
 *
 *  The vendor LVGL lives in libtuyaos.a as per-file objects (lv_init.c.o, ...), and
 *  ONLY lv_vendor.c.o references them. So if we never call lv_vendor_init/start, the
 *  linker pulls in NONE of the vendor lv_*.o, and our own LVGL (a sketch library)
 *  provides those symbols with no collision. (Verified: no other SDK archive
 *  references any lv_* symbol.) This header is that replacement: we drive LVGL
 *  ourselves and push pixels through the SDK's panel device (tdl_disp_*), which is
 *  the lower-level, LVGL-independent display path.
 *
 *  ARCHITECTURE (same dirty-rect model as the ESP32-S3 path in OpenWatchFace.ino):
 *    - LVGL renders each dirty area into its own small PARTIAL draw buffer (px_map).
 *    - flush_cb pushes that area's EXACT rectangle straight to the panel via the SDK
 *      QSPI windowed write (CASET x1..x2, RASET y1..y2 + w*h contiguous pixels). The
 *      CO5300 has persistent GRAM, so untouched pixels keep their last value. There is
 *      NO full-frame framebuffer and NO carry, so nothing can go stale -> no trails and
 *      no flicker-to-start (both were artifacts of the earlier full-frame pool +
 *      ping-pong carry, since tdl_disp_get_free_fb returns the first free buffer by pool
 *      INDEX, not a deterministic 'other' buffer, and could hand back an in-flight one).
 *    - tdl_disp_dev_flush() is ASYNC (queues to the SDK qspi_task, reads the buffer
 *      LATER, then calls free_cb). So we don't hand it LVGL's px_map (LVGL would reuse
 *      px_map mid-transfer); we keep TWO small BOUNCE buffers: copy+byte-swap px_map into
 *      one, queue its push, and return flush_ready immediately so LVGL renders the next
 *      area while this one clocks out. A bounce buffer's free_cb posts its semaphore on
 *      DMA-done; we wait on it before refilling. (Like the S3's async tile overlap.)
 *    - touch via tdl_tp_dev_read() (the CST92xx we already registered).
 *    - NO vendor task: the firmware runs lv_timer_handler() in loop().
 *
 *  Included ONLY on BOARD_PLATFORM_TUYA. Uses OUR LVGL (lvgl.h), never lv_vendor.h.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_TUYA

#include <lvgl.h>            // OUR LVGL v9.5 (sketch library), NOT the vendor's

extern "C" {
#include "tuya_cloud_types.h"
#include "tdl_display_manage.h"     // tdl_disp_find_dev / _dev_open / _dev_flush / _dev_get_info
#include "tdl_display_type.h"       // TDL_DISP_FRAME_BUFF_T, DISP_FB_TP_PSRAM
#include "tdl_tp_manage.h"          // tdl_tp_find_dev / _dev_open / _dev_read / TDL_TP_POS_T
}

/* PSRAM overclock (render lever — draw buffers are in PSRAM and render is PSRAM-latency bound).
 * Going above the 120MHz boot clock requires raising voltage AND enabling the PSRAM DPLL before
 * selecting it (a bare bk_psram_set_clk corrupts — the DPLL source is off at boot). The full
 * sequence + decode lives here; it is RECORD-ONLY until OWF_PSRAM_FREQ_APPLY is set to 1. */
#include "compat/owf_tuya_psram_freq.h"

/* Our own CO5300 QSPI panel driver (init/flush/rotation in our source, on the tkl_qspi HAL).
 * Compiles to nothing unless OWF_T5_OWN_PANEL=1; when on, it REPLACES the SDK tdl_disp path
 * below (allocation, flush, bring-up) so the display driver is fully ours. Touch stays on the
 * SDK tdl_tp layer either way. */
#include "owf_tuya_co5300_qspi.h"

/* Bounce-buffer type: our own struct when we own the panel, else the SDK's. Both expose the
 * same fields (frame/x_start/y_start/width/height/len/free_cb/free_arg) so the flush/begin
 * code below is shared. */
#if OWF_T5_OWN_PANEL
typedef owf_pnl_fb_t owf_fb_t;
#else
typedef TDL_DISP_FRAME_BUFF_T owf_fb_t;
#endif

/* The display + touch are registered (CO5300 + CST92xx) by owf_tuya_register_panel()
 * under the name OWF_T5_DISPLAY_NAME - see owf_tuya_port.h, which is included first. */

#ifndef OWF_T5_LCD_W
#define OWF_T5_LCD_W 466
#endif
#ifndef OWF_T5_LCD_H
#define OWF_T5_LCD_H 466
#endif

/* Draw buffers in internal SRAM instead of PSRAM — TRIED, MEASURED WORSE, keep 0 (PSRAM).
 * The theory was: render is memory-bound and SRAM is single-cycle, so it should beat PSRAM.
 * On-device it LOST, at an identical ~137k px/frm dirty load:
 *   PSRAM 156-line: render 26.2ms/frm, qspi_wait ~0us,    27-28 fps
 *   SRAM   96-line: render 28.5ms/frm, qspi_wait ~1300us, 25-26 fps   <- worse on both axes
 * Why, against the "single-cycle SRAM" expectation:
 *  1) The buffers land in SHARED SRAM0 (0x28000000) on the BUS MATRIX, accessed UNCACHED at bus
 *     latency — NOT zero-wait core-coupled TCM (DTCM is far too small for 142KB). The SW renderer
 *     is READ-dominated (blend reads dest+src, AA, glyphs), so the PSRAM path's per-buffer
 *     write-through READ cache beats uncached SRAM reads; the write-half savings got swamped.
 *  2) It also killed the FREE render/DMA overlap (the qspi_wait 0 -> 1300us jump): with PSRAM,
 *     render reads from CACHE while the zero-copy push DMA reads the PSRAM bus — different
 *     resources, so they overlap and LVGL never stalls. With SRAM, render AND the push DMA both
 *     hammer the SAME SRAM0 bus -> contention -> render slows AND LVGL laps the DMA.
 * So PSRAM buffers + write-through cache + 160MHz overclock is the better operating point.
 * (Cacheable SRAM could in theory win, but needs MPU cacheable mapping + a cache-clean before each
 * DMA flush for coherency — a bigger change with real corruption risk, not worth ~10%.)
 * Set 1 only to re-run the experiment; 96-line bands then keep two SRAM buffers under budget. */
#ifndef OWF_DRAWBUF_IN_SRAM
#define OWF_DRAWBUF_IN_SRAM 0
#endif

/* Partial draw buffer height (lines). LVGL renders into these and we push them straight to the
 * panel (zero-copy). PSRAM affords the full 156; the SRAM experiment used 96 to fit two buffers. */
#ifndef OWF_LVGL_DRAW_LINES
#if OWF_DRAWBUF_IN_SRAM
#define OWF_LVGL_DRAW_LINES 96
#else
#define OWF_LVGL_DRAW_LINES 156
#endif
#endif

static TDL_DISP_HANDLE_T   s_owf_disp     = NULL;   // panel device handle
static TDL_TP_HANDLE_T     s_owf_tp       = NULL;   // touch device handle
static lv_display_t       *s_owf_lv_disp  = NULL;

/* ====================================================================================
 *  PER-AREA DIRTY-RECT PUSH (the same model as the ESP32-S3 path in OpenWatchFace.ino).
 *
 *  We keep NO full-frame framebuffer and NO carry. LVGL renders each dirty area into its
 *  own partial draw buffer (px_map); on flush we push that area's EXACT rectangle straight
 *  to the panel (CASET x1..x2, RASET y1..y2, then w*h contiguous pixels). The CO5300 has
 *  persistent GRAM, so untouched pixels keep their last value. Because nothing reuses a
 *  full-frame buffer that holds an old image, there is NOTHING to go stale -> no trails,
 *  no flicker-to-start. (Those bugs were artifacts of the old full-frame pool + ping-pong
 *  carry: tdl_disp_get_free_fb returns the first free buffer BY POOL INDEX, not a
 *  deterministic "other" buffer, so under fast scroll it could hand back the in-flight
 *  buffer and the next frame rendered over a buffer still being clocked out.)
 *
 *  ZERO-COPY: the two draw buffers LVGL renders into ARE the buffers we hand to the panel —
 *  no memcpy in the flush. Each is wrapped in a TDL_DISP_FRAME_BUFF_T so tdl_disp_dev_flush
 *  can push it; LVGL gets the struct's .frame as its partial draw buffer. On flush we match
 *  px_map back to its struct, set the window, push it ASYNC, and DO NOT call flush_ready yet.
 *
 *  SYNC via deferred flush_ready: tdl_disp_dev_flush() is async — the qspi_task reads the
 *  buffer LATER and calls free_cb when its DMA completes. LVGL must not redraw a buffer while
 *  the panel is still reading it. LVGL enforces this itself: before each flush it blocks in
 *  wait_for_flushing() until lv_display_flush_ready() clears the 'flushing' flag. So we leave
 *  the flag set in flush_cb and call lv_display_flush_ready() from free_cb (DMA-done). With
 *  TWO buffers LVGL ping-pongs: it renders the next area into the OTHER buffer while this one
 *  clocks out, and only blocks if it laps the DMA — true render/DMA overlap with no copy. */
#define OWF_BOUNCE_NUM 2
#define OWF_BOUNCE_MAX_PX ((uint32_t)OWF_T5_LCD_W * OWF_LVGL_DRAW_LINES)  // largest LVGL area
static owf_fb_t *s_owf_bounce[OWF_BOUNCE_NUM] = { NULL, NULL };

/* Panel-ready guard (the SDK path also needs the device handle; the own path doesn't). */
#if OWF_T5_OWN_PANEL
  #define OWF_PANEL_READY() (s_owf_bounce[0] != NULL)
#else
  #define OWF_PANEL_READY() (s_owf_disp && s_owf_bounce[0])
#endif

/* ---- LVGL tick: LVGL v9 pulls time via a callback (set in begin) ---------- */
static uint32_t owf_lvgl_tick_cb(void) {
  return (uint32_t)millis();   // Arduino millis() - simple, always available on this core
}

/* Set OWF_LVGL_PROFILE 1 to log frame stats every ~2s: frames/sec, time spent IN the flush
 * cb per frame (swap-copy + queue), and dirty pixels per frame. */
#ifndef OWF_LVGL_PROFILE
#define OWF_LVGL_PROFILE 0
#endif

/* free_cb: the SDK's qspi_task calls this once a pushed buffer's DMA completes. The buffer is
 * one of LVGL's draw buffers, so DMA-done means LVGL may now reuse it — tell LVGL the flush is
 * finished. lv_display_flush_ready just clears disp->flushing (a single write), which is the
 * canonical cross-thread/ISR use, so calling it from the qspi_task is safe. This is what
 * unblocks LVGL's wait_for_flushing() before it redraws this buffer. */
/* Count completed frame DMAs so loop() can tell when the FIRST frame has actually reached the
 * panel — the trigger for the deferred PSRAM reclock (owf_tuya_psram_overclock_deferred). */
static volatile uint32_t s_owf_frames_flushed = 0;

static void owf_bounce_free_cb(owf_fb_t *fb) {
  (void)fb;
  s_owf_frames_flushed++;
  if (s_owf_lv_disp) lv_display_flush_ready(s_owf_lv_disp);
}

#if OWF_LVGL_PROFILE
/* Render-vs-wait split. LVGL emits RENDER_START/READY around the actual drawing and
 * FLUSH_WAIT_START/FINISH around the stall where the render thread blocks waiting for the
 * previous QSPI push to free the buffer. If wait >> render, the frame rate is bus-bound
 * (QSPI), not compute-bound — making rendering faster (e.g. parallel) can't raise fps. */
static volatile uint32_t s_owf_render_us = 0, s_owf_wait_us = 0;   // accumulated per 2s window
static uint32_t s_owf_render_t0 = 0, s_owf_wait_t0 = 0;
static void owf_lvgl_profile_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  uint32_t now = (uint32_t)micros();
  switch (code) {
    case LV_EVENT_RENDER_START:      s_owf_render_t0 = now; break;
    case LV_EVENT_RENDER_READY:      s_owf_render_us += now - s_owf_render_t0; break;
    case LV_EVENT_FLUSH_WAIT_START:  s_owf_wait_t0 = now; break;
    case LV_EVENT_FLUSH_WAIT_FINISH: s_owf_wait_us += now - s_owf_wait_t0; break;
    default: break;
  }
}
#endif

/* ---- Even-pixel area alignment (REQUIRED by the CO5300) --------------------------------
 * The CO5300 QSPI panel requires draw windows aligned to EVEN pixel boundaries on both axes.
 * LVGL hands flush arbitrary dirty rects (a toggled button, a slider, the brightness % text
 * can start at an odd x or have an odd width). An odd window desyncs the panel's pixel-pair
 * write stride from our packed row data, so every row shifts progressively -> the italic/
 * curved "warp" we saw on buttons/slider/% (the clock face happened to sit on even bounds,
 * so it was usually fine). Snapping each invalidated area outward to even bounds BEFORE LVGL
 * renders means px_map already holds the aligned rectangle's pixels — no edge-pixel gymnastics
 * in the flush. This is the same fix the ESP32-S3 path uses (rounder_event_cb), since that
 * board drives the SAME CO5300 chip. */
static void owf_lvgl_rounder_cb(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
  area->x1 = (area->x1 >> 1) << 1;          // x1 down to even
  area->y1 = (area->y1 >> 1) << 1;          // y1 down to even
  area->x2 = ((area->x2 >> 1) << 1) + 1;    // x2 up to odd  -> even width
  area->y2 = ((area->y2 >> 1) << 1) + 1;    // y2 up to odd  -> even height
}

/* ---- Flush: push the EXACT dirty rectangle straight to the panel (ZERO-COPY) ----------
 * px_map IS one of LVGL's two draw buffers (== a frame-buff's .frame). Match it, set the
 * window, and push that buffer directly — no copy. We DON'T call flush_ready here; free_cb
 * does, on DMA-done, so LVGL won't redraw the buffer until the panel finished reading it.
 * px_map is contiguous w*h RGB565_SWAPPED in raster order — exactly what a windowed panel
 * write expects, so an arbitrary rect is fine (the CO5300 places it via CASET/RASET). */
static void owf_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
#if OWF_LVGL_PROFILE
  uint32_t t_in = (uint32_t)micros();
  static uint32_t s_cb_us = 0, s_dirty_px = 0, s_frames = 0, s_win = 0;
#endif
  bool deferred = false;   // true once we've queued an async push (free_cb will flush_ready)
  if (OWF_PANEL_READY()) {
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    const uint32_t npx = (uint32_t)w * (uint32_t)h;

    /* ZERO-COPY: px_map IS one of our two frame buffers (we handed their .frame to LVGL as the
     * partial draw buffers). Match it back to its struct, set the window, and push it directly
     * — no memcpy. LVGL renders in RGB565_SWAPPED, so the bytes are already panel order. */
    owf_fb_t *bb = NULL;
    for (uint8_t i = 0; i < OWF_BOUNCE_NUM; i++) {
      if (s_owf_bounce[i] && (uint8_t *)s_owf_bounce[i]->frame == px_map) { bb = s_owf_bounce[i]; break; }
    }
    if (bb) {
      /* Exact-rect window. width/height are END+1 (the SDK computes set_window(x,y,width-1,
       * height-1)), so width = x2+1, height = y2+1. len = the contiguous byte count. */
      bb->x_start = (uint16_t)area->x1;
      bb->y_start = (uint16_t)area->y1;
      bb->width   = (uint16_t)(area->x2 + 1);
      bb->height  = (uint16_t)(area->y2 + 1);
      bb->len     = npx * 2;
      /* Push ASYNC and return WITHOUT flush_ready. LVGL's 'flushing' flag stays set; free_cb
       * (DMA-done) calls lv_display_flush_ready to clear it. LVGL renders the next area into the
       * OTHER buffer meanwhile, and only blocks (wait_for_flushing) if it laps this DMA. */
#if OWF_T5_OWN_PANEL
      owf_t5_panel_flush(bb);            // our own async push (qspi task + tx_sem)
#else
      tdl_disp_dev_flush(s_owf_disp, bb);
#endif
      deferred = true;   // free_cb will call flush_ready when this buffer's DMA completes
#if OWF_LVGL_PROFILE
      s_dirty_px += npx;
      if (lv_display_flush_is_last(disp)) s_frames++;
#endif
    }
    /* if bb == NULL (px_map didn't match — shouldn't happen) deferred stays false and we
     * flush_ready below so LVGL isn't wedged. */
  }
#if OWF_LVGL_PROFILE
  s_cb_us += (uint32_t)micros() - t_in;
  if (s_win == 0) s_win = t_in;
  if ((uint32_t)micros() - s_win >= 2000000u) {     // ~2s window
    uint32_t span_ms = ((uint32_t)micros() - s_win) / 1000u;
    uint32_t fps     = span_ms ? s_frames * 1000u / span_ms : 0;
    uint32_t dirty   = s_frames ? s_dirty_px / s_frames : 0;
    uint32_t pct     = s_frames ? dirty * 100u / ((uint32_t)OWF_T5_LCD_W * OWF_T5_LCD_H) : 0;
    uint32_t render_pf = s_frames ? s_owf_render_us / s_frames : 0;   // us/frame rendering
    uint32_t wait_pf   = s_frames ? s_owf_wait_us   / s_frames : 0;   // us/frame blocked on QSPI
    Serial.print("[lvgl-perf] "); Serial.print(s_frames); Serial.print(" frm/2s (~");
    Serial.print(fps); Serial.print(" fps) | cb="); Serial.print(s_cb_us);
    Serial.print(" us/2s | dirty="); Serial.print(dirty); Serial.print(" px/frm (");
    Serial.print(pct); Serial.print("% of frame) | render="); Serial.print(render_pf);
    Serial.print(" us/frm | qspi_wait="); Serial.print(wait_pf); Serial.println(" us/frm");
    s_cb_us = s_dirty_px = s_frames = 0; s_win = (uint32_t)micros();
    s_owf_render_us = s_owf_wait_us = 0;
  }
#endif
  /* Only flush_ready here if we did NOT queue an async push. On the normal (deferred) path,
   * free_cb calls flush_ready when the DMA completes — calling it here too would let LVGL
   * redraw the buffer mid-DMA (tearing). */
  if (!deferred) lv_display_flush_ready(disp);
}

/* ---- Touch read: poll the CST92xx via the SDK; report to LVGL --------------- */
/* Set whenever the touch poll sees a finger. The loop reads+clears this (via
 * owf_tuya_take_touch_activity) to refresh its idle timer — undimming / staying awake on
 * touch. On the ESP boards a touch ISR sets the .ino's s_touch_activity; the Tuya touch is
 * POLLED here (no ISR), so without this the idle timer never reset on touch and the panel
 * stayed dim / slept even while you were tapping it. */
static volatile bool s_owf_touch_activity = false;
static inline bool owf_tuya_take_touch_activity(void) {
  bool a = s_owf_touch_activity; s_owf_touch_activity = false; return a;
}

static void owf_lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  TDL_TP_POS_T pt[1];
  uint8_t n = 0;
  if (s_owf_tp && tdl_tp_dev_read(s_owf_tp, 1, pt, &n) == OPRT_OK && n > 0) {
    data->point.x = pt[0].x;
    data->point.y = pt[0].y;
    data->state   = LV_INDEV_STATE_PRESSED;
    s_owf_touch_activity = true;             // finger seen -> user activity (undim / stay awake)
  } else {
    data->state   = LV_INDEV_STATE_RELEASED;
  }
}

/* ---- Bring-up: our LVGL + display + indev (NO vendor task) ------------------ */
static inline bool owf_lvgl_own_begin(void) {
  // PSRAM overclock: DEFERRED to first-paint by default (OWF_PSRAM_DEFER_TO_FIRST_DRAW). The app
  // is XIP'd from PSRAM, so flipping the divider at boot — while the panel/buffers/first frames
  // are still bringing up and the CPU is fetching code from PSRAM — races the divider edge and
  // freezes the whole device (the cold-boot green-line hang). So we DON'T flip here; we leave the
  // vendor's stock 80 MHz and let loop() call owf_tuya_psram_overclock_deferred() once after the
  // first frame, in a quiesced, SRAM-safe window. See owf_tuya_psram_freq.h for the full rationale.
  // (With OWF_PSRAM_DEFER_TO_FIRST_DRAW=0 this reverts to the old inline-at-boot flip.)
  #define OWF_PSRAM_STR_(x) #x
  #define OWF_PSRAM_STR(x)  OWF_PSRAM_STR_(x)
  if (owf_tuya_psram_overclock())
    Serial.println("[lvgl-own] PSRAM overclock applied at boot ("
                   OWF_PSRAM_STR(OWF_PSRAM_CLK) " @ " OWF_PSRAM_STR(OWF_PSRAM_VOLT) ")");
  else
    Serial.println("[lvgl-own] PSRAM left at stock 80MHz; reclock deferred to first paint");

#if OWF_T5_OWN_PANEL
  // OWN DISPLAY DRIVER: bring up the CO5300 ourselves (QSPI + reset + init seq + hardware
  // rotation) instead of the SDK's tdl_disp. The SDK display registration (from
  // owf_tuya_register_panel) stays UNUSED here — we never tdl_disp_dev_open it. Touch (CST92xx)
  // still comes from the SDK tdl_tp layer (registered by owf_tuya_register_panel).
  if (!owf_t5_panel_open()) { Serial.println("[lvgl-own] own panel open failed"); return false; }
#ifndef OWF_T5_PANEL_SELFTEST
#define OWF_T5_PANEL_SELFTEST 0
#endif
#if OWF_T5_PANEL_SELFTEST
  // M1 bring-up mode: push R/G/B bands forever (NO LVGL) so we can confirm QSPI framing, the
  // init sequence, hardware rotation (MADCTL) and the offset in isolation. Set back to 0 once
  // first light + orientation look right. Expect: 3 color bands top->bottom, full-bleed, upright.
  Serial.println("[own-pnl] SELFTEST mode — LVGL disabled");
  for (;;) { owf_t5_panel_selftest(32); delay(2000); }
#endif
  s_owf_tp = tdl_tp_find_dev((char *)OWF_T5_DISPLAY_NAME);
  if (s_owf_tp) tdl_tp_dev_open(s_owf_tp);
#else
  // The panel + touch were registered by owf_tuya_register_panel() (call it first).
  s_owf_disp = tdl_disp_find_dev((char *)OWF_T5_DISPLAY_NAME);
  s_owf_tp   = tdl_tp_find_dev((char *)OWF_T5_DISPLAY_NAME);
  if (!s_owf_disp) { Serial.println("[lvgl-own] display dev not found"); return false; }
  if (tdl_disp_dev_open(s_owf_disp) != OPRT_OK) { Serial.println("[lvgl-own] disp open failed"); return false; }
  if (s_owf_tp) tdl_tp_dev_open(s_owf_tp);   // touch optional for first light
#endif

  // Two PSRAM draw buffers, each sized for the largest LVGL partial area. ZERO-COPY: these ARE
  // LVGL's partial draw buffers (their .frame is handed to lv_display_set_buffers below), so
  // LVGL renders directly into the buffer we hand to the panel — no flush memcpy. Each carries
  // a free_cb that calls lv_display_flush_ready on DMA-done, which is how LVGL learns it may
  // reuse the buffer (it blocks in wait_for_flushing until then). No pool, no carry.
  for (uint8_t i = 0; i < OWF_BOUNCE_NUM; i++) {
#if OWF_T5_OWN_PANEL
    // OWN PANEL: allocate the bounce buffer ourselves (PSRAM + write-through read cache applied
    // inside owf_t5_panel_create_fb). No SDK frame-buff, no fmt field.
    s_owf_bounce[i] = owf_t5_panel_create_fb(OWF_BOUNCE_MAX_PX * 2);
    if (!s_owf_bounce[i]) { Serial.println("[lvgl-own] draw-buf alloc failed"); return false; }
    s_owf_bounce[i]->free_cb  = owf_bounce_free_cb;
    s_owf_bounce[i]->free_arg = (void *)(uintptr_t)i;
#else
#if OWF_DRAWBUF_IN_SRAM
    // Internal SRAM: single-cycle read AND write (the render bottleneck). The SDK flush path does
    // no cache maintenance for either RAM type, so SRAM buffers are DMA-coherent as-is — and the
    // PSRAM write-through helper below is NOT applied (it operates on PSRAM addresses only).
    s_owf_bounce[i] = tdl_disp_create_frame_buff(DISP_FB_TP_SRAM, OWF_BOUNCE_MAX_PX * 2);
#else
    s_owf_bounce[i] = tdl_disp_create_frame_buff(DISP_FB_TP_PSRAM, OWF_BOUNCE_MAX_PX * 2);
#endif
    if (!s_owf_bounce[i]) { Serial.println("[lvgl-own] draw-buf alloc failed"); return false; }
    s_owf_bounce[i]->fmt      = TUYA_PIXEL_FMT_RGB565;
    s_owf_bounce[i]->free_cb  = owf_bounce_free_cb;
    s_owf_bounce[i]->free_arg = (void *)(uintptr_t)i;
#if !OWF_DRAWBUF_IN_SRAM
    // PSRAM buffers only: mark cacheable WRITE-THROUGH so the SW renderer's per-pixel reads hit
    // cache instead of raw PSRAM, while writes still flow straight to PSRAM (QSPI DMA stays
    // coherent). One HW channel per buffer; see owf_tuya_psram_freq.h. (SRAM needs none of this —
    // it's already single-cycle and coherent, which is why SRAM is the bigger win.)
    if (owf_tuya_psram_cache_buffer(s_owf_bounce[i]->frame, OWF_BOUNCE_MAX_PX * 2))
      Serial.println("[lvgl-own] draw buf write-through cache enabled");
#endif
#endif  /* OWF_T5_OWN_PANEL */
  }

  lv_init();
  lv_tick_set_cb(owf_lvgl_tick_cb);          // v9: LVGL reads time via this cb

  s_owf_lv_disp = lv_display_create(OWF_T5_LCD_W, OWF_T5_LCD_H);
  if (!s_owf_lv_disp) { Serial.println("[lvgl-own] lv_display_create failed"); return false; }
  // RENDER IN PANEL BYTE ORDER (RGB565_SWAPPED): LVGL's blend loops emit pixels already
  // byte-swapped to the CO5300's wire order, so the buffer LVGL renders is wire-ready and we
  // push it as-is (zero-copy) — the swap happens for free inside the blend while pixels are in
  // registers. (Was plain RGB565 needing a swap pass when the DMA2D draw unit required normal
  // byte order; DMA2D is disabled now, so that constraint is gone.) Same approach as the
  // ESP32-S3 path's FLUSH_RENDER_SWAPPED. Requires LV_DRAW_SW_SUPPORT_RGB565_SWAPPED (enabled).
  lv_display_set_color_format(s_owf_lv_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_flush_cb(s_owf_lv_disp, owf_lvgl_flush_cb);
  // CO5300 needs even-aligned draw windows; snap every invalidated area outward to even
  // bounds before render (fixes the italic/warp shear on buttons/slider/%). Same as the S3.
  lv_display_add_event_cb(s_owf_lv_disp, owf_lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#if OWF_LVGL_PROFILE
  // Render-vs-QSPI-wait split (tells us if fps is compute-bound or bus-bound).
  lv_display_add_event_cb(s_owf_lv_disp, owf_lvgl_profile_event_cb, LV_EVENT_RENDER_START, NULL);
  lv_display_add_event_cb(s_owf_lv_disp, owf_lvgl_profile_event_cb, LV_EVENT_RENDER_READY, NULL);
  lv_display_add_event_cb(s_owf_lv_disp, owf_lvgl_profile_event_cb, LV_EVENT_FLUSH_WAIT_START, NULL);
  lv_display_add_event_cb(s_owf_lv_disp, owf_lvgl_profile_event_cb, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
#endif

  // PARTIAL mode, ZERO-COPY: hand LVGL the two frame buffers' .frame as its draw buffers, so
  // it renders straight into the buffer we push to the panel (no flush memcpy). flush_cb
  // matches px_map back to its struct; free_cb defers flush_ready until DMA-done so LVGL never
  // redraws a buffer mid-push.
  const uint32_t buf_bytes = OWF_BOUNCE_MAX_PX * 2;
  lv_display_set_buffers(s_owf_lv_disp, s_owf_bounce[0]->frame, s_owf_bounce[1]->frame,
                         buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  // Touch indev (pointer).
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, owf_lvgl_touch_cb);
  lv_indev_set_display(indev, s_owf_lv_disp);

  Serial.println("[lvgl-own] LVGL v9.5 up (own display + indev, per-area dirty-rect push)");
  return true;
}

/* Run LVGL once per loop iteration (replaces the vendor handler task). */
static inline void owf_lvgl_own_handler(void) {
  lv_timer_handler();
}

#endif /* BOARD_PLATFORM_TUYA */
