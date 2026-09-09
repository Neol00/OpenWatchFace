/* bt_hci.c — Bluetooth HCI transport over SMD (WCN3620 on msm8909w).
 *
 * The Bluetooth core inside WCNSS talks plain HCI over two SMD channels on
 * the WCNSS edge: APPS_RIVA_BT_CMD carries commands out and events back,
 * APPS_RIVA_BT_ACL carries ACL data both ways (the Gen 4 device tree lists
 * both as smdtty ports; Android's libbt-vendor opens /dev/smd2 + /dev/smd3).
 * No H4 packet-type byte: the channel identifies the packet type, exactly
 * like the kernel's hci_smd.c. The channels can be packet (alloc flag
 * 0x200) or stream; both are handled -- a stream is re-framed here from the
 * HCI headers (event: 2 B + len, ACL: 4 B + len16).
 *
 * The firmware is the same WCNSS image WiFi loads, so bt_hci_open() boots it
 * through wlan_up() when it is not resident yet.
 */
#include "platform.h"
#if defined(PLAT_WCNSS_FW_BASE) && defined(PLAT_SMEM_BASE)
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

#define WCNSS_IPC_BIT 17u
static struct smd_chan s_cmd, s_acl;
static int s_open;
static void (*s_evt_cb)(const uint8_t *, uint32_t);
static void (*s_acl_cb)(const uint8_t *, uint32_t);

/* re-framing buffers (stream channels deliver arbitrary chunks) */
static uint8_t  s_evt_buf[260];  static uint32_t s_evt_n;
static uint8_t  s_acl_buf[1024 + 4]; static uint32_t s_acl_n;

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
static void hexdump(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && i < 48u; i++) {
        static const char h[] = "0123456789abcdef";
        con_dbg_c(h[p[i] >> 4]); con_dbg_c(h[p[i] & 15u]); con_dbg_c(' ');
    }
    if (n > 48u) con_dbg("...");
}

static int open_one(const char *name, struct smd_chan *c)
{
    uint32_t cid = 0, flags = 0, t0 = timer_ms(), last = 0xFFFFFFFFu;
    for (;;) {
        if (wcnss_channel_lookup(name, &cid, &flags) == 0) break;
        if (timer_ms() - t0 > 5000u) {
            con_dbg("bt: no "); con_dbg(name); con_dbg(" channel after 5 s; table:\n");
            wcnss_list_channels();
            return -1;
        }
        if (timer_ms() - t0 > last + 1000u || last == 0xFFFFFFFFu) { last = timer_ms() - t0; }
        timer_delay_ms(50);
    }
    con_dbg("bt: "); con_dbg(name); con_dbg(" cid "); con_dbg_dec(cid);
    con_dbg((flags & 0x200u) ? " (packet) ... " : " (stream) ... ");
    if (smd_open(c, SMEM_HOST_WCNSS, cid, WCNSS_IPC_BIT) < 0) { say("open FAILED\n"); return -1; }
    c->stream = (flags & 0x200u) ? 0u : 1u;
    con_dbg("open, fifo "); con_dbg_hex(c->fifo_size); vsay("\n");
    return 0;
}

int bt_hci_is_open(void) { return s_open; }
/* The WCNSS firmware is gone (PAS shutdown): forget the SMD channels so the
 * next bt_hci_open() re-boots the subsystem instead of writing into a void. */
void bt_hci_drop(void) { if (s_open) { smd_close(&s_cmd); smd_close(&s_acl); } s_open = 0; s_evt_n = s_acl_n = 0; }

int bt_hci_open(void)
{
    if (s_open) return 0;
    if (!wcnss_fw_resident()) {
        vsay("bt: WCNSS firmware not loaded -- bringing it up\n");
        if (wlan_up() != 0) { say("bt: WCNSS bring-up failed\n"); return -1; }
    }
    if (open_one("APPS_RIVA_BT_CMD", &s_cmd) < 0) return -1;
    if (open_one("APPS_RIVA_BT_ACL", &s_acl) < 0) return -1;
    s_evt_n = s_acl_n = 0;
    s_open = 1;
    say("bt: HCI transport open\n");
    return 0;
}

int bt_hci_send_cmd(const uint8_t *pkt, uint32_t len)
{
    uint32_t t0 = timer_ms();
    if (!s_open) return -1;
    while (smd_send(&s_cmd, pkt, len) < 0) {
        if (timer_ms() - t0 > 1000u) { say("bt: cmd send timeout\n"); return -1; }
        timer_delay_ms(1);
    }
    return 0;
}

