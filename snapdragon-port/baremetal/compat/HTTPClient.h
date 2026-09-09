/* HTTPClient.h — the subset of the ESP32 HTTPClient the app uses, over
 * WiFiClient / WiFiClientSecure: begin(client, url) / begin(url), headers,
 * GET, getString, getSize, end. http:// and https:// (the latter needs a
 * WiFiClientSecure with a CA set). Handles Content-Length, chunked bodies
 * and up to 5 redirects, including a redirect to another host — which is
 * what github.com does for release assets.
 *
 * The body is held in one malloc()ed buffer. The default cap is 256 KB (the
 * GitHub release JSON fits with room); the over-the-air installer raises it
 * with setBodyLimit() to take a whole boot image, which on a 512 MB watch is
 * nothing. Bytes past the cap are read and discarded so the connection ends
 * cleanly. */
#pragma once
#include <Arduino.h>
#include "WiFiClientSecure.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define HTTPC_STRICT_FOLLOW_REDIRECTS 1
#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_SEND_HEADER_FAILED (-2)
#define HTTPC_ERROR_NOT_CONNECTED      (-4)
#define HTTPC_ERROR_READ_TIMEOUT       (-11)
class HTTPClient {
public:
    ~HTTPClient() { end(); freeBody(); }
    bool begin(WiFiClient &c, const String &url) { client_ = &c; own_ = false; return parseUrl(url.c_str()); }
    bool begin(const String &url) { own_ = true; client_ = &ownClient_; return parseUrl(url.c_str()); }
    bool begin(const char *url) { return begin(String(url)); }
    void end() { if (client_) client_->stop(); }
    void setTimeout(uint16_t ms) { timeout_ = ms; }
    void setConnectTimeout(int32_t ms) { ctimeout_ = (uint32_t)ms; }
    void addHeader(const String &name, const String &value) { headers_ += name; headers_ += ": "; headers_ += value; headers_ += "\r\n"; }
    void setFollowRedirects(int) { follow_ = true; }
    void setRedirectLimit(int n) { hops_ = n; }
    void setUserAgent(const String &ua) { ua_ = ua; }
    void setBodyLimit(size_t bytes) { limit_ = bytes; }
    /* progress while the body streams in: (received, content-length or 0) */
    void onBody(void (*fn)(size_t, size_t, void *), void *arg) { prog_ = fn; parg_ = arg; }
    int GET() {
        for (int hop = 0; hop <= hops_; hop++) {
            int code = doGet();
            if (follow_ && (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && location_.length()) {
                if (!parseUrl(location_.c_str())) return code;
                continue;
            }
            return code;
        }
        return -1;
    }
    String getString() { return body_ ? String(body_) : String(); }
    int getSize() { return (int)blen_; }
    const uint8_t *getBody() const { return (const uint8_t *)body_; }
    size_t getBodyLength() const { return blen_; }
    /* hand the buffer to the caller (who then owns and free()s it) */
    uint8_t *takeBody(size_t *len) { uint8_t *b = (uint8_t *)body_; if (len) *len = blen_; body_ = nullptr; blen_ = bcap_ = 0; return b; }
    bool truncated() const { return truncated_; }
private:
    bool parseUrl(const char *url) {
        const char *p = url;
        if (!strncmp(p, "http://", 7)) { p += 7; secure_ = false; port_ = 80; }
        else if (!strncmp(p, "https://", 8)) { p += 8; secure_ = true; port_ = 443; }
        else return false;
        const char *slash = strchr(p, '/');
        String hostport = slash ? String(p).substring(0, (unsigned)(slash - p)) : String(p);
        path_ = slash ? String(slash) : String("/");
        int colon = hostport.indexOf(':');
        if (colon >= 0) { host_ = hostport.substring(0, colon); port_ = (uint16_t)atoi(hostport.c_str() + colon + 1); }
        else host_ = hostport;
        return host_.length() > 0;
    }
    void freeBody() { free(body_); body_ = nullptr; blen_ = bcap_ = 0; truncated_ = false; }
    bool append(const uint8_t *b, size_t n) {
        if (blen_ + n > limit_) { truncated_ = true; n = limit_ > blen_ ? limit_ - blen_ : 0; if (!n) return true; }
        if (blen_ + n + 1 > bcap_) {
            size_t nc = bcap_ ? bcap_ : 4096;
            while (nc < blen_ + n + 1) nc *= 2;
            if (nc > limit_ + 1) nc = limit_ + 1;
            char *nb = (char *)realloc(body_, nc);
            if (!nb) { truncated_ = true; return false; }
            body_ = nb; bcap_ = nc;
        }
        memcpy(body_ + blen_, b, n); blen_ += n; body_[blen_] = 0;
        return true;
    }
    int doGet() {
        freeBody(); location_ = String();
        if (!client_) return HTTPC_ERROR_NOT_CONNECTED;
        if (secure_ && !client_->isSecure()) return HTTPC_ERROR_CONNECTION_REFUSED;
        client_->stop();
        client_->setTimeout(ctimeout_);
        if (!client_->connect(host_.c_str(), port_)) return HTTPC_ERROR_CONNECTION_REFUSED;
        client_->setTimeout(timeout_);
        String req; req.reserve(256 + headers_.length());
        req += "GET "; req += path_; req += " HTTP/1.1\r\nHost: "; req += host_;
        req += "\r\nUser-Agent: "; req += ua_; req += "\r\nConnection: close\r\nAccept: */*\r\n"; req += headers_; req += "\r\n";
        if (client_->write((const uint8_t *)req.c_str(), req.length()) != req.length()) { client_->stop(); return HTTPC_ERROR_SEND_HEADER_FAILED; }
        /* status line + headers */
        String hdr; int code = 0; long clen = -1; bool chunked = false;
        if (!readLine(hdr)) { client_->stop(); return HTTPC_ERROR_READ_TIMEOUT; }
        if (hdr.length() > 12 && hdr.startsWith("HTTP/")) code = atoi(hdr.c_str() + 9);
        for (;;) {
            if (!readLine(hdr)) break;
            if (hdr.length() == 0) break;
            String low = hdr; low.toLowerCase();
            if (low.startsWith("content-length:")) clen = atol(hdr.c_str() + 15);
            else if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) chunked = true;
            else if (low.startsWith("location:")) { location_ = hdr.substring(9); location_.trim(); }
        }
        total_ = clen > 0 ? (size_t)clen : 0;
        /* body */
        if (chunked) {
            for (;;) {
                if (!readLine(hdr)) break;
                long n = strtol(hdr.c_str(), nullptr, 16);
                if (n <= 0) break;
                if (!readBytes((size_t)n)) break;
                readLine(hdr);                       /* CRLF after the chunk */
            }
        } else if (clen >= 0) {
            readBytes((size_t)clen);
        } else {
            readBytes(limit_);                       /* until close */
        }
        client_->stop();
        return code;
    }
    bool readLine(String &out) {
        out = String(); uint8_t b;
        for (int i = 0; i < 4096; i++) {
            int r = client_->readTimeout(&b, 1, timeout_);
            if (r <= 0) return out.length() > 0;
            if (b == '\n') return true;
            if (b != '\r') out += (char)b;
        }
        return true;
    }
    bool readBytes(size_t n) {
        static uint8_t buf[4096];
        size_t last = 0;
        while (n) {
            size_t want = n < sizeof buf ? n : sizeof buf;
            int r = client_->readTimeout(buf, want, timeout_);
            if (r <= 0) return false;
            append(buf, (size_t)r);
            n -= (size_t)r;
            if (prog_ && blen_ - last >= 32768) { last = blen_; prog_(blen_, total_, parg_); }
        }
        if (prog_) prog_(blen_, total_, parg_);
        return true;
    }
    WiFiClient *client_ = nullptr, ownClient_;
    bool own_ = false, secure_ = false, follow_ = false, truncated_ = false;
    String host_, path_, headers_, location_, ua_ = "OpenWatchFace";
    char *body_ = nullptr; size_t blen_ = 0, bcap_ = 0, limit_ = 256 * 1024, total_ = 0;
    void (*prog_)(size_t, size_t, void *) = nullptr; void *parg_ = nullptr;
    int hops_ = 5;
    uint16_t port_ = 80, timeout_ = 6000; uint32_t ctimeout_ = 6000;
};
