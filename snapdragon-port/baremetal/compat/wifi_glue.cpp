/* wifi_glue.cpp — WiFiClass over platform/wcnss.c (wlan_up/down/scan). */
#include "WiFi.h"
#include <string.h>
#include <stdio.h>
extern "C" {
#include "platform.h"
}
WiFiClass WiFi;

#include "FreeRTOS.h"
#include "semphr.h"
#if defined(PLAT_WLAN_APP)
/* Capacity matches the UI's WSCAN_MAX: the scan list is meant to show every AP
 * in range, so the glue must not be the thing that truncates it first. */
#define WIFI_GLUE_SCAN_MAX 24
static struct wlan_scan_net s_nets[WIFI_GLUE_SCAN_MAX];
static int s_nnets = -1;                  /* -1 = no scan results held */

/* The app drives WiFi from TWO tasks -- the UI (scan screen, settings toggle,
 * sleep entry) and the "net" task (wifi_connect on a schedule). The radio is
 * one set of SMD channels and DMA rings with no locking of its own, so every
 * entry point below is serialised on this mutex (recursive: scanNetworks may
 * call mode()). */
struct WifiLock { WifiLock() { wlan_lock(); } ~WifiLock() { wlan_unlock(); } };   /* the radio's own recursive lock */

bool WiFiClass::mode(wifi_mode_t m)
{
    WifiLock l;
    if (m == WIFI_OFF) return wlan_down() == 0;
    return wlan_up() == 0;
}
wl_status_t WiFiClass::begin(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return WL_CONNECT_FAILED;
    /* Split the wall-clock cost in two, because the two halves have completely
     * different fixes: everything up to "associated + keys installed" is ours
     * (scan strategy, PBKDF2, HAL round-trips), while the DHCP half is the AP's
     * lease server and lwIP's retry backoff. Without this line the answer to
     * "why does WiFi take 20 s" is a guess. */
    uint32_t t_link = timer_ms();
    {
        WifiLock l;                                   /* link: scan, join, WPA2 handshake, keys */
        if (wlan_up() != 0) return WL_CONNECT_FAILED;
        if (wlan_connect(ssid, pass ? pass : "") != 0) return WL_CONNECT_FAILED;
    }
    t_link = timer_ms() - t_link;
    /* IP: DHCP needs the poll task, which needs the lock we just released */
    uint32_t t_dhcp = timer_ms();
    int rc = net_up();
    t_dhcp = timer_ms() - t_dhcp;
    con_puts("[wifi] link "); con_putdec(t_link);
    con_puts(" ms + dhcp "); con_putdec(t_dhcp);
    con_puts(" ms = "); con_putdec(t_link + t_dhcp); con_puts(" ms\n"); con_flush();
    if (rc != 0) { con_puts("[wifi] link up but no IP lease\n"); return WL_DISCONNECTED; }
    return WL_CONNECTED;
}
wl_status_t WiFiClass::status() { return (wlan_connected() && net_has_ip()) ? WL_CONNECTED : WL_DISCONNECTED; }
bool WiFiClass::disconnect(bool wifioff, bool) { WifiLock l; wlan_disconnect(); if (wifioff) wlan_down(); return true; }
int WiFiClass::RSSI() { return wlan_connected() ? wlan_sta_rssi() : -127; }
IPAddressLite WiFiClass::localIP() { IPAddressLite a; a.v = net_ip(); return a; }
int WiFiClass::hostByName(const char *host, IPAddressLite &out) { uint32_t ip; if (net_dns(host, &ip, 5000) < 0) return 0; out.v = ip; return 1; }
int16_t WiFiClass::scanNetworks(bool, bool)
{
    WifiLock l;
    if (wlan_up() != 0) return WIFI_SCAN_FAILED;
    int n = wlan_scan(s_nets, (unsigned)WIFI_GLUE_SCAN_MAX);
    if (n < 0) { s_nnets = -1; return WIFI_SCAN_FAILED; }
    s_nnets = n;
    return (int16_t)n;                     /* synchronous: the result is ready now */
}
int16_t WiFiClass::scanComplete() { return s_nnets < 0 ? WIFI_SCAN_FAILED : (int16_t)s_nnets; }
void WiFiClass::scanDelete() { s_nnets = -1; }
String WiFiClass::SSID(uint8_t i) { return (s_nnets > 0 && i < s_nnets) ? String(s_nets[i].ssid) : String(""); }
int32_t WiFiClass::RSSI(uint8_t i) { return (s_nnets > 0 && i < s_nnets) ? s_nets[i].rssi : -127; }
wifi_auth_mode_t WiFiClass::encryptionType(uint8_t i)
{ return (s_nnets > 0 && i < s_nnets && s_nets[i].secured) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN; }
int32_t WiFiClass::channel(uint8_t i) { return (s_nnets > 0 && i < s_nnets) ? s_nets[i].chan : 0; }
String WiFiClass::BSSIDstr(uint8_t i)
{
    char b[18] = "";
    if (s_nnets > 0 && i < s_nnets) {
        const uint8_t *m = s_nets[i].bssid;
        snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return String(b);
}
#else
bool WiFiClass::mode(wifi_mode_t) { return true; }
wl_status_t WiFiClass::begin(const char *, const char *) { return WL_DISCONNECTED; }
wl_status_t WiFiClass::status() { return WL_DISCONNECTED; }
bool WiFiClass::disconnect(bool, bool) { return true; }
int WiFiClass::RSSI() { return -127; }
int16_t WiFiClass::scanNetworks(bool, bool) { return WIFI_SCAN_FAILED; }
int16_t WiFiClass::scanComplete() { return WIFI_SCAN_FAILED; }
void WiFiClass::scanDelete() {}
String WiFiClass::SSID(uint8_t) { return String(""); }
int32_t WiFiClass::RSSI(uint8_t) { return -127; }
wifi_auth_mode_t WiFiClass::encryptionType(uint8_t) { return WIFI_AUTH_OPEN; }
int32_t WiFiClass::channel(uint8_t) { return 0; }
String WiFiClass::BSSIDstr(uint8_t) { return String(""); }
IPAddressLite WiFiClass::localIP() { return IPAddressLite(); }
int WiFiClass::hostByName(const char *, IPAddressLite &) { return 0; }
#endif
