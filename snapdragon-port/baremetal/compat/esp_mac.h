/* esp_mac.h — per-unit MAC. No radio yet: fixed locally-administered address. */
#pragma once
#include <cstdint>
#include <cstring>
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;
/* The device's real WLAN MAC (persist/wifimac.ini, embedded next to the NV
 * blob; a locally-administered fallback when a board has none). Used for every
 * type: the BLE name suffix (deviceRadioName) reads octets 4..5 of it, and the
 * old fixed placeholder here gave the Gen 4 and the C2 the SAME name (2026-09-03). */
extern "C" const uint8_t *wlan_mac(void);
static inline int esp_read_mac(uint8_t *o, esp_mac_type_t) {
    if (!o) return -1;
#if defined(PLAT_BOARD_FOSSIL_GEN6)
    /* WCNSS never boots on this watch, so wlan_mac() is all zeros and every
     * unit advertised as "WatchFace-0001". The Bluetooth controller has a real
     * per-unit address (from its NVM); use that once it is known. */
    {
        const uint8_t *bd = bt_wcn3990_bdaddr();
        if (bd && (bd[0] | bd[1] | bd[2] | bd[3] | bd[4] | bd[5])) {
            for (int i = 0; i < 6; i++) o[i] = bd[5 - i];   /* HCI is LSB-first */
            return 0;
        }
    }
#endif
    memcpy(o, wlan_mac(), 6); return 0;
}
static inline int esp_efuse_mac_get_default(uint8_t *o) { return esp_read_mac(o, ESP_MAC_WIFI_STA); }
