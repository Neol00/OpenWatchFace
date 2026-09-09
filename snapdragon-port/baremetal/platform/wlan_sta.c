/* wlan_sta.c — station side: authenticate, associate, WPA2-PSK 4-way
 * handshake, install the keys (step 7 of WIFI-BRINGUP.md).
 *
 * The sequence is the one mainline's wcn36xx + mac80211 + wpa_supplicant
 * perform, collapsed into one polled state machine:
 *   ADD_STA_SELF                    (add_interface)      -> self sta/dpu index
 *   SET_LINK_ST(PREASSOC) + JOIN + CONFIG_BSS(no sta)    (bss_info_changed: BSSID)
 *   TX Authentication(open, seq 1)  -> RX seq 2 status 0
 *   TX Association Request (+RSN IE) -> RX Response      -> AID
 *   SET_LINK_ST(POSTASSOC) + CONFIG_BSS(sta, update) + CONFIG_STA  (assoc)
 *   RX EAPOL 1/4 -> TX 2/4 -> RX 3/4 (MIC, GTK unwrap) -> TX 4/4
 *   CONFIG_BSS(update) + CONFIG_STA + SET_STAKEY(CCMP TK) + SET_BSSKEY(GTK)
 * Frames go out through the DXE mgmt ring (auth/assoc) and data ring
 * (EAPOL); replies arrive through the RX ring handler.
 * HAL message layouts: hal.h v6.6, config_bss/config_sta in their V1 form
 * (fw 1.5.x is newer than 1.2.2.24) truncated by the no-VHT deltas, as
 * wcn36xx_smd_config_bss_v1() does for a WCN3620. */
#include "platform.h"
#if defined(PLAT_WCNSS_FW_BASE) && defined(PLAT_SMEM_BASE)
#include <string.h>

#if defined(LOG_VERBOSE)
#define vsay(s)        say(s)
#define vsay_hex(s, v) say_hex(s, v)
#define vsay_dec(s, v) say_dec(s, v)
#else
#define vsay(s)        ((void)(s))
#define vsay_hex(s, v) ((void)(s), (void)(v))
#define vsay_dec(s, v) ((void)(s), (void)(v))
#endif
static void say(const char *s) { con_puts(s); con_flush(); usb_poll(); }
static void say_hex(const char *s, uint32_t v) { con_puts(s); con_puthex(v); con_flush(); usb_poll(); }

/* ---- HAL ids / enums ----------------------------------------------------- */
#define HAL_CONFIG_STA_REQ    12u
#define HAL_CONFIG_STA_RSP    13u
#define HAL_CONFIG_BSS_REQ    16u
#define HAL_CONFIG_BSS_RSP    17u
#define HAL_DELETE_BSS_REQ    18u
#define HAL_DELETE_BSS_RSP    19u
#define HAL_JOIN_REQ          20u
#define HAL_JOIN_RSP          21u
#define HAL_SET_BSSKEY_REQ    24u
#define HAL_SET_BSSKEY_RSP    25u
#define HAL_SET_STAKEY_REQ    26u
#define HAL_SET_STAKEY_RSP    27u
#define HAL_SET_LINK_ST_REQ   44u
#define HAL_SET_LINK_ST_RSP   45u
#define HAL_ADD_STA_SELF_REQ  125u
#define HAL_ADD_STA_SELF_RSP  126u
#define HAL_DEL_STA_SELF_REQ  127u
#define HAL_DEL_STA_SELF_RSP  128u
#define LINK_IDLE 0u
#define LINK_PREASSOC 1u
#define LINK_POSTASSOC 2u
#define ED_NONE 0u
#define ED_CCMP 4u
#define BSS_INFRASTRUCTURE 0u
#define NW_11G 2u
#define NW_11N 3u
#define STA_11n 6u
#define PERSONA_STA 0u

/* ---- state --------------------------------------------------------------- */
static uint32_t s_buf[512];
static uint8_t  s_bssid[6], s_self[6], s_ssid[33]; static uint32_t s_ssid_len;
static uint8_t  s_chan;
static uint8_t  s_self_sta, s_self_dpu, s_bss_index = 0xFF, s_bss_sta, s_bss_dpu, s_ucast_sign;
static uint8_t  s_peer_sta, s_peer_dpu, s_peer_sign;
static uint32_t s_tx_data, s_rx_data, s_rx_data_other;
void wlan_sta_counters(uint32_t *tx, uint32_t *rx, uint32_t *rx_other) { *tx = s_tx_data; *rx = s_rx_data; *rx_other = s_rx_data_other; }
static uint16_t s_aid, s_seq;
static int8_t   s_rssi;
static int      s_connected, s_self_added;
static uint8_t  s_pmk[32], s_ptk[48], s_anonce[32], s_snonce[32], s_replay[8], s_gtk[32], s_gtk_len, s_gtk_id;
static uint8_t  s_rsn_ie[22];
static volatile int s_ev;                 /* last RX event: 1 auth ok, 2 assoc ok, 3 eapol1, 4 eapol3, -1 fail */
static uint16_t s_ev_status;

