/* wlan_net.c — lwIP (NO_SYS, raw API) on top of the station link
 * (step 8 of WIFI-BRINGUP.md). One netif, DHCP, DNS, SNTP, and a small
 * blocking TCP client API for the Arduino-facing classes in compat/.
 *
 * THREADING: lwIP NO_SYS is single-context. Everything that touches it runs
 * under wlan_lock() (the same recursive mutex the radio uses): the poll task
 * that moves frames in and runs the timers, and the app's calls. The blocking
 * helpers below release the lock while they wait, so the poll task can
 * deliver what they are waiting for. */
#include "platform.h"
unsigned long owf_lwip_rand(void)
{
    static uint32_t x;
    x ^= (uint32_t)timer_ticks(); x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x ? x : 0x9E3779B9u;
}
#if defined(PLAT_WCNSS_FW_BASE) && defined(PLAT_SMEM_BASE) && defined(PLAT_WLAN_APP)
#include <string.h>
#include <sys/time.h>
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/apps/sntp.h"
#include "netif/ethernet.h"

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
static void say_ip(const char *s, uint32_t ip)
{
    con_dbg(s); con_dbg_dec(ip >> 24); con_dbg_c('.'); con_dbg_dec((ip >> 16) & 255u); con_dbg_c('.');
    con_dbg_dec((ip >> 8) & 255u); con_dbg_c('.'); con_dbg_dec(ip & 255u); con_flush(); usb_poll();
}

static struct netif s_nif;
static int s_lwip_inited, s_nif_added, s_task_started, s_time_synced, s_link_up;

void owf_lwip_set_time(unsigned long sec)
{
    struct timeval tv; tv.tv_sec = (time_t)sec; tv.tv_usec = 0;
    settimeofday(&tv, 0);
    s_time_synced = 1;
    say("net: SNTP set the clock\n");
}

/* ---- netif ------------------------------------------------------------- */
static err_t low_level_output(struct netif *nif, struct pbuf *p)
{
    static uint8_t buf[1600];
    uint32_t n = 0; struct pbuf *q;
    (void)nif;
    if (p->tot_len > sizeof buf) return ERR_BUF;
    for (q = p; q; q = q->next) { memcpy(buf + n, q->payload, q->len); n += q->len; }
    return wlan_sta_tx_eth(buf, n) == 0 ? ERR_OK : ERR_IF;
}
static err_t nif_init(struct netif *nif)
{
    nif->name[0] = 'w'; nif->name[1] = 'l';
    nif->output = etharp_output;
    nif->linkoutput = low_level_output;
    nif->mtu = 1500;
    nif->hwaddr_len = 6; memcpy(nif->hwaddr, wlan_mac(), 6);
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    nif->hostname = "openwatchface";
    return ERR_OK;
}
static void data_rx(const uint8_t *eth, uint32_t len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) return;
    pbuf_take(p, eth, (u16_t)len);
    if (s_nif.input(p, &s_nif) != ERR_OK) pbuf_free(p);
}
static void status_cb(struct netif *nif)
{
    if (netif_is_up(nif) && !ip4_addr_isany_val(*netif_ip4_addr(nif))) {
        say_ip("net: IP ", lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(nif))));
        say_ip("  gw ", lwip_ntohl(ip4_addr_get_u32(netif_ip4_gw(nif))));
        say_ip("  dns ", lwip_ntohl(ip4_addr_get_u32(dns_getserver(0)))); vsay("\n");
    }
}

