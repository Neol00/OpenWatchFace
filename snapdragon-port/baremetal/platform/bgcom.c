/* bgcom.c — the AP side of the QCC1110 "bgcom" link, from bare metal.
 *
 * Protocol (drivers/soc/qcom/bgcom_spi.c, PROTOCOL code only; every pin is
 * from this watch's DTB via boards/fossil_gen5.h). SPI mode 0, the BG is the
 * slave, one transaction per CS assertion:
 *   register read : tx [reg][0][0][0]         rx: 4 LE bytes per register
 *   FIFO read     : tx [0x41][0][0][0]        rx: N words from to-master FIFO
 *   FIFO write    : tx [0x40] + N LE words    (into the to-slave FIFO)
 * Registers: 0x05 SLAVE_STATUS (bit31 awake, 30 app running, 29 to-slave
 * FIFO ready, 28 to-master FIFO ready, 27 AHB ready), 0x06 timestamp,
 * 0x09 auto-clear status (error bits), 0x0B FIFO_FILL (low16 = words in the
 * to-master FIFO waiting for us, high16 = free words in the to-slave FIFO),
 * 0x0D FIFO_SIZE (low16 to-master, high16 to-slave).
 *
 * The to-master FIFO is MULTIPLEXED (parse_fifo): frames of
 *   { u16 event_id; u16 len; u8 payload[len] }
 *   event_id 0x0001 = GLINK commands (16-byte records, glink_bgcom_xprt.c)
 *   event_id 0xFFFE = RSB event: u8 sub_id (1 rotation, 2 button), u32 time,
 *                     s16 value -- the CROWN.
 *
 * GLINK command record: { u16 id; u16 param1; u32 param2; u32 param3;
 * u32 param4 } with ids VERSION 0, VERSION_ACK 1, OPEN 2, CLOSE 3, OPEN_ACK 4,
 * CLOSE_ACK 5, RX_INTENT 6, RX_DONE 7, RX_DONE_W_REUSE 8, RX_INTENT_REQ 9,
 * RX_INTENT_REQ_ACK 10, TX_DATA 11, TX_DATA_CONT 12, READ_NOTIF 13,
 * SIGNALS 14, TRACER_PKT 15, TRACER_PKT_CONT 16, TX_SHORT_DATA 17.
 * Link-up: each side sends VERSION{version 1, features TRACER_PKT bit2};
 * the other answers VERSION_ACK. OPEN carries the channel name right after
 * the record, padded to 16.
 *
 * v270 scope: bit-banged bus (the TZ app owns the QUP while it loads; we
 * take the pads back as GPIOs afterwards), drain + decode whatever the BG
 * has queued, answer its VERSION, send ours, and keep polling for a while
 * printing every frame -- including crown events if the RSB is already
 * live. No channel is opened yet. */
#include "platform.h"
#include "bg_codec_cal.h"
#if defined(PLAT_HAS_BG_QCC1110)

#include <string.h>
#if defined(AUDIO_PER_SOUND)
#include "FreeRTOS.h"
#include "task.h"
#endif

#define BGCOM_REG_STATUS      0x05u
#define BGCOM_REG_FIFO_FILL   0x0Bu
#define BGCOM_REG_FIFO_SIZE   0x0Du
#define BGCOM_CMD_FIFO_WRITE  0x40u
#define BGCOM_CMD_FIFO_READ   0x41u

#define GLINK_VERSION_CMD     0u
#define GLINK_VERSION_ACK_CMD 1u
#define GLINK_OPEN_CMD        2u
#define GLINK_OPEN_ACK_CMD    4u
#define GLINK_RX_INTENT_CMD   6u
#define GLINK_TRACER_PKT_FEATURE (1u << 2)

static int bgcom_wake(void);   /* defined below: the kernel's bgcom_resume() */

/* ---- bit-banged SPI, mode 0 ---------------------------------------------- */
static void dly(void) { uint32_t t0 = timer_us32(); while ((uint32_t)(timer_us32() - t0) < 4u) ; }

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        tlmm_out(PLAT_BG_SPI_MOSI, (out >> i) & 1u); dly();
        tlmm_out(PLAT_BG_SPI_CLK, 1);
        in = (uint8_t)((in << 1) | (tlmm_in(PLAT_BG_SPI_MISO) & 1u));
        dly();
        tlmm_out(PLAT_BG_SPI_CLK, 0);
    }
    return in;
}

static void cs(int on)
{
    if (on) { tlmm_out(PLAT_BG_SPI_CS, 0); dly(); dly(); }
    else    { dly(); tlmm_out(PLAT_BG_SPI_CS, 1); dly(); dly(); dly(); }
}

void bgcom_bus_take(void)
{
    /* Plain GPIOs: CS idle high, CLK idle low (mode 0), MOSI low, MISO in. */
    tlmm_cfg(PLAT_BG_SPI_CS,   0u, 0u, 2u, 1); tlmm_out(PLAT_BG_SPI_CS, 1);
    tlmm_cfg(PLAT_BG_SPI_CLK,  0u, 0u, 2u, 1); tlmm_out(PLAT_BG_SPI_CLK, 0);
    tlmm_cfg(PLAT_BG_SPI_MOSI, 0u, 0u, 2u, 1); tlmm_out(PLAT_BG_SPI_MOSI, 0);
    tlmm_cfg(PLAT_BG_SPI_MISO, 0u, 0u, 2u, 0);
    timer_delay_us(200u);
}

int bgcom_reg_read(uint8_t reg, unsigned n, uint32_t *w)
{
    cs(1);
    xfer(reg); xfer(0); xfer(0); xfer(0);
    for (unsigned i = 0; i < n; i++) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4u; b++) v |= (uint32_t)xfer(0) << (8u * b);
        w[i] = v;
    }
    cs(0);
    return 0;
}

int bgcom_fifo_read(unsigned nwords, uint32_t *w)
{
    cs(1);
    xfer(BGCOM_CMD_FIFO_READ); xfer(0); xfer(0); xfer(0);
    for (unsigned i = 0; i < nwords; i++) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4u; b++) v |= (uint32_t)xfer(0) << (8u * b);
        w[i] = v;
    }
    cs(0);
    return 0;
}

/* AHB read: [0x43][addr LE x4][0][0][0] then the words (bgcom_ahb_read,
 * BG_SPI_AHB_READ_CMD_LEN = 8). Data the BG sends by TX_DATA lives in ITS
 * memory and is fetched this way. */
int bgcom_ahb_read(uint32_t addr, unsigned nwords, uint32_t *w)
{
    cs(1);
    xfer(0x43u);
    for (unsigned b = 0; b < 4u; b++) xfer((uint8_t)(addr >> (8u * b)));
    xfer(0); xfer(0); xfer(0);
    for (unsigned i = 0; i < nwords; i++) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4u; b++) v |= (uint32_t)xfer(0) << (8u * b);
        w[i] = v;
    }
    cs(0);
    return 0;
}

/* AHB write: [0x42][addr LE x4] then the words (BG_SPI_AHB_CMD_LEN = 5, so
 * unlike the read there are no pad bytes). This is how the AP puts a payload
 * into the BG's memory before a TX_DATA record points at it. */
int bgcom_ahb_write(uint32_t addr, unsigned nwords, const uint32_t *w)
{
    cs(1);
    xfer(0x42u);
    for (unsigned b = 0; b < 4u; b++) xfer((uint8_t)(addr >> (8u * b)));
    for (unsigned i = 0; i < nwords; i++)
        for (unsigned b = 0; b < 4u; b++) xfer((uint8_t)(w[i] >> (8u * b)));
    cs(0);
    return 0;
}

int bgcom_fifo_write(unsigned nwords, const uint32_t *w)
{
    cs(1);
    xfer(BGCOM_CMD_FIFO_WRITE);
    for (unsigned i = 0; i < nwords; i++)
        for (unsigned b = 0; b < 4u; b++) xfer((uint8_t)(w[i] >> (8u * b)));
    cs(0);
    return 0;
}

/* ---- GLINK over the FIFO -------------------------------------------------- */
struct glink_cmd { uint16_t id; uint16_t p1; uint32_t p2, p3, p4; };

static int s_remote_version_seen, s_version_acked;

