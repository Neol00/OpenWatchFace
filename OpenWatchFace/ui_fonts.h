/* ============================================================================
 *  ui_fonts.h — the shared FONT_* aliases used across the whole UI layer.
 *
 *  Header-only, compiled into the .ino TU. INCLUDE EARLY — BEFORE the app_*.h
 *  screens, quick_shade.h, watchface.h and every other module that draws text:
 *  they all reference these FONT_* names, so the aliases must exist first.
 *
 *  Only the big clock face is a custom-generated glyph set; the rest map onto
 *  the built-in Montserrat sizes already enabled in lv_conf.h.
 * ========================================================================== */
#pragma once
#include <lvgl.h>
#include "board.h"   // pick a per-board font tier (large vs small panel)

/* The FONT_* aliases are PER-BOARD. The UI was authored for the S3-2.06's big
 * 410x502 panel, so its fonts are large; a smaller panel (C6-1.47, 172x320)
 * needs a smaller tier or text overflows / looks "zoomed in". Layout DIMENSIONS
 * scale continuously via UI_PX() (ui_scale.h), but LVGL fonts are pre-rendered at
 * fixed sizes, so each board picks the nearest stock Montserrat sizes.
 *
 * Big clock font: Montserrat SemiBold 110px, generated as a REAL LVGL font (no
 * runtime scaling — that crashed the CO5300 render path). Restricted glyph range
 * 0x30-0x3A (digits + colon only) keeps the .c tiny despite the size. Generate at
 * lvgl.io/tools/fontconverter:  Name=montserrat_clock_110, Size=110, Bpp=4,
 * Range=0x30-0x3A, font=Montserrat-SemiBold.ttf — drop the .c in this folder.
 *
 * The custom clock glyph exists ONLY at 110px (digits + colon, no letters) and
 * can't shrink, so a small panel uses a stock Montserrat size for the clock
 * instead. FONT_TIME_IS_CUSTOM tells the watch face whether the clock font has
 * only digits (custom) or a full glyph set (stock). */

/* Gate on the SCREEN, not PSRAM: the font tier is a function of panel SIZE, and a
 * board can have PSRAM AND a small panel (the S3-1.47: PSRAM, but a 172x320 panel
 * like the C6). Keying off BOARD_SCREEN_NARROW gives every slim portrait panel the
 * small tier regardless of chip/memory. (Was BOARD_HAS_PSRAM, which only worked
 * while the 2.06 was the lone PSRAM board.) */
#if BOARD_DISPLAY_GC9A01_SPI
/* GC9A01 240x240 (the S3 Super Mini DIY build) — gate on the DISPLAY, not on
 * BOARD_SCREEN_ROUND: the T5-E1's AMOLED is round too but full-size, so
 * roundness says nothing about the font tier. This panel is ~half the 410-wide
 * reference, so each font is "a little over half" its reference size (110->48
 * clock, 28->16, 20->12, 24->14). The clock uses the LARGEST stock Montserrat
 * (48; built-in fonts stop there — no custom generated glyph set on this
 * board), full glyph range -> FONT_TIME_IS_CUSTOM 0. */
