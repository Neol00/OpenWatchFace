/* gfx_text.c — text straight into the framebuffer (8x8 font, 2x scale).
 *
 * Board-neutral by design: the gen6-only version of this lived in fb_splash.c
 * and could only ever be tested by flashing the watch. Here it uses the
 * fb_* accessors, so the QEMU target renders it too and the renderer can be
 * verified with tools/qemu-screenshot.sh — no hardware, no flash cycle.
 */
#include "platform.h"

/* 8x8 glyphs for fb_text_dump: index via k_font_idx below. */
static const uint8_t k_font[53][8] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },  /*   */
    { 0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 },  /* A */
    { 0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00 },  /* B */
    { 0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00 },  /* C */
    { 0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00 },  /* D */
    { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00 },  /* E */
    { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00 },  /* F */
    { 0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00 },  /* G */
    { 0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 },  /* H */
    { 0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 },  /* I */
    { 0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00 },  /* J */
    { 0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00 },  /* K */
    { 0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00 },  /* L */
    { 0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00 },  /* M */
    { 0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00 },  /* N */
    { 0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },  /* O */
    { 0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00 },  /* P */
    { 0x3C,0x66,0x66,0x66,0x6E,0x6C,0x36,0x00 },  /* Q */
    { 0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00 },  /* R */
    { 0x3E,0x60,0x60,0x3C,0x06,0x06,0x7C,0x00 },  /* S */
    { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 },  /* T */
    { 0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },  /* U */
    { 0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00 },  /* V */
    { 0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00 },  /* W */
    { 0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00 },  /* X */
    { 0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00 },  /* Y */
    { 0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00 },  /* Z */
    { 0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0x00 },  /* 0 */
    { 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 },  /* 1 */
    { 0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00 },  /* 2 */
    { 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 },  /* 3 */
    { 0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00 },  /* 4 */
    { 0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 },  /* 5 */
    { 0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00 },  /* 6 */
    { 0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00 },  /* 7 */
    { 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 },  /* 8 */
    { 0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00 },  /* 9 */
    { 0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00 },  /* : */
    { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 },  /* - */
    { 0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00 },  /* = */
    { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 },  /* . */
    { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30 },  /* , */
    { 0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00 },  /* / */
    { 0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00 },  /* ( */
    { 0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00 },  /* ) */
    { 0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00 },  /* ! */
    { 0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00 },  /* ? */
    { 0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00 },  /* + */
    { 0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00 },  /* > */
    { 0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00 },  /* < */
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00 },  /* _ */
    { 0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00 },  /* star */
    { 0x66,0xFF,0x66,0x66,0xFF,0x66,0x00,0x00 },  /* # */
};
static const char k_font_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-=.,/()!?+><_*#";

/* isqrt — integer square root, for the round-panel chord calculation below.
 * No libm here (and no FPU state to save in a bring-up path). */
static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

/* fb_text_dump — render text DIRECTLY into the framebuffer with the 8x8
 * font above, 2x scaled. No LVGL, no fonts, no objects, no heap: the same
 * path that paints the diagnostic colors (proven working since the first
 * display bring-up). Unknown chars render as blank; lowercase folds to
 * uppercase.
 *
 * CENTRED AND CLIPPED TO THE CIRCLE (2026-08-28). This used to start every
 * line at a fixed (36, 44) and run left-to-right, which is a layout for a
 * RECTANGULAR screen. Both watches have ROUND panels: the corners of that
 * rectangle are not merely dim, they are physically absent, so the first
 * characters of every line were rendered into glass that does not exist and
 * the reader had to guess the message. (Found the honest way: a recovery
 * prompt that could not be read.)
 *
 * So: the block is centred vertically, each line is centred horizontally,
 * and each line is limited to the CHORD of the panel circle at its own row —
 * a line near the top or bottom of the screen has far less usable width than
 * one through the middle, which is exactly what a naive centre-and-hope
 * layout gets wrong. */
