#pragma once
/* ============================================================================
 *  ota_install.h — download the image ota_check.h found and write it to the
 *  idle OTA slot. The second half of the updater; the check is the first.
 *
 *  HOW IT STAYS SAFE. The image goes into whichever app slot is NOT running
 *  (esp_ota_get_next_update_partition, via the Arduino Update class). The
 *  slot is only marked bootable at the very end, after
 *     1. every byte the manifest promised has arrived (size match),
 *     2. the SHA-256 over those bytes equals the manifest's, and
 *     3. Update.end() has validated the ESP image header in the slot.
 *  A download that is truncated, corrupted, or swapped for another board's
 *  image fails one of those and the running firmware is untouched — the
 *  partition table comment explains why the flash is spent on two slots.
 *  The manifest itself arrives over TLS pinned to a known root (ota_ca.h), so
 *  the sha256 in it is trusted as far as that pin is.
 *
 *  WHY BOTH ROOTS. The URL starts on github.com (Sectigo/USERTrust) and
 *  302-redirects to a Let's Encrypt CDN host. OTA_ROOT_CA carries both, and
 *  HTTPClient follows the redirect on the same WiFiClientSecure, so the pin
 *  holds on both hops. Do not "optimise" this to a single root.
 *
 *  BLOCKING, ON THE UI TASK, LIKE THE CHECK. A 3 MB image at the rates these
 *  radios manage takes 20-60 s. The caller passes a progress callback that
 *  repaints the label; nothing else on the screen moves. That is acceptable
 *  for a user-initiated install and avoids a second task fighting for the
 *  same 45 KB of internal SRAM the TLS session needs.
 *
 *  Two implementations: ESP32 (this file's second half, Update.h into the
 *  idle ota_x slot) and the Wear 2100 watches (first half, the whole image
 *  verified in DDR then written to `boot` by platform/bootimg_write.c). The
 *  Tuya and Maix ports have neither; OTA_INSTALL_SUPPORTED is 0 there.
 * ========================================================================== */
#include "ota_check.h"

#define OTA_INSTALL_SUPPORTED (!BOARD_PLATFORM_TUYA && !BOARD_PLATFORM_MAIX)

typedef void (*ota_progress_fn)(uint32_t done, uint32_t total, void *arg);

/* THE BOARD MARKER. Every image carries "OWF-BOARD:<key>" in its rodata, and
 * the installer refuses an image that does not contain the marker of the
 * board it is running on. The asset filename already selects by key, but the
 * filename is something a person typed at release time; the marker is what
 * the compiler put in. On the Wear 2100 watches this is the last line of
 * defence before overwriting `boot` with an image built for another panel
 * (a C2 image on an S2 draws wrong; a Gen 4 image on a C2 does not boot). */
static const char owf_board_marker[] = "OWF-BOARD:" OTA_BOARD_KEY;

static bool ota_has_marker(const uint8_t *img, size_t len) {
  size_t n = sizeof owf_board_marker;          /* including the NUL */
  if (len < n) return false;
  for (size_t i = 0; i + n <= len; i++)
    if (img[i] == 'O' && memcmp(img + i, owf_board_marker, n) == 0) return true;
  return false;
}

static bool ota_hex_eq(const char *hex, const uint8_t *bin, size_t n) {
  static const char *d = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    char a = hex[2 * i], b = hex[2 * i + 1];
    if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
    if (a != d[bin[i] >> 4] || b != d[bin[i] & 15]) return false;
  }
  return true;
}

#if OTA_INSTALL_SUPPORTED && BOARD_PLATFORM_FOSSIL
/* ---- Wear 2100 watches: whole image into DDR, verify, write `boot` --------
 * No second slot on aboot, so the image is fully downloaded and checked in
 * RAM (2.6 MB on a 512 MB watch) before a single block is written; the write
 * itself is platform/bootimg_write.c, which is the only path with permission
 * to touch `boot`. Progress: first half download, second half write+verify. */
/* mbedTLS is only linked on the boards with the radio stack (Wear 2100); the
 * Gen 6 build has neither TLS nor a way to download, so its hash check is
 * compiled out and the check fails earlier at the TLS connect anyway. */