#define FONT_TIME  lv_font_montserrat_48    // HH:MM (stock font; full glyph set)
#define FONT_TIME_IS_CUSTOM 0
/* Text roles via UI_FONT (240 wide -> 16/12/12/14, identical to the old hand set). */
#elif BOARD_SCREEN_LOWRES_DIAL
/* MID-RESOLUTION panel (S3-Touch-LCD-2, 240x320): too wide to be "narrow" — it
 * keeps the full 3-column launcher and the reference layouts — but every font here
 * was authored against the 410-wide 2.06, and this panel is 41% narrower. Left on
 * the large tier the text is proportionally oversized: layout DIMENSIONS scale
 * continuously through UI_PX(), but a pre-rendered font does not scale with them,
 * so the gap shows up as text that crowds its own boxes.
 *
 * THE WHOLE TIER IS SCALED BY PANEL WIDTH: 240/410 = 0.585, applied to each 2.06
 * size and rounded to an enabled stock Montserrat (lv_conf.h has 8..48 built in):
 *
 *     FONT_LABEL       28 -> 16.4 -> 16
 *     FONT_TOP         24 -> 14.0 -> 14
 *     FONT_SMALL       20 -> 11.7 -> 12
 *     FONT_ABOUT_BODY  20 -> 11.7 -> 12
 *
 * Sanity check: that is the SAME set the 240x240 GC9A01 tier above arrived at
 * independently — two 240-wide panels landing on identical sizes is the expected
 * answer, not a coincidence. 12 is also the practical floor for body text on these
 * panels (the slim C6 tier bottoms out there too).
 *
 * The dial is a 80 px cut of the SAME custom clock glyph (montserrat_clock_80.c —
 * digits + colon, 4bpp, same generator settings as the 110, just Size=80).
 * FONT_TIME_IS_CUSTOM stays 1: same restricted glyph set, so the watch face's
 * "digits only" handling is unchanged.
 *
 * SIZE THE DIAL BY PHYSICAL mm, NOT BY A FRACTION OF THE PANEL. This panel is
 * 240x320 in ~2", so its pixel pitch is ~0.127 mm — about 57% COARSER than the
 * AMOLEDs (~0.079 mm). A pixel size therefore renders MUCH larger here than the
 * same number does on a dense panel, and any "dial px / panel width" ratio is
 * measuring how much empty MARGIN a panel has, not how big the clock looks:
 *
 *     board      pitch mm   dial px   dial mm   vs 2.06
 *     S3-2.06    0.0807     110       8.88      ref
 *     S3-1.8     0.0789     110       8.67      -2%
 *     S3-1.64    0.0778     110       8.56      -4%
 *     LCD-2 @88  0.1270      88      11.18     +26%
 *     LCD-2 @80  0.1270      80      10.16     +14%
 *     LCD-2 @72  0.1270      72       9.14      +3%   
 *
 * The three AMOLEDs sit within 4% of each other physically even though their
 * width-ratios differ hugely — which is why the ratio metric is the wrong one.
 * 80 keeps the clock slightly larger than the AMOLED family on purpose: this is a
 * physically small, coarse panel, and a little extra size reads better at a glance.
 * Drop to a 72 px cut for near-exact parity; it is one filename + one alias here.
 *
 * Gate is BOARD_SCREEN_LOWRES_DIAL (board.h) — a RESOLUTION test, not a board id,
 * so any future panel of about this size picks the whole tier up automatically.
 * Placed ABOVE the large-panel branch because both would match. */
LV_FONT_DECLARE(montserrat_clock_72);
#define FONT_TIME  montserrat_clock_72      // large HH:MM (custom: digits+colon only)
#define FONT_TIME_IS_CUSTOM 1
/* Text roles via UI_FONT (240 wide -> 16/12/12/14, identical to the old hand set). */
#elif BOARD_SCREEN_ROUND_SMALL
/* SMALL ROUND face (TicWatch C2 / S2, 360x360): wide enough to keep the reference
 * text tier (360 >= the 280 anchor, and the pitch is dense — ~0.075 mm on a 1.3"
 * 360 panel, i.e. AMOLED-class), but only 360 px TALL against a 502 px reference.
 * The DIAL is the one element where that matters, because it is sized in absolute
 * pixels and sits between two fixed rows.
 *
 * Pick the cut by the dial's share of the panel WIDTH, which is what actually
 * reads as "how big is the clock" on a round face (the mm argument in the
 * LOWRES_DIAL tier above is about a coarse LCD; these panels are dense, so the
 * ratio is the right metric here):
 *
 *     board          panel w   cut   "12:34" px   share
 *     Fossil Gen 4     454     110      ~297       65%
 *     T5-1.75          466     110      ~297       64%
 *     C2 @110          360     110      ~297       82%   <- overflows, clipped
 *     C2 @88           360      88      ~238       66%   <- matches the family
 *     C2 @80           360      80      ~216       60%
 *
 * 88 lands the C2 within a point of the two accepted round boards, so the clock
 * looks the same SIZE on the wrist across the family. Dropping to 80 is one
 * filename + one alias here if it still reads large.
 *
 * The glyph set is identical (digits + colon, 4bpp), so FONT_TIME_IS_CUSTOM stays
 * 1 and the watch face's digits-only handling is unchanged.
 *
 * NB the font alone did NOT fix the reported clipping: montserrat_clock_110 has a
 * line_height of 79 px (digits+colon only — no ascenders/descenders), so at 88 it
 * is 61 px, and the top stat row still reached into it. The vertical anchors in
 * watchface.h move too; see the ROUND_SMALL branch there. */
