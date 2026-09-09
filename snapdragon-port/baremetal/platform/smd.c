/* smd.c — Shared Memory Driver: byte/packet channels to the other cores
 * (the RPM transport), plus the RPM "ping" that validates it.
 *
 * WHAT SMD IS: a pair of ring buffers in SMEM per channel, one per direction,
 * each with an 11-word control block (state, handshake flags, head, tail),
 * plus a doorbell: writing one bit of the APCS IPC register raises an
 * interrupt on the remote core. That is the whole transport; the RPM's
 * regulator votes and the WCNSS's WLAN control messages are both packets on
 * such a channel. Port of drivers/rpmsg/qcom_smd.c reduced to what a single
 * threaded, polling caller needs: open, send, receive. No interrupts yet —
 * the caller polls, which is fine for request/response traffic and keeps
 * the first image free of GIC plumbing.
 *
 * WHY THE RPM FIRST: it is already running (it brought the SoC up), and its
 * "rpm_requests" channel exists in the SMD table from boot (cid 4, edge 15 —
 * measured on the Gen 6). So SMD can be proved end to end BEFORE any radio
 * firmware is loaded, against a peer that is known good. rpm_diag() sends a
 * request for a resource type that does not exist; the RPM must answer with
 * an "err" message saying so. A reply proves both directions, and nothing
 * on the PMIC changed.
 *
 * LAYOUT (measured): channel info = SMEM item 14+cid, 0x58 bytes = two
 * 11-word blocks, [0] = ours (tx), [1] = theirs (rx). FIFOs = item 338+cid,
 * first half tx, second half rx. The RPM's live in the RPM message RAM
 * (SMEM aux region 0x60000), which smem_get() now follows.
 */
#include "platform.h"
#if defined(PLAT_SMEM_BASE) && defined(PLAT_APCS_IPC)

#include <string.h>

/* smd_channel_info_word field indices */
enum { I_STATE, I_DSR, I_CTS, I_CD, I_RI, I_HEAD, I_TAIL, I_STATE_F, I_BLOCKREADINTR, I_TAILP, I_HEADP, I_WORDS };

/* TWO INFO LAYOUTS (qcom_smd.c): the RPM edge uses smd_channel_info_word
 * (11 x u32 per direction, 0x58 for the pair); the WCNSS edge uses the
 * compact smd_channel_info (20 bytes: u32 state; u8 fDSR,fCTS,fCD,fRI,fHEAD,
 * fTAIL,fSTATE,fBLOCKREADINTR; u32 tail; u32 head -- 0x28 for the pair).
 * Measured on the Gen 4 (2026-09-03): WCNSS_CTRL reported info size 0x28.
 * Every field access goes through these two helpers so the rest of the
 * transport is layout-blind. */
static uint32_t ci_get(const struct smd_chan *c, volatile uint32_t *blk, int f)
{
    if (!c->byte_info) return blk[f];
    switch (f) {
    case I_STATE: return blk[0];
    case I_TAILP: return blk[3];
    case I_HEADP: return blk[4];
    default:      return ((volatile uint8_t *)blk)[4 + (f - I_DSR)];
    }
}
static void ci_set(const struct smd_chan *c, volatile uint32_t *blk, int f, uint32_t v)
{
    if (!c->byte_info) { blk[f] = v; return; }
    switch (f) {
    case I_STATE: blk[0] = v; break;
    case I_TAILP: blk[3] = v; break;
    case I_HEADP: blk[4] = v; break;
    default:      ((volatile uint8_t *)blk)[4 + (f - I_DSR)] = (uint8_t)v; break;
    }
}
#define TX_GET(f)     ci_get(c, c->tx, (f))
#define RX_GET(f)     ci_get(c, c->rx, (f))
#define TX_SET(f, v)  ci_set(c, c->tx, (f), (v))
#define RX_SET(f, v)  ci_set(c, c->rx, (f), (v))

#define SMD_CHANNEL_CLOSED   0u
#define SMD_CHANNEL_OPENING  1u
#define SMD_CHANNEL_OPENED   2u
#define SMD_PKT_HDR_WORDS    5u
#define SMEM_SMD_BASE_ID     14u
#define SMEM_SMD_FIFO_BASE_ID 338u

