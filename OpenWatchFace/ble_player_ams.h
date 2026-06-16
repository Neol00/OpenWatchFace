/* ============================================================================
 *  ble_player_ams.h — AMS (Apple Media Service) CLIENT: feeds the source-agnostic
 *  Player (player_state.h) from the iPhone over the existing bonded BLE link.
 *
 *  AMS is the media cousin of ANCS: same GATT-client-on-an-inbound-connection
 *  pattern (we are the client; the iPhone hosts the service), same encrypted-bond
 *  prerequisite, same notify interception via the custom GAP handler. It provides
 *  "now playing" metadata + playback state and accepts transport commands.
 *
 *  AMS GATT (service 89D3502B-...):
 *    - Remote Command  (write + notify): we WRITE a 1-byte command (play/pause/...);
 *      the notify lists which commands are currently available.
 *    - Entity Update   (write + notify): we WRITE [EntityID, AttrIDs...] to register
 *      interest; the phone NOTIFIES "[EntityID][AttrID][Flags] value" on change.
 *    - Entity Attribute (read): one-shot fetch of a truncated attribute (unused here;
 *      Entity Update streaming covers our needs).
 *
 *  WHAT WE SUBSCRIBE TO:
 *    Entity Player(0): attr PlaybackInfo(0) -> "state,rate,elapsed" (we use state).
 *    Entity Track(2):  attrs Artist(0), Album(1), Title(2).
 *  On each update we push into player_state. We register as the active command sink
 *  so the Player UI's buttons route here.
 *
 *  THREADING: callbacks run on the NimBLE host task. They only touch player_state
 *  (under its lock) — never LVGL. Commands come FROM the loop via the registered
 *  sink (ams_cmd) and write a GATT characteristic; safe from the loop thread.
 *
 *  INCLUDE AFTER ble_ancs.h (reuses its conn handle + the custom GAP handler routes
 *  notifies here) and player_state.h. ble_provision.h calls ams_on_encrypted/
 *  ams_reset alongside the ANCS ones. Header-only, in the .ino TU.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>

/* AMS UUIDs (Apple-defined), stored LSB-first for NimBLE. */
static const ble_uuid128_t AMS_SVC_UUID = BLE_UUID128_INIT(   // 89D3502B-0F36-433A-8EF4-C502AD55F8DC
    0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E,
    0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89);
static const ble_uuid128_t AMS_REMOTE_CMD_UUID = BLE_UUID128_INIT( // 9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2
    0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8,
    0x8A, 0x4A, 0xB1, 0x57, 0xD8, 0x81, 0x3C, 0x9B);
static const ble_uuid128_t AMS_ENTITY_UPD_UUID = BLE_UUID128_INIT( // 2F7CABCE-808D-411F-9A0C-BB92BA96C102
    0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A,
    0x1F, 0x41, 0x8D, 0x80, 0xCE, 0xAB, 0x7C, 0x2F);
static const ble_uuid128_t AMS_ENTITY_ATTR_UUID = BLE_UUID128_INIT( // C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7
    0xD7, 0xD5, 0xBB, 0x70, 0xA8, 0xA3, 0xAB, 0xA6,
    0xD8, 0x46, 0xAB, 0x23, 0x8C, 0xF3, 0xB2, 0xC6);

/* AMS Remote Command IDs (1-byte writes to Remote Command). */
#define AMS_CMD_PLAY        0
#define AMS_CMD_PAUSE       1
#define AMS_CMD_TOGGLE      2
#define AMS_CMD_NEXT        3
#define AMS_CMD_PREV        4

/* AMS Entity + Attribute IDs. */
#define AMS_ENTITY_PLAYER   0
#define AMS_ENTITY_TRACK    2
#define AMS_PLAYER_ATTR_NAME     0   // localized app name (e.g. "Music")
#define AMS_PLAYER_ATTR_PLAYBACK 1   // "state,rate,elapsed" CSV; state 0=paused 1=playing
#define AMS_PLAYER_ATTR_VOLUME   2   // float 0..1 as string
#define AMS_TRACK_ATTR_ARTIST    0
#define AMS_TRACK_ATTR_ALBUM     1
#define AMS_TRACK_ATTR_TITLE     2

/* ---- discovered handles (per connection) ---- */
static uint16_t s_ams_conn      = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_ams_svc_start = 0;
static uint16_t s_ams_svc_end   = 0;
static uint16_t s_ams_rc_val    = 0;   // Remote Command value handle (we write commands here)
static uint16_t s_ams_rc_cccd   = 0;
static uint16_t s_ams_eu_val    = 0;   // Entity Update value handle (write to subscribe; notifies updates)
static uint16_t s_ams_eu_cccd   = 0;
static uint16_t s_ams_ea_val    = 0;   // Entity Attribute value handle (write to select, then READ to poll)
static bool     s_ams_rc_subbed = false;
static bool     s_ams_eu_subbed = false;

