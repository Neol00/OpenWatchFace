/* ============================================================================
 *  ui_scale.h — resolution-independent UI sizing.
 *
 *  The whole UI was hand-laid-out in PIXELS for the original S3-2.06 panel
 *  (410x502). Rather than re-tune every screen per board, those numbers are kept
 *  AS THE REFERENCE and scaled to the active panel at runtime:
 *
 *      UI_PX(n)  -> n pixels on the 410-wide reference, scaled to this screen.
 *
 *  Scale is by WIDTH (screenWidth / UI_REF_W): the watch face and menus are
 *  width-driven, and width-fit is what stops the "zoomed in / overflowing"
 *  symptom on a narrower panel. The same factor scales vertical spacing too, so
 *  proportions are preserved; aspect-ratio-specific tweaks (e.g. the C6's taller
 *  portrait) are handled by per-board overrides on individual screens later.
 *
 *  On the S3-2.06 the factor is exactly 1.0, so every UI_PX(n) == n and the
 *  layout is byte-for-byte unchanged. Only smaller/larger panels are remapped.
 *
 *  Integer math (percent) to avoid floats in the hot UI path. screenWidth is the
 *  global set from gfx->width() in setup(), so UI_PX is only valid AFTER the
 *  display is initialized (all UI construction is — it runs in/after setup()).
 *
 *  Header-only; include EARLY (before ui_fonts.h and any screen). No deps beyond
 *  the screenWidth global declared in the .ino.
 * ========================================================================== */
#pragma once
#include <stdint.h>

/* Reference design width — the panel the UI pixel values were authored against. */
#define UI_REF_W 410

/* NOTE: this header is part of the single .ino translation unit and relies on the
 * `screenWidth` global (static uint32_t in the .ino) already being declared — so
 * it MUST be included AFTER that declaration. It deliberately does NOT re-declare
 * screenWidth (an `extern` would clash with the file-static definition). */

/* Scale factor as a percent (e.g. 100 = 1.0x on the S3, ~42 on the C6's 172px). */
static inline int ui_scale_pct(void) {
  return (int)((screenWidth * 100u) / UI_REF_W);
}

/* Map a reference-pixel value to this screen. Rounds to nearest. Signed so it
 * works for offsets/insets that can be negative (e.g. corner-tray -56). */
static inline int ui_px(int ref_px) {
  return (ref_px * ui_scale_pct() + (ref_px >= 0 ? 50 : -50)) / 100;
}

#define UI_PX(n) (ui_px((n)))

/* ---- narrow-panel scrollbar nudge ----------------------------------------
 * On a slim portrait panel (BOARD_SCREEN_NARROW, e.g. the C6-1.47) the scroll
 * containers are centered at LV_PCT(92), so the default scrollbar drew ~25 px in
 * from the screen edge and crowded the content (it overlapped widgets in the
 * Power list). This shared style nudges the scrollbar toward the right edge.
 * Apply it to a scroll container with ui_apply_scrollbar_nudge(obj).
 *
 * On WIDE panels (S3-2.06) both functions are no-ops, so the S3 is untouched and
 * call sites don't need their own #if. Init once in setup() AFTER the display is
 * up (it depends on the screen width via BOARD_SCREEN_NARROW being meaningful). */
/* On a slim portrait panel (BOARD_SCREEN_NARROW, e.g. the C6-1.47) the scroll
 * containers are centered at LV_PCT(92), so their RIGHT EDGE sits ~4% in from the
 * screen edge — and LVGL draws the vertical scrollbar at that edge, leaving it
 * stranded over the content instead of hugging the screen edge. The robust fix is
 * to make the scroll container span the FULL screen width on narrow panels and
 * inset its content with padding instead: then the scrollbar lands at the true
 * right edge, clear of the content. ui_apply_scrollbar_nudge() does exactly that
 * for a container. On wide panels (S3-2.06) it's a no-op, so the S3 is untouched. */
#if BOARD_SCREEN_NARROW
static inline void ui_apply_scrollbar_nudge(lv_obj_t *obj) {
  lv_obj_set_width(obj, LV_PCT(100));                 // span the full screen width
  lv_obj_set_style_pad_left(obj,  UI_PX(16), 0);      // content inset on the left
  lv_obj_set_style_pad_right(obj, UI_PX(22), 0);      // wider RIGHT inset = a lane for the scrollbar
  // LVGL draws the scrollbar at the container's right edge MINUS pad_right of the
  // SCROLLBAR part. The container's right edge is now the true screen edge (full
  // width), so a small pad_right here parks the bar in the right inset lane —
  // clear of the content, hugging the screen edge instead of overlapping widgets.
  lv_obj_set_style_pad_right(obj, UI_PX(2), LV_PART_SCROLLBAR);
  lv_obj_set_style_width(obj,     UI_PX(6), LV_PART_SCROLLBAR);
}
#else
static inline void ui_apply_scrollbar_nudge(lv_obj_t *)     {}
#endif
static inline void ui_scrollbar_style_init(void)            {}  /* nothing to pre-build now */