static inline void smd_wmb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void smd_ring(struct smd_chan *c)
{
    smd_wmb();
    mmio_write(PLAT_APCS_IPC, 1u << c->ipc_bit);
}

static void smd_set_state(struct smd_chan *c, uint32_t state)
{
    uint32_t is_open = (state == SMD_CHANNEL_OPENED);
    TX_SET(I_DSR, is_open); TX_SET(I_CTS, is_open); TX_SET(I_CD, is_open);
    TX_SET(I_STATE, state);
    TX_SET(I_STATE_F, 1);
    c->state = state;
    smd_ring(c);
}

static uint32_t smd_tx_avail(struct smd_chan *c)
{
    uint32_t mask = c->fifo_size - 1u;
    return mask - ((TX_GET(I_HEADP) - TX_GET(I_TAILP)) & mask);
}

static uint32_t smd_rx_avail(struct smd_chan *c)
{
    return (RX_GET(I_HEADP) - RX_GET(I_TAILP)) & (c->fifo_size - 1u);
}

/* FIFO copies. The RPM edge's FIFOs live in the RPM message RAM, which
 * only takes WORD accesses, and its channels are word-aligned anyway. The
 * WCNSS edge's FIFOs are ordinary SMEM in DDR and its packets are NOT
 * always word multiples (the last HAL NV fragment is 1015 bytes; the Gen 4
 * v10 run showed the firmware silently drops a packet whose SMD length is
 * padded past its HAL header length). qcom_smd.c copies byte-wise on
 * byte-layout channels and advances head/tail by the exact length; so do we. */
static void smd_copy_out(struct smd_chan *c, uint32_t off, void *dst, uint32_t n)
{
    if (c->byte_info) {
        volatile uint8_t *f = (volatile uint8_t *)c->rx_fifo; uint8_t *d = dst; uint32_t i;
        for (i = 0; i < n; i++) d[i] = f[off + i];
    } else {
        volatile uint32_t *fifo = c->rx_fifo; uint32_t *d = dst, i, w = n / 4u;
        for (i = 0; i < w; i++) d[i] = fifo[(off / 4u) + i];
        if (n & 3u) d[w] = fifo[(off / 4u) + w];   /* tail word; caller's buffer is word-sized */
    }
}

static void smd_copy_in(struct smd_chan *c, uint32_t off, const void *src, uint32_t n)
{
    if (c->byte_info) {
        volatile uint8_t *f = (volatile uint8_t *)c->tx_fifo; const uint8_t *s = src; uint32_t i;
        for (i = 0; i < n; i++) f[off + i] = s[i];
    } else {
        volatile uint32_t *fifo = c->tx_fifo; const uint32_t *s = src; uint32_t i;
        for (i = 0; i < n / 4u; i++) fifo[(off / 4u) + i] = s[i];
    }
}

static void smd_write_fifo(struct smd_chan *c, const void *data, uint32_t n)
{
    uint32_t head = TX_GET(I_HEADP);
    uint32_t len  = c->fifo_size - head;
    if (len > n) len = n;
    if (len) smd_copy_in(c, head, data, len);
    if (len != n) smd_copy_in(c, 0, (const uint8_t *)data + len, n - len);
    TX_SET(I_HEADP, (head + n) & (c->fifo_size - 1u));
}

static void smd_peek(struct smd_chan *c, void *buf, uint32_t n)
{
    uint32_t tail = RX_GET(I_TAILP);
    uint32_t len  = c->fifo_size - tail;
    if (len > n) len = n;
    if (len) smd_copy_out(c, tail, buf, len);
    if (len != n) smd_copy_out(c, 0, (uint8_t *)buf + len, n - len);
}

static void smd_advance(struct smd_chan *c, uint32_t n)
{
    RX_SET(I_TAILP, (RX_GET(I_TAILP) + n) & (c->fifo_size - 1u));
}

static int smd_wait_remote(struct smd_chan *c, uint32_t want_a, uint32_t want_b, uint32_t ms)
{
    uint32_t t0 = timer_ms();
    for (;;) {
        uint32_t rs = RX_GET(I_STATE);
        if (rs == want_a || rs == want_b) return 0;
        if (timer_ms() - t0 > ms) return -1;
        timer_delay_ms(1);
    }
}

