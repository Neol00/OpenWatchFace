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

/* Scale factor as a percent (e.g. 100 = 1.0x on the S3, ~42 on the C6's 172px).
 * A board can pin an EXPLICIT factor with BOARD_UI_SCALE_PCT instead of the
 * width-derived one — used by the round 240x240 (width says 59%, but the panel
 * is only 48% of the reference HEIGHT, so the width factor overflows vertically;
 * a flat 50% fits both axes on the round bezel). */
static inline int ui_scale_pct(void) {
#ifdef BOARD_UI_SCALE_PCT
  return BOARD_UI_SCALE_PCT;
#else
  return (int)((screenWidth * 100u) / UI_REF_W);
#endif
}

/* Map a reference-pixel value to this screen. Rounds to nearest. Signed so it
 * works for offsets/insets that can be negative (e.g. corner-tray -56). */
static inline int ui_px(int ref_px) {
  return (ref_px * ui_scale_pct() + (ref_px >= 0 ? 50 : -50)) / 100;
}

#define UI_PX(n) (ui_px((n)))

/* ---- container width for the app screens ---------------------------------
 * Most app screens set their scroll column to a FIXED width authored for the
 * 410-wide reference (374 / 372 / 360 / 330 px). Those numbers are correct on a
 * panel near the reference width, but on a mid-size one they simply overhang:
 * 374 px of container on the S3-1.64's 280 px panel is 94 px of overflow, which
 * is why every app clipped at the sides there.
 *
 * UI_COL_W(ref) returns the width to actually use:
 *   - reference-class panels (S3-2.06, S3-1.8): the ORIGINAL fixed value, so
 *     those boards are byte-for-byte unchanged — important, because several of
 *     these containers hold rows with fixed pixel offsets tuned to that width;
 *   - sub-reference panels (S3-1.64, S3-Touch-LCD-2): a percentage of the real
 *     screen instead, so the column always fits with a small side margin.
 *
 * Gated on BOARD_SCREEN_SUBREF ("is the panel narrower than the reference"), NOT
 * on a launcher-grid tier. Those are different questions and coupling them is a
 * trap: a panel can be wide enough for a 3-column app grid while still being far
 * too narrow for a 374 px container (the 240-wide S3-Touch-LCD-2 is exactly that),
 * so keying this off the grid tier made a grid change silently re-break every app
 * screen's width. See the BOARD_SCREEN_SUBREF comment in board.h.
 *
 * Pass the ORIGINAL literal as `ref` and the call site keeps documenting what the
 * 2.06 layout was: lv_obj_set_width(col, UI_COL_W(374)). */
#if BOARD_SCREEN_SUBREF
#define UI_COL_W(ref) (LV_PCT(94))
#elif BOARD_SCREEN_ROUND_SMALL
/* SMALL ROUND face: every `ref` passed here (374 / 372 / 360 / 330) was authored
 * on the 410-wide panel, and the first three are AT OR WIDER THAN a 360 px screen.
 * BOARD_SCREEN_SUBREF is (width < 340), so this panel misses that branch by 20 px
 * and was handed the raw literal — a 374 px container centered on 360 px of glass
 * hangs 7 px off each side, and a 360 px one leaves no margin at all on a face
 * whose corners are cut off anyway. A percentage of the parent keeps the container
 * inside the circle wherever it is used. */
#define UI_COL_W(ref) (LV_PCT(86))
#else
#define UI_COL_W(ref) (ref)
#endif

/* The app column's CONTENT width in real pixels — what ui_app_column_layout()
 * above actually produces, minus its padding.
 *
 * This exists because a percentage is NOT usable everywhere: a flex ROW_WRAP child
 * with LV_SIZE_CONTENT height sized by LV_PCT never establishes a real wrap
 * boundary (LVGL sizes it from its own wide children instead), so such a container
 * needs an EXPLICIT pixel width or its items march off to the right in a
 * horizontal scroll rather than wrapping. The Power app's CPU-speed ladder is
 * exactly that case and carries a long comment saying so.
 *
 * Keep this in lockstep with ui_app_column_layout(); it is the same arithmetic. */
static inline int ui_app_column_content_w(void) {
#if BOARD_SCREEN_ROUND_SMALL
  return (int)((screenWidth * 86u) / 100u) - 2 * UI_PX(6);
#else
  return 374 - 2 * 6;
#endif
}

/* ---- the app screens' main scroll column ---------------------------------
 * Power, Weather and Appearance all opened with the SAME four lines: a fixed
 * UI_COL_W(374) width, a height of screenHeight-84-10, a top align of 84 and a
 * 6 px pad. Those are RAW pixels authored on the 410x502 reference, and on a
 * small ROUND face every one of them is wrong:
 *
 *   - width 374 on a 360 px panel is WIDER THAN THE SCREEN. Centered, the column
 *     runs from x=-7 to x=367, so the first and last 7 px of every row are simply
 *     off-glass — the reported "the Battery header clips out the side". This was
 *     invisible to the tier system because BOARD_SCREEN_SUBREF is (width < 340),
 *     so a 360-wide panel reads as reference-class and UI_COL_W hands back the
 *     literal 374.
 *   - top 84 / bottom 10 are raw px too: the column ran to y=350 on a 360 px
 *     circle, where the glass is only ~120 px wide.
 *
 * On a round face the usable width is a CHORD, not the panel width, and the
 * chord narrows toward both poles. LV_PCT(86) (310 px on the C2) leaves 25 px of
 * margin each side, which stays inside the circle across the whole y-range the
 * column actually occupies, and the taller bottom margin keeps the last row out
 * of the narrow bottom arc.
 *
 * Every other board takes the ORIGINAL four lines unchanged, so this is a no-op
 * on the S3 family and on the two large round boards.
 *
 * NB the same 374-on-a-454-panel arithmetic is a LATENT squeeze on the Fossil
 * Gen 4 (374 is 82% of 454, and the chord at the column's top edge is ~346 px).
 * It has not been reported there, so it is deliberately left alone rather than
 * changed blind on an accepted board. */
static inline void ui_app_column_layout(lv_obj_t *col, int ref_w) {
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_set_width(col, LV_PCT(86));
  lv_obj_set_height(col, (int)screenHeight - UI_PX(84) - UI_PX(36));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(84));
  lv_obj_set_style_pad_all(col, UI_PX(6), 0);
  (void)ref_w;
#else
  lv_obj_set_width(col, UI_COL_W(ref_w));
  lv_obj_set_height(col, (int)screenHeight - 84 - 10);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 84);
  lv_obj_set_style_pad_all(col, 6, 0);
#endif
}

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