/* ---- byte cursor for packed HAL messages -------------------------------- */
struct cur { uint8_t *p; uint32_t n; };
static void c8(struct cur *c, uint32_t v)  { c->p[c->n++] = (uint8_t)v; }
static void c16(struct cur *c, uint32_t v) { c8(c, v); c8(c, v >> 8); }
static void c32(struct cur *c, uint32_t v) { c16(c, v & 0xFFFFu); c16(c, v >> 16); }
static void cmem(struct cur *c, const void *s, uint32_t n) { memcpy(c->p + c->n, s, n); c->n += n; }
static void czero(struct cur *c, uint32_t n) { memset(c->p + c->n, 0, n); c->n += n; }
static void hal_hdr(struct cur *c, uint32_t type) { c->n = 0; c16(c, type); c16(c, 0); c32(c, 0); }
static void hal_fin(struct cur *c) { uint32_t l = c->n; c->p[4] = (uint8_t)l; c->p[5] = (uint8_t)(l >> 8); c->p[6] = 0; c->p[7] = 0; }

static int hal_ok(const char *what, uint32_t rsp_type, uint32_t len, uint32_t min)
{
    uint32_t got = wlan_hal_xfer(s_buf, len, rsp_type, s_buf, sizeof s_buf, 3000u);
    vsay(what);
    if (got < min) { say(": no reply\n"); return -1; }
    if (s_buf[2] != 0u) { vsay_hex(": status ", s_buf[2]); vsay("\n"); return -1; }
    vsay(" ok\n");
    return 0;
}

/* ---- HAL requests -------------------------------------------------------- */
static int hal_add_sta_self(void)
{
    struct cur c = { (uint8_t *)s_buf, 0 };
    if (s_self_added) { vsay("sta: self station already registered\n"); return 0; }
    hal_hdr(&c, HAL_ADD_STA_SELF_REQ); cmem(&c, s_self, 6); c32(&c, 0); hal_fin(&c);
    if (hal_ok("sta: ADD_STA_SELF", HAL_ADD_STA_SELF_RSP, c.n, 15) < 0) return -1;
    s_self_sta = ((uint8_t *)s_buf)[12]; s_self_dpu = ((uint8_t *)s_buf)[13];
    s_self_added = 1;
    return 0;
}
static void hal_del_sta_self(void)
{
    struct cur c = { (uint8_t *)s_buf, 0 };
    if (!s_self_added) return;
    hal_hdr(&c, HAL_DEL_STA_SELF_REQ); cmem(&c, s_self, 6); hal_fin(&c);
    hal_ok("sta: DEL_STA_SELF", HAL_DEL_STA_SELF_RSP, c.n, 12);
    s_self_added = 0;
}
void wlan_sta_reset(void) { s_self_added = 0; s_connected = 0; s_bss_index = 0xFF; }
static int hal_set_link(uint32_t state)
{
    struct cur c = { (uint8_t *)s_buf, 0 };
    hal_hdr(&c, HAL_SET_LINK_ST_REQ); cmem(&c, s_bssid, 6); c32(&c, state); cmem(&c, s_self, 6); hal_fin(&c);
    return hal_ok(state == LINK_PREASSOC ? "sta: SET_LINK_ST preassoc" : state == LINK_POSTASSOC ? "sta: SET_LINK_ST postassoc" : "sta: SET_LINK_ST idle",
                  HAL_SET_LINK_ST_RSP, c.n, 12);
}
static int hal_join(void)
{
    struct cur c = { (uint8_t *)s_buf, 0 };
    hal_hdr(&c, HAL_JOIN_REQ); cmem(&c, s_bssid, 6); c8(&c, s_chan); cmem(&c, s_self, 6);
    c8(&c, 0); c32(&c, 0 /* single channel centred */); c32(&c, LINK_PREASSOC); c8(&c, 0xBF); hal_fin(&c);
    return hal_ok("sta: JOIN", HAL_JOIN_RSP, c.n, 12);
}

