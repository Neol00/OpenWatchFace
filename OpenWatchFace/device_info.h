/* ============================================================================
 *  device_info.h — Central product identity (name, version, author, radio name).
 *
 *  Single source of truth for what this firmware calls itself. Surfaced in the
 *  boot log and the Settings > About screen, and (later) used for the BLE
 *  advertising name and WiFi-AP SSID once provisioning lands.
 *
 *  Included early in WatchFace's main sketch so every module can see these.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include "esp_mac.h"   // esp_efuse_mac_get_default — per-unit MAC octets for the radio name

#define DEVICE_NAME    "OpenWatchFace"        // full product name (chip-neutral; supports non-ESP32 boards)
#define DEVICE_SHORT   "WatchFace"            // short form for radio names (BLE/Gadgetbridge — kept stable so paired phones don't need re-pairing)
#define DEVICE_VERSION "1.2.0"                // firmware version
#define DEVICE_AUTHOR  "Noel Ejemyr"          // project author
#define DEVICE_VENDOR  "Waveshare"            // hardware vendor
#define DEVICE_BOARD   BOARD_NAME             // board model — follows the selected board (board.h)

/* Phone-friendly radio name: "WatchFace-AB12" with a per-unit suffix, so multiple
 * units stay distinct in a device list. Kept short to fit BLE/SSID length limits.
 *
 * The suffix used to come from ESP.getEfuseMac()'s LOW bytes — but that 64-bit
 * value stores the 6 MAC octets with octet 0 (the Espressif OUI, IDENTICAL across
 * every chip) in the LOW byte and the unique device octets in the HIGH bytes. So
 * (uint8_t)mac / (uint8_t)(mac>>8) were reading the shared OUI tail, and two
 * different boards collided. Read the genuinely-unique octets via the byte API
 * (esp_efuse_mac_get_default: out[0]=OUI .. out[5]=most-unique) instead. */
static inline String deviceRadioName() {
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", m[4], m[5]);   // unique device octets
  return String(DEVICE_SHORT "-") + suffix;
}
