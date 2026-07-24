/* ============================================================================
 *  tuya/compat/HTTPClient.h - ESP32 HTTPClient shim over the REAL T5 HTTP client
 *  (http_client_request, with built-in TLS + pinned-CA validation).
 *
 *  The firmware fetches notifications over HTTPS:
 *      WiFiClientSecure client; client.setCACert(NOTIFY_ROOT_CA);
 *      HTTPClient http; http.begin(client, NOTIFY_URL);
 *      http.setTimeout(5000); http.addHeader("Authorization", "Bearer ...");
 *      int code = http.GET(); String body = http.getString(); http.end();
 *  The TuyaOpen HTTP API is shaped differently (one GET call takes headers+CA+url),
 *  so this presents the incremental ESP32 surface and drives http_client_request()
 *  underneath. TLS is REAL - the pinned CA (from the WiFiClientSecure) is passed to
 *  the request and validated against the server cert.
 *
 *  begin(client, url) parses the https URL into host/port/path (the Tuya request
 *  struct wants them split). Only GET is used by the firmware.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (gated per-include in notif_net.h).
 * ========================================================================== */
#pragma once
#include <Arduino.h>   // String
#include "WiFiClientSecure.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "http_client_interface.h"
}

class HTTPClient {
public:
    HTTPClient() {}
    ~HTTPClient() { freeResp(); }

    /* ESP32 form: begin(secureClient, url). Captures the CA from the client. */
    bool begin(WiFiClientSecure &client, const String &url) {
        _ca = client.caCert(); _ca_len = client.caLen();
        if (client.timeout()) _timeout_ms = client.timeout();
        return parseUrl(url);
    }
    bool begin(const String &url) { _ca = nullptr; _ca_len = 0; return parseUrl(url); }

    void setTimeout(uint32_t ms) { _timeout_ms = ms; }
    void setConnectTimeout(uint32_t ms) { if (ms > _timeout_ms) _timeout_ms = ms; }  // one timeout knob on the Tuya client
    void setReuse(bool) {}

    void addHeader(const String &name, const String &value) {
        if (_nhdr >= MAX_HDR) return;
        _hk[_nhdr] = name; _hv[_nhdr] = value; _nhdr++;
    }

    /* Returns the HTTP status code (>0) or a negative error, mirroring ESP32. */
    int GET() {
        freeResp();
        http_client_header_t hdrs[MAX_HDR];
        for (int i = 0; i < _nhdr; i++) { hdrs[i].key = _hk[i].c_str(); hdrs[i].value = _hv[i].c_str(); }

        http_client_request_t req = {};
        req.host          = _host.c_str();
        req.port          = _port;
        req.path          = _path.c_str();
        req.cacert        = (const uint8_t *)_ca;
        req.cacert_len    = _ca_len;
        req.tls_no_verify = (_ca == nullptr);
        req.method        = (char *)"GET";
        req.headers       = _nhdr ? hdrs : nullptr;
        req.headers_count = (uint8_t)_nhdr;
        req.body          = nullptr;
        req.body_length   = 0;
        req.timeout_ms    = _timeout_ms;

        http_client_status_t st = http_client_request(&req, &_resp);
        _have_resp = true;
        if (st != HTTP_CLIENT_SUCCESS) return -1;          // transport/TLS failure
        return _resp.status_code ? (int)_resp.status_code : -1;
    }

    String getString() {
        if (!_have_resp || !_resp.body || !_resp.body_length) return String();
        return String((const char *)_resp.body, (unsigned)_resp.body_length);
    }

    void end() { freeResp(); _nhdr = 0; }

private:
    static const int MAX_HDR = 8;

    bool parseUrl(const String &url) {
        /* https://host[:port]/path   (the firmware always uses https). */
        int scheme = url.indexOf("://");
        if (scheme < 0) return false;
        _port = 443;
        int hstart = scheme + 3;
        int pstart = url.indexOf('/', hstart);
        String hostport = (pstart < 0) ? url.substring(hstart) : url.substring(hstart, pstart);
        _path = (pstart < 0) ? String("/") : url.substring(pstart);
        int colon = hostport.indexOf(':');
        if (colon < 0) { _host = hostport; }
        else { _host = hostport.substring(0, colon); _port = (uint16_t)hostport.substring(colon + 1).toInt(); }
        return _host.length() > 0;
    }
    void freeResp() { if (_have_resp) { http_client_free(&_resp); _have_resp = false; } }

    String   _host, _path;
    uint16_t _port = 443;
    const char *_ca = nullptr;
    size_t   _ca_len = 0;
    uint32_t _timeout_ms = 5000;

    String _hk[MAX_HDR], _hv[MAX_HDR];
    int    _nhdr = 0;

    http_client_response_t _resp = {};
    bool _have_resp = false;
};