/* config_sta_params_v1 (no-VHT truncation applied by the caller) */
static void put_sta_params(struct cur *c, int with_peer)
{
    static const uint16_t dsss[4] = { 0x02, 0x04, 0x0b, 0x16 };
    static const uint16_t ofdm[8] = { 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };
    uint32_t i;
    cmem(c, s_bssid, 6);                       /* bssid (STA: the peer AP) */
    c16(c, with_peer ? s_aid : 0);             /* aid */
    c8(c, 0);                                  /* type: 0 = STA */
    c8(c, 1);                                  /* short preamble */
    cmem(c, s_self, 6);                        /* mac */
    c16(c, 1);                                 /* listen interval */
    c8(c, 0);                                  /* wmm */
    c8(c, 1); c8(c, 1); c8(c, 0); c8(c, 1);    /* ht_capable, tx_channel_width, rifs, lsig_txop */
    c8(c, 3); c8(c, 5); c8(c, 0);              /* max_ampdu_size, density, amsdu */
    c8(c, 1); c8(c, 1);                        /* sgi40, sgi20 */
    c8(c, 0);                                  /* rmf */
    c32(c, s_connected ? ED_CCMP : ED_NONE);   /* encrypt_type */
    c8(c, 0); c8(c, 0); c8(c, 0); c8(c, 1);    /* action, uapsd, max_sp_len, green_field */
    c32(c, 0);                                 /* mimo_ps static */
    c8(c, 0); c8(c, 0); c8(c, 1);              /* delayed_ba, max_ampdu_duration, dsss_cck_40 */
    c8(c, s_self_sta);                         /* sta_index (self) */
    c8(c, s_bss_index);                        /* bssid_index */
    c8(c, 0);                                  /* p2p */
    c8(c, 0);                                  /* ldpc bits */
    /* supported_rates_v1 (66 bytes) */
    c32(c, STA_11n);
    for (i = 0; i < 4u; i++) c16(c, dsss[i]);
    for (i = 0; i < 8u; i++) c16(c, ofdm[i]);
    czero(c, 6); c16(c, 0); c32(c, 0);
    c8(c, 0xFF); czero(c, 15);                 /* mcs set: MCS 0-7 */
    c16(c, 0); czero(c, 8);                    /* rx_highest, vht mcs maps */
    c8(c, 0); c8(c, 0);                        /* vht_capable, vht width (truncated off) */
}

static int hal_config_bss(int update)
{
    struct cur c = { (uint8_t *)s_buf, 0 };
    hal_hdr(&c, HAL_CONFIG_BSS_REQ);
    cmem(&c, s_bssid, 6); cmem(&c, s_self, 6);
    c32(&c, BSS_INFRASTRUCTURE); c8(&c, 1 /* oper_mode STA */); c32(&c, NW_11G);
    c8(&c, 1);                                 /* short slot */
    c8(&c, 0); c8(&c, 0); c8(&c, 0); c8(&c, 0); c8(&c, 0); c8(&c, 0); c8(&c, 0);   /* coexist..., lsig full, rifs */
    c16(&c, 100); c8(&c, 1);                   /* beacon interval, dtim */
    c8(&c, 0); c8(&c, s_chan); c8(&c, 0); c8(&c, 0);   /* tx width, oper ch, ext ch, reserved */
    c8(&c, (uint8_t)s_ssid_len); cmem(&c, s_ssid, s_ssid_len); czero(&c, 32u - s_ssid_len);   /* ssid */
    c8(&c, (uint8_t)update);                   /* action */
    c8(&c, 0); czero(&c, 12);                  /* rateset */
    c8(&c, 0); c8(&c, 0); c8(&c, 0);           /* ht, obss, rmf */
    c32(&c, 0);                                /* ht oper mode */
    c8(&c, 0); c8(&c, 0); c8(&c, 0); c8(&c, 0); c8(&c, 0);   /* dual cts, probe retry, hidden, proxy, edca valid */
    czero(&c, 16);                             /* 4 edca records */
    c8(&c, 0); czero(&c, 240);                 /* ext sta key valid + params */
    c8(&c, PERSONA_STA); c8(&c, 0); c8(&c, 0); c8(&c, 0x14);   /* persona, spectrum, tx_mgmt_power, max_tx_power */
    put_sta_params(&c, update);
    c8(&c, 0); c8(&c, 0);                      /* bss vht (truncated off) */
    hal_fin(&c);
    c.n -= 12u; hal_fin(&c);                   /* WCN36XX_DIFF_BSS_PARAMS_V1_NOVHT */
    if (hal_ok(update ? "sta: CONFIG_BSS update" : "sta: CONFIG_BSS", HAL_CONFIG_BSS_RSP, c.n, 29) < 0) return -1;
    {
        const uint8_t *r = (const uint8_t *)s_buf + 12;
        s_bss_index = r[0]; s_bss_dpu = r[1]; s_ucast_sign = r[2]; s_bss_sta = r[7];
        vsay_hex("sta:   bss_index ", s_bss_index); vsay_hex(" bss_sta ", s_bss_sta); vsay_hex(" dpu ", s_bss_dpu);
        vsay_hex(" sign ", s_ucast_sign); vsay_hex(" self_sta ", r[8]); vsay_hex(" bcast_sta ", r[9]); vsay("\n");
    }
    return 0;
}

static int hal_config_sta(void)
{
    struct cur c = { (uint8_t *)s_buf, 0 };
    hal_hdr(&c, HAL_CONFIG_STA_REQ);
    put_sta_params(&c, 1);
    hal_fin(&c); c.n -= 10u; hal_fin(&c);      /* WCN36XX_DIFF_STA_PARAMS_V1_NOVHT */
    if (hal_ok("sta: CONFIG_STA", HAL_CONFIG_STA_RSP, c.n, 21) < 0) return -1;
    s_peer_sta = ((uint8_t *)s_buf)[12]; s_peer_dpu = ((uint8_t *)s_buf)[14]; s_peer_sign = ((uint8_t *)s_buf)[18];
    vsay_hex("sta:   peer sta_index ", s_peer_sta); vsay_hex(" dpu ", s_peer_dpu); vsay_hex(" ucast sign ", s_peer_sign); vsay("\n");
    return 0;
}

