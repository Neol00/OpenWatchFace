/* ============================================================================
 *  ble_tuya.h - Tuya T5-E1 BLE peripheral (Phase A) on the RAW NimBLE host.
 *
 *  The Tuya T5 ships Apache NimBLE underneath its tkl_ble / tal_ble wrapper,
 *  but that wrapper is too small for this firmware: its GATT server caps at
 *  2 services / 4 chars, it has NO Security Manager, it drops inbound
 *  notifications, and it stubs out GATT-client writes. So - exactly like the
 *  ESP32 build, which talks to NimBLE under Arduino's BLEDevice - we drive the
 *  RAW NimBLE host directly (ble_gap_*, ble_gatts_*, ble_gattc_*, ble_store_*).
 *  The rebuilt libtuyaos.a exports all of these as real symbols.
 *
 *  This file is the Tuya counterpart of ble_provision.h. It is included (instead
 *  of ble_provision.h) for the Tuya target. It owns:
 *    - host bring-up via the Tuya stack init (tkl_ble_stack_init) + our own
 *      GAP event handler and GATT service table registered on the raw host,
 *    - advertising (so the watch is discoverable / pairable in the UI),
 *    - one custom GATT service (WiFi-provision + find-watch + find-phone) plus
 *      the Nordic UART Service for Gadgetbridge (defined in ble_gadgetbridge_tuya.h)
 *      and a standard Battery Service,
 *    - the security config (request bonding + an encrypted link; see the SM note),
 *    - bonded-device listing / forget via ble_store_*,
 *    - the same public API surface the rest of the .ino calls (ble_begin/end,
 *      ble_apply_enabled, ble_is_up, ble_phone_connected, ble_ping_phone,
 *      ble_report_battery, ble_bond_*, ble_ui_tick, the pair-code overlay flags).
 *
 *  SECURITY / BONDING NOTE (Phase B): this build runs against the SM-ENABLED
 *  libtuyaos.a rebuild (TY_HS_BLE_SM_SC/LEGACY/BONDING/MITM=1, key dist ENC+ID,
 *  vendored mesh_crypt TinyCrypt sources; rebuilt from the WSL ~/TuyaOpen tree -
 *  see tuya-t5e1-ble-stack-facts in project memory). IO cap is DisplayOnly: on
 *  pairing the watch shows a random 6-digit code (PASSKEY_ACTION below +
 *  ble_ui_tick overlay) that the phone types, giving the authenticated bonded
 *  link iOS requires before exposing ANCS. The package headers must match the
 *  lib's SM config - edit 4 in patches/apply_tuya_package_patches.sh keeps the
 *  installed tuya_ble_cfg.h in sync after a package reinstall.
 *
 *  THREADING: every NimBLE callback here runs on the NimBLE host task, NOT the
 *  LVGL/loop thread (identical to the ESP32 build). Callbacks never touch LVGL -
 *  they take store_lock for the WiFi/notif stores, or set volatile flags that the
 *  loop renders via ble_ui_tick(). Mirrors ble_provision.h's threading contract.
 *
 *  INCLUDE ORDER (in the .ino, Tuya branch): after device_info.h (deviceRadioName),
 *  settings_store.h (settings_get_ble_enabled, prefs), wifi_store.h, watch_base.h
 *  (store_lock), and the FONT_* macros. ble_gadgetbridge_tuya.h is included right
 *  after this file (it provides gb_* used by the NUS characteristic) and the ANCS
 *  client (ble_ancs_tuya.h, Phase B) later, after the notif store - same staging
 *  as the ESP32 build. Header-only, compiled into the single .ino TU.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>

/* Raw NimBLE host - shipped on the SDK's public include path. C API; this header
 * is compiled as C++ in the .ino TU, so wrap in extern "C". */
extern "C" {
#include "ble_hs.h"             // pulls the whole host: ble_gap.h, ble_gatt.h, ble_hs_adv.h,
                                // ble_sm.h, ble_store.h, ble_uuid.h, ble_hs_mbuf.h, the os_mbuf
                                // layer (via tuya_hs_port.h) and tuya_ble.h (ble_addr_cmp,
                                // BLE_ERR_*). Declares ble_hs_cfg as `tuya_ble_hs_cfg`.
#include "ble_svc_gap.h"        // ble_svc_gap_device_name / _set
#include "ble_svc_gatt.h"       // ble_svc_gatt_changed (GATT-table-changed indication, Phase B)

/* tkl_ble_stack_init() does the host bring-up (HCI init + run + sync wait). It is
 * exported by libtuyaos.a, but its header (tkl_bluetooth.h) is NOT on the Arduino
 * sketch include path (only src/tal_bluetooth/{nimble/,}include are) - so we
 * forward-declare the one prototype we call and let the linker resolve it. The two
 * role bits are stable ABI (server=1, client=2). */
OPERATE_RET tkl_ble_stack_init(uint8_t role);

/* Random source for the 6-digit pair code (exported by libtuyaos.a; its header
 * tal_system.h is not needed for one prototype). Returns [0, range). */
int tal_system_get_random(uint32_t range);
}
#ifndef TKL_BLE_ROLE_SERVER
#define TKL_BLE_ROLE_SERVER (0x01)   /* GATT server / peripheral */
#endif
#ifndef TKL_BLE_ROLE_CLIENT
#define TKL_BLE_ROLE_CLIENT (0x02)   /* GATT client / central (Phase B ANCS) */
#endif

/* ble_hs_cfg is exported under the Tuya name. Alias so the code reads like stock
 * NimBLE. (Declared in ble_hs.h as `extern struct ble_hs_cfg tuya_ble_hs_cfg;`.) */
#define ble_hs_cfg tuya_ble_hs_cfg

/* Bond store (THE ANCS pairing fix): the Tuya tree ships NO store backend, so the
 * host's pre-pairing capacity check failed with ENOTSUP and every inbound Pairing
 * Request was answered with SMP Pairing Failed before iOS could show its dialog.
 * This provides the RAM+tal_kv backend and owf_ble_store_init() wires it up. */
#include "ble_store_kv_tuya.h"

/* ---- forward declares for the sibling BLE units (defined later in the TU) ----
 * Gadgetbridge NUS server (ble_gadgetbridge_tuya.h) and the Phase-B ANCS/AMS
 * clients. Same forward-declare pattern ble_provision.h uses, so our GATT write
 * callback + GAP handler can route to them across the single .ino TU. */
