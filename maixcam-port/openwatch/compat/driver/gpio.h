/* compat/driver/gpio.h — IDF GPIO API stubs (no MCU pins on a Linux host). */
#pragma once
#include <cstdint>

typedef int gpio_num_t;
typedef int esp_err_t;
typedef enum { GPIO_MODE_DISABLE, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_MODE_OUTPUT_OD } gpio_mode_t;
#define GPIO_NUM_NC ((gpio_num_t)-1)

static inline esp_err_t gpio_hold_en(gpio_num_t)   { return 0; }
static inline esp_err_t gpio_hold_dis(gpio_num_t)  { return 0; }
static inline void      gpio_deep_sleep_hold_en(void)  {}
static inline void      gpio_deep_sleep_hold_dis(void) {}
static inline esp_err_t gpio_set_level(gpio_num_t, uint32_t) { return 0; }
static inline int       gpio_get_level(gpio_num_t) { return 0; }
static inline esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t) { return 0; }
static inline esp_err_t gpio_reset_pin(gpio_num_t) { return 0; }
static inline esp_err_t gpio_pullup_en(gpio_num_t)    { return 0; }
static inline esp_err_t gpio_pullup_dis(gpio_num_t)   { return 0; }
static inline esp_err_t gpio_pulldown_en(gpio_num_t)  { return 0; }
static inline esp_err_t gpio_pulldown_dis(gpio_num_t) { return 0; }
