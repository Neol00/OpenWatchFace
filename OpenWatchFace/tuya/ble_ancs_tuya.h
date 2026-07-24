/* ============================================================================
 *  ble_ancs_tuya.h - ANCS (Apple Notification Center Service) CLIENT for the
 *  Tuya T5-E1, on the RAW NimBLE host (Phase B of the Tuya BLE port).
 *
 *  Direct port of the ESP32 build's ble_ancs.h. The protocol machine (discovery,
 *  subscribe, attribute round-trips, reassembly, dedup fingerprints, incoming-call
 *  handling) is identical; only the plumbing differs:
 *    - notify-RX arrives via ble_tuya_route_notify_rx() (called from ble_tuya.h's
 *      owf_gap_event on BLE_GAP_EVENT_NOTIFY_RX) instead of a custom GAP handler
 *      registered on the Arduino core (there is no core here - WE own the handler).
 *    - the store->loop flags (s_ancs_ui_dirty / s_ancs_removed / s_ancs_added_total)
 *      and their take-accessors live in ble_notif_flags_tuya.h (shared with the
 *      Gadgetbridge server), so this file does NOT define them.
 *    - UUIDs use the OWF_UUID128 positional-init macro from ble_tuya.h (the SDK's
 *      BLE_UUID128_INIT uses C designated initializers this C++ dialect rejects).
 *    - AMS is not ported yet: ams_on_encrypted()/ams_reset() are no-op stubs at the
 *      bottom (guarded by BLE_TUYA_HAVE_AMS so a future ble_ams_tuya.h replaces them).
 *
 *  REQUIRES the SM-enabled libtuyaos.a rebuild (TY_HS_BLE_SM_SC/LEGACY/BONDING/MITM=1
 *  + vendored mesh_crypt TinyCrypt sources): iOS only exposes ANCS on an encrypted,
 *  bonded link. See tuya-t5e1-ble-stack-facts in project memory for the rebuild spec.
 *
 *  THREADING: every callback here runs on the NimBLE host task, never touches LVGL.
 *  It writes the store under store_lock and sets s_pop_* + s_ancs_ui_dirty; the loop
 *  shows the popup and refreshes the bell (same contract as ble_gadgetbridge_tuya.h).
 *
 *  INCLUDE ORDER (.ino, Tuya branch): after ble_tuya.h (OWF_UUID128, s_ble_conn,
 *  ble_begin/ble_end), notif_store.h / notif_archive_sd.h / notif_net.h (store,
 *  archive, s_pop_*), ble_notif_flags_tuya.h (shared flags) and
 *  ble_gadgetbridge_tuya.h. The .ino defines BLE_TUYA_HAVE_ANCS 1 BEFORE including
 *  ble_tuya.h so its inert Phase-A stubs are suppressed. Header-only, in the .ino TU.
 * ========================================================================== */
#pragma once
#include <Arduino.h>

