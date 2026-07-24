/* ============================================================================
 *  tuya/compat/driver/gpio.h - ESP-IDF GPIO hold/level API shim.
 *
 *  board_sleep.h uses gpio_hold_en/dis to LATCH panel/touch reset + backlight pins
 *  across deep sleep on the ESP boards. On the T5 the platform retains pad state and
 *  wake pins are programmed via tkl_wakeup (see tuya/compat/esp_sleep.h), so these
 *  are no-ops. Runtime GPIO the firmware actually drives goes through Arduino
 *  digital* (the TuyaOpen core), not this IDF header.
 *
 *  Shares the gpio_num_t / esp_err_t typedefs with driver/rtc_io.h via guards
 *  (both are included together in board_sleep.h).
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino/board_sleep.h route
 *  <driver/gpio.h> here).
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

typedef enum { GPIO_MODE_DISABLE, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_MODE_OUTPUT_OD } gpio_mode_t;
#ifndef GPIO_NUM_NC
#define GPIO_NUM_NC ((gpio_num_t)-1)
#endif

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
