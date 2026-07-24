/* ============================================================================
 *  tuya/compat/WiFiClientSecure.h - ESP32 WiFiClientSecure shim (CA holder).
 *
 *  The firmware creates a WiFiClientSecure, pins the notify-server's root CA on it
 *  (setCACert), and hands it to HTTPClient::begin(client, url). On the T5 the TLS +
 *  CA validation happens INSIDE the Tuya HTTP GET call (it takes ca/ca_len args), so
 *  this class is just a typed carrier for the CA pointer that our compat HTTPClient
 *  (tuya/compat/HTTPClient.h) reads back. Real TLS - the cert is actually used.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated per-include in notif_net.h).
 * ========================================================================== */
#pragma once
#include <cstddef>
#include <cstring>

class WiFiClientSecure {
public:
    void setCACert(const char *ca) { _ca = ca; _ca_len = ca ? strlen(ca) : 0; }
    void setInsecure() { _ca = nullptr; _ca_len = 0; }   // no pinning (firmware doesn't use this)
    void setTimeout(uint32_t ms) { _timeout_ms = ms; }

    const char *caCert() const { return _ca; }
    size_t      caLen()  const { return _ca_len; }
    uint32_t    timeout() const { return _timeout_ms; }

private:
    const char *_ca = nullptr;
    size_t      _ca_len = 0;
    uint32_t    _timeout_ms = 5000;
};
