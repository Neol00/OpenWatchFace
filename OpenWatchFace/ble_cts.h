/* ============================================================================
 *  ble_cts.h — Current Time Service (CTS) CLIENT: pull the clock off the iPhone.
 *
 *  The iPhone is the ONLY time source on a watch with no WiFi (the Fossil Gen 6 —
 *  its WCNSS WiFi is unreachable without rooting Wear OS), and ANCS carries no
 *  clock: a notification's Date attribute is only sent when a notification exists,
 *  and only for that notification. But iOS ALSO exposes the standard Bluetooth
 *  Current Time Service (0x1805) on the same bonded link ANCS/AMS use, so the same
 *  GATT client can simply read the phone's wall clock and write it to the RTC.
 *  Android/Gadgetbridge phones keep using setTime() (ble_gadgetbridge.h); this file
 *  is the iPhone half of the same job.
 *
 *  HOW IT WORKS (we are the GATT CLIENT, like ANCS/AMS):
 *    1. On an encrypted+bonded link, discover 0x1805 and its Current Time
 *       characteristic 0x2A2B (read + notify).
 *    2. READ it once -> apply to the RTC.
 *    3. Subscribe to its CCCD so the phone PUSHES a new value when the clock
 *       changes (timezone change, DST, manual set, NTP correction on the phone).
 *    4. Re-read every CTS_RESYNC_MS anyway, to trim crystal drift on a link that
 *       stays up for days.
 *
 *  SERIALIZATION: NimBLE runs few GATT procedures per connection, and ANCS + AMS
 *  already chain their discovery back-to-back off the encryption event (a
 *  concurrent third chain is what makes procedures fail with ENOMEM). So CTS does
 *  NOT start from the encryption callback: ancs_on_encrypted() only ARMS it, and
 *  cts_poll() — called from the main loop — kicks the discovery a couple of seconds
 *  later, once the ANCS/AMS bursts are done, retrying a few times if the stack is
 *  still busy.
 *
 *  TIME BASE: CTS carries LOCAL wall-clock time (iOS reports the phone's local
 *  time, including its timezone/DST), and the RTC holds local time too — so the
 *  fields go straight to board_clock_set() with no offset, unlike the Gadgetbridge
 *  path which converts a UTC epoch. rtc_last_ntp_epoch is refreshed so a WiFi board
 *  doesn't immediately re-sync from NTP over the top.
 *
 *  THREADING: discovery/read/notify callbacks run on the NimBLE host task; cts_poll
 *  runs on the loop. Neither touches LVGL. The RTC write is wrapped in
 *  i2c_lock()/i2c_unlock(), exactly like gb_handle_settime().
 *
 *  INCLUDE AFTER ble_ancs.h / ble_player_ams.h (it is armed + reset from the ANCS
 *  hooks and routed from the shared GAP notify handler). Header-only, in the .ino TU.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>

/* Standard SIG-assigned 16-bit UUIDs. */
static const ble_uuid16_t CTS_SVC_UUID      = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t CTS_CUR_TIME_UUID = BLE_UUID16_INIT(0x2A2B);

#define CTS_FIRST_TRY_MS   2500u        // let the ANCS + AMS discovery chains finish first
#define CTS_RETRY_MS       4000u        // ...then retry this often if the stack was busy
#define CTS_MAX_TRIES      5
#define CTS_RESYNC_MS      (6u * 60u * 60u * 1000u)   // drift trim on a long-lived link

static uint16_t s_cts_conn     = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_cts_svc_start = 0, s_cts_svc_end = 0;
static uint16_t s_cts_val      = 0;     // Current Time value handle
static uint16_t s_cts_cccd     = 0;
static bool     s_cts_subbed   = false;
static bool     s_cts_synced   = false; // we have applied at least one CTS reading
static uint8_t  s_cts_tries    = 0;
static uint32_t s_cts_next_ms  = 0;     // millis() of the next discovery attempt / re-read
static bool     s_cts_discovering = false;

/* One-shot flag for the loop: the clock jumped, so any open clock UI should redraw. */
static volatile bool s_cts_dirty = false;

static void cts_reset(void) {
  s_cts_conn = BLE_HS_CONN_HANDLE_NONE;
  s_cts_svc_start = s_cts_svc_end = 0;
  s_cts_val = s_cts_cccd = 0;
  s_cts_subbed = s_cts_synced = false;
  s_cts_tries = 0;
  s_cts_next_ms = 0;
  s_cts_discovering = false;
}

/* ===================== apply a Current Time reading ======================= */

