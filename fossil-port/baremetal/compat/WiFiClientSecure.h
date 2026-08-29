/* WiFiClientSecure.h — TLS client STUB (no radio yet). */
#pragma once
#include <cstdint>
#include <cstddef>
class WiFiClient {
public:
    int connect(const char*, uint16_t) { return 0; }
    int connect(uint32_t, uint16_t) { return 0; }
    size_t write(const uint8_t*, size_t n) { return n; }
    int available() { return 0; }
    int read() { return -1; }
    int read(uint8_t*, size_t) { return -1; }
    void stop() {}
    uint8_t connected() { return 0; }
    operator bool() { return false; }
    void setTimeout(uint32_t) {}
};
class WiFiClientSecure : public WiFiClient {
public:
    void setCACert(const char*) {}
    void setInsecure() {}
    void setHandshakeTimeout(unsigned long) {}
};
