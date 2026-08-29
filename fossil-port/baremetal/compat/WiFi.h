/* WiFi.h — Arduino WiFi STUB (no radio on this port yet; Phase 8). */
#pragma once
#include <Arduino.h>
#include "WiFiClientSecure.h"
#include <cstdint>
typedef enum { WL_NO_SHIELD=255, WL_IDLE_STATUS=0, WL_NO_SSID_AVAIL, WL_SCAN_COMPLETED,
               WL_CONNECTED, WL_CONNECT_FAILED, WL_CONNECTION_LOST, WL_DISCONNECTED } wl_status_t;
typedef enum { WIFI_OFF=0, WIFI_STA, WIFI_AP, WIFI_AP_STA } wifi_mode_t;
class IPAddressLite { public: uint32_t v=0; operator uint32_t(){return v;} String toString() const { return String("0.0.0.0"); } };
class WiFiClass {
public:
    bool mode(wifi_mode_t) { return true; }
    wl_status_t begin(const char* = nullptr, const char* = nullptr) { return WL_DISCONNECTED; }
    wl_status_t status() { return WL_DISCONNECTED; }
    bool disconnect(bool = false, bool = false) { return true; }
    int RSSI() { return -127; }
    IPAddressLite localIP() { return IPAddressLite(); }
    int hostByName(const char*, IPAddressLite&) { return 0; }
    void setSleep(bool) {}
    void setTxPower(int) {}
};
extern WiFiClass WiFi;
