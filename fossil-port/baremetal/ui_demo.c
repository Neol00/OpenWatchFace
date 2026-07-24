/* ui_demo.c — LVGL 9.5 on the bare-metal runtime: a minimal round watch face.
 *
 * Proves the full stack the firmware needs: LVGL heap (newlib), tick source
 * (arch timer), direct-mode rendering into the platform framebuffer, and the
 * FreeRTOS task loop that will later host the real OpenWatchFace UI.
 */
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"

#define UI_W 390
#define UI_H 390

static lv_obj_t *s_time_label;
static lv_obj_t *s_sub_label;

#if defined(PLAT_BOARD_FOSSIL_GEN4)
int mdp3_flush(const void *buf);
int touch_init(void);
int touch_read(uint16_t *x, uint16_t *y);

/* LVGL pulls input state; touch_read() polls the controller over I2C. */
static void touch_lvgl_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x = 0, y = 0;
    int down = touch_read(&x, &y);
    if (down == 1) {
        data->point.x = (int32_t)x;
        data->point.y = (int32_t)y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state   = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area; (void)px_map;
#if defined(PLAT_BOARD_FOSSIL_GEN4)
    /* Command-mode panel: nothing scans out on its own, so every flush is an
     * explicit MDP3 DMA_P push. Direct render mode means the whole framebuffer
     * is already composed; LVGL calls us once per refresh with the last area.
     * (Partial-ROI push — reprogramming DMA_P_SIZE/OUT_XY per dirty area — is
     * the obvious follow-up optimisation.) */
    if (lv_display_flush_is_last(disp))
        mdp3_flush(lv_display_get_buf_active(disp)->data);
#endif
    lv_display_flush_ready(disp);      /* direct mode: pixels already in the fb */
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t s = timer_ms() / 1000u;
    lv_label_set_text_fmt(s_time_label, "%02u:%02u", (unsigned)(s / 60u) % 100u, (unsigned)(s % 60u));
}

static void ui_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    /* accent ring, mirroring the watch bezel */
    lv_obj_t *arc = lv_arc_create(scr);
    lv_obj_set_size(arc, UI_W - 8, UI_H - 8);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_value(arc, 70);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xE0641E), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);

    s_time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_label_set_text(s_time_label, "00:00");
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -20);

    s_sub_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_sub_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sub_label, lv_color_hex(0x9C9C9C), 0);
    lv_label_set_text(s_sub_label, "OpenWatchFace  |  bare-metal " PLAT_NAME);
    lv_obj_align(s_sub_label, LV_ALIGN_CENTER, 0, 40);

    lv_timer_create(clock_timer_cb, 500, NULL);
}

void ui_task(void *arg)
{
    (void)arg;
    void *fb = fb_init(UI_W, UI_H);
    if (!fb) {
        con_puts("ui: no framebuffer, task idle\n");
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    lv_init();
    lv_tick_set_cb(timer_ms);

    lv_display_t *disp = lv_display_create(UI_W, UI_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, fb, NULL, UI_W * UI_H * 4, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

#if defined(PLAT_BOARD_FOSSIL_GEN4)
    /* Touch is optional: a controller that does not answer must not stop the
     * UI from running (it is also the most likely thing to be mis-addressed
     * until the stock DTB confirms the bus and address). */
    if (touch_init() == 0) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_lvgl_cb);
        con_puts("ui: touch input registered\n");
    } else {
        con_puts("ui: no touch, display-only\n");
    }
#endif

    ui_build();
    con_puts("ui: LVGL " LVGL_VERSION_INFO " running\n");

    for (;;) {
        uint32_t wait = lv_timer_handler();
        if (wait == LV_NO_TIMER_READY || wait > 50) wait = 50;
        if (wait < 2) wait = 2;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}