static int glink_send(const void *rec, unsigned bytes)
{
    /* Wrap in a FIFO frame? No: the kernel writes the raw 16-byte records
     * into the to-slave FIFO (bgcom_fifo_write in tx_cmd_safe), the frame
     * header exists only on the to-master direction. */
    uint32_t fill = 0;
    bgcom_reg_read(BGCOM_REG_FIFO_FILL, 1, &fill);
    if ((fill >> 16) < bytes / 4u + 4u) {
        con_puts("bgcom: to-slave FIFO full (free="); con_putdec(fill >> 16); con_puts(")\n");
        return -1;
    }
    return bgcom_fifo_write(bytes / 4u, (const uint32_t *)rec);
}

static void glink_tx_version(unsigned ack)
{
    struct { uint16_t id, version; uint32_t features, fifo_size, reserved; } c;
    memset(&c, 0, sizeof c);
    c.id = ack ? GLINK_VERSION_ACK_CMD : GLINK_VERSION_CMD;
    c.version = 1u;
    c.features = GLINK_TRACER_PKT_FEATURE;
    con_puts(ack ? "bgcom: glink -> VERSION_ACK v1\n" : "bgcom: glink -> VERSION v1\n");
    (void)glink_send(&c, sizeof c);
}

static const char *glink_name(unsigned id)
{
    static const char *n[] = { "VERSION", "VERSION_ACK", "OPEN", "CLOSE", "OPEN_ACK",
        "CLOSE_ACK", "RX_INTENT", "RX_DONE", "RX_DONE_W_REUSE", "RX_INTENT_REQ",
        "RX_INTENT_REQ_ACK", "TX_DATA", "TX_DATA_CONT", "READ_NOTIF", "SIGNALS",
        "TRACER_PKT", "TRACER_PKT_CONT", "TX_SHORT_DATA" };
    return id < sizeof n / sizeof n[0] ? n[id] : "?";
}

/* ---- channel state (one channel at a time is all we need: RSB_CTRL) ------ */
#define GLINK_CLOSE_ACK_CMD        5u
#define GLINK_RX_DONE_CMD          7u
#define GLINK_RX_DONE_W_REUSE_CMD  8u
#define GLINK_RX_INTENT_REQ_CMD    9u
#define GLINK_RX_INTENT_REQ_ACK_CMD 10u
#define GLINK_TX_SHORT_DATA_CMD    17u
#define RSB_LCID                   1u      /* our id for the RSB_CTRL channel */

#define CODEC_LCID                 2u      /* our id for the CODEC_CHANNEL    */

static unsigned s_codec_rcid;              /* the BG's id for CODEC_CHANNEL   */
static int      s_codec_open_acked;
static int      s_link_reset;      /* the BG re-sent VERSION: channels are stale */
static uint32_t s_codec_riid, s_codec_riid_addr;
static int      s_codec_have_intent, s_codec_tx_done, s_codec_rx_ready;
static uint8_t  s_codec_rx[256];
static unsigned s_codec_rx_len;
static uint32_t s_codec_rx_liid;

static unsigned s_rsb_rcid;                /* the BG's id for RSB_CTRL (9 seen) */
static int      s_rsb_open_acked;          /* BG acked our OPEN               */
static int      s_intent_req_granted;      /* BG granted our RX_INTENT_REQ    */
static uint32_t s_remote_riid, s_remote_intent_size; static int s_have_remote_intent;
static int      s_tx_done;                 /* BG consumed our data            */
static uint8_t  s_rx_data[16]; static unsigned s_rx_len; static int s_rx_ready;
static uint32_t s_rx_liid;                 /* intent id the reply landed in   */

static void glink_tx_open_ack(unsigned rcid)
{
    struct { uint16_t id, rcid, reserved1, xprt_resp; uint32_t r2, r3; } c;
    memset(&c, 0, sizeof c);
    c.id = GLINK_OPEN_ACK_CMD; c.rcid = (uint16_t)rcid;
    (void)glink_send(&c, sizeof c);
}

static void glink_tx_open(unsigned lcid, const char *name)
{
    uint8_t buf[48];
    struct { uint16_t id, lcid, length, req_xprt; uint32_t r1, r2; } c;
    unsigned n = (unsigned)strlen(name) + 1u, total;
    memset(&c, 0, sizeof c); memset(buf, 0, sizeof buf);
    c.id = GLINK_OPEN_CMD; c.lcid = (uint16_t)lcid; c.length = (uint16_t)n;
    memcpy(buf, &c, sizeof c); memcpy(buf + sizeof c, name, n);
    total = (unsigned)((sizeof c + n + 15u) & ~15u);
    con_puts("bgcom: glink -> OPEN lcid="); con_putdec(lcid); con_puts(" \""); con_puts(name); con_puts("\"\n");
    (void)glink_send(buf, total);
}

static void glink_tx_local_rx_intent(unsigned lcid, uint32_t size, uint32_t liid)
{
    struct { uint16_t id, lcid; uint32_t count; uint32_t r0, r1; uint32_t size, liid; uint32_t a0, a1; } c;
    memset(&c, 0, sizeof c);
    c.id = GLINK_RX_INTENT_CMD; c.lcid = (uint16_t)lcid; c.count = 1u; c.size = size; c.liid = liid;
    (void)glink_send(&c, sizeof c);
}

static void glink_tx_rx_intent_req(unsigned lcid, uint32_t size)
{
    struct { uint16_t id, lcid; uint32_t size; uint32_t r0, r1; } c;
    memset(&c, 0, sizeof c);
    c.id = GLINK_RX_INTENT_REQ_CMD; c.lcid = (uint16_t)lcid; c.size = (size + 3u) & ~3u;
    (void)glink_send(&c, sizeof c);
}

static void glink_tx_rx_done(unsigned lcid, uint32_t liid)
{
    struct { uint16_t id, lcid; uint32_t liid; uint32_t r0, r1; } c;
    memset(&c, 0, sizeof c);
    c.id = GLINK_RX_DONE_CMD; c.lcid = (uint16_t)lcid; c.liid = liid;
    (void)glink_send(&c, sizeof c);
}

static void glink_tx_short(unsigned lcid, uint32_t riid, const void *data, unsigned size)
{
    struct { uint16_t id, lcid; uint32_t riid, size, size_left; uint8_t data[16]; } c;
    memset(&c, 0, sizeof c);
    c.id = GLINK_TX_SHORT_DATA_CMD; c.lcid = (uint16_t)lcid; c.riid = riid; c.size = size; c.size_left = 0;
    memcpy(c.data, data, size > 16u ? 16u : size);
    (void)glink_send(&c, sizeof c);
}

/* TX_DATA: for anything longer than the 16 inline bytes. The payload goes
 * into the BG's own memory at the address the remote intent named (AHB
 * write), and the record only describes it (tx_data() in
 * glink_bgcom_xprt.c writes addr = 0 -- the BG knows where its intent is). */
static void glink_tx_data(unsigned lcid, uint32_t riid, uint32_t addr,
                          const void *data, unsigned size)
{
    struct { uint16_t id, lcid; uint32_t riid; uint32_t rsv0, rsv1;
             uint32_t size, size_left; uint32_t a0, a1; } c;
    unsigned words = (size + 3u) / 4u;
    bgcom_ahb_write(addr, words, (const uint32_t *)data);
    memset(&c, 0, sizeof c);
    c.id = 11u; c.lcid = (uint16_t)lcid; c.riid = riid;
    c.size = words * 4u; c.size_left = 0;
    (void)glink_send(&c, sizeof c);
}

