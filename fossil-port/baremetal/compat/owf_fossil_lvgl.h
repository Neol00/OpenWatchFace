/* owf_fossil_lvgl.h — fossil-port LVGL bring-up for the real OpenWatchFace UI.
 *
 * The fossil analog of tuya/owf_tuya_lvgl_own.h: the firmware owns its own LVGL
 * v9.5 (no vendor task), rendering DIRECT-mode into the continuous-splash
 * framebuffer the bootloader left scanning (fb_splash.c). owf_fossil_lvgl_begin()
 * runs lv_init() + creates the display + touch indev; the firmware's loop() drives
 * lv_timer_handler() itself. Mirrors ui_demo.c's setup, adapted to the firmware's
 * "bring up early, adopt as default later" flow.
 *
 * Touch: the Raydium driver (touch_raydium.c — U128BLA03 per the AsteroidOS
 * hoki defconfig) is polled from the LVGL read callback. If touch_init() finds
 * no controller (bus probe fails), the callback degrades to released-only and
 * the UI runs display-only — same graceful path as before.
 */
#pragma once
#include <lvgl.h>
#include "board.h"   /* LCD_WIDTH / LCD_HEIGHT fallback geometry */

extern "C" {
    void    *fb_init(uint32_t w, uint32_t h);   /* runtime: fb_splash.c */
    uint32_t fb_width(void);
    uint32_t fb_height(void);
    uint32_t fb_bpp(void);                      /* 3 = RGB888, 4 = XRGB8888 */
    void     fb_flush_all(void);
    void     fb_trace(uint32_t xrgb);           /* VISUAL_TRACE milestone paint */
    uint32_t timer_ms(void);
    uint32_t timer_freq_hz(void);               /* render census timebase */
    uint64_t timer_ticks(void);
    void     con_puts(const char *s);
    /* LVGL render census counters (defined in platform/pwr_diag.c, printed as
     * 10 s deltas by the PWR census). See platform.h for what each means. */
    extern volatile uint32_t g_lv_refr_n, g_lv_render_n;
    extern volatile uint32_t g_lv_refr_us, g_lv_flush_us, g_lv_px;
    extern volatile uint32_t g_touch_us, g_touch_n;   /* indev read cost */
    int      touch_init(void);                  /* runtime: touch_raydium.c */
    int      touch_read(uint16_t *x, uint16_t *y);
    void     bootmark(uint32_t stage);          /* IMEM breadcrumbs */
    void     bootmark_aux(unsigned idx, uint32_t val);
    void     vib_buzz(unsigned n, uint32_t ms); /* BUZZ_TRACE reporting */
    void     wdog_stage(unsigned stage);        /* WDOG_TRACE reporting */
}
#define BOOTMARK_FB    7u
#define BOOTMARK_LVGL 10u

/* Microseconds elapsed since a timer_ticks() sample. Used by every census
 * counter below; defined before its first use. */
static inline uint32_t owf_us_since(uint64_t t0) {
    uint32_t per_us = timer_freq_hz() / 1000000u;
    return per_us ? (uint32_t)((timer_ticks() - t0) / per_us) : 0u;
}

static bool s_fossil_touch_ok = false;

/* LVGL pulls input state; touch_read() polls the Raydium over I2C. Keeps the
 * last-known position on release (LVGL wants that for click detection). */
/* Touch activity, for the .ino loop's idle/dim timer.
 *
 * WHY THIS EXISTS (2026-08-28): every other board's touch path raises the
 * loop's s_touch_activity flag when a finger is seen — the FT3168 ISR, the
 * CST816/CST328/AXS5106L pollers, and the Tuya port through its own
 * owf_tuya_take_touch_activity(). This callback did not, and it is the only
 * one that did not. The visible symptom was that tapping the WATCH FACE never
 * un-dimmed the panel, while opening the app menu or the quick shade did —
 * because the loop refreshes the idle timer whenever either of those is OPEN,
 * so navigating anywhere looked like it worked and only the quiet clock face
 * was affected.
 *
 * Taken, not read: the loop consumes the flag, so a tap cannot be counted
 * twice and a held finger keeps re-setting it. Same contract as the Tuya
 * port's take_touch_activity(). */