static void gb_rx_bytes(const uint8_t *data, uint16_t len);   // NUS RX (phone->watch)
static void gb_reset(void);
static void gb_set_tx_handle(uint16_t conn, uint16_t val_handle);   // NUS TX target
static bool gb_send(const char *json);

/* Phase B (ANCS/AMS) hooks - no-ops in Phase A (the headers aren't included yet),
 * but the GAP handler calls them so Phase B slots in with zero handler changes.
 * Weakly provided here as empty inlines; ble_ancs_tuya.h (when added) defines the
 * real ones FIRST and #defines BLE_TUYA_HAVE_ANCS to suppress these. */
#ifndef BLE_TUYA_HAVE_ANCS
static inline void ancs_on_connect(uint16_t) {}
static inline void ancs_on_encrypted(uint16_t) {}
static inline void ams_on_encrypted(uint16_t) {}
static inline void ancs_reset(void) {}
static inline void ams_reset(void) {}
static inline void ancs_req_pump(void) {}
static inline bool ble_tuya_route_notify_rx(uint16_t, uint16_t, const uint8_t *, uint16_t) { return false; }
#else
static void ancs_on_connect(uint16_t conn_handle);
static void ancs_on_encrypted(uint16_t conn_handle);
static void ams_on_encrypted(uint16_t conn_handle);
static void ancs_reset(void);
static void ams_reset(void);
static void ancs_req_pump(void);   // re-kick the CP attribute queue (name-read terminal cb)
static bool ble_tuya_route_notify_rx(uint16_t conn, uint16_t attr, const uint8_t *data, uint16_t len);
#endif

/* The SDK's BLE_UUID128_INIT / BLE_UUID16_INIT macros use C designated
 * initializers (.u.type = ...), which this Arduino C++ dialect rejects
 * ("expected primary-expression before '.'"). The structs are simple aggregates
 * — ble_uuid128_t = { ble_uuid_t u; uint8_t value[16]; } — so initialise them
 * positionally instead. These build clean in C++ and produce identical bytes. */
#define OWF_UUID128(...) { { BLE_UUID_TYPE_128 }, { __VA_ARGS__ } }
#define OWF_UUID16(v)    { { BLE_UUID_TYPE_16  }, (v) }

/* ---- Custom 128-bit UUIDs (identical to the ESP32 build so the same phone app
 * works on both). NimBLE stores 128-bit UUIDs LSB-first. ------------------- */
/* Service 6b1f0001-9a3e-4c7a-9b2d-2f1a8c5e7d10 and its three chars. */
static const ble_uuid128_t OWF_SVC_UUID = OWF_UUID128(
    0x10, 0x7d, 0x5e, 0x8c, 0x1a, 0x2f, 0x2d, 0x9b,
    0x7a, 0x4c, 0x3e, 0x9a, 0x01, 0x00, 0x1f, 0x6b);
static const ble_uuid128_t OWF_PROV_UUID = OWF_UUID128(   // ...0002 WiFi provision (write)
    0x10, 0x7d, 0x5e, 0x8c, 0x1a, 0x2f, 0x2d, 0x9b,
    0x7a, 0x4c, 0x3e, 0x9a, 0x02, 0x00, 0x1f, 0x6b);
static const ble_uuid128_t OWF_FIND_UUID = OWF_UUID128(   // ...0003 find-phone (notify)
    0x10, 0x7d, 0x5e, 0x8c, 0x1a, 0x2f, 0x2d, 0x9b,
    0x7a, 0x4c, 0x3e, 0x9a, 0x03, 0x00, 0x1f, 0x6b);
static const ble_uuid128_t OWF_FINDWATCH_UUID = OWF_UUID128( // ...0004 find-watch (write)
    0x10, 0x7d, 0x5e, 0x8c, 0x1a, 0x2f, 0x2d, 0x9b,
    0x7a, 0x4c, 0x3e, 0x9a, 0x04, 0x00, 0x1f, 0x6b);
static const ble_uuid128_t OWF_PAIR_UUID = OWF_UUID128(      // ...0005 pair trigger (auth-gated read)
    0x10, 0x7d, 0x5e, 0x8c, 0x1a, 0x2f, 0x2d, 0x9b,
    0x7a, 0x4c, 0x3e, 0x9a, 0x05, 0x00, 0x1f, 0x6b);

