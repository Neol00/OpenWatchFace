/* lv_conf.h — LVGL 9.5 config for the fossil-port BARE-METAL build.
 * (Selected via -DLV_CONF_INCLUDE_SIMPLE + include order; the Arduino
 * sketchbook's ESP-tuned lv_conf.h must NOT be picked up here.)
 * Unset options fall back to lv_conf_internal.h defaults. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32

/* newlib-nano provides malloc, string, snprintf; heap grows above the image */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE   /* single UI task owns LVGL (firmware's rule) */

#define LV_DEF_REFR_PERIOD 16

/* Fonts for the demo face; the firmware build swaps in its own set later. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#endif /* LV_CONF_H */
