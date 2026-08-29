#pragma once
/* ============================================================================
 *  ota_check.h — "is there a newer OpenWatchFace?", and nothing else.
 *
 *  DELIBERATELY THE CHECK ONLY. No download, no flashing, no partition work.
 *  The first question an over-the-air updater has to answer is whether this
 *  device can fetch a signed-by-TLS file from the internet at all, and that is
 *  worth answering on its own before anything is built on top of it — a
 *  download path that fails is indistinguishable from an install path that
 *  fails if both land in the same commit.
 *
 *  WHY THESE URLS. The obvious design fetches the GitHub *API*
 *  (api.github.com/repos/.../releases/latest). Do not: api.github.com and
 *  github.com are served from a Sectigo chain rooted at USERTrust ECC, which
 *  is NOT the CA this firmware pins, so every request would fail certificate
 *  validation. Measured 2026-08-28:
 *
 *      github.com, api.github.com, codeload  -> Sectigo      pin FAILS
 *      raw.githubusercontent.com             -> Let's Encrypt  pin OK
 *      release-assets.githubusercontent.com  -> Let's Encrypt  pin OK
 *      objects.githubusercontent.com         -> Let's Encrypt  pin OK
 *      <user>.github.io                      -> Let's Encrypt  pin OK
 *
 *  So the manifest is a plain FILE COMMITTED TO THE REPO, fetched over
 *  raw.githubusercontent.com — no GitHub API, and therefore no API rate limit
 *  and no token. The firmware image is an ordinary release asset, reached
 *  through the normal github.com/.../releases/download/... URL.
 *
 *  That download URL is why ota_ca.h pins TWO roots: it starts on github.com
 *  (Sectigo) and only then redirects to the Let's Encrypt CDN, so Let's Encrypt
 *  alone fails on the first hop. The CDN URLs cannot be used directly instead —
 *  they are signed and expire. Any host added here must be checked against
 *  OTA_ROOT_CA first, or the feature breaks in the field with a bare
 *  "connect failed".
 *
 *  MANIFEST FORMAT (ota/latest.json in the repo, one object per board):
 *      {
 *        "version": "1.5.0",
 *        "notes":   "short human line shown in the UI",
 *        "builds": {
 *          "ws-s3-amoled-164": { "url": "https://release-assets.../owf-....bin",
 *                                "size": 1746732,
 *                                "sha256": "…64 hex…" }
 *        }
 *      }
 *  Parsed with the same substring approach notif_net.h uses for its payloads —
 *  a real JSON parser is not worth ~4 KB here, and the document is ours.
 * ========================================================================== */

/* SELF-CONTAINED so it can be included BEFORE the app screens that use it —
 * app_wifi_ble.h has the button, and it is included well before notif_net.h.
 * The platform include block mirrors notif_net.h's exactly (real on ESP32,
 * Tuya-shimmed, stubbed on the Fossil watches, whose HTTPClient::GET() returns
 * -1 — so this reports "TLS/conn err -1" on a watch with no radio yet, which
 * is what it should say). No capability gate needed. */
#include "board.h"
#include "device_info.h"        /* DEVICE_VERSION — what we compare against */
#include <WiFi.h>
#if BOARD_PLATFORM_TUYA
#include "tuya/compat/WiFiClientSecure.h"
#include "tuya/compat/HTTPClient.h"
#else
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#endif
#include "ota_ca.h"             /* ISRG Root X1 + USERTrust ECC — see the
                                 * note there on why updates need BOTH */

/* Defined in notif_net.h, later in the translation unit. Declared here so the
 * include order can put this header where the UI needs it. */
static bool wifi_connect(void);

#ifndef OTA_MANIFEST_URL
/* Points at the repo's own file. Branch is pinned deliberately: a manifest on
 * a moving branch is a manifest anyone with push access can point at any
 * binary, and "main" is the branch releases are cut from. */
#define OTA_MANIFEST_URL \
  "https://raw.githubusercontent.com/Neol00/OpenWatchFace/main/ota/latest.json"
#endif

/* The board key this device looks for inside "builds", and the name its image
 * is published under. BOTH COME FROM THE BOARD HEADER (BOARD_OTA_KEY) rather
 * than a ladder here: adding a board should mean editing that board's file and
 * nothing else. A board that has not declared one still compiles — it reports
 * "No build for unknown", which says exactly what to fix.
 *
 * The release asset is named owf-<key>-<version>.bin. That is a convention, not
 * a requirement: the manifest carries the real URL, so a renamed asset only
 * needs the manifest updated. Keeping to it means the filename alone identifies
 * which watch a downloaded image belongs to. */
