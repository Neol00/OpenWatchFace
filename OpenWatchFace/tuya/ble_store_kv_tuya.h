/* ============================================================================
 *  tuya/ble_store_kv_tuya.h - NimBLE bond store for the Tuya T5 port.
 *
 *  ROOT CAUSE OF THE SILENT PAIRING FAILURE (found via the OWF-SMDBG probes in
 *  the rebuilt libtuyaos.a): the iPhone answers our Security Request with a
 *  Pairing Request IMMEDIATELY, but ble_sm_pair_req_rx() rejects it -
 *  "OWF-SMDBG rx op=1 verdict sm_err=8 app=8" - because the pre-pairing
 *  bond-capacity check (ble_sm_chk_store_overflow -> ble_store_util_count)
 *  hits store_read_cb == NULL and returns BLE_HS_ENOTSUP (8). The host then
 *  TXes SMP Pairing Failed (op=5 reason=8) and iOS gives up without ever
 *  showing its pairing dialog. No GAP event fires, so it was invisible.
 *
 *  On the ESP32 builds NimBLE-Arduino installs its NVS bond store behind the
 *  scenes - that's the whole reason "it just works" there. The Tuya tree ships
 *  only the store FRAMEWORK (ble_store.c dispatches to ble_hs_cfg.store_*_cb)
 *  with no backend at all. This header is that backend:
 *
 *    - RAM tables mirroring upstream NimBLE's ble_store_ram.c:
 *        our_sec[3] / peer_sec[3]   (3 == TY_HS_BLE_STORE_MAX_BONDS in the lib)
 *        cccd[8]                    (8 == TY_HS_BLE_STORE_MAX_CCCDS)
 *    - persisted as ONE tal_kv blob ("owf_blesec") written on every store
 *      write/delete, loaded at ble_begin(); bonds survive reboot/power-off, so
 *      an already-paired iPhone re-encrypts on reconnect instead of re-pairing
 *      (ANCS then resubscribes with no user interaction - ESP32 parity).
 *    - store_status_cb = ble_store_util_status_rr (exported by libtuyaos.a):
 *      on capacity overflow the OLDEST bond is evicted round-robin, matching
 *      the ESP32 behaviour.
 *
 *  INCLUDE from ble_tuya.h (which owns the NimBLE host includes + tal_kv is
 *  already linked via owf_tuya_port.h). All callbacks run on the NimBLE host
 *  task under ble_hs_lock - they touch only these tables + tal_kv (own mutex,
 *  no BLE calls), so no deadlock. Blob layout is fw-private (raw structs incl.
 *  bitfields); a version byte guards against layout drift across flashes.
 * ========================================================================== */
#pragma once

#define OWF_STORE_MAX_SEC   3    /* MUST equal TY_HS_BLE_STORE_MAX_BONDS (lib build) */
#define OWF_STORE_MAX_CCCD  8    /* MUST equal TY_HS_BLE_STORE_MAX_CCCDS (lib build) */
#define OWF_STORE_KV_KEY    "owf_blesec"
#define OWF_STORE_BLOB_VER  1

static struct ble_store_value_sec  s_bs_our_sec [OWF_STORE_MAX_SEC];
static struct ble_store_value_sec  s_bs_peer_sec[OWF_STORE_MAX_SEC];
static struct ble_store_value_cccd s_bs_cccd    [OWF_STORE_MAX_CCCD];
static uint8_t s_bs_our_n = 0, s_bs_peer_n = 0, s_bs_cccd_n = 0;

/* ---- persistence ---------------------------------------------------------- */
typedef struct {
  uint8_t ver, our_n, peer_n, cccd_n;
  struct ble_store_value_sec  our_sec [OWF_STORE_MAX_SEC];
  struct ble_store_value_sec  peer_sec[OWF_STORE_MAX_SEC];
  struct ble_store_value_cccd cccd    [OWF_STORE_MAX_CCCD];
} owf_store_blob_t;