/* ---- ANCS UUIDs (Apple-defined, 128-bit), stored LSB-first for NimBLE ---- */
static const ble_uuid128_t ANCS_SVC_UUID = OWF_UUID128(   /* 7905F431-B5CE-4E99-A40F-4B1E122D00D0 */
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
static const ble_uuid128_t ANCS_NOTIF_SRC_UUID = OWF_UUID128( /* 9FBF120D-6301-42D9-8C58-25E699A21DBD */
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C,
    0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
static const ble_uuid128_t ANCS_CTRL_PT_UUID = OWF_UUID128(   /* 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9 */
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98,
    0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
static const ble_uuid128_t ANCS_DATA_SRC_UUID = OWF_UUID128(  /* 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB */
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE,
    0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);

/* ANCS protocol constants */
#define ANCS_CMD_GET_NOTIF_ATTRS   0x00   /* Control Point command: fetch attributes */
#define ANCS_CMD_PERFORM_ACTION    0x02   /* Control Point command: act on a notification */
#define ANCS_ACTION_POSITIVE       0x00   /* e.g. "view"/accept */
#define ANCS_ACTION_NEGATIVE       0x01   /* dismiss / clear (what a watch swipe sends) */
#define ANCS_EVENT_ADDED           0x00
#define ANCS_EVENT_MODIFIED        0x01
#define ANCS_EVENT_REMOVED         0x02
#define ANCS_EVENTFLAG_PREEXISTING 0x04   /* bit set on backlog-replay items */

/* ANCS CategoryID (8-byte NS event byte [2]). Apple-defined. Only the two CALL
 * categories are special-cased; the STORED notification's category still comes from
 * the app id (notif_cat_from_appid).
 *
 * WHY - the "duplicate missed call" bug: one missed call produces TWO ANCS
 * notifications. First INCOMING_CALL (transient: "X is calling..."), which iOS
 * REMOVEs when the call ends, then a brand-new MISSED_CALL. Storing the incoming
 * call like a normal notification leaves it lingering next to the missed call ->
 * two cards. So INCOMING_CALL is LIVE, transient state (never stored; auto-cleared
 * on its REMOVED) and only MISSED_CALL becomes the single stored notification. */
#define ANCS_CAT_OTHER             0
#define ANCS_CAT_INCOMING_CALL     1
#define ANCS_CAT_MISSED_CALL       2
#define ANCS_CAT_VOICEMAIL         3
#define ANCS_ATTR_APP_ID           0x00
#define ANCS_ATTR_TITLE            0x01
#define ANCS_ATTR_MESSAGE          0x03
#define ANCS_ATTR_DATE             0x05   /* timestamp "yyyyMMdd'T'HHmmSS" */
#define ANCS_ATTR_TITLE_MAXLEN     48     /* ask iOS to cap title length */
#define ANCS_ATTR_MSG_MAXLEN       220    /* ...and message length (fits NOTIF_BODY_MAX) */
#define ANCS_ATTR_DATE_LEN         16     /* fixed-format date string length (no NUL) */

/* Map an ANCS-derived id into the store's 64-bit id space, high above HTTP ids
 * (epoch-millis, ~1e12), so the sources never collide. */
#define ANCS_ID_BASE  ((uint64_t)0x5000000000000000ULL)

/* ---- per-connection discovered handles + state (reset on disconnect) ---- */
static uint16_t s_ancs_conn       = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_ancs_svc_start  = 0;
static uint16_t s_ancs_svc_end    = 0;
static uint16_t s_ancs_ns_val     = 0;    /* Notification Source value handle */
static uint16_t s_ancs_ns_cccd    = 0;
static uint16_t s_ancs_cp_val     = 0;    /* Control Point value handle (requests) */
static uint16_t s_ancs_ds_val     = 0;    /* Data Source value handle (replies) */
static uint16_t s_ancs_ds_cccd    = 0;
static bool     s_ancs_ns_subbed  = false;
static bool     s_ancs_ds_subbed  = false;
static bool     s_ancs_pair_kicked = false;  /* security re-initiated after an auth bounce */
static uint32_t s_ancs_pair_kick_ms = 0;     /* when it was kicked (pair watchdog) */
static uint8_t  s_ancs_pair_tries   = 0;     /* watchdog escalation state (0..2) */

/* NOTE: s_ancs_ui_dirty / s_ancs_removed / s_ancs_added_total and the take-accessors
 * are defined in ble_notif_flags_tuya.h (shared with Gadgetbridge) - not here. */

/* ---- Data Source reassembly buffer ----
 * One attribute reply may span several GATT notifications; accumulate until a
 * complete record parses. Requests are serial, so one buffer suffices. */
#define ANCS_DS_BUF_MAX  600
static uint8_t  s_ds_buf[ANCS_DS_BUF_MAX];
static uint16_t s_ds_len = 0;

/* ANCS CategoryID of the in-flight attribute request (serial round-trips, so a
 * single value). Set in ancs_request_attrs, read in ancs_parse_and_store. */
static uint8_t s_inflight_cat = ANCS_CAT_OTHER;

/* ---- pending attribute-request queue ----
 * iOS replays the whole backlog as ONE burst of ADDED events after the NS
 * subscribe. A Control Point write per event exhausts NimBLE's per-connection GATT
 * procedure slots (rc=6 BLE_HS_ENOMEM -> notifications silently LOST). So ADDED
 * events queue here and pump strictly one-in-flight. */
#define ANCS_REQQ_MAX 32
static uint32_t s_reqq_uid[ANCS_REQQ_MAX];
static uint8_t  s_reqq_cat[ANCS_REQQ_MAX];
static uint8_t  s_reqq_head = 0, s_reqq_count = 0;   /* ring (head = next to send) */
static bool     s_req_inflight   = false;
static uint32_t s_req_started_ms = 0;                /* stale-reply watchdog */
static void ancs_req_complete(void);                 /* defined after the pump */

/* ---- content fingerprint (durable de-dup key) ----
 * FNV-1a 32-bit hash of app + 0x1f + date + 0x1f + title. Stable across
 * reconnects/power-off (the ANCS UID is not), so a backlog replay after sleep/wake
 * doesn't re-add the same item. Store id = ANCS_ID_BASE | fingerprint. */
static uint32_t ancs_fnv1a(uint32_t h, const char *s) {
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}
static uint32_t ancs_fingerprint(const char *app, const char *date, const char *title) {
  uint32_t h = 2166136261u;                  /* FNV offset basis */
  h = ancs_fnv1a(h, app);   h = ancs_fnv1a(h, "\x1f");
  h = ancs_fnv1a(h, date);  h = ancs_fnv1a(h, "\x1f");
  h = ancs_fnv1a(h, title);
  return h;
}

/* ---- session UID -> store-id map ----
 * A REMOVED event carries only the live UID; this ring remembers which store id
 * each UID mapped to THIS connection. Cleared on disconnect (UIDs are per-session). */
#define ANCS_UIDMAP_MAX  64
static uint32_t s_uidmap_uid[ANCS_UIDMAP_MAX];
static uint64_t s_uidmap_id[ANCS_UIDMAP_MAX];
static uint8_t  s_uidmap_head = 0;   /* next slot to overwrite (ring) */
static uint8_t  s_uidmap_count = 0;

static void ancs_uidmap_put(uint32_t uid, uint64_t id) {
  for (uint8_t i = 0; i < s_uidmap_count; i++)        /* update if already mapped */
    if (s_uidmap_uid[i] == uid) { s_uidmap_id[i] = id; return; }
  s_uidmap_uid[s_uidmap_head] = uid;
  s_uidmap_id[s_uidmap_head]  = id;
  s_uidmap_head = (s_uidmap_head + 1) % ANCS_UIDMAP_MAX;
  if (s_uidmap_count < ANCS_UIDMAP_MAX) s_uidmap_count++;
}
/* Resolve a UID to its store id; false if never seen this session. */
static bool ancs_uidmap_get(uint32_t uid, uint64_t *out) {
  for (uint8_t i = 0; i < s_uidmap_count; i++)
    if (s_uidmap_uid[i] == uid) { *out = s_uidmap_id[i]; return true; }
  return false;
}
/* Reverse: store id -> live UID (for dismissing on the phone). Only works for
 * notifications iOS sent THIS connection. */
static bool ancs_uidmap_get_uid(uint64_t id, uint32_t *out) {
  for (uint8_t i = 0; i < s_uidmap_count; i++)
    if (s_uidmap_id[i] == id) { *out = s_uidmap_uid[i]; return true; }
  return false;
}

/* ---- live incoming-call state (transient; never written to the store) ----
 * Set on an INCOMING_CALL ADDED, cleared on its REMOVED (answered/declined/ended).
 * Keeps the incoming call OUT of the stored list so a missed call stays a single
 * entry; also the hook for a future live call screen + answer/decline. */
static volatile bool     s_incoming_active = false;            /* ringing now */
static volatile uint32_t s_incoming_uid    = 0;                /* its live ANCS UID */
static char              s_incoming_who[NOTIF_TITLE_MAX] = ""; /* caller, for the UI */
static volatile bool     s_incoming_dirty  = false;            /* loop should refresh */

static void ancs_reset(void) {
  s_ancs_conn      = BLE_HS_CONN_HANDLE_NONE;
  s_ancs_svc_start = 0; s_ancs_svc_end = 0;
  s_ancs_ns_val = 0; s_ancs_ns_cccd = 0;
  s_ancs_cp_val = 0;
  s_ancs_ds_val = 0; s_ancs_ds_cccd = 0;
  s_ancs_ns_subbed = false; s_ancs_ds_subbed = false;
  s_ancs_pair_kicked = false;
  s_ancs_pair_kick_ms = 0; s_ancs_pair_tries = 0;
  s_ds_len = 0;
  s_uidmap_head = 0; s_uidmap_count = 0;   /* UIDs are per-connection */
  s_reqq_head = 0; s_reqq_count = 0;       /* ...and so are queued requests */
  s_req_inflight = false;
  /* A ringing call can't survive a disconnect; drop live state so a reconnect
   * doesn't show a stale "incoming call". */
  if (s_incoming_active) { s_incoming_active = false; s_incoming_uid = 0; s_incoming_dirty = true; }
}

/* ===================== attribute reply parsing ============================ */

/* Parse a COMPLETE "Get Notification Attributes" response in s_ds_buf and, if it
 * carries a title/message, push it into the notification store. Layout:
 *   [CommandID=0][UID:4 LE][ {AttrID:1}{Len:2 LE}{Data:Len} ... ]
 * Runs on the NimBLE task -> guards the store, never touches LVGL. */
static void ancs_parse_and_store(void) {
  if (s_ds_len < 5 || s_ds_buf[0] != ANCS_CMD_GET_NOTIF_ATTRS) {
    s_ds_len = 0;
    ancs_req_complete();           /* malformed reply still frees the slot */
    return;
  }
  uint32_t uid = (uint32_t)s_ds_buf[1] | ((uint32_t)s_ds_buf[2] << 8) |
                 ((uint32_t)s_ds_buf[3] << 16) | ((uint32_t)s_ds_buf[4] << 24);

  char title[NOTIF_TITLE_MAX] = "";
  char body[NOTIF_BODY_MAX]   = "";
  char app[40]                = "";
  char date[ANCS_ATTR_DATE_LEN + 1] = "";

  uint16_t p = 5;
  while (p + 3 <= s_ds_len) {
    uint8_t  attr = s_ds_buf[p];
    uint16_t alen = (uint16_t)s_ds_buf[p + 1] | ((uint16_t)s_ds_buf[p + 2] << 8);
    p += 3;
    if (p + alen > s_ds_len) return;          /* attribute incomplete -> wait for more */
    const char *src = (const char *)&s_ds_buf[p];
    switch (attr) {
      case ANCS_ATTR_APP_ID: {
        uint16_t n = alen < sizeof(app) - 1 ? alen : sizeof(app) - 1;
        memcpy(app, src, n); app[n] = '\0';
        break;
      }
      case ANCS_ATTR_TITLE: {
        uint16_t n = alen < NOTIF_TITLE_MAX - 1 ? alen : NOTIF_TITLE_MAX - 1;
        memcpy(title, src, n); title[n] = '\0';
        break;
      }
      case ANCS_ATTR_MESSAGE: {
        uint16_t n = alen < NOTIF_BODY_MAX - 1 ? alen : NOTIF_BODY_MAX - 1;
        memcpy(body, src, n); body[n] = '\0';
        break;
      }
      case ANCS_ATTR_DATE: {
        uint16_t n = alen < ANCS_ATTR_DATE_LEN ? alen : ANCS_ATTR_DATE_LEN;
        memcpy(date, src, n); date[n] = '\0';
        break;
      }
      default: break;
    }
    p += alen;
  }

  /* Full record. A title is the minimum worth showing; fall back to the app id
   * if iOS sent an empty title. */
  if (title[0] == '\0' && app[0] != '\0') {
    strncpy(title, app, NOTIF_TITLE_MAX - 1);
    title[NOTIF_TITLE_MAX - 1] = '\0';
  }
  if (title[0] == '\0' && body[0] == '\0') {                          /* nothing usable */
    s_ds_len = 0;
    ancs_req_complete();
    return;
  }

  /* INCOMING CALL: transient - do NOT write to the store (see category note above).
   * Track as live ringing state; self-clears on the REMOVED event. */
  if (s_inflight_cat == ANCS_CAT_INCOMING_CALL) {
    s_incoming_uid = uid;
    strncpy(s_incoming_who, title, sizeof(s_incoming_who) - 1);
    s_incoming_who[sizeof(s_incoming_who) - 1] = '\0';
    ancs_uidmap_put(uid, ANCS_ID_BASE | (uint64_t)uid);  /* so decline can find the UID */
    s_incoming_active = true;
    s_incoming_dirty  = true;
    USBSerial.printf("[ancs] incoming call from \"%s\" (uid=%lu) - transient, not stored\n",
                     s_incoming_who, (unsigned long)uid);
    s_ds_len = 0;
    ancs_req_complete();
    return;
  }

  /* DURABLE id: content fingerprint (app + date + title), stable across reconnects
   * and power-off; the UID is not. Store/archive/removal all key on this id. */
  uint32_t fp = ancs_fingerprint(app, date, title);
  uint64_t id = ANCS_ID_BASE | (uint64_t)fp;
  ancs_uidmap_put(uid, id);        /* so a REMOVED (UID-only) can find the entry */
  uint8_t cat = notif_cat_from_appid(app);   /* per-app icon/layout */

  store_lock();
  /* NVS cache FIRST: it de-dups by id, telling us whether this is genuinely NEW.
   * Only then append to the archive (na_append has NO de-dup of its own) - this is
   * what stops duplicate archive lines on every backlog replay after sleep/wake. */
  bool added = notif_store_add(id, title, body, cat);
  if (added && na_available()) na_append(id, title, body, cat);
  if (added) {
    notif_store_save();
    /* Stash newest for the popup card, same as the Gadgetbridge/HTTP paths. */
    s_pop_id = id;
    strncpy(s_pop_title, title, sizeof(s_pop_title) - 1);
    s_pop_title[sizeof(s_pop_title) - 1] = '\0';
    strncpy(s_pop_body, body, sizeof(s_pop_body) - 1);
    s_pop_body[sizeof(s_pop_body) - 1] = '\0';
    s_pop_have = true;
    s_ancs_ui_dirty = true;       /* loop: pop the card + refresh the bell */
    s_ancs_added_total++;         /* timer-wake background check counter */
  }
  store_unlock();

  if (added)
    USBSerial.printf("[ancs] stored fp=%08lX app=\"%s\" date=\"%s\" title=\"%s\"\n",
                     (unsigned long)fp, app, date, title);
  else
    USBSerial.printf("[ancs] dup (already stored) fp=%08lX app=\"%s\" title=\"%s\"\n",
                     (unsigned long)fp, app, title);
  s_ds_len = 0;                    /* ready for the next reply */
  ancs_req_complete();             /* reply done -> send the next queued request */
}

/* Data Source notify RX: append to the reassembly buffer, then try to parse. */
static void ancs_data_source_rx(const uint8_t *data, uint16_t len) {
  if (len == 0) return;
  if (s_ds_len + len > ANCS_DS_BUF_MAX) {                  /* overflow -> drop reply, */
    s_ds_len = 0;                                          /* free the slot */
    ancs_req_complete();
    return;
  }
  memcpy(&s_ds_buf[s_ds_len], data, len);
  s_ds_len += len;
  ancs_parse_and_store();          /* no-op until a full record is present */
}

/* ===================== Control Point request ============================== */

/* Ask iOS for AppIdentifier + Title + Message + Date of a given UID.
 * Layout: [CmdID=0][UID:4 LE][AttrID][maxlen:2 LE]... (AppID/Date: no maxlen).
 * Date is a FIXED 16-char string - the backbone of the dedup fingerprint. */
static bool ancs_request_attrs(uint32_t uid, uint8_t ancs_cat) {
  if (!s_ancs_cp_val) return false;
  s_inflight_cat = ancs_cat;       /* remembered for ancs_parse_and_store */
  uint8_t req[32];
  uint16_t n = 0;
  req[n++] = ANCS_CMD_GET_NOTIF_ATTRS;
  req[n++] = (uint8_t)(uid);
  req[n++] = (uint8_t)(uid >> 8);
  req[n++] = (uint8_t)(uid >> 16);
  req[n++] = (uint8_t)(uid >> 24);
  req[n++] = ANCS_ATTR_APP_ID;                                  /* no length for app id */
  req[n++] = ANCS_ATTR_TITLE;
  req[n++] = (uint8_t)(ANCS_ATTR_TITLE_MAXLEN);
  req[n++] = (uint8_t)(ANCS_ATTR_TITLE_MAXLEN >> 8);
  req[n++] = ANCS_ATTR_MESSAGE;
  req[n++] = (uint8_t)(ANCS_ATTR_MSG_MAXLEN);
  req[n++] = (uint8_t)(ANCS_ATTR_MSG_MAXLEN >> 8);
  req[n++] = ANCS_ATTR_DATE;                                    /* no length (fixed 16) */

  s_ds_len = 0;                    /* fresh reassembly for this reply */
  int rc = ble_gattc_write_flat(s_ancs_conn, s_ancs_cp_val, req, n, NULL, NULL);
  if (rc != 0) USBSerial.printf("[ancs] control-point write rc=%d (uid=%lu)\n", rc, (unsigned long)uid);
  return rc == 0;
}

/* Send the next queued request, strictly one in flight. PEEK-then-pop: a failed
 * CP write (e.g. the GATT pipe briefly held by the peer-name read) used to LOSE
 * the item — now it stays queued and the next pump (reply complete, next NS event,
 * or the name-read's terminal callback) retries it. */
static void ancs_req_pump(void) {
  while (!s_req_inflight && s_reqq_count && s_ancs_cp_val) {
    uint32_t uid = s_reqq_uid[s_reqq_head];
    uint8_t  cat = s_reqq_cat[s_reqq_head];
    if (!ancs_request_attrs(uid, cat)) break;   // pipe busy: keep the item, retry later
    s_reqq_head  = (s_reqq_head + 1) % ANCS_REQQ_MAX;
    s_reqq_count--;
    s_req_inflight   = true;
    s_req_started_ms = millis();
  }
}

/* In-flight reply fully consumed (or unusable): free the slot, send the next. */
static void ancs_req_complete(void) {
  s_req_inflight = false;
  ancs_req_pump();
}

/* Queue an ADDED/MODIFIED attribute fetch. The stale-reply watchdog stops a lost
 * reply from wedging the queue forever. */
static void ancs_req_enqueue(uint32_t uid, uint8_t cat) {
  if (s_req_inflight && millis() - s_req_started_ms > 3000) s_req_inflight = false;
  if (s_reqq_count >= ANCS_REQQ_MAX) {
    USBSerial.printf("[ancs] request queue full - dropping uid=%lu\n", (unsigned long)uid);
    return;
  }
  uint8_t tail = (s_reqq_head + s_reqq_count) % ANCS_REQQ_MAX;
  s_reqq_uid[tail] = uid;
  s_reqq_cat[tail] = cat;
  s_reqq_count++;
  ancs_req_pump();
}

/* ===================== Notification Source RX ============================= */

/* Remove an ANCS notification (by UID) from BOTH stores so the watch mirrors the
 * phone. Runs on the NimBLE task -> guards the store, flags the loop. */
static void ancs_remove_uid(uint32_t uid) {
  uint64_t id;
  if (!ancs_uidmap_get(uid, &id)) return;   /* never stored this session */
  store_lock();
  bool hit = false;
  if (na_available() && na_remove(id)) hit = true;     /* archive (CSV rewrite) */
  if (notif_store_remove_by_id(id))    hit = true;     /* NVS newest-32 cache */
  store_unlock();
  if (hit) {
    s_ancs_removed = true;          /* loop refreshes the bell + any open list */
    USBSerial.printf("[ancs] removed uid=%lu (cleared on phone)\n", (unsigned long)uid);
  }
}

/* Notification Source RX: 8-byte event [EventID][Flags][CategoryID][Count][UID:4].
 *   ADDED    -> fetch attributes; INCOMING_CALL becomes transient live state.
 *   REMOVED  -> cleared on the phone: drop from the watch too; clear live call.
 *   MODIFIED -> treat like ADDED (re-fetch); notif_store_add de-dups by id. */
static void ancs_notif_source_rx(const uint8_t *data, uint16_t len) {
  if (len < 8) return;
  uint8_t  event_id = data[0];
  uint8_t  category = data[2];
  uint32_t uid = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                 ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

  if (event_id == ANCS_EVENT_REMOVED) {
    if (s_incoming_active && uid == s_incoming_uid) {
      s_incoming_active = false; s_incoming_uid = 0; s_incoming_dirty = true;
      USBSerial.println("[ancs] incoming call ended (removed)");
    }
    ancs_remove_uid(uid);
    return;
  }

  /* ADDED or MODIFIED: queue the attribute fetch (serialized via ancs_req_pump). */
  USBSerial.printf("[ancs] NS event=%u cat=%u uid=%lu -> fetch attrs\n",
                   (unsigned)event_id, (unsigned)category, (unsigned long)uid);
  ancs_req_enqueue(uid, category);
}

/* ===================== subscribe (write CCCDs) ============================ */

/* Forward decls: ancs_ds_sub_done chains to the NS write, both defined below. */
static void ancs_write_cccd(uint16_t cccd_handle,
                            int (*cb)(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *));
static int  ancs_ns_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                             struct ble_gatt_attr *attr, void *arg);

/* A CCCD write bounced. On a NOT-YET-ENCRYPTED link this is the EXPECTED iOS
 * behaviour: ANCS is gated behind an authenticated bond, so the write fails with
 * Insufficient Authentication (ATT 0x05) / Insufficient Encryption (0x0F). THAT
 * is the moment iOS accepts a pairing request - an unsolicited Security Request
 * at connect time is ignored on an unbonded iPhone. So: re-initiate security
 * once; after ENC_CHANGE fires, ancs_on_encrypted() re-subscribes. */
static void ancs_sub_failed(uint16_t conn, int status, const char *which) {
  if (s_ancs_pair_kicked) {
    USBSerial.printf("[ancs] %s CCCD write status=%d (pairing already requested)\n", which, status);
    return;
  }
  s_ancs_pair_kicked = true;
  s_ancs_pair_kick_ms = millis();          /* arm the pair watchdog below */
  s_ancs_pair_tries   = 0;
  USBSerial.printf("[ancs] %s CCCD write status=%d - requesting pairing (iOS gates ANCS behind a bond)\n",
                   which, status);
  int rc = ble_gap_security_initiate(conn);
  // rc=2 (BLE_HS_EALREADY) just means an SM procedure is already running for this
  // link (e.g. the peer started pairing on its own) - the pairing IS coming, so
  // it is not an error. Anything else non-zero is a real failure.
  if (rc == 0)                    USBSerial.println("[ancs] security_initiate ok - expect the passkey dialog");
  else if (rc == BLE_HS_EALREADY) USBSerial.println("[ancs] security already in progress (ok)");
  else                            USBSerial.printf ("[ancs] security_initiate rc=%d (FAILED)\n", rc);
}

/* Pair watchdog — call periodically from the loop thread (ble_ui_tick). The
 * Security Request above goes out on the air (rc=0 == TX'd), but the iPhone answers
 * with a Pairing Request only if it does NOT already hold a bond for this watch. If
 * the phone still lists the watch in Settings > Bluetooth from an EARLIER pairing
 * (reflash / bond-store wipe on our side), it silently ignores the request — no
 * pairing dialog on the phone, no PASSKEY_ACTION here, no code on our screen, and
 * the link just sits unencrypted forever. The watch cannot delete a bond stored on
 * the phone, so: retry once after NimBLE's ~30s SM-procedure timeout has cleared
 * the first attempt, then print exactly what the user must do. Disarmed by
 * ancs_on_encrypted() (pairing worked) or ancs_reset() (disconnect). */
static void ancs_pair_watchdog(void) {
  if (!s_ancs_pair_kicked || s_ancs_pair_kick_ms == 0 || s_ancs_pair_tries >= 2) return;
  if (s_ancs_conn == BLE_HS_CONN_HANDLE_NONE) return;
  if (millis() - s_ancs_pair_kick_ms < 35000) return;   /* past the 30s SM proc timeout */
  if (s_ancs_pair_tries == 0) {
    s_ancs_pair_tries = 1;
    s_ancs_pair_kick_ms = millis();
    int rc = ble_gap_security_initiate(s_ancs_conn);
    USBSerial.printf("[ancs] no pairing response in 35s - security re-initiate rc=%d\n", rc);
  } else {
    s_ancs_pair_tries = 2;                                /* final: report, stop nagging */
    /* With the bond store wired in (ble_store_kv_tuya.h) pairing should complete in
     * seconds; landing here means something new - the OWF-SMDBG probe lines in the
     * instrumented libtuyaos.a (tx/rx op + sm_err/app verdicts) name the exact spot. */
    USBSerial.println("[ancs] pairing still not completing after 2 attempts - check the");
    USBSerial.println("[ancs] OWF-SMDBG lines above for the SM opcode/verdict that failed.");
  }
}

static int ancs_ds_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg) {
  (void)attr; (void)arg;
  if (err && err->status != 0) { ancs_sub_failed(conn, err->status, "DS"); return 0; }
  s_ancs_ds_subbed = true;
  USBSerial.println("[ancs] subscribed to Data Source");
  /* DS is in; now (and only now, one proc at a time) subscribe Notification
   * Source, whose subscribe triggers iOS's backlog burst. */
  if (s_ancs_ns_cccd) ancs_write_cccd(s_ancs_ns_cccd, ancs_ns_sub_done);
  else USBSerial.println("[ancs] Notification Source CCCD not found");
  return 0;
}

static int ancs_ns_sub_done(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg) {
  (void)attr; (void)arg;
  if (err && err->status != 0) { ancs_sub_failed(conn, err->status, "NS"); return 0; }
  s_ancs_ns_subbed = true;
  USBSerial.println("[ancs] subscribed to Notification Source - backlog + live events will flow");
  /* ANCS discovery chain fully done. NimBLE allows only ONE GATT procedure in
   * flight per connection, so AMS discovery kicks off HERE (serialized), not
   * concurrently from the ENC_CHANGE event. No-op until AMS is ported. */
  ams_on_encrypted(conn);
  /* Peer-name read LAST — after everything notification-critical. It shares the
   * single GATT pipe with the backlog's CP writes; the hardened pump keeps a
   * bounced write queued and the read's terminal cb re-pumps. (When AMS is ported
   * as a real GATT chain, move this to the end of AMS's chain instead.) */
  ble_peer_name_fetch(conn);
  return 0;
}
static void ancs_write_cccd(uint16_t cccd_handle,
                            int (*cb)(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *)) {
  static const uint8_t en[2] = { 0x01, 0x00 };
  int rc = ble_gattc_write_flat(s_ancs_conn, cccd_handle, en, sizeof(en), cb, NULL);
  if (rc != 0) USBSerial.printf("[ancs] write_flat(cccd 0x%04X) rc=%d\n", cccd_handle, rc);
}

/* ===================== descriptor discovery (find CCCDs) ==================
 * NimBLE quirk: ble_gattc_disc_all_dscs echoes the `start` handle you pass back to
 * the callback as `chr_val_handle` - it is NOT "the characteristic this descriptor
 * belongs to". So disc_all_dscs runs ONCE PER CHARACTERISTIC, bounded past that
 * characteristic's own value handle. Chain: NS's descriptors, then DS's, then
 * subscribe both. */

/* Step 2b: Data Source descriptors -> its CCCD -> subscribe both channels. */
static int ancs_dsc_ds_cb(uint16_t conn, const struct ble_gatt_error *err,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  (void)conn; (void)chr_val_handle; (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[ancs] DS dsc disc error status=%d\n", err->status);
    return 0;
  }
  if (dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16)
    s_ancs_ds_cccd = dsc->handle;
  if (err && err->status == BLE_HS_EDONE) {
    USBSerial.printf("[ancs] cccds: NS=0x%04X DS=0x%04X\n", s_ancs_ns_cccd, s_ancs_ds_cccd);
    /* Subscribe Data Source ONLY here. NimBLE allows exactly ONE GATT procedure
     * in flight per connection, so we CANNOT fire the NS write in the same tick -
     * it would bounce (ENOMEM) and, pre-bond, race the security request. NS is
     * chained strictly after DS's write completes (ancs_ds_sub_done). Pre-bond,
     * this single DS write is what bounces on auth and triggers pairing. */
    if (s_ancs_ds_cccd) ancs_write_cccd(s_ancs_ds_cccd, ancs_ds_sub_done);
    else USBSerial.println("[ancs] Data Source CCCD not found");
  }
  return 0;
}

/* Step 2a: Notification Source descriptors -> its CCCD, then chain to DS. */
static int ancs_dsc_ns_cb(uint16_t conn, const struct ble_gatt_error *err,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  (void)chr_val_handle; (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[ancs] NS dsc disc error status=%d\n", err->status);
    return 0;
  }
  if (dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16)
    s_ancs_ns_cccd = dsc->handle;
  if (err && err->status == BLE_HS_EDONE) {
    /* Discover the Data Source's descriptors. Bound tightly past DS's value handle
     * so ITS CCCD is captured without bleeding into a neighbour. */
    uint16_t ds_end = s_ancs_ds_val + 2;
    if (ds_end > s_ancs_svc_end) ds_end = s_ancs_svc_end;
    ble_gattc_disc_all_dscs(conn, s_ancs_ds_val, ds_end, ancs_dsc_ds_cb, NULL);
  }
  return 0;
}

/* ===================== characteristic discovery ========================== */

static int ancs_chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg) {
  (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[ancs] chr disc error status=%d\n", err->status);
    return 0;
  }
  if (chr) {
    if      (ble_uuid_cmp(&chr->uuid.u, &ANCS_NOTIF_SRC_UUID.u) == 0) s_ancs_ns_val = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &ANCS_CTRL_PT_UUID.u)  == 0) s_ancs_cp_val = chr->val_handle;
    else if (ble_uuid_cmp(&chr->uuid.u, &ANCS_DATA_SRC_UUID.u) == 0) s_ancs_ds_val = chr->val_handle;
  }
  if (err && err->status == BLE_HS_EDONE) {
    USBSerial.printf("[ancs] chars: NS=0x%04X CP=0x%04X DS=0x%04X\n",
                     s_ancs_ns_val, s_ancs_cp_val, s_ancs_ds_val);
    if (s_ancs_ns_val && s_ancs_cp_val && s_ancs_ds_val) {
      /* NS's descriptors first (tight bound so we get ITS CCCD); the chain then
       * does DS, then subscribes both. */
      uint16_t ns_end = s_ancs_ns_val + 2;
      if (ns_end > s_ancs_svc_end) ns_end = s_ancs_svc_end;
      ble_gattc_disc_all_dscs(conn, s_ancs_ns_val, ns_end, ancs_dsc_ns_cb, NULL);
    } else {
      USBSerial.println("[ancs] missing a required ANCS characteristic");
    }
  }
  return 0;
}

