/* wlan_crypto.c — the primitives WPA2-PSK needs and nothing more:
 * SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1 (passphrase -> PMK, RFC 2898 / IEEE
 * 802.11i H.4), the 802.11i PRF (PTK derivation), AES-128 decrypt and the
 * RFC 3394 key unwrap (GTK inside message 3). Straight, unoptimised
 * implementations; a full TLS library (mbedTLS) arrives with the IP stack. */
#include "platform.h"
#include <string.h>

/* ---- SHA-1 --------------------------------------------------------------- */
static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
static void sha1_block(struct sha1 *c, const uint8_t *p)
{
    uint32_t w[80], a, b, cc, d, e, i, t;
    for (i = 0; i < 16; i++) w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);            k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
void sha1_init(struct sha1 *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu; c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->len = 0; c->n = 0;
}
void sha1_update(struct sha1 *c, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        uint32_t k = 64u - c->n; if (k > len) k = len;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; len -= k;
        if (c->n == 64u) { sha1_block(c, c->buf); c->n = 0; }
    }
}
void sha1_final(struct sha1 *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8u; uint8_t pad = 0x80; uint8_t z = 0; int i;
    sha1_update(c, &pad, 1);
    while (c->n != 56u) sha1_update(c, &z, 1);
    for (i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); sha1_update(c, &b, 1); }
    for (i = 0; i < 5; i++) { out[4*i] = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16); out[4*i+2] = (uint8_t)(c->h[i] >> 8); out[4*i+3] = (uint8_t)c->h[i]; }
}

void hmac_sha1(const uint8_t *key, uint32_t klen, const uint8_t *msg, uint32_t mlen, uint8_t out[20])
{
    uint8_t k[64], ipad[64], opad[64], inner[20]; struct sha1 c; uint32_t i;
    memset(k, 0, 64);
    if (klen > 64u) { sha1_init(&c); sha1_update(&c, key, klen); sha1_final(&c, k); }
    else memcpy(k, key, klen);
    for (i = 0; i < 64u; i++) { ipad[i] = k[i] ^ 0x36u; opad[i] = k[i] ^ 0x5Cu; }
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, msg, mlen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

/* HMAC over several pieces (avoids concatenation buffers) */
static void hmac_sha1_v(const uint8_t *key, uint32_t klen, const uint8_t *const *parts, const uint32_t *plens, uint32_t np, uint8_t out[20])
{
    uint8_t k[64], ipad[64], opad[64], inner[20]; struct sha1 c; uint32_t i;
    memset(k, 0, 64);
    if (klen > 64u) { sha1_init(&c); sha1_update(&c, key, klen); sha1_final(&c, k); }
    else memcpy(k, key, klen);
    for (i = 0; i < 64u; i++) { ipad[i] = k[i] ^ 0x36u; opad[i] = k[i] ^ 0x5Cu; }
    sha1_init(&c); sha1_update(&c, ipad, 64);
    for (i = 0; i < np; i++) sha1_update(&c, parts[i], plens[i]);
    sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

/* PBKDF2-HMAC-SHA1, 4096 iterations, 32-byte output: the WPA2 PMK. */
void wpa_pmk_from_passphrase(const char *pass, const uint8_t *ssid, uint32_t ssid_len, uint8_t pmk[32])
{
    uint32_t blk, i, j, plen = (uint32_t)strlen(pass);
    uint8_t u[20], t[20], salt[36];
    for (blk = 1; blk <= 2u; blk++) {
        memcpy(salt, ssid, ssid_len);
        salt[ssid_len] = 0; salt[ssid_len + 1] = 0; salt[ssid_len + 2] = 0; salt[ssid_len + 3] = (uint8_t)blk;
        hmac_sha1((const uint8_t *)pass, plen, salt, ssid_len + 4u, u);
        memcpy(t, u, 20);
        for (i = 1; i < 4096u; i++) {
            hmac_sha1((const uint8_t *)pass, plen, u, 20, u);
            for (j = 0; j < 20u; j++) t[j] ^= u[j];
            if ((i & 0x3FFu) == 0u) wdog_pet();
        }
        if (blk == 1u) memcpy(pmk, t, 20); else memcpy(pmk + 20, t, 12);   /* PMK = T1 || T2[0..11] */
    }
}

/* 802.11i PRF-384: PTK = PRF(PMK, "Pairwise key expansion", min(AA,SPA)||max(AA,SPA)||min(N)||max(N)) */
void wpa_ptk(const uint8_t pmk[32], const uint8_t *aa, const uint8_t *spa,
             const uint8_t *anonce, const uint8_t *snonce, uint8_t ptk[48])
{
    static const char label[] = "Pairwise key expansion";
    uint8_t data[76], out[20], ctr; uint32_t i, off = 0;
    const uint8_t *m1 = memcmp(aa, spa, 6) < 0 ? aa : spa, *m2 = m1 == aa ? spa : aa;
    const uint8_t *n1 = memcmp(anonce, snonce, 32) < 0 ? anonce : snonce, *n2 = n1 == anonce ? snonce : anonce;
    memcpy(data, m1, 6); memcpy(data + 6, m2, 6); memcpy(data + 12, n1, 32); memcpy(data + 44, n2, 32);
    for (ctr = 0; off < 48u; ctr++) {
        const uint8_t zero = 0;
        const uint8_t *parts[4] = { (const uint8_t *)label, &zero, data, &ctr };
        const uint32_t plens[4] = { sizeof label - 1u, 1u, 76u, 1u };
        hmac_sha1_v(pmk, 32, parts, plens, 4, out);
        for (i = 0; i < 20u && off < 48u; i++) ptk[off++] = out[i];
    }
}

/* ---- AES-128 (decrypt only, for RFC 3394 unwrap) ------------------------- */
static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static uint8_t inv_sbox[256];
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0u)); }
static uint8_t mul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1u) r ^= a; a = xt(a); b >>= 1; } return r; }

