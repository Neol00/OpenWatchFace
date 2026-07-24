/* ============================================================================
 *  tuya/compat/owf_tuya_sntp.h - a minimal SNTP (NTP) client for the T5.
 *
 *  The Tuya core has NO SNTP service (lwIP's sntp is not compiled into the
 *  prebuilt libs, and the standalone build has no Tuya-cloud time sync). So the
 *  firmware's configTzTime() was a stub that only set TZ and never fetched network
 *  time - which is why the clock never updated from WiFi.
 *
 *  This implements SNTP ourselves over plain UDP (WiFiUDP is real on this core):
 *  send a 48-byte NTP request to an NTP server on port 123, read the reply, take
 *  the "transmit timestamp" (seconds since 1900), convert to Unix epoch, and write
 *  it to the system clock via settimeofday() (owf_tuya_time.cpp -> the AON RTC, the
 *  same clock time()/gettimeofday() read). board_clock_set() (the PCF85063 chip)
 *  is then re-stamped by the caller through the normal board API.
 *
 *  UTC ONLY: NTP returns UTC. We set the system clock to UTC and rely on the POSIX
 *  TZ env (set by configTzTime) so localtime()/getLocalTime() apply the local offset
 *  + DST - exactly how the ESP path works. So board_clock_now()/the watch face read
 *  local time, while the kernel clock holds UTC.
 *
 *  Included only on BOARD_PLATFORM_TUYA. Call owf_tuya_sntp_sync() while WiFi is up.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_TUYA

#include <WiFi.h>
#include <WiFiUdp.h>
#include <sys/time.h>
#include <time.h>

/* NTP timestamps count seconds since 1900-01-01; Unix epoch is 1970-01-01.
 * The difference is 70 years including 17 leap days = 2208988800 seconds. */
#define OWF_NTP_UNIX_DELTA  2208988800UL
#define OWF_NTP_PORT        123
#define OWF_NTP_PKT_LEN     48

/* Query one NTP server (by hostname) and, on success, set the system clock to the
 * returned UTC time. Returns true if the clock was set. Blocks up to ~timeout_ms.
 * settimeofday() here routes to bk_rtc_settimeofday (owf_tuya_time.cpp). */