/* ===================== service discovery ================================= */

static int ancs_svc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg) {
  (void)arg;
  if (err && err->status != 0 && err->status != BLE_HS_EDONE) {
    USBSerial.printf("[ancs] svc disc error status=%d\n", err->status);
    return 0;
  }
  if (svc) { s_ancs_svc_start = svc->start_handle; s_ancs_svc_end = svc->end_handle; }
  if (err && err->status == BLE_HS_EDONE) {
    if (s_ancs_svc_start) {
      USBSerial.printf("[ancs] ANCS service [0x%04X..0x%04X] - discovering chars\n",
                       s_ancs_svc_start, s_ancs_svc_end);
      ble_gattc_disc_all_chrs(conn, s_ancs_svc_start, s_ancs_svc_end, ancs_chr_cb, NULL);
    } else {
      USBSerial.println("[ancs] no ANCS service exposed (not iPhone, or not yet trusted)");
    }
  }
  return 0;
}

/* ===================== public entry points =============================== */

/* Called from owf_gap_event's CONNECT, BEFORE any pairing. iOS publishes the
 * ANCS service to unbonded peers, so discovery runs now; the SUBSCRIBE then
 * bounces with Insufficient Authentication, and that bounce (ancs_sub_failed)
 * is what makes an unbonded iPhone accept our pairing request. */
