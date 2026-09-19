/* gfx_spinner.c -- boot loading spinner drawn straight into the framebuffer (no LVGL yet).
 * Six dots chase around a circle with the Windows-11 style bunching motion: every dot follows
 * the same angle curve, staggered in time, and the curve has a slow and a fast phase so the
 * dots bunch up and spread out. Integer math only (sine table); 16 or 32 bpp framebuffers. */
#include <stdint.h>
#include <string.h>
#include "platform.h"

static const int16_t k_sin[256] = { 0,804,1608,2410,3212,4011,4808,5602,6393,7179,7962,8739,9512,10278,11039,11793,12539,13279,14010,14732,15446,16151,16846,17530,18204,18868,19519,20159,20787,21403,22005,22594,23170,23731,24279,24811,25329,25832,26319,26790,27245,27683,28105,28510,28898,29268,29621,29956,30273,30571,30852,31113,31356,31580,31785,31971,32137,32285,32412,32521,32609,32678,32728,32757,32767,32757,32728,32678,32609,32521,32412,32285,32137,31971,31785,31580,31356,31113,30852,30571,30273,29956,29621,29268,28898,28510,28105,27683,27245,26790,26319,25832,25329,24811,24279,23731,23170,22594,22005,21403,20787,20159,19519,18868,18204,17530,16846,16151,15446,14732,14010,13279,12539,11793,11039,10278,9512,8739,7962,7179,6393,5602,4808,4011,3212,2410,1608,804,0,-804,-1608,-2410,-3212,-4011,-4808,-5602,-6393,-7179,-7962,-8739,-9512,-10278,-11039,-11793,-12539,-13279,-14010,-14732,-15446,-16151,-16846,-17530,-18204,-18868,-19519,-20159,-20787,-21403,-22005,-22594,-23170,-23731,-24279,-24811,-25329,-25832,-26319,-26790,-27245,-27683,-28105,-28510,-28898,-29268,-29621,-29956,-30273,-30571,-30852,-31113,-31356,-31580,-31785,-31971,-32137,-32285,-32412,-32521,-32609,-32678,-32728,-32757,-32767,-32757,-32728,-32678,-32609,-32521,-32412,-32285,-32137,-31971,-31785,-31580,-31356,-31113,-30852,-30571,-30273,-29956,-29621,-29268,-28898,-28510,-28105,-27683,-27245,-26790,-26319,-25832,-25329,-24811,-24279,-23731,-23170,-22594,-22005,-21403,-20787,-20159,-19519,-18868,-18204,-17530,-16846,-16151,-15446,-14732,-14010,-13279,-12539,-11793,-11039,-10278,-9512,-8739,-7962,-7179,-6393,-5602,-4808,-4011,-3212,-2410,-1608,-804 };
static int32_t isin(uint32_t a) { return k_sin[(a >> 8) & 255u]; }           /* a: 0..65535 = one turn */
static int32_t icos(uint32_t a) { return isin(a + 16384u); }

static void dot(uint8_t *fb, uint32_t w, uint32_t h, uint32_t bpp, uint32_t stride,
                int cx, int cy, int r, uint32_t lum)
{
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= (int)h) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= (int)w) continue;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) continue;
            uint8_t *p = fb + (uint32_t)y * stride + (uint32_t)x * bpp;
            if (bpp == 4u) { p[0] = (uint8_t)lum; p[1] = (uint8_t)lum; p[2] = (uint8_t)lum; p[3] = 0; }
            else { uint16_t c = (uint16_t)(((lum >> 3) << 11) | ((lum >> 2) << 5) | (lum >> 3)); p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); }
        }
    }
}

void fb_spinner_frame(uint32_t ms)
{
    uint8_t *fb = (uint8_t *)fb_ptr();
    uint32_t w = fb_width(), h = fb_height(), bpp = fb_bpp();
    if (!fb || (bpp != 2u && bpp != 4u)) return;
    uint32_t stride = w * bpp;
    memset(fb, 0, stride * h);
    int cx = (int)w / 2, cy = (int)h / 2;
    int R = (int)((w < h ? w : h) / 9u);          /* ring radius: 40 px on a 360 px panel */
    int rd = R / 5 > 2 ? R / 5 : 2;               /* dot radius */
    const uint32_t period = 1800u;                /* one turn */
    for (uint32_t i = 0; i < 6u; i++) {
        uint32_t u = ms + 60000u - i * 110u;      /* stagger; offset keeps it positive */
        uint32_t a = (uint32_t)(((uint64_t)u * 65536u) / period);
        a += (uint32_t)((isin(a) * 9000) >> 15);  /* slow/fast phase -> bunching */
        a -= 16384u;                              /* start at 12 o'clock */
        int x = cx + (int)(((int64_t)R * icos(a)) >> 15);
        int y = cy + (int)(((int64_t)R * isin(a)) >> 15);
        dot(fb, w, h, bpp, stride, x, y, rd, 255u - i * 28u);
    }
    fb_flush_all();
}
