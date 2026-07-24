/* compat/driver/rtc_io.h — IDF RTC-GPIO API stubs (deep-sleep pin control; n/a on Linux). */
#pragma once
#include "gpio.h"   // gpio_num_t, esp_err_t

typedef enum {
    RTC_GPIO_MODE_INPUT_ONLY, RTC_GPIO_MODE_OUTPUT_ONLY,
    RTC_GPIO_MODE_INPUT_OUTPUT, RTC_GPIO_MODE_DISABLED,
} rtc_gpio_mode_t;

static inline bool      rtc_gpio_is_valid_gpio(gpio_num_t) { return false; }
static inline esp_err_t rtc_gpio_init(gpio_num_t)          { return 0; }
static inline esp_err_t rtc_gpio_deinit(gpio_num_t)        { return 0; }
static inline esp_err_t rtc_gpio_set_direction(gpio_num_t, rtc_gpio_mode_t) { return 0; }
static inline esp_err_t rtc_gpio_set_level(gpio_num_t, uint32_t) { return 0; }
static inline int       rtc_gpio_get_level(gpio_num_t)     { return 0; }
static inline esp_err_t rtc_gpio_hold_en(gpio_num_t)       { return 0; }
static inline esp_err_t rtc_gpio_hold_dis(gpio_num_t)      { return 0; }
static inline esp_err_t rtc_gpio_isolate(gpio_num_t)       { return 0; }
static inline esp_err_t rtc_gpio_pulldown_en(gpio_num_t)   { return 0; }
static inline esp_err_t rtc_gpio_pulldown_dis(gpio_num_t)  { return 0; }
static inline esp_err_t rtc_gpio_pullup_en(gpio_num_t)     { return 0; }
static inline esp_err_t rtc_gpio_pullup_dis(gpio_num_t)    { return 0; }
