#pragma once
/* ============================================================================
 *  ota_check.h — "is there a newer OpenWatchFace?", and nothing else.
 *
 *  DELIBERATELY THE CHECK ONLY. No download, no flashing, no partition work;
 *  that is ota_install.h, which consumes the OtaManifest this produces. The
 *  split is kept on purpose: a manifest fetch that fails is a network or
 *  certificate problem, an install that fails is a flash or image problem,
 *  and each reports its own stage.
 *
 *  WHERE THE UPDATE COMES FROM: the GitHub release itself, nothing else. The
 *  check asks api.github.com for the repo's latest release, reads the tag
 *  (its version), and looks for an asset named owf-<board key>-<version><ext>
 *  in that release. Publishing a release with the right asset name is the
 *  whole publishing step — no file in the repo has to be edited or pushed.
 *  (An earlier design read a committed ota/latest.json; that file was never
 *  pushed and every watch reported "No manifest (404)". Gone.)
 *
 *  CERTIFICATES. api.github.com and github.com are served from a Sectigo
 *  chain (USERTrust ECC root); the CDN the download redirects to
 *  (*.githubusercontent.com) is Let's Encrypt (ISRG Root X1). OTA_ROOT_CA in
 *  ota_ca.h carries both, and every hop is verified against it. The CDN URLs
 *  are signed and expire, so only the github.com download URL is stored.
 *
 *  INTEGRITY. Size comes from the API. If the release also carries an asset
 *  named SHA256SUMS (the output of `sha256sum owf-*` uploaded alongside), the
 *  line for this board's asset is used and the installer refuses a hash
 *  mismatch. Without it the installer still checks the size, the image
 *  magic and the board marker inside the image — TLS to the pinned root
 *  covers the transport.
 *
 *  Unauthenticated API calls are limited to 60/hour per IP; the check is a
 *  button, so that is plenty. A 403 is reported as the rate limit.
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

#ifndef OTA_REPO
#define OTA_REPO "Neol00/OpenWatchFace"
#endif
#define OTA_API_HOST "api.github.com"
#define OTA_LATEST_URL "https://" OTA_API_HOST "/repos/" OTA_REPO "/releases/latest"

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

/* Asset extension: an ESP app image is a .bin; the Wear 2100 watches boot an
 * Android boot image, published as .img. */
#ifndef OTA_ASSET_EXT
#  if BOARD_PLATFORM_FOSSIL
#    define OTA_ASSET_EXT ".img"
#  else
#    define OTA_ASSET_EXT ".bin"
#  endif
#endif

/* Convenience for the release tooling and the About screen: the exact asset
 * filename this build expects to be published as. */
#define OTA_ASSET_NAME "owf-" OTA_BOARD_KEY "-" DEVICE_VERSION OTA_ASSET_EXT

/* What the check found. Deliberately plain data: the UI renders it, the
 * installer (later) consumes it, and neither needs the HTTP layer again. */
struct OtaManifest {
  bool     ok;              /* the fetch AND the parse both succeeded      */
  int      http_code;       /* as returned, or a negative HTTPClient error */
  char     err[48];         /* short reason when !ok — shown verbatim      */
  char     version[24];     /* "1.5.0" (the tag without its v/V)           */
  char     notes[80];       /* the release title                           */
  char     url[192];        /* firmware image for THIS board               */
  char     asset[64];       /* its filename, owf-<key>-<ver><ext>          */
  char     sha256[65];      /* 64 hex + NUL, or "" when no SHA256SUMS      */
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
#define OTA_MANIFEST_HOST OTA_API_HOST
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

  /* Both roots: api.github.com is Sectigo (USERTrust ECC), the download CDN
   * is Let's Encrypt. */
  client.setCACert(OTA_ROOT_CA);
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
  USBSerial.println("[ota] TLS up, asking for the latest release");
#endif

