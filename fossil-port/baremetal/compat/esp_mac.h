/* esp_mac.h — per-unit MAC. No radio yet: fixed locally-administered address. */
#pragma once
#include <cstdint>
#include <cstring>
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;
static inline int esp_read_mac(uint8_t *o, esp_mac_type_t) {
    static const uint8_t m[6] = {0x02,0x00,'O','W','F','6'};
    if (!o) return -1; memcpy(o, m, 6); return 0;
}
static inline int esp_efuse_mac_get_default(uint8_t *o) { return esp_read_mac(o, ESP_MAC_WIFI_STA); }