LV_FONT_DECLARE(montserrat_clock_88);
#define FONT_TIME  montserrat_clock_88      // large HH:MM (custom: digits+colon only)
#define FONT_TIME_IS_CUSTOM 1
/* Text roles via UI_FONT: reference sizes (360 wide is above the 280 anchor). */
#elif !BOARD_SCREEN_NARROW
/* S3-2.06 (large panel): the original clock glyph. */
LV_FONT_DECLARE(montserrat_clock_110);
#define FONT_TIME  montserrat_clock_110     // enormous HH:MM (custom: digits+colon only)
#define FONT_TIME_IS_CUSTOM 1
/* Text roles via UI_FONT: identity for this whole branch — the 280-wide 1.64,
 * 368-wide 1.8, 410 reference and 466 T5 are ALL accepted at the reference
 * sizes (dense AMOLED pitch; see the learned-anchor note in ui_px_scale). */
#else
/* Slim portrait panel (C6-1.47 / S3-1.47, 172x320): stock Montserrat tier,
 * HAND-TUNED (deliberately richer than the width ratio — see UI_FONT above). */
#define FONT_TIME  lv_font_montserrat_48    // HH:MM (stock font; full glyph set)
#define FONT_TIME_IS_CUSTOM 0
#define FONT_LABEL lv_font_montserrat_12
#define FONT_SMALL lv_font_montserrat_12
#define FONT_ABOUT_BODY lv_font_montserrat_12
#define FONT_TOP   lv_font_montserrat_10
#endif

/* The four text ROLES, authored on the 410-wide reference and auto-scaled by
 * UI_FONT everywhere except the hand-tuned narrow tier above (which defined
 * them already). One definition for every non-narrow board is exactly what
 * keeps a role visually identical across all screens of a device. */
#ifndef FONT_LABEL
#define FONT_LABEL      UI_FONT(28)   // headings / app titles
#define FONT_SMALL      UI_FONT(20)   // body + button text
#define FONT_ABOUT_BODY UI_FONT(20)   // About screen body (== FONT_SMALL)
#define FONT_TOP        UI_FONT(24)   // watch-face top-row values (%, date, day)
#endif

/* ---- Automatic font scaling (UI_FONT) --------------------------------------
 * THE central rule for text size on the non-narrow boards: every piece of text
 * names the size it was AUTHORED at on the 410-wide S3-2.06 reference, and
 * UI_FONT(ref_px) rescales that through the LEARNED device curve (ui_px_scale —
 * anchored on the hand-accepted boards, see its comment) and snaps to the
 * nearest stock Montserrat (8..48 — all of them are enabled in lv_conf.h). One
 * formula, applied everywhere, is what keeps a ROLE the same size across every
 * screen of a device: two labels authored at 20 land on the same real font on
 * every board, instead of each site hand-picking its own shrink.
 *
 *     UI_FONT(20) -> 280..466 wide: 20   240x320: 12   240x280: 12
 *
 * The FONT_* role aliases below are defined THROUGH this, so the ~180 existing
 * role-based call sites scale automatically; sites with a one-off authored size
 * call UI_FONT(n) directly instead of naming lv_font_montserrat_n.
 *
 * WHAT IT DELIBERATELY DOES NOT TOUCH:
 *   - NARROW boards (172-wide C6/S3-1.47): their tier was hand-tuned site by
 *     site (the pure width ratio lands at illegibly small sizes there, and the
 *     hand values deviate from any single formula on purpose). On narrow builds
 *     UI_FONT is the identity and the existing #if BOARD_SCREEN_NARROW hand
 *     branches keep doing the work.
 *   - FONT_TIME: the big clock is a custom pre-rendered glyph set with fixed
 *     cuts (110/80/72), picked per tier below — a stock ladder can't replace it.
 *   - The app-menu tile ICONS (montserrat_34 + icons34): explicitly kept at 34
 *     on every board — reported as looking right; do not fold into UI_FONT.
 *
 * Expands to an lvalue (*fn()), so `&UI_FONT(20)` yields the lv_font_t* that
 * every LVGL call wants — same usage shape as `&lv_font_montserrat_20`. Runtime
 * cost is a few integer ops per call at screen-BUILD time only. */