static inline bool owf_tuya_sntp_query(const char *server, uint32_t timeout_ms,
                                       uint32_t *out_utc_epoch) {
  if (!server || WiFi.status() != WL_CONNECTED) { USBSerial.println("[ntp] not connected"); return false; }

  WiFiUDP udp;
  // Bind to our LOCAL IP + a fixed port (matching the Tuya WiFiUDPClient example, which is
  // the known-good usage on this stack). Binding to INADDR_ANY via begin(port) sent fine but
  // no reply was delivered back - binding to the actual local IP fixes the inbound route.
  IPAddress local = WiFi.localIP();
  if (!udp.begin(local, (uint16_t)8888)) { USBSerial.println("[ntp] udp.begin failed"); return false; }

  uint8_t pkt[OWF_NTP_PKT_LEN];
  memset(pkt, 0, sizeof(pkt));
  // LI=0 (no warning), VN=4 (NTP v4), Mode=3 (client) -> 0b00'100'011 = 0x23.
  pkt[0] = 0x23;

  // CRITICAL: do NOT use udp.beginPacket(host, port). The Tuya WiFiUDP's hostname overload
  // has a bug - it casts the resolved IP integer to (const uint8_t*) and constructs IPAddress
  // from that as a POINTER, dereferencing a garbage address -> PANIC (this was the crash).
  // So we resolve DNS ourselves with WiFi.hostByName() (correct lwIP path) and use the
  // IPAddress overload of beginPacket(), which never does that bad cast.
  IPAddress ntp_ip;
  if (!WiFi.hostByName(server, ntp_ip)) {
    USBSerial.printf("[ntp] DNS failed for %s\n", server); udp.stop(); return false;
  }
  USBSerial.printf("[ntp] %s -> %d.%d.%d.%d\n", server, ntp_ip[0], ntp_ip[1], ntp_ip[2], ntp_ip[3]);

  // BYTE-ORDER FIX: Tuya's WiFiUDP::endPacket() sends to (uint32_t)remote_ip WITHOUT any
  // host->network swap, but the tal_net stack wants the address byte-SWAPPED (begin() swaps
  // via UNI_HTONL before binding; endPacket does not). So beginPacket(IPAddress) sends to the
  // wrong-endian address -> packet misdelivered -> NO REPLY (exactly what we saw). We pre-swap
  // the resolved IP's 32-bit value and wrap it back in an IPAddress, so endPacket's raw cast
  // emits the correct network-order address.
  uint32_t ip_host = (uint32_t)ntp_ip;             // raw stored value (host order)
  uint32_t ip_swapped = ((ip_host & 0x000000FFu) << 24) | ((ip_host & 0x0000FF00u) << 8) |
                        ((ip_host & 0x00FF0000u) >> 8)  | ((ip_host & 0xFF000000u) >> 24);
  IPAddress ntp_ip_net(ip_swapped);
  if (!udp.beginPacket(ntp_ip_net, OWF_NTP_PORT)) { USBSerial.println("[ntp] beginPacket failed"); udp.stop(); return false; }
  if (udp.write(pkt, sizeof(pkt)) != sizeof(pkt)) { USBSerial.println("[ntp] write failed"); udp.stop(); return false; }
  if (!udp.endPacket()) { USBSerial.println("[ntp] endPacket(send) failed"); udp.stop(); return false; }

  // Wait for the reply (non-blocking socket; parsePacket returns 0 until data arrives).
  uint32_t start = millis();
  int len = 0;
  while (millis() - start < timeout_ms) {
    len = udp.parsePacket();
    if (len >= OWF_NTP_PKT_LEN) break;
    len = 0;
    delay(20);
  }
  if (len < OWF_NTP_PKT_LEN) { USBSerial.printf("[ntp] no reply (len=%d)\n", len); udp.stop(); return false; }

  int got = udp.read(pkt, (size_t)OWF_NTP_PKT_LEN);
  udp.stop();
  if (got < OWF_NTP_PKT_LEN) { USBSerial.printf("[ntp] short read (got=%d)\n", got); return false; }

  // Transmit timestamp = bytes 40..43 (seconds since 1900, big-endian).
  uint32_t secs1900 = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                      ((uint32_t)pkt[42] << 8)  |  (uint32_t)pkt[43];
  USBSerial.printf("[ntp] reply secs1900=%lu\n", (unsigned long)secs1900);
  if (secs1900 == 0) { USBSerial.println("[ntp] zero timestamp"); return false; }

  uint32_t unix_epoch = secs1900 - OWF_NTP_UNIX_DELTA;
  // Sanity: must be after 2024-01-01 (1704067200). Rejects garbage / KoD packets.
  if (unix_epoch < 1704067200UL) { USBSerial.printf("[ntp] epoch too small=%lu\n", (unsigned long)unix_epoch); return false; }

  struct timeval tv;
  tv.tv_sec  = (time_t)unix_epoch;                 // UTC
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) { USBSerial.println("[ntp] settimeofday failed"); return false; }
  USBSerial.printf("[ntp] clock set to UTC epoch %lu\n", (unsigned long)unix_epoch);
  // Hand the KNOWN-GOOD epoch back to the caller. Do NOT make the caller read it back via
  // gettimeofday()/getLocalTime() - that readback on this board returns a corrupted value
  // (observed year 3855160), so the caller must convert THIS epoch directly with localtime_r.
  if (out_utc_epoch) *out_utc_epoch = (uint32_t)unix_epoch;
  return true;
}

/* Try a short list of NTP servers; return true once one succeeds. On success, *out_utc_epoch
 * holds the fetched UTC Unix epoch (the caller converts it to local time with localtime_r —
 * it must NOT read the system clock back, which is corrupted on this board). */
static inline bool owf_tuya_sntp_sync(uint32_t *out_utc_epoch) {
  static const char *servers[] = { "pool.ntp.org", "time.nist.gov", "time.google.com" };
  for (unsigned i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
    if (owf_tuya_sntp_query(servers[i], 3000, out_utc_epoch)) return true;
  }
  return false;
}

#endif /* BOARD_PLATFORM_TUYA */