static void ancs_on_connect(uint16_t conn_handle) {
  ancs_reset();
  s_ancs_conn = conn_handle;
  USBSerial.printf("[ancs] connected (conn=0x%04X) - probing for ANCS pre-bond...\n", conn_handle);
  int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &ANCS_SVC_UUID.u, ancs_svc_cb, NULL);
  if (rc != 0) USBSerial.printf("[ancs] disc_svc_by_uuid rc=%d\n", rc);
}

/* Called from owf_gap_event's ENC_CHANGE once the link is ENCRYPTED. If the
 * pre-bond probe already discovered the handles (subscribe bounced on auth),
 * just re-write the CCCDs; otherwise run the full discover -> subscribe chain.
 * iOS replays the pending-notification backlog once NS is subscribed. */
static void ancs_on_encrypted(uint16_t conn_handle) {
  if (s_ancs_ns_subbed && conn_handle == s_ancs_conn) return;  /* already running */
  if (conn_handle == s_ancs_conn && s_ancs_ns_cccd && s_ancs_ds_cccd) {
    /* Handles already discovered pre-bond - just re-subscribe. DS only here; NS
     * chains after DS completes (ancs_ds_sub_done) - one GATT proc at a time. */
    USBSerial.println("[ancs] link encrypted - re-subscribing (handles cached)");
    s_ancs_pair_kicked = false;   /* fresh subscribe round on the now-encrypted link */
    s_ancs_pair_kick_ms = 0; s_ancs_pair_tries = 0;   /* disarm the pair watchdog */
    ancs_write_cccd(s_ancs_ds_cccd, ancs_ds_sub_done);
    return;
  }
  ancs_reset();
  s_ancs_conn = conn_handle;
  USBSerial.printf("[ancs] link encrypted (conn=0x%04X) - discovering ANCS...\n", conn_handle);
  int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &ANCS_SVC_UUID.u, ancs_svc_cb, NULL);
  if (rc != 0) USBSerial.printf("[ancs] disc_svc_by_uuid rc=%d\n", rc);
}

