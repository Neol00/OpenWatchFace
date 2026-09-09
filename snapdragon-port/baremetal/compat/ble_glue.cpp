/* ble_glue.cpp — implementation of compat/BLEDevice.h on the NimBLE host.
 *
 * Lifecycle (differs from ESP where the host auto-starts): BLEDevice::init()
 * opens the HCI transport (booting WCNSS if WiFi has not) and initialises the
 * host once; the app then builds its services; the FIRST startAdvertising()
 * registers the GATT table and starts the host, and the host's sync callback
 * starts advertising. deinit() stops the host (ble_hs_stop), resets the GATT
 * table and drops the server, so the next init()/createServer() starts clean.
 * Every callback into the app runs on the "bt-host" task. */
#include "BLEDevice.h"
#include <stdlib.h>
#include <stdio.h>
extern "C" {
#include "platform.h"
#include "nimble/nimble_port.h"
#include "host/util/util.h"
#include "host/ble_hs_stop.h"
#include "store/config/ble_store_config.h"
void ble_store_config_init(void);
void owf_ble_store_load(void);
void owf_ble_store_hook(void);
void owf_ble_host_task_start(void);
}
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

static void blog(const char *s) { con_dbg("[ble] "); con_dbg(s); con_dbg("\n"); }

/* ================================================================ UUID */
static int hexv(char c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }
BLEUUID::BLEUUID() : m_valid(false) { memset(&m_uuid, 0, sizeof m_uuid); }
BLEUUID::BLEUUID(uint16_t u) : m_valid(true) { memset(&m_uuid, 0, sizeof m_uuid); m_uuid.u16.u.type = BLE_UUID_TYPE_16; m_uuid.u16.value = u; }
BLEUUID::BLEUUID(uint32_t u) : m_valid(true) { memset(&m_uuid, 0, sizeof m_uuid); m_uuid.u32.u.type = BLE_UUID_TYPE_32; m_uuid.u32.value = u; }
BLEUUID::BLEUUID(const char *s) : m_valid(false)
{
    memset(&m_uuid, 0, sizeof m_uuid);
    size_t n = strlen(s);
    if (n == 4 || n == 8) {
        uint32_t v = 0; for (size_t i = 0; i < n; i++) { int h = hexv(s[i]); if (h < 0) return; v = (v << 4) | (uint32_t)h; }
        if (n == 4) { m_uuid.u16.u.type = BLE_UUID_TYPE_16; m_uuid.u16.value = (uint16_t)v; }
        else        { m_uuid.u32.u.type = BLE_UUID_TYPE_32; m_uuid.u32.value = v; }
        m_valid = true;
    } else if (n == 36) {
        uint8_t b[16]; int k = 0;
        for (size_t i = 0; i < n && k < 16; ) {
            if (s[i] == '-') { i++; continue; }
            int h1 = hexv(s[i]), h2 = hexv(s[i + 1]); if (h1 < 0 || h2 < 0) return;
            b[k++] = (uint8_t)((h1 << 4) | h2); i += 2;
        }
        m_uuid.u128.u.type = BLE_UUID_TYPE_128;
        for (int i = 0; i < 16; i++) m_uuid.u128.value[i] = b[15 - i];   /* NimBLE stores LSB first */
        m_valid = true;
    }
}
String BLEUUID::toString() const
{
    char buf[40];
    if (!m_valid) return String("<null>");
    ble_uuid_to_str(&m_uuid.u, buf);
    return String(buf);
}
String BLEAddress::toString() const
{
    char b[18];
    snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m_addr[5], m_addr[4], m_addr[3], m_addr[2], m_addr[1], m_addr[0]);
    return String(b);
}

/* ====================================================== characteristic */
BLECharacteristic::BLECharacteristic(BLEUUID uuid, uint32_t properties) : m_uuid(uuid), m_props(properties) {}
String BLECharacteristic::getValue() const
{
    String s; s.reserve(m_value.size() + 1);
    s.concat((const char *)m_value.data(), (unsigned)m_value.size());
    return s;
}
void BLECharacteristic::setValue(const uint8_t *data, size_t size) { m_value.assign(data, data + size); }