/* Current Time (0x2A2B) value, 10 bytes:
 *   [0..1] year LE   [2] month 1-12   [3] day 1-31   [4] hours   [5] minutes
 *   [6] seconds      [7] day-of-week  [8] fractions256   [9] adjust reason
 * Anything shorter/implausible is ignored rather than written to the RTC — a bad
 * clock is worse than a missing one (alarms, sleep rows and notification ids all
 * key off it). */
static bool cts_apply(const uint8_t *v, uint16_t len) {
  if (len < 7) { USBSerial.printf("[cts] short value (%u bytes)\n", (unsigned)len); return false; }
  uint16_t year = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
  uint8_t mon = v[2], day = v[3], hh = v[4], mi = v[5], ss = v[6];
  if (year < 2020 || year > 2099 || mon < 1 || mon > 12 || day < 1 || day > 31 ||
      hh > 23 || mi > 59 || ss > 60) {
    USBSerial.printf("[cts] implausible time %04u-%02u-%02u %02u:%02u:%02u — ignored\n",
                     year, mon, day, hh, mi, ss);
    return false;
  }
  if (ss > 59) ss = 59;                      // a leap second would fail mktime's range

  i2c_lock();                                // shared bus: the UI reads touch on core 1
  board_clock_set(year, mon, day, hh, mi, ss);
  i2c_unlock();

  struct tm tmv = {};
  tmv.tm_year = year - 1900; tmv.tm_mon = mon - 1; tmv.tm_mday = day;
  tmv.tm_hour = hh; tmv.tm_min = mi; tmv.tm_sec = ss; tmv.tm_isdst = -1;
  rtc_last_ntp_epoch = (uint32_t)mktime(&tmv);   // suppress a redundant NTP re-sync
  s_cts_synced = true;
  s_cts_dirty  = true;
  s_cts_next_ms = millis() + CTS_RESYNC_MS;
  USBSerial.printf("[cts] time sync from phone: %04u-%02u-%02u %02u:%02u:%02u\n",
                   year, mon, day, hh, mi, ss);
  return true;
}

/* ===================== read + subscribe =================================== */

static int cts_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (err && err->status != 0) { USBSerial.printf("[cts] CCCD write status=%d\n", err->status); return 0; }
  s_cts_subbed = true;
  USBSerial.println("[cts] subscribed — the phone will push clock changes");
  return 0;
}

static int cts_read_cb(uint16_t conn, const struct ble_gatt_error *err,
                       struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)arg;
  if (err && err->status != 0) {
    USBSerial.printf("[cts] read status=%d\n", err->status);
    return 0;
  }
  if (attr && attr->om) {
    uint8_t buf[16];
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len > sizeof(buf)) len = sizeof(buf);
    if (os_mbuf_copydata(attr->om, 0, len, buf) == 0) cts_apply(buf, len);
  }
  // Subscribing is a nice-to-have on top of the read: with it the phone pushes DST
  // and timezone changes the moment they happen. Some phones don't support notify
  // on 0x2A2B, hence the CCCD may simply not be there — the periodic re-read covers it.
  if (s_cts_cccd && !s_cts_subbed) {
    static const uint8_t en[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(s_cts_conn, s_cts_cccd, en, sizeof(en), cts_sub_done, NULL);
    if (rc != 0) USBSerial.printf("[cts] cccd write kickoff rc=%d\n", rc);
  }
  return 0;
}

static void cts_read_now(void) {
  if (!s_cts_val || s_cts_conn == BLE_HS_CONN_HANDLE_NONE) return;
  int rc = ble_gattc_read(s_cts_conn, s_cts_val, cts_read_cb, NULL);
  if (rc != 0) {
    USBSerial.printf("[cts] read kickoff rc=%d — will retry\n", rc);
    s_cts_next_ms = millis() + CTS_RETRY_MS;     // busy stack: try again shortly
  }
}

/* ===================== discovery ========================================== */

static int cts_dsc_cb(uint16_t conn, const struct ble_gatt_error *err,
                      uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  (void)conn; (void)chr_val_handle; (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) return 0;
  if (dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) s_cts_cccd = dsc->handle;
  if (err && err->status == BLE_HS_EDONE) {
    s_cts_discovering = false;
    cts_read_now();                  // read first; the read's completion subscribes
  }
  return 0;
}