/* Nordic UART Service (Gadgetbridge / Bangle.js transport). Standard UUIDs. */
static const ble_uuid128_t OWF_NUS_SVC_UUID = OWF_UUID128(   // 6e400001-...
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t OWF_NUS_RX_UUID = OWF_UUID128(    // 6e400002-... phone->watch (write)
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t OWF_NUS_TX_UUID = OWF_UUID128(    // 6e400003-... watch->phone (notify)
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* Standard 16-bit Battery Service + Battery Level. */
#define OWF_BATT_SVC_UUID16  0x180F
#define OWF_BATT_LVL_UUID16  0x2A19

/* GATT schema version - bump when the attribute table changes so a bonded phone
 * drops its cached handles (Service Changed). Starts at 1 for the Tuya table
 * (which differs from the ESP32 table: 1 custom svc + NUS + Battery, merged). */
#define OWF_GATT_SCHEMA_VER 2   /* v2: + pair-trigger char (...0005, auth-gated read) */

/* ---- runtime state (written on the NimBLE task, read by the loop) -------- */
static bool              s_ble_up         = false;   // host inited + advertising
static volatile bool     s_ble_connected  = false;
static volatile uint16_t s_ble_conn       = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     s_ble_forgetting = false;   // suppress auto-re-adv during a forget
static volatile bool     s_ble_show_key   = false;   // show the pair-code overlay
static volatile uint32_t s_ble_passkey    = 0;       // 6-digit code to display
static volatile bool     s_ble_pairing    = false;   // a passkey was shown this session
static volatile bool     s_ble_findwatch_req = false; // phone wrote find-watch -> ring (loop consumes)

/* characteristic value handles (filled by ble_gatts_add_svcs via val_handle ptrs) */
static uint16_t s_h_prov      = 0;
static uint16_t s_h_find      = 0;   // find-phone (notify)
static uint16_t s_h_findwatch = 0;
static uint16_t s_h_nus_rx    = 0;
static uint16_t s_h_nus_tx    = 0;   // Gadgetbridge TX (notify)
static uint16_t s_h_batt      = 0;
static uint16_t s_h_pair      = 0;   // pair trigger (auth-gated read -> phone initiates pairing)

/* Battery value cache: a read is served from here; ble_report_battery() updates it
 * and pushes a notify. Declared up here so the GATT access cb can read it. */
static uint8_t  s_ble_batt_val = 0;

/* Result toast + overlay bookkeeping - same enum/values as ble_provision.h so
 * app_wifi_ble.h works unchanged. */
enum BleToast { BLE_TOAST_NONE = 0, BLE_TOAST_PAIRED, BLE_TOAST_SAVED, BLE_TOAST_DUP, BLE_TOAST_FULL,
                BLE_TOAST_FORGETFAIL,
                // Low-internal-SRAM radio-bring-up refusals — MUST stay in sync with the copy
                // in ble_provision.h (notif_net.h sets these unconditionally, so both BLE
                // backends' enums need them). See the *_MIN_FREE_INTERNAL guards.
                BLE_TOAST_WIFI_OOM_BLE,    // WiFi refused, BLE up   -> "turn off BLE first"
                BLE_TOAST_WIFI_OOM_BOOT,   // WiFi refused, BLE off  -> "try a reboot"
                BLE_TOAST_BLE_OOM_WIFI,    // BLE refused,  WiFi on  -> "turn off WiFi first"
                BLE_TOAST_BLE_OOM_BOOT };  // BLE refused,  WiFi off -> "try a reboot"
static volatile BleToast s_ble_toast      = BLE_TOAST_NONE;
static char              s_ble_toast_ssid[WIFI_SSID_MAX] = "";
static lv_obj_t         *s_ble_key_box    = nullptr;   // loop thread only
static lv_obj_t         *s_ble_toast_box  = nullptr;   // loop thread only
static uint32_t          s_ble_toast_until = 0;
static volatile bool     s_ble_dirty      = false;     // one-shot repaint when an overlay appears/clears
static volatile bool     s_ble_bond_dirty = false;     // one-shot: bond added/removed

#define BLE_BOND_MAX 3   // mirrors TY_HS_BLE_STORE_MAX_BONDS

/* ----------------------------- public: status ---------------------------- */
static bool ble_is_up(void)            { return s_ble_up; }
static bool ble_phone_connected(void)  { return s_ble_up && s_ble_connected; }
static bool ble_overlay_active(void)   { return s_ble_show_key || s_ble_key_box || s_ble_toast_box; }
static bool ble_take_dirty(void)       { bool d = s_ble_dirty;      s_ble_dirty = false;      return d; }
static bool ble_take_bond_dirty(void)  { bool d = s_ble_bond_dirty; s_ble_bond_dirty = false; return d; }
static bool ble_take_find_watch_req(void){ bool r = s_ble_findwatch_req; s_ble_findwatch_req = false; return r; }

/* =========================== GATT access callback ========================= *
 * One access callback for every characteristic. NimBLE passes the value handle
 * (arg) we stashed at registration so we can tell which characteristic was hit.
 * Runs on the NimBLE host task. */
static int owf_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
  // Disambiguate by the value handle NimBLE assigned (filled into s_h_* via each
  // char's val_handle at registration). attr_handle is that value handle.
  uint16_t which = attr_handle;

  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    // Flatten the mbuf chain into a stack buffer.
    uint8_t buf[256];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(buf)) len = sizeof(buf);
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0) return BLE_ATT_ERR_UNLIKELY;

    if (which == s_h_nus_rx) {
      gb_rx_bytes(buf, len);                    // Gadgetbridge line bytes
    } else if (which == s_h_findwatch) {
      s_ble_findwatch_req = true;               // ring the watch (loop consumes)
      USBSerial.println("[ble] find-watch ping");
    } else if (which == s_h_prov) {
      // WiFi provision: "SSID,password" split on the FIRST comma (pass may contain commas).
      buf[len] = '\0';
      char *comma = strchr((char *)buf, ',');
      String ssid, pass;
      if (comma) { *comma = '\0'; ssid = (char *)buf; pass = comma + 1; }
      else       { ssid = (char *)buf; pass = ""; }
      ssid.trim();
      if (ssid.length()) {
        store_lock();
        bool existed = false;
        for (uint8_t i = 0; i < s_wifi_net_count; i++)
          if (strncmp(s_wifi_nets[i].ssid, ssid.c_str(), WIFI_SSID_MAX) == 0) { existed = true; break; }
        bool added = wifi_nets_add(ssid.c_str(), pass.c_str());
        if (added) wifi_nets_save();
        store_unlock();
        strncpy(s_ble_toast_ssid, ssid.c_str(), sizeof(s_ble_toast_ssid) - 1);
        s_ble_toast_ssid[sizeof(s_ble_toast_ssid) - 1] = '\0';
        s_ble_toast = added ? BLE_TOAST_SAVED : (existed ? BLE_TOAST_DUP : BLE_TOAST_FULL);
        USBSerial.printf("[ble] provision \"%s\" -> %s\n", ssid.c_str(),
                         added ? "saved" : (existed ? "duplicate" : "full"));
      }
    }
    return 0;
  }

  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (which == s_h_batt) {
      os_mbuf_append(ctxt->om, &s_ble_batt_val, 1);   // current battery %
      return 0;
    }
    if (which == s_h_pair) {
      // Reached only over an already-authenticated link (READ_AUTHEN below): reading
      // it unencrypted gets ATT Insufficient Authentication from the NimBLE host,
      // which makes the CENTRAL (iOS) initiate authenticated pairing itself — the
      // inbound path that does not depend on our outbound Security Request.
      uint8_t ok = 1;
      os_mbuf_append(ctxt->om, &ok, 1);
      USBSerial.println("[ble] pair characteristic read on an authenticated link");
      return 0;
    }
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

/* =========================== GATT service table ========================== *
 * Built once at ble_begin() and kept for the program life (NimBLE retains the
 * pointer). Three services: the custom OWF service (4 chars), the Nordic UART
 * Service for Gadgetbridge (2 chars), and the standard Battery Service (1 char).
 * Populated field-by-field in owf_build_gatt_table() rather than with C++
 * designated initializers - the Tuya Arduino core's C++ dialect can't be assumed
 * to support those, and the existing port (owf_tuya_port.h) assigns fields the
 * same way. Each char's value handle lands in an s_h_* global via val_handle;
 * owf_gatt_access_cb disambiguates by comparing attr_handle to those. Arrays are
 * one longer than the char count so the final {0} entry terminates the list. */
