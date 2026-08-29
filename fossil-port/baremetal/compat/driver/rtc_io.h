/* ============================================================================
 *  tuya/compat/driver/rtc_io.h - ESP RTC-GPIO hold API shim.
 *
 *  On the ESP boards the firmware uses rtc_gpio_* to keep the wake button's pull
 *  configured and HELD across deep sleep. On the T5 the wake pin is programmed via
 *  tkl_wakeup_source_set() (see tuya/compat/esp_sleep.h) and the platform retains
 *  the pad config internally, so these become no-ops. gpio_num_t / RTC mode enums
 *  are provided so the call sites compile unchanged.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino/board_sleep.h route
 *  <driver/rtc_io.h> here).
 * ========================================================================== */
#pragma once
#include <cstdint>

#ifndef OWF_TUYA_GPIO_NUM_T_DEFINED
#define OWF_TUYA_GPIO_NUM_T_DEFINED
typedef int gpio_num_t;
#endif
#ifndef OWF_TUYA_ESP_ERR_T_DEFINED
#define OWF_TUYA_ESP_ERR_T_DEFINED
typedef int esp_err_t;
#endif
typedef enum { RTC_GPIO_MODE_INPUT_ONLY = 0, RTC_GPIO_MODE_OUTPUT_ONLY,
               RTC_GPIO_MODE_INPUT_OUTPUT, RTC_GPIO_MODE_DISABLED } rtc_gpio_mode_t;

static inline esp_err_t rtc_gpio_init(gpio_num_t)            { return 0; }
static inline esp_err_t rtc_gpio_deinit(gpio_num_t)          { return 0; }
static inline esp_err_t rtc_gpio_set_direction(gpio_num_t, rtc_gpio_mode_t) { return 0; }
static inline esp_err_t rtc_gpio_pullup_en(gpio_num_t)       { return 0; }
static inline esp_err_t rtc_gpio_pullup_dis(gpio_num_t)      { return 0; }
static inline esp_err_t rtc_gpio_pulldown_en(gpio_num_t)     { return 0; }
static inline esp_err_t rtc_gpio_pulldown_dis(gpio_num_t)    { return 0; }
static inline esp_err_t rtc_gpio_hold_en(gpio_num_t)         { return 0; }
static inline esp_err_t rtc_gpio_hold_dis(gpio_num_t)        { return 0; }
static inline bool      rtc_gpio_is_valid_gpio(gpio_num_t)   { return true; }