static int cts_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_chr *chr, void *arg) {
  (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[cts] chr disc error status=%d\n", err->status);
    s_cts_discovering = false;
    return 0;
  }
  if (chr && ble_uuid_cmp(&chr->uuid.u, &CTS_CUR_TIME_UUID.u) == 0) s_cts_val = chr->val_handle;
  if (err && err->status == BLE_HS_EDONE) {
    if (!s_cts_val) {
      USBSerial.println("[cts] no Current Time characteristic");
      s_cts_discovering = false;
      return 0;
    }
    // Bound the descriptor scan tightly past the value handle so we get THIS
    // characteristic's CCCD, not a neighbour's (same rule as the ANCS chain).
    uint16_t end = s_cts_val + 2;
    if (end > s_cts_svc_end) end = s_cts_svc_end;
    ble_gattc_disc_all_dscs(conn, s_cts_val, end, cts_dsc_cb, NULL);
  }
  return 0;
}

static int cts_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_svc *svc, void *arg) {
  (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[cts] svc disc error status=%d\n", err->status);
    s_cts_discovering = false;
    return 0;
  }
  if (svc) { s_cts_svc_start = svc->start_handle; s_cts_svc_end = svc->end_handle; }
  if (err && err->status == BLE_HS_EDONE) {
    if (s_cts_svc_start) {
      USBSerial.printf("[cts] service [0x%04X..0x%04X] — discovering chars\n",
                       s_cts_svc_start, s_cts_svc_end);
      ble_gattc_disc_all_chrs(conn, s_cts_svc_start, s_cts_svc_end, cts_chr_cb, NULL);
    } else {
      // Android phones don't serve CTS (Gadgetbridge sends setTime instead), and iOS
      // only exposes it to a BONDED peer — so this is normal, not an error.
      USBSerial.println("[cts] no Current Time Service exposed (Android, or not trusted yet)");
      s_cts_discovering = false;
    }
  }
  return 0;
}

/* ===================== public entry points ================================ */

/* ARM the client — called from ancs_on_encrypted() when the link encrypts. Does no
 * GATT work itself: cts_poll() starts the discovery once ANCS/AMS are done. */
static void cts_on_encrypted(uint16_t conn_handle) {
  cts_reset();
  s_cts_conn    = conn_handle;
  s_cts_next_ms = millis() + CTS_FIRST_TRY_MS;
}

/* Called from the main loop every iteration. Kicks (or retries) discovery until the
 * clock has been synced once, then re-reads every CTS_RESYNC_MS to trim drift.
 * Cheap: a couple of comparisons on the common path. */
static void cts_poll(void) {
  if (s_cts_conn == BLE_HS_CONN_HANDLE_NONE || !s_ble_connected) return;
  if (s_cts_discovering) return;
  if (!s_cts_next_ms || (int32_t)(millis() - s_cts_next_ms) < 0) return;

  if (s_cts_val) {                       // already discovered -> this is a drift re-read
    s_cts_next_ms = millis() + CTS_RESYNC_MS;
    cts_read_now();
    return;
  }
  if (s_cts_tries >= CTS_MAX_TRIES) { s_cts_next_ms = 0; return; }   // give up quietly
  s_cts_tries++;
  s_cts_next_ms = millis() + CTS_RETRY_MS;
  s_cts_discovering = true;
  int rc = ble_gattc_disc_svc_by_uuid(s_cts_conn, &CTS_SVC_UUID.u, cts_svc_cb, NULL);
  if (rc != 0) {
    s_cts_discovering = false;           // busy/out of procedures -> retry on the next window
    USBSerial.printf("[cts] disc_svc_by_uuid rc=%d (try %u)\n", rc, (unsigned)s_cts_tries);
  }
}

/* True once each time the clock was stepped by a CTS reading, so the loop can force
 * a watchface redraw instead of waiting for its next minute tick. */
static bool cts_take_dirty(void) {
  if (!s_cts_dirty) return false;
  s_cts_dirty = false;
  return true;
}

/* Have we ever synced from the phone on this link? (Diagnostics / settings UI.) */
static bool cts_synced(void) { return s_cts_synced; }

/* Notify-RX router: called from the shared GAP handler in ble_ancs.h. Returns true
 * if this was our characteristic. The phone pushes a fresh Current Time on timezone
 * / DST / manual clock changes. Runs on the NimBLE host task. */
static bool cts_handle_notify_rx(uint16_t conn_handle, uint16_t attr_handle,
                                 const struct os_mbuf *om) {
  if (!om || conn_handle != s_cts_conn || !s_cts_val || attr_handle != s_cts_val) return false;
  uint8_t buf[16];
  uint16_t len = OS_MBUF_PKTLEN((struct os_mbuf *)om);
  if (len > sizeof(buf)) len = sizeof(buf);
  if (os_mbuf_copydata(om, 0, len, buf) == 0) cts_apply(buf, len);
  return true;
}
