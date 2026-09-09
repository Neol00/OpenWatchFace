/* WiFiClientSecure.h — Arduino TCP client classes over platform/wlan_net.c.
 * WiFiClient is a real blocking TCP client (lwIP raw API behind a small
 * ring buffer). WiFiClientSecure is TLS 1.2 via mbedTLS (compat/tls_client.cpp)
 * on the boards that have the radio (PLAT_WLAN_APP); elsewhere connect()
 * fails cleanly so HTTPS users report "no connection" instead of hanging. */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
extern "C" {
#include "platform.h"            /* net_dns / net_tcp_* / timer_ms / wdog_pet */
void     vTaskDelay(unsigned long ticks);
#if PLAT_WLAN_APP
void    *tls_connect(const char *host, uint16_t port, const char *ca_pem, uint32_t connect_ms, uint32_t handshake_ms, char *err, size_t errcap);
int      tls_write(void *h, const void *data, size_t len);
int      tls_read(void *h, void *buf, size_t max);
int      tls_available(void *h);
int      tls_connected(void *h);
void     tls_close(void *h);
#endif
}
class WiFiClient {
public:
    virtual ~WiFiClient() { stop(); }
    virtual int connect(const char *host, uint16_t port) {
        uint32_t ip; stop();
        if (net_dns(host, &ip, 5000) < 0) return 0;
        h_ = net_tcp_connect(ip, port, timeout_);
        return h_ ? 1 : 0;
    }
    virtual int connect(uint32_t ip, uint16_t port) { stop(); h_ = net_tcp_connect(ip, port, timeout_); return h_ ? 1 : 0; }
    virtual size_t write(const uint8_t *b, size_t n) { if (!h_) return 0; int r = net_tcp_write(h_, b, (uint32_t)n, timeout_); return r < 0 ? 0 : (size_t)r; }
    size_t write(const char *s) { size_t n = 0; while (s[n]) n++; return write((const uint8_t *)s, n); }
    size_t print(const char *s) { return write(s); }
    virtual int available() { return h_ ? net_tcp_available(h_) : 0; }
    int read() { uint8_t b; return (read(&b, 1) == 1) ? b : -1; }
    virtual int read(uint8_t *b, size_t n) { if (!h_) return -1; int r = net_tcp_read(h_, b, (uint32_t)n); return r; }
    /* blocking read with the client timeout, for the HTTP parser */
    int readTimeout(uint8_t *b, size_t n, uint32_t ms) {
        uint32_t t = timer_ms();
        for (;;) {
            int r = read(b, n);
            if (r != 0) return r;
            if (timer_ms() - t > ms) return 0;
            net_keepalive();            /* long downloads block the loop task: wdog + dead-man + USB log */
            vTaskDelay(2);
        }
    }
    virtual void stop() { if (h_) { net_tcp_close(h_); h_ = nullptr; } }
    virtual uint8_t connected() { return h_ ? (uint8_t)net_tcp_connected(h_) : 0; }
    operator bool() { return h_ != nullptr; }
    void setTimeout(uint32_t ms) { timeout_ = ms; }
    uint32_t getTimeout() const { return timeout_; }
    virtual bool isSecure() const { return false; }
protected:
    void *h_ = nullptr;
    uint32_t timeout_ = 6000;
};
class WiFiClientSecure : public WiFiClient {
public:
    ~WiFiClientSecure() override { stop(); }
#if PLAT_WLAN_APP
    int connect(const char *host, uint16_t port) override {
        stop(); err_[0] = 0;
        h_ = tls_connect(host, port, ca_, timeout_, hs_ms_, err_, sizeof err_);
        return h_ ? 1 : 0;
    }
    int connect(uint32_t, uint16_t) override { return 0; }   /* TLS needs a name for SNI */
    size_t write(const uint8_t *b, size_t n) override { if (!h_) return 0; int r = tls_write(h_, b, n); return r < 0 ? 0 : (size_t)r; }
    int available() override { return h_ ? tls_available(h_) : 0; }
    int read(uint8_t *b, size_t n) override { if (!h_) return -1; return tls_read(h_, b, n); }
    void stop() override { if (h_) { tls_close(h_); h_ = nullptr; } }
    uint8_t connected() override { return h_ ? (uint8_t)tls_connected(h_) : 0; }
#else
    int connect(const char *, uint16_t) override { snprintf(err_, sizeof err_, "no TLS on this board"); return 0; }
    int connect(uint32_t, uint16_t) override { return 0; }
#endif
    void setCACert(const char *pem) { ca_ = pem; }
    void setInsecure() {}                                  /* deliberately not honoured */
    void setHandshakeTimeout(unsigned long s) { hs_ms_ = (uint32_t)s * 1000u; }
    int  lastError(char *buf, size_t cap) { snprintf(buf, cap, "%s", err_); return err_[0] ? 1 : 0; }
    bool isSecure() const override { return true; }
private:
    const char *ca_ = nullptr;
    uint32_t hs_ms_ = 20000;
    char err_[96] = {0};
};