static void owf_store_save(void) {
  owf_store_blob_t b;
  memset(&b, 0, sizeof(b));
  b.ver = OWF_STORE_BLOB_VER;
  b.our_n = s_bs_our_n; b.peer_n = s_bs_peer_n; b.cccd_n = s_bs_cccd_n;
  memcpy(b.our_sec,  s_bs_our_sec,  sizeof(b.our_sec));
  memcpy(b.peer_sec, s_bs_peer_sec, sizeof(b.peer_sec));
  memcpy(b.cccd,     s_bs_cccd,     sizeof(b.cccd));
  if (tal_kv_set((const char *)OWF_STORE_KV_KEY, (uint8_t *)&b, sizeof(b)) != OPRT_OK)
    USBSerial.println("[blestore] kv save FAILED (bond won't survive reboot)");
}

static void owf_store_load(void) {
  uint8_t *val = NULL; size_t len = 0;
  s_bs_our_n = s_bs_peer_n = s_bs_cccd_n = 0;
  if (tal_kv_get((const char *)OWF_STORE_KV_KEY, &val, &len) != OPRT_OK || !val) return;
  if (len == sizeof(owf_store_blob_t)) {
    owf_store_blob_t *b = (owf_store_blob_t *)val;
    if (b->ver == OWF_STORE_BLOB_VER &&
        b->our_n <= OWF_STORE_MAX_SEC && b->peer_n <= OWF_STORE_MAX_SEC &&
        b->cccd_n <= OWF_STORE_MAX_CCCD) {
      s_bs_our_n = b->our_n; s_bs_peer_n = b->peer_n; s_bs_cccd_n = b->cccd_n;
      memcpy(s_bs_our_sec,  b->our_sec,  sizeof(s_bs_our_sec));
      memcpy(s_bs_peer_sec, b->peer_sec, sizeof(s_bs_peer_sec));
      memcpy(s_bs_cccd,     b->cccd,     sizeof(s_bs_cccd));
    }
  }
  tal_kv_free(val);
  USBSerial.printf("[blestore] loaded: %u bond(s), %u cccd(s)\n",
                   (unsigned)s_bs_peer_n, (unsigned)s_bs_cccd_n);
}

/* ---- lookups (mirroring upstream ble_store_ram.c semantics) --------------- */
static bool owf_addr_is_any(const ble_addr_t *a) {
  static const uint8_t z[6] = {0};
  return a->type == 0 && memcmp(a->val, z, 6) == 0;
}

static int owf_store_find_sec(const struct ble_store_key_sec *key,
                              const struct ble_store_value_sec *arr, int n) {
  int skip = key->idx;
  for (int i = 0; i < n; i++) {
    if (!owf_addr_is_any(&key->peer_addr) &&
        ble_addr_cmp(&arr[i].peer_addr, &key->peer_addr) != 0) continue;
    if (key->ediv_rand_present &&
        (arr[i].ediv != key->ediv || arr[i].rand_num != key->rand_num)) continue;
    if (skip > 0) { skip--; continue; }
    return i;
  }
  return -1;
}

static int owf_store_find_cccd(const struct ble_store_key_cccd *key) {
  int skip = key->idx;
  for (int i = 0; i < s_bs_cccd_n; i++) {
    if (!owf_addr_is_any(&key->peer_addr) &&
        ble_addr_cmp(&s_bs_cccd[i].peer_addr, &key->peer_addr) != 0) continue;
    if (key->chr_val_handle != 0 &&
        s_bs_cccd[i].chr_val_handle != key->chr_val_handle) continue;
    if (skip > 0) { skip--; continue; }
    return i;
  }
  return -1;
}

/* ---- the three callbacks --------------------------------------------------- */
static int owf_store_read_cb(int obj_type, const union ble_store_key *key,
                             union ble_store_value *value) {
  int i;
  switch (obj_type) {
  case BLE_STORE_OBJ_TYPE_OUR_SEC:
    i = owf_store_find_sec(&key->sec, s_bs_our_sec, s_bs_our_n);
    if (i < 0) return BLE_HS_ENOENT;
    value->sec = s_bs_our_sec[i];
    return 0;
  case BLE_STORE_OBJ_TYPE_PEER_SEC:
    i = owf_store_find_sec(&key->sec, s_bs_peer_sec, s_bs_peer_n);
    if (i < 0) return BLE_HS_ENOENT;
    value->sec = s_bs_peer_sec[i];
    return 0;
  case BLE_STORE_OBJ_TYPE_CCCD:
    i = owf_store_find_cccd(&key->cccd);
    if (i < 0) return BLE_HS_ENOENT;
    value->cccd = s_bs_cccd[i];
    return 0;
  default:
    return BLE_HS_ENOTSUP;
  }
}