/* ---- live incoming-call accessors (LVGL/loop thread) ----
 * Replace the inert Phase-A versions in ble_notif_flags_tuya.h (suppressed by
 * BLE_TUYA_HAVE_ANCS). Same contract as the ESP32 build. */
static bool ancs_take_incoming_dirty(void) {
  if (!s_incoming_dirty) return false;
  s_incoming_dirty = false;
  return true;
}
static bool        ancs_incoming_active(void) { return s_incoming_active; }
static const char *ancs_incoming_who(void)    { return s_incoming_who; }

/* ===================== dismiss FROM the watch (watch -> phone) ============
 * "Perform Notification Action / Negative" on the Control Point so iOS clears the
 * notification on the phone too. BEST-EFFORT: needs a connected phone, wired ANCS,
 * and a live UID from THIS session; otherwise silently does nothing (the caller's
 * local store removal still happens). Runs on the loop thread; ble_gattc_write_flat
 * is safe to call from there. */
static int ancs_dismiss_done(uint16_t conn, const struct ble_gatt_error *err,
                             struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (err && err->status != 0)
    USBSerial.printf("[ancs] dismiss write FAILED att_status=%d\n", err->status);
  else
    USBSerial.println("[ancs] dismiss write ACKed by phone");
  return 0;
}