static inline const lv_font_t *ui_font_nearest(int px) {
  static const lv_font_t *const ladder[] = {
    &lv_font_montserrat_8,  &lv_font_montserrat_10, &lv_font_montserrat_12,
    &lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_18,
    &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
    &lv_font_montserrat_26, &lv_font_montserrat_28, &lv_font_montserrat_30,
    &lv_font_montserrat_32, &lv_font_montserrat_34, &lv_font_montserrat_36,
    &lv_font_montserrat_38, &lv_font_montserrat_40, &lv_font_montserrat_42,
    &lv_font_montserrat_44, &lv_font_montserrat_46, &lv_font_montserrat_48,
  };
  int snapped = ((px + 1) / 2) * 2;              // nearest even (ties round UP)
  if (snapped < 8)  snapped = 8;
  if (snapped > 48) snapped = 48;
  return ladder[(snapped - 8) / 2];
}
#define UI_FONT_REF_W 410                        /* the S3-2.06 authoring width */
#define UI_FONT_REF_H 502                        /* ...and height */
/* The ONE scaling rule, shared by text (UI_FONT) and icon groups (UI_ICON_*).
 *
 * THE CURVE IS LEARNED FROM THE HAND-ACCEPTED DEVICES — it is NOT a width ratio
 * off the single 410 reference. A pure ratio was tried and it REGRESSED every
 * board that had been hand-tuned and accepted: the S3-1.64 (280x456) got 20 px
 * labels instead of its accepted 28. The reason a ratio can't work is PIXEL
 * PITCH: the 280-410-wide AMOLEDs are dense (~0.078-0.081 mm/px), so the same
 * pixel count renders physically SMALL there — they genuinely need the full
 * reference sizes — while the 240-wide LCDs are coarse (~0.13 mm/px), where the
 * reference sizes render huge. (Same physics as the FONT_TIME dial-mm table
 * below.) So the accepted tiers, plotted against width, are the model:
 *
 *   ANCHORS (user-accepted, do not re-derive):
 *     >= 280 wide  (1.64 280, 1.8 368, 2.06 410, T5 466)  -> 1.000 (reference)
 *     240 wide     (S3-LCD-2 240x320, GC9A01 240x240)     -> 0.585 (16/12/14 tier)
 *     <= 220 wide  (narrow C6/S3-1.47)                    -> hand tiers, identity here
 *   Between the 240 and 280 anchors the factor interpolates linearly; below 240
 *   it holds the LCD-2 anchor. A future device lands on this curve; if its look
 *   is then hand-corrected, ADD ITS POINT HERE rather than bending the formula.
 *
 * HEIGHT TERM (portrait only): two boards can share a width but not a height —
 * the S3-1.69 (240x280) is squatter than the LCD-2 (240x320), and the same fonts
 * crowd its shorter layout. A portrait panel shorter than the reference aspect
 * shrinks by h / (w * 502/410), capped at 1. Square/round panels (GC9A01) skip
 * it: they don't stack the tall reference layout, and the term would drag them
 * below their accepted tier. */
static inline int ui_px_scale(int ref_px) {
#if BOARD_SCREEN_NARROW || (LCD_WIDTH >= 280)
  return ref_px;
#else
  int s = 585 + ((LCD_WIDTH - 240) * (1000 - 585)) / (280 - 240);  // per-mille
  if (s < 585)  s = 585;    // below the 240 anchor: hold the accepted LCD tier
  if (s > 1000) s = 1000;
#if BOARD_SCREEN_PORTRAIT
  int hf = (LCD_HEIGHT * 1000 * UI_FONT_REF_W) / (LCD_WIDTH * UI_FONT_REF_H);
  if (hf > 1000) hf = 1000;      // taller-than-reference aspect: no penalty
  s = s * hf / 1000;
#endif
  return (ref_px * s + 500) / 1000;
#endif
}
#define UI_FONT(ref_px) (*ui_font_nearest(ui_px_scale(ref_px)))