static const ble_uuid16_t OWF_BATT_SVC_U16 = OWF_UUID16(OWF_BATT_SVC_UUID16);
static const ble_uuid16_t OWF_BATT_LVL_U16 = OWF_UUID16(OWF_BATT_LVL_UUID16);

static struct ble_gatt_chr_def s_owf_chrs[5];   // prov, find, findwatch, pair, + terminator
static struct ble_gatt_chr_def s_nus_chrs[3];   // rx, tx, + terminator
static struct ble_gatt_chr_def s_batt_chrs[2];  // batt level, + terminator
static struct ble_gatt_svc_def s_owf_svcs[4];   // 3 services + terminator

static void owf_build_gatt_table(void) {
  memset(s_owf_chrs, 0, sizeof(s_owf_chrs));
  memset(s_nus_chrs, 0, sizeof(s_nus_chrs));
  memset(s_batt_chrs, 0, sizeof(s_batt_chrs));
  memset(s_owf_svcs, 0, sizeof(s_owf_svcs));

  // Custom service characteristics.
  s_owf_chrs[0].uuid = &OWF_PROV_UUID.u;
  s_owf_chrs[0].access_cb = owf_gatt_access_cb;
  s_owf_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
  s_owf_chrs[0].val_handle = &s_h_prov;
  s_owf_chrs[1].uuid = &OWF_FIND_UUID.u;
  s_owf_chrs[1].access_cb = owf_gatt_access_cb;
  s_owf_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
  s_owf_chrs[1].val_handle = &s_h_find;
  s_owf_chrs[2].uuid = &OWF_FINDWATCH_UUID.u;
  s_owf_chrs[2].access_cb = owf_gatt_access_cb;
  s_owf_chrs[2].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
  s_owf_chrs[2].val_handle = &s_h_findwatch;
  // Pair trigger: READ + READ_AUTHEN. An unencrypted read bounces with ATT
  // Insufficient Authentication, and iOS (the central) responds by initiating
  // AUTHENTICATED (MITM) pairing itself -> inbound SM Pairing Request -> our
  // PASSKEY_ACTION shows the 6-digit code. This is the pairing path that works
  // even if the T5 platform drops our outbound slave Security Request (the
  // suspected cause of the silent ANCS pairing stall). To pair from nRF: connect,
  // then READ characteristic ...0005 of the OWF service.
  s_owf_chrs[3].uuid = &OWF_PAIR_UUID.u;
  s_owf_chrs[3].access_cb = owf_gatt_access_cb;
  s_owf_chrs[3].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN;
  s_owf_chrs[3].val_handle = &s_h_pair;

  // Nordic UART Service (Gadgetbridge). RX is write-no-rsp (Bangle.js transport),
  // TX is notify. Unencrypted on purpose: Gadgetbridge's writes would be silently
  // dropped under a WRITE_ENC requirement (same rationale as the ESP32 build).
  s_nus_chrs[0].uuid = &OWF_NUS_RX_UUID.u;
  s_nus_chrs[0].access_cb = owf_gatt_access_cb;
  s_nus_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
  s_nus_chrs[0].val_handle = &s_h_nus_rx;
  s_nus_chrs[1].uuid = &OWF_NUS_TX_UUID.u;
  s_nus_chrs[1].access_cb = owf_gatt_access_cb;
  s_nus_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
  s_nus_chrs[1].val_handle = &s_h_nus_tx;

  // Standard Battery Service.
  s_batt_chrs[0].uuid = &OWF_BATT_LVL_U16.u;
  s_batt_chrs[0].access_cb = owf_gatt_access_cb;
  s_batt_chrs[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
  s_batt_chrs[0].val_handle = &s_h_batt;

  s_owf_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  s_owf_svcs[0].uuid = &OWF_SVC_UUID.u;
  s_owf_svcs[0].characteristics = s_owf_chrs;
  s_owf_svcs[1].type = BLE_GATT_SVC_TYPE_PRIMARY;
  s_owf_svcs[1].uuid = &OWF_NUS_SVC_UUID.u;
  s_owf_svcs[1].characteristics = s_nus_chrs;
  s_owf_svcs[2].type = BLE_GATT_SVC_TYPE_PRIMARY;
  s_owf_svcs[2].uuid = &OWF_BATT_SVC_U16.u;
  s_owf_svcs[2].characteristics = s_batt_chrs;
}

/* ============================ advertising ================================ */
static int owf_gap_event(struct ble_gap_event *event, void *arg);   // defined below

static void owf_adv_start(void) {
  struct ble_hs_adv_fields fields; memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.tx_pwr_lvl_is_present = 1;
  fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
  const char *name = ble_svc_gap_device_name();
  fields.name = (uint8_t *)name;
  fields.name_len = name ? strlen(name) : 0;
  fields.name_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) USBSerial.printf("[ble] adv_set_fields rc=%d\n", rc);

  // Advertise the custom service UUID in the scan response so a companion app can
  // find us by UUID (the 128-bit UUID won't fit alongside the name in the 31-byte
  // adv payload). Gadgetbridge matches by NAME, so the name in the adv is enough.
  struct ble_hs_adv_fields rsp; memset(&rsp, 0, sizeof(rsp));
  rsp.uuids128 = (ble_uuid128_t *)&OWF_SVC_UUID;
  rsp.num_uuids128 = 1;
  rsp.uuids128_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&rsp);

  struct ble_gap_adv_params advp; memset(&advp, 0, sizeof(advp));
  advp.conn_mode = BLE_GAP_CONN_MODE_UND;
  advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
  rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                         &advp, owf_gap_event, NULL);
  if (rc != 0) USBSerial.printf("[ble] adv_start rc=%d\n", rc);
}