static void ancs_dismiss_id(uint64_t id) {
  if (!s_ble_connected) { USBSerial.println("[ancs] dismiss skipped: not connected"); return; }
  if (!s_ancs_cp_val)   { USBSerial.println("[ancs] dismiss skipped: ANCS Control Point not ready"); return; }
  uint32_t uid;
  if (!ancs_uidmap_get_uid(id, &uid)) {
    USBSerial.printf("[ancs] dismiss skipped: no live UID for id=%08lX (not seen this session). "
                     "uidmap has %u entries\n",
                     (unsigned long)(uint32_t)id, s_uidmap_count);
    return;
  }

  uint8_t req[6];                                      /* cmd(1) + uid(4) + action(1) */
  req[0] = ANCS_CMD_PERFORM_ACTION;
  req[1] = (uint8_t)(uid);
  req[2] = (uint8_t)(uid >> 8);
  req[3] = (uint8_t)(uid >> 16);
  req[4] = (uint8_t)(uid >> 24);
  req[5] = ANCS_ACTION_NEGATIVE;                       /* dismiss */
  USBSerial.printf("[ancs] sending dismiss for uid=%lu (cp_handle=0x%04X)\n",
                   (unsigned long)uid, s_ancs_cp_val);
  int rc = ble_gattc_write_flat(s_ancs_conn, s_ancs_cp_val, req, sizeof(req),
                                ancs_dismiss_done, NULL);
  if (rc != 0) USBSerial.printf("[ancs] dismiss write kickoff rc=%d (uid=%lu)\n", rc, (unsigned long)uid);
}