static void glink_rx(const uint8_t *p, unsigned len)
{
    unsigned off = 0;
    while (off + 16u <= len) {
        struct glink_cmd c;
        memcpy(&c, p + off, sizeof c);
        off += 16u;
        con_puts("bgcom: glink <- "); con_puts(glink_name(c.id));
        con_puts(" p1="); con_puthex(c.p1); con_puts(" p2="); con_puthex(c.p2);
        con_puts(" p3="); con_puthex(c.p3); con_puts(" p4="); con_puthex(c.p4);
        if (c.id == GLINK_OPEN_CMD) {
            unsigned nlen = c.p2 & 0xFFFFu, i;
            const char *nm = (const char *)(p + off);
            con_puts(" name=\"");
            for (i = 0; i < nlen && off + i < len && p[off + i]; i++) con_putc(p[off + i]);
            con_puts("\"");
            if (nlen == 9u && off + 8u < len && memcmp(nm, "RSB_CTRL", 8) == 0) s_rsb_rcid = c.p1;
            if (nlen == 14u && off + 13u < len && memcmp(nm, "CODEC_CHANNEL", 13) == 0) s_codec_rcid = c.p1;
            off += (nlen + 15u) & ~15u;
            con_puts("\n");
            glink_tx_open_ack(c.p1);           /* the core acks every remote open */
            continue;
        }
        if (c.id == GLINK_RX_INTENT_CMD) {
            /* p2 = count; then count x { u32 size; u32 id; u64 addr } */
            unsigned i;
            for (i = 0; i < c.p2 && off + 16u <= len; i++) {
                uint32_t sz, id, addr;
                memcpy(&sz, p + off, 4); memcpy(&id, p + off + 4, 4); memcpy(&addr, p + off + 8, 4);
                off += 16u;
                con_puts(" intent{size="); con_putdec(sz); con_puts(" id="); con_putdec(id);
                con_puts(" addr="); con_puthex(addr); con_puts("}");
                if (c.p1 == s_rsb_rcid) { s_remote_riid = id; s_remote_intent_size = sz; s_have_remote_intent = 1; }
                if (s_codec_rcid && c.p1 == s_codec_rcid) {
                    s_codec_riid = id; s_codec_riid_addr = addr; s_codec_have_intent = 1;
                }
            }
            con_puts("\n");
            continue;
        }
        if (c.id == 11u || c.id == 12u) {
            /* TX_DATA / TX_DATA_CONT: p1 rcid, p2 our liid; then rx_desc
             * { u32 size; u32 size_left; u64 addr } -- data is in BG memory. */
            uint32_t sz = 0, left = 0, addr = 0;
            if (off + 16u <= len) { memcpy(&sz, p + off, 4); memcpy(&left, p + off + 4, 4); memcpy(&addr, p + off + 8, 4); }
            off += 16u;
            con_puts(" desc{size="); con_putdec(sz); con_puts(" left="); con_putdec(left);
            con_puts(" ahb="); con_puthex(addr); con_puts("}");
            if (s_codec_rcid && c.p1 == s_codec_rcid && sz > sizeof s_codec_rx)
                con_puts(" [codec reply too big to capture]");
            if (s_codec_rcid && c.p1 == s_codec_rcid && sz && sz <= sizeof s_codec_rx) {
                uint32_t w[sizeof s_codec_rx / 4];
                memset(w, 0, sizeof w);
                bgcom_ahb_read(addr, (sz + 3u) / 4u, w);
                memcpy(s_codec_rx, w, sizeof s_codec_rx);
                s_codec_rx_len = sz; s_codec_rx_liid = c.p2;
                con_puts(" data=");
                for (unsigned i = 0; i < sz; i++) con_puthex(s_codec_rx[i]);
                if (left == 0) s_codec_rx_ready = 1;
                con_puts("\n");
                continue;
            }
            if (sz && sz <= 16u) {
                uint32_t w[4] = { 0, 0, 0, 0 };
                bgcom_ahb_read(addr, (sz + 3u) / 4u, w);
                memcpy(s_rx_data, w, 16); s_rx_len = sz; s_rx_liid = c.p2;
                con_puts(" data="); for (unsigned i = 0; i < sz; i++) con_puthex(s_rx_data[i]);
                if (left == 0) s_rx_ready = 1;
            }
            con_puts("\n");
            continue;
        }
        if (c.id == GLINK_TX_SHORT_DATA_CMD) {
            /* p1 rcid, p2 liid, p3 size, p4 size_left; 16 data bytes follow */
            unsigned n = c.p3 > 16u ? 16u : c.p3, i;
            con_puts(" data=");
            for (i = 0; i < n && off + i < len; i++) con_puthex(p[off + i]);
            if (off + 16u <= len) { memcpy(s_rx_data, p + off, 16); s_rx_len = n; s_rx_liid = c.p2; s_rx_ready = 1; }
            off += 16u;
            con_puts("\n");
            continue;
        }
        con_puts("\n");
        switch (c.id) {
        case GLINK_VERSION_CMD:
            if (s_version_acked) {
                /* A second VERSION means the BG restarted its transport, so
                 * every channel we opened is stale on its side and it will
                 * never answer an intent request on one (v285). */
                con_puts("bgcom: BG RE-NEGOTIATED the link -- our channels are stale\n");
                s_link_reset = 1; s_codec_open_acked = 0; s_codec_have_intent = 0;
            }
            s_remote_version_seen = 1; glink_tx_version(1u);
            break;
        case GLINK_VERSION_ACK_CMD: s_version_acked = 1; break;
        case GLINK_OPEN_ACK_CMD:
            if (c.p1 == RSB_LCID)   s_rsb_open_acked = 1;
            if (c.p1 == CODEC_LCID) s_codec_open_acked = 1;
            break;
        case GLINK_RX_INTENT_REQ_ACK_CMD: if (c.p1 == RSB_LCID) s_intent_req_granted = (c.p2 == 1u) ? 1 : -1; break;
        case GLINK_RX_DONE_CMD:
            if (s_codec_rcid && c.p1 == s_codec_rcid) s_codec_tx_done = 1; else s_tx_done = 1;
            break;
        case GLINK_RX_DONE_W_REUSE_CMD: /* the intent stays valid for reuse */
            if (s_codec_rcid && c.p1 == s_codec_rcid) { s_codec_tx_done = 1; s_codec_have_intent = 1; }
            else { s_tx_done = 1; s_have_remote_intent = 1; }
            break;
        default: break;
        }
    }
}

/* ---- the crown as an input device ------------------------------------------
 * Same four entry points the Gen 4's rot_pat9126.c provides, so the app's
 * crown_nav.h needs no change. The BG reports ONE signed count per detent
 * (v272: value=+1 per click), so the app board header scales accordingly. */
static volatile int s_crown_accum;
static int s_crown_live;
volatile int g_bgcom_ap_bringup_done;   /* v471: AP-side link-up + crown handshake finished (or gave up) */
/* 2026-09-18: the AP is finished with the BG bus for good (codec restart included). mss_boot.c holds
 * the SSCTL "bg-wear" event -- the handoff of the bus to the modem -- until this is set. gen5-modem-27
 * sent it at +83.3 s and THEN ran the whole codec restart over the same bus, under the modem. */
volatile int g_bgcom_codec_done;
static int s_quiet;                    /* after bring-up: no per-frame prints */

int  crown_init(void)    { return s_crown_live ? 0 : -1; }
int  crown_present(void) { return s_crown_live; }
int  crown_take_delta(void)
{
    int d = s_crown_accum;
    s_crown_accum = 0;
    return d;
}

/* ---- FIFO frames ------------------------------------------------------------ */
static void frames_rx(const uint8_t *d, unsigned len)
{
    unsigned off = 0;
    while (off + 4u <= len) {
        unsigned id = d[off] | (d[off + 1] << 8);
        unsigned plen = d[off + 2] | (d[off + 3] << 8);
        off += 4u;
        if (id == 0) break;                     /* parse_fifo stops on a 0 byte */
        if (off + plen > len) { con_puts("bgcom: frame overruns buffer\n"); break; }
        if (id == 0x0001u) {
            glink_rx(d + off, plen);
        } else if (id == 0xFFFEu && plen >= 7u) {
            unsigned sub = d[off];
            uint32_t tm = d[off + 1] | (d[off + 2] << 8) | (d[off + 3] << 16) | ((uint32_t)d[off + 4] << 24);
            int16_t val = (int16_t)(d[off + 5] | (d[off + 6] << 8));
            if (!s_quiet) {
                con_puts("crown: EVENT sub="); con_putdec(sub);
                con_puts(sub == 1u ? " (rotation)" : sub == 2u ? " (button)" : "");
                con_puts(" t="); con_putdec(tm); con_puts(" value="); con_putdec((uint32_t)(int32_t)val); con_puts("\n");
            }
            if (sub == 1u && val != 0) {
#if defined(PLAT_CROWN_INVERT)
                val = (int16_t)-val;
#endif
                s_crown_accum += val;
            }
        } else {
            con_puts("bgcom: frame id="); con_puthex(id); con_puts(" len="); con_putdec(plen); con_puts("\n");
        }
        off += plen;
    }
}