/* Latest per-field cache (so a Track update of only Title keeps the prior Artist). */
static char s_ams_title[PLAYER_STR_MAX]  = "";
static char s_ams_artist[PLAYER_STR_MAX] = "";
static char s_ams_album[PLAYER_STR_MAX]  = "";

static void ams_reset(void) {
  s_ams_conn = BLE_HS_CONN_HANDLE_NONE;
  s_ams_svc_start = s_ams_svc_end = 0;
  s_ams_rc_val = s_ams_rc_cccd = 0;
  s_ams_eu_val = s_ams_eu_cccd = 0;
  s_ams_ea_val = 0;
  s_ams_rc_subbed = s_ams_eu_subbed = false;
  s_ams_title[0] = s_ams_artist[0] = s_ams_album[0] = '\0';
  player_clear(PSRC_AMS);
}

/* ---- command sink: the Player UI calls this (via player_send_command) ---- */
static void ams_write_cmd(uint8_t cmd) {
  if (!s_ams_rc_val || s_ams_conn == BLE_HS_CONN_HANDLE_NONE) return;
  ble_gattc_write_flat(s_ams_conn, s_ams_rc_val, &cmd, 1, NULL, NULL);
}
static void ams_cmd(PlayerCmd cmd) {
  switch (cmd) {
    case PCMD_TOGGLE: ams_write_cmd(AMS_CMD_TOGGLE); break;
    case PCMD_NEXT:   ams_write_cmd(AMS_CMD_NEXT);   break;
    case PCMD_PREV:   ams_write_cmd(AMS_CMD_PREV);   break;
  }
}

/* ---- sync sink: poll the TRUE playback state from the phone (self-correct the
 * play/pause button if a pushed Entity Update was ever missed). AMS Entity Attribute
 * works as select-then-read: WRITE [Entity, Attr] to choose what to fetch, then READ
 * the same characteristic to get its value. The read reply (just the value bytes,
 * no entity/attr header) is parsed in ams_ea_read_cb. */
static int ams_ea_read_cb(uint16_t conn, const struct ble_gatt_error *err,
                          struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)arg;
  if (err && err->status != 0) return 0;
  if (!attr || !attr->om) return 0;
  char buf[PLAYER_STR_MAX];
  uint16_t mlen = OS_MBUF_PKTLEN(attr->om);
  uint16_t n = mlen < PLAYER_STR_MAX - 1 ? mlen : PLAYER_STR_MAX - 1;
  if (os_mbuf_copydata(attr->om, 0, n, buf) != 0) return 0;
  buf[n] = '\0';
  // Value is PlaybackInfo "state,rate,elapsed"; first field is the state.
  // IMPORTANT: Entity Attribute is meant for re-reading TRUNCATED Entity Update values;
  // some iOS versions return an EMPTY value when polled for PlaybackInfo. An empty (or
  // non-numeric) reply must NOT be treated as state 0 (paused) — that would stomp the
  // correct pushed state every poll and pin the button to "paused". Only accept a reply
  // that actually starts with a digit; otherwise ignore it and trust the last push.
  if (n == 0 || buf[0] < '0' || buf[0] > '9') return 0;
  int state = atoi(buf);
  player_set_state(state == 1 ? PLAYING : PLAY_PAUSED);
  return 0;
}
static int ams_ea_select_cb(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg) {
  (void)attr; (void)arg;
  if (err && err->status != 0) return 0;
  ble_gattc_read(conn, s_ams_ea_val, ams_ea_read_cb, NULL);   // now read the selected value
  return 0;
}
static void ams_sync(void) {
  if (!s_ams_ea_val || s_ams_conn == BLE_HS_CONN_HANDLE_NONE) return;
  const uint8_t sel[] = { AMS_ENTITY_PLAYER, AMS_PLAYER_ATTR_PLAYBACK };
  ble_gattc_write_flat(s_ams_conn, s_ams_ea_val, sel, sizeof(sel), ams_ea_select_cb, NULL);
}

