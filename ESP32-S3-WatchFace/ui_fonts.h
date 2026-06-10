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

/* Big clock font: Montserrat SemiBold 110px, generated as a REAL LVGL font (no
 * runtime scaling — that crashed the CO5300 render path). Restricted glyph range
 * 0x30-0x3A (digits + colon only) keeps the .c tiny despite the size. Generate at
 * lvgl.io/tools/fontconverter:  Name=montserrat_clock_110, Size=110, Bpp=4,
 * Range=0x30-0x3A, font=Montserrat-SemiBold.ttf — drop the .c in this folder.
 * The other text uses built-in Montserrat sizes (already enabled in lv_conf.h). */
LV_FONT_DECLARE(montserrat_clock_110);

#define FONT_TIME  montserrat_clock_110     // enormous HH:MM
#define FONT_LABEL lv_font_montserrat_28    // (kept for notifications/other UI)
#define FONT_SMALL lv_font_montserrat_20    // small text
#define FONT_TOP   lv_font_montserrat_24    // the top-row value text (%, date, day)