/* Open channel `cid` on the edge whose doorbell is `ipc_bit`. The channel
 * must already exist in the SMD alloc table (the SBL/RPM create the RPM ones;
 * WCNSS creates its own after boot). `host` selects which partition the
 * items live in: SMEM_GLOBAL for the RPM edge, SMEM_HOST_WCNSS for WCNSS. */
int smd_open(struct smd_chan *c, uint32_t host, uint32_t cid, uint32_t ipc_bit)
{
    uint32_t isz = 0, fsz = 0;
    void *info, *fifo;

    memset(c, 0, sizeof *c);
    if (host == 0xFFFFu) {
        info = smem_get(SMEM_SMD_BASE_ID + cid, &isz);
        fifo = smem_get(SMEM_SMD_FIFO_BASE_ID + cid, &fsz);
    } else {
        info = smem_get_host(host, SMEM_SMD_BASE_ID + cid, &isz);
        fifo = smem_get_host(host, SMEM_SMD_FIFO_BASE_ID + cid, &fsz);
    }
    if (!info || !fifo) { con_puts("smd: no info/fifo items for cid "); con_putdec(cid); con_puts("\n"); return -1; }
    if (isz == 2u * I_WORDS * 4u)      c->byte_info = 0;
    else if (isz == 2u * 20u)          c->byte_info = 1;
    else {
        con_dbg("smd: cid "); con_dbg_dec(cid); con_dbg(" info size ");
        con_puthex(isz); con_puts(" - neither word (0x58) nor byte (0x28) layout\n");
        return -1;
    }
    if (fsz < 2u * 64u || (fsz & (fsz - 1u))) {   /* two power-of-two halves */
        con_puts("smd: odd fifo size "); con_puthex(fsz); con_puts("\n"); return -1;
    }
    c->tx = (volatile uint32_t *)info;
    c->rx = c->byte_info ? (volatile uint32_t *)((uint8_t *)info + 20u) : c->tx + I_WORDS;
    c->fifo_size = fsz / 2u;
    c->tx_fifo = (volatile uint32_t *)fifo;
    c->rx_fifo = (volatile uint32_t *)((uint8_t *)fifo + c->fifo_size);
    c->ipc_bit = ipc_bit;
    c->cid = cid;

    /* qcom_smd_channel_reset() */
    TX_SET(I_STATE, SMD_CHANNEL_CLOSED);
    TX_SET(I_DSR, 0); TX_SET(I_CTS, 0); TX_SET(I_CD, 0); TX_SET(I_RI, 0);
    TX_SET(I_HEAD, 0); TX_SET(I_TAIL, 0);
    TX_SET(I_STATE_F, 1);
    TX_SET(I_BLOCKREADINTR, 1);
    TX_SET(I_HEADP, 0);
    RX_SET(I_TAILP, 0);
    smd_ring(c);
    c->state = SMD_CHANNEL_CLOSED;

    /* qcom_smd_channel_open(): OPENING, wait, OPENED, wait. */
    smd_set_state(c, SMD_CHANNEL_OPENING);
    if (smd_wait_remote(c, SMD_CHANNEL_OPENING, SMD_CHANNEL_OPENED, 3000u) < 0) {   /* 3 s: a restarted firmware answers slower */
        con_puts("smd: remote never entered opening (state ");
        con_putdec(RX_GET(I_STATE)); con_puts(")\n");
        smd_set_state(c, SMD_CHANNEL_CLOSED);
        return -1;
    }
    smd_set_state(c, SMD_CHANNEL_OPENED);
    if (smd_wait_remote(c, SMD_CHANNEL_OPENED, SMD_CHANNEL_OPENED, 1000u) < 0) {
        con_puts("smd: remote never entered opened\n");
        smd_set_state(c, SMD_CHANNEL_CLOSED);
        return -1;
    }
    return 0;
}

/* Close our half (qcom_smd_channel_close): state CLOSED + doorbell, so a
 * restarted remote does not find us "already open" from a previous life.
 * The item pointers are kept: the restart path polls the remote state word. */
void smd_close(struct smd_chan *c)
{
    if (!c->tx) return;
    smd_set_state(c, SMD_CHANNEL_CLOSED);
    TX_SET(I_HEAD, 0); TX_SET(I_TAIL, 0); TX_SET(I_HEADP, 0);
    smd_ring(c);
}