/* ---- Entity Update notify: parse "[EntityID][AttrID][Flags] value..." ---- */
static void ams_entity_update_rx(const uint8_t *d, uint16_t len) {
  if (len < 3) return;
  uint8_t entity = d[0];
  uint8_t attr   = d[1];
  // d[2] = flags (bit0 = truncated). Value is the remaining bytes (UTF-8, NOT NUL-term).
  const char *val = (const char *)&d[3];
  uint16_t vlen = len - 3;
  char buf[PLAYER_STR_MAX];
  uint16_t n = vlen < PLAYER_STR_MAX - 1 ? vlen : PLAYER_STR_MAX - 1;
  memcpy(buf, val, n); buf[n] = '\0';

  if (entity == AMS_ENTITY_TRACK) {
    if      (attr == AMS_TRACK_ATTR_ARTIST) strncpy(s_ams_artist, buf, sizeof(s_ams_artist));
    else if (attr == AMS_TRACK_ATTR_ALBUM)  strncpy(s_ams_album,  buf, sizeof(s_ams_album));
    else if (attr == AMS_TRACK_ATTR_TITLE)  strncpy(s_ams_title,  buf, sizeof(s_ams_title));
    s_ams_artist[sizeof(s_ams_artist)-1] = s_ams_album[sizeof(s_ams_album)-1] =
      s_ams_title[sizeof(s_ams_title)-1] = '\0';
    player_set_sink(PSRC_AMS, ams_cmd, ams_sync);              // we're the active source
    player_set_track(s_ams_title, s_ams_artist, s_ams_album);
  } else if (entity == AMS_ENTITY_PLAYER && attr == AMS_PLAYER_ATTR_PLAYBACK) {
    // "state,rate,elapsed" — state: 0=paused 1=playing 2=rewind 3=fast-fwd.
    int state = atoi(buf);
    player_set_sink(PSRC_AMS, ams_cmd, ams_sync);
    player_set_state(state == 1 ? PLAYING : PLAY_PAUSED);
  }
}

/* ---- subscribe + register, FULLY SERIALIZED ----
 * NimBLE allows only ONE GATT procedure per connection at a time, so every write
 * below waits for the previous one's completion callback before issuing the next:
 *   EU CCCD subscribe -> RC CCCD subscribe -> register Track -> register Player.
 * Firing them back-to-back makes all but the first fail with "busy" and the entity
 * registration silently never happens (=> no updates => "Nothing playing"). */

static void ams_write_cccd(uint16_t cccd, int (*cb)(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *)) {
  static const uint8_t en[2] = { 0x01, 0x00 };
  ble_gattc_write_flat(s_ams_conn, cccd, en, sizeof(en), cb, NULL);
}

/* step 4: register the Track entity -> done; both registered, updates flow. */
static int ams_reg_track_done(uint16_t conn, const struct ble_gatt_error *err,
                              struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (err && err->status != 0) USBSerial.printf("[ams] reg Track status=%d\n", err->status);
  return 0;
}
/* step 3: register the PLAYER entity (PlaybackInfo — drives the play/pause button) -> then
 * the Track entity. Player is registered FIRST and on the clean first write because it is
 * the critical one; Track follows on the chained write (which we already know works). */
static int ams_reg_player_done(uint16_t conn, const struct ble_gatt_error *err,
                               struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (err && err->status != 0) USBSerial.printf("[ams] reg Player status=%d (registering Track anyway)\n", err->status);
  const uint8_t track[] = { AMS_ENTITY_TRACK, AMS_TRACK_ATTR_ARTIST,
                            AMS_TRACK_ATTR_ALBUM, AMS_TRACK_ATTR_TITLE };
  ble_gattc_write_flat(s_ams_conn, s_ams_eu_val, track, sizeof(track), ams_reg_track_done, NULL);
  return 0;
}
/* step 2b (after RC subscribe): write the Player entity registration first. */
static void ams_register_entities(void) {
  const uint8_t player[] = { AMS_ENTITY_PLAYER, AMS_PLAYER_ATTR_PLAYBACK };
  ble_gattc_write_flat(s_ams_conn, s_ams_eu_val, player, sizeof(player), ams_reg_player_done, NULL);
}
/* step 2: RC subscribed -> register entities. */
static int ams_rc_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (err && err->status != 0) USBSerial.printf("[ams] RC CCCD status=%d\n", err->status);
  else s_ams_rc_subbed = true;
  if (s_ams_eu_val) ams_register_entities();   // proceed even if RC failed (EU is what matters)
  return 0;
}
/* step 1: EU subscribed -> subscribe RC next. */
static int ams_eu_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (err && err->status != 0) { USBSerial.printf("[ams] EU CCCD status=%d\n", err->status); return 0; }
  s_ams_eu_subbed = true;
  if (s_ams_rc_cccd) ams_write_cccd(s_ams_rc_cccd, ams_rc_sub_done);
  else               ams_register_entities();   // no RC CCCD? still register entities
  return 0;
}