/* ---- poll task ---------------------------------------------------------- */
static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        wlan_lock();
        if (s_link_up) { wcn36xx_rx_poll(); sys_check_timeouts(); }
        wlan_unlock();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int net_up(void)
{
    ip4_addr_t z; ip4_addr_set_zero(&z);
    wlan_lock();
    if (!s_lwip_inited) { lwip_init(); s_lwip_inited = 1; }
    if (!s_nif_added) {
        netif_add(&s_nif, &z, &z, &z, 0, nif_init, ethernet_input);
        netif_set_default(&s_nif);
        netif_set_status_callback(&s_nif, status_cb);
        s_nif_added = 1;
    }
    memcpy(s_nif.hwaddr, wlan_mac(), 6);
    wlan_sta_set_data_rx(data_rx);
    netif_set_link_up(&s_nif);
    netif_set_up(&s_nif);
    s_link_up = 1;
    wlan_sta_tx_probe();
    vsay("net: DHCP discover ...\n");
    dhcp_start(&s_nif);
    if (!s_task_started) {
        xTaskCreate(net_task, "wlan-net", 4096, 0, 2, 0);
        s_task_started = 1;
    }
    wlan_unlock();
    return net_wait_ip(15000u) ? 0 : -1;
}

void net_down(void)
{
    wlan_lock();
    if (s_nif_added && s_link_up) {
        sntp_stop();
        dhcp_release_and_stop(&s_nif);
        netif_set_down(&s_nif);
        netif_set_link_down(&s_nif);
    }
    s_link_up = 0;
    wlan_sta_set_data_rx(0);
    wlan_unlock();
}

int net_has_ip(void)
{
    int r; wlan_lock();
    r = s_link_up && netif_is_up(&s_nif) && !ip4_addr_isany_val(*netif_ip4_addr(&s_nif));
    wlan_unlock(); return r;
}
uint32_t net_ip(void)
{
    uint32_t ip = 0; wlan_lock();
    if (s_link_up) ip = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&s_nif)));
    wlan_unlock(); return ip;
}
int net_wait_ip(uint32_t ms)
{
    uint32_t t = timer_ms(), last = 0;
    while (timer_ms() - t < ms) {
        if (net_has_ip()) return 1;
        if (timer_ms() - t - last >= 3000u) {
            uint32_t tx, rx, ro; last = timer_ms() - t;
            wlan_sta_counters(&tx, &rx, &ro);
            con_dbg("net: waiting for DHCP: data tx "); con_dbg_dec(tx); con_dbg(" rx "); con_dbg_dec(rx); con_dbg(" rx-other "); con_dbg_dec(ro); con_dbg("\n"); con_flush(); usb_poll();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    say("net: no DHCP lease\n");
    return 0;
}

/* ---- DNS ---------------------------------------------------------------- */
struct dns_wait { volatile int done; uint32_t ip; };
static void dns_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    struct dns_wait *w = arg; (void)name;
    w->ip = addr ? lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr))) : 0; w->done = 1;
}
/* v196: the update check runs BLOCKING on the UI task. Every wait in the
 * network/TLS/HTTP path petted the hardware watchdog but never the 30 s
 * dead-man timer (reboot_msm.c), and never serviced the USB console: a slow
 * GitHub fetch rebooted the watch to the bootloader with an empty log. */
void net_keepalive(void)
{
    wdog_pet();
    deadman_kick();
    usb_poll();
}

int net_dns(const char *host, uint32_t *ip, uint32_t ms)
{
    struct dns_wait w = { 0, 0 }; ip_addr_t a; err_t e; uint32_t t;
    if (ip4addr_aton(host, ip_2_ip4(&a))) { *ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&a))); return 0; }
    wlan_lock();
    e = dns_gethostbyname(host, &a, dns_cb, &w);
    wlan_unlock();
    if (e == ERR_OK) { *ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&a))); return 0; }
    if (e != ERR_INPROGRESS) return -1;
    for (t = timer_ms(); !w.done && timer_ms() - t < ms; ) { net_keepalive(); vTaskDelay(pdMS_TO_TICKS(10)); }
    if (!w.done || !w.ip) { say("net: DNS failed for "); say(host); say("\n"); return -1; }
    *ip = w.ip;
    return 0;
}