/* The kernel calls bgcom_resume() before EVERY register, FIFO and AHB
 * access: re-read SLAVE_STATUS every 1 ms until bit31 says awake (the bus
 * activity itself wakes the chip), 100 retries, and it treats a failure as
 * fatal. v283 proved why -- a FIFO write issued while the BG was asleep was
 * swallowed, and from then on the BG answered every record we sent with a
 * VERSION_ACK, i.e. the to-slave stream was out of sync. So every write now
 * goes through here first. */
static int bgcom_wake(void)
{
    uint32_t r = 0;
    unsigned tries;
    bgcom_reg_read(BGCOM_REG_STATUS, 1, &r);
    if (r & (1u << 31)) return 0;
    for (tries = 0; tries < 250u && !(r & (1u << 31)); tries++) {
        timer_delay_us(1000u);
        bgcom_reg_read(BGCOM_REG_STATUS, 1, &r);
    }
    if (!s_quiet || !(r & (1u << 31))) {
        static int moaned;
        if (!s_quiet || !moaned) {
            moaned = 1;
            con_puts("bgcom: BG was asleep, ");
            con_puts((r & (1u << 31)) ? "awake after " : "STILL ASLEEP after ");
            con_putdec(tries); con_puts(" ms\n");
        }
    }
    if (!(r & (1u << 31))) {
        static int told;
        if (!told) {
            told = 1;
            con_puts("bgcom: BG will not wake: status="); con_puthex(r);
            con_puts(" bg2ap-status(gpio97)="); con_putdec((uint32_t)tlmm_in(PLAT_BG2AP_STATUS_GPIO));
            con_puts(" bg2ap-errfatal(gpio95)="); con_putdec((uint32_t)tlmm_in(PLAT_BG2AP_ERRFATAL_GPIO));
            con_puts("\n");
        }
        return -1;
    }
    return 0;
}

/* Read the status block; drain the to-master FIFO if anything waits. */
static int bgcom_poll(int verbose)
{
    uint32_t r[5];
    static uint8_t buf[512 + 4];
    unsigned used;

    if (bgcom_wake() < 0) return -1;
    bgcom_reg_read(BGCOM_REG_STATUS, 5, r);
    used = r[3] & 0xFFFFu;
    if (used) {
        /* v286 caught us consuming 2 words of a 5-word frame: FIFO_FILL can
         * name a frame the BG is still writing, and splitting one desyncs
         * the stream for good. Wait until the count stops growing. */
        uint32_t again[5];
        unsigned settle;
        for (settle = 0; settle < 10u; settle++) {
            timer_delay_us(300u);
            bgcom_reg_read(BGCOM_REG_STATUS, 5, again);
            if ((again[3] & 0xFFFFu) == used) break;
            used = again[3] & 0xFFFFu;
            r[3] = again[3];
        }
    }
    if (verbose) {
        con_puts("bgcom: status="); con_puthex(r[0]); con_puts(" autoclr="); con_puthex(r[2]);
        con_puts(" fill="); con_puthex(r[3]); con_puts(" size="); con_puthex(r[4]); con_puts("\n");
    }
    if (used == 0) return 0;
    if (used > 128u) used = 128u;
    bgcom_fifo_read(used, (uint32_t *)buf);
    buf[used * 4u] = 0;
    {
        /* Every frame starts { u16 event_id; u16 len }, so word 0 must carry
         * a known id in its low half. Anything else means we started reading
         * mid-frame and the to-master stream is out of step (v284). */
        uint32_t w0 = ((uint32_t *)buf)[0];
        unsigned id0 = w0 & 0xFFFFu;
        if (id0 != 0x0001u && id0 != 0xFFFEu && w0 != 0u) {
            con_puts("bgcom: FIFO OUT OF SYNC -- first word "); con_puthex(w0);
            con_puts(" is not a frame header (fill="); con_puthex(r[3]); con_puts(")\n");
        }
    }
    if (!s_quiet) {
        con_puts("bgcom: drained "); con_putdec(used); con_puts(" words (fill=");
        con_puthex(r[3]); con_puts("):");
        for (unsigned i = 0; i < used; i++) { con_puts(" "); con_puthex(((uint32_t *)buf)[i]); }
        con_puts("\n");
    }
    frames_rx(buf, used * 4u);
    return (int)used;
}

#if defined(AUDIO_PER_SOUND)
/* chime43: the BG bus now has more than one user after boot -- the crown poll (app task) and the
 * per-sound codec START/STOP (app task, and the probes, which run from other tasks). One owner at a
 * time. Waiters yield (vTaskDelay) so a higher-priority waiter cannot starve the holder. */
static volatile uint32_t s_bus_lock;
static int  bus_lock(int wait)
{
    while (__sync_lock_test_and_set(&s_bus_lock, 1u)) { if (!wait) return 0; vTaskDelay(1); }
    return 1;
}
static void bus_unlock(void) { __sync_lock_release(&s_bus_lock); }
#if !defined(AUDIO_CODEC_WITH_DSP)
static void speaker_service(void);
#endif
#endif

/* Main-loop poll. The BG raises bgcom-irq (gpio110) while its to-master FIFO
 * holds data, so the bus is only touched when there is something to read,
 * plus a slow heartbeat in case the line is missed. */
void crown_poll(void)
{
    static uint32_t last_ms;
    uint32_t now;
    if (!s_crown_live) return;
#if defined(MSS_BOOT)
    /* The modem has its own BG SPI driver; once it was told the BG is up (SSCTL "bg-wear"
     * AFTER_POWERUP, sent on every MSS_BOOT build) the AP must not re-mux gpio12-15 back to
     * itself with the take-back below. This used to be gated on MSS_BG_AP_RELEASE, which the
     * release flag set does not define -- so on the Gen 5 the guard was absent and the app's
     * first crown_poll() after "app starts (modem ready)" hung the watch (gen5-modem-19..24). */
    { extern volatile int g_bgcom_ap_released; if (g_bgcom_ap_released) return; }
#endif
#if defined(AUDIO_PER_SOUND) && !defined(AUDIO_CODEC_WITH_DSP)
    speaker_service();
#endif
    now = timer_ms();
    if (!tlmm_in(PLAT_BG_IRQ_GPIO) && now - last_ms < 250u) return;
    last_ms = now;
    /* v274: a TZ command after bring-up had re-muxed the pads to the QUP and
     * every read came back 0 ("still asleep"). If anyone took the pads,
     * take them back before touching the bus. */
    if (((mmio_read(0x01000000u + 0x1000u * PLAT_BG_SPI_CLK) >> 2) & 0xFu) != 0u) {
        con_puts("bgcom: pads were re-muxed away from us -- taking the bus back\n");
        bgcom_bus_take();
    }
#if defined(AUDIO_PER_SOUND)
    if (!bus_lock(0)) return;
    (void)bgcom_poll(0);
    bus_unlock();
#else
    (void)bgcom_poll(0);
#endif
}

/* ---- RSB_CTRL: the crown ---------------------------------------------------
 * bg_rsb.c: message { u32 cmd_id; u32 data } (8 B), reply intent 4 B whose
 * first byte must be 0x01. Sequence on BG-up: LDO11 1.8 V -> configure
 * (cmd 1, data 1) -> LDO15 3.0 V -> enable (cmd 2, data 1). Each message:
 * queue a local 4 B rx intent, glink_tx with REQ_INTENT (asks the BG for a
 * remote intent if none is queued), wait tx_done, wait the reply. */
static int wait_flag(volatile int *flag, uint32_t ms)
{
    uint32_t t0 = timer_ms();
    while (timer_ms() - t0 < ms) {
        if (*flag) return 1;
        { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(5000u); }
    }
    return *flag ? 1 : 0;
}

