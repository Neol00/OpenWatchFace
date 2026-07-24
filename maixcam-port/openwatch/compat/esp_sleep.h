/* compat/esp_sleep.h — deep-sleep API stubbed (no MCU sleep on a Linux host). */
#pragma once
#include <cstdint>

typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_TOUCHPAD,
    ESP_SLEEP_WAKEUP_ULP,
    ESP_SLEEP_WAKEUP_GPIO,
    ESP_SLEEP_WAKEUP_UART,
} esp_sleep_wakeup_cause_t;

typedef enum { ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_DOMAIN_MAX } esp_sleep_pd_domain_t;
typedef enum { ESP_PD_OPTION_OFF, ESP_PD_OPTION_ON, ESP_PD_OPTION_AUTO } esp_sleep_pd_option_t;
typedef enum { ESP_EXT1_WAKEUP_ANY_HIGH = 1, ESP_EXT1_WAKEUP_ALL_LOW = 0 } esp_sleep_ext1_wakeup_mode_t;
typedef enum { ESP_GPIO_WAKEUP_GPIO_LOW = 0, ESP_GPIO_WAKEUP_GPIO_HIGH = 1 } esp_deepsleep_gpio_wake_up_mode_t;
typedef int esp_err_t;

#ifndef ESP_OK
#define ESP_OK 0
#endif
static inline const char *esp_err_to_name(esp_err_t) { return "ESP_OK"; }

static inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) { return ESP_SLEEP_WAKEUP_UNDEFINED; }
static inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t) { return 0; }
static inline esp_err_t esp_sleep_enable_ext0_wakeup(int, int)  { return 0; }
static inline esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t, esp_sleep_ext1_wakeup_mode_t) { return 0; }
static inline esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t, int) { return 0; }
static inline esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t, esp_sleep_pd_option_t) { return 0; }
static inline void esp_deep_sleep_start(void) {}
static inline void esp_deep_sleep(uint64_t) {}