/* ---- SNTP --------------------------------------------------------------- */
void net_sntp_start(const char *s1, const char *s2)
{
    wlan_lock();
    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    if (s1 && *s1) sntp_setservername(0, s1);
    if (s2 && *s2) sntp_setservername(1, s2);
    sntp_init();
    wlan_unlock();
    vsay("net: SNTP started ("); vsay(s1 ? s1 : "?"); vsay(")\n");
}
int net_time_synced(void) { return s_time_synced; }

/* ---- TCP client --------------------------------------------------------- */
#define TCP_RX_RING 65536u          /* v203: 4x TCP_WND; the window is only re-opened as the app drains */
struct net_tcp {
    struct tcp_pcb *pcb;
    uint8_t rx[TCP_RX_RING]; uint32_t rh, rt;    /* ring head/tail */
    volatile int connected, closed, err;
    uint32_t sent_ack;                            /* bytes acknowledged */
};
static struct net_tcp s_conn[3];

/* v203 FLOW CONTROL. The old callback acknowledged every packet to lwIP the
 * moment it arrived (tcp_recved here) and DROPPED bytes when the 16 KB ring
 * was full. A TLS download drains the ring in 16 KB records slower than the
 * link fills it, so every multi-hundred-KB transfer lost bytes at ~200-400 KB,
 * the TLS stream went bad and the connection died (the OTA install). Now:
 * a packet the ring cannot hold is refused (ERR_MEM, lwIP keeps it and
 * redelivers), and the window is re-opened from net_tcp_read() as the app
 * actually consumes the bytes. */
static uint32_t ring_free(const struct net_tcp *c) { return (c->rh + TCP_RX_RING - c->rt - 1u) % TCP_RX_RING; }
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct net_tcp *c = arg; struct pbuf *q; uint32_t i;
    (void)err; (void)pcb;
    if (!p) { c->closed = 1; return ERR_OK; }
    if (ring_free(c) < p->tot_len) return ERR_MEM;                 /* not consumed: lwIP retries later */
    for (q = p; q; q = q->next)
        for (i = 0; i < q->len; i++) { c->rx[c->rt] = ((uint8_t *)q->payload)[i]; c->rt = (c->rt + 1u) % TCP_RX_RING; }
    pbuf_free(p);
    return ERR_OK;
}
static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) { struct net_tcp *c = arg; (void)pcb; c->sent_ack += len; return ERR_OK; }
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) { struct net_tcp *c = arg; (void)pcb; if (err == ERR_OK) c->connected = 1; else c->err = 1; return ERR_OK; }
static void tcp_err_cb(void *arg, err_t err) { struct net_tcp *c = arg; (void)err; c->pcb = 0; c->err = 1; c->closed = 1; }