static int rsb_msg(uint32_t cmd, uint32_t data)
{
    static uint32_t liid = 0;
    uint32_t msg[2] = { cmd, data };

    s_rx_ready = 0; s_tx_done = 0;
    glink_tx_local_rx_intent(RSB_LCID, 4u, ++liid);
    if (!s_have_remote_intent) {
        s_intent_req_granted = 0;
        glink_tx_rx_intent_req(RSB_LCID, 8u);
        if (!wait_flag(&s_have_remote_intent, 2000u)) {
            con_puts("bgcom: rsb: no remote rx intent (req_ack="); con_putdec((uint32_t)s_intent_req_granted); con_puts(")\n");
            return -1;
        }
    }
    con_puts("bgcom: rsb -> cmd "); con_putdec(cmd); con_puts(" data "); con_putdec(data);
    con_puts(" (riid "); con_putdec(s_remote_riid); con_puts(")\n");
    glink_tx_short(RSB_LCID, s_remote_riid, msg, 8u);
    s_have_remote_intent = 0;
    if (!wait_flag(&s_tx_done, 2000u)) { con_puts("bgcom: rsb: no tx_done\n"); return -2; }
    if (!wait_flag(&s_rx_ready, 2000u)) { con_puts("bgcom: rsb: no reply\n"); return -3; }
    glink_tx_rx_done(RSB_LCID, s_rx_liid);
    con_puts("bgcom: rsb <- reply "); con_puthex(s_rx_data[0]);
    con_puts(s_rx_data[0] == 1u ? " (OK)\n" : " (BAD)\n");
    return s_rx_data[0] == 1u ? 0 : -4;
}

static void rsb_bringup(void)
{
    int rc;
    con_puts("bgcom: --- RSB_CTRL (crown) bring-up ---\n");
    rc = rpm_ldo_on(11u, 1800000u, 100u);
    con_puts("bgcom: pm660_l11 1.8V rc="); con_putdec((uint32_t)rc); con_puts("\n");

    glink_tx_open(RSB_LCID, "RSB_CTRL");
    if (!wait_flag(&s_rsb_open_acked, 2000u)) { con_puts("bgcom: RSB_CTRL open not acked\n"); return; }
    con_puts("bgcom: RSB_CTRL open (lcid "); con_putdec(RSB_LCID); con_puts(", rcid "); con_putdec(s_rsb_rcid); con_puts(")\n");

    if (rsb_msg(1u, 1u) != 0) { con_puts("bgcom: RSB configure FAILED\n"); return; }
    con_puts("bgcom: RSB configured\n");

    rc = rpm_ldo_on(15u, 3000000u, 100u);
    con_puts("bgcom: pm660_l15 3.0V rc="); con_putdec((uint32_t)rc); con_puts("\n");

    if (rsb_msg(2u, 1u) != 0) { con_puts("bgcom: RSB enable FAILED\n"); return; }
    con_puts("bgcom: *** RSB ENABLED -- the crown is live ***\n");
    s_crown_accum = 0;
    s_crown_live = 1;
}

/* ---- CODEC_CHANNEL: the speaker --------------------------------------------
 * Reverse-engineered from the stock vendor module audio_bg_codec.ko (pulled
 * out of gen5-vendor.img /lib/modules; notes 87-88). The amplifier is an NXP
 * TFA9897 hanging off the QCC1110, so the AP never touches it -- it only
 * sends commands down GLINK channel "CODEC_CHANNEL". PCM itself does NOT
 * come this way (it rides primary MI2S); this block is control only.
 *
 * Every command is a 12-byte packetizer header plus a payload:
 *   { u8 0x80; u8 pad; u16 opcode; u8 1; u8 1; u16 token; u32 payload_len }
 * and the BG answers with the same shape, where u16 at +2 == 2 means OK and
 * u16 at +6 echoes the token (pktzr_send_pkt / pktzr_resp_cb).
 * Opcodes: OPEN 0x21, SET_PARAMS 0x22, START 0x23, STOP 0x24, CLOSE 0x27,
 * INIT_PARAMS 0x29 (calibration), DATA 0x0C (no response).
 *
 * The stock order is: vdd-spkr (pm660_l11, 1.8 V, 100 mA) -> OPEN with the
 * 7-word stream config -> INIT_PARAMS with the calibration blob -> per
 * stream SET_PARAMS -> START. */
#define PKTZR_OPEN        0x21u
#define PKTZR_SET_PARAMS  0x22u
#define PKTZR_START       0x23u
#define PKTZR_STOP        0x24u
#define PKTZR_CLOSE       0x27u
#define PKTZR_INIT_PARAMS 0x29u

#define CODEC_CAL_LEN     3136u            /* the module always sends this much */

static uint8_t  s_codec_pkt[CODEC_CAL_LEN + 12u];
static uint16_t s_codec_token;

/* The 7-word stream config _bg_codec_hw_params sends, read off the stock module:
 *   { dai_word, rx_rate, rx_width, rx_ch, tx_rate, tx_width, tx_ch }
 * bg_cdc_hw_params builds it at bg_cdc+0x80. For dai->id 0..1 the first word is
 * hardcoded 1; only ids 2..7 index the table at .rodata 0xd00 =
 * {2,4,0x10000,0x20000,0x20000,0x10000}. So bg_cdc_rx1 AND bg_cdc_rx2 both send
 * 1 -- they are two channels of one port group, not separate bits. v6 sent 2 and
 * the BG died mid-SET_PARAMS (gen5-modem-15: "BG will not wake: status=0").
 *
 * The speaker backend is still bg_cdc_rx2: msm_bg.c's LPASS_BE_PRI_TDM_RX_1 (the
 * backend every playback path in mixer_paths_bg.xml routes to) pairs cpu dai
 * msm-dai-q6-tdm.36866 (AFE port 0x9002) with codec dai "bg_cdc_rx2". It is mono
 * (PRI_TDM_RX_0 Channels = One), so RX channels is 1.
 *
 * START's payload is 8 B { dai_word, src }, src = bg_cdc+0xe4+id*4 set by the
 * "RX_n SRC" kcontrol (bg_put_src). mixer_paths_bg.xml sets no SRC control, so
 * stock leaves it 0 -- 0 is what we send. */
#define BG_SPK_DAI_WORD 1u            /* bg_cdc_rx1/rx2 share port group 1 */
static const uint32_t s_codec_cfg[7] = { BG_SPK_DAI_WORD, 48000u, 16u, 1u, 0u, 0u, 0u };