/* ---- peer device-name capture (ports ble_provision.h's ble_peer_name_fetch) ----
 * The bond store only holds addresses, but every phone serves the standard GAP
 * Device Name characteristic (0x2A00) — e.g. "Noel's iPhone". Read it once per
 * encrypted connect and cache it in prefs keyed "bn"+mac, so the WiFi & BLE app's
 * paired list shows the phone's name instead of a MAC (ble_bond_label already
 * reads these keys; only this writer was missing on the Tuya port).
 *
 * ORDERING (differs from ESP32 — deliberate): the ESP32 build runs this read FIRST
 * and chains ANCS behind it. Tried here, that ordering KILLED notifications — the
 * read never completed on this stack and ANCS discovery was never kicked. So on the
 * T5 the chain is reversed: ANCS discovery/subscribe runs first (the proven path),
 * and ancs_ns_sub_done() calls this at the very END of the chain. Notifications can
 * never be hostage to the name feature; worst case the list shows a MAC until the
 * next connect. The terminal callback re-kicks the ANCS attribute pump in case a
 * backlog CP write bounced off the busy GATT pipe while the read was in flight. */
static char s_name_key[16] = "";   // prefs key of the peer whose name read is in flight

static int ble_name_read_cb(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)arg;
  if (err && err->status == 0 && attr && attr->om) {
    char name[32];
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len > sizeof(name) - 1) len = sizeof(name) - 1;
    if (len && os_mbuf_copydata(attr->om, 0, len, name) == 0) {
      name[len] = '\0';
      prefs.putString(s_name_key, name);
      s_ble_bond_dirty = true;       // an open paired list re-labels itself
      USBSerial.printf("[ble] peer name \"%s\"\n", name);
    }
    return 0;                        // not terminal — EDONE still follows
  }
  // Terminal (EDONE or error): the GATT pipe is free again — resume any queued
  // ANCS attribute fetches that couldn't send while our read held the pipe.
  USBSerial.printf("[ble] peer-name read done (status=%d)\n", err ? err->status : -1);
  ancs_req_pump();
  return 0;
}

static void ble_peer_name_fetch(uint16_t conn_handle) {
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(conn_handle, &desc) != 0) return;
  const uint8_t *a = desc.peer_id_addr.val;
  snprintf(s_name_key, sizeof(s_name_key), "bn%02x%02x%02x%02x%02x%02x",
           a[5], a[4], a[3], a[2], a[1], a[0]);
  // BLE_UUID16_INIT uses C designated initializers (".u.type = ...") that this C++
  // dialect rejects - plain aggregate init of the same struct instead.
  static const ble_uuid16_t NAME_UUID = { { BLE_UUID_TYPE_16 }, 0x2A00 };
  int rc = ble_gattc_read_by_uuid(conn_handle, 0x0001, 0xFFFF, &NAME_UUID.u,
                                  ble_name_read_cb, NULL);
  USBSerial.printf("[ble] peer-name read start rc=%d\n", rc);   // rc!=0: skip (MAC shown)
}

/* ============================== GAP events =============================== *
 * Single GAP event handler for the connection (server role) + all the security
 * and notify-RX events. Runs on the NimBLE host task. This is also where Phase B
 * (ANCS/AMS) taps in: on ENC_CHANGE we kick discovery, and NOTIFY_RX routes to
 * the ANCS/AMS handlers. */