/* Clear-all from the watch: dismiss every notification with a known live UID. */
static void ancs_dismiss_all(void) {
  if (!s_ble_connected || !s_ancs_cp_val) return;
  for (uint8_t i = 0; i < s_uidmap_count; i++) {
    uint8_t req[6];
    uint32_t uid = s_uidmap_uid[i];
    req[0] = ANCS_CMD_PERFORM_ACTION;
    req[1] = (uint8_t)(uid);
    req[2] = (uint8_t)(uid >> 8);
    req[3] = (uint8_t)(uid >> 16);
    req[4] = (uint8_t)(uid >> 24);
    req[5] = ANCS_ACTION_NEGATIVE;
    ble_gattc_write_flat(s_ancs_conn, s_ancs_cp_val, req, 6, NULL, NULL);
  }
  if (s_uidmap_count)
    USBSerial.printf("[ancs] clear-all: sent dismiss for %u notifications\n", s_uidmap_count);
}

/* TIMER-WAKE background check (called from the sleep code's light check, no loop
 * running). Bring up BLE if needed, wait up to budget_ms for the iPhone to
 * reconnect + ANCS to subscribe + the backlog to drain into the store. Returns
 * true if anything new arrived (caller full-boots to show it). */
static bool ancs_background_check(uint32_t budget_ms) {
  uint32_t before = s_ancs_added_total;

  bool we_started_ble = false;
  if (!ble_is_up()) { ble_begin(); we_started_ble = true; }   /* iOS reconnects to adv */

  uint32_t t0 = millis();
  bool got = false;
  while (millis() - t0 < budget_ms) {
    if (s_ancs_added_total != before) { got = true; break; }  /* something landed */
    delay(50);                                                 /* let NimBLE run */
  }

  /* Grace window so a multi-item backlog finishes draining before the boot. */
  if (got) {
    uint32_t grace0 = millis();
    uint32_t last = s_ancs_added_total;
    while (millis() - grace0 < 1500) {
      delay(100);
      if (s_ancs_added_total != last) { last = s_ancs_added_total; grace0 = millis(); }
    }
  }

  /* Nothing new: tear down BLE only if WE brought it up. */
  if (!got && we_started_ble) ble_end();
  return got;
}

