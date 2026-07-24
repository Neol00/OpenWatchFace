/* ============================================================================
 *  tuya/compat/esp_mac.h - per-unit MAC read, backed by the REAL T5 WiFi MAC.
 *
 *  device_info.h derives a per-unit suffix for the device/BLE name from the radio
 *  MAC (esp_efuse_mac_get_default: out[0]=OUI .. out[5]=most-unique). The T5 exposes
 *  its station MAC via tkl_wifi_get_mac(WF_STATION, &nw_mac) - same 6-byte order - so
 *  each board gets a genuinely unique name, not a fake constant. If the read fails
 *  (e.g. radio not yet initialised at the moment of the call), fall back to a fixed
 *  locally-administered address so the name is still well-formed.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino/device_info.h route
 *  <esp_mac.h> here, gated per-include).
 * ========================================================================== */
#pragma once
#include <cstdint>
#include <cstring>

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_wifi.h"
}

typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;

static inline int esp_read_mac(uint8_t *out, esp_mac_type_t) {
    if (!out) return -1;
    NW_MAC_S nw;
    memset(&nw, 0, sizeof(nw));
    if (tkl_wifi_get_mac(WF_STATION, &nw) == OPRT_OK) {
        memcpy(out, nw.mac, 6);
        return 0;
    }
    /* Radio MAC unavailable - locally-administered fallback ("OWFT5"). */
    static const uint8_t fallback[6] = {0x02, 0x00, 'O', 'W', 'F', 'T'};
    memcpy(out, fallback, 6);
    return 0;
}

static inline int esp_efuse_mac_get_default(uint8_t *out) {
    return esp_read_mac(out, ESP_MAC_WIFI_STA);
}