static int hal_set_stakey(const uint8_t *tk)
{
    struct cur c = { (uint8_t *)s_buf, 0 };
    hal_hdr(&c, HAL_SET_STAKEY_REQ);
    c16(&c, s_bss_sta); c32(&c, ED_CCMP); c32(&c, 0); c8(&c, 0);
    /* key[0] */ c8(&c, 0); c8(&c, 1); c32(&c, 2 /* TX_RX */); czero(&c, 16); c8(&c, 0); c16(&c, 16); cmem(&c, tk, 16); czero(&c, 16);
    czero(&c, 3u * 57u);
    c8(&c, 1);                                 /* single_tid_rc */
    hal_fin(&c);
    return hal_ok("sta: SET_STAKEY (PTK)", HAL_SET_STAKEY_RSP, c.n, 12);
}
static int hal_set_bsskey(void)
{
    struct cur c = { (uint8_t *)s_buf, 0 };
    hal_hdr(&c, HAL_SET_BSSKEY_REQ);
    c8(&c, s_bss_index); c32(&c, ED_CCMP); c8(&c, 1);
    c8(&c, s_gtk_id); c8(&c, 0); c32(&c, 1 /* RX_ONLY */); czero(&c, 16); c8(&c, 0); c16(&c, s_gtk_len); cmem(&c, s_gtk, s_gtk_len); czero(&c, 32u - s_gtk_len);
    czero(&c, 3u * 57u);
    c8(&c, 0);
    hal_fin(&c);
    return hal_ok("sta: SET_BSSKEY (GTK)", HAL_SET_BSSKEY_RSP, c.n, 12);
}

/* ---- frames -------------------------------------------------------------- */
static uint8_t s_frame[1600];
static uint8_t s_eth[1600];
static void (*s_data_rx)(const uint8_t *eth, uint32_t len);
void wlan_sta_set_data_rx(void (*fn)(const uint8_t *eth, uint32_t len)) { s_data_rx = fn; }

static void bd_build(uint8_t bd[40], uint32_t hdr_len, uint32_t len, int mgmt, int encrypt)
{
    uint32_t w[10], i;
    memset(w, 0, sizeof w);
    /* word0: dpu_ne bit3, dpu_sign bits 21-23, dpu_rf bits 24-31 (BMU_WQ_TX = 25) */
    /* data: the DPU signature is the STATION's (config_sta rsp uc_ucast_sig,
     * mainline sta_priv->ucast_dpu_sign), not the BSS's -- v21 used the BSS
     * one and every encrypted frame was dropped by the DPU */
    w[0] = (encrypt ? 0u : (1u << 3)) | ((uint32_t)(mgmt ? 0u : s_peer_sign) << 21) | (25u << 24);
    /* pdu word3: mpdu_data_off bits 7-15, mpdu_header_off 16-23, mpdu_header_len 24-31 */
    w[3] = ((40u + hdr_len) << 7) | (40u << 16) | (hdr_len << 24);
    /* pdu word4: tid bits 8-11, bd_ssn bits 12-13 (1 = fill DPU non-QoS), mpdu_len 16-31 */
    w[4] = ((mgmt ? 7u : 0u) << 8) | (1u << 12) | (len << 16);
    /* word5: queue_id bits 7-11 (9 = unicast), bd_rate 12-13 (2 mgmt / 0 data), sta_index 16-23, dpu_desc_idx 24-31 */
    w[5] = (9u << 7) | ((mgmt ? 2u : 0u) << 12) | ((uint32_t)(mgmt ? s_self_sta : s_bss_sta) << 16) | ((uint32_t)(mgmt ? s_self_dpu : s_bss_dpu) << 24);
    for (i = 0; i < 10u; i++) {                /* to big-endian, as buff_to_be() */
        uint32_t v = w[i], b = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
        memcpy(bd + 4u * i, &b, 4);
    }
    memset(bd + 24, 0xBD, 4);                  /* tx_bd_sign */
}

static int tx_mgmt(const uint8_t *body, uint32_t blen, uint32_t fc)
{
    uint8_t bd[40]; uint8_t *f = s_frame;
    memset(f, 0, 24);
    f[0] = (uint8_t)fc; f[1] = (uint8_t)(fc >> 8);
    memcpy(f + 4, s_bssid, 6); memcpy(f + 10, s_self, 6); memcpy(f + 16, s_bssid, 6);
    f[22] = (uint8_t)(s_seq << 4); f[23] = (uint8_t)(s_seq >> 4); s_seq++;
    memcpy(f + 24, body, blen);
    bd_build(bd, 24, 24 + blen, 1, 0);
    return wcn36xx_tx(bd, f, 24 + blen, 1);
}

