/* mss_fastrpc.c -- FastRPC listener for the Wear 3100 modem (2026-09-15).
 *
 * WHY: on the SDW3100 watches the modem's sensor hub is the SEE framework, and
 * SEE reads its registry through FastRPC "apps_std" file calls served on the AP
 * by /vendor/bin/mdsprpcd (stock darter bugreport: 3 listener threads, started
 * 0.7 s after "modem: Brought out of reset"). The listener protocol is AP
 * initiated: the AP attaches, calls adsp_listener_init2 and then loops on
 * adsp_listener_next2, which the DSP completes only when it has a call for us.
 * Nobody polling = the sensor-hub thread blocks in its file call forever =
 * "dog.c:1522 Watchdog detects stalled initialization" (v443/v451/v455).
 *
 * STEP 1 (this file): attach, init2, poll next2 and FAIL every call with
 * AEE_EUNSUPPORTED, logging what the modem asked for. If SEE gives up cleanly
 * the modem finishes init and can sleep. Step 2 serves apps_std for real.
 *
 * Wire format = drivers/char/adsprpc.c of the stock 4.9 kernel (legacy
 * compute, no fdlist/crclist): a 40-byte smq_msg per invoke over the
 * "fastrpcsmd-apps-dsp" SMD channel, a 16-byte {ctx, retval} response; the
 * argument block lives in AP memory the DSP reads by PHYSICAL address:
 *   remote_arg64 rpra[n] {pv, len} | smq_invoke_buf list[n] {num, pgidx} |
 *   smq_phy_page pages[n] {addr, size} | args (each 128-aligned)
 * Method ids / packing from the open fastrpc user library (quic/fastrpc):
 *   attach: handle 1, sc MAKE(0,1,0), in0 = tgid
 *   adsp_listener (static handle 3): init2 = method 3, next2 = method 4
 *     next2 sc MAKE(4,2,2): in0 {prevCtx, prevResult, prevbufsLen, bufsLen}
 *     in1 prevbufs, out0 {ctx, handle, sc, bufsLenReq}, out1 bufs
 *   bufs = for each in-buf: u32 len, align 8, data; then u32 len per out-buf
 *   prevbufs = for each out-buf: u32 len, align 8, data
 *   remotectl (static handle 0): open = method 0 (name in, handle + dlerror out) */
#include "platform.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

#if defined(MSS_BOOT) && (MSS_OPEN_MASK & 2u)

#define SC_MAKE(m, in, out) ((((m) & 0x1fu) << 24) | (((in) & 0xffu) << 16) | (((out) & 0xffu) << 8))
#define SC_METHOD(sc)  (((sc) >> 24) & 0x1fu)
#define SC_INBUFS(sc)  (((sc) >> 16) & 0xffu)
#define SC_OUTBUFS(sc) (((sc) >> 8) & 0xffu)
#define AEE_EUNSUPPORTED 20
#define OUR_PID   588u          /* what mdsprpcd had; any non-zero process id */
#define BUFS_LEN  4096u
#define NARGS_MAX 8u

struct smq_msg { uint32_t pid, tid; uint64_t ctx; uint32_t handle, sc; uint64_t addr, size; };
struct smq_rsp { uint64_t ctx; int32_t retval; uint32_t pad; };
struct rarg { uint64_t pv, len; };
struct rlist { int32_t num, pgidx; };
struct rpage { uint64_t addr, size; };

/* v463: TWO listeners on the one channel. pd 0 = the guest OS (mdsprpcd's), pd 2 = the SENSORS
 * protection domain (kernel FASTRPC_INIT_ATTACH_SENSORS: fl->pd = 2, or-ed into every ctx). The
 * SEE sensor hub lives in that PD and reads AP files (/vendor/etc/sensors/...) at init; v456's
 * pd-0 listener never saw a call because the calls queue on the PD the caller runs in. */
