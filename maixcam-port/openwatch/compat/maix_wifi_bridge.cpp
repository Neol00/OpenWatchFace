/* ============================================================================
 *  maix_wifi_bridge.cpp — low-level WiFi primitives over maix::network::wifi.
 *
 *  These back compat/WiFi.h, so the firmware's OWN autoconnect path
 *  (notif_net.h: wifi_connect() -> WiFi.begin()/status(), driven by the net task)
 *  connects via MaixCDK on the MaixCam — no parallel connect logic here.
 *
 *  connected() is throttle-cached (a real wpa query at most every ~1.5s) so the UI
 *  can poll WiFi.status() every frame cheaply. MaixCDK-only TU (no Arduino.h).
 * ========================================================================== */
#include <string>
#include <mutex>
#include <atomic>
#include <cstring>
#include "maix_basic.hpp"
#include "maix_wifi.hpp"
#include "owf_maix_hooks.h"

using namespace maix;

static network::wifi::Wifi *s_wifi = nullptr;
static std::mutex           s_mtx;
static std::atomic<int>     s_connected{0};
static std::atomic<int>     s_rssi{0};
static std::mutex           s_ip_mtx;
static std::string          s_ip;
static uint64_t             s_last_check = 0;   /* ms; 0 forces an immediate query */

/* lazily open wlan0 (caller holds s_mtx). Retries on later calls if it wasn't up yet. */
static network::wifi::Wifi *wifi_locked()
{
    if (!s_wifi) {
        try { s_wifi = new network::wifi::Wifi(); }
        catch (...) { s_wifi = nullptr; }
    }
    return s_wifi;
}

int owf_maix_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return 0;
    s_last_check = 0;                      /* re-query connection state promptly */
    std::lock_guard<std::mutex> lk(s_mtx);
    auto w = wifi_locked();
    if (!w) return 0;
    try {
        /* wait=false: kick off association and return; status() tracks the result. */
        return w->connect(ssid, pass ? pass : "", false, 20) == err::ERR_NONE ? 1 : 0;
    } catch (...) { return 0; }
}

void owf_maix_wifi_disconnect(void)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    auto w = wifi_locked();
    if (w) { try { w->disconnect(); } catch (...) {} }
    s_connected = 0;
}

int owf_maix_wifi_connected(void)
{
    uint64_t now = time::ticks_ms();
    if (s_last_check != 0 && now - s_last_check < 1500)
        return s_connected.load();          /* cached */

    s_last_check = now;
    bool conn = false;
    std::string ip;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        auto w = wifi_locked();
        if (w) {
            try { conn = w->is_connected(); } catch (...) {}
            if (conn) { try { ip = w->get_ip(); } catch (...) {} }
        }
    }
    s_connected = conn ? 1 : 0;
    { std::lock_guard<std::mutex> lk(s_ip_mtx); s_ip = ip; }
    return s_connected.load();
}

int owf_maix_wifi_rssi(void) { return s_rssi.load(); }   /* current-link RSSI not exposed; 0 */

int owf_maix_wifi_ip(char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    std::lock_guard<std::mutex> lk(s_ip_mtx);
    strncpy(buf, s_ip.c_str(), cap - 1);
    buf[cap - 1] = 0;
    return (int)s_ip.size();
}
