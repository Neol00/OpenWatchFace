/* ============================================================================
 *  screen_cache.h — PSRAM pre-render cache for instant screen open.
 *
 *  THE IDEA: the app screens are mostly STATIC layout (a grid of tiles, a spec
 *  sheet, a settings list). Re-laying-out + re-rasterizing all of that every time
 *  you open a screen is wasted CPU. So we take ONE snapshot of a fully-rendered
 *  screen into a PSRAM buffer (lv_snapshot_take_to_buf), and on the NEXT open we
 *  blit that bitmap straight to the panel — the screen APPEARS instantly, with no
 *  layout/raster delay — then let LVGL render the real (possibly live-valued)
 *  content over it on the following tick. "Cache-then-render": instant first
 *  paint, live values correct themselves a frame later.
 *
 *  WHY PSRAM: each full-screen RGB565 snapshot is screenW*screenH*2 ≈ 412 KB —
 *  far too big for the ~17 KB of internal SRAM left once BLE is up. We OWN the
 *  buffer (heap_caps_malloc MALLOC_CAP_SPIRAM) and hand it to the _to_buf snapshot
 *  variant, so the cache is guaranteed to live in the empty 8 MB PSRAM and never
 *  competes with the render line-buffers / BLE / stacks in fast SRAM. Buffers are
 *  allocated LAZILY (on first capture of each slot) so PSRAM is only spent on
 *  screens you actually open.
 *
 *  FORMAT: LVGL renders RGB565 native (LV_COLOR_DEPTH 16, no swap on this build),
 *  which is exactly what gfx->draw16bitRGBBitmap expects — so the snapshot blits
 *  with the same call the normal flush uses, no byte-swap.
 *
 *  STALENESS: a snapshot is a photograph — only valid if the screen looks the same
 *  next open. The launcher GRID is truly static (re-capture only on accent
 *  restyle). Screens with live values (About's free-mem, lists, etc.) use
 *  cache-then-render so a stale value shows for at most one frame before the live
 *  render paints over it. TRULY animated screens (Timer counting) gain nothing and
 *  simply shouldn't be cached.
 *
 *  Header-only, in the .ino TU. INCLUDE AFTER the .ino's gfx + screenWidth/Height
 *  globals and BEFORE app_menu.h (which calls into it). Requires LV_USE_SNAPSHOT 1
 *  (patched in lv_conf.h).
 * ========================================================================== */
#pragma once
#include <lvgl.h>
#include "esp_heap_caps.h"

/* One cache slot per app + the launcher grid. Keep in sync with the call sites;
 * unused slots cost nothing (lazy alloc). */
enum {
  SC_SLOT_MENU = 0,     // (unused) the launcher is now a live swipe pager, not cached
                        // — kept so the other slot numbers don't shift
  SC_SLOT_ABOUT,
  SC_SLOT_POWER,
  SC_SLOT_WIFI_BLE,
  SC_SLOT_PLAYER,
  SC_SLOT_NOTIFS,
  SC_SLOT_FILES,
  SC_SLOT_APPEARANCE,
  SC_SLOT_FIND_PHONE,
  SC_SLOT_COUNT
};

typedef struct {
  uint8_t      *buf;     // PSRAM RGB565 bitmap (screenW*screenH*2), or NULL if not yet captured
  bool          valid;   // a successful capture is present and current
} sc_slot_t;

static sc_slot_t s_sc[SC_SLOT_COUNT];

/* Bytes for one full-screen RGB565 snapshot. screenWidth/Height are the .ino
 * globals (set in setup before any screen opens). */
static inline size_t sc_buf_bytes(void) {
  return (size_t)screenWidth * (size_t)screenHeight * 2;
}

/* True if slot has a current cached frame ready to blit. */
static inline bool screen_cache_valid(uint8_t slot) {
  return slot < SC_SLOT_COUNT && s_sc[slot].valid && s_sc[slot].buf != nullptr;
}

/* Mark a slot stale (e.g. accent restyle changed how everything looks). The buffer
 * is kept (re-used by the next capture); only the valid flag drops. */
static inline void screen_cache_invalidate(uint8_t slot) {
  if (slot < SC_SLOT_COUNT) s_sc[slot].valid = false;
}
static inline void screen_cache_invalidate_all(void) {
  for (uint8_t i = 0; i < SC_SLOT_COUNT; i++) s_sc[i].valid = false;
}

/* Blit a cached frame straight to the panel — the instant-appearance path. No-op
 * (returns false) if the slot isn't cached, so callers can fall back to a normal
 * render. Pushes the whole screen via the same call the flush path uses. */
static inline bool screen_cache_blit(uint8_t slot) {
  if (!screen_cache_valid(slot)) return false;
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)s_sc[slot].buf, screenWidth, screenHeight);
  return true;
}

/* Capture a fully-rendered screen object into the slot's PSRAM buffer. Allocates
 * the buffer lazily on first use. Call this AFTER the screen has been built +
 * rendered (so the snapshot reflects the final look), at the END of an open path —
 * the snapshot itself rasterizes the object, so doing it after the screen is shown
 * keeps that cost off the visible open frame. Safe to call on every open; it just
 * refreshes the cache for next time. */
static inline void screen_cache_capture(uint8_t slot, lv_obj_t *scr) {
  if (slot >= SC_SLOT_COUNT || scr == nullptr) return;

  // Lazy-allocate this slot's PSRAM buffer on first capture.
  if (!s_sc[slot].buf) {
    s_sc[slot].buf = (uint8_t *)heap_caps_malloc(sc_buf_bytes(), MALLOC_CAP_SPIRAM);
    if (!s_sc[slot].buf) {
      USBSerial.printf("[scache] slot %u: PSRAM alloc of %u KB failed — caching disabled for it\n",
                       (unsigned)slot, (unsigned)(sc_buf_bytes() / 1024));
      return;   // out of PSRAM: leave the slot uncached, callers just render normally
    }
  }

  // Wrap our PSRAM buffer in a draw-buffer descriptor and let LVGL render `scr` into
  // it. lv_snapshot_take_to_draw_buf reshapes the descriptor (w/h/stride) to the
  // object's size over OUR data pointer — it does NOT reallocate when the buffer is
  // big enough, so the snapshot stays in our PSRAM buffer. Init it full-screen-sized
  // (screenWidth x screenHeight) in RGB565, which matches the panel + the blit call
  // above (no byte-swap). The non-deprecated API (vs lv_snapshot_take_to_buf).
  lv_draw_buf_t draw_buf;
  lv_draw_buf_init(&draw_buf, screenWidth, screenHeight, LV_COLOR_FORMAT_RGB565,
                   LV_STRIDE_AUTO, s_sc[slot].buf, sc_buf_bytes());
  lv_result_t r = lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &draw_buf);
  s_sc[slot].valid = (r == LV_RESULT_OK);
  if (r != LV_RESULT_OK)
    USBSerial.printf("[scache] slot %u: snapshot failed (rc=%d)\n", (unsigned)slot, (int)r);
}