static int owf_gap_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0) {
      s_ble_connected = true;
      s_ble_conn = event->connect.conn_handle;
      gb_set_tx_handle(s_ble_conn, s_h_nus_tx);
      // Security at connect ONLY for a peer we already have a bond with (fast
      // encryption restore on reconnect). For an UNBONDED peer we must NOT send
      // a Security Request here: an unbonded iPhone IGNORES the unsolicited
      // request, but the SM procedure it creates lingers ~30s and then blocks
      // (EALREADY) the correctly-timed request. Apple's flow: touch a protected
      // GATT resource first - the ANCS CCCD subscribe below bounces with
      // Insufficient Authentication, and ble_ancs_tuya.h initiates security AT
      // THAT MOMENT, which is when iOS raises its pairing dialog.
      {
        struct ble_gap_conn_desc d;
        bool bonded = false;
        if (ble_gap_conn_find(event->connect.conn_handle, &d) == 0) {
          // Diagnostic for the bonded-reconnect path: ota = the address on air,
          // id = after resolving-list resolution. id==ota with type 1 (random)
          // means the iPhone's RPA did NOT resolve -> bond can't match.
          USBSerial.printf("[ble] peer ota=%d:%02X..%02X id=%d:%02X..%02X\n",
                           d.peer_ota_addr.type, d.peer_ota_addr.val[5], d.peer_ota_addr.val[0],
                           d.peer_id_addr.type,  d.peer_id_addr.val[5],  d.peer_id_addr.val[0]);
          ble_addr_t addrs[BLE_BOND_MAX]; int n = 0;
          if (ble_store_util_bonded_peers(addrs, &n, BLE_BOND_MAX) == 0)
            for (int i = 0; i < n; i++)
              if (ble_addr_cmp(&addrs[i], &d.peer_id_addr) == 0) { bonded = true; break; }
        }
        if (bonded) {
          int src = ble_gap_security_initiate(event->connect.conn_handle);
          USBSerial.printf("[ble] connected (conn=0x%04X), bonded peer, security_initiate rc=%d\n",
                           event->connect.conn_handle, src);
        } else {
          USBSerial.printf("[ble] connected (conn=0x%04X), unbonded peer - security deferred "
                           "to the ANCS auth bounce\n", event->connect.conn_handle);
        }
      }
      ancs_on_connect(event->connect.conn_handle);   // pre-bond ANCS discovery (no-op in Phase A)
    } else {
      owf_adv_start();                          // failed connect -> keep advertising
    }
    return 0;

  case BLE_GAP_EVENT_DISCONNECT:
    s_ble_connected = false;
    s_ble_conn = BLE_HS_CONN_HANDLE_NONE;
    s_ble_show_key = false;
    ancs_reset(); ams_reset(); gb_reset();
    gb_set_tx_handle(BLE_HS_CONN_HANDLE_NONE, 0);
    if (s_ble_up && !s_ble_forgetting) owf_adv_start();
    return 0;

  case BLE_GAP_EVENT_ENC_CHANGE: {
    struct ble_gap_conn_desc desc;
    USBSerial.printf("[ble] enc_change status=%d\n", event->enc_change.status);
    if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 && desc.sec_state.encrypted) {
      s_ble_show_key = false;
      if (s_ble_pairing) { s_ble_toast = BLE_TOAST_PAIRED; s_ble_bond_dirty = true; }
      s_ble_pairing = false;
      USBSerial.println("[ble] link encrypted");
      // Kick ANCS discovery FIRST (one GATT procedure per connection; AMS and the
      // peer-name read are chained from the END of ANCS's subscribe chain in
      // ancs_ns_sub_done — see the ordering note above ble_peer_name_fetch).
      ancs_on_encrypted(event->enc_change.conn_handle);
    }
    return 0;
  }

  case BLE_GAP_EVENT_PASSKEY_ACTION:
    // DisplayOnly flow: the stack asks US for the passkey to show. Generate a fresh
    // random 6-digit code, raise the overlay (ble_ui_tick paints it on the loop
    // thread), and hand the code to the stack. The phone then prompts its user to
    // type it - completing an authenticated (MITM) pairing, which is what iOS
    // requires before it exposes ANCS to us.
    if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
      struct ble_sm_io io;
      memset(&io, 0, sizeof(io));
      io.action  = BLE_SM_IOACT_DISP;
      io.passkey = (uint32_t)tal_system_get_random(1000000);   // 000000..999999
      s_ble_passkey = io.passkey;
      s_ble_show_key = true;                 // ble_ui_tick raises the code overlay
      s_ble_pairing  = true;                 // a code was shown -> genuine pairing
      s_ble_dirty    = true;
      int prc = ble_sm_inject_io(event->passkey.conn_handle, &io);
      USBSerial.printf("[ble] passkey display %06u inject rc=%d\n",
                       (unsigned)io.passkey, prc);
      if (prc != 0) s_ble_show_key = false;  // stack rejected it - don't show a dead code
    } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
      // Shouldn't happen with DisplayOnly IO cap; confirm rather than stall.
      struct ble_sm_io io;
      memset(&io, 0, sizeof(io));
      io.action = BLE_SM_IOACT_NUMCMP;
      io.numcmp_accept = 1;
      ble_sm_inject_io(event->passkey.conn_handle, &io);
      USBSerial.println("[ble] numcmp auto-accepted (unexpected with DisplayOnly)");
    } else {
      USBSerial.printf("[ble] passkey action=%d (unhandled)\n",
                       event->passkey.params.action);
    }
    return 0;

  case BLE_GAP_EVENT_NOTIFY_RX: {
    // Inbound notification (Phase B ANCS/AMS live here). Flatten + route.
    uint8_t buf[256];
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len > sizeof(buf)) len = sizeof(buf);
    if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, NULL) == 0) {
      bool consumed = ble_tuya_route_notify_rx(event->notify_rx.conn_handle,
                                               event->notify_rx.attr_handle, buf, len);
      // Visibility for the ANCS pipeline: which handle notified and whether a
      // handler claimed it. An iOS NS/DS event landing "unrouted" means the
      // discovered value handles don't match what iOS is notifying on.
      USBSerial.printf("[ble] notify_rx attr=0x%04X len=%u %s\n",
                       event->notify_rx.attr_handle, (unsigned)len,
                       consumed ? "-> ancs" : "(unrouted)");
    }
    return 0;
  }

  case BLE_GAP_EVENT_MTU:
    USBSerial.printf("[ble] mtu=%d\n", event->mtu.value);
    return 0;

  case BLE_GAP_EVENT_REPEAT_PAIRING: {
    // The phone is re-pairing while we still hold an OLD bond for it (e.g. a
    // Phase A Just-Works bond, or the user removed the watch on the phone only).
    // Standard NimBLE pattern: delete our stale bond and tell the stack to RETRY
    // so the fresh pairing (with the passkey) can proceed - otherwise pairing is
    // silently rejected and the link stays unencrypted.
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
      ble_store_util_delete_peer(&desc.peer_id_addr);
    USBSerial.println("[ble] repeat pairing: dropped stale bond, retrying");
    return BLE_GAP_REPEAT_PAIRING_RETRY;
  }

  default:
    USBSerial.printf("[ble] gap event type=%d (unhandled)\n", event->type);
    return 0;
  }
}

/* ============================== lifecycle ================================ */

static bool s_ble_svcs_registered = false;

static void ble_begin(void) {
  if (s_ble_up) return;

  // Bond store FIRST — before the stack init. Host sync (inside stack init) runs
  // ble_hs_misc_restore_irks(), which iterates the store to load each bonded
  // peer's IRK into the controller's resolving list. With the store registered/
  // loaded only AFTER init (the original order), sync saw an empty store: no IRKs
  // -> a bonded iPhone reconnecting under a rotated RPA read as "unbonded peer",
  // its encryption-restore failed (svc disc error status=7 loops) and every
  // session forced a FRESH pairing.
  owf_ble_store_init();

  // Bring up the NimBLE host via the Tuya stack init (server + client roles so
  // Phase B ANCS can act as a GATT client on the same link). This wraps
  // tuya_ble_pre_init + ble_hs_sched_start and waits for sync.
  if (tkl_ble_stack_init(TKL_BLE_ROLE_SERVER | TKL_BLE_ROLE_CLIENT) != OPRT_OK) {
    USBSerial.println("[ble] stack init FAILED");
    return;
  }

  // Security config. PHASE B: libtuyaos.a is the SM-enabled rebuild
  // (TY_HS_BLE_SM_SC/LEGACY/BONDING/MITM=1 + vendored mesh_crypt TinyCrypt; see
  // tuya-t5e1-ble-stack-facts in project memory and edit 4 in
  // patches/apply_tuya_package_patches.sh). DisplayOnly + MITM: the watch SHOWS a
  // fresh random 6-digit code (pair-code overlay via ble_ui_tick) that the phone
  // types, giving the authenticated bonded link iOS requires before exposing ANCS.
  ble_hs_cfg.sm_io_cap         = BLE_HS_IO_DISPLAY_ONLY;      // we display, phone types
  ble_hs_cfg.sm_bonding        = 1;
  ble_hs_cfg.sm_mitm           = 1;                           // authenticated pairing (passkey)
  ble_hs_cfg.sm_sc             = 1;
  ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  // Bond store callbacks were registered BEFORE the stack init (host sync needs
  // them to restore bonded-peer IRKs into the resolving list). Re-assert the four
  // pointers here in case the Tuya init touched the config struct — without them
  // every Pairing Request is rejected (sm_err=8) before the passkey stage.
  ble_hs_cfg.store_read_cb   = owf_store_read_cb;
  ble_hs_cfg.store_write_cb  = owf_store_write_cb;
  ble_hs_cfg.store_delete_cb = owf_store_delete_cb;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  // GAP device name = per-unit "WatchFace-AB12" (MAC suffix), same as ESP32.
  ble_svc_gap_device_name_set(deviceRadioName().c_str());

  // Register our GATT table once (NimBLE keeps the pointer for the program life).
  // owf_gatt_access_cb disambiguates characteristics by attr_handle vs the s_h_*
  // value handles that ble_gatts_add_svcs fills via each char's val_handle.
  //
  // NimBLE builds the GATT attribute database in ble_gatts_start(), which runs at
  // host SYNC - and tkl_ble_stack_init() above already waited for sync. Services
  // added after that are accepted (rc=0) but NEVER enter the attribute table: the
  // phone saw only 0x1800/0x1801, and the OWF/NUS/Battery services (and their
  // s_h_* handles) were dead. The documented late-registration sequence is:
  // gatts_reset (empties the DB; no connections exist yet), re-init the built-in
  // GAP+GATT services, add ours, then gatts_start to rebuild the DB.
  if (!s_ble_svcs_registered) {
    owf_build_gatt_table();
    int rc = ble_gatts_reset();
    if (rc == 0) {
      ble_svc_gap_init();
      ble_svc_gatt_init();
      ble_svc_gap_device_name_set(deviceRadioName().c_str());
      rc = ble_gatts_count_cfg(s_owf_svcs);
      if (rc == 0) rc = ble_gatts_add_svcs(s_owf_svcs);
      if (rc == 0) rc = ble_gatts_start();
    }
    if (rc != 0) { USBSerial.printf("[ble] gatts register rc=%d\n", rc); }
    else         { USBSerial.printf("[ble] gatts registered: owf=0x%04X nus=0x%04X batt=0x%04X\n",
                                    (unsigned)s_h_pair, (unsigned)s_h_nus_tx, (unsigned)s_h_batt); }
    s_ble_svcs_registered = true;
  }

  owf_adv_start();
  s_ble_up = true;
  // The "SM/DisplayOnly" tag identifies the Phase B build on serial - if it is
  // missing, an old image (or the pre-SM libtuyaos.a) is running.
  USBSerial.println("[ble] up (advertising, SM/DisplayOnly build, Gadgetbridge + ANCS ready)");
}

