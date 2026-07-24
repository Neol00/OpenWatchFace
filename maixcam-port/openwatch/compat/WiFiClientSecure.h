/* compat/WiFiClientSecure.h — TLS client stub (no networking yet). */
#pragma once
#include "Arduino.h"

class WiFiClient {
public:
    int  connect(const char *, uint16_t) { return 0; }   /* 0 = fail */
    void stop() {}
    bool connected() { return false; }
    size_t write(const uint8_t *, size_t n) { return n; }
    int  available() { return 0; }
    int  read() { return -1; }
    void setTimeout(uint32_t) {}
};

class WiFiClientSecure : public WiFiClient {
public:
    void setCACert(const char *) {}
    void setInsecure() {}
    void setHandshakeTimeout(unsigned long) {}
};