  HTTPClient http;
  if (!http.begin(client, OTA_LATEST_URL)) {
    snprintf(m->err, sizeof m->err, "begin() failed");
    return false;
  }
  http.setConnectTimeout(8000);          /* TLS handshake headroom */
  http.setTimeout(8000);
  http.setUserAgent("OpenWatchFace");    /* the API refuses requests without one */
  http.addHeader("Accept", "application/vnd.github+json");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  m->http_code = code;
#if OTA_DIAG
  USBSerial.printf("[ota] GET %s -> %d\n", OTA_LATEST_URL, code);
#endif
  if (code != 200) {
    if (code < 0) {
      snprintf(m->err, sizeof m->err, "TLS/conn err %d", code);
#if BOARD_PLATFORM_FOSSIL
      { char t[96]; if (client.lastError(t, sizeof t)) snprintf(m->err, sizeof m->err, "%.44s", t); }
#endif
    }
    else if (code == 404) snprintf(m->err, sizeof m->err, "No release published yet");
    else if (code == 403) snprintf(m->err, sizeof m->err, "GitHub rate limit, retry later");
    else                  snprintf(m->err, sizeof m->err, "HTTP %d", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  /* tag "V1.4.1" / "v1.4.1" / "1.4.1" -> version "1.4.1" */
  char tag[24];
  if (ota_json_str(body, 0, "tag_name", tag, sizeof tag) < 0) {
    snprintf(m->err, sizeof m->err, "No tag_name in release");
    return false;
  }
  {
    const char *t = tag;
    if (*t == 'v' || *t == 'V') t++;
    snprintf(m->version, sizeof m->version, "%s", t);
  }
  {
    int at = body.indexOf("\"tag_name\"");
    ota_json_str(body, at < 0 ? 0 : at, "name", m->notes, sizeof m->notes);   /* release title */
  }

  /* The asset for THIS board, by exact filename, then its size and download
   * URL — both follow the name inside the same asset object. */
  snprintf(m->asset, sizeof m->asset, "owf-" OTA_BOARD_KEY "-%s" OTA_ASSET_EXT, m->version);
  int a = body.indexOf(String("\"") + m->asset + "\"");
  if (a < 0) {
    snprintf(m->err, sizeof m->err, "No %s in %s", OTA_BOARD_KEY, tag);
    return false;
  }
  /* The asset object nests an "uploader" user object between "name" and
   * "size"; anchor on "content_type", which only the asset itself has, so
   * a "size" key inside the nested object can never be picked up. */
  {
    int ct = body.indexOf("\"content_type\"", a);
    ota_json_u32(body, ct < 0 ? a : ct, "size", &m->size);
  }
  ota_json_str(body, a, "browser_download_url", m->url, sizeof m->url);
  if (!m->url[0] || m->size == 0) {
    snprintf(m->err, sizeof m->err, "Asset entry incomplete");
    return false;
  }
  if (strncmp(m->url, "https://", 8) != 0) {
    snprintf(m->err, sizeof m->err, "URL not https");
    return false;
  }

  /* Optional SHA256SUMS asset: fetch it and pick this asset's line. */
  m->sha256[0] = '\0';
  {
    int sidx = body.indexOf("\"SHA256SUMS\"");
    char surl[192] = {0};
    if (sidx >= 0) ota_json_str(body, sidx, "browser_download_url", surl, sizeof surl);
    body = String();                      /* free the release JSON first */
    if (surl[0]) {
      HTTPClient h2;
      if (h2.begin(client, surl)) {
        h2.setConnectTimeout(8000); h2.setTimeout(8000);
        h2.setUserAgent("OpenWatchFace");
        h2.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        int c2 = h2.GET();
        if (c2 == 200) {
          String sums = h2.getString();
          int k = sums.indexOf(m->asset);
          /* "<64 hex>  <name>" — the hash is the 64 chars before the two spaces */
          if (k >= 66) {
            int hs = k - 1;
            while (hs > 0 && (sums[hs] == ' ' || sums[hs] == '*')) hs--;
            if (hs >= 63) { sums.substring(hs - 63, hs + 1).toCharArray(m->sha256, sizeof m->sha256); }
          }
        }
        h2.end();
      }
#if OTA_DIAG
      USBSerial.printf("[ota] SHA256SUMS: %s\n", m->sha256[0] ? m->sha256 : "(no line for this asset)");
#endif
    }
  }

  m->newer = ota_ver_cmp(DEVICE_VERSION, m->version) < 0;
  m->ok = true;
  return true;
}