static int tx_eapol(const uint8_t *eapol, uint32_t elen)
{
    static const uint8_t llc[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E };
    uint8_t bd[40]; uint8_t *f = s_frame;
    memset(f, 0, 24);
    f[0] = 0x08; f[1] = 0x01;                  /* data, ToDS */
    memcpy(f + 4, s_bssid, 6); memcpy(f + 10, s_self, 6); memcpy(f + 16, s_bssid, 6);
    f[22] = (uint8_t)(s_seq << 4); f[23] = (uint8_t)(s_seq >> 4); s_seq++;
    memcpy(f + 24, llc, 8); memcpy(f + 32, eapol, elen);
    bd_build(bd, 24, 32 + elen, 0, 0);
    return wcn36xx_tx(bd, f, 32 + elen, 0);
}

/* Ethernet II frame from the IP stack -> encrypted 802.11 data frame to the AP */
int wlan_sta_tx_eth(const uint8_t *eth, uint32_t len)
{
    uint8_t bd[40]; uint8_t *f = s_frame; uint32_t plen;
    if (!s_connected || len < 14u || len > 1514u) return -1;
    plen = len - 14u;
    memset(f, 0, 24);
    f[0] = 0x08; f[1] = 0x41;                  /* data, ToDS, PROTECTED (the DPU encrypts; the AP expects the bit) */
    memcpy(f + 4, s_bssid, 6); memcpy(f + 10, s_self, 6); memcpy(f + 16, eth, 6);   /* addr3 = destination */
    s_tx_data++;
    f[22] = (uint8_t)(s_seq << 4); f[23] = (uint8_t)(s_seq >> 4); s_seq++;
    f[24] = 0xAA; f[25] = 0xAA; f[26] = 0x03; f[27] = 0; f[28] = 0; f[29] = 0; f[30] = eth[12]; f[31] = eth[13];
    memcpy(f + 32, eth + 14, plen);
    bd_build(bd, 24, 32 + plen, 0, 1);         /* dpu_ne = 0: the DPU encrypts with the PTK */
    return wcn36xx_tx(bd, f, 32 + plen, 0);
}

/* Diagnostic (v21): is it the ENCRYPTED data path that wedges the TX ring?
 * Send a harmless data frame (ethertype 0x88B5, IEEE local experimental)
 * once without and once with the DPU encrypt flag, report both. */
int wlan_sta_tx_probe(void)
{
    static const uint8_t body[16] = { 0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0xB5, 'o', 'w', 'f', 0, 0, 0, 0, 0 };
    uint8_t bd[40]; uint8_t *f = s_frame; int r1, r2;
    if (!s_connected) return -1;
    vsay_hex("sta: tx probe: bss_sta ", s_bss_sta); vsay_hex(" bss_dpu ", s_bss_dpu); vsay_hex(" sign(sta) ", s_peer_sign);
    vsay_hex(" peer_sta ", s_peer_sta); vsay_hex(" peer_dpu ", s_peer_dpu); vsay("\n");
    memset(f, 0, 24); f[0] = 0x08; f[1] = 0x01;
    memcpy(f + 4, s_bssid, 6); memcpy(f + 10, s_self, 6); memcpy(f + 16, s_bssid, 6);
    f[22] = (uint8_t)(s_seq << 4); f[23] = (uint8_t)(s_seq >> 4); s_seq++;
    memcpy(f + 24, body, 16);
    bd_build(bd, 24, 40, 0, 0);
    say("sta: tx probe PLAIN ... "); r1 = wcn36xx_tx(bd, f, 40, 0); say(r1 == 0 ? "ok\n" : "STALLED\n");
    f[22] = (uint8_t)(s_seq << 4); f[23] = (uint8_t)(s_seq >> 4); s_seq++; f[1] = 0x41;
    bd_build(bd, 24, 40, 0, 1);
    say("sta: tx probe ENCRYPTED ... "); r2 = wcn36xx_tx(bd, f, 40, 0); say(r2 == 0 ? "ok\n" : "STALLED\n");
    return (r1 == 0 && r2 == 0) ? 0 : -1;
}

/* ---- EAPOL-Key ------------------------------------------------------------ */
#define KI_MIC     0x0100u
#define KI_SECURE  0x0200u
#define KI_ACK     0x0080u
#define KI_INSTALL 0x0040u
#define KI_PAIRWISE 0x0008u
#define KI_ENCDATA 0x1000u

static void eapol_mic(uint8_t *e, uint32_t elen)
{
    uint8_t mic[20];
    memset(e + 81, 0, 16);
    hmac_sha1(s_ptk, 16, e, elen, mic);        /* KCK = PTK[0..16], HMAC-SHA1-128 (desc version 2) */
    memcpy(e + 81, mic, 16);
}

