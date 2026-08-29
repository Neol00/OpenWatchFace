/* HTTPClient.h — Arduino HTTPClient STUB (no network yet). GET always fails. */
#pragma once
#include <Arduino.h>
#include "WiFiClientSecure.h"
#define HTTPC_STRICT_FOLLOW_REDIRECTS 1
class HTTPClient {
public:
    bool begin(WiFiClient&, const String&) { return true; }
    bool begin(const String&) { return true; }
    bool begin(const char*) { return true; }
    void end() {}
    void setTimeout(uint16_t) {}
    void setConnectTimeout(int32_t) {}
    void addHeader(const String&, const String&) {}
    void setFollowRedirects(int) {}
    void setUserAgent(const String&) {}
    int  GET() { return -1; }               /* connection failed */
    String getString() { return String(); }
};
