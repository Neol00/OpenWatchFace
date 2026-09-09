/* owf_mbedtls_config.h — mbedTLS 3.6 trimmed for the snapdragon-port.
 *
 * Two tenants:
 *   1. NimBLE's Security Manager: AES-128 ECB, AES-CMAC, P-256 ECDH.
 *   2. WiFiClientSecure (compat/tls_client.cpp): a TLS 1.2 CLIENT good enough
 *      for GitHub (api.github.com, github.com, *.githubusercontent.com) and
 *      the notification server — ECDHE-RSA / ECDHE-ECDSA with AES-GCM, X.509
 *      chains rooted at ISRG Root X1 (RSA-4096) or USERTrust ECC (P-384),
 *      SNI, server-name verification. No TLS 1.3 (needs the PSA crypto core,
 *      which would double the footprint), no session tickets, no client
 *      certificates, no renegotiation.
 *
 * Certificate validity DATES are NOT checked (MBEDTLS_HAVE_TIME_DATE is off):
 * the watch may not have a clock when it first goes online, and a 1970 clock
 * makes every certificate on earth "not yet valid". Chain and hostname are
 * still verified against the pinned root, so this only loses expiry checks. */
#pragma once
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT          /* mbedtls_hardware_poll(): platform/rng_msm.c */

/* ---- primitives ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CMAC_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C                         /* legacy intermediates only */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE 4
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_GENPRIME                        /* unused, keeps rsa.c whole */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* ---- X.509 + TLS 1.2 client ---- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096        /* we only send small requests */