static int codec_cmd(const char *what, unsigned opcode,
                     const void *payload, unsigned plen)
{
    static uint32_t liid = 100u;
    unsigned total = plen + 12u;
    uint32_t t0;

    if (total > sizeof s_codec_pkt) return -1;
    memset(s_codec_pkt, 0, total);
    s_codec_pkt[0] = 0x80u;
    s_codec_pkt[2] = (uint8_t)(opcode & 0xFFu);
    s_codec_pkt[3] = (uint8_t)(opcode >> 8);
    s_codec_pkt[4] = 1u; s_codec_pkt[5] = 1u;
    if (++s_codec_token == 0u) s_codec_token = 1u;
    s_codec_pkt[6] = (uint8_t)(s_codec_token & 0xFFu);
    s_codec_pkt[7] = (uint8_t)(s_codec_token >> 8);
    s_codec_pkt[8] = (uint8_t)(plen & 0xFFu);
    s_codec_pkt[9] = (uint8_t)((plen >> 8) & 0xFFu);
    if (plen) memcpy(s_codec_pkt + 12, payload, plen);

    s_codec_rx_ready = 0; s_codec_tx_done = 0;
    glink_tx_local_rx_intent(CODEC_LCID, 4096u, ++liid);

    if (s_link_reset) {
        s_link_reset = 0;
        con_puts("codec: re-opening CODEC_CHANNEL after the link reset\n");
        glink_tx_open(CODEC_LCID, "CODEC_CHANNEL");
        t0 = timer_ms();
        while (!s_codec_open_acked && timer_ms() - t0 < 3000u)
            { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(5000u); }
        con_puts(s_codec_open_acked ? "codec: re-open acked\n" : "codec: re-open NOT acked\n");
    }

    /* One request, one wait -- v280 got its intents this way. The re-ask
     * loop added in v282 only multiplied the traffic. */
    if (!s_codec_have_intent) {
        {   uint32_t st[5];
            bgcom_reg_read(BGCOM_REG_STATUS, 5, st);
            con_puts("codec: ask intent: status="); con_puthex(st[0]);
            con_puts(" (awake="); con_putdec((st[0] >> 31) & 1u);
            con_puts(" app="); con_putdec((st[0] >> 30) & 1u);
            con_puts(") fill="); con_puthex(st[3]); con_puts("\n");
        }
        glink_tx_rx_intent_req(CODEC_LCID, total);
        t0 = timer_ms();
        while (!s_codec_have_intent && timer_ms() - t0 < 2000u)
            { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(5000u); }
        if (!s_codec_have_intent) {
            con_puts("codec: "); con_puts(what); con_puts(": no remote rx intent\n");
            return -2;
        }
    }

    con_puts("codec -> "); con_puts(what); con_puts(" (opcode "); con_puthex(opcode);
    con_puts(", "); con_putdec(plen); con_puts(" B, token "); con_putdec(s_codec_token);
    con_puts(", riid "); con_putdec(s_codec_riid); con_puts(" @ "); con_puthex(s_codec_riid_addr);
    con_puts(")\n");

    if (total <= 16u) glink_tx_short(CODEC_LCID, s_codec_riid, s_codec_pkt, total);
    else              glink_tx_data(CODEC_LCID, s_codec_riid, s_codec_riid_addr, s_codec_pkt, total);
    s_codec_have_intent = 0;

    t0 = timer_ms();
    while (!s_codec_tx_done && timer_ms() - t0 < 3000u)
        { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(5000u); }
    if (!s_codec_tx_done) { con_puts("codec: no tx_done\n"); return -3; }

    t0 = timer_ms();
    while (!s_codec_rx_ready && timer_ms() - t0 < 20000u)
        { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(5000u); }
    if (!s_codec_rx_ready) { con_puts("codec: no response\n"); return -4; }
    glink_tx_rx_done(CODEC_LCID, s_codec_rx_liid);

    {
        unsigned status = (unsigned)s_codec_rx[2] | ((unsigned)s_codec_rx[3] << 8);
        unsigned token  = (unsigned)s_codec_rx[6] | ((unsigned)s_codec_rx[7] << 8);
        con_puts("codec <- "); con_puts(what); con_puts(": status "); con_putdec(status);
        con_puts(" token "); con_putdec(token);
        con_puts(status == 2u ? " (OK)\n" : " (FAIL)\n");
        if (token != s_codec_token) con_puts("codec: token mismatch\n");
        return status == 2u ? 0 : -5;
    }
}

/* v280 saw the BG consume INIT_PARAMS (RX_DONE) and then never answer, where
 * v279 answered at once -- so a missing response is worth one retry before
 * we call the sequence dead. */
static int codec_cmd_try(const char *what, unsigned opcode,
                         const void *payload, unsigned plen)
{
    int rc = codec_cmd(what, opcode, payload, plen);
    if (rc == -2 || rc == -4) {
        con_puts("codec: retrying "); con_puts(what); con_puts(" once\n");
        rc = codec_cmd(what, opcode, payload, plen);
    }
    return rc;
}

/* OPEN -> INIT_PARAMS (cal) -> SET_PARAMS -> START. 0 = the path is up. */
/* OPEN + INIT_PARAMS (the TFA9897 calibration container). Stock (bg_codec.c bg_cdc_cal_init /
 * _bg_codec_hw_params) re-sends both after every suspend/resume before the next SET_PARAMS. */
static int codec_open_cal(void)
{
    if (codec_cmd_try("OPEN", PKTZR_OPEN, s_codec_cfg, sizeof s_codec_cfg) != 0) return -1;
    /* Calibration: the stock payload is [u32 len1][cal1][u32 len2][cal2] in a 3136-byte buffer
     * (bg_cdc_cal in audio_bg_codec.ko). cal2 is the TFA9897's container -- patch, config,
     * speaker, profile and EQ. We used to send len2 = 0, i.e. an unconfigured amp. */
    {
        static uint8_t cal[CODEC_CAL_LEN];
        uint32_t off = 0, n1 = (uint32_t)sizeof k_bg_cal1, n2 = (uint32_t)sizeof k_bg_cal2;
        memset(cal, 0, sizeof cal);
        memcpy(cal + off, &n1, 4u); off += 4u;
        memcpy(cal + off, k_bg_cal1, n1); off += n1;
        memcpy(cal + off, &n2, 4u); off += 4u;
        memcpy(cal + off, k_bg_cal2, n2); off += n2;
        con_puts("codec: cal payload "); con_putdec(off); con_puts(" B of "); con_putdec((uint32_t)sizeof cal);
        con_puts(" (stock TFA9897 container)\n");
        /* 2026-09-18: .rodata has been read WRONG at runtime on this watch (gen5-modem-34/35), and
         * these tables live on the same pages. A garbled container is still acknowledged with
         * status 2 and gives a silent amp. Compare against the CRCs of the source arrays. */
        { uint32_t a = 0xFFFFFFFFu, b = 0xFFFFFFFFu;
          for (uint32_t i = 0; i < n1; i++) { a ^= k_bg_cal1[i]; for (int k = 0; k < 8; k++) a = (a >> 1) ^ (0xEDB88320u & (0u - (a & 1u))); }
          for (uint32_t i = 0; i < n2; i++) { b ^= k_bg_cal2[i]; for (int k = 0; k < 8; k++) b = (b >> 1) ^ (0xEDB88320u & (0u - (b & 1u))); }
          a = ~a; b = ~b;
          con_puts("codec: cal crc cal1 "); con_puthex(a); con_puts(a == 0x254899f4u && n1 == 20u ? " OK" : " MISMATCH");
          con_puts(" cal2 "); con_puthex(b); con_puts(b == 0x9d88b128u && n2 == 3003u ? " OK" : " MISMATCH"); con_puts(" (vs the source arrays)\n"); }
        if (codec_cmd_try("INIT_PARAMS (cal)", PKTZR_INIT_PARAMS, cal, sizeof cal) != 0) return -1;
    }
    return 0;
}
static int codec_speaker_start(void)
{
    /* spkr_en: gpio72, plain GPIO, output-high, 8 mA (triggerfish/darter DT pinctrl group
     * "spkr_en", states spkr_en_active / spkr_en_sleep). No node references it, so no kernel
     * driver drives it -- but nothing in our firmware does either, and every start so far has
     * been acknowledged yet silent. Logged before/after so the value is on record. */
    con_puts("codec: spkr_en gpio72 cfg before "); con_puthex(mmio_read(0x01000000u + 0x1000u * 72u));
    tlmm_cfg(72u, 0u, 0u, 8u, 1);
    tlmm_out(72u, 1);
    con_puts(" -> "); con_puthex(mmio_read(0x01000000u + 0x1000u * 72u)); con_puts(" (driven high)\n");

    if (codec_open_cal() != 0) return -1;

#if defined(AUDIO_CODEC_WITH_DSP)
    /* chime47: boot stops here. SET_PARAMS -> START come with every sound, right after the DSP
     * port is up (mss_apr.c), and STOP right before the DSP teardown -- the stock per-stream order. */
    con_puts("codec: OPEN + calibration done -- SET_PARAMS/START come with each sound\n");
    return 0;
#endif
    if (codec_cmd_try("SET_PARAMS (48k/16/1, bg_cdc_rx2)", PKTZR_SET_PARAMS, s_codec_cfg, sizeof s_codec_cfg) != 0) return -1;

    {
        uint32_t start[2] = { BG_SPK_DAI_WORD, 0u };   /* { dai word, SRC (stock leaves 0) } */
        if (codec_cmd_try("START", PKTZR_START, start, sizeof start) != 0) return -1;
    }

    con_puts("codec: *** the speaker path is STARTED -- the amp should be powered ***\n");
    return 0;
}

#if defined(AUDIO_PER_SOUND)
/* ---- per-sound speaker path (chime43) -------------------------------------
 * SET_PARAMS -> START before a sound, STOP SPK_TAIL_MS after the last sample (the DSP holds up
 * to 150 ms in flight), so the BG, the codec and the amp can sleep between sounds. */
