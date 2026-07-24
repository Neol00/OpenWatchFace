/* compat/HTTPClient.h — Arduino HTTPClient stub (no networking yet). All requests
 * fail cleanly so notif_net's fetch path no-ops. */
#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"

#define HTTP_CODE_OK 200

class HTTPClient {
public:
    bool begin(const String &) { return true; }
    bool begin(WiFiClient &, const String &) { return true; }
    bool begin(const char *) { return true; }
    void end() {}
    void addHeader(const String &, const String &) {}
    void setTimeout(uint16_t) {}
    void setConnectTimeout(int32_t) {}
    void setReuse(bool) {}
    int  GET()  { return -1; }   /* negative = connection failed */
    int  POST(const String &) { return -1; }
    String getString() { return String(); }
    int  getSize() { return -1; }
    WiFiClient &getStream() { static WiFiClient c; return c; }
};
