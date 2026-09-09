/* WiFi.h — Arduino WiFi API over the snapdragon-port WCNSS/wcn36xx stack.
 *
 * On PLAT_WLAN_APP boards (Gen 4, TicWatch C2) mode(WIFI_STA) brings the radio
 * up (rails, PAS firmware load, NV, HAL start, DXE) and mode(WIFI_OFF) takes
 * it down, so the Settings WiFi toggle really powers the Pronto core off.
 * scanNetworks() runs a passive scan and the result accessors serve the app's
 * scan list. begin()/status(): association is not implemented yet, so status
 * stays WL_DISCONNECTED and the app's connect attempts time out cleanly.
 * On other boards (Gen 6) every call is a no-op stub as before. */
#pragma once
#include <Arduino.h>
#include "WiFiClientSecure.h"
#include <cstdint>
#include <stdio.h>
typedef enum { WL_NO_SHIELD=255, WL_IDLE_STATUS=0, WL_NO_SSID_AVAIL, WL_SCAN_COMPLETED,
               WL_CONNECTED, WL_CONNECT_FAILED, WL_CONNECTION_LOST, WL_DISCONNECTED } wl_status_t;
typedef enum { WIFI_OFF=0, WIFI_STA, WIFI_AP, WIFI_AP_STA } wifi_mode_t;
typedef enum { WIFI_AUTH_OPEN=0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
               WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_PSK, WIFI_AUTH_MAX } wifi_auth_mode_t;
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED  (-2)
class IPAddressLite { public: uint32_t v=0; operator uint32_t(){return v;}
    String toString() const { char b[16]; snprintf(b, sizeof b, "%u.%u.%u.%u", (unsigned)(v >> 24), (unsigned)((v >> 16) & 255), (unsigned)((v >> 8) & 255), (unsigned)(v & 255)); return String(b); } };
class WiFiClass {
public:
    bool mode(wifi_mode_t m);
    wl_status_t begin(const char* ssid = nullptr, const char* pass = nullptr);
    wl_status_t status();
    bool disconnect(bool wifioff = false, bool eraseap = false);
    int RSSI();
    IPAddressLite localIP();
    int hostByName(const char *host, IPAddressLite &out);
    void setSleep(bool) {}
    void setTxPower(int) {}
    /* scan API (subset the app uses) */
    int16_t scanNetworks(bool async = false, bool show_hidden = false);
    int16_t scanComplete();
    void scanDelete();
    String SSID(uint8_t i);
    int32_t RSSI(uint8_t i);
    wifi_auth_mode_t encryptionType(uint8_t i);
    int32_t channel(uint8_t i);
    String BSSIDstr(uint8_t i);
};
extern WiFiClass WiFi;