/* One packet: 20-byte header (length word + 4 zero words) then `len` bytes,
 * len a multiple of 4. -1 if it does not fit right now. */
int smd_send(struct smd_chan *c, const void *data, uint32_t len)
{
    uint32_t hdr[SMD_PKT_HDR_WORDS] = { len, 0, 0, 0, 0 };
    uint32_t tlen = sizeof hdr + len;

    if (c->stream) tlen = len;                      /* stream channels: raw bytes, no header */
    if (c->state != SMD_CHANNEL_OPENED || tlen >= c->fifo_size) return -1;
    if (!c->byte_info && (len & 3u)) return -1;     /* word channels: word multiples only */
    if (smd_tx_avail(c) < tlen) return -1;

    TX_SET(I_TAIL, 0);
    if (!c->stream) smd_write_fifo(c, hdr, sizeof hdr);
    smd_write_fifo(c, data, len);
    TX_SET(I_HEAD, 1);
    smd_ring(c);
    return 0;
}

/* Poll for one whole packet, up to `timeout_ms`. Returns the payload length
 * copied into buf (truncated to max, rounded down to words), 0 on timeout. */
uint32_t smd_recv(struct smd_chan *c, void *buf, uint32_t max, uint32_t timeout_ms)
{
    uint32_t t0 = timer_ms();

    for (;;) {
        uint32_t avail;

        RX_SET(I_STATE_F, 0);          /* we have seen any state change */
        if (RX_GET(I_STATE) != SMD_CHANNEL_OPENED) return 0;
        RX_SET(I_HEAD, 0);             /* we have seen the new data */

        avail = smd_rx_avail(c);
        if (c->stream) {                /* whatever is there, up to max */
            if (avail) {
                uint32_t got = avail > max ? max : avail;
                smd_peek(c, buf, got);
                smd_advance(c, got);
                RX_SET(I_TAIL, 1);
                if (!RX_GET(I_BLOCKREADINTR)) smd_ring(c);
                return got;
            }
        } else
        if (!c->pkt_size && avail >= SMD_PKT_HDR_WORDS * 4u) {
            uint32_t hdr[SMD_PKT_HDR_WORDS];
            smd_peek(c, hdr, sizeof hdr);
            smd_advance(c, sizeof hdr);
            c->pkt_size = hdr[0];
            continue;
        }
        if (c->pkt_size && avail >= c->pkt_size) {
            uint32_t n = c->pkt_size, got = n > max ? (max & ~3u) : n;
            /* word channels copy a tail word; keep 3 spare bytes in the caller's buffer */
            if (!c->byte_info && got > max - 3u) got = (max - 3u) & ~3u;
            smd_peek(c, buf, got);
            smd_advance(c, n);
            c->pkt_size = 0;
            RX_SET(I_TAIL, 1);
            if (!RX_GET(I_BLOCKREADINTR)) smd_ring(c);
            return got;
        }
        if (timer_ms() - t0 > timeout_ms) return 0;
        timer_delay_ms(1);
    }
}

uint32_t smd_rx_pending(struct smd_chan *c) { return smd_rx_avail(c); }

/* ---- RPM: the first peer -------------------------------------------------
 * Message format (drivers/soc/qcom/smd-rpm.c):
 *   { 'req\0', length } { msg_id, flags(=set: 0 active), type, id, data_len }
 *   then key/value triples { key, len, value... }.
 * Reply: { 'req\0', length } then messages { 'msg#', 4, msg_id } and
 * optionally { 'err\0', n, "text" }. */
#define RPM_SERVICE_REQUEST  0x00716572u   /* "req\0" */
#define RPM_MSG_ERR          0x00727265u   /* "err\0" */
#define RPM_MSG_ID           0x2367736du   /* "msg#" */
#define RPM_EDGE_IPC_BIT     0u            /* apcs bit 0 = RPM (both SoCs) */
#define RPM_CID_RPM_REQUESTS 4u            /* measured: cid 4 edge 15 */

static struct smd_chan s_rpm;
static int s_rpm_open;
static uint32_t s_rpm_msg_id = 1;

