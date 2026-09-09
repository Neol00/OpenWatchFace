/* wcn36xx.c — the WLAN data path (DXE DMA engine) and a passive scan
 * (step 6 of WIFI-BRINGUP.md, second half).
 *
 * Port of the parts of drivers/net/wireless/ath/wcn36xx/{dxe.c,txrx.c,smd.c}
 * that a polling, single-threaded caller needs to SEE FRAMES: the four DXE
 * rings (two RX, two TX) programmed exactly as wcn36xx_dxe_init() does, an RX
 * poll that walks the descriptors the engine has filled, and the HAL
 * software-scan sequence INIT_SCAN -> {START_SCAN(ch), dwell, END_SCAN(ch)}
 * -> FINISH_SCAN, during which the firmware forwards every beacon it hears
 * through the RX rings. Beacons are parsed for SSID / BSSID / channel / RSSI
 * and printed. No TX yet (probe requests would make the scan active).
 *
 * MEMORY: descriptors and RX buffers are plain .bss (cacheable, VA == PA).
 * The engine reads/writes them by physical address, so every hand-over is
 * fenced with explicit cache maintenance: clean before the engine may read,
 * invalidate before we read what the engine wrote. Descriptors are 32 bytes
 * and 32-byte aligned, i.e. exactly one cache line each.
 *
 * RX BUFFER DESCRIPTOR: the first 76 bytes of each received buffer are the
 * wcn36xx_rx_bd, delivered BIG-ENDIAN word by word (txrx.c byte-swaps it
 * with buff_to_be before reading bitfields). We swap the same way.
 */
#include "platform.h"
#if defined(PLAT_WCNSS_FW_BASE) && defined(PLAT_SMEM_BASE)

#include <string.h>

/* ---- console ------------------------------------------------------------ */
#if defined(LOG_VERBOSE)
#define vsay(s)        say(s)
#define vsay_hex(s, v) say_hex(s, v)
#define vsay_dec(s, v) say_dec(s, v)
#else
#define vsay(s)        ((void)(s))
#define vsay_hex(s, v) ((void)(s), (void)(v))
#define vsay_dec(s, v) ((void)(s), (void)(v))
#endif
static void say(const char *s) { con_puts(s); con_flush(); usb_poll(); blackbox_sync(); }
static void say_hex(const char *s, uint32_t v) { con_puts(s); con_puthex(v); con_flush(); usb_poll(); }
static void say_dec(const char *s, uint32_t v) { con_puts(s); con_putdec(v); con_flush(); usb_poll(); }