#ifndef OTA_BOARD_KEY
#  ifdef BOARD_OTA_KEY
#    define OTA_BOARD_KEY BOARD_OTA_KEY
#  else
#    define OTA_BOARD_KEY "unknown"
#  endif
#endif

/* Convenience for the release tooling and the About screen: the exact asset
 * filename this build expects to be published as. */
#define OTA_ASSET_NAME "owf-" OTA_BOARD_KEY "-" DEVICE_VERSION ".bin"

/* What the check found. Deliberately plain data: the UI renders it, the
 * installer (later) consumes it, and neither needs the HTTP layer again. */
struct OtaManifest {
  bool     ok;              /* the fetch AND the parse both succeeded      */
  int      http_code;       /* as returned, or a negative HTTPClient error */
  char     err[48];         /* short reason when !ok — shown verbatim      */
  char     version[24];     /* "1.5.0"                                     */
  char     notes[80];       /* one human line                              */
  char     url[192];        /* firmware image for THIS board               */
  char     sha256[65];      /* 64 hex chars + NUL                          */
  uint32_t size;            /* bytes, for the progress bar and a sanity gate */
  bool     newer;           /* version differs from the running build      */
};

/* ---- version compare ------------------------------------------------------
 * Numeric per component so "1.10.0" beats "1.9.0" — a plain strcmp gets that
 * backwards, which is the classic way an updater silently stops offering
 * updates after the tenth minor release. Missing components read as 0, so
 * "1.5" and "1.5.0" compare equal. */
static int ota_ver_cmp(const char *a, const char *b) {
  while (*a || *b) {
    long x = 0, y = 0;
    while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0');
    while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
    if (x != y) return x < y ? -1 : 1;
    while (*a && *a != '.') a++;
    while (*b && *b != '.') b++;
    if (*a == '.') a++;
    if (*b == '.') b++;
  }
  return 0;
}

/* Copy a JSON string value for `key`, searching from `from`. Returns the index
 * just past the value, or -1. Same shape as notif_net.h's json_find_string so
 * the two read alike; no escape handling, because we author the document. */
static int ota_json_str(const String &s, int from, const char *key,
                        char *out, size_t cap) {
  String pat = String("\"") + key + "\"";
  int k = s.indexOf(pat, from);
  if (k < 0) return -1;
  int c = s.indexOf(':', k + pat.length());
  if (c < 0) return -1;
  int q1 = s.indexOf('"', c);
  if (q1 < 0) return -1;
  int q2 = s.indexOf('"', q1 + 1);
  if (q2 < 0) return -1;
  size_t n = (size_t)(q2 - q1 - 1);
  if (n >= cap) n = cap - 1;
  memcpy(out, s.c_str() + q1 + 1, n);
  out[n] = '\0';
  return q2 + 1;
}

static int ota_json_u32(const String &s, int from, const char *key, uint32_t *out) {
  String pat = String("\"") + key + "\"";
  int k = s.indexOf(pat, from);
  if (k < 0) return -1;
  int c = s.indexOf(':', k + pat.length());
  if (c < 0) return -1;
  *out = (uint32_t)strtoul(s.c_str() + c + 1, nullptr, 10);
  return c + 1;
}

/* ---- what "-1" actually means --------------------------------------------
 * The first field report of this feature was "Check failed: TLS/conn err -1,
 * and it logs nothing", which is two faults. HTTPClient's -1 is a catch-all for
 * "the connection never came up", and it covers at least four causes that need
 * four different fixes: DNS never resolved, TCP was refused, the TLS handshake
 * was rejected, or the socket layer could not get the ~45 KB of contiguous
 * internal SRAM a TLS session needs. Collapsing those into one number leaves
 * nothing to act on — so the check now walks the connection one stage at a
 * time, names the stage that failed, and narrates all of it over USBSerial.
 *
 * The diagnostics are ESP-only. The Fossil watches have no radio yet and their
 * HTTPClient is a stub whose GET() returns -1 by construction; "TLS/conn err
 * -1" is the correct answer there, so they keep the plain path. */
#define OTA_DIAG (!BOARD_PLATFORM_FOSSIL && !BOARD_PLATFORM_MAIX)

#if OTA_DIAG
#include <time.h>
#include "esp_heap_caps.h"
extern HWCDC USBSerial;

/* Defined in notif_net.h, later in the translation unit (like wifi_connect).
 *
 * TLS NEEDS A CLOCK. mbedtls validates notBefore/notAfter on every certificate
 * in the chain, so a device that has never synced sits in 1970 and every
 * certificate on earth reads as "not yet valid" — the handshake fails with a
 * bare -1 indistinguishable from a network fault. Time sync lives on the
 * notification task, so a watch with notifications disabled, or one that was
 * only just booted, arrives here with no clock at all. That is the single most
 * likely cause of an INSTANT failure: it needs no timeout to expire. Sync here
 * rather than assuming something else already did. */