static void ble_end(void) {
  if (!s_ble_up) return;
  s_ble_up = false;
  if (s_ble_connected && s_ble_conn != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(s_ble_conn, BLE_ERR_REM_USER_CONN_TERM);
    for (uint32_t t0 = millis(); s_ble_connected && millis() - t0 < 300; ) delay(10);
  }
  ble_gap_adv_stop();
  s_ble_connected = false;
  s_ble_show_key = false;
  gb_set_tx_handle(BLE_HS_CONN_HANDLE_NONE, 0);
  gb_reset();
  // Note: we keep the host inited (deinit on this port is heavy + the watch
  // full-reboots on deep sleep anyway, returning the RAM). Just stop advertising.
  USBSerial.println("[ble] down (advertising stopped)");
}

static void ble_apply_enabled(void) {
  if (settings_get_ble_enabled()) ble_begin();
  else                            ble_end();
}

/* ESP32-only NVS-leak fix (see ble_provision.h): its nvs_entry_find() key
 * enumeration has no tal_kv equivalent here, and this port stores only the few
 * per-bond "bn*" nickname keys (removed in ble_bond_forget), so there is nothing
 * accumulating to prune. Kept as a no-op so the .ino boot path stays shared. */
static inline void ble_prune_orphan_peer_keys(void) {}

/* ===================== find-my-phone / battery report ==================== */
static bool ble_ping_phone(void) {
  if (!s_ble_up || !s_ble_connected) return false;
  bool ok = gb_send("{\"t\":\"findPhone\",\"n\":true}");     // Android/Gadgetbridge
  if (s_h_find) {                                            // custom companion app
    ble_gattc_notify_custom(s_ble_conn, s_h_find, ble_hs_mbuf_from_flat((void *)"RING", 4));
    ok = true;
  }
  return ok;
}
static bool ble_stop_phone_ring(void) {
  if (!s_ble_up || !s_ble_connected) return false;
  return gb_send("{\"t\":\"findPhone\",\"n\":false}");
}
static void ble_report_battery(int pct, bool charging) {
  if (!s_ble_up || pct < 0) return;
  s_ble_batt_val = (pct > 100) ? 100 : (uint8_t)pct;
  if (s_h_batt && s_ble_connected)
    ble_gattc_notify_custom(s_ble_conn, s_h_batt, ble_hs_mbuf_from_flat(&s_ble_batt_val, 1));
  char line[48];
  snprintf(line, sizeof(line), "{\"t\":\"status\",\"bat\":%u,\"chg\":%u}", s_ble_batt_val, charging ? 1 : 0);
  gb_send(line);
}

/* ===================== bonded-device management ========================== */
static int ble_bond_count(void) {
  if (!s_ble_up) return 0;
  ble_addr_t addrs[BLE_BOND_MAX]; int n = 0;
  if (ble_store_util_bonded_peers(addrs, &n, BLE_BOND_MAX) != 0) return 0;
  return n;
}
static bool ble_bond_addr_str(int i, char *buf, size_t cap) {
  if (!s_ble_up) return false;
  ble_addr_t addrs[BLE_BOND_MAX]; int n = 0;
  if (ble_store_util_bonded_peers(addrs, &n, BLE_BOND_MAX) != 0 || i < 0 || i >= n) return false;
  const uint8_t *v = addrs[i].val;
  snprintf(buf, cap, "%02X:%02X:%02X:%02X:%02X:%02X", v[5], v[4], v[3], v[2], v[1], v[0]);
  return true;
}
static bool ble_bond_label(int i, char *buf, size_t cap) {
  if (!s_ble_up) return false;
  ble_addr_t addrs[BLE_BOND_MAX]; int n = 0;
  if (ble_store_util_bonded_peers(addrs, &n, BLE_BOND_MAX) != 0 || i < 0 || i >= n) return false;
  const uint8_t *v = addrs[i].val;
  char key[16];
  snprintf(key, sizeof(key), "bn%02x%02x%02x%02x%02x%02x", v[5], v[4], v[3], v[2], v[1], v[0]);
  String nm = prefs.getString(key, "");
  if (nm.length()) { strlcpy(buf, nm.c_str(), cap); return true; }
  return ble_bond_addr_str(i, buf, cap);
}
static bool ble_bond_forget(int i) {
  if (!s_ble_up) return false;
  ble_addr_t addrs[BLE_BOND_MAX]; int n = 0;
  if (ble_store_util_bonded_peers(addrs, &n, BLE_BOND_MAX) != 0 || i < 0 || i >= n) return false;
  s_ble_forgetting = true;
  ble_gap_adv_stop();
  if (s_ble_connected && s_ble_conn != BLE_HS_CONN_HANDLE_NONE) {
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_ble_conn, &d) == 0 && ble_addr_cmp(&d.peer_id_addr, &addrs[i]) == 0) {
      ble_gap_terminate(s_ble_conn, BLE_ERR_REM_USER_CONN_TERM);
      for (uint32_t t0 = millis(); s_ble_connected && millis() - t0 < 500; ) delay(10);
    }
  }
  int rc = ble_gap_unpair(&addrs[i]);
  if (rc != 0) { USBSerial.printf("[ble] unpair rc=%d\n", rc); s_ble_forgetting = false; if (s_ble_up) owf_adv_start(); return false; }
  // drop cached per-peer name
  const uint8_t *v = addrs[i].val; char key[16];
  snprintf(key, sizeof(key), "bn%02x%02x%02x%02x%02x%02x", v[5], v[4], v[3], v[2], v[1], v[0]);
  prefs.remove(key);
  s_ble_bond_dirty = true;
  s_ble_forgetting = false;
  if (s_ble_up) owf_adv_start();
  return true;
}