static int chr_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    return ((BLECharacteristic *)arg)->access(conn, ctxt);
}
static int dsc_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    BLEDescriptor *d = (BLEDescriptor *)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC)
        return os_mbuf_append(ctxt->om, d->m_value.data(), (uint16_t)d->m_value.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        d->m_value.resize(len);
        if (len) os_mbuf_copydata(ctxt->om, 0, len, d->m_value.data());
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
int BLECharacteristic::access(uint16_t conn, struct ble_gatt_access_ctxt *ctxt)
{
    (void)conn;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (m_cb) m_cb->onRead(this);
        return os_mbuf_append(ctxt->om, m_value.data(), (uint16_t)m_value.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        m_value.resize(len);
        if (len) os_mbuf_copydata(ctxt->om, 0, len, m_value.data());
        if (m_cb) m_cb->onWrite(this);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
void BLECharacteristic::notify(bool is_notification)
{
    BLEServer *srv = BLEDevice::getServer();
    if (!srv || srv->m_connId == BLE_HS_CONN_HANDLE_NONE || !m_handle) return;
    if (!(m_subs & (1u << (srv->m_connId & 15u)))) return;    /* nobody subscribed */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(m_value.data(), (uint16_t)m_value.size());
    if (!om) return;
    int rc = is_notification ? ble_gatts_notify_custom(srv->m_connId, m_handle, om)
                             : ble_gatts_indicate_custom(srv->m_connId, m_handle, om);
    if (rc != 0) { con_puts("[ble] notify rc="); con_putdec((uint32_t)rc); con_puts("\n"); }
    if (m_cb) m_cb->onNotify(this);
}

/* ============================================================= service */
BLECharacteristic *BLEService::createCharacteristic(BLEUUID uuid, uint32_t properties)
{
    BLECharacteristic *c = new BLECharacteristic(uuid, properties);
    addCharacteristic(c);
    return c;
}
void BLEService::addCharacteristic(BLECharacteristic *c) { c->m_service = this; m_chars.push_back(c); }
BLECharacteristic *BLEService::getCharacteristic(BLEUUID uuid)
{
    for (auto *c : m_chars) if (c->m_uuid.equals(uuid)) return c;
    return nullptr;
}
bool BLEService::start() { m_started = true; return true; }
void BLEService::buildDef()
{
    m_chrDefs.clear(); m_dscDefs.clear();
    m_dscDefs.resize(m_chars.size());
    for (size_t i = 0; i < m_chars.size(); i++) {
        BLECharacteristic *c = m_chars[i];
        auto &dd = m_dscDefs[i];
        for (auto *d : c->m_descs) {
            struct ble_gatt_dsc_def x; memset(&x, 0, sizeof x);
            x.uuid = &d->getUUID().getNative()->u;
            x.att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE;
            x.access_cb = dsc_access_cb; x.arg = d;
            x.min_key_size = 0;
            dd.push_back(x);
        }
        struct ble_gatt_dsc_def end; memset(&end, 0, sizeof end); dd.push_back(end);
    }
    for (size_t i = 0; i < m_chars.size(); i++) {
        BLECharacteristic *c = m_chars[i];
        struct ble_gatt_chr_def x; memset(&x, 0, sizeof x);
        x.uuid = &c->m_uuid.getNative()->u;
        x.access_cb = chr_access_cb; x.arg = c;
        x.descriptors = (m_dscDefs[i].size() > 1) ? m_dscDefs[i].data() : nullptr;
        x.flags = (ble_gatt_chr_flags)c->m_props;
        x.min_key_size = 0;
        x.val_handle = &c->m_handle;
        m_chrDefs.push_back(x);
    }
    struct ble_gatt_chr_def end; memset(&end, 0, sizeof end); m_chrDefs.push_back(end);
    memset(m_svcDef, 0, sizeof m_svcDef);
    m_svcDef[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    m_svcDef[0].uuid = &m_uuid.getNative()->u;
    m_svcDef[0].characteristics = m_chrDefs.data();
}

/* ============================================================== server */
BLEServer::BLEServer() {}
BLEService *BLEServer::createService(BLEUUID uuid, uint32_t, uint8_t)
{
    BLEService *s = new BLEService(uuid, this);
    m_services.push_back(s);
    return s;
}
BLEService *BLEServer::getServiceByUUID(BLEUUID uuid)
{
    for (auto *s : m_services) if (s->m_uuid.equals(uuid)) return s;
    return nullptr;
}
BLEAdvertising *BLEServer::getAdvertising() { return BLEDevice::getAdvertising(); }
void BLEServer::startAdvertising() { BLEDevice::startAdvertising(); }
void BLEServer::start() { BLEDevice::startAdvertising(); }
int BLEServer::disconnect(uint16_t connId, uint8_t reason) { return ble_gap_terminate(connId, reason); }
void BLEServer::updateConnParams(uint16_t conn, uint16_t minItvl, uint16_t maxItvl, uint16_t latency, uint16_t timeout)
{
    struct ble_gap_upd_params p; memset(&p, 0, sizeof p);
    p.itvl_min = minItvl; p.itvl_max = maxItvl; p.latency = latency; p.supervision_timeout = timeout;
    p.min_ce_len = 0x0010; p.max_ce_len = 0x0300;
    ble_gap_update_params(conn, &p);
}
uint16_t BLEServer::getPeerMTU(uint16_t conn) { return ble_att_mtu(conn); }

/* register every started service with the host; must precede ble_hs_start */
void BLEServer::registerServices()
{
    for (auto *s : m_services) {
        if (!s->m_started) continue;
        s->buildDef();
        int rc = ble_gatts_count_cfg(s->m_svcDef);
        if (rc == 0) rc = ble_gatts_add_svcs(s->m_svcDef);
        if (rc != 0) { con_puts("[ble] add service "); con_puts(s->m_uuid.toString().c_str()); con_puts(" rc="); con_putdec((uint32_t)rc); con_puts("\n"); }
    }
}

int BLEServer::gapEvent(struct ble_gap_event *ev, void *arg)
{
    BLEServer *srv = BLEDevice::getServer();
    BLESecurityCallbacks *sec = BLEDevice::getSecurityCallbacks();
    struct ble_gap_conn_desc desc;
    int rc = 0;
    (void)arg;

    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            if (srv) { srv->m_connId = ev->connect.conn_handle; srv->m_connCount++; }
            if (BLESecurity::m_regen) BLESecurity::nextPasskey();
            if (ble_gap_conn_find(ev->connect.conn_handle, &desc) == 0) {
                con_puts("[ble] connected "); con_puts(BLEAddress(desc.peer_id_addr).toString().c_str()); con_puts("\n");
                if (srv && srv->m_cb) srv->m_cb->onConnect(srv, &desc);
            } else if (srv && srv->m_cb) srv->m_cb->onConnect(srv);
            /* like the ESP wrapper: pair as soon as the link is up, so the watch shows
             * its passkey without the phone first having to touch an encrypted
             * characteristic (iOS + ANCS need the bond before anything else) */
            if (BLESecurity::m_securityEnabled && BLESecurity::m_forceSecurity) {
                rc = ble_gap_security_initiate(ev->connect.conn_handle);
                con_puts("[ble] security initiate rc="); con_putdec((uint32_t)rc); con_puts("\n");
            }
        } else {
            blog("connect failed; advertising again");
            BLEDevice::startAdvertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        con_puts("[ble] disconnected, reason "); con_putdec((uint32_t)ev->disconnect.reason); con_puts("\n");
        if (srv) {
            for (auto *s : srv->m_services) for (auto *c : s->m_chars) c->m_subs &= ~(1u << (ev->disconnect.conn.conn_handle & 15u));
            if (srv->m_connCount) srv->m_connCount--;
            srv->m_connId = BLE_HS_CONN_HANDLE_NONE;
            if (srv->m_cb) srv->m_cb->onDisconnect(srv, &ev->disconnect.conn);
            if (srv->m_advOnDisc) BLEDevice::startAdvertising();
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (srv) {
            for (auto *s : srv->m_services) for (auto *c : s->m_chars) {
                if (c->m_handle != ev->subscribe.attr_handle) continue;
                uint16_t bit = 1u << (ev->subscribe.conn_handle & 15u);
                if (ev->subscribe.cur_notify || ev->subscribe.cur_indicate) c->m_subs |= bit; else c->m_subs &= ~bit;
                if (c->m_cb) c->m_cb->onStatus(c, (ev->subscribe.cur_notify || ev->subscribe.cur_indicate) ? 0 : 1, 0);
            }
        }
        break;
    case BLE_GAP_EVENT_MTU:
        con_dbg("[ble] MTU "); con_dbg_dec(ev->mtu.value); con_dbg("\n");
        if (srv && srv->m_cb && ble_gap_conn_find(ev->mtu.conn_handle, &desc) == 0) srv->m_cb->onMtuChanged(srv, &desc, ev->mtu.value);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0) {
            con_dbg("[ble] enc change status "); con_dbg_dec((uint32_t)ev->enc_change.status);
            con_dbg(desc.sec_state.encrypted ? " encrypted" : " not encrypted"); con_dbg(desc.sec_state.bonded ? " bonded\n" : "\n");
            if (sec) sec->onAuthenticationComplete(&desc);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* the peer lost its bond: delete ours and let it pair again */
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pk; memset(&pk, 0, sizeof pk);
        pk.action = ev->passkey.params.action;
        if (pk.action == BLE_SM_IOACT_DISP) {
            pk.passkey = BLESecurity::m_staticPasskey ? BLESecurity::m_passkey : BLESecurity::nextPasskey();
            con_puts("[ble] passkey to display: "); con_putdec(pk.passkey); con_puts("\n");
            if (sec) sec->onPassKeyNotify(pk.passkey);
            rc = ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        } else if (pk.action == BLE_SM_IOACT_NUMCMP) {
            pk.numcmp_accept = sec ? sec->onConfirmPIN(ev->passkey.params.numcmp) : 1;
            rc = ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        } else if (pk.action == BLE_SM_IOACT_INPUT) {
            pk.passkey = sec ? sec->onPassKeyRequest() : BLESecurity::m_passkey;
            rc = ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        } else if (pk.action == BLE_SM_IOACT_OOB) {
            for (int i = 0; i < 16; i++) pk.oob[i] = (uint8_t)i;
            rc = ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        }
        if (rc) { con_puts("[ble] inject_io rc="); con_putdec((uint32_t)rc); con_puts("\n"); }
        break;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        static uint32_t n;
        if (n++ < 50u) { con_dbg("[ble] notify rx attr 0x"); con_dbg_hex(ev->notify_rx.attr_handle); con_dbg(" len "); con_dbg_dec(ev->notify_rx.om ? OS_MBUF_PKTLEN(ev->notify_rx.om) : 0u); con_dbg(ev->notify_rx.indication ? " (indication)\n" : "\n"); }
        break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_NOTIFY_TX:
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
    default:
        break;
    }
    if (BLEDevice::getCustomGapHandler()) BLEDevice::getCustomGapHandler()(ev, arg);
    return 0;
}

/* ========================================================= advertising */
bool BLEAdvertising::removeServiceUUID(BLEUUID uuid)
{
    for (size_t i = 0; i < m_uuids.size(); i++) if (m_uuids[i].equals(uuid)) { m_uuids.erase(m_uuids.begin() + i); return true; }
    return false;
}
bool BLEAdvertising::isAdvertising() { return ble_gap_adv_active() != 0; }
bool BLEAdvertising::stop() { int rc = ble_gap_adv_stop(); return rc == 0 || rc == BLE_HS_EALREADY; }

bool BLEAdvertising::start(uint32_t duration, void (*cb)(BLEAdvertising *))
{
    (void)cb;
    if (!BLEDevice::m_synced) return false;
    if (ble_gap_adv_active()) return true;

    struct ble_hs_adv_fields adv, rsp;
    memset(&adv, 0, sizeof adv); memset(&rsp, 0, sizeof rsp);
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ble_uuid16_t u16[4]; ble_uuid32_t u32[2]; ble_uuid128_t u128[2];
    int n16 = 0, n32 = 0, n128 = 0;
    for (auto &u : m_uuids) {
        const ble_uuid_any_t *a = u.getNative();
        if (a->u.type == BLE_UUID_TYPE_16 && n16 < 4) u16[n16++] = a->u16;
        else if (a->u.type == BLE_UUID_TYPE_32 && n32 < 2) u32[n32++] = a->u32;
        else if (a->u.type == BLE_UUID_TYPE_128 && n128 < 2) u128[n128++] = a->u128;
    }
    if (n16) { adv.uuids16 = u16; adv.num_uuids16 = n16; adv.uuids16_is_complete = 1; }
    if (n32) { adv.uuids32 = u32; adv.num_uuids32 = n32; adv.uuids32_is_complete = 1; }
    if (n128) { adv.uuids128 = u128; adv.num_uuids128 = n128; adv.uuids128_is_complete = 1; }
    if (m_appearance) { adv.appearance = m_appearance; adv.appearance_is_present = 1; }

    String name = m_name.length() ? m_name : BLEDevice::getDeviceName();
    struct ble_hs_adv_fields *nameTo = m_scanRsp ? &rsp : &adv;
    nameTo->name = (uint8_t *)name.c_str(); nameTo->name_len = (uint8_t)name.length(); nameTo->name_is_complete = 1;
    if (m_txPower) { rsp.tx_pwr_lvl_is_present = 1; rsp.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO; }

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc == BLE_HS_EMSGSIZE && !m_scanRsp) {         /* too long: move the name to the scan response */
        adv.name = nullptr; adv.name_len = 0; adv.name_is_complete = 0;
        rsp.name = (uint8_t *)name.c_str(); rsp.name_len = (uint8_t)name.length(); rsp.name_is_complete = 1;
        rc = ble_gap_adv_set_fields(&adv);
    }
    if (rc != 0) { con_puts("[ble] adv_set_fields rc="); con_putdec((uint32_t)rc); con_puts("\n"); return false; }
    if (rsp.name || rsp.tx_pwr_lvl_is_present) {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
        if (rc != 0) { con_puts("[ble] adv_rsp_set_fields rc="); con_putdec((uint32_t)rc); con_puts("\n"); }
    }

    struct ble_gap_adv_params p; memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (m_minItvl) p.itvl_min = m_minItvl;
    if (m_maxItvl) p.itvl_max = m_maxItvl;
    rc = ble_gap_adv_start(BLEDevice::m_ownAddrType, NULL, duration ? (int32_t)duration : BLE_HS_FOREVER, &p, BLEServer::gapEvent, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) { con_puts("[ble] adv_start rc="); con_putdec((uint32_t)rc); con_puts("\n"); return false; }
    blog("advertising");
    return true;
}

/* ============================================================ security */
bool BLESecurity::m_staticPasskey = false, BLESecurity::m_regen = false, BLESecurity::m_passkeySet = false;
bool BLESecurity::m_forceSecurity = true, BLESecurity::m_securityEnabled = false;   /* ESP default: pair right after connect */
uint32_t BLESecurity::m_passkey = BLE_SM_DEFAULT_PASSKEY;
void BLESecurity::setAuthenticationMode(bool bonding, bool mitm, bool sc)
{
    ble_hs_cfg.sm_bonding = bonding; ble_hs_cfg.sm_mitm = mitm; ble_hs_cfg.sm_sc = sc;
    m_securityEnabled = bonding || mitm || sc;
    if (bonding) { ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID; ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID; }
}
void BLESecurity::setAuthenticationMode(uint8_t auth_req) { setAuthenticationMode(auth_req & 1, (auth_req >> 2) & 1, (auth_req >> 3) & 1); }
void BLESecurity::setCapability(uint8_t iocap) { ble_hs_cfg.sm_io_cap = iocap; }
void BLESecurity::setInitEncryptionKey(uint8_t k) { ble_hs_cfg.sm_our_key_dist = k; }
void BLESecurity::setRespEncryptionKey(uint8_t k) { ble_hs_cfg.sm_their_key_dist = k; }
void BLESecurity::setPassKey(bool staticPasskey, uint32_t passkey)
{
    m_staticPasskey = staticPasskey; m_passkeySet = true;
    m_passkey = staticPasskey ? passkey : nextPasskey();
}
void BLESecurity::resetSecurity() { m_staticPasskey = false; m_regen = false; m_passkeySet = false; m_passkey = BLE_SM_DEFAULT_PASSKEY; }
bool BLESecurity::startSecurity(uint16_t conn, int *rcPtr) { int rc = ble_gap_security_initiate(conn); if (rcPtr) *rcPtr = rc; return rc == 0; }
uint32_t BLESecurity::nextPasskey()
{
    static bool seeded = false;
    if (!seeded) { srand(timer_ms() ^ 0x5A5A1234u); seeded = true; }
    uint32_t pk = ((uint32_t)rand() * 7919u + (uint32_t)rand()) % 1000000u;
    if (pk < 100000u) pk += 100000u;                       /* always 6 visible digits */
    m_passkey = pk;
    return pk;
}

/* ============================================================== device */
bool BLEDevice::m_initialized = false, BLEDevice::m_hostStarted = false, BLEDevice::m_synced = false;
BLEServer *BLEDevice::m_pServer = nullptr;
BLEAdvertising *BLEDevice::m_bleAdvertising = nullptr;
BLESecurityCallbacks *BLEDevice::m_securityCallbacks = nullptr;
gap_event_handler BLEDevice::m_customGapHandler = nullptr;
String BLEDevice::m_deviceName;
uint16_t BLEDevice::m_localMTU = 247;
uint8_t BLEDevice::m_ownAddrType = BLE_OWN_ADDR_PUBLIC;
static bool s_portInit = false;

static int store_status_cb_wrapper(struct ble_store_status_event *ev, void *arg)
{
    return ble_store_util_status_rr(ev, arg);
}

void BLEDevice::onReset(int reason)
{
    con_puts("[ble] host reset, reason "); con_putdec((uint32_t)reason); con_puts("\n");
    m_synced = false;
}
void BLEDevice::ensureAddr()
{
    /* The WCN3620 reports a bogus public BD_ADDR (00:00:00:00:xx:xx) until Android's
     * vendor NV write programs it. Use a RANDOM STATIC address instead, generated
     * once and persisted (NVS "ble/addr") so bonded phones still recognise us after
     * a reboot. */
    uint8_t a[6]; int rc;
    if (nvs_get("ble", "addr", a, 6) != 6 || (a[5] & 0xC0) != 0xC0) {
        ble_addr_t gen;
        rc = ble_hs_id_gen_rnd(0, &gen);
        if (rc == 0) { memcpy(a, gen.val, 6); nvs_put("ble", "addr", 3, a, 6); nvs_commit(); blog("generated a random static address"); }
    }
    rc = ble_hs_id_set_rnd(a);
    if (rc != 0) { con_puts("[ble] set_rnd rc="); con_putdec((uint32_t)rc); con_puts("\n"); ble_hs_util_ensure_addr(0); m_ownAddrType = BLE_OWN_ADDR_PUBLIC; }
    else m_ownAddrType = BLE_OWN_ADDR_RANDOM;
    uint8_t cur[6]; int isnrpa = 0;
    if (ble_hs_id_copy_addr(m_ownAddrType == BLE_OWN_ADDR_RANDOM ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC, cur, &isnrpa) == 0) {
        con_puts("[ble] own address "); con_puts(BLEAddress(cur).toString().c_str());
        con_dbg(m_ownAddrType == BLE_OWN_ADDR_RANDOM ? " (random static)\n" : " (public)\n");
    }
}
void BLEDevice::onSync()
{
    ensureAddr();
    m_synced = true;
    blog("host synced");
    if (m_bleAdvertising) m_bleAdvertising->start();
}

bool BLEDevice::init(String deviceName)
{
    if (m_initialized) return true;
    if (deviceName.length()) m_deviceName = deviceName;
    if (!m_deviceName.length()) m_deviceName = "WatchFace";

    /* Bring the controller up. This is the ONE place the BLE stack touches
     * board-specific hardware: everything above it (GAP, GATT, pairing, ANCS,
     * AMS) is plain NimBLE and is identical on every watch.
     *   8909w  - the controller is the WCNSS core, reached over SMD; opening
     *            the HCI channels boots it if needed.
     *   Gen 6  - the controller is a WCN3990 on blsp2_uart2: power it, then
     *            download its firmware over HCI, THEN let the host start.
     *            Doing it here (rather than on a boot timer) is what keeps the
     *            ordering right - the host never talks to a chip that has no
     *            firmware yet. */
#if defined(PLAT_BOARD_FOSSIL_GEN6)
    int rc = bt_wcn3990_bringup();
#else
    wlan_lock();
    int rc = bt_hci_open();
    wlan_unlock();
#endif
    if (rc != 0) { blog("HCI transport not available"); return false; }

    if (!s_portInit) {
        nimble_port_init();
        s_portInit = true;
    }
    ble_hs_cfg.reset_cb = BLEDevice::onReset;
    ble_hs_cfg.sync_cb = BLEDevice::onSync;
    ble_hs_cfg.store_status_cb = store_status_cb_wrapper;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0; ble_hs_cfg.sm_mitm = 0; ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();
    owf_ble_store_hook();
    owf_ble_store_load();
    ble_svc_gap_device_name_set(m_deviceName.c_str());
    printf("[ble] gap name \"%s\"\n", m_deviceName.c_str());
    ble_att_set_preferred_mtu(m_localMTU);
    owf_ble_host_task_start();
    m_initialized = true;
    blog("initialised");
    return true;
}

bool BLEDevice::startHost()
{
    if (m_hostStarted) return true;
    if (m_pServer) m_pServer->registerServices();
    ble_hs_sched_start();            /* runs ble_hs_start() + sync ON THE HOST TASK (its parent task) */
    m_hostStarted = true;
    /* wait for sync (the host resets the controller and reads its state) */
    for (uint32_t t0 = timer_ms(); !m_synced && timer_ms() - t0 < 5000u; ) vTaskDelay(pdMS_TO_TICKS(10));
    if (!m_synced) blog("host did not sync within 5 s");
    return m_synced;
}

static SemaphoreHandle_t s_stopSem;
static void stop_cb(int status, void *arg) { (void)status; (void)arg; if (s_stopSem) xSemaphoreGive(s_stopSem); }
static struct ble_hs_stop_listener s_stopListener;

void BLEDevice::deinit(bool)
{
    if (!m_initialized) return;
    if (m_bleAdvertising) m_bleAdvertising->stop();
    if (m_hostStarted) {
        if (!s_stopSem) s_stopSem = xSemaphoreCreateBinary();
        int rc = ble_hs_stop(&s_stopListener, stop_cb, NULL);
        if (rc == 0 && s_stopSem) xSemaphoreTake(s_stopSem, pdMS_TO_TICKS(3000));
        ble_gatts_reset();
        m_hostStarted = false;
        m_synced = false;
    }
    if (m_pServer) {
        for (auto *s : m_pServer->m_services) { for (auto *c : s->m_chars) delete c; delete s; }
        delete m_pServer; m_pServer = nullptr;
    }
    delete m_bleAdvertising; m_bleAdvertising = nullptr;
    m_initialized = false;
    blog("deinitialised");
}

BLEServer *BLEDevice::createServer()
{
    if (!m_pServer) m_pServer = new BLEServer();
    return m_pServer;
}
BLEAdvertising *BLEDevice::getAdvertising()
{
    if (!m_bleAdvertising) m_bleAdvertising = new BLEAdvertising();
    return m_bleAdvertising;
}
void BLEDevice::startAdvertising()
{
    if (!m_initialized) return;
    if (!m_hostStarted) { startHost(); return; }        /* onSync starts advertising */
    getAdvertising()->start();
}
void BLEDevice::stopAdvertising() { if (m_bleAdvertising) m_bleAdvertising->stop(); }
BLEAddress BLEDevice::getAddress()
{
    uint8_t a[6] = { 0 }; int isnrpa = 0;
    ble_hs_id_copy_addr(m_ownAddrType == BLE_OWN_ADDR_RANDOM ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC, a, &isnrpa);
    return BLEAddress(a);
}
int BLEDevice::setMTU(uint16_t mtu) { m_localMTU = mtu; return ble_att_set_preferred_mtu(mtu); }

extern "C" int owf_ble_controller_enabled(void) { return BLEDevice::getInitialized() ? 1 : 0; }