int bt_hci_send_acl(const uint8_t *pkt, uint32_t len)
{
    uint32_t t0 = timer_ms();
    if (!s_open) return -1;
    while (smd_send(&s_acl, pkt, len) < 0) {
        if (timer_ms() - t0 > 1000u) { say("bt: acl send timeout\n"); return -1; }
        timer_delay_ms(1);
    }
    return 0;
}

void bt_hci_set_rx(void (*evt)(const uint8_t *, uint32_t), void (*acl)(const uint8_t *, uint32_t))
{
    s_evt_cb = evt; s_acl_cb = acl;
}

/* feed bytes through the re-framer; hdr = header length, lenoff/len16 describe the length field */
static void feed(uint8_t *buf, uint32_t *n, uint32_t cap, const uint8_t *d, uint32_t dn,
                 uint32_t hdr, int len16, void (*cb)(const uint8_t *, uint32_t))
{
    while (dn) {
        uint32_t need, take;
        if (*n < hdr) need = hdr - *n;
        else {
            uint32_t plen = len16 ? (uint32_t)buf[hdr - 2] | ((uint32_t)buf[hdr - 1] << 8) : buf[hdr - 1];
            need = hdr + plen - *n;
        }
        take = need < dn ? need : dn;
        if (*n + take > cap) { *n = 0; return; }       /* garbage: resync */
        memcpy(buf + *n, d, take); *n += take; d += take; dn -= take;
        if (*n >= hdr) {
            uint32_t plen = len16 ? (uint32_t)buf[hdr - 2] | ((uint32_t)buf[hdr - 1] << 8) : buf[hdr - 1];
            if (*n == hdr + plen) { if (cb) cb(buf, *n); *n = 0; }
        }
    }
}

void bt_hci_poll(void)
{
    static uint8_t tmp[1028];
    uint32_t got;
    if (!s_open) return;
    /* smd_recv with timeout 0 still sleeps a millisecond when nothing is there;
     * look first so an idle poll costs nothing */
    while (smd_rx_pending(&s_cmd) && (got = smd_recv(&s_cmd, tmp, sizeof tmp, 0u)) != 0)
        feed(s_evt_buf, &s_evt_n, sizeof s_evt_buf, tmp, got, 2u, 0, s_evt_cb);
    while (smd_rx_pending(&s_acl) && (got = smd_recv(&s_acl, tmp, sizeof tmp, 0u)) != 0)
        feed(s_acl_buf, &s_acl_n, sizeof s_acl_buf, tmp, got, 4u, 1, s_acl_cb);
}

/* ---- synchronous command (diag / early bring-up only) ------------------ */
static uint8_t  s_sync_evt[260]; static uint32_t s_sync_n; static uint16_t s_sync_op;
static void sync_evt(const uint8_t *p, uint32_t n)
{
    /* 0x0E Command Complete {ncmd, opcode16, status...}, 0x0F Command Status {status, ncmd, opcode16} */
    uint16_t op = 0;
    if (p[0] == 0x0E && n >= 5u) op = (uint16_t)(p[3] | (p[4] << 8));
    else if (p[0] == 0x0F && n >= 6u) op = (uint16_t)(p[4] | (p[5] << 8));
    else { con_dbg("bt:   event 0x"); con_dbg_hex(p[0]); con_dbg(" len "); con_dbg_dec(n - 2u); con_dbg(": "); hexdump(p + 2, n - 2u); vsay("\n"); return; }
    if (op == s_sync_op && !s_sync_n) { memcpy(s_sync_evt, p, n); s_sync_n = n; }
}

uint32_t bt_hci_cmd_wait(const uint8_t *cmd, uint32_t len, uint8_t *evt, uint32_t max, uint32_t ms)
{
    void (*old)(const uint8_t *, uint32_t) = s_evt_cb;
    uint32_t t0 = timer_ms(), n;
    s_sync_op = (uint16_t)(cmd[0] | (cmd[1] << 8)); s_sync_n = 0;
    s_evt_cb = sync_evt;
    if (bt_hci_send_cmd(cmd, len) < 0) { s_evt_cb = old; return 0; }
    while (!s_sync_n && timer_ms() - t0 < ms) { bt_hci_poll(); timer_delay_ms(1); }
    s_evt_cb = old;
    n = s_sync_n > max ? max : s_sync_n;
    if (n) memcpy(evt, s_sync_evt, n);
    return n;
}