/* ---- cache maintenance -------------------------------------------------- */
static void dc_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    for (; a < end; a += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void dc_inval(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < end; a += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void dc_clean_inval(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    for (; a < end; a += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- DXE registers (dxe.h; pronto: ccu 0xA204000, dxe 0xA202000) --------- */
#define DXE_BASE            0x0A202000u
#define CCU_BASE            0x0A204000u
#define CCU_DXE_INT_SELECT_PRONTO 0x10DCu
#define DXE_R(off)          mmio_read(DXE_BASE + (off))
#define DXE_W(off, v)       mmio_write(DXE_BASE + (off), (v))
#define DXE_REG_CSR_RESET   0x00u
#define DXE_ENCH_ADDR       0x04u
#define DXE_REG_CH_EN       0x08u
#define DXE_INT_MASK_REG    0x18u
#define DXE_INT_SRC_RAW_REG 0x20u
#define DXE_0_INT_CLR       0x30u
#define DXE_0_INT_ED_CLR    0x34u
#define DXE_0_INT_DONE_CLR  0x38u
#define DXE_0_INT_ERR_CLR   0x3Cu
#define DXE_REG_RESET_VAL   0x5C89u
#define CH_STAT_INT_DONE    0x8000u
#define CH_STAT_INT_ERR     0x4000u
#define CH_STAT_INT_ED      0x2000u
#define INT_CH0 0x1u
#define INT_CH1 0x2u
#define INT_CH3 0x8u
#define INT_CH4 0x10u
#define TX_L_OFF 0x400u
#define RX_L_OFF 0x440u
#define RX_H_OFF 0x4C0u
#define TX_H_OFF 0x500u
#define CH_STATUS     0x04u
#define CH_SRC_ADDR   0x0Cu
#define CH_DEST_ADDR  0x14u
#define CH_NEXT_DESC  0x1Cu

/* descriptor ctrl words (dxe.h) */
#define C_VLD  (1u << 0)
#define C_EOP  (1u << 3)
#define C_SIQ  (1u << 5)
#define C_DIQ  (1u << 6)
#define C_PDU_REL (1u << 8)
#define C_INT  (1u << 17)
#define C_SWAP (1u << 20)
#define C_ENDIAN (1u << 21)
#define C_XTYPE(x) ((x) << 1)
#define C_BTHLD(x) ((x) << 9)
#define C_PRIO(x)  ((x) << 13)
#define XTYPE_H2B 2u
#define XTYPE_B2H 3u
#define CTRL_TX_L   (C_XTYPE(XTYPE_H2B) | C_DIQ | C_BTHLD(5) | C_PRIO(4) | C_INT | C_SWAP | C_ENDIAN)
#define CTRL_TX_H   (C_XTYPE(XTYPE_H2B) | C_DIQ | C_BTHLD(7) | C_PRIO(6) | C_INT | C_SWAP | C_ENDIAN)
#define CTRL_RX_L   (C_VLD | C_XTYPE(XTYPE_B2H) | C_EOP | C_SIQ | C_PDU_REL | C_BTHLD(6) | C_PRIO(5) | C_INT | C_SWAP)
#define CTRL_RX_H   (C_VLD | C_XTYPE(XTYPE_B2H) | C_EOP | C_SIQ | C_PDU_REL | C_BTHLD(8) | C_PRIO(6) | C_INT | C_SWAP)
/* channel control registers */
#define CC_EN (1u<<0)
#define CC_EOP (1u<<3)
#define CC_SIQ (1u<<5)
#define CC_DIQ (1u<<6)
#define CC_PDU_REL (1u<<8)
#define CC_INE_ED (1u<<17)
#define CC_INE_ERR (1u<<18)
#define CC_INE_DONE (1u<<19)
#define CC_EDEN (1u<<20)
#define CC_EDVEN (1u<<21)
#define CC_ENDIAN (1u<<26)
#define CC_SWAP (1u<<31)
#define CC_XTYPE(x) ((x)<<1)
#define CC_BTHLD(x) ((x)<<9)
#define CC_PRIO(x) ((x)<<13)
#define CC_SEL(x) ((x)<<22)
#define CC_COMMON (CC_EN | CC_EOP | CC_PDU_REL | CC_INE_ED | CC_INE_ERR | CC_INE_DONE | CC_EDEN | CC_EDVEN | CC_ENDIAN | CC_SWAP)
#define CH_CTL_RX_L (CC_COMMON | CC_XTYPE(XTYPE_B2H) | CC_SIQ | CC_BTHLD(6) | CC_PRIO(5) | CC_SEL(1))
#define CH_CTL_RX_H (CC_COMMON | CC_XTYPE(XTYPE_B2H) | CC_SIQ | CC_BTHLD(8) | CC_PRIO(6) | CC_SEL(3))
#define CH_CTL_TX_L (CC_COMMON | CC_XTYPE(XTYPE_H2B) | CC_DIQ | CC_BTHLD(5) | CC_PRIO(4) | CC_SEL(0))
#define CH_CTL_TX_H (CC_COMMON | CC_XTYPE(XTYPE_H2B) | CC_DIQ | CC_BTHLD(7) | CC_PRIO(6) | CC_SEL(4))
#define WQ_TX_PRONTO_V3 0x6u
#define WQ_RX_L 0xBu
#define WQ_RX_H 0x4u
#define PKT_SIZE 0xF20u

struct dxe_desc { uint32_t ctrl, fr_len, src_l, dst_l, next_l, src_h, dst_h, next_h; };

#define N_RX_L 32u
#define N_RX_H 16u
#define N_TX_L 16u
#define N_TX_H 8u
static struct dxe_desc s_rx_l_desc[N_RX_L] __attribute__((aligned(32)));
static struct dxe_desc s_rx_h_desc[N_RX_H] __attribute__((aligned(32)));
static struct dxe_desc s_tx_l_desc[N_TX_L] __attribute__((aligned(32)));
static struct dxe_desc s_tx_h_desc[N_TX_H] __attribute__((aligned(32)));
static uint8_t s_rx_l_buf[N_RX_L][PKT_SIZE] __attribute__((aligned(64)));
static uint8_t s_rx_h_buf[N_RX_H][PKT_SIZE] __attribute__((aligned(64)));
static uint32_t s_rx_l_head, s_rx_h_head, s_tx_l_head, s_tx_h_head;
static int s_dxe_up;
/* TX: BD chunks + frame staging, one per ring pair slot */
#define TX_FRAME_MAX 1600u
static uint8_t s_tx_h_bd[N_TX_H / 2][64] __attribute__((aligned(64)));
static uint8_t s_tx_l_bd[N_TX_L / 2][64] __attribute__((aligned(64)));
static uint8_t s_tx_h_frm[N_TX_H / 2][TX_FRAME_MAX] __attribute__((aligned(64)));
static uint8_t s_tx_l_frm[N_TX_L / 2][TX_FRAME_MAX] __attribute__((aligned(64)));
#define CTRL_TX_H_BD  (C_VLD | C_XTYPE(XTYPE_H2B) | C_DIQ | C_BTHLD(7) | C_PRIO(6) | C_SWAP | C_ENDIAN)
#define CTRL_TX_H_SKB (C_VLD | C_XTYPE(XTYPE_H2B) | C_EOP | C_DIQ | C_BTHLD(7) | C_PRIO(6) | C_INT | C_SWAP | C_ENDIAN)
#define CTRL_TX_L_BD  (C_VLD | C_XTYPE(XTYPE_H2B) | C_DIQ | C_BTHLD(5) | C_PRIO(4) | C_SWAP | C_ENDIAN)
#define CTRL_TX_L_SKB (C_VLD | C_XTYPE(XTYPE_H2B) | C_EOP | C_DIQ | C_BTHLD(5) | C_PRIO(4) | C_INT | C_SWAP | C_ENDIAN)
static void (*s_rx_handler)(const uint8_t *f, uint32_t len, int8_t rssi);
void wcn36xx_set_rx_handler(void (*fn)(const uint8_t *f, uint32_t len, int8_t rssi)) { s_rx_handler = fn; }

static void ring_build(struct dxe_desc *d, uint32_t n, uint32_t ctrl, uint32_t src, uint32_t dst_wq, uint8_t (*bufs)[PKT_SIZE])
{
    uint32_t i;
    memset(d, 0, n * sizeof *d);
    for (i = 0; i < n; i++) {
        d[i].ctrl = ctrl;
        if (bufs) { d[i].src_l = src; d[i].dst_l = (uint32_t)(uintptr_t)bufs[i]; }
        else      { d[i].dst_l = dst_wq; }
        d[i].next_l = (uint32_t)(uintptr_t)&d[(i + 1u) % n];
    }
    dc_clean_inval(d, n * sizeof *d);
}

int wcn36xx_dxe_init(void)
{
    uint32_t v;
    vsay("wcn36xx: DXE init (dxe @0xA202000, ccu @0xA204000) ... ");
    DXE_W(DXE_REG_CSR_RESET, DXE_REG_RESET_VAL);
    /* RX-avail on CH1/CH3 and xfer-done on CH0/CH4, pronto select register */
    mmio_write(CCU_BASE + CCU_DXE_INT_SELECT_PRONTO, ((INT_CH3 | INT_CH1) << 16) | INT_CH0 | INT_CH4);

    /* TX rings: descriptors only, nothing queued */
    ring_build(s_tx_l_desc, N_TX_L, CTRL_TX_L, 0, WQ_TX_PRONTO_V3, 0);
    DXE_W(TX_L_OFF + CH_NEXT_DESC, (uint32_t)(uintptr_t)s_tx_l_desc);
    DXE_W(TX_L_OFF + CH_DEST_ADDR, WQ_TX_PRONTO_V3);
    v = DXE_R(DXE_REG_CH_EN);
    ring_build(s_tx_h_desc, N_TX_H, CTRL_TX_H, 0, WQ_TX_PRONTO_V3, 0);
    DXE_W(TX_H_OFF + CH_NEXT_DESC, (uint32_t)(uintptr_t)s_tx_h_desc);
    DXE_W(TX_H_OFF + CH_DEST_ADDR, WQ_TX_PRONTO_V3);
    v = DXE_R(DXE_REG_CH_EN);

    /* RX rings: every descriptor owns a buffer and is VALID */
    dc_clean_inval(s_rx_l_buf, sizeof s_rx_l_buf);
    dc_clean_inval(s_rx_h_buf, sizeof s_rx_h_buf);
    ring_build(s_rx_l_desc, N_RX_L, CTRL_RX_L, WQ_RX_L, 0, s_rx_l_buf);
    DXE_W(RX_L_OFF + CH_NEXT_DESC, (uint32_t)(uintptr_t)s_rx_l_desc);
    DXE_W(RX_L_OFF + CH_SRC_ADDR, WQ_RX_L);
    DXE_W(RX_L_OFF + CH_DEST_ADDR, s_rx_l_desc[0].next_l);
    DXE_W(RX_L_OFF, CH_CTL_RX_L);
    ring_build(s_rx_h_desc, N_RX_H, CTRL_RX_H, WQ_RX_H, 0, s_rx_h_buf);
    DXE_W(RX_H_OFF + CH_NEXT_DESC, (uint32_t)(uintptr_t)s_rx_h_desc);
    DXE_W(RX_H_OFF + CH_SRC_ADDR, WQ_RX_H);
    DXE_W(RX_H_OFF + CH_DEST_ADDR, s_rx_h_desc[0].next_l);
    DXE_W(RX_H_OFF, CH_CTL_RX_H);
    s_rx_l_head = s_rx_h_head = s_tx_l_head = s_tx_h_head = 0;

    /* channel interrupt enables (we poll INT_SRC_RAW, but the engine's
     * bookkeeping wants them on, as mainline does) */
    DXE_W(DXE_INT_MASK_REG, DXE_R(DXE_INT_MASK_REG) | INT_CH0 | INT_CH1 | INT_CH3 | INT_CH4);
    __asm__ volatile("dsb sy" ::: "memory");
    vsay_hex("ch_en=", DXE_R(DXE_REG_CH_EN)); vsay_hex(" int_mask=", DXE_R(DXE_INT_MASK_REG));
    vsay_hex(" rx_l_status=", DXE_R(RX_L_OFF + CH_STATUS)); vsay("\n");
    (void)v;
    s_dxe_up = 1;
    return 0;
}

/* ---- RX ----------------------------------------------------------------- */
struct scan_result { uint8_t bssid[6]; char ssid[33]; uint8_t chan; int8_t rssi; uint8_t secured; uint16_t seen; };
static struct scan_result s_res[24];
static uint32_t s_nres, s_nframes, s_nbeacon;

static void hex2(uint32_t v) { const char *h = "0123456789abcdef"; con_dbg_c(h[(v >> 4) & 15u]); con_dbg_c(h[v & 15u]); }
static uint32_t be32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }

static void note_beacon(const uint8_t *f, uint32_t len, int8_t rssi, uint8_t bd_chan)
{
    /* f = 802.11 header (24) + timestamp 8 + interval 2 + capab 2 + IEs */
    const uint8_t *bssid = f + 16, *ie = f + 36, *end = f + len;
    char ssid[33] = ""; uint8_t chan = bd_chan, secured = (f[34] >> 4) & 1u; uint32_t i;   /* capab bit 4 = privacy */
    while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
        if (ie[0] == 0 && ie[1] <= 32) { memcpy(ssid, ie + 2, ie[1]); ssid[ie[1]] = 0; }
        if (ie[0] == 3 && ie[1] == 1) chan = ie[2];
        ie += 2 + ie[1];
    }
    for (i = 0; i < s_nres; i++)
        if (!memcmp(s_res[i].bssid, bssid, 6)) { s_res[i].seen++; if (rssi > s_res[i].rssi) s_res[i].rssi = rssi; return; }
    if (s_nres >= sizeof s_res / sizeof s_res[0]) return;
    memcpy(s_res[s_nres].bssid, bssid, 6);
    memcpy(s_res[s_nres].ssid, ssid, 33);
    s_res[s_nres].chan = chan; s_res[s_nres].rssi = rssi; s_res[s_nres].seen = 1; s_res[s_nres].secured = secured;
    s_nres++;
    con_dbg("  new: ch "); con_dbg_dec(chan); con_dbg(" rssi -"); con_dbg_dec((uint32_t)-rssi);
    con_dbg(" bssid "); for (i = 0; i < 6u; i++) { hex2(bssid[i]); if (i < 5u) con_dbg_c(':'); }
    con_dbg(" \""); con_dbg(ssid[0] ? ssid : "<hidden>"); con_dbg("\"\n"); con_flush(); usb_poll();
}

static void rx_frame(uint8_t *buf)
{
    uint32_t bd[19], i, hdr_off, mpdu_len;
    int8_t rssi; uint8_t chan;
    dc_inval(buf, PKT_SIZE);
    for (i = 0; i < 19u; i++) { uint32_t w; memcpy(&w, buf + 4u * i, 4); bd[i] = be32(w); }
    s_nframes++;
    /* word 3: pdu_count:7 mpdu_data_off:9 mpdu_header_off:8 mpdu_header_len:8; word 4: ... mpdu_len:16 */
    hdr_off  = (bd[3] >> 16) & 0xFFu;
    mpdu_len = (bd[4] >> 16) & 0xFFFFu;
    rssi = (int8_t)-(int)(100u - ((bd[7] >> 24) & 0xFFu));      /* get_rssi0: 100 - phy_stat0[31:24] */
    chan = (uint8_t)((bd[0] >> 13) & 0xFu);                       /* rx_ch */
    if (hdr_off < 76u || hdr_off + mpdu_len > PKT_SIZE || mpdu_len < 24u) return;
    {
        const uint8_t *f = buf + hdr_off;
        uint32_t fc = f[0] | (f[1] << 8);
        if ((fc & 0xFCu) == 0x80u || (fc & 0xFCu) == 0x50u) {       /* beacon / probe response */
            s_nbeacon++;
            note_beacon(f, mpdu_len, rssi, chan);
        }
        if (s_rx_handler) s_rx_handler(f, mpdu_len, rssi);
    }
}

static uint32_t rx_poll_ring(struct dxe_desc *d, uint32_t n, uint32_t *head, uint8_t (*bufs)[PKT_SIZE],
                             uint32_t ctrl, uint32_t en_mask, uint32_t status_off)
{
    uint32_t got = 0, reason = DXE_R(status_off);
    DXE_W(DXE_0_INT_CLR, en_mask);
    if (reason & CH_STAT_INT_ERR)  DXE_W(DXE_0_INT_ERR_CLR, en_mask);
    if (reason & CH_STAT_INT_DONE) DXE_W(DXE_0_INT_DONE_CLR, en_mask);
    if (reason & CH_STAT_INT_ED)   DXE_W(DXE_0_INT_ED_CLR, en_mask);
    for (;;) {
        struct dxe_desc *x = &d[*head];
        dc_inval(x, sizeof *x);
        if (x->ctrl & C_VLD) break;
        rx_frame(bufs[*head]);
        dc_inval(bufs[*head], PKT_SIZE);          /* no stale lines before the engine refills it */
        x->ctrl = ctrl;
        dc_clean(x, sizeof *x);
        *head = (*head + 1u) % n;
        got++;
        if (got >= n) break;
    }
    if (got) DXE_W(DXE_ENCH_ADDR, en_mask);
    return got;
}

uint32_t wcn36xx_rx_poll(void)
{
    uint32_t src, got = 0;
    if (!s_dxe_up) return 0;
    src = DXE_R(DXE_INT_SRC_RAW_REG);
    if (src & INT_CH1) got += rx_poll_ring(s_rx_l_desc, N_RX_L, &s_rx_l_head, s_rx_l_buf, CTRL_RX_L, INT_CH1, RX_L_OFF + CH_STATUS);
    if (src & INT_CH3) got += rx_poll_ring(s_rx_h_desc, N_RX_H, &s_rx_h_head, s_rx_h_buf, CTRL_RX_H, INT_CH3, RX_H_OFF + CH_STATUS);
    return got;
}

/* ---- TX (dxe.c wcn36xx_dxe_tx_frame) --------------------------------------
 * Each transmit is a PAIR of descriptors: the 40-byte buffer descriptor
 * (BD, built by the caller, already byte-swapped to big-endian) then the
 * frame. The skb descriptor is marked valid first, then the BD one, then
 * the channel control register is written to kick the engine. Completion:
 * the engine clears VLD on both. Polled, 100 ms budget. */
static int tx_reap_wait(struct dxe_desc *d, uint32_t ms)
{
    uint32_t t = timer_ms();
    for (;;) {
        dc_inval(d, sizeof *d);
        if (!(d->ctrl & C_VLD)) return 0;
        if (timer_ms() - t > ms) return -1;
    }
}

int wcn36xx_tx(const uint8_t bd[40], const uint8_t *frame, uint32_t len, int high)
{
    struct dxe_desc *ring = high ? s_tx_h_desc : s_tx_l_desc;
    uint32_t n = high ? N_TX_H : N_TX_L, *head = high ? &s_tx_h_head : &s_tx_l_head;
    uint32_t slot = *head / 2u, int_mask = high ? INT_CH4 : INT_CH0, off = high ? TX_H_OFF : TX_L_OFF;
    uint8_t *bdbuf = high ? s_tx_h_bd[slot] : s_tx_l_bd[slot];
    uint8_t *frm   = high ? s_tx_h_frm[slot] : s_tx_l_frm[slot];
    struct dxe_desc *dbd = &ring[*head], *dsk = &ring[(*head + 1u) % n];
    uint32_t reason;
    int rc;

    if (!s_dxe_up || len > TX_FRAME_MAX) return -1;
    memcpy(bdbuf, bd, 40); dc_clean(bdbuf, 64);
    memcpy(frm, frame, len); dc_clean(frm, (len + 63u) & ~63u);

    dc_inval(dbd, sizeof *dbd); dc_inval(dsk, sizeof *dsk);
    dbd->src_l = (uint32_t)(uintptr_t)bdbuf; dbd->dst_l = WQ_TX_PRONTO_V3; dbd->fr_len = 40u;
    dsk->src_l = (uint32_t)(uintptr_t)frm;   dsk->dst_l = WQ_TX_PRONTO_V3; dsk->fr_len = len;
    dc_clean(dbd, sizeof *dbd); dc_clean(dsk, sizeof *dsk);
    dsk->ctrl = high ? CTRL_TX_H_SKB : CTRL_TX_L_SKB; dc_clean(dsk, sizeof *dsk);
    dbd->ctrl = high ? CTRL_TX_H_BD  : CTRL_TX_L_BD;  dc_clean(dbd, sizeof *dbd);
    __asm__ volatile("dsb sy" ::: "memory");
    DXE_W(off, high ? CH_CTL_TX_H : CH_CTL_TX_L);

    rc = tx_reap_wait(dsk, 100u);
    /* ack the transfer-done interrupt the way the irq handler does */
    reason = DXE_R(off + CH_STATUS);
    DXE_W(DXE_0_INT_CLR, int_mask);
    if (reason & CH_STAT_INT_ERR)  { DXE_W(DXE_0_INT_ERR_CLR, int_mask); say_hex("wcn36xx: TX error, ch status ", reason); say("\n"); rc = -1; }
    if (reason & CH_STAT_INT_DONE) DXE_W(DXE_0_INT_DONE_CLR, int_mask);
    if (reason & CH_STAT_INT_ED)   DXE_W(DXE_0_INT_ED_CLR, int_mask);
    *head = (*head + 2u) % n;
    if (rc < 0) {
        uint32_t k;
        vsay_hex("wcn36xx: TX not completed, desc ctrl ", dsk->ctrl);
        vsay_hex(" ch_status ", DXE_R(off + CH_STATUS)); vsay_hex(" ch_en ", DXE_R(DXE_REG_CH_EN));
        vsay_hex(" ch_err ", DXE_R(0x10u)); vsay_hex(" int_src ", DXE_R(DXE_INT_SRC_RAW_REG));
        vsay_hex(" bd_desc ctrl ", dbd->ctrl); vsay("\n  bd:");
        for (k = 0; k < 10u; k++) { uint32_t w; memcpy(&w, bd + 4u * k, 4); con_dbg_c(' '); con_dbg_hex(w); }
        vsay("\n");
        /* unwedge the ring: drop the pair so the next frame does not queue behind it */
        dsk->ctrl &= ~C_VLD; dbd->ctrl &= ~C_VLD; dc_clean(dsk, sizeof *dsk); dc_clean(dbd, sizeof *dbd);
    }
    return rc;
}

/* ---- HAL software scan -------------------------------------------------- */
#define HAL_INIT_SCAN_REQ   4u
#define HAL_INIT_SCAN_RSP   5u
#define HAL_START_SCAN_REQ  6u
#define HAL_START_SCAN_RSP  7u
#define HAL_END_SCAN_REQ    8u
#define HAL_END_SCAN_RSP    9u
#define HAL_FINISH_SCAN_REQ 10u
#define HAL_FINISH_SCAN_RSP 11u
#define HAL_SYS_MODE_SCAN   2u

static uint32_t s_buf[256];

/* send + wait for `want`, polling RX in between so beacons are not missed */
static int hal_xfer(struct smd_chan *ch, uint32_t len, uint32_t want)
{
    uint32_t got, i, t;
    for (t = timer_ms(); smd_send(ch, s_buf, len) < 0; ) { if (timer_ms() - t > 1000u) return -1; timer_delay_ms(1); }
    for (i = 0; i < 16u; i++) {
        got = smd_recv(ch, s_buf, sizeof s_buf, 3000u);
        if (got < 8u) return -1;
        if ((s_buf[0] & 0xFFFFu) == want) return (int)got;
        con_dbg("  [hal ind "); con_dbg_hex(s_buf[0] & 0xFFFFu); con_dbg("] ");
    }
    return -1;
}

int wcn36xx_scan(struct smd_chan *wlan, uint32_t dwell_ms, struct wlan_scan_net *out, uint32_t max)
{
    uint8_t *b = (uint8_t *)s_buf;
    uint32_t ch, i, t;
    int got;

    s_nres = s_nframes = s_nbeacon = 0;
    if (!s_dxe_up && wcn36xx_dxe_init() < 0) return -1;

    /* INIT_SCAN: mode SCAN, no BSS to notify (we are not associated) */
    memset(s_buf, 0, 48);
    s_buf[0] = HAL_INIT_SCAN_REQ; s_buf[1] = 48u; s_buf[2] = HAL_SYS_MODE_SCAN;
    vsay("wcn36xx: INIT_SCAN ... ");
    got = hal_xfer(wlan, 48u, HAL_INIT_SCAN_RSP);
    if (got < 12 || s_buf[2] != 0u) { say_hex("FAILED status=", got >= 12 ? s_buf[2] : 0xFFFFFFFFu); say("\n"); return -1; }
    vsay("ok\nwcn36xx: passive scan, channels 1..13, dwell "); con_dbg_dec(dwell_ms); vsay(" ms each\n");

    for (ch = 1; ch <= 13u; ch++) {
        s_buf[0] = HAL_START_SCAN_REQ; s_buf[1] = 9u; b[8] = (uint8_t)ch;
        got = hal_xfer(wlan, 9u, HAL_START_SCAN_RSP);
        if (got < 12 || s_buf[2] != 0u) { say_dec("wcn36xx: START_SCAN ch ", ch); say_hex(" FAILED status=", got >= 12 ? s_buf[2] : 0xFFFFFFFFu); say("\n"); break; }
        for (t = timer_ms(); timer_ms() - t < dwell_ms; ) { wcn36xx_rx_poll(); timer_delay_ms(2); }
        s_buf[0] = HAL_END_SCAN_REQ; s_buf[1] = 9u; b[8] = (uint8_t)ch;
        got = hal_xfer(wlan, 9u, HAL_END_SCAN_RSP);
        if (got < 12 || s_buf[2] != 0u) { say_dec("wcn36xx: END_SCAN ch ", ch); say(" FAILED\n"); break; }
        wcn36xx_rx_poll();
        wdog_pet(); deadman_kick();
    }

    /* FINISH_SCAN: mode SCAN, back to channel 1, single-channel centred */
    memset(s_buf, 0, 56);
    s_buf[0] = HAL_FINISH_SCAN_REQ; s_buf[1] = 53u; s_buf[2] = HAL_SYS_MODE_SCAN;
    b[12] = 1u;                                   /* oper_channel */
    /* cb_state (u32 @13..16) = 0, bssid @17, notify/frame_type/len @23..25, mgmt hdr @26, scan_entry @50 */
    vsay("wcn36xx: FINISH_SCAN ... ");
    got = hal_xfer(wlan, 53u, HAL_FINISH_SCAN_RSP);
    say(got >= 12 && s_buf[2] == 0u ? "ok\n" : "FAILED\n");

    vsay_dec("wcn36xx: RX frames ", s_nframes); vsay_dec(", beacons ", s_nbeacon); vsay_dec(", networks ", s_nres); vsay("\n");
    for (i = 0; i < s_nres; i++) {
        uint32_t k;
        con_dbg("  ch "); if (s_res[i].chan < 10u) con_dbg_c(' '); con_dbg_dec(s_res[i].chan);
        con_dbg("  rssi -"); con_dbg_dec((uint32_t)-s_res[i].rssi);
        con_dbg("  "); for (k = 0; k < 6u; k++) { hex2(s_res[i].bssid[k]); if (k < 5u) con_dbg_c(':'); }
        con_dbg("  x"); con_dbg_dec(s_res[i].seen);
        con_dbg("  \""); con_dbg(s_res[i].ssid[0] ? s_res[i].ssid : "<hidden>"); con_dbg("\"\n"); con_flush(); usb_poll();
    }
    vsay("wcn36xx: *** SCAN DONE ***\n");
    if (out) {
        for (i = 0; i < s_nres && i < max; i++) {
            memcpy(out[i].ssid, s_res[i].ssid, 33); memcpy(out[i].bssid, s_res[i].bssid, 6);
            out[i].chan = s_res[i].chan; out[i].rssi = s_res[i].rssi; out[i].secured = s_res[i].secured;
        }
        return (int)i;
    }
    return (int)s_nres;
}

#else
int wcn36xx_dxe_init(void) { return -1; }
uint32_t wcn36xx_rx_poll(void) { return 0; }
int wcn36xx_scan(struct smd_chan *wlan, uint32_t dwell_ms, struct wlan_scan_net *out, uint32_t max) { (void)wlan; (void)dwell_ms; (void)out; (void)max; return -1; }
int wcn36xx_tx(const uint8_t bd[40], const uint8_t *frame, uint32_t len, int high) { (void)bd; (void)frame; (void)len; (void)high; return -1; }
void wcn36xx_set_rx_handler(void (*fn)(const uint8_t *f, uint32_t len, int8_t rssi)) { (void)fn; }
#endif