#if defined(__has_include)
#  if __has_include("mbedtls/sha256.h")
#    include "mbedtls/sha256.h"
#    define OTA_HAVE_SHA256 1
#  endif
#endif
#ifndef OTA_HAVE_SHA256
#  define OTA_HAVE_SHA256 0
#endif
extern "C" int bootimg_write(const void *img, uint32_t len, void (*progress)(uint32_t, uint32_t));
extern "C" void con_puts(const char *s);

static ota_progress_fn s_ota_prog; static void *s_ota_parg; static uint32_t s_ota_total;
static uint32_t s_ota_base;                 /* bytes already banked by earlier (resumed) requests */
static void ota_dl_prog(size_t got, size_t total, void *arg) {
  (void)arg; (void)total; if (!s_ota_prog) return;
  uint32_t t = s_ota_total ? s_ota_total : (uint32_t)total;
  s_ota_prog((uint32_t)(s_ota_base + got) / 2, t, s_ota_parg);
}
static void ota_wr_prog(uint32_t done, uint32_t total) {
  if (!s_ota_prog || !total) return;
  s_ota_prog(s_ota_total / 2 + (uint32_t)((uint64_t)done * (s_ota_total / 2) / total), s_ota_total, s_ota_parg);
}

/* Download m->size bytes into a buffer, RESUMING with HTTP Range requests
 * whenever a transfer stalls (the first real install stopped at 163 KB of
 * 2.7 MB: a stalled read hit the client timeout and the whole file was lost).
 * Up to OTA_DL_ATTEMPTS connections; each one continues from the byte the
 * previous one reached. Returns the buffer (caller frees) or nullptr + err. */
#ifndef OTA_DL_ATTEMPTS
#define OTA_DL_ATTEMPTS 10
#endif
static uint8_t *ota_download(const OtaManifest *m, WiFiClientSecure &client, char *err, size_t errcap) {
  uint8_t *img = (uint8_t *)malloc(m->size);
  if (!img) { snprintf(err, errcap, "No memory for %u KB", (unsigned)(m->size / 1024)); return nullptr; }
  uint32_t got = 0;
  for (int attempt = 1; attempt <= OTA_DL_ATTEMPTS && got < m->size; attempt++) {
    s_ota_base = got;
    HTTPClient http;
    if (!http.begin(client, m->url)) { snprintf(err, errcap, "begin() failed"); free(img); return nullptr; }
    http.setConnectTimeout(10000);
    http.setTimeout(20000);
    http.setUserAgent("OpenWatchFace");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setBodyLimit(m->size - got + 4096);
    http.onBody(ota_dl_prog, nullptr);
    if (got) { char r[48]; snprintf(r, sizeof r, "bytes=%u-", (unsigned)got); http.addHeader("Range", r); }
    int code = http.GET();
    size_t len = 0; uint8_t *part = nullptr;
    if (code == 206 || (code == 200 && got == 0)) {
      part = http.takeBody(&len);
    } else if (code == 200 && got > 0) {
      /* server ignored the Range: it sent the whole file again from 0 */
      part = http.takeBody(&len); got = 0; s_ota_base = 0;
    } else {
      if (code < 0) { char t[96]; if (client.lastError(t, sizeof t)) snprintf(err, errcap, "%.60s", t); else snprintf(err, errcap, "Download err %d", code); }
      else snprintf(err, errcap, "Download HTTP %d", code);
      http.end();
      if (code < 0) { con_puts("[ota] connection failed, retrying\n"); continue; }   /* transport: retry */
      free(img); return nullptr;                                                     /* HTTP status: final */
    }
    http.end();
    if (part && len) {
      if (got + len > m->size) len = m->size - got;
      memcpy(img + got, part, len); got += (uint32_t)len;
    }
    free(part);
    if (got < m->size) {
      char l[96]; snprintf(l, sizeof l, "[ota] download stalled at %u/%u bytes, resuming (attempt %d/%d)\n",
                           (unsigned)got, (unsigned)m->size, attempt + 1, OTA_DL_ATTEMPTS);
      con_puts(l);
      delay(500);
    }
  }
  if (got != m->size) { snprintf(err, errcap, "Download incomplete (%u of %u KB)", (unsigned)(got / 1024), (unsigned)(m->size / 1024)); free(img); return nullptr; }
  return img;
}

