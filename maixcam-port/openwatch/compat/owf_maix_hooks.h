/* ============================================================================
 *  owf_maix_hooks.h — shared hooks between main.cpp (MaixCDK side) and the
 *  Arduino shim (firmware side).
 *
 *  g_owf_boot_level: the logical level digitalRead() reports for the BOOT button.
 *  The firmware's loop() polls digitalRead(BOOT_BTN_GPIO) (LOW = pressed) for its
 *  single-tap-menu / double-tap-sleep handling. On the MaixCam-Pro there's no MCU
 *  pin; main.cpp drives this from the physical User button via maix::key, so the
 *  existing firmware button logic works unchanged. 1 = HIGH/released, 0 = LOW/pressed.
 * ========================================================================== */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int g_owf_boot_level;   /* default 1 (released) */

/* Panel brightness 0..100 -> maix::display::set_backlight(). Defined in the maix
 * bridge TU (arduino_time_maix.cpp); the firmware's board_display_set_brightness()
 * calls it on the Maix platform. Safe before the display is up (no-ops). */
void owf_maix_set_backlight(int pct_0_100);

/* AXP2101 PMU (on-board, addr 0x34) bridge -> maix::ext_dev::pmu::PMU. Lazily
 * opened; all return safe values if the PMU isn't present. */
int owf_maix_pmu_begin(void);     /* open PMU; 1 if ok */
int owf_maix_pmu_ok(void);        /* 1 if PMU is open */
int owf_maix_bat_percent(void);   /* 0..100, or -1 if unknown */
int owf_maix_bat_mv(void);        /* battery mV, 0 if unknown */
int owf_maix_is_charging(void);   /* 1 if charging */
int owf_maix_vbus_in(void);       /* 1 if USB/adapter present */

/* WiFi bridge -> maix::network::wifi::Wifi (wpa_supplicant). connected() is cached
 * (cheap to poll from the UI); a background thread auto-connects to the configured
 * network. Returns safe values if WiFi is unavailable. */
int  owf_maix_wifi_connect(const char *ssid, const char *pass);  /* start a connect; 1 if accepted */
int  owf_maix_wifi_connected(void);            /* cached: 1 if associated + has IP */
void owf_maix_wifi_disconnect(void);
int  owf_maix_wifi_ip(char *buf, int cap);     /* dotted IP into buf; returns length */
int  owf_maix_wifi_rssi(void);                 /* cached RSSI dBm (0 if unknown) */
/* NOTE: there is no separate auto-connect here. The firmware's own net task
 * (notif_net.h: wifi_connect) calls WiFi.begin()/status(), which route to the
 * owf_maix_wifi_* primitives above via compat/WiFi.h. */

/* System info (real, from /proc + /sys + maix::sys) for the About / Power screens. */
long owf_maix_mem_total_kb(void);
long owf_maix_mem_avail_kb(void);
long owf_maix_mem_reserved_kb(void);   /* RAM carved out of Linux for media/NPU (kernel-only) */
int  owf_maix_cpu_mhz(void);
int  owf_maix_cpu_temp_c(void);        /* -273 if unknown */
int  owf_maix_cpu_usage_pct(void);     /* delta since previous call */
long owf_maix_disk_total_kb(void);
long owf_maix_disk_free_kb(void);
void owf_maix_os_version(char *buf, int cap);

#ifdef __cplusplus
}
#endif