/* ---- Icon GROUPS (UI_ICON_MDI / UI_ICON_SYM) -------------------------------
 * A tray or button row that mixes MDI glyphs (the generated icons14/22/28/34
 * fonts) with built-in LV_SYMBOLs (Montserrat) must render BOTH at the SAME
 * size — a 22 px moon next to a 14 px bell in the same tray reads as a bug, and
 * was one. But the MDI font only exists at the four generated cuts, so the
 * group's size can't land on any even number the way body text can: the shared
 * scaled size is SNAPPED TO THE NEAREST MDI CUT, and both faces use it —
 * UI_ICON_MDI(ref) picks the icons* font, UI_ICON_SYM(ref) the Montserrat cut
 * at the SAME pixel size. Same ref_px in the same group = same size, always.
 *
 *   authored 22 (watch-face tray):  410 -> 22   240x320 -> 14   240x280 -> 14
 *   authored 28 (quick-shade row):  410 -> 28   240x320 -> 14 (16 snaps down)
 *
 * NB the app-menu tile icons deliberately do NOT use this: they are pinned at
 * 34 on every board (explicitly approved look — see MENU_TILE_ICON_FONT). */
LV_FONT_DECLARE(icons14);
LV_FONT_DECLARE(icons22);
LV_FONT_DECLARE(icons28);
LV_FONT_DECLARE(icons34);
static inline int ui_icon_snap_px(int px) {
  static const int cuts[4] = { 14, 22, 28, 34 };   // the generated MDI sizes
  int best = cuts[0];
  for (int i = 1; i < 4; i++) {
    int da = px - cuts[i]; if (da < 0) da = -da;
    int db = px - best;    if (db < 0) db = -db;
    if (da < db) best = cuts[i];
  }
  return best;
}
static inline const lv_font_t *ui_icon_mdi_font(int px) {
  switch (px) {
    case 14: return &icons14;
    case 22: return &icons22;
    case 28: return &icons28;
    default: return &icons34;
  }
}
#define UI_ICON_MDI(ref_px) (*ui_icon_mdi_font(ui_icon_snap_px(ui_px_scale(ref_px))))
#define UI_ICON_SYM(ref_px) (*ui_font_nearest(ui_icon_snap_px(ui_px_scale(ref_px))))

/* ---- Automatic font fitting -----------------------------------------------
 * ui_font_fit(): the LARGEST stock Montserrat (8..max_px) that renders `text` on
 * ONE line within max_w pixels. This is the missing piece between UI_PX() (which
 * scales layout boxes continuously with the panel) and the FONT_* tiers (which
 * are fixed pre-rendered sizes): a box scaled down to a small panel can end up
 * narrower than its tier font's rendering of a long word, and LVGL then wraps —
 * and a wrapped label inside a fixed box CLIPS into whatever sits above it (the
 * app-menu tile captions were the reported case, "Notifications" wrapping into
 * the icon on the 240-wide panels).
 *
 * Every stock size 8..48 is enabled in lv_conf.h, so the ladder below is the
 * full set; the cost of the unused big cuts is already paid. Measuring uses
 * lv_text_get_size with no width cap = the true single-line width.
 *
 * Callers pass the size their tier WANTS (the authored look) as max_px; the
 * helper only ever steps DOWN from it, and only as far as the text actually
 * needs — so short labels keep the authored size and only the long ones shrink.
 * If even 8 px cannot fit, 8 px is returned: pair the label with
 * LV_LABEL_LONG_DOT *and a fixed one-line height* so the tail truncates to
 * "..." instead of wrapping (DOT against an auto-height label still wraps —
 * that silent gotcha is what broke the previous menu-caption fix). */
