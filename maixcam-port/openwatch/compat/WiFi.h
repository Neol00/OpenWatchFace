/* compat/WiFi.h — Arduino WiFi stub (no radio yet; notifications-over-WiFi is a
 * later sockets/libcurl port). Reports "disconnected" so the firmware's net paths
 * no-op cleanly. */
#pragma once
#include <cstdint>
#include <cstdio>
#include "Arduino.h"
#include "owf_maix_hooks.h"   // owf_maix_wifi_* bridge (real WiFi via MaixCDK)

enum wl_status_t { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL, WL_SCAN_COMPLETED,
                   WL_CONNECTED, WL_CONNECT_FAILED, WL_CONNECTION_LOST, WL_DISCONNECTED };
enum { WIFI_OFF = 0, WIFI_STA, WIFI_AP, WIFI_AP_STA };
typedef int wifi_mode_t;
typedef int wifi_power_t;

class IPAddress {
public:
    IPAddress(uint32_t v = 0) : v_(v) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : v_(a | b << 8 | c << 16 | (uint32_t)d << 24) {}
    String toString() const {
        char b[16]; snprintf(b, sizeof b, "%u.%u.%u.%u",
            v_ & 0xff, (v_ >> 8) & 0xff, (v_ >> 16) & 0xff, (v_ >> 24) & 0xff);
        return String(b);
    }
    operator uint32_t() const { return v_; }
private:
    uint32_t v_;
};

class WiFiClass {
public:
    bool        mode(wifi_mode_t) { return true; }   /* STA-only on Maix */
    wl_status_t begin(const char *ssid = nullptr, const char *pass = nullptr) {
        owf_maix_wifi_connect(ssid, pass);           /* kick off association; poll status() */
        return WL_DISCONNECTED;
    }
    wl_status_t status() { return owf_maix_wifi_connected() ? WL_CONNECTED : WL_DISCONNECTED; }
    bool        disconnect(bool = false, bool = false) { owf_maix_wifi_disconnect(); return true; }
    bool        isConnected() { return owf_maix_wifi_connected() != 0; }
    void        setAutoReconnect(bool) {}
    void        setAutoConnect(bool) {}
    void        persistent(bool) {}
    void        setSleep(bool) {}
    bool        setTxPower(wifi_power_t) { return true; }
    int32_t     RSSI() { return owf_maix_wifi_rssi(); }
    IPAddress   localIP() {
        char ip[40] = {0}; owf_maix_wifi_ip(ip, sizeof(ip));
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
            return IPAddress((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
        return IPAddress();
    }
    String      SSID() { return String(); }
    String      macAddress() { return String("02:00:4d:41:49:58"); }
    uint8_t *   macAddress(uint8_t *m) { static const uint8_t f[6]={2,0,0x4d,0x41,0x49,0x58}; if(m) memcpy(m,f,6); return m; }
};

extern WiFiClass WiFi;
