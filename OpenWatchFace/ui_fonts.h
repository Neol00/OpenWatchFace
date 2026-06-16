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

#if BOARD_HAS_PSRAM
/* S3-2.06 (large panel): the original, unchanged set. */
LV_FONT_DECLARE(montserrat_clock_110);
#define FONT_TIME  montserrat_clock_110     // enormous HH:MM (custom: digits+colon only)
#define FONT_TIME_IS_CUSTOM 1
#define FONT_LABEL lv_font_montserrat_28    // (kept for notifications/other UI)
#define FONT_SMALL lv_font_montserrat_20    // small text
#define FONT_ABOUT_BODY lv_font_montserrat_20  // About screen body (== FONT_SMALL on S3)
#define FONT_TOP   lv_font_montserrat_24    // the top-row value text (%, date, day)
#else
/* C6-1.47 (small panel): stock Montserrat tier (~42% of the reference sizes). */
#define FONT_TIME  lv_font_montserrat_48    // HH:MM (stock font; full glyph set)
#define FONT_TIME_IS_CUSTOM 0
#define FONT_LABEL lv_font_montserrat_12
#define FONT_SMALL lv_font_montserrat_12
#define FONT_ABOUT_BODY lv_font_montserrat_12
#define FONT_TOP   lv_font_montserrat_10
#endif