/* 4 (802.1X hdr) + 95 (key frame fixed) + key data */
static int tx_eapol_msg(uint32_t key_info, const uint8_t *nonce, const uint8_t *kd, uint32_t kdlen, uint32_t key_len)
{
    static uint8_t e[4 + 95 + 64];
    uint32_t elen = 99u + kdlen;
    memset(e, 0, sizeof e);
    e[0] = 2; e[1] = 3; e[2] = (uint8_t)((95u + kdlen) >> 8); e[3] = (uint8_t)(95u + kdlen);
    e[4] = 2;                                  /* descriptor type RSN */
    e[5] = (uint8_t)(key_info >> 8); e[6] = (uint8_t)key_info;
    e[7] = (uint8_t)(key_len >> 8); e[8] = (uint8_t)key_len;
    memcpy(e + 9, s_replay, 8);
    if (nonce) memcpy(e + 17, nonce, 32);
    e[97] = (uint8_t)(kdlen >> 8); e[98] = (uint8_t)kdlen;
    if (kdlen) memcpy(e + 99, kd, kdlen);
    eapol_mic(e, elen);
    return tx_eapol(e, elen);
}

static void rx_eapol(const uint8_t *e, uint32_t elen)
{
    uint32_t key_info, kdlen; uint8_t mic[20], save[16];
    if (elen < 99u || e[1] != 3 || e[4] != 2) return;
    key_info = ((uint32_t)e[5] << 8) | e[6];
    kdlen = ((uint32_t)e[97] << 8) | e[98];
    if (99u + kdlen > elen) return;
    if ((key_info & (KI_PAIRWISE | KI_ACK | KI_MIC)) == (KI_PAIRWISE | KI_ACK)) {      /* message 1 */
        memcpy(s_anonce, e + 17, 32); memcpy(s_replay, e + 9, 8);
        s_ev = 3; return;
    }
    if ((key_info & (KI_PAIRWISE | KI_ACK | KI_MIC | KI_INSTALL)) == (KI_PAIRWISE | KI_ACK | KI_MIC | KI_INSTALL)) {   /* message 3 */
        uint8_t *m = (uint8_t *)e;
        memcpy(save, m + 81, 16); memset(m + 81, 0, 16);
        hmac_sha1(s_ptk, 16, m, elen, mic); memcpy(m + 81, save, 16);
        if (memcmp(mic, save, 16)) { say("sta: EAPOL 3/4 MIC mismatch (wrong password?)\n"); s_ev = -1; return; }
        memcpy(s_replay, e + 9, 8);
        if (key_info & KI_ENCDATA) {
            static uint8_t kd[256]; uint32_t n = kdlen - 8u, i = 0;
            if (kdlen > sizeof kd + 8u || aes_key_unwrap(s_ptk + 16, e + 99, kdlen, kd) < 0) { say("sta: GTK unwrap failed\n"); s_ev = -1; return; }
            s_gtk_len = 0;
            while (i + 2u <= n) {
                uint32_t l = kd[i + 1];
                if (kd[i] == 0xDD && l >= 6u && kd[i+2] == 0x00 && kd[i+3] == 0x0F && kd[i+4] == 0xAC && kd[i+5] == 0x01) {
                    s_gtk_id = kd[i + 6] & 3u; s_gtk_len = (uint8_t)(l - 6u); if (s_gtk_len > 32u) s_gtk_len = 32u;
                    memcpy(s_gtk, kd + i + 8, s_gtk_len);
                }
                if (kd[i] == 0xDD && l == 0u) break;
                i += 2u + l;
            }
        }
        s_ev = 4; return;
    }
}