static int cmd(const char *what, const uint8_t *c, uint32_t len, uint8_t *evt, uint32_t max)
{
    uint32_t n;
    con_dbg("bt: "); con_dbg(what); con_dbg(" ... ");
    n = bt_hci_cmd_wait(c, len, evt, max, 2000u);
    if (!n) { vsay("NO REPLY within 2 s\n"); return -1; }
    if (evt[0] == 0x0E) { con_dbg("status "); con_dbg_hex(evt[5]); con_dbg(" ["); hexdump(evt + 6, n - 6u); vsay("]\n"); return evt[5] ? -1 : 0; }
    con_dbg("cmd status "); con_dbg_hex(evt[2]); vsay("\n");
    return evt[2] ? -1 : 0;
}

void bt_hci_diag(void)
{
    static uint8_t evt[260];
    static const uint8_t reset[]   = { 0x03, 0x0C, 0x00 };
    static const uint8_t version[] = { 0x01, 0x10, 0x00 };
    static const uint8_t bdaddr[]  = { 0x09, 0x10, 0x00 };
    static const uint8_t le_feat[] = { 0x03, 0x20, 0x00 };
    static const uint8_t adv_par[] = { 0x06, 0x20, 0x0F, 0x40,0x01, 0x40,0x01, 0x00, 0x00, 0x00, 0,0,0,0,0,0, 0x07, 0x00 };
    /* flags + complete local name "OWF Gen4" */
    static const uint8_t adv_dat[] = { 0x08, 0x20, 0x20, 14, 0x02,0x01,0x06, 0x09,0x09,'O','W','F',' ','G','e','n','4',
                                       0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    static const uint8_t adv_en[]  = { 0x0A, 0x20, 0x01, 0x01 };
    uint32_t t0;

    vsay("bt: ---- HCI self-test ----\n");
    if (bt_hci_open() < 0) return;
    if (cmd("HCI_Reset", reset, sizeof reset, evt, sizeof evt) < 0) return;
    if (cmd("Read_Local_Version", version, sizeof version, evt, sizeof evt) == 0)
        { con_dbg("bt:   HCI ver "); con_dbg_dec(evt[6]); con_dbg(" rev "); con_dbg_dec(evt[7] | (evt[8] << 8));
          con_dbg(" LMP ver "); con_dbg_dec(evt[9]); con_dbg(" manuf "); con_dbg_dec(evt[10] | (evt[11] << 8)); vsay("\n"); }
    if (cmd("Read_BD_ADDR", bdaddr, sizeof bdaddr, evt, sizeof evt) == 0) {
        int i; con_dbg("bt:   BD_ADDR ");
        for (i = 5; i >= 0; i--) { con_dbg_hex(evt[6 + i]); if (i) con_dbg_c(':'); }
        vsay("\n");
    }
    cmd("LE_Read_Local_Supported_Features", le_feat, sizeof le_feat, evt, sizeof evt);
    if (cmd("LE_Set_Advertising_Parameters", adv_par, sizeof adv_par, evt, sizeof evt) < 0) return;
    if (cmd("LE_Set_Advertising_Data", adv_dat, sizeof adv_dat, evt, sizeof evt) < 0) return;
    if (cmd("LE_Set_Advertise_Enable", adv_en, sizeof adv_en, evt, sizeof evt) < 0) return;
    vsay("bt: *** ADVERTISING as \"OWF Gen4\" for 30 s -- look for it in a BLE scanner ***\n");
    t0 = timer_ms();
    s_evt_cb = sync_evt; s_sync_op = 0xFFFF;
    while (timer_ms() - t0 < 30000u) { bt_hci_poll(); timer_delay_ms(5); wdog_pet(); }
    s_evt_cb = 0;
    vsay("bt: ---- self-test done (still advertising) ----\n");
}

#else
int  bt_hci_open(void) { return -1; }
int  bt_hci_is_open(void) { return 0; }
int  bt_hci_send_cmd(const uint8_t *p, uint32_t n) { (void)p; (void)n; return -1; }
int  bt_hci_send_acl(const uint8_t *p, uint32_t n) { (void)p; (void)n; return -1; }
void bt_hci_set_rx(void (*e)(const uint8_t *, uint32_t), void (*a)(const uint8_t *, uint32_t)) { (void)e; (void)a; }
void bt_hci_poll(void) {}
void bt_hci_drop(void) {}
uint32_t bt_hci_cmd_wait(const uint8_t *c, uint32_t l, uint8_t *e, uint32_t m, uint32_t t) { (void)c;(void)l;(void)e;(void)m;(void)t; return 0; }
void bt_hci_diag(void) {}
#endif