#define SPK_TAIL_MS 3000u   /* chime44: 300 ms flapped STOP/START at every gap in the alarm melody */
static int s_spk_on, s_p1_pending;
static uint32_t s_spk_last_play;
static int codec_cmd_try(const char *what, unsigned opcode, const void *payload, unsigned plen);

static int spk_start(const char *why)            /* bus lock held */
{
    uint32_t start[2] = { BG_SPK_DAI_WORD, 0u }, t0 = timer_ms();
    int rc;
    if (s_spk_on) return 0;
    con_puts("codec: speaker ON for "); con_puts(why); con_puts("\n");
#if defined(AUDIO_RECAL_PER_SOUND)
    /* skipjack bg_codec.c: OPEN + calibration again before SET_PARAMS, as stock does after every
     * resume. The only sound that has ever played is the one right after the boot OPEN + cal. */
    if (codec_open_cal() != 0) con_puts("codec: per-sound OPEN + cal FAILED, trying START anyway\n");
#endif
    rc = codec_cmd_try("SET_PARAMS (48k/16/1, bg_cdc_rx2)", PKTZR_SET_PARAMS, s_codec_cfg, sizeof s_codec_cfg);
    if (rc == 0) rc = codec_cmd_try("START", PKTZR_START, start, sizeof start);
    s_spk_on = (rc == 0);
    s_spk_last_play = timer_ms();
    con_puts(rc == 0 ? "codec: speaker STARTED in " : "codec: speaker START FAILED after ");
    con_putdec(timer_ms() - t0); con_puts(" ms\n");
    return rc;
}
static void spk_stop(const char *why)            /* bus lock held */
{
    uint32_t stop[2] = { BG_SPK_DAI_WORD, 0u };
    if (!s_spk_on) return;
    con_puts("codec: speaker OFF ("); con_puts(why); con_puts(")\n");
    (void)codec_cmd_try("STOP", PKTZR_STOP, stop, sizeof stop);
    s_spk_on = 0;
}
#if defined(AUDIO_BG_KEEPALIVE)
/* chime52 (user idea): since chime48 the BG drives the TDM clock (DSP = slave). The BG sleeps when its
 * bus goes idle and nothing talks to it during playback, so the clock may stop mid-sound while the DSP
 * keeps returning WRITE_DONE. Poll it every 5 ms while the speaker is STARTed, from a task above the
 * app's priority so LVGL renders cannot starve it. Counts how often it was found asleep. */