/* ---- RX dispatch (called from the DXE poll) -------------------------------- */
static void rx_handler(const uint8_t *f, uint32_t len, int8_t rssi)
{
    uint32_t fc = f[0] | (f[1] << 8), type = fc & 0x0Cu, sub = fc & 0xF0u, hlen;
    if (len < 24u || memcmp(f + 10, s_bssid, 6)) return;      /* only from our AP */
    if (memcmp(f + 4, s_self, 6) && !(f[4] & 1u)) return;      /* to us, or broadcast/multicast */
    s_rssi = rssi;
    if (type == 0x00u) {                                        /* management */
        const uint8_t *b = f + 24;
        if (sub != 0x80u) {                                     /* everything but beacons: show it */
            con_dbg("  [rx mgmt sub "); con_dbg_hex(sub); con_dbg(" len "); con_dbg_dec(len);
            if (len >= 28u) { con_dbg(" b0-3 "); con_dbg_hex(b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24)); }
            con_dbg("] ");
        }
        if (sub == 0xB0u && len >= 30u) {                       /* authentication */
            uint32_t seq = b[2] | (b[3] << 8), st = b[4] | (b[5] << 8);
            if (seq == 2u) { s_ev_status = (uint16_t)st; s_ev = st == 0u ? 1 : -1; }
        } else if (sub == 0x10u && len >= 30u) {                /* association response */
            uint32_t st = b[2] | (b[3] << 8);
            s_aid = (uint16_t)((b[4] | (b[5] << 8)) & 0x3FFFu);
            s_ev_status = (uint16_t)st; s_ev = st == 0u ? 2 : -1;
        } else if (sub == 0xC0u || sub == 0xA0u) {              /* deauth / disassoc */
            say("sta: deauthenticated by the AP\n"); s_connected = 0; s_ev = -1;
        }
    } else if (type == 0x08u) {                                 /* data */
        if (sub & 0x40u) return;                                /* null / no-data subtypes */
        hlen = (sub & 0x80u) ? 26u : 24u;
        if (len < hlen + 8u) return;
        if (f[hlen + 6] == 0x88 && f[hlen + 7] == 0x8E) { if (len >= hlen + 12u) rx_eapol(f + hlen + 8u, len - hlen - 8u); return; }
        if (s_connected && !(f[hlen] == 0xAA && f[hlen + 1] == 0xAA)) {
            s_rx_data_other++;
            if (s_rx_data_other <= 4u) { con_dbg("  [rx data non-LLC len "); con_dbg_dec(len); con_dbg(" fc "); con_dbg_hex(fc); con_dbg(" b "); con_dbg_hex(f[hlen] | (f[hlen+1] << 8) | (f[hlen+2] << 16) | ((uint32_t)f[hlen+3] << 24)); con_dbg("]\n"); }
        }
        if (s_connected && s_data_rx && f[hlen] == 0xAA && f[hlen + 1] == 0xAA) {
            uint32_t plen = len - hlen - 8u;
            s_rx_data++;
            if (s_rx_data <= 8u) { con_dbg("  [rx data type "); con_dbg_hex((f[hlen + 6] << 8) | f[hlen + 7]); con_dbg(" len "); con_dbg_dec(len); con_dbg(" fc "); con_dbg_hex(fc); con_dbg("]\n"); }
            if (plen + 14u > sizeof s_eth) return;
            memcpy(s_eth, f + 4, 6);                            /* dst = addr1 */
            memcpy(s_eth + 6, f + 16, 6);                       /* src = addr3 (FromDS) */
            s_eth[12] = f[hlen + 6]; s_eth[13] = f[hlen + 7];
            memcpy(s_eth + 14, f + hlen + 8u, plen);
            s_data_rx(s_eth, plen + 14u);
        }
    }
}

static int wait_ev(int want, uint32_t ms)
{
    uint32_t t = timer_ms();
    while (timer_ms() - t < ms) {
        wcn36xx_rx_poll();
        if (s_ev == want) { s_ev = 0; return 0; }
        if (s_ev < 0) { s_ev = 0; return -1; }
        timer_delay_ms(1);
    }
    return -2;
}

static void make_nonce(uint8_t *n)
{
    struct sha1 c; uint8_t h[20]; uint64_t t = timer_ticks(); uint32_t i;
    for (i = 0; i < 32u; i += 20u) {
        sha1_init(&c); sha1_update(&c, &t, 8); sha1_update(&c, s_self, 6); sha1_update(&c, &i, 4); sha1_final(&c, h);
        memcpy(n + i, h, i + 20u <= 32u ? 20u : 12u); t += 0x9E3779B97F4A7C15ull;
    }
}

/* ---- the connect sequence ------------------------------------------------ */
int wlan_sta_connected(void) { return s_connected; }
int8_t wlan_sta_rssi(void) { return s_rssi; }