static bool ntp_sync_if_due(bool force);

/* Any epoch past 2023-11 means a real sync landed. The base is what matters
 * here, not precision. */
#define OTA_TIME_SANE(t) ((long)(t) > 1700000000L)

/* ---- why TLS costs internal SRAM, and what can be moved off it ------------
 * mbedtls holds two 16 KB record buffers (MBEDTLS_SSL_IN/OUT_CONTENT_LEN, the
 * maximum a TLS record may be — the peer is entitled to send one that big), a
 * few KB of parsed certificate structures, and the session state. Those are
 * plain malloc()s, so on a PSRAM board they CAN live in PSRAM, and with
 * CONFIG_SPIRAM_USE_MALLOC most already do: the core routes any allocation
 * larger than CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL to external RAM.
 *
 * What CANNOT move is the rest of the path. The WiFi driver's and lwIP's packet
 * buffers must be DMA-capable, and PSRAM is not — every frame in and out of the
 * radio is copied through internal SRAM by hardware that cannot address SPI
 * RAM. That floor is the WiFi stack's, not the updater's, and no amount of
 * PSRAM removes it. So "put TLS in PSRAM" is largely already true and does not
 * buy back the internal SRAM the connection still needs.
 *
 * The two knobs that WOULD help both live in sdkconfig (MBEDTLS_DYNAMIC_BUFFER,
 * and shrinking the record length), which a precompiled Arduino core does not
 * expose — changing them means rebuilding the core.
 *
 * Hence a FLOOR, not a forecast. This refuses only when the largest contiguous
 * internal block is too small for the handshake to have any chance, so the
 * failure is named rather than surfacing as a bare -1. It is deliberately low:
 * a gate set above what a board can ever offer becomes the fault itself. A
 * board that knows its own numbers can override it. Anything between the floor
 * and comfortable is logged and attempted — the real error beats a guess. */
#ifndef OTA_TLS_MIN_BLOCK
#define OTA_TLS_MIN_BLOCK (20 * 1024)   /* refuse below this */
#endif
#ifndef OTA_TLS_WANT_BLOCK
#define OTA_TLS_WANT_BLOCK (44 * 1024)  /* log a warning below this */
#endif

/* The one host the manifest is fetched from; probed by name (never by IP, which
 * would defeat certificate hostname verification). */
#define OTA_MANIFEST_HOST "raw.githubusercontent.com"
#endif /* OTA_DIAG */

/* ---- the check -----------------------------------------------------------
 * Blocking, and expected to be: it is user-initiated from a button, takes a
 * second or two, and the caller shows a spinner. Never called from the loop. */