void *net_tcp_connect(uint32_t ip, uint16_t port, uint32_t ms)
{
    struct net_tcp *c = 0; ip_addr_t a; uint32_t i, t;
    for (i = 0; i < 3u; i++) if (!s_conn[i].pcb && !s_conn[i].connected) { c = &s_conn[i]; break; }
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    ip_addr_set_ip4_u32(&a, lwip_htonl(ip));
    wlan_lock();
    c->pcb = tcp_new();
    if (!c->pcb) { wlan_unlock(); return 0; }
    tcp_arg(c->pcb, c); tcp_recv(c->pcb, tcp_recv_cb); tcp_sent(c->pcb, tcp_sent_cb); tcp_err(c->pcb, tcp_err_cb);
    if (tcp_connect(c->pcb, &a, port, tcp_connected_cb) != ERR_OK) { tcp_abort(c->pcb); c->pcb = 0; wlan_unlock(); return 0; }
    wlan_unlock();
    for (t = timer_ms(); !c->connected && !c->err && timer_ms() - t < ms; ) { net_keepalive(); vTaskDelay(pdMS_TO_TICKS(5)); }
    if (!c->connected) { net_tcp_close(c); return 0; }
    return c;
}
int net_tcp_write(void *h, const void *data, uint32_t len, uint32_t ms)
{
    struct net_tcp *c = h; const uint8_t *p = data; uint32_t t = timer_ms(), done = 0;
    if (!c || !c->pcb || c->closed) return -1;
    while (done < len) {
        uint32_t room, n; err_t e;
        wlan_lock();
        if (!c->pcb) { wlan_unlock(); return -1; }
        room = tcp_sndbuf(c->pcb); n = len - done; if (n > room) n = room;
        e = n ? tcp_write(c->pcb, p + done, (u16_t)n, TCP_WRITE_FLAG_COPY) : ERR_OK;
        if (e == ERR_OK && n) { done += n; tcp_output(c->pcb); }
        wlan_unlock();
        if (e != ERR_OK && e != ERR_MEM) return -1;
        if (done < len) { if (timer_ms() - t > ms) return -1; net_keepalive(); vTaskDelay(pdMS_TO_TICKS(5)); }
    }
    return (int)done;
}
int net_tcp_available(void *h)
{
    struct net_tcp *c = h; int n; if (!c) return 0;
    wlan_lock(); n = (int)((c->rt + TCP_RX_RING - c->rh) % TCP_RX_RING); wlan_unlock();
    return n;
}
int net_tcp_read(void *h, void *buf, uint32_t max)
{
    struct net_tcp *c = h; uint8_t *d = buf; uint32_t n = 0;
    if (!c) return -1;
    wlan_lock();
    while (n < max && c->rh != c->rt) { d[n++] = c->rx[c->rh]; c->rh = (c->rh + 1u) % TCP_RX_RING; }
    if (n && c->pcb) tcp_recved(c->pcb, (u16_t)(n > 0xFFFFu ? 0xFFFFu : n));   /* re-open the window for what was consumed */
    wlan_unlock();
    if (n) return (int)n;
    return c->closed ? -1 : 0;
}
int net_tcp_connected(void *h)
{
    struct net_tcp *c = h; if (!c) return 0;
    return (c->connected && !c->closed) || net_tcp_available(h) > 0;
}
void net_tcp_close(void *h)
{
    struct net_tcp *c = h; if (!c) return;
    wlan_lock();
    if (c->pcb) {
        tcp_arg(c->pcb, 0); tcp_recv(c->pcb, 0); tcp_sent(c->pcb, 0); tcp_err(c->pcb, 0);
        if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
        c->pcb = 0;
    }
    c->connected = 0; c->closed = 1;
    wlan_unlock();
    /* slot is free once no pcb references it */
    c->connected = 0;
}

#else
#include <sys/time.h>
void owf_lwip_set_time(unsigned long sec) { struct timeval tv; tv.tv_sec = (time_t)sec; tv.tv_usec = 0; settimeofday(&tv, 0); }
int net_up(void) { return -1; }
void net_down(void) {}
int net_has_ip(void) { return 0; }
uint32_t net_ip(void) { return 0; }
int net_wait_ip(uint32_t ms) { (void)ms; return 0; }
int net_dns(const char *h, uint32_t *ip, uint32_t ms) { (void)h; (void)ip; (void)ms; return -1; }
void net_sntp_start(const char *a, const char *b) { (void)a; (void)b; }
int net_time_synced(void) { return 0; }
void *net_tcp_connect(uint32_t ip, uint16_t port, uint32_t ms) { (void)ip; (void)port; (void)ms; return 0; }
int net_tcp_write(void *h, const void *d, uint32_t l, uint32_t ms) { (void)h; (void)d; (void)l; (void)ms; return -1; }
int net_tcp_read(void *h, void *b, uint32_t m) { (void)h; (void)b; (void)m; return -1; }
int net_tcp_available(void *h) { (void)h; return 0; }
int net_tcp_connected(void *h) { (void)h; return 0; }
void net_tcp_close(void *h) { (void)h; }
#endif