struct lst {
    uint8_t ctxbuf[16384] __attribute__((aligned(4096)));   /* invoke metadata + copied args */
    uint8_t bufs[BUFS_LEN] __attribute__((aligned(4096)));  /* next2 out1: the DSP's call */
    uint8_t prev[BUFS_LEN] __attribute__((aligned(4096)));  /* next2 in1: our reply bufs */
    uint32_t prim_in[4] __attribute__((aligned(128)));
    uint32_t prim_out[4] __attribute__((aligned(128)));
    uint32_t tgid, pd, id;
    int state, started;
    uint32_t seq, calls, errs, t_sent;
    uint64_t ctx_sent;
};
static struct lst s_l[2] = { { .tgid = OUR_PID, .pd = 0u, .id = 0u }, { .tgid = OUR_PID + 1u, .pd = 2u, .id = 1u } };
static struct lst *L;                                           /* the instance being worked on */
#define s_ctxbuf  (L->ctxbuf)
#define s_bufs    (L->bufs)
#define s_prev    (L->prev)
#define s_prim_in (L->prim_in)
#define s_prim_out (L->prim_out)
#define s_tgid    (L->tgid)
#define s_state   (L->state)
#define s_started (L->started)
#define s_seq     (L->seq)
#define s_calls   (L->calls)
#define s_errs    (L->errs)
#define s_ctx_sent (L->ctx_sent)
#define s_t_sent  (L->t_sent)