int wlan_sta_connect(const char *ssid, const char *pass, const struct wlan_scan_net *bss)
{
    static const uint8_t rsn[22] = { 0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0 };
    static const uint8_t rates[10] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    static const uint8_t xrates[6] = { 50, 4, 0x30, 0x48, 0x60, 0x6C };
    uint8_t body[128]; uint32_t n; int r;

    s_connected = 0; s_ev = 0; s_seq = 0;
    memcpy(s_self, wlan_mac(), 6);
    memcpy(s_bssid, bss->bssid, 6); s_chan = bss->chan;
    s_ssid_len = (uint32_t)strlen(ssid); if (s_ssid_len > 32u) s_ssid_len = 32u;
    memcpy(s_ssid, ssid, s_ssid_len);
    memcpy(s_rsn_ie, rsn, 22);
    say("sta: connecting to \""); say(ssid); say_hex("\" ch ", s_chan); say_hex(" rssi -", (uint32_t)-bss->rssi); say("\n");

    vsay("sta: deriving PMK (PBKDF2, 8192 HMACs) ... ");
    wpa_pmk_from_passphrase(pass, s_ssid, s_ssid_len, s_pmk);
    vsay("done\n");

    wcn36xx_set_rx_handler(rx_handler);
    if (hal_add_sta_self() < 0) return -1;
    vsay_hex("sta:   self sta_index ", s_self_sta); vsay_hex(" dpu ", s_self_dpu); vsay("\n");
    if (hal_set_link(LINK_PREASSOC) < 0) return -1;
    if (hal_join() < 0) return -1;
    if (hal_config_bss(0) < 0) return -1;

    /* authentication (open system) */
    body[0] = 0; body[1] = 0; body[2] = 1; body[3] = 0; body[4] = 0; body[5] = 0;
    {
        /* Retried like the association request: on the C2 the AP's reply to
         * the very first frame after CONFIG_BSS showed up more than a second
         * late (seen during the next scan), so one 1 s try was not enough. */
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            vsay("sta: TX authentication ... ");
            if (tx_mgmt(body, 6, 0x00B0u) < 0) { say("TX failed\n"); return -1; }
            r = wait_ev(1, 2000u);
            if (r == 0) break;
            if (r == -1) { vsay("REFUSED"); vsay_hex(", status ", s_ev_status); vsay("\n"); return -1; }
            say("no response within 2 s\n");
        }
        if (r) return -1;
    }
    vsay("authenticated\n");

    /* association request: capab, listen interval, SSID, rates, ext rates, RSN */
    n = 0;
    body[n++] = 0x11; body[n++] = 0x04;        /* ESS | privacy | short slot */
    body[n++] = 1; body[n++] = 0;              /* listen interval */
    body[n++] = 0; body[n++] = (uint8_t)s_ssid_len; memcpy(body + n, s_ssid, s_ssid_len); n += s_ssid_len;
    memcpy(body + n, rates, 10); n += 10;
    memcpy(body + n, xrates, 6); n += 6;
    memcpy(body + n, rsn, 22); n += 22;
    {
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            vsay("sta: TX association request ... ");
            if (tx_mgmt(body, n, 0x0000u) < 0) { say("TX failed\n"); return -1; }
            r = wait_ev(2, 2000u);
            if (r == 0) break;
            if (r == -1) { vsay("REFUSED"); vsay_hex(", status ", s_ev_status); vsay("\n"); return -1; }
            say("no response within 2 s\n");
        }
        if (r) return -1;
    }
    vsay_hex("associated, AID ", s_aid); vsay("\n");

    if (hal_set_link(LINK_POSTASSOC) < 0) return -1;
    if (hal_config_bss(1) < 0) return -1;
    if (hal_config_sta() < 0) return -1;

    /* 4-way handshake */
    vsay("sta: waiting for EAPOL 1/4 ... ");
    r = wait_ev(3, 3000u);
    if (r) { vsay("not received\n"); return -1; }
    vsay("got ANonce\n");
    make_nonce(s_snonce);
    wpa_ptk(s_pmk, s_bssid, s_self, s_anonce, s_snonce, s_ptk);
    vsay("sta: TX EAPOL 2/4 ... ");
    if (tx_eapol_msg(2u | KI_PAIRWISE | KI_MIC, s_snonce, s_rsn_ie, 22, 0) < 0) { say("TX failed\n"); return -1; }
    r = wait_ev(4, 3000u);
    if (r) { say(r == -2 ? "no 3/4 (password rejected?)\n" : "failed\n"); return -1; }
    vsay_hex("3/4 verified, GTK ", s_gtk_len); vsay_hex(" B id ", s_gtk_id); vsay("\n");
    vsay("sta: TX EAPOL 4/4 ... ");
    if (tx_eapol_msg(2u | KI_PAIRWISE | KI_MIC | KI_SECURE, 0, 0, 0, 0) < 0) { say("TX failed\n"); return -1; }
    vsay("sent\n");

    /* keys into the firmware (mainline set_key order: config_bss/sta then keys) */
    s_connected = 1;                           /* encrypt_type CCMP from here on */
    if (hal_config_bss(1) < 0 || hal_config_sta() < 0 || hal_set_stakey(s_ptk + 32) < 0) { s_connected = 0; return -1; }
    if (s_gtk_len && hal_set_bsskey() < 0) { s_connected = 0; return -1; }
    say("sta: *** CONNECTED to \""); say(ssid); say("\" -- keys installed ***\n");
    return 0;
}

int wlan_sta_disconnect(void)
{
    if (!s_connected && s_bss_index == 0xFFu && !s_self_added) return 0;
    s_connected = 0;
    if (s_bss_index != 0xFFu) {
        struct cur c = { (uint8_t *)s_buf, 0 };
        hal_hdr(&c, HAL_DELETE_BSS_REQ); c8(&c, s_bss_index); hal_fin(&c);
        hal_ok("sta: DELETE_BSS", HAL_DELETE_BSS_RSP, c.n, 12);
        s_bss_index = 0xFF;
    }
    hal_set_link(LINK_IDLE);
    hal_del_sta_self();
    wcn36xx_set_rx_handler(0);
    return 0;
}

#else
int wlan_sta_connect(const char *s, const char *p, const struct wlan_scan_net *b) { (void)s; (void)p; (void)b; return -1; }
int wlan_sta_connected(void) { return 0; }
int wlan_sta_disconnect(void) { return 0; }
void wlan_sta_reset(void) {}
int wlan_sta_tx_eth(const uint8_t *e, uint32_t l) { (void)e; (void)l; return -1; }
int wlan_sta_tx_probe(void) { return -1; }
void wlan_sta_counters(uint32_t *tx, uint32_t *rx, uint32_t *rx_other) { *tx = *rx = *rx_other = 0; }
void wlan_sta_set_data_rx(void (*fn)(const uint8_t *eth, uint32_t len)) { (void)fn; }
int8_t wlan_sta_rssi(void) { return -127; }
#endif