int rpm_smd_init(void)
{
    if (s_rpm_open) return 0;
    if (smd_open(&s_rpm, 0xFFFFu, RPM_CID_RPM_REQUESTS, RPM_EDGE_IPC_BIT) < 0) return -1;
    s_rpm_open = 1;
    return 0;
}

/* Send one request and wait for its ack. `kv` is the key/value payload
 * (words). Returns 0 = accepted, -2 = RPM said "resource does not exist",
 * -3 = other RPM error (text printed), -1 = transport failure/timeout. */
int rpm_smd_request(uint32_t set, uint32_t type, uint32_t id, const uint32_t *kv, uint32_t kv_bytes)
{
    uint32_t pkt[64], rsp[64], n, i, got, status = 0;
    uint32_t my_id = s_rpm_msg_id++;

    if (!s_rpm_open || kv_bytes > sizeof pkt - 28u) return -1;
    pkt[0] = RPM_SERVICE_REQUEST;
    pkt[1] = 20u + kv_bytes;
    pkt[2] = my_id; pkt[3] = set; pkt[4] = type; pkt[5] = id; pkt[6] = kv_bytes;
    for (i = 0; i < kv_bytes / 4u; i++) pkt[7 + i] = kv[i];
    n = 28u + kv_bytes;

    if (smd_send(&s_rpm, pkt, n) < 0) { con_puts("rpm: send failed\n"); return -1; }
    got = smd_recv(&s_rpm, rsp, sizeof rsp, 2000u);
    if (got < 8u) { con_puts("rpm: no reply within 2 s\n"); return -1; }
    if (rsp[0] != RPM_SERVICE_REQUEST) { con_puts("rpm: reply is not 'req'\n"); return -1; }

    /* walk the messages */
    i = 2;
    while ((i + 2u) * 4u <= got && i * 4u < 8u + rsp[1]) {
        uint32_t mtype = rsp[i], mlen = rsp[i + 1];
        if (mtype == RPM_MSG_ID) {
            if (rsp[i + 2] != my_id) { con_puts("rpm: ack for a different msg_id\n"); }
        } else if (mtype == RPM_MSG_ERR) {
            const char *txt = (const char *)&rsp[i + 2];
            uint32_t k, lim = mlen; if (lim > 60u) lim = 60u;
            con_puts("rpm: err \"");
            for (k = 0; k < lim && txt[k]; k++) con_putc(txt[k]);
            con_puts("\" (type "); con_puthex(type); con_puts(" id "); con_putdec(id); con_puts(")\n");
            status = (lim >= 23u && !memcmp(txt, "resource does not exist", 23)) ? 2u : 3u;
        }
        i += 2u + (mlen + 3u) / 4u;
    }
    return -(int)status;
}

uint32_t smd_remote_state(struct smd_chan *c) { return RX_GET(I_STATE); }

void rpm_diag(void)
{
    static const uint32_t kv[3] = { 0x6e657773u /* "swen" */, 4u, 0u };
    int r;

    con_dbg("rpm: opening rpm_requests (cid 4) ... ");
    con_flush(); usb_poll(); timer_delay_ms(50);
    if (rpm_smd_init() < 0) { con_puts("FAILED\n"); return; }
    con_dbg("open. tx fifo "); con_dbg_hex(s_rpm.fifo_size);
    con_dbg(" B, remote state "); con_dbg_dec(ci_get(&s_rpm, s_rpm.rx, I_STATE)); con_dbg("\n");
    con_flush(); usb_poll();

    /* A request for a resource TYPE that cannot exist ("tset"). Expected
     * answer: err "resource does not exist". That is a full round trip with
     * no side effect, which is exactly what we want on the first try. */
    con_dbg("rpm: ping (type 'tset' id 0, expect 'resource does not exist')\n");
    con_flush(); usb_poll();
    r = rpm_smd_request(0u, 0x74657374u /* "tset" */, 0u, kv, sizeof kv);
    con_dbg(r == -2 ? "rpm: PING OK - SMD round trip to the RPM works\n"
           : r == 0  ? "rpm: accepted?! (unexpected, but the round trip works)\n"
           :           "rpm: ping failed\n");
    con_flush(); usb_poll();
}

#endif /* PLAT_SMEM_BASE && PLAT_APCS_IPC */
