/* tls_client.cpp — the TLS transport behind compat WiFiClientSecure on the
 * Wear 2100 watches: mbedTLS 3.6 (already vendored for NimBLE's crypto) as a
 * TLS 1.2 client over the blocking lwIP TCP helpers in platform/wlan_net.c.
 *
 * One session at a time is all the app ever needs (the update check, the
 * download, the notification poll are sequential), but the state is per
 * handle so two could coexist. Memory: ~20 KB of record buffers + the parsed
 * chain, all malloc()ed from the DDR heap — no internal-SRAM squeeze like the
 * ESP32 has.
 *
 * Verification is REQUIRED against the CA the caller set (the app pins ISRG
 * Root X1 + USERTrust ECC in ota_ca.h) and the hostname is checked via SNI /
 * subjectAltName. What is NOT checked is certificate dates — see the note in
 * owf_mbedtls_config.h.
 *
 * The watchdog is petted from the blocking loops: a handshake with a
 * RSA-4096 root on a 1.1 GHz A7 takes ~1-2 s, a stalled peer up to the
 * caller's timeout, and only the main loop pets otherwise. */
extern "C" {
#include "platform.h"
}
#if PLAT_WLAN_APP
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "mbedtls/net_sockets.h"   /* MBEDTLS_ERR_NET_* codes only */
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

extern "C" void vTaskDelay(unsigned long ticks);

struct TlsConn {
    void                    *tcp;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    bool                     up;
    char                     err[96];
};

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    TlsConn *c = (TlsConn *)ctx;
    int r = net_tcp_write(c->tcp, buf, (uint32_t)len, 8000);
    if (r < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    if (r == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    TlsConn *c = (TlsConn *)ctx;
    int r = net_tcp_read(c->tcp, buf, (uint32_t)len);
    if (r > 0) return r;
    if (r == 0) return net_tcp_connected(c->tcp) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_CONN_RESET;
}

/* No mbedtls_strerror: error.c is generated and not in the vendored tree.
 * The hex code is what the mbedTLS docs index anyway. */
static void set_err(TlsConn *c, const char *what, int rc)
{
    snprintf(c->err, sizeof c->err, "%s: -0x%04x", what, (unsigned)(-rc));
}

extern "C" void *tls_connect(const char *host, uint16_t port, const char *ca_pem,
                             uint32_t connect_ms, uint32_t handshake_ms, char *err, size_t errcap)
{
    TlsConn *c = (TlsConn *)calloc(1, sizeof(TlsConn));
    if (!c) { if (err) snprintf(err, errcap, "no memory"); return nullptr; }
    mbedtls_ssl_init(&c->ssl); mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca); mbedtls_entropy_init(&c->entropy); mbedtls_ctr_drbg_init(&c->drbg);

    int rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                                   (const unsigned char *)"owf-tls", 7);
    if (rc) { set_err(c, "drbg seed", rc); goto fail; }
    if (!ca_pem || !ca_pem[0]) { snprintf(c->err, sizeof c->err, "no CA set (setInsecure is not supported)"); goto fail; }
    rc = mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)ca_pem, strlen(ca_pem) + 1);
    if (rc < 0) { set_err(c, "CA parse", rc); goto fail; }
    rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) { set_err(c, "ssl defaults", rc); goto fail; }
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, nullptr);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc) { set_err(c, "ssl setup", rc); goto fail; }
    rc = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (rc) { set_err(c, "hostname", rc); goto fail; }

    {
        uint32_t ip;
        if (net_dns(host, &ip, 5000) < 0) { snprintf(c->err, sizeof c->err, "DNS failed for %s", host); goto fail; }
        c->tcp = net_tcp_connect(ip, port, connect_ms);
        if (!c->tcp) { snprintf(c->err, sizeof c->err, "TCP connect failed"); goto fail; }
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, nullptr);

    {
        uint32_t t0 = timer_ms();
        for (;;) {
            rc = mbedtls_ssl_handshake(&c->ssl);
            if (rc == 0) break;
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                    uint32_t fl = mbedtls_ssl_get_verify_result(&c->ssl);
                    snprintf(c->err, sizeof c->err, "cert verify failed (flags 0x%lx)", (unsigned long)fl);
                } else set_err(c, "handshake", rc);
                goto fail;
            }
            if ((uint32_t)(timer_ms() - t0) > handshake_ms) { snprintf(c->err, sizeof c->err, "handshake timeout"); goto fail; }
            net_keepalive();
            vTaskDelay(1);
        }
    }
    con_dbg("tls: "); con_dbg(host); con_dbg(" up, "); con_dbg(mbedtls_ssl_get_ciphersuite(&c->ssl)); con_dbg("\n");
    c->up = true;
    return c;

fail:
    con_dbg("tls: "); con_dbg(c->err); con_dbg("\n");
    if (err) snprintf(err, errcap, "%s", c->err);
    if (c->tcp) net_tcp_close(c->tcp);
    mbedtls_ssl_free(&c->ssl); mbedtls_ssl_config_free(&c->conf); mbedtls_x509_crt_free(&c->ca);
    mbedtls_ctr_drbg_free(&c->drbg); mbedtls_entropy_free(&c->entropy);
    free(c);
    return nullptr;
}

extern "C" int tls_write(void *h, const void *data, size_t len)
{
    TlsConn *c = (TlsConn *)h;
    size_t done = 0;
    if (!c || !c->up) return -1;
    while (done < len) {
        int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data + done, len - done);
        if (r > 0) { done += (size_t)r; continue; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) { net_keepalive(); vTaskDelay(1); continue; }
        c->up = false; return -1;
    }
    return (int)done;
}

/* >0 bytes, 0 nothing available right now, <0 closed */
extern "C" int tls_read(void *h, void *buf, size_t max)
{
    TlsConn *c = (TlsConn *)h;
    if (!c || !c->up) return -1;
    int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, max);
    if (r > 0) return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) { c->up = false; return -1; }
    c->up = false; return -1;
}

extern "C" int tls_available(void *h)
{
    TlsConn *c = (TlsConn *)h;
    if (!c || !c->up) return 0;
    size_t n = mbedtls_ssl_get_bytes_avail(&c->ssl);
    if (n) return (int)n;
    return net_tcp_available(c->tcp) > 0 ? 1 : 0;     /* a record is waiting */
}

extern "C" int tls_connected(void *h)
{
    TlsConn *c = (TlsConn *)h;
    return c && c->up && (net_tcp_connected(c->tcp) || mbedtls_ssl_get_bytes_avail(&c->ssl) > 0);
}

extern "C" void tls_close(void *h)
{
    TlsConn *c = (TlsConn *)h;
    if (!c) return;
    if (c->up) mbedtls_ssl_close_notify(&c->ssl);
    if (c->tcp) net_tcp_close(c->tcp);
    mbedtls_ssl_free(&c->ssl); mbedtls_ssl_config_free(&c->conf); mbedtls_x509_crt_free(&c->ca);
    mbedtls_ctr_drbg_free(&c->drbg); mbedtls_entropy_free(&c->entropy);
    free(c);
}
#endif /* PLAT_WLAN_APP */
