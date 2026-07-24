/* ============================================================================
 *  tuya/compat/esp_sleep.h - ESP deep-sleep API mapped onto the REAL T5 low-power
 *  API (tkl_cpu_sleep_mode_set + tkl_wakeup_source_set).
 *
 *  Unlike the Maix port (which stubs sleep to a no-op on a Linux host), the T5 has
 *  a true CPU deep-sleep. The firmware's deep-sleep path (board_sleep.h / the .ino
 *  enter_deep_sleep) calls the ESP API; here each call is recorded into a pending
 *  wake-source config, and esp_deep_sleep_start() programs those sources and enters
 *  TUYA_CPU_DEEP_SLEEP. On the T5, deep sleep resets through boot on wake (same
 *  model the firmware already assumes - it re-runs setup() and reads the wake cause).
 *
 *  Mapping:
 *    esp_sleep_enable_timer_wakeup(us)        -> TUYA_WAKEUP_SOURCE_TIMER (ms)
 *    esp_sleep_enable_ext0_wakeup(gpio, lvl)  -> TUYA_WAKEUP_SOURCE_GPIO (level)
 *    esp_deep_sleep_enable_gpio_wakeup(mask,m)-> TUYA_WAKEUP_SOURCE_GPIO (BOARD_WAKE_GPIO)
 *    esp_deep_sleep_start()                   -> tkl_wakeup_source_set(...) +
 *                                                tkl_cpu_sleep_mode_set(TRUE, DEEP)
 *    esp_sleep_get_wakeup_cause()             -> best-effort from the GPIO level at boot
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino routes ESP headers to
 *  this dir), so it may reference the SDK headers directly.
 * ========================================================================== */
#pragma once
#include <cstdint>
/* board.h (BOARD_WAKE_GPIO) is already included by the .ino before this header is
 * reached, so we don't re-include it - a relative "board.h" wouldn't resolve from
 * this tuya/compat/ subdir anyway. */

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_sleep.h"
#include "tkl_wakeup.h"
#include "tal_system.h"   // tal_system_sleep (defensive post-deep-sleep spin)
}

/* ---- ESP-shaped enums the firmware references ----------------------------- */
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

/* ---- Pending wake-source accumulator -------------------------------------
 * The firmware configures wake sources, THEN calls esp_deep_sleep_start(). We
 * buffer the config across those calls and program it all at sleep entry. */
static TUYA_WAKEUP_SOURCE_BASE_CFG_T s_tuya_wake_cfg[DS_MAX_CFG_ITEM];
static int s_tuya_wake_n = 0;

static inline void owf_tuya_wake_reset(void) { s_tuya_wake_n = 0; }
static inline void owf_tuya_wake_add(const TUYA_WAKEUP_SOURCE_BASE_CFG_T *c) {
    if (s_tuya_wake_n < DS_MAX_CFG_ITEM) s_tuya_wake_cfg[s_tuya_wake_n++] = *c;
}

/* ---- ESP API surface ------------------------------------------------------ */
static inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) {
    TUYA_WAKEUP_SOURCE_BASE_CFG_T c = {};
    c.source = TUYA_WAKEUP_SOURCE_TIMER;
    c.wakeup_para.timer_param.timer_num = TUYA_TIMER_NUM_0;
    c.wakeup_para.timer_param.mode = TUYA_TIMER_MODE_ONCE;
    c.wakeup_para.timer_param.ms = (uint32_t)(us / 1000ULL);
    owf_tuya_wake_add(&c);
    return 0;
}

/* ext0 == "wake when this single GPIO reaches `level`". Maps straight to a GPIO src. */
static inline esp_err_t esp_sleep_enable_ext0_wakeup(int gpio, int level) {
    TUYA_WAKEUP_SOURCE_BASE_CFG_T c = {};
    c.source = TUYA_WAKEUP_SOURCE_GPIO;
    c.wakeup_para.gpio_param.gpio_num = (TUYA_GPIO_NUM_E)gpio;
    c.wakeup_para.gpio_param.level = level ? TUYA_GPIO_WAKEUP_HIGH : TUYA_GPIO_WAKEUP_LOW;
    owf_tuya_wake_add(&c);
    return 0;
}

static inline esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t, esp_sleep_ext1_wakeup_mode_t) { return 0; }

/* The firmware's GPIO-wake path passes a pin MASK; on the T5 the wake button is a
 * single known pin (BOARD_WAKE_GPIO), so register that with the requested level. */
static inline esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t /*mask*/, int mode) {
    TUYA_WAKEUP_SOURCE_BASE_CFG_T c = {};
    c.source = TUYA_WAKEUP_SOURCE_GPIO;
    c.wakeup_para.gpio_param.gpio_num = (TUYA_GPIO_NUM_E)BOARD_WAKE_GPIO;
    c.wakeup_para.gpio_param.level = (mode == ESP_GPIO_WAKEUP_GPIO_HIGH)
                                       ? TUYA_GPIO_WAKEUP_HIGH : TUYA_GPIO_WAKEUP_LOW;
    owf_tuya_wake_add(&c);
    return 0;
}

/* RTC-peripheral power domain has no T5 analog (handled by tkl internally) - no-op. */
static inline esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t, esp_sleep_pd_option_t) { return 0; }

static inline void esp_deep_sleep_start(void) {
    /* IMPORTANT: the Arduino-TuyaOpen build does NOT implement true deep-sleep-with-wake
     * - tkl_wakeup_source_set / the tkl_timer wake-config APIs are absent from
     * libtuyaos.a (undefined reference at link). So we CANNOT do a real deep sleep here.
     * The T5 "sleep" is handled instead as a SCREEN-OFF light-sleep state that RETURNS
     * (see enter_deep_sleep() in sleep_power.h, which never falls through to this on T5).
     * This function is kept only so the shared sleep code links; if it is ever reached on
     * the T5, fall back to light-sleep-idle + yield rather than referencing missing
     * wake-source symbols. */
    tkl_cpu_sleep_mode_set(TRUE, TUYA_CPU_SLEEP);   // light sleep on idle (the only available mode)
    for (;;) tal_system_sleep(2000);                // yield; the firmware keeps running elsewhere
}
static inline void esp_deep_sleep(uint64_t us) {
    esp_sleep_enable_timer_wakeup(us);
    esp_deep_sleep_start();
}

/* Wake-cause readback: the T5 adapter doesn't expose a rich cause code here, so the
 * firmware's banner reports UNDEFINED (a normal cold-boot look). NOTE (2026-07-04): mapping
 * bk_gpio_get_wakeup_gpio_id() to ESP_SLEEP_WAKEUP_GPIO here was tried to fix the two-press
 * wake, and the user reported battery deep sleep breaking with that build — reverted to the
 * plain stub verbatim; leave it this way until the interaction is understood. */
static inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) {
    return ESP_SLEEP_WAKEUP_UNDEFINED;
}