/* ===================== notify-RX routing (from owf_gap_event) ============= */

/* Called by ble_tuya.h's GAP handler for every inbound notification, already
 * flattened. Returns true if consumed (ANCS handles). AMS gets first refusal here
 * once ble_ams_tuya.h is ported. Runs on the NimBLE host task. */
static bool ble_tuya_route_notify_rx(uint16_t conn, uint16_t attr,
                                     const uint8_t *data, uint16_t len) {
  if (conn != s_ancs_conn) return false;
  if (attr == s_ancs_ns_val && s_ancs_ns_val) { ancs_notif_source_rx(data, len); return true; }
  if (attr == s_ancs_ds_val && s_ancs_ds_val) { ancs_data_source_rx(data, len); return true; }
  return false;
}

/* ===================== AMS stubs (Phase B.2) ==============================
 * ble_tuya.h forward-declares ams_on_encrypted/ams_reset when BLE_TUYA_HAVE_ANCS
 * is defined. AMS (iPhone media) is not ported yet; these no-ops satisfy the
 * linker. ble_ams_tuya.h will define BLE_TUYA_HAVE_AMS and the real versions. */
#ifndef BLE_TUYA_HAVE_AMS
static void ams_on_encrypted(uint16_t conn_handle) { (void)conn_handle; }
static void ams_reset(void) {}
#endif