static volatile bool s_fossil_touch_activity = false;

extern "C" bool owf_fossil_take_touch_activity(void) {
    bool v = s_fossil_touch_activity;
    s_fossil_touch_activity = false;
    return v;
}

static void owf_fossil_touch_cb(lv_indev_t *, lv_indev_data_t *data) {
    static int32_t last_x = 0, last_y = 0;
    uint16_t x, y;
    /* Timed: this callback runs on LVGL's indev period whether or not anything
     * is drawn, so it is the leading candidate for the idle 11-12% that the LV
     * census just showed is NOT rendering (idle window: refr=1, refr%=0). The
     * Gen 6 touch_read is INT-gated (polls TLMM 65, not the bus) so it SHOULD
     * be nearly free — measure rather than assume. */
    uint64_t tt0 = timer_ticks();
    int trd = s_fossil_touch_ok ? touch_read(&x, &y) : -1;
    g_touch_us += owf_us_since(tt0);
    g_touch_n++;
    if (trd == 1) {
        last_x = x; last_y = y;
        s_fossil_touch_activity = true;   /* any finger = user activity */
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

/* ---- render census (2026-08-07) ------------------------------------------
 * pwr_diag reports a steady cpu=11-12% inside loop() at idle and cannot say
 * what it is. Reading the source narrowed it (DIRECT render mode means LVGL
 * redraws dirty rectangles, not whole frames) but could not settle it, so
 * measure instead of guess. Counters are accumulated here and printed as 10 s
 * deltas by pwr_diag — never per frame, which would drown the ramlog. */
static uint64_t s_lv_refr_t0;

static void owf_lv_refr_start_cb(lv_event_t *) { s_lv_refr_t0 = timer_ticks(); }
static void owf_lv_refr_ready_cb(lv_event_t *) {
    g_lv_refr_us += owf_us_since(s_lv_refr_t0);
    g_lv_refr_n++;
}
/* Only fires when there is actually something to draw — the gap between this
 * and g_lv_refr_n is how many refresh cycles were pure overhead. */
static void owf_lv_render_start_cb(lv_event_t *) { g_lv_render_n++; }

/* The MDP already scans the splash buffer continuously; a flush only has to push
 * our CPU writes out of D-cache so the scanout master sees them (fb_flush_all). */
static void owf_fossil_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *) {
    if (area) {
        g_lv_px += (uint32_t)(area->x2 - area->x1 + 1)
                 * (uint32_t)(area->y2 - area->y1 + 1);
    }
    if (lv_display_flush_is_last(disp)) {
        /* fb_flush_all cleans the WHOLE framebuffer range out of D-cache
         * (416*416*3 = 519 KB) regardless of how small the dirty area was —
         * a prime suspect for constant cost, so time it separately. */
        uint64_t f0 = timer_ticks();
        fb_flush_all();
        g_lv_flush_us += owf_us_since(f0);
    }
    lv_display_flush_ready(disp);   /* direct mode: pixels are already in the fb */
}

/* Full LVGL bring-up. Returns true on success (a live framebuffer + display).
 * On failure (bootloader did not leave the panel lit) returns false; the caller
 * keeps s_display_ready false and the UI stays dark but the firmware still runs. */
static inline bool owf_fossil_lvgl_begin(void) {
#if defined(WDOG_TRACE)
    /* Reaching here means ALL of setup() before the display block survived. */
    wdog_stage(11);
#endif
    void *fb = fb_init(LCD_WIDTH, LCD_HEIGHT);   /* args are only a fallback hint */
    /* Record what the display probe found, whether or not it succeeded — these
     * land in IMEM and are readable after the fact (devmem 0x0860080c/0x08600810). */
    bootmark(BOOTMARK_FB);
    bootmark_aux(1, (uint32_t)(uintptr_t)fb);
    bootmark_aux(2, (fb_width() << 16) | fb_height());
    bootmark_aux(3, fb_bpp());
#if defined(BUZZ_TRACE)
    vib_buzz(fb ? 3 : 4, 120);   /* 3 short = framebuffer OK, 4 short = failed */
#endif
#if defined(WDOG_TRACE)
    wdog_stage(12);           /* fb_init() returned */
#endif
    if (!fb) { con_puts("[fossil-lvgl] fb_init failed (panel not lit)\n"); return false; }
#if defined(VISUAL_TRACE)
    fb_trace(0x404040);      /* M1 DARK GRAY: display path up, fb claimed */
#endif

    /* Real geometry is what aboot programmed into the MDP, not the compile guess. */
    uint32_t w = fb_width(), h = fb_height();

    lv_init();
    lv_tick_set_cb(timer_ms);
#if defined(WDOG_TRACE)
    wdog_stage(27);          /* lv_init() returned */
#endif

    lv_display_t *d = lv_display_create((int32_t)w, (int32_t)h);
    if (!d) { con_puts("[fossil-lvgl] lv_display_create failed\n"); return false; }
    /* Match whatever aboot's splash pipe is actually configured for — LK often
     * uses RGB888 (3 bytes/px), not XRGB8888. fb_splash.c read SRC_FORMAT. */
    uint32_t bpp = fb_bpp();
    lv_display_set_color_format(d, bpp == 3 ? LV_COLOR_FORMAT_RGB888
                                            : LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d, fb, NULL, w * h * bpp, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, owf_fossil_flush_cb);
    lv_display_add_event_cb(d, owf_lv_refr_start_cb,   LV_EVENT_REFR_START,   NULL);
    lv_display_add_event_cb(d, owf_lv_render_start_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(d, owf_lv_refr_ready_cb,   LV_EVENT_REFR_READY,   NULL);
    lv_display_set_default(d);
#if defined(WDOG_TRACE)
    wdog_stage(28);          /* display created + buffers bound */
#endif

    lv_indev_t *indev = lv_indev_create();
#if defined(WDOG_TRACE)
    wdog_stage(46);          /* lv_indev_create returned */
#endif
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
#if defined(WDOG_TRACE)
    wdog_stage(47);          /* set_type returned */
#endif
    lv_indev_set_read_cb(indev, owf_fossil_touch_cb);
    /* Poll at 15 ms, not the 33 ms default: the Raydium queues reports at its
     * own scan rate and we consume by polling (no IRQ wired), so the poll
     * rate bounds both tap latency and queue-drain headroom (touch_raydium.c
     * queue-drain rev 2). */
    lv_timer_set_period(lv_indev_get_read_timer(indev), 15);
#if defined(VISUAL_TRACE)
    fb_trace(0x0000FF);      /* M2 BLUE(/red): LVGL display + indev bound */
#endif

    /* Bus-probe for the Raydium (bounded; a few ms per silent bus). Failure is
     * non-fatal: the UI still renders, input just stays released. */
    bootmark(BOOTMARK_LVGL);
#if defined(WDOG_TRACE)
    wdog_stage(45);          /* indev created; about to touch_init */
#endif
    s_fossil_touch_ok = (touch_init() == 0);
#if defined(WDOG_TRACE)
    wdog_stage(29);          /* touch_init returned; bring-up complete */
#endif
#if defined(VISUAL_TRACE)
    fb_trace(0x00FF00);      /* M3 GREEN: touch_init returned */
#endif
    con_puts(s_fossil_touch_ok ? "[fossil-lvgl] LVGL up, touch live\n"
                               : "[fossil-lvgl] LVGL up, display-only (no touch)\n");
    return true;
}