static void say(const char *s) { con_puts(s); }
static void hex(const char *s, uint32_t v) { con_puts(s); con_puthex(v); }
static void dec(const char *s, uint32_t v) { con_puts(s); con_putdec(v); }
static void cache_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    for (; a < end; a += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void cache_inval(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < end; a += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

enum { ST_IDLE = 0, ST_ATTACH, ST_RCTL_OPEN, ST_REGISTER, ST_INIT2, ST_NEXT2, ST_DEAD };

/* 2026-09-17: the step mdsprpcd does that we did not. Its adsp_default_listener_start (vendor
 * libmdsp_default_listener.so, quic/fastrpc adsp_default_listener.c) attaches the guest OS,
 * then remote_handle_open("adsp_default_listener") -- remotectl open, static handle 0,
 * method 0 -- and calls adsp_default_listener_register() on that handle (method 0, no args).
 * The listener threads (init2/next2) alone do not make the DSP route apps_std reverse calls
 * to us: v463's listeners attached and listened but never received one call, while the
 * sensors framework never registered its QMI service (0x190) and dog.c:1522 fired. pd 0
 * (guest OS) only, as mdsprpcd. A failure is logged and the listener starts as before. */
#define DEFAULT_LISTENER_NAME "adsp_default_listener"
#define DLERR_LEN 255u                 /* as fastrpc_apps_user.c (char dlerrstr[255]); 256 got AEE_EBADPARM 0x8000040e, suspected cause */
static uint32_t s_rctl_prim_in[2] __attribute__((aligned(128)));
static uint32_t s_rctl_prim_out[2] __attribute__((aligned(128)));   /* rout handle, rout nErr */
static char s_rctl_name[sizeof DEFAULT_LISTENER_NAME] __attribute__((aligned(128))) = DEFAULT_LISTENER_NAME;
static char s_rctl_dlerr[DLERR_LEN] __attribute__((aligned(128)));
static uint32_t s_dl_handle;
static struct smd_chan *s_ch;

/* Build and send one invoke. args[i] = {ptr, len} for in then out bufs. */
static int invoke(uint32_t handle, uint32_t sc, void *const *ptr, const uint32_t *len)
{
    uint32_t n = SC_INBUFS(sc) + SC_OUTBUFS(sc);
    if (n > NARGS_MAX) return -1;
    struct rarg  *rpra  = (struct rarg *)s_ctxbuf;
    struct rlist *list  = (struct rlist *)(rpra + n);
    struct rpage *pages = (struct rpage *)(list + n);
    uint32_t meta = (uint32_t)((uint8_t *)(pages + n) - s_ctxbuf);
    uint32_t off = (meta + 127u) & ~127u;
    memset(s_ctxbuf, 0, meta);
    for (uint32_t i = 0; i < n; i++) {
        list[i].num = len[i] ? 1 : 0; list[i].pgidx = (int32_t)i;
        rpra[i].len = len[i];
        if (!len[i]) { rpra[i].pv = 0; pages[i].addr = pages[i].size = 0; continue; }
        if (off + len[i] > sizeof s_ctxbuf) return -2;
        uint8_t *a = s_ctxbuf + off;
        if (i < SC_INBUFS(sc)) memcpy(a, ptr[i], len[i]); else memset(a, 0, len[i]);
        rpra[i].pv = (uintptr_t)a;                              /* identity mapped: phys == virt */
        pages[i].addr = (uintptr_t)a & ~4095u;
        pages[i].size = ((((uintptr_t)a + len[i] - 1u) & ~4095u) - ((uintptr_t)a & ~4095u)) + 4096u;
        off = (off + len[i] + 127u) & ~127u;
    }
    cache_clean(s_ctxbuf, off);
    struct smq_msg m;
    memset(&m, 0, sizeof m);
    m.pid = s_tgid; m.tid = s_tgid;
    s_ctx_sent = 0xC0DE0000u | (L->id << 12) | ((++s_seq & 0xffu) << 4) | L->pd;   /* kernel: (ptr & ~0xFFF) | (idx << 4) | pd */
    m.ctx = s_ctx_sent; m.handle = handle; m.sc = sc;
    m.addr = (uintptr_t)s_ctxbuf; m.size = (off + 4095u) & ~4095u;
    s_t_sent = timer_ms();
    return smd_send(s_ch, &m, sizeof m);
}

/* Read back an out arg after the response (the DSP wrote our memory). */
static const uint8_t *out_arg(uint32_t sc, uint32_t idx)
{
    uint32_t n = SC_INBUFS(sc) + SC_OUTBUFS(sc);
    struct rarg *rpra = (struct rarg *)s_ctxbuf;
    if (idx >= n || !rpra[idx].len) return NULL;
    cache_inval((const void *)(uintptr_t)rpra[idx].pv, (uint32_t)rpra[idx].len);
    return (const uint8_t *)(uintptr_t)rpra[idx].pv;
}

static int send_attach(void)
{
    void *p[1] = { &s_tgid }; uint32_t l[1] = { 4u };
    s_state = ST_ATTACH;
    return invoke(1u, SC_MAKE(0, 1, 0), p, l);
}
/* remotectl open (remotectl.idl: open(in string name, rout long handle, rout sequence<char> dlerror,
 * rout long nErr)): sc MAKE(0,2,2): in0 primIn {nameLen, dlerrLen}, in1 name, out0 primROut
 * {handle, nErr}, out1 dlerror. A 4-byte out0 got AEE_EBADPARM 0x8000040e twice. */
static int send_rctl_open(void)
{
    s_rctl_prim_in[0] = sizeof s_rctl_name; s_rctl_prim_in[1] = DLERR_LEN;
    void *p[4] = { s_rctl_prim_in, s_rctl_name, s_rctl_prim_out, s_rctl_dlerr };
    uint32_t l[4] = { 8u, sizeof s_rctl_name, 8u, DLERR_LEN };
    s_state = ST_RCTL_OPEN;
    return invoke(0u, SC_MAKE(0, 2, 2), p, l);
}
static int send_register(void)
{
    s_state = ST_REGISTER;
    return invoke(s_dl_handle, SC_MAKE(0, 0, 0), NULL, NULL);
}
static int send_init2(void)
{
    s_state = ST_INIT2;
    return invoke(3u, SC_MAKE(3, 0, 0), NULL, NULL);
}
static int send_next2(uint32_t prev_ctx, int32_t prev_result, uint32_t prev_len)
{
    s_prim_in[0] = prev_ctx; s_prim_in[1] = (uint32_t)prev_result; s_prim_in[2] = prev_len; s_prim_in[3] = BUFS_LEN;
    void *p[4] = { s_prim_in, s_prev, s_prim_out, s_bufs };
    uint32_t l[4] = { 16u, prev_len, 16u, BUFS_LEN };
    s_state = ST_NEXT2;
    return invoke(3u, SC_MAKE(4, 2, 2), p, l);
}

/* The DSP's call arrived in bufs: log it, build the failed reply, send the next next2. */
static void serve_call(const uint32_t *prim, const uint8_t *bufs, uint32_t bufs_len)
{
    uint32_t ctx = prim[0], handle = prim[1], sc = prim[2], lenreq = prim[3];
    uint32_t nin = SC_INBUFS(sc), nout = SC_OUTBUFS(sc);
    s_calls++;
    dec("fastrpc[pd", L->pd); hex("]: call #", s_calls); hex(" handle 0x", handle); hex(" sc 0x", sc);
    dec(" method ", SC_METHOD(sc)); dec(" in ", nin); dec(" out ", nout); dec(" bufsLenReq ", lenreq); say("\n");
    /* unpack in-bufs (u32 len, align 8, data) -- print the first 48 bytes of each as hex + ascii */
    uint32_t pos = 0, outlen[NARGS_MAX];
    for (uint32_t i = 0; i < nin && pos + 4u <= bufs_len; i++) {
        uint32_t len; memcpy(&len, bufs + pos, 4u); pos += 4u;
        if (len) pos = (pos + 7u) & ~7u;
        say("fastrpc:   in"); con_putdec(i); dec(" len ", len); say(":");
        for (uint32_t k = 0; k < len && k < 48u && pos + k < bufs_len; k++) { static const char hx[] = "0123456789abcdef"; uint8_t b = bufs[pos + k]; char c2[3] = { hx[b >> 4], hx[b & 15], 0 }; if (!(k % 4u)) say(" "); say(c2); }
        say("  \""); for (uint32_t k = 0; k < len && k < 48u && pos + k < bufs_len; k++) { char c = (char)bufs[pos + k]; char s2[2] = { (c >= 32 && c < 127) ? c : '.', 0 }; say(s2); } say("\"\n");
        pos += len;
    }
    for (uint32_t i = 0; i < nout && i < NARGS_MAX; i++) {
        uint32_t len = 0; if (pos + 4u <= bufs_len) memcpy(&len, bufs + pos, 4u); pos += 4u; outlen[i] = len;
        say("fastrpc:   out"); con_putdec(i); dec(" len ", len); say("\n");
    }
    /* reply: for each out-buf u32 len, align 8, zeroed data (the call FAILED, the DSP ignores it) */
    uint32_t rp = 0;
    for (uint32_t i = 0; i < nout && i < NARGS_MAX; i++) {
        uint32_t len = outlen[i];
        if (rp + 4u > BUFS_LEN) break;
        memcpy(s_prev + rp, &len, 4u); rp += 4u;
        if (len) { rp = (rp + 7u) & ~7u; if (rp + len > BUFS_LEN) { len = 0; } else { memset(s_prev + rp, 0, len); rp += len; } }
    }
    int rc = send_next2(ctx, -AEE_EUNSUPPORTED, rp);
    if (rc < 0) { s_errs++; hex("fastrpc: next2 (reply) send failed rc ", (uint32_t)-rc); say("\n"); s_state = ST_DEAD; }
}

/* Called from the modem task whenever the fastrpc channel is open. Non-blocking. */
static void handle_rsp(const struct smq_rsp *rp);
void mss_fastrpc_poll(struct smd_chan *ch)
{
    s_ch = ch;
    for (unsigned i = 0; i < 2u; i++) {
        L = &s_l[i];
        if (!s_started) {
            s_started = 1;
            dec("fastrpc[pd", L->pd); say("]: listener starting (attach -> init2 -> next2 loop, every call FAILS: step 1)\n");
            if (send_attach() < 0) { say("fastrpc: attach send failed\n"); s_state = ST_DEAD; }
            return;                                  /* one attach per poll: the other instance next round */
        }
    }
    struct smq_rsp r; uint32_t g;
    while ((g = smd_recv(ch, &r, sizeof r, 0u)) != 0u) {
        if (g < 12u) { dec("fastrpc: short rsp ", g); say("\n"); continue; }
        L = NULL;
        for (unsigned i = 0; i < 2u; i++) if (s_l[i].ctx_sent == r.ctx && s_l[i].state != ST_DEAD) L = &s_l[i];
        if (!L) { hex("fastrpc: rsp ctx mismatch 0x", (uint32_t)r.ctx); say("\n"); continue; }
        handle_rsp(&r);
    }
}
static void handle_rsp(const struct smq_rsp *rp)
{
    const struct smq_rsp r = *rp;
    {
        int st = s_state;
        if (st == ST_ATTACH) {
            dec("fastrpc[pd", L->pd); hex("]: attach retval ", (uint32_t)r.retval); dec(" after ", timer_ms() - s_t_sent); say(" ms\n");
            if (r.retval != 0) { s_state = ST_DEAD; return; }
            if (L->pd == 0u) { if (send_rctl_open() < 0) s_state = ST_DEAD; return; }
            if (send_init2() < 0) { s_state = ST_DEAD; return; }
        } else if (st == ST_RCTL_OPEN) {
            const uint8_t *po = out_arg(SC_MAKE(0, 2, 2), 2u);
            const uint8_t *de = out_arg(SC_MAKE(0, 2, 2), 3u);
            uint32_t rctl_nerr = 0;
            if (po) { memcpy(&s_dl_handle, po, 4u); memcpy(&rctl_nerr, po + 4, 4u); }
            dec("fastrpc[pd", L->pd); hex("]: remotectl open \"" DEFAULT_LISTENER_NAME "\" retval ", (uint32_t)r.retval);
            hex(" handle ", s_dl_handle); hex(" nErr ", rctl_nerr);
            if (r.retval != 0 && de && de[0]) { say(" dlerror \""); char e[DLERR_LEN]; memcpy(e, de, DLERR_LEN); e[DLERR_LEN - 1u] = 0; say(e); say("\""); }
            say("\n");
            if (r.retval == 0 && rctl_nerr == 0 && send_register() >= 0) return;
            say("fastrpc: default listener not registered, starting the listener anyway\n");
            if (send_init2() < 0) s_state = ST_DEAD;
        } else if (st == ST_REGISTER) {
            dec("fastrpc[pd", L->pd); hex("]: adsp_default_listener_register retval ", (uint32_t)r.retval); say("\n");
            if (send_init2() < 0) s_state = ST_DEAD;
        } else if (st == ST_INIT2) {
            dec("fastrpc[pd", L->pd); hex("]: init2 retval ", (uint32_t)r.retval); say("\n");
            if (r.retval != 0) { s_state = ST_DEAD; return; }
            if (send_next2(0u, 0, 0u) < 0) { s_state = ST_DEAD; return; }
            dec("fastrpc[pd", L->pd); say("]: listening (next2 outstanding)\n");
        } else if (st == ST_NEXT2) {
            if (r.retval != 0) {
                s_errs++; hex("fastrpc: next2 retval ", (uint32_t)r.retval); say(" -> re-arming in 200 ms\n");
                vTaskDelay(pdMS_TO_TICKS(200));
                if (s_errs > 50u || send_next2(0u, 0, 0u) < 0) s_state = ST_DEAD;
                return;
            }
            const uint8_t *prim = out_arg(SC_MAKE(4, 2, 2), 2u);
            const uint8_t *bufs = out_arg(SC_MAKE(4, 2, 2), 3u);
            uint32_t pv[4]; memcpy(pv, prim, 16u);
            serve_call(pv, bufs, BUFS_LEN);
        }
    }
}

void mss_fastrpc_stats(void)
{
    for (unsigned i = 0; i < 2u; i++) { dec(" fastrpc pd", s_l[i].pd); dec(" st ", (uint32_t)s_l[i].state); dec(" calls ", s_l[i].calls); dec(" errs ", s_l[i].errs); }
}
#else
void mss_fastrpc_poll(struct smd_chan *ch) { (void)ch; }
void mss_fastrpc_stats(void) {}
#endif