static inline const lv_font_t *ui_font_fit(const char *text, int32_t max_w, int max_px) {
  static const lv_font_t *const ladder[] = {
    &lv_font_montserrat_8,  &lv_font_montserrat_10, &lv_font_montserrat_12,
    &lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_18,
    &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
    &lv_font_montserrat_26, &lv_font_montserrat_28, &lv_font_montserrat_30,
    &lv_font_montserrat_32, &lv_font_montserrat_34, &lv_font_montserrat_36,
    &lv_font_montserrat_38, &lv_font_montserrat_40, &lv_font_montserrat_42,
    &lv_font_montserrat_44, &lv_font_montserrat_46, &lv_font_montserrat_48,
  };
  static const uint8_t ladder_px[] = { 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28,
                                       30, 32, 34, 36, 38, 40, 42, 44, 46, 48 };
  int top = 0;
  for (int i = 0; i < (int)(sizeof(ladder_px) / sizeof(ladder_px[0])); i++)
    if (ladder_px[i] <= max_px) top = i;
  for (int i = top; i > 0; i--) {
    lv_point_t sz;
    lv_text_get_size(&sz, text, ladder[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x <= max_w) return ladder[i];
  }
  return ladder[0];   // floor: smallest stock size (caller truncates the rest)
}

/* ---- Single-line enforcement (ui_label_single_line) -------------------------
 * Make a label GENUINELY one line: DOT truncation that actually truncates. In
 * LVGL 9 the "…" replacement only happens when the text overflows the label's
 * FIXED size — with the default content-sized height the label simply grows a
 * second line and "DOT" silently degrades to WRAP. Every LONG_DOT site in this
 * firmware had exactly that latent bug (menu captions, SSID rows, file names,
 * notification titles, player metadata...), and the wrapped second line then
 * collided with whatever sat above or below — THE recurring clipping bug.
 *
 * So: pin the height to the CURRENT font's line height (call AFTER the font is
 * applied; the caller keeps setting its own width). Never call
 * lv_label_set_long_mode(x, LV_LABEL_LONG_DOT) directly — use this. */
static inline void ui_label_single_line(lv_obj_t *lbl) {
  const lv_font_t *f = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_height(lbl, lv_font_get_line_height(f));
}

/* ---- Shared MDI icon fonts at the UI's three glyph sizes ----
 * icons22 is the notification font (full range). icons14 / icons28 carry the smaller
 * set of UI glyphs used outside notifications (e.g. the coffee-outline corner + shade
 * icons). All three are the same Material Design Icons typeface, so a glyph looks
 * identical across sizes. Declared here, the single home for the MDI fonts. */
LV_FONT_DECLARE(icons14);
LV_FONT_DECLARE(icons28);
LV_FONT_DECLARE(icons34);   // app-menu tile size (matches MENU_TILE_ICON_FONT montserrat_34)
LV_FONT_DECLARE(icons88);   // big current-conditions weather icon (Weather app); weather glyphs only

/* Material Design Icons codepoints used by the UI (outside the notif categories).
 * Look glyphs up on pictogrammers.com/library/mdi. Add the codepoint to the relevant
 * icons*.c --range and regenerate when introducing a new one. */
#define MDI_COFFEE          0xF0176   // coffee cup — caffeine indicator
#define MDI_MINI_DISK       0xF0A06   // small hard disk — Flash (FFat) volume
#define MDI_MINI_SD         0xF0A05   // small SD card — SD Card volume
#define MDI_WATCH           0xF0897   // smartwatch — Timer/Stopwatch/Alarm app tile.
#define MDI_RUN_FAST        0xF111F   // running figure — Fitness app tile
#define MDI_SLEEP           0xF0594   // moon (sleep) — Sleep app tile + face DND badge

/* ---- Weather glyphs (in icons22 + icons34; see weather_store.h / app_weather.h) ----
 * The Weather app draws these in a single tint each. The MAIN-DIAL "partly cloudy"
 * widget is special: it LAYERS two of these (a yellow MDI_WX_SUNNY behind a white
 * MDI_WX_CLOUDY) to get the two-tone yellow-sun-with-white-cloud look a single glyph
 * can't give — see watchface_set_weather(). MDI_WX_NIGHT is the app-tile icon too.
 * All ten are present in BOTH icons22.c and icons34.c (regenerated --range lists). */
// AUTHORITATIVE MDI codepoints (verified against pictogrammers.com/library/mdi).
// The earlier POURING/RAINY/SNOWY values were shifted by one and rendered wrong glyphs
// (drizzle showed a SNOWFLAKE because 0xF0598 is actually weather-snowy, not -rainy).
#define MDI_WX_SUNNY        0xF0599   // weather-sunny         — clear day (yellow)
#define MDI_WX_PARTLY       0xF0595   // weather-partly-cloudy — sun + cloud (single-tint form)
#define MDI_WX_CLOUDY       0xF0590   // weather-cloudy        — overcast / the white cloud layer
#define MDI_WX_POURING      0xF0596   // weather-pouring       — heavy rain (blue)
#define MDI_WX_RAINY        0xF0597   // weather-rainy         — showers / light rain (blue)
#define MDI_WX_SNOWY        0xF0598   // weather-snowy         — snow (white)
#define MDI_WX_LIGHTNING    0xF0593   // weather-lightning     — thunderstorm
#define MDI_WX_FOG          0xF0591   // weather-fog           — fog / mist / haze
#define MDI_WX_WINDY        0xF059D   // weather-windy         — windy
#define MDI_WX_NIGHT        0xF0594   // weather-night         — clear night (also the app tile)