static bool ota_install(const OtaManifest *m, char *err, size_t errcap,
                        ota_progress_fn prog, void *arg) {
  err[0] = '\0';
  if (!m->ok || !m->url[0] || m->size == 0) { snprintf(err, errcap, "Nothing to install"); return false; }
  if (!wifi_connect()) { snprintf(err, errcap, "No WiFi"); return false; }
  s_ota_prog = prog; s_ota_parg = arg; s_ota_total = m->size; s_ota_base = 0;

  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
  client.setHandshakeTimeout(20);
  uint8_t *img = ota_download(m, client, err, errcap);
  if (!img) return false;
  size_t len = m->size;
#if OTA_HAVE_SHA256
  if (m->sha256[0]) {
    uint8_t d[32]; mbedtls_sha256((const unsigned char *)img, len, d, 0);
    if (!ota_hex_eq(m->sha256, d, 32)) { snprintf(err, errcap, "SHA-256 mismatch"); free(img); return false; }
  }
#else
  if (m->sha256[0]) { snprintf(err, errcap, "No SHA-256 support in this build"); free(img); return false; }
#endif
  if (len < 2048 || memcmp(img, "ANDROID!", 8) != 0) { snprintf(err, errcap, "Not a boot image"); free(img); return false; }
  if (!ota_has_marker(img, len)) {
    snprintf(err, errcap, "Image is not for " OTA_BOARD_KEY); free(img); return false;
  }
  con_puts("[ota] image verified, writing boot partition\n");
  int rc = bootimg_write(img, (uint32_t)len, ota_wr_prog);
  free(img);
  if (rc < 0) {
    snprintf(err, errcap, rc == -5 || rc == -6 ? "WRITE FAILED (%d), do not power off, retry"
                                                : "Boot write refused (%d)", rc);
    return false;
  }
  return true;
}

#elif OTA_INSTALL_SUPPORTED
#include <Update.h>
#include "mbedtls/sha256.h"
#include "esp_ota_ops.h"

/* Internal SRAM the install needs on top of what the check already proved:
 * the TLS session (same as the check), Update's 4 KB sector buffer and our
 * 4 KB read buffer. The floor is deliberately close to the check's so that a
 * board that can check can install; a fail here is named, not a bare error. */
#ifndef OTA_INSTALL_MIN_BLOCK
#define OTA_INSTALL_MIN_BLOCK (24 * 1024)
#endif

/* Seconds without a single byte before the download is declared dead. GitHub's
 * CDN streams steadily; a gap this long means the link is gone. */
#ifndef OTA_STALL_S
#define OTA_STALL_S 20
#endif

/* Download m->url and write it to the idle slot. On success the slot is
 * marked bootable and the caller should reboot. On failure `err` names the
 * stage and the running firmware is untouched. */