/* ===================== pair-code overlay + toast (LVGL thread) =========== *
 * Identical behaviour/styling to ble_provision.h's ble_ui_tick(). Must run on
 * the loop (LVGL) thread; reads the volatile flags set by the NimBLE task. */
static void ble_ui_tick(void) {
  if (s_ble_show_key && !s_ble_key_box) {
    s_ble_key_box = lv_obj_create(lv_layer_sys());
#if BOARD_SCREEN_NARROW
    lv_obj_set_size(s_ble_key_box, LV_PCT(92), 140);
#else
    lv_obj_set_size(s_ble_key_box, 300, 140);
#endif
    lv_obj_center(s_ble_key_box);
    lv_obj_set_style_bg_color(s_ble_key_box, lv_color_hex(0x101820), 0);
    lv_obj_set_style_radius(s_ble_key_box, 16, 0);
    lv_obj_clear_flag(s_ble_key_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(s_ble_key_box);
    lv_obj_set_style_text_font(t, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(t, "BLE pairing - enter code:");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *k = lv_label_create(s_ble_key_box);
#if BOARD_SCREEN_NARROW
    lv_obj_set_style_text_font(k, &lv_font_montserrat_28, 0);
#else
    lv_obj_set_style_text_font(k, &FONT_LABEL, 0);
#endif
    lv_obj_set_style_text_color(k, lv_color_white(), 0);
    lv_label_set_text_fmt(k, "%06u", (unsigned)s_ble_passkey);
    lv_obj_align(k, LV_ALIGN_CENTER, 0, 14);
    s_ble_dirty = true;
  } else if (!s_ble_show_key && s_ble_key_box) {
    lv_obj_del(s_ble_key_box); s_ble_key_box = nullptr; s_ble_dirty = true;
  }

  if (s_ble_toast != BLE_TOAST_NONE && !s_ble_toast_box) {
    BleToast r = s_ble_toast; s_ble_toast = BLE_TOAST_NONE;
    s_ble_toast_box = lv_obj_create(lv_layer_sys());
#if BOARD_SCREEN_NARROW
    lv_obj_set_size(s_ble_toast_box, LV_PCT(92), 92);
#else
    lv_obj_set_size(s_ble_toast_box, 340, 92);
#endif
    lv_obj_align(s_ble_toast_box, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_color(s_ble_toast_box, lv_color_hex(0x141414), 0);
    lv_obj_set_style_radius(s_ble_toast_box, 16, 0);
    lv_obj_clear_flag(s_ble_toast_box, LV_OBJ_FLAG_SCROLLABLE);
    uint32_t col; char msg[80];
    if (r == BLE_TOAST_PAIRED)      { col = 0x33A0FF; snprintf(msg, sizeof(msg), LV_SYMBOL_BLUETOOTH "  Phone paired"); }
    else if (r == BLE_TOAST_SAVED)  { col = 0x32D74B; snprintf(msg, sizeof(msg), LV_SYMBOL_OK "  WiFi saved\n%s", s_ble_toast_ssid); }
    else if (r == BLE_TOAST_DUP)    { col = 0xFF9F0A; snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  Already saved\n%s", s_ble_toast_ssid); }
    else if (r == BLE_TOAST_FORGETFAIL) { col = 0xFF453A; snprintf(msg, sizeof(msg), LV_SYMBOL_CLOSE "  Couldn't forget phone\nTry again"); }
    else if (r == BLE_TOAST_WIFI_OOM_BLE)  { col = 0xFF453A; snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  Couldn't start WiFi - low RAM\nTurn off BLE first"); }
    else if (r == BLE_TOAST_WIFI_OOM_BOOT) { col = 0xFF453A; snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  Couldn't start WiFi - low RAM\nTry a reboot"); }
    else if (r == BLE_TOAST_BLE_OOM_WIFI)  { col = 0xFF453A; snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  Couldn't start BLE - low RAM\nTurn off WiFi first"); }
    else if (r == BLE_TOAST_BLE_OOM_BOOT)  { col = 0xFF453A; snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  Couldn't start BLE - low RAM\nTry a reboot"); }
    else                            { col = 0xFF453A; snprintf(msg, sizeof(msg), LV_SYMBOL_CLOSE "  Network list full"); }
    lv_obj_t *l = lv_label_create(s_ble_toast_box);
    lv_obj_set_style_text_font(l, &FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
#if BOARD_SCREEN_NARROW
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); lv_obj_set_width(l, LV_PCT(90));
#else
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);  lv_obj_set_width(l, 312);
#endif
    lv_label_set_text(l, msg);
    lv_obj_center(l);
    s_ble_toast_until = millis() + 3500;
    s_ble_dirty = true;
  } else if (s_ble_toast_box && (int32_t)(millis() - s_ble_toast_until) > 0) {
    lv_obj_del(s_ble_toast_box); s_ble_toast_box = nullptr; s_ble_dirty = true;
  }
}