static int owf_store_write_sec(const struct ble_store_value_sec *v,
                               struct ble_store_value_sec *arr, uint8_t *n) {
  struct ble_store_key_sec key;
  memset(&key, 0, sizeof(key));
  key.peer_addr = v->peer_addr;               /* replace an existing bond for this peer */
  int i = owf_store_find_sec(&key, arr, *n);
  if (i < 0) {
    if (*n >= OWF_STORE_MAX_SEC) return BLE_HS_ESTORE_CAP;   /* -> overflow evt -> rr evict -> retry */
    i = (*n)++;
  }
  arr[i] = *v;
  return 0;
}

static int owf_store_write_cb(int obj_type, const union ble_store_value *value) {
  int rc;
  switch (obj_type) {
  case BLE_STORE_OBJ_TYPE_OUR_SEC:
    rc = owf_store_write_sec(&value->sec, s_bs_our_sec, &s_bs_our_n);
    break;
  case BLE_STORE_OBJ_TYPE_PEER_SEC:
    rc = owf_store_write_sec(&value->sec, s_bs_peer_sec, &s_bs_peer_n);
    break;
  case BLE_STORE_OBJ_TYPE_CCCD: {
    struct ble_store_key_cccd key;
    memset(&key, 0, sizeof(key));
    key.peer_addr = value->cccd.peer_addr;
    key.chr_val_handle = value->cccd.chr_val_handle;
    int i = owf_store_find_cccd(&key);
    if (i < 0) {
      if (s_bs_cccd_n >= OWF_STORE_MAX_CCCD) return BLE_HS_ESTORE_CAP;
      i = s_bs_cccd_n++;
    }
    s_bs_cccd[i] = value->cccd;
    rc = 0;
    break;
  }
  default:
    return BLE_HS_ENOTSUP;
  }
  if (rc == 0) owf_store_save();
  return rc;
}

static int owf_store_delete_cb(int obj_type, const union ble_store_key *key) {
  int i;
  switch (obj_type) {
  case BLE_STORE_OBJ_TYPE_OUR_SEC:
    i = owf_store_find_sec(&key->sec, s_bs_our_sec, s_bs_our_n);
    if (i < 0) return BLE_HS_ENOENT;
    memmove(&s_bs_our_sec[i], &s_bs_our_sec[i + 1],
            (--s_bs_our_n - i) * sizeof(s_bs_our_sec[0]));
    break;
  case BLE_STORE_OBJ_TYPE_PEER_SEC:
    i = owf_store_find_sec(&key->sec, s_bs_peer_sec, s_bs_peer_n);
    if (i < 0) return BLE_HS_ENOENT;
    memmove(&s_bs_peer_sec[i], &s_bs_peer_sec[i + 1],
            (--s_bs_peer_n - i) * sizeof(s_bs_peer_sec[0]));
    break;
  case BLE_STORE_OBJ_TYPE_CCCD:
    i = owf_store_find_cccd(&key->cccd);
    if (i < 0) return BLE_HS_ENOENT;
    memmove(&s_bs_cccd[i], &s_bs_cccd[i + 1],
            (--s_bs_cccd_n - i) * sizeof(s_bs_cccd[0]));
    break;
  default:
    return BLE_HS_ENOTSUP;
  }
  owf_store_save();
  return 0;
}

/* Wire the callbacks + load persisted bonds. Call from ble_begin() right after
 * the sm_* config block (before advertising; the config struct is read at
 * pairing time so ordering vs stack init doesn't matter, but the tables must
 * be loaded before a bonded peer reconnects). */
static void owf_ble_store_init(void) {
  ble_hs_cfg.store_read_cb   = owf_store_read_cb;
  ble_hs_cfg.store_write_cb  = owf_store_write_cb;
  ble_hs_cfg.store_delete_cb = owf_store_delete_cb;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;   /* evict oldest on overflow */
  owf_store_load();
}
