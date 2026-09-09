/* BLEDevice.h — the Arduino-ESP32 BLE API surface OpenWatchFace uses, on the
 * upstream NimBLE host (third_party/nimble) over HCI-on-SMD (platform/bt_hci.c).
 *
 * This is NOT the ESP library: it is a small re-implementation of the classes
 * ble_provision.h / ble_gadgetbridge.h touch (server, service, characteristic,
 * advertising, security, device). The ANCS/AMS clients call NimBLE directly and
 * are served by the same host. Signatures mirror Arduino-ESP32 3.x (NimBLE mode)
 * so the app compiles unchanged. BLEServer.h / BLESecurity.h include this file. */
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <vector>
extern "C" {
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

#define CONFIG_NIMBLE_ENABLED 1
#define CONFIG_BT_NIMBLE_ENABLED 1
#define BLE_SM_DEFAULT_PASSKEY 123456

/* ESP IO-capability names the app uses */
#define ESP_IO_CAP_OUT    BLE_HS_IO_DISPLAY_ONLY
#define ESP_IO_CAP_IO     BLE_HS_IO_DISPLAY_YESNO
#define ESP_IO_CAP_IN     BLE_HS_IO_KEYBOARD_ONLY
#define ESP_IO_CAP_NONE   BLE_HS_IO_NO_INPUT_OUTPUT
#define ESP_IO_CAP_KBDISP BLE_HS_IO_KEYBOARD_DISPLAY

class BLEServer; class BLEService; class BLECharacteristic; class BLEAdvertising;

/* ---------------------------------------------------------------- UUID */
class BLEUUID {
public:
    BLEUUID();
    BLEUUID(const char *s);
    BLEUUID(const String &s) : BLEUUID(s.c_str()) {}
    BLEUUID(uint16_t u);
    BLEUUID(uint32_t u);
    BLEUUID(const ble_uuid_any_t &u) : m_uuid(u), m_valid(true) {}
    bool equals(const BLEUUID &o) const { return m_valid && o.m_valid && ble_uuid_cmp(&m_uuid.u, &o.m_uuid.u) == 0; }
    bool operator==(const BLEUUID &o) const { return equals(o); }
    const ble_uuid_any_t *getNative() const { return &m_uuid; }
    String toString() const;
    uint8_t bitSize() const { return m_uuid.u.type; }
private:
    ble_uuid_any_t m_uuid;
    bool m_valid;
};

class BLEAddress {
public:
    BLEAddress() { memset(m_addr, 0, 6); }
    BLEAddress(const uint8_t *a) { memcpy(m_addr, a, 6); }
    BLEAddress(const ble_addr_t &a) { memcpy(m_addr, a.val, 6); }
    String toString() const;
    const uint8_t *getNative() const { return m_addr; }
private:
    uint8_t m_addr[6];
};

/* ------------------------------------------------------ characteristic */
class BLECharacteristicCallbacks {
public:
    virtual ~BLECharacteristicCallbacks() {}
    virtual void onRead(BLECharacteristic *) {}
    virtual void onWrite(BLECharacteristic *) {}
    virtual void onNotify(BLECharacteristic *) {}
    virtual void onStatus(BLECharacteristic *, int, uint32_t) {}
};

class BLEDescriptor {
public:
    BLEDescriptor(BLEUUID uuid, uint16_t maxLen = 100) : m_uuid(uuid) { (void)maxLen; }
    BLEDescriptor(const char *uuid, uint16_t maxLen = 100) : m_uuid(uuid) { (void)maxLen; }
    void setValue(const uint8_t *d, size_t n) { m_value.assign(d, d + n); }
    void setValue(const String &s) { setValue((const uint8_t *)s.c_str(), s.length()); }
    BLEUUID getUUID() const { return m_uuid; }
    std::vector<uint8_t> m_value;
    uint16_t m_handle = 0;
private:
    BLEUUID m_uuid;
    friend class BLEService;
};

class BLECharacteristic {
public:
    static const uint32_t PROPERTY_READ         = BLE_GATT_CHR_F_READ;
    static const uint32_t PROPERTY_READ_ENC     = BLE_GATT_CHR_F_READ_ENC;
    static const uint32_t PROPERTY_READ_AUTHEN  = BLE_GATT_CHR_F_READ_AUTHEN;
    static const uint32_t PROPERTY_READ_AUTHOR  = BLE_GATT_CHR_F_READ_AUTHOR;
    static const uint32_t PROPERTY_WRITE        = BLE_GATT_CHR_F_WRITE;
    static const uint32_t PROPERTY_WRITE_NR     = BLE_GATT_CHR_F_WRITE_NO_RSP;
    static const uint32_t PROPERTY_WRITE_ENC    = BLE_GATT_CHR_F_WRITE_ENC;
    static const uint32_t PROPERTY_WRITE_AUTHEN = BLE_GATT_CHR_F_WRITE_AUTHEN;
    static const uint32_t PROPERTY_WRITE_AUTHOR = BLE_GATT_CHR_F_WRITE_AUTHOR;
    static const uint32_t PROPERTY_NOTIFY       = BLE_GATT_CHR_F_NOTIFY;
    static const uint32_t PROPERTY_BROADCAST    = BLE_GATT_CHR_F_BROADCAST;
    static const uint32_t PROPERTY_INDICATE     = BLE_GATT_CHR_F_INDICATE;

    BLECharacteristic(BLEUUID uuid, uint32_t properties);
    BLECharacteristic(const char *uuid, uint32_t properties) : BLECharacteristic(BLEUUID(uuid), properties) {}
    void addDescriptor(BLEDescriptor *d) { m_descs.push_back(d); }
    BLEUUID getUUID() const { return m_uuid; }
    String getValue() const;
    const uint8_t *getData() const { return m_value.data(); }
    size_t getLength() const { return m_value.size(); }
    uint16_t getHandle() const { return m_handle; }
    void setValue(const uint8_t *data, size_t size);
    void setValue(const String &s) { setValue((const uint8_t *)s.c_str(), s.length()); }
    void setValue(const char *s) { setValue((const uint8_t *)s, strlen(s)); }
    void setValue(uint16_t v) { setValue((const uint8_t *)&v, 2); }
    void setValue(uint32_t v) { setValue((const uint8_t *)&v, 4); }
    void setValue(int v) { setValue((const uint8_t *)&v, 4); }
    void notify(bool is_notification = true);
    void indicate() { notify(false); }
    void setCallbacks(BLECharacteristicCallbacks *cb) { m_cb = cb; }
    BLECharacteristicCallbacks *getCallbacks() { return m_cb; }
    uint32_t getProperties() const { return m_props; }

    /* internal */
    int access(uint16_t conn, struct ble_gatt_access_ctxt *ctxt);
    BLEUUID m_uuid;
    uint32_t m_props;
    std::vector<uint8_t> m_value;
    std::vector<BLEDescriptor *> m_descs;
    BLECharacteristicCallbacks *m_cb = nullptr;
    uint16_t m_handle = 0;
    uint16_t m_subs = 0;      /* bitmask of conn handles subscribed (notify or indicate) */
    BLEService *m_service = nullptr;
};

/* ------------------------------------------------------------- service */
class BLEService {
public:
    BLEService(BLEUUID uuid, BLEServer *srv) : m_uuid(uuid), m_server(srv) {}
    BLECharacteristic *createCharacteristic(BLEUUID uuid, uint32_t properties);
    BLECharacteristic *createCharacteristic(const char *uuid, uint32_t properties) { return createCharacteristic(BLEUUID(uuid), properties); }
    void addCharacteristic(BLECharacteristic *c);
    BLECharacteristic *getCharacteristic(BLEUUID uuid);
    BLECharacteristic *getCharacteristic(const char *uuid) { return getCharacteristic(BLEUUID(uuid)); }
    BLEUUID getUUID() const { return m_uuid; }
    BLEServer *getServer() { return m_server; }
    bool start();
    void stop() {}
    uint16_t getHandle() const { return m_handle; }

    /* internal: builds the NimBLE table (kept alive here) */
    void buildDef();
    BLEUUID m_uuid;
    BLEServer *m_server;
    std::vector<BLECharacteristic *> m_chars;
    std::vector<struct ble_gatt_chr_def> m_chrDefs;
    std::vector<std::vector<struct ble_gatt_dsc_def>> m_dscDefs;
    struct ble_gatt_svc_def m_svcDef[2];
    uint16_t m_handle = 0;
    bool m_started = false;
};

/* -------------------------------------------------------------- server */
class BLEServerCallbacks {
public:
    virtual ~BLEServerCallbacks() {}
    virtual void onConnect(BLEServer *) {}
    virtual void onConnect(BLEServer *s, struct ble_gap_conn_desc *) { onConnect(s); }
    virtual void onDisconnect(BLEServer *) {}
    virtual void onDisconnect(BLEServer *s, struct ble_gap_conn_desc *) { onDisconnect(s); }
    virtual void onMtuChanged(BLEServer *, struct ble_gap_conn_desc *, uint16_t) {}
};

class BLEServer {
public:
    BLEServer();
    BLEService *createService(BLEUUID uuid, uint32_t numHandles = 15, uint8_t inst_id = 0);
    BLEService *createService(const char *uuid) { return createService(BLEUUID(uuid)); }
    BLEService *getServiceByUUID(BLEUUID uuid);
    BLEService *getServiceByUUID(const char *uuid) { return getServiceByUUID(BLEUUID(uuid)); }
    BLEAdvertising *getAdvertising();
    void setCallbacks(BLEServerCallbacks *cb) { m_cb = cb; }
    void startAdvertising();
    void start();
    bool isStarted() const { return m_started; }
    uint32_t getConnectedCount() const { return m_connCount; }
    uint16_t getConnId() const { return m_connId; }
    int disconnect(uint16_t connId, uint8_t reason = BLE_ERR_REM_USER_CONN_TERM);
    void updateConnParams(uint16_t conn, uint16_t minItvl, uint16_t maxItvl, uint16_t latency, uint16_t timeout);
    uint16_t getPeerMTU(uint16_t conn);
    void advertiseOnDisconnect(bool en) { m_advOnDisc = en; }

    /* internal */
    static int gapEvent(struct ble_gap_event *ev, void *arg);
    void registerServices();
    std::vector<BLEService *> m_services;
    BLEServerCallbacks *m_cb = nullptr;
    uint16_t m_connId = BLE_HS_CONN_HANDLE_NONE;
    uint32_t m_connCount = 0;
    bool m_started = false;
    bool m_advOnDisc = false;
};

/* --------------------------------------------------------- advertising */
class BLEAdvertisementData {
public:
    void setName(String n) { m_name = n; }
    void setFlags(uint8_t f) { m_flags = f; }
    String m_name; uint8_t m_flags = 0;
};

class BLEAdvertising {
public:
    void addServiceUUID(BLEUUID uuid) { m_uuids.push_back(uuid); }
    void addServiceUUID(const char *uuid) { addServiceUUID(BLEUUID(uuid)); }
    bool removeServiceUUID(BLEUUID uuid);
    void setScanResponse(bool b) { m_scanRsp = b; }
    void setName(String n) { m_name = n; }
    void setAppearance(uint16_t a) { m_appearance = a; }
    void setMinInterval(uint16_t v) { m_minItvl = v; }
    void setMaxInterval(uint16_t v) { m_maxItvl = v; }
    void setMinPreferred(uint16_t v) { m_minPref = v; }
    void setMaxPreferred(uint16_t v) { m_maxPref = v; }
    void setAdvertisementType(uint8_t) {}
    void addTxPower() { m_txPower = true; }
    bool setAdvertisementData(BLEAdvertisementData &) { return true; }
    bool setScanResponseData(BLEAdvertisementData &) { return true; }
    bool start(uint32_t duration = 0, void (*cb)(BLEAdvertising *) = nullptr);
    bool stop();
    bool isAdvertising();
    void reset() { m_uuids.clear(); }

    /* internal */
    std::vector<BLEUUID> m_uuids;
    String m_name;
    bool m_scanRsp = false, m_txPower = false;
    uint16_t m_appearance = 0, m_minItvl = 0, m_maxItvl = 0, m_minPref = 0, m_maxPref = 0;
};

/* ------------------------------------------------------------ security */
class BLESecurityCallbacks {
public:
    virtual ~BLESecurityCallbacks() {}
    virtual uint32_t onPassKeyRequest() { return BLE_SM_DEFAULT_PASSKEY; }
    virtual void onPassKeyNotify(uint32_t) {}
    virtual bool onSecurityRequest() { return true; }
    virtual bool onConfirmPIN(uint32_t) { return true; }
    virtual bool onAuthorizationRequest(uint16_t, uint16_t, bool) { return true; }
    virtual void onAuthenticationComplete(ble_gap_conn_desc *) {}
};

class BLESecurity {
public:
    static void setAuthenticationMode(bool bonding, bool mitm, bool sc);
    static void setAuthenticationMode(uint8_t auth_req);
    static void setCapability(uint8_t iocap);
    static void setInitEncryptionKey(uint8_t k = 3);
    static void setRespEncryptionKey(uint8_t k = 3);
    static void setKeySize(uint8_t = 16) {}
    static void setPassKey(bool staticPasskey = false, uint32_t passkey = BLE_SM_DEFAULT_PASSKEY);
    static void regenPassKeyOnConnect(bool en = false) { m_regen = en; }
    static void resetSecurity();
    static uint32_t getPassKey() { return m_passkey; }
    static bool startSecurity(uint16_t connHandle, int *rcPtr = nullptr);
    static void setForceAuthentication(bool force) { m_forceSecurity = force; }
    static bool getForceAuthentication() { return m_forceSecurity; }

    /* internal */
    static uint32_t nextPasskey();
    static bool m_staticPasskey, m_regen, m_passkeySet, m_forceSecurity, m_securityEnabled;
    static uint32_t m_passkey;
};

/* -------------------------------------------------------------- device */
typedef int (*gap_event_handler)(struct ble_gap_event *event, void *arg);

class BLEDevice {
public:
    static bool init(String deviceName = "");
    static void deinit(bool release_memory = false);
    static bool getInitialized() { return m_initialized; }
    static BLEServer *createServer();
    static BLEServer *getServer() { return m_pServer; }
    static BLEAdvertising *getAdvertising();
    static void startAdvertising();
    static void stopAdvertising();
    static void setSecurityCallbacks(BLESecurityCallbacks *cb) { m_securityCallbacks = cb; }
    static BLESecurityCallbacks *getSecurityCallbacks() { return m_securityCallbacks; }
    static void setCustomGapHandler(gap_event_handler h) { m_customGapHandler = h; }
    static gap_event_handler getCustomGapHandler() { return m_customGapHandler; }
    static BLEAddress getAddress();
    static String getDeviceName() { return m_deviceName; }
    static int setMTU(uint16_t mtu);
    static uint16_t getMTU() { return m_localMTU; }
    static bool isHostedBLE() { return false; }

    /* internal */
    static void onSync();
    static void onReset(int reason);
    static bool startHost();
    static void ensureAddr();
    static bool m_initialized, m_hostStarted, m_synced;
    static BLEServer *m_pServer;
    static BLEAdvertising *m_bleAdvertising;
    static BLESecurityCallbacks *m_securityCallbacks;
    static gap_event_handler m_customGapHandler;
    static String m_deviceName;
    static uint16_t m_localMTU;
    static uint8_t m_ownAddrType;
};

extern "C" int owf_ble_controller_enabled(void);
