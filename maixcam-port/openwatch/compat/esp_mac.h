/* compat/esp_mac.h — MAC read stub. */
#pragma once
#include <cstdint>
#include <cstring>

typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;

static inline int esp_read_mac(uint8_t *out, esp_mac_type_t) {
    static const uint8_t fake[6] = {0x02, 0x00, 0x4d, 0x41, 0x49, 0x58}; /* 02:..:"MAIX" */
    if (out) memcpy(out, fake, 6);
    return 0;
}
static inline int esp_efuse_mac_get_default(uint8_t *out) { return esp_read_mac(out, ESP_MAC_WIFI_STA); }