static void aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
    uint32_t i; uint8_t rcon = 1;
    memcpy(rk, key, 16);
    for (i = 16; i < 176u; i += 4) {
        uint8_t t[4]; memcpy(t, rk + i - 4, 4);
        if ((i % 16u) == 0u) {
            uint8_t tmp = t[0]; t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[tmp];
            rcon = xt(rcon);
        }
        rk[i] = rk[i-16] ^ t[0]; rk[i+1] = rk[i-15] ^ t[1]; rk[i+2] = rk[i-14] ^ t[2]; rk[i+3] = rk[i-13] ^ t[3];
    }
}
static void aes128_decrypt_block(const uint8_t rk[176], uint8_t s[16])
{
    int r; uint32_t i;
    if (!inv_sbox[1]) for (i = 0; i < 256u; i++) inv_sbox[sbox[i]] = (uint8_t)i;
    for (i = 0; i < 16u; i++) s[i] ^= rk[160 + i];
    for (r = 9; r >= 0; r--) {
        uint8_t t[16];
        /* inv shift rows */
        for (i = 0; i < 16u; i++) t[i] = s[i];
        for (i = 0; i < 4u; i++) { s[4*i+1] = t[4*((i+3)%4)+1]; s[4*i+2] = t[4*((i+2)%4)+2]; s[4*i+3] = t[4*((i+1)%4)+3]; }
        for (i = 0; i < 16u; i++) s[i] = inv_sbox[s[i]];
        for (i = 0; i < 16u; i++) s[i] ^= rk[r * 16 + i];
        if (r > 0) {
            for (i = 0; i < 4u; i++) {
                uint8_t a0 = s[4*i], a1 = s[4*i+1], a2 = s[4*i+2], a3 = s[4*i+3];
                s[4*i]   = mul(a0,14) ^ mul(a1,11) ^ mul(a2,13) ^ mul(a3,9);
                s[4*i+1] = mul(a0,9)  ^ mul(a1,14) ^ mul(a2,11) ^ mul(a3,13);
                s[4*i+2] = mul(a0,13) ^ mul(a1,9)  ^ mul(a2,14) ^ mul(a3,11);
                s[4*i+3] = mul(a0,11) ^ mul(a1,13) ^ mul(a2,9)  ^ mul(a3,14);
            }
        }
    }
}

/* RFC 3394 unwrap with a 128-bit KEK. in = (n+1)*8 bytes, out = n*8. 0 = ok. */
int aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t inlen, uint8_t *out)
{
    uint8_t rk[176], a[8], b[16]; uint32_t n, i, j, t;
    if (inlen < 24u || (inlen & 7u)) return -1;
    n = inlen / 8u - 1u;
    aes128_expand(kek, rk);
    memcpy(a, in, 8); memcpy(out, in + 8, n * 8u);
    for (j = 6u; j-- > 0u; ) {                 /* j = 5 .. 0 */
        for (i = n; i >= 1u; i--) {
            t = n * j + i;
            memcpy(b, a, 8); b[7] ^= (uint8_t)t; b[6] ^= (uint8_t)(t >> 8);
            memcpy(b + 8, out + (i - 1u) * 8u, 8);
            aes128_decrypt_block(rk, b);
            memcpy(a, b, 8); memcpy(out + (i - 1u) * 8u, b + 8, 8);
        }
    }
    for (i = 0; i < 8u; i++) if (a[i] != 0xA6u) return -1;
    return 0;
}