static volatile uint32_t s_ka_polls, s_ka_asleep;
static void bg_keepalive_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_spk_on) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (bus_lock(0)) {
            uint32_t st = 0;
            bgcom_reg_read(BGCOM_REG_STATUS, 1, &st);
            s_ka_polls++;
            if (!(st & (1u << 31))) s_ka_asleep++;
            (void)bgcom_poll(0);               /* wakes it if needed and drains its FIFO */
            bus_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
static void bg_keepalive_start(void)
{
    static int started;
    if (started) return;
    started = 1;
    con_puts(xTaskCreate(bg_keepalive_task, "bg-ka", 2048, 0, 4, 0) == pdPASS
             ? "codec: BG keepalive task up (5 ms status poll while the speaker is on)\n"
             : "codec: BG keepalive task create FAILED\n");
}
#endif
int bgcom_speaker_on(const char *why)
{
    int rc;
    if (!s_codec_open_acked) return -1;
    bus_lock(1);
    rc = spk_start(why);
#if defined(AUDIO_BG_KEEPALIVE)
    s_ka_polls = 0; s_ka_asleep = 0;
    bg_keepalive_start();
#endif
    bus_unlock();
    return rc;
}
void bgcom_speaker_off(const char *why)
{
    bus_lock(1);
#if defined(AUDIO_BG_KEEPALIVE)
    if (s_spk_on) { con_puts("codec: BG keepalive: "); con_putdec(s_ka_polls); con_puts(" polls, found ASLEEP ");
                    con_putdec(s_ka_asleep); con_puts(" times during this sound\n"); }
#endif
    spk_stop(why);
    bus_unlock();
}
#if !defined(AUDIO_CODEC_WITH_DSP)
/* From crown_poll (the app's loop): START when the app has sound queued, STOP once idle. */
static void speaker_service(void)
{
    uint32_t now = timer_ms();
    if (!s_codec_open_acked || !g_bgcom_codec_done) return;
    if (q6_audio_playing()) {
        s_spk_last_play = now;
        if (!s_spk_on && bus_lock(0)) { (void)spk_start("an app sound"); bus_unlock(); }
    } else if (s_spk_on && now - s_spk_last_play > SPK_TAIL_MS && bus_lock(0)) {
        spk_stop("idle");
        bus_unlock();
    }
}
#endif
static void codec_report_body(void);
void bgcom_codec_report(void)
{
    bus_lock(1);
    codec_report_body();
    bus_unlock();
    if (s_p1_pending) { s_p1_pending = 0; q6_audio_probe(500u, "P1 after the boot STOP (fresh START per sound)"); }
}
static void codec_report_body(void)
#else
void bgcom_codec_report(void)
#endif
{
    uint32_t t0;
    int rc;

    con_puts("codec: --- CODEC_CHANNEL (speaker) control test ---\n");
    if (!s_codec_rcid) {
        t0 = timer_ms();
        while (!s_codec_rcid && timer_ms() - t0 < 3000u)
            { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(20000u); }
    }
    if (!s_codec_rcid) { con_puts("codec: the BG never opened CODEC_CHANNEL -- stop\n"); return; }
    con_puts("codec: BG's CODEC_CHANNEL rcid "); con_putdec(s_codec_rcid); con_puts("\n");

    /* vdd-spkr: pm660_l11 at 1.8 V, 100 mA (the module asks for 100 mA of
     * load the first time a stream starts). The crown already turned it on. */
    rc = rpm_ldo_on(11u, 1800000u, 100u);
    con_puts("codec: vdd-spkr (pm660_l11 1.8V) rc="); con_putdec((uint32_t)rc); con_puts("\n");

    glink_tx_open(CODEC_LCID, "CODEC_CHANNEL");
    t0 = timer_ms();
    while (!s_codec_open_acked && timer_ms() - t0 < 3000u)
        { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(5000u); }
    if (!s_codec_open_acked) { con_puts("codec: CODEC_CHANNEL open not acked -- stop\n"); return; }
    con_puts("codec: CODEC_CHANNEL open (lcid "); con_putdec(CODEC_LCID); con_puts(")\n");

    if (codec_speaker_start() != 0) return;
#if defined(AUDIO_CODEC_WITH_DSP)
    con_puts("codec: --- done (the speaker path is driven per sound from mss_apr.c) ---\n");
    return;
#endif
    /* 2026-09-17: wait for mss_apr.c to bring the DSP playback stream up (g_audio_tone_state 2), then
     * LEAVE the amp running: the app's dings and the alarm play through that stream (q6_audio_tone),
     * and re-running OPEN/START per sound would mean driving the BG bus from the app's task.
     * 2026-09-18: this is chime23's sequence again, verbatim -- the last image that made a sound.
     * chime27..36 varied the order (no restart / START after the clock / tone after START) with
     * no audible result; each of those was ONE boot against a ~30 % baseline and proved nothing. */
#if defined(AUDIO_DSP_AFTER_MODEM)
#define CODEC_STREAM_WAIT_MS 300000u   /* chime45: the DSP start waits for the modem to finish starting */
#else
#define CODEC_STREAM_WAIT_MS 90000u
#endif
    con_puts("codec: waiting for the DSP playback stream (max "); con_putdec(CODEC_STREAM_WAIT_MS / 1000u); con_puts(" s)\n");
    t0 = timer_ms();
    while (timer_ms() - t0 < 5000u || (g_audio_tone_state < 2 && timer_ms() - t0 < CODEC_STREAM_WAIT_MS))
        { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(20000u); }
    con_puts("codec: stream state "); con_putdec((uint32_t)g_audio_tone_state);
#if defined(AUDIO_PER_SOUND)
    /* chime43: stock runs SET_PARAMS -> START when a sound opens and STOP when it closes
     * (audio_bg_codec.ko hw_params/trigger); nothing keeps the path up in between. Every log
     * since 40: audible only right after a START, silent later. So: let the boot tone finish,
     * STOP, and START again for every sound (speaker_service / the probes). */
    if (g_audio_tone_state == 2) {
        con_puts(" -- DSP stream up; per-sound mode: STOP once the boot tone has played\n");
        t0 = timer_ms();
        while (q6_audio_playing() && timer_ms() - t0 < 10000u)
            { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(20000u); }
        t0 = timer_ms();
        while (timer_ms() - t0 < SPK_TAIL_MS)
            { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(20000u); }
        s_spk_on = 1;
        spk_stop("the boot tone has played");
        s_p1_pending = 1;
        con_puts("codec: --- done ---\n");
        return;
    }
#endif
#ifdef AUDIO_NO_RESTART
    /* chime39: leave the path exactly as it was when the tone played -- no STOP/re-START. */
    if (g_audio_tone_state == 2) {
        con_puts(" -- DSP stream up; speaker path LEFT RUNNING as started (no restart)\n");
        con_puts("codec: --- done ---\n");
        return;
    }
#endif
    if (g_audio_tone_state == 2) {
        uint32_t stop[2] = { BG_SPK_DAI_WORD, 0u };
        con_puts(" -- DSP stream up; restarting the speaker path so the BG starts WITH the clock\n");
        (void)codec_cmd("STOP", PKTZR_STOP, stop, sizeof stop);
        t0 = timer_ms();
        while (timer_ms() - t0 < 100u)
            { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(5000u); }
        if (codec_speaker_start() == 0) con_puts("codec: speaker path RESTARTED and left running for the app's sounds\n");
        else                            con_puts("codec: restart FAILED -- the app's sounds will be silent\n");
        con_puts("codec: --- done ---\n");
        return;
    }
    con_puts(g_audio_tone_state == 3 ? " (FAILED)" : " (never started / timed out)");
    con_puts(" -- stopping the speaker path\n");

    {
        uint32_t stop[2] = { BG_SPK_DAI_WORD, 0u };
        (void)codec_cmd("STOP", PKTZR_STOP, stop, sizeof stop);
    }
    /* No CLOSE: the stock driver only closes the channel when the module is
     * unloaded, and v279 found the BG asleep by the time we sent one. */
    con_puts("codec: --- done ---\n");
}

void bgcom_bringup_report(void)
{
    uint32_t t0;
    con_puts("bgcom: --- AP-side bgcom + GLINK link-up (v270) ---\n");
    bgcom_bus_take();
    if (bgcom_poll(1) < 0) { con_puts("bgcom: BG not awake on the bus -- stop\n"); g_bgcom_ap_bringup_done = 1; g_bgcom_codec_done = 1; return; }

    /* Our side of link-up: VERSION now, ACK whatever arrives. */
    glink_tx_version(0u);

    t0 = timer_ms();
    while (timer_ms() - t0 < 6000u) {
        { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(20000u); }
        if (s_remote_version_seen && s_version_acked) break;
    }
    con_puts("bgcom: link-up: remote VERSION "); con_puts(s_remote_version_seen ? "seen" : "NOT seen");
    con_puts(", our VERSION "); con_puts(s_version_acked ? "ACKed" : "NOT acked"); con_puts("\n");

    /* Let the BG announce its channels (v270 saw nine, RSB_CTRL last). */
    t0 = timer_ms();
    while (timer_ms() - t0 < 3000u) {
        { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(20000u); }
        if (s_rsb_rcid) break;
    }
    if (!s_rsb_rcid) { con_puts("bgcom: BG never opened RSB_CTRL -- stop\n"); g_bgcom_ap_bringup_done = 1; g_bgcom_codec_done = 1; return; }

/* The crown is OPT-IN: a board gets RSB_CTRL only if it declares
 * PLAT_HAS_ROTARY_CROWN. Forcing the bring-up on a board with no crown to turn
 * crashes it hard, so the default for an undeclared board is "no crown".
 * PLAT_NO_ROTARY_CROWN remains an explicit override, which matters because
 * boards/fossil_gen5e.h wraps the Gen 5 header and would otherwise inherit it. */
#if defined(PLAT_HAS_ROTARY_CROWN) && !defined(PLAT_NO_ROTARY_CROWN)
    rsb_bringup();
#else
    con_puts("bgcom: no rotating crown on this board -- RSB_CTRL bring-up skipped\n");
#endif
    con_puts("bgcom: --- done ---\n");

    /* From here the app owns the crown: crown_nav.h calls crown_poll() every
     * loop and crown_take_delta() scrolls/opens the shade. */
    if (s_crown_live) con_puts("crown: LIVE -- events now go to the app (scroll lists, roll down for the shade)\n");
    g_bgcom_ap_bringup_done = 1;   /* v471: mss_boot may now hand the bus to the modem (MSS_BG_AP_RELEASE) */

    /* The speaker test runs AFTER the crown is live, so the crown is never
     * waiting on it and a codec flake cannot delay or break it. */
#if defined(MSS_BOOT)
    /* v465: stock order is BG up -> modem (Q6) up -> codec. audio_bg_codec.ko refuses to start
     * ("Adsp is not loaded yet", q6core_is_adsp_ready) until the modem's Q6 is up, and since v464
     * the modem waits for the BG, so the codec test must wait for the modem, not race its load. */
    { extern int mss_ready(void); extern int mss_task_done(void);
      uint32_t m0 = timer_ms();
      while (!mss_ready() && !mss_task_done() && (uint32_t)(timer_ms() - m0) < 120000u) {
#if defined(MSS_BG_AP_RELEASE)
          timer_delay_us(100000u);   /* v466b: hands off the bus while the modem boots */
#else
          { q6_audio_service(); if (bgcom_poll(0) <= 0) timer_delay_us(100000u); }
#endif
      }
#if defined(MSS_BG_AP_RELEASE)
      con_puts("codec: MSS_BG_AP_RELEASE build -- AP leaves the BG bus to the modem, codec test SKIPPED\n");
      if (0)
#endif
      con_puts(mss_ready() ? "codec: modem is up -- running the codec test after it, as on stock\n"
                           : "codec: modem never came up -- codec test SKIPPED (it needs the modem's Q6)\n");
      if (mss_ready()) bgcom_codec_report(); }
#else
    bgcom_codec_report();
#endif

    /* Prove the link still wakes, then quiet it for the app. */
#if defined(MSS_BOOT)
    /* The modem takes the bus as soon as g_bgcom_codec_done is set, so no wake test after it. */
    s_quiet = 1;
#if defined(MSS_BG_PIN_HANDOFF)
    /* chime42: the SSCTL hand-off below told the modem the BG is its, but gpio12-15 stayed plain
     * GPIOs for our bit-bang, so the modem's QUP4 (bgcom_spi_mpss) had no path to the BG. Mux them
     * to QUP4 first. Sends nothing to the BG; sets g_bgcom_ap_released, so crown_poll() stops. */
    { extern void bg_bus_release_to_modem(void); bg_bus_release_to_modem(); }
#endif
    g_bgcom_codec_done = 1;
    con_puts("bgcom: AP done with the BG bus -- handing it to the modem, no wake test\n");
    return;
#endif
    if (s_crown_live) {
        int ok = 0;
        for (unsigned i = 0; i < 5u && !ok; i++) { ok = bgcom_poll(0) >= 0; if (!ok) timer_delay_us(200000u); }
        con_puts(ok ? "bgcom: link still wakes after the codec test\n"
                    : "bgcom: WARNING link will not wake after the codec test -- the crown is dead\n");
    }

    /* Last of all: is the PCM side reachable from the AP at all? This can hang
     * the AHB (see lpass_probe.c), so it runs only once the BG is completely
     * up -- crown live, codec proven -- and nothing depends on it returning. */
#if defined(LPASS_PROBE)
    lpass_probe_report();
#else
    /* v418: gone. The LPASS belongs to the modem's Q6, which now boots (MSS_BOOT); an AP-side
     * poke into it while the modem owns it can hang the AHB. The speaker path goes through the
     * modem from here on. */
#endif

    s_quiet = 1;
}

#endif /* PLAT_HAS_BG_QCC1110 */