static bool ota_check(OtaManifest *m) {
  memset(m, 0, sizeof(*m));
  m->http_code = 0;

  if (!wifi_connect()) { snprintf(m->err, sizeof m->err, "No WiFi"); return false; }

  WiFiClientSecure client;

#if OTA_DIAG
  /* --- stage 1: memory. Cheapest check, and a failure here would otherwise
   * surface as an indistinguishable -1 from inside start_ssl_client. */
  size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t heap_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  USBSerial.printf("[ota] internal free=%uKB largest=%uKB\n",
                   (unsigned)(heap_free / 1024), (unsigned)(heap_big / 1024));
  if (heap_big < OTA_TLS_MIN_BLOCK) {
    snprintf(m->err, sizeof m->err, "Low memory (%uKB free)",
             (unsigned)(heap_big / 1024));
    USBSerial.printf("[ota] FAIL: %s — below the %uKB floor; PSRAM cannot help, "
                     "the WiFi/lwIP path needs DMA-capable internal RAM\n",
                     m->err, (unsigned)(OTA_TLS_MIN_BLOCK / 1024));
    return false;
  }
  if (heap_big < OTA_TLS_WANT_BLOCK) {
    USBSerial.printf("[ota] WARN: only %uKB contiguous internal (want %uKB); "
                     "trying anyway\n", (unsigned)(heap_big / 1024),
                     (unsigned)(OTA_TLS_WANT_BLOCK / 1024));
  }

  /* --- stage 2: the clock (see the note above — the usual instant failure). */
  time_t nowt = time(nullptr);
  if (!OTA_TIME_SANE(nowt)) {
    USBSerial.println("[ota] clock not set -> forcing an NTP sync");
    ntp_sync_if_due(true);
    nowt = time(nullptr);
  }
  if (!OTA_TIME_SANE(nowt)) {
    snprintf(m->err, sizeof m->err, "Clock not set (NTP failed)");
    USBSerial.printf("[ota] FAIL: %s — every cert reads as not-yet-valid\n", m->err);
    return false;
  }
  USBSerial.printf("[ota] clock ok: epoch %ld\n", (long)nowt);

  /* --- stage 3: DNS, on its own, so a resolver failure says so. */
  IPAddress ip;
  if (!WiFi.hostByName(OTA_MANIFEST_HOST, ip)) {
    snprintf(m->err, sizeof m->err, "DNS failed");
    USBSerial.printf("[ota] FAIL: cannot resolve %s\n", OTA_MANIFEST_HOST);
    return false;
  }
  USBSerial.printf("[ota] %s -> %s\n", OTA_MANIFEST_HOST, ip.toString().c_str());
#endif /* OTA_DIAG */

  /* ISRG Root X1 alone: raw.githubusercontent.com is Let's Encrypt, and parsing
   * one root instead of two is memory this board would rather keep. The
   * two-root OTA_ROOT_CA is for the download, which starts on github.com. */
  client.setCACert(OTA_ROOT_CA_LE);
  client.setHandshakeTimeout(20);        /* seconds; the default 120 s is worse
                                          * than useless on a UI-thread call */

#if OTA_DIAG
  /* --- stage 4: TCP + TLS, before HTTPClient is involved at all. Connecting by
   * NAME (not the IP resolved above) keeps hostname verification intact.
   * HTTPClient reuses a client that is already connected, so this probe costs
   * nothing: the handshake it performs is the one the GET would have done. */
  if (!client.connect(OTA_MANIFEST_HOST, 443)) {
    char tls_err[128] = {0};
    client.lastError(tls_err, sizeof tls_err);
    snprintf(m->err, sizeof m->err, "TLS handshake failed");
    USBSerial.printf("[ota] FAIL: TLS to %s: %s\n", OTA_MANIFEST_HOST,
                     tls_err[0] ? tls_err : "(no mbedtls detail)");
    return false;
  }
  USBSerial.println("[ota] TLS up, fetching manifest");
#endif

  HTTPClient http;
  if (!http.begin(client, OTA_MANIFEST_URL)) {
    snprintf(m->err, sizeof m->err, "begin() failed");
    return false;
  }
  http.setConnectTimeout(8000);          /* TLS handshake headroom */
  http.setTimeout(8000);
  http.setUserAgent("OpenWatchFace");
  /* raw.githubusercontent.com serves a 302 to a CDN host; follow it, and note
   * that the CDN is also Let's Encrypt so the pin still holds. */
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  m->http_code = code;
#if OTA_DIAG
  USBSerial.printf("[ota] GET %s -> %d\n", OTA_MANIFEST_URL, code);
#endif
  if (code != 200) {
    /* A negative code is an HTTPClient/TLS error, not an HTTP status — the
     * distinction matters when diagnosing, so say which. A 404 here is not a
     * fault in this code: it means ota/latest.json has not been pushed to the
     * branch OTA_MANIFEST_URL names, or the URL names the wrong account. */
    if (code < 0)         snprintf(m->err, sizeof m->err, "TLS/conn err %d", code);
    else if (code == 404) snprintf(m->err, sizeof m->err, "No manifest (404)");
    else                  snprintf(m->err, sizeof m->err, "HTTP %d", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  if (ota_json_str(body, 0, "version", m->version, sizeof m->version) < 0) {
    snprintf(m->err, sizeof m->err, "No version field");
    return false;
  }
  ota_json_str(body, 0, "notes", m->notes, sizeof m->notes);

  /* Find this board's object, then read only from there — otherwise the first
   * "url" in the document wins regardless of which board it belongs to. */
  int b = body.indexOf(String("\"") + OTA_BOARD_KEY + "\"");
  if (b < 0) {
    snprintf(m->err, sizeof m->err, "No build for " OTA_BOARD_KEY);
    return false;
  }
  ota_json_str(body, b, "url",    m->url,    sizeof m->url);
  ota_json_str(body, b, "sha256", m->sha256, sizeof m->sha256);
  ota_json_u32(body, b, "size",   &m->size);

  if (!m->url[0] || strlen(m->sha256) != 64 || m->size == 0) {
    snprintf(m->err, sizeof m->err, "Build entry incomplete");
    return false;
  }
  /* Refuse a URL we could not validate the certificate for. Cheap, and it
   * turns a confusing field failure into a clear one at check time. */
  if (strncmp(m->url, "https://", 8) != 0) {
    snprintf(m->err, sizeof m->err, "URL not https");
    return false;
  }

  m->newer = ota_ver_cmp(DEVICE_VERSION, m->version) < 0;
  m->ok = true;
  return true;
}