static bool ota_install(const OtaManifest *m, char *err, size_t errcap,
                        ota_progress_fn prog, void *arg) {
  err[0] = '\0';
  if (!m->ok || !m->url[0] || m->size == 0) {
    snprintf(err, errcap, "Nothing to install"); return false;
  }
  if (!wifi_connect()) { snprintf(err, errcap, "No WiFi"); return false; }

  size_t heap_big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  USBSerial.printf("[ota] install %s (%u bytes, %s), internal largest=%uKB\n",
                   m->url, (unsigned)m->size, owf_board_marker, (unsigned)(heap_big / 1024));
  if (heap_big < OTA_INSTALL_MIN_BLOCK) {
    snprintf(err, errcap, "Low memory (%uKB free)", (unsigned)(heap_big / 1024));
    return false;
  }

  /* The slot must exist and fit before a single byte is fetched. */
  const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
  if (!slot) {
    snprintf(err, errcap, "No OTA slot (partition table)");
    USBSerial.println("[ota] FAIL: no ota_x partition to write — the board is "
                      "built with a single-app table");
    return false;
  }
  if (m->size > slot->size) {
    snprintf(err, errcap, "Image %u KB > slot %u KB",
             (unsigned)(m->size / 1024), (unsigned)(slot->size / 1024));
    return false;
  }
  USBSerial.printf("[ota] target slot %s @0x%x (%u KB)\n", slot->label,
                   (unsigned)slot->address, (unsigned)(slot->size / 1024));

  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);           /* both roots: github.com + CDN */
  client.setHandshakeTimeout(20);

  HTTPClient http;
  if (!http.begin(client, m->url)) { snprintf(err, errcap, "begin() failed"); return false; }
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.setUserAgent("OpenWatchFace");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);

  int code = http.GET();
  USBSerial.printf("[ota] GET image -> %d\n", code);
  if (code != 200) {
    if (code < 0) snprintf(err, errcap, "Download err %d", code);
    else          snprintf(err, errcap, "Download HTTP %d", code);
    http.end();
    return false;
  }
  int len = http.getSize();
  if (len > 0 && (uint32_t)len != m->size) {
    snprintf(err, errcap, "Size mismatch (%d vs %u)", len, (unsigned)m->size);
    http.end();
    return false;
  }

  if (!Update.begin(m->size, U_FLASH)) {
    snprintf(err, errcap, "Update.begin: %s", Update.errorString());
    http.end();
    return false;
  }

  uint8_t *buf = (uint8_t *)malloc(4096);
  if (!buf) { snprintf(err, errcap, "No buffer"); Update.abort(); http.end(); return false; }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  WiFiClient *stream = http.getStreamPtr();
  uint32_t got = 0, last_report = 0;
  unsigned long last_data = millis();
  bool ok = true;

  while (got < m->size) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (!stream->connected() && got < m->size) {
        snprintf(err, errcap, "Connection lost at %u KB", (unsigned)(got / 1024));
        ok = false; break;
      }
      if (millis() - last_data > OTA_STALL_S * 1000UL) {
        snprintf(err, errcap, "Stalled at %u KB", (unsigned)(got / 1024));
        ok = false; break;
      }
      delay(1);
      continue;
    }
    if (avail > 4096) avail = 4096;
    if (avail > m->size - got) avail = m->size - got;   /* never past the promise */
    int n = stream->readBytes(buf, avail);
    if (n <= 0) { delay(1); continue; }
    last_data = millis();
    if (Update.write(buf, (size_t)n) != (size_t)n) {
      snprintf(err, errcap, "Flash write: %s", Update.errorString());
      ok = false; break;
    }
    mbedtls_sha256_update(&sha, buf, (size_t)n);
    got += (uint32_t)n;
    if (prog && (got - last_report >= 32 * 1024 || got == m->size)) {
      last_report = got;
      prog(got, m->size, arg);
    }
    delay(0);                               /* let WiFi/lwIP breathe */
  }
  http.end();
  free(buf);

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (ok && m->sha256[0] && !ota_hex_eq(m->sha256, digest, 32)) {
    snprintf(err, errcap, "SHA-256 mismatch");
    USBSerial.println("[ota] FAIL: downloaded image hash != manifest — refusing");
    ok = false;
  }
  if (!ok) { Update.abort(); return false; }

  /* end() validates the image header in the slot and flips otadata to it. */
  if (!Update.end()) {
    snprintf(err, errcap, "Update.end: %s", Update.errorString());
    return false;
  }
  USBSerial.printf("[ota] installed %u bytes to %s, sha ok — reboot to run it\n",
                   (unsigned)got, slot->label);
  return true;
}

#else  /* !OTA_INSTALL_SUPPORTED */

static bool ota_install(const OtaManifest *m, char *err, size_t errcap,
                        ota_progress_fn prog, void *arg) {
  (void)m; (void)prog; (void)arg;
  snprintf(err, errcap, "Install not supported on this board");
  return false;
}

#endif