/* ---- descriptor discovery (find the two CCCDs), chained -> start subscribe chain ---- */
static int ams_dsc_eu_cb(uint16_t conn, const struct ble_gatt_error *err,
                         uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  (void)conn; (void)chr_val_handle; (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) return 0;
  if (dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) s_ams_eu_cccd = dsc->handle;
  if (err && err->status == BLE_HS_EDONE) {
    if (s_ams_eu_cccd) ams_write_cccd(s_ams_eu_cccd, ams_eu_sub_done);   // start the serial chain
    else USBSerial.println("[ams] Entity Update CCCD not found");
  }
  return 0;
}
static int ams_dsc_rc_cb(uint16_t conn, const struct ble_gatt_error *err,
                         uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  (void)chr_val_handle; (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) return 0;
  if (dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) s_ams_rc_cccd = dsc->handle;
  if (err && err->status == BLE_HS_EDONE) {
    uint16_t eu_end = s_ams_eu_val + 2;
    if (eu_end > s_ams_svc_end) eu_end = s_ams_svc_end;
    ble_gattc_disc_all_dscs(conn, s_ams_eu_val, eu_end, ams_dsc_eu_cb, NULL);
  }
  return 0;
}

/* ---- characteristic + service discovery ---- */
static int ams_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_chr *chr, void *arg) {
  (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) return 0;
  if (chr) {
    if      (ble_uuid_cmp(&chr->uuid.u, &AMS_REMOTE_CMD_UUID.u)  == 0) s_ams_rc_val = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &AMS_ENTITY_UPD_UUID.u)  == 0) s_ams_eu_val = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &AMS_ENTITY_ATTR_UUID.u) == 0) s_ams_ea_val = chr->val_handle;
  }
  if (err && err->status == BLE_HS_EDONE) {
    // Entity Update is the ONLY hard requirement — it delivers now-playing metadata,
    // which is what the Player displays. Remote Command (controls) is optional: if it
    // wasn't found we still show the track, the buttons just won't do anything.
    if (s_ams_eu_val) {
      if (s_ams_rc_val) {
        // Find RC's CCCD first (chains to EU's, then the subscribe sequence).
        uint16_t rc_end = s_ams_rc_val + 2;
        if (rc_end > s_ams_svc_end) rc_end = s_ams_svc_end;
        ble_gattc_disc_all_dscs(conn, s_ams_rc_val, rc_end, ams_dsc_rc_cb, NULL);
      } else {
        // No RC: skip straight to discovering EU's CCCD + subscribing.
        USBSerial.println("[ams] no Remote Command char — display only (no controls)");
        uint16_t eu_end = s_ams_eu_val + 2;
        if (eu_end > s_ams_svc_end) eu_end = s_ams_svc_end;
        ble_gattc_disc_all_dscs(conn, s_ams_eu_val, eu_end, ams_dsc_eu_cb, NULL);
      }
    } else {
      USBSerial.println("[ams] Entity Update characteristic not found — cannot read now-playing");
    }
  }
  return 0;
}
static int ams_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_svc *svc, void *arg) {
  (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) return 0;
  if (svc) { s_ams_svc_start = svc->start_handle; s_ams_svc_end = svc->end_handle; }
  if (err && err->status == BLE_HS_EDONE) {
    if (s_ams_svc_start) {
      ble_gattc_disc_all_chrs(conn, s_ams_svc_start, s_ams_svc_end, ams_chr_cb, NULL);
    } else {
      USBSerial.println("[ams] no AMS service exposed (no media app, or not trusted yet)");
    }
  }
  return 0;
}

/* ---- public entry points ---- */

/* Called from onAuthenticationComplete once encrypted (alongside ancs_on_encrypted).
 * Discovers AMS + subscribes. iOS only exposes AMS when a media app is present, so a
 * "no AMS service" log is normal when nothing has played yet. */
static void ams_on_encrypted(uint16_t conn_handle) {
  if (s_ams_eu_subbed && conn_handle == s_ams_conn) return;
  ams_reset();
  s_ams_conn = conn_handle;
  int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &AMS_SVC_UUID.u, ams_svc_cb, NULL);
  if (rc != 0) USBSerial.printf("[ams] disc_svc_by_uuid rc=%d\n", rc);
}

/* Notify-RX router: called from the custom GAP handler (ble_ancs.h's ancs_gap_event)
 * for inbound notifications. Returns true if the notification was an AMS one we
 * consumed. Runs on the NimBLE host task. */
static bool ams_handle_notify_rx(uint16_t conn_handle, uint16_t attr_handle,
                                 const struct os_mbuf *om) {
  if (conn_handle != s_ams_conn || !om) return false;
  if (attr_handle != s_ams_eu_val && attr_handle != s_ams_rc_val) return false;

  uint8_t buf[PLAYER_STR_MAX + 8];
  uint16_t mlen = OS_MBUF_PKTLEN((struct os_mbuf *)om);
  if (mlen > sizeof(buf)) mlen = sizeof(buf);
  if (os_mbuf_copydata(om, 0, mlen, buf) != 0) return true;

  if (attr_handle == s_ams_eu_val) ams_entity_update_rx(buf, mlen);
  // Remote Command notify (available-commands list) is informational; we ignore it.
  return true;
}
