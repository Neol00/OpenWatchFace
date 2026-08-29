/* esp_sleep.h — deep-sleep API stub. No sleep path yet: enables are no-ops and
 * "deep sleep" just reboots (setup() re-runs, which the firmware already expects). */
#pragma once
#include <cstdint>
extern "C" { void reboot_now(void); }
typedef enum { ESP_SLEEP_WAKEUP_UNDEFINED=0, ESP_SLEEP_WAKEUP_EXT0=2, ESP_SLEEP_WAKEUP_EXT1=3,
               ESP_SLEEP_WAKEUP_TIMER=4, ESP_SLEEP_WAKEUP_GPIO=7 } esp_sleep_wakeup_cause_t;
typedef enum { ESP_PD_DOMAIN_RTC_PERIPH=0, ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_DOMAIN_RTC_FAST_MEM,
               ESP_PD_DOMAIN_MAX } esp_sleep_pd_domain_t;
typedef enum { ESP_PD_OPTION_OFF=0, ESP_PD_OPTION_ON, ESP_PD_OPTION_AUTO } esp_sleep_pd_option_t;
typedef int esp_err_t;
typedef int gpio_num_t_placeholder;
#if defined(PLAT_BOARD_FOSSIL_GEN6)
extern "C" { void plat_suspend(void); void plat_suspend_set_timer_us(unsigned long long us); }
static inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { plat_suspend_set_timer_us(us); return 0; }
#else
static inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t) { return 0; }
#endif
static inline esp_err_t esp_sleep_enable_ext0_wakeup(int, int)  { return 0; }
static inline esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t, int) { return 0; }
static inline esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t, int) { return 0; }
static inline esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t, esp_sleep_pd_option_t) { return 0; }
static inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) { return ESP_SLEEP_WAKEUP_UNDEFINED; }
/* BRING-UP SEMANTICS (2026-08-03): deep sleep must NOT reboot. The old
 * reboot_now() here turned every firmware sleep decision (double-tap on the
 * BOOT button — which is the SAME physical button held to boot recovery —
 * idle timeout, DND background path) into a silent trip to Wear OS. The
 * firmware's callers already tolerate enter_deep_sleep() RETURNING (that is
 * the documented T5 suspend behavior), so emulate a short suspend-and-wake:
 * pause, then return and keep running. Real PM660 sleep comes later. */
extern "C" { void timer_delay_ms(uint32_t ms); void con_puts(const char *s); }
static inline void esp_deep_sleep_start(void) {
#if defined(PLAT_BOARD_FOSSIL_GEN6)
    /* THE SLEEP-RETRY FREEZE (2026-08-07): pausing 900 ms and returning made
     * the still-idle watch re-enter sleep forever (cpu=96%, UI dead). The
     * caller's contract is the T5 one — BLOCK until a real wake source.
     * plat_suspend (suspend_msm.c): panel off, WFI until touch INT or the
     * armed timer deadline, panel on, resume in place. */
    plat_suspend();
#else
    con_puts("[sleep-stub] deep sleep requested - pausing 900ms, resuming\n");
    timer_delay_ms(900);
#endif
}
static inline void esp_deep_sleep(uint64_t) { esp_deep_sleep_start(); }

#ifndef ESP_OK
#define ESP_OK 0
#endif
static inline const char *esp_err_to_name(esp_err_t) { return "OK"; }
typedef enum { ESP_GPIO_WAKEUP_GPIO_LOW = 0, ESP_GPIO_WAKEUP_GPIO_HIGH = 1 } esp_deepsleep_gpio_wake_up_mode_t;
