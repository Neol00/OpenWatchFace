/* ============================================================================
 *  tuya/compat/esp_wifi.h - ESP-IDF WiFi control shim (TX-power only).
 *
 *  The firmware calls esp_wifi_set_max_tx_power() to apply its WiFi TX-power ladder.
 *  The T5 tkl_wifi adapter exposes no equivalent max-tx-power setter, so this is a
 *  no-op: the radio still associates/works (via the TuyaOpen WiFi lib), the power
 *  ladder still drives the Settings UI, but the dBm cap isn't pushed to the driver.
 *  Wire to a real T5 call here if/when the platform exposes one.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino routes <esp_wifi.h> here).
 * ========================================================================== */
#pragma once
#include <cstdint>

typedef int esp_err_t;

static inline esp_err_t esp_wifi_set_max_tx_power(int8_t)    { return 0; }
static inline esp_err_t esp_wifi_get_max_tx_power(int8_t *p) { if (p) *p = 0; return 0; }