void fb_text_dump(const char *s)
{
    uintptr_t fb = (uintptr_t)fb_ptr();
    uint32_t w = fb_width(), h = fb_height(), bpp = fb_bpp();
    uint32_t stride = w * bpp;
    if (!fb || bpp < 2) return;

    const uint32_t sc = 2u, cw = 8u * sc, ch = 8u * sc;
    const uint32_t cx = w / 2u, cy = h / 2u;
    /* Inscribed radius, minus a margin: the bezel/curvature eats the last few
     * pixels, and a glyph touching the exact edge is unreadable anyway. */
    const uint32_t rad = (w < h ? w : h) / 2u;
    const uint32_t safe_r = rad > 12u ? rad - 12u : rad;

    /* clear to black */
    for (uint32_t y = 0; y < h; y++) {
        volatile uint8_t *row = (volatile uint8_t *)(fb + y * stride);
        for (uint32_t i = 0; i < w * bpp; i++) row[i] = 0;
    }

    /* Pass 1: measure. Count lines so the block can be centred vertically. */
    uint32_t nlines = 1;
    for (const char *p = s; *p; p++) if (*p == '\n') nlines++;
    uint32_t block_h = nlines * ch;
    /* Start above centre by half the block; clamp so a long message still
     * begins on screen rather than above it. */
    uint32_t y_start = (block_h / 2u <= cy) ? cy - block_h / 2u : 0u;

    /* Pass 2: render, one line at a time. */
    const char *p = s;
    for (uint32_t line = 0; line < nlines; line++) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        uint32_t len = (uint32_t)(end - p);

        uint32_t ly = y_start + line * ch;
        if (ly + ch > h) break;

        /* Usable half-width at THIS row: the chord of the safe circle at
         * whichever of the line's edges is furthest from the vertical centre
         * (its top or its bottom), so no corner of a glyph pokes outside. */
        uint32_t dy_top = (ly > cy) ? ly - cy : cy - ly;
        uint32_t dy_bot = (ly + ch > cy) ? (ly + ch) - cy : cy - (ly + ch);
        uint32_t dy = dy_top > dy_bot ? dy_top : dy_bot;
        uint32_t half = 0;
        if (dy < safe_r) half = isqrt32(safe_r * safe_r - dy * dy);
        uint32_t maxchars = (2u * half) / cw;

        if (maxchars == 0) { p = *end ? end + 1 : end; continue; }
        if (len > maxchars) len = maxchars;      /* clip, never overflow */

        uint32_t line_w = len * cw;
        uint32_t x_start = (line_w / 2u <= cx) ? cx - line_w / 2u : 0u;

        for (uint32_t i = 0; i < len; i++) {
            char c = p[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);

            unsigned gi = 0, found = 0;
            for (unsigned k = 0; k_font_chars[k]; k++)
                if (k_font_chars[k] == c) { gi = k; found = 1; break; }
            if (!found) continue;

            for (uint32_t gy = 0; gy < 8u; gy++) {
                uint8_t bits = k_font[gi][gy];
                for (uint32_t sy = 0; sy < sc; sy++) {
                    uint32_t py = ly + gy * sc + sy;
                    if (py >= h) break;
                    volatile uint8_t *row = (volatile uint8_t *)(fb + py * stride);
                    for (uint32_t gx = 0; gx < 8u; gx++) {
                        if (!(bits & (0x80u >> gx))) continue;
                        for (uint32_t sx = 0; sx < sc; sx++) {
                            uint32_t px = x_start + i * cw + gx * sc + sx;
                            if (px >= w) break;
                            volatile uint8_t *q = row + px * bpp;
                            q[0] = 0x40; q[1] = 0xFF; q[2] = 0x60;   /* green */
                            if (bpp == 4) q[3] = 0xFF;
                        }
                    }
                }
            }
        }
        p = *end ? end + 1 : end;
    }
    fb_flush_all();
}
