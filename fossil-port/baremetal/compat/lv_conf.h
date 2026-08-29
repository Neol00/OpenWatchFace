/* lv_conf.h — fossil-port LVGL config override.
 *
 * Reuses the firmware's real lv_conf.h verbatim (fonts, widgets, features — must
 * stay byte-identical between the app TU and liblvgl or their struct layouts
 * diverge) but forces ONE change: single-threaded LVGL.
 *
 * The firmware's config sets LV_USE_OS = LV_OS_FREERTOS, which compiles
 * lv_freertos.c — and that file calls ESP's xTaskCreatePinnedToCore, absent from
 * bare-metal FreeRTOS. The fossil port runs lv_timer_handler() itself in loop()
 * and never uses lv_lock/lv_thread, so LVGL needs no OS integration at all.
 *
 * Put -I compat AHEAD of -I <libraries> for BOTH the app and the LVGL build so
 * this file (not libraries/lv_conf.h) is what "lv_conf.h" resolves to.
 */
#pragma once
#include "../../../libraries/lv_conf.h"   /* the firmware's real LVGL config */

#undef  LV_USE_OS
#define LV_USE_OS LV_OS_NONE

/* Multi-unit SW rendering needs an OS (worker threads); single-core fossil renders
 * single-threaded, so one draw unit. (Multi-core render is a later optimisation.) */
#undef  LV_DRAW_SW_DRAW_UNIT_CNT
#define LV_DRAW_SW_DRAW_UNIT_CNT 1

/* THE BLACK-SCREEN BUG (2026-08-03, first-light session). The firmware's config
 * is ESP32-heritage: RGB565-only software rendering. The fossil display scans
 * out XRGB8888/RGB888 (MDP pipe format), and owf_fossil_lvgl.h sets the display
 * to that format — which the renderer was compiled WITHOUT. LVGL 9 then
 * silently skips every draw task for the unsupported target format and flushes
 * an untouched (black) buffer: firmware fully alive, direct fb writes visible,
 * LVGL content never on the glass. Enable the 32/24-bit target paths (ARGB8888
 * too: transparent intermediate layers render in display format + alpha). */
#undef  LV_DRAW_SW_SUPPORT_RGB888
#define LV_DRAW_SW_SUPPORT_RGB888   1
#undef  LV_DRAW_SW_SUPPORT_XRGB8888
#define LV_DRAW_SW_SUPPORT_XRGB8888 1
#undef  LV_DRAW_SW_SUPPORT_ARGB8888
#define LV_DRAW_SW_SUPPORT_ARGB8888 1
