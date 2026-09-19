/* mss_rmtfs.c -- QMI remote-storage server for the modem (stock: the rmt_storage daemon).
 *
 * The modem's EFS lives in the eMMC partitions modemst1 / modemst2 / fsg / fsc and the
 * modem reads and writes them THROUGH the application processor: QMI service 14
 * ("remotefs"/RMTFS) instance 1, announced by us on node 1 port 0x4000 over the IPC
 * router (mss_boot.c). Without an answer the modem's init parks right after SSCTL and
 * never reaches its sleep task (MPSS shutdowns stay 0 forever, v351..v388).
 *
 * Wire format (QMI, little endian, as in the Linaro rmtfs and the QC rmt_storage daemon):
 *   request  : flags 0, txn u16, msg u16, len u16, TLVs {type u8, len u16, value}
 *   response : flags 2, same txn/msg, TLV 2 = {result u16, error u16}, then optionals
 *   1 OPEN         req  TLV1 path (raw chars)            resp 0x10 caller_id u32
 *   2 CLOSE        req  TLV1 caller_id u32
 *   3 RW_IOVEC     req  TLV1 caller_id, TLV2 direction u8 (0 read 1 write),
 *                       TLV3 u8 count + {sector_addr u32, data_phy_addr_offset u32,
 *                       num_sector u32}[], TLV4 is_force_sync u8
 *   4 ALLOC_BUFF   req  TLV1 caller_id, TLV2 buff_size u32   resp 0x10 buff_address u64
 *   5 GET_DEV_ERR  req  TLV1 caller_id                        resp 0x10 status u8
 * Data moves through the STATIC shared buffer the stock DT reserves for exactly this
 * (qcom,rmtfs_sharedmem@87c00000, 0xE0000; mapped Device/uncached by mmu.c), offsets in
 * RW_IOVEC are relative to that buffer's base; every caller is handed the base (Linaro
 * behaviour, proven against real modems). Sector = 512 B = one eMMC block.
 *
 * EFS WRITE RULE (user, 2026-09-13): the modem may write its partitions only when
 * OpenWatchFace is flashed in `boot` (owf_is_flashed() == 1). Otherwise writes are
 * acknowledged but NOT performed (the modem's copy stays in its RAM; the next boot re-reads
 * flash). The eMMC layer enforces it a second time: the modem write window is armed only
 * here, only once, only over those four partitions. */
#include <stdint.h>
#include <string.h>
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"
/* per-board sensor registry data (tools/gen_snsreg.py from each persist backup + vendor sensors.qti) */
#if defined(PLAT_BOARD_FOSSIL_GEN4)
#include "sns_reg_gen4.h"
#elif defined(PLAT_BOARD_FOSSIL_DARTER)
#include "sns_reg_darter.h"
#elif defined(PLAT_BOARD_FOSSIL_GEN5E)
#include "sns_reg_gen5e.h"
#elif defined(PLAT_BOARD_FOSSIL_GEN5)
#include "sns_reg_gen5.h"
#elif defined(PLAT_BOARD_TICWATCH_S2)
#include "sns_reg_s2.h"
#else
#include "sns_reg_c2.h"
#endif

#define RMTFS_MEM_BASE  0x87c00000u
#define RMTFS_MEM_SIZE  0x000E0000u
#define RMTFS_SECTOR    512u
#define RMTFS_PORT      0x4000u
#define RFSA_PORT       0x4011u
#define MEMSHARE_PORT   0x400du
#define RMTFS_NODE_APPS 1u

int owf_is_flashed(void);

struct rmtfs_file { uint32_t lba, nblk; int open; };
static struct rmtfs_file s_f[4];
static const char *const k_path[4] = { "/boot/modem_fs1", "/boot/modem_fs2", "/boot/modem_fsg", "/boot/modem_fsc" };
static const char *const k_part[4] = { "modemst1", "modemst2", "fsg", "fsc" };
static int s_parts_looked_up, s_write_ok = -1;   /* -1 undecided, 0 blocked, 1 allowed */
static uint8_t s_bounce[16u * RMTFS_SECTOR] __attribute__((aligned(32)));
static uint8_t s_tx[400];
static uint32_t s_stat_rd, s_stat_wr, s_stat_wr_blocked;

static void say(const char *s) { con_puts(s); }
static void hex(const char *s, uint32_t v) { con_puts(s); con_puthex(v); }
static void dec(const char *s, uint32_t v) { con_puts(s); con_putdec(v); }

/* copy to/from the uncached shared buffer: word accesses when aligned, bytes otherwise */
static void mem_to_shared(uint32_t off, const uint8_t *src, uint32_t n)
{
    volatile uint8_t *d8 = (volatile uint8_t *)(RMTFS_MEM_BASE + off);
    if (!(off & 3u) && !((uintptr_t)src & 3u) && !(n & 3u)) {
        volatile uint32_t *d = (volatile uint32_t *)d8; const uint32_t *s = (const uint32_t *)src;
        for (uint32_t i = 0; i < n / 4u; i++) d[i] = s[i];
    } else for (uint32_t i = 0; i < n; i++) d8[i] = src[i];
}
static void shared_to_mem(uint8_t *dst, uint32_t off, uint32_t n)
{
    const volatile uint8_t *s8 = (const volatile uint8_t *)(RMTFS_MEM_BASE + off);
    if (!(off & 3u) && !((uintptr_t)dst & 3u) && !(n & 3u)) {
        const volatile uint32_t *s = (const volatile uint32_t *)s8; uint32_t *d = (uint32_t *)dst;
        for (uint32_t i = 0; i < n / 4u; i++) d[i] = s[i];
    } else for (uint32_t i = 0; i < n; i++) dst[i] = s8[i];
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* IPC router v1 control message RESUME_TX: the modem set confirm_rx on a DATA packet to us and
 * stops sending after its quota until we acknowledge (ipc_router_core.c msm_ipc_router_send_resume_tx:
 * cli.node_id/port_id = the packet's DESTINATION, sent to the packet's source node). */
int mss_rtr_resume_tx(struct smd_chan *c, uint32_t src_node, uint32_t dst_node, uint32_t dst_port)
{
    uint32_t m[13] = { 1u, 7u, RMTFS_NODE_APPS, 0xfffffffeu, 0u, 20u, src_node, 0xfffffffeu,
                       7u, dst_node, dst_port, 0u, 0u };
    return smd_send(c, m, sizeof m);
}

static uint32_t s_src_port = RMTFS_PORT;               /* which of our servers is answering */
static int reply(struct smd_chan *c, uint32_t node, uint32_t port, uint32_t txn, uint32_t msg,
                 uint32_t result, uint32_t err, const uint8_t *opt, uint32_t optlen)
{
    uint32_t tlvlen = 7u + optlen;                     /* result TLV 3+4 */
    uint32_t hdr[8] = { 1u, 1u, RMTFS_NODE_APPS, s_src_port, 0u, 7u + tlvlen, node, port };
    if (32u + 7u + tlvlen > sizeof s_tx) return -1;
    memcpy(s_tx, hdr, 32);
    uint8_t *q = s_tx + 32;
    q[0] = 2u; q[1] = (uint8_t)txn; q[2] = (uint8_t)(txn >> 8); q[3] = (uint8_t)msg; q[4] = (uint8_t)(msg >> 8);
    q[5] = (uint8_t)tlvlen; q[6] = (uint8_t)(tlvlen >> 8);
    q[7] = 2u; q[8] = 4u; q[9] = 0u; q[10] = (uint8_t)result; q[11] = (uint8_t)(result >> 8); q[12] = (uint8_t)err; q[13] = (uint8_t)(err >> 8);
    if (optlen) memcpy(q + 14, opt, optlen);
    {   /* v390: show the exact reply bytes (QMI header + TLVs) next to the request's hex line */
        uint32_t n = 32u + 7u + tlvlen; int rc = smd_send(c, s_tx, n);
        dec("rmtfs: tx ", n); dec(" B rc ", (uint32_t)(rc < 0)); say(" :");
        for (uint32_t k = 0; k < n; k++) { static const char hx[] = "0123456789abcdef"; char c2[3] = { hx[s_tx[k] >> 4], hx[s_tx[k] & 15], 0 }; if (!(k % 4u)) say(" "); say(c2); }
        say("\n");
        return rc;
    }
}

static void lookup_parts(void)
{
    uint32_t lba[4], n[4], cnt = 0;
    if (s_parts_looked_up) return;
    s_parts_looked_up = 1;
    for (unsigned i = 0; i < 4u; i++) {
        if (emmc_gpt_find(k_part[i], &s_f[i].lba, &s_f[i].nblk) < 0) { s_f[i].lba = s_f[i].nblk = 0; say("rmtfs: partition "); say(k_part[i]); say(" NOT FOUND\n"); continue; }
        say("rmtfs: "); say(k_part[i]); hex(" lba ", s_f[i].lba); dec(" blocks ", s_f[i].nblk); say("\n");
        lba[cnt] = s_f[i].lba; n[cnt] = s_f[i].nblk; cnt++;
    }
#if defined(MSS_EFS_WRITES)
    s_write_ok = owf_is_flashed() == 1;
#else
    /* v450: EFS writes acknowledged but NOT performed on every board, flashed or RAM-booted. The
     * eMMC write path has never executed for real (every flashed C2 run ended "written 0"), and the
     * Gen 5E crashes when flashed to boot while the same image RAM-boots fine -- the first real
     * modem write is the only thing a flashed boot adds. Validate that path on its own
     * (-DMSS_EFS_WRITES) before letting the modem near modemst1/modemst2. */
    s_write_ok = 0; (void)owf_is_flashed();
    say("rmtfs: EFS writes DISABLED in this build (acknowledged, not performed)\n");
#endif
    if (s_write_ok && cnt) {
        if (emmc_write_window_modem(lba, n, cnt) < 0) { s_write_ok = 0; say("rmtfs: modem write window refused -> EFS writes BLOCKED\n"); }
        else say("rmtfs: OWF flashed -> EFS writes ALLOWED (modem window armed)\n");
    } else say("rmtfs: EFS writes BLOCKED (boot holds another OS or test image): writes acknowledged, not performed\n");
}

static int caller_valid(uint32_t id) { return id >= 1u && id <= 4u && s_f[id - 1u].open; }

/* direction 0 = modem reads (flash -> shared buffer), 1 = modem writes (shared buffer -> flash) */
static int do_iovec(uint32_t id, uint32_t dir, uint32_t sector, uint32_t off, uint32_t nsec)
{
    struct rmtfs_file *f = &s_f[id - 1u]; uint32_t done = 0;
    if (sector + nsec < sector || sector + nsec > f->nblk) return -2;
    if (off + nsec * RMTFS_SECTOR < off || off + nsec * RMTFS_SECTOR > RMTFS_MEM_SIZE) return -3;
    while (nsec) {
        uint32_t chunk = nsec > 16u ? 16u : nsec, bytes = chunk * RMTFS_SECTOR;
        if (dir == 0u) {
            if (emmc_read(f->lba + sector, chunk, s_bounce) < 0) return -4;
            mem_to_shared(off, s_bounce, bytes);
            s_stat_rd += chunk;
        } else if (s_write_ok == 1) {
            shared_to_mem(s_bounce, off, bytes);
            if (emmc_write(f->lba + sector, chunk, s_bounce) < 0) return -5;
            s_stat_wr += chunk;
        } else s_stat_wr_blocked += chunk;
        sector += chunk; off += bytes; nsec -= chunk;
        /* v392: a 1790-sector read killed v391 (no reply, no log): ~1800 back-to-back PIO block
         * transfers in this task starve the main loop that pets the hardware watchdog. Yield every
         * chunk and leave a breadcrumb every 256 sectors for the blackbox. */
        done += chunk;
        if (!(done & 255u)) { dec("rmtfs:   ... ", done); say(" sectors\n"); }
        vTaskDelay(1);
    }
    return 0;
}

/* pk = a complete IPC router v1 DATA packet addressed to node 1 port 0x4000 (header checked by caller) */
void mss_rmtfs_handle(struct smd_chan *c, const uint8_t *pk, uint32_t got)
{
    uint32_t w[8]; memcpy(w, pk, 32);
    uint32_t node = w[2], port = w[3];
    s_src_port = w[7];
    if (got < 39u) return;
    const uint8_t *q = pk + 32;
    uint32_t flags = q[0], txn = (uint32_t)q[1] | (uint32_t)q[2] << 8, msg = (uint32_t)q[3] | (uint32_t)q[4] << 8;
    uint32_t tlen = (uint32_t)q[5] | (uint32_t)q[6] << 8;
    if (flags & 2u) return;                                    /* a response, not ours to serve */
    if (39u + tlen > got) { dec("rmtfs: short packet, tlv len ", tlen); dec(" got ", got); say("\n"); tlen = got - 39u; }
    /* TLV index by type 0..7 */
    const uint8_t *tv[8] = { 0 }; uint32_t tl[8] = { 0 };
    /* tv[] is indexed by TLV type 0..7 — mark which are present */
    uint8_t have[8] = { 0 };
    for (uint32_t p = 0; p + 3u <= tlen; ) {
        uint32_t ty = q[7 + p], ln = (uint32_t)q[8 + p] | (uint32_t)q[9 + p] << 8;
        if (p + 3u + ln > tlen) break;
        if (ty < 8u) { have[ty] = 1u; tv[ty] = q + 10 + p; tl[ty] = ln; }
        p += 3u + ln;
    }

    if (w[7] == MEMSHARE_PORT) {
        /* v397: memshare (svc 0x34 inst 0x101; kernel drivers/soc/qcom/memshare/msm_memshare.c).
         * QUERY_SIZE 0x24: req TLV1 client_id, resp TLV2 result + 0x10 size u32.
         * ALLOC_GENERIC 0x22: req TLV1 num_bytes, TLV2 client_id, TLV3 proc_id, TLV4 sequence_id;
         * resp TLV2 result, 0x10 sequence_id u32, 0x11 u8 count + {phy_addr u64, num_bytes u32}.
         * Stock DT: client 0 = modem GPS, 2 MB allocated at boot -> one static 2 MB block here. */
        static uint8_t s_ms0[0x200000u] __attribute__((aligned(4096)));
        uint32_t addr = (uint32_t)(uintptr_t)s_ms0, size = sizeof s_ms0;
        if (msg == 0x24u) {
            uint32_t id = have[1] && tl[1] >= 4u ? rd32(tv[1]) : 0xffu;
            hex("memshare: QUERY_SIZE client ", id);
            uint8_t opt[7] = { 0x10u, 4u, 0u, 0u, 0u, 0u, 0u };
            uint32_t sz = id == 0u ? size : 0u; memcpy(opt + 3, &sz, 4);
            hex(" -> ", sz); say("\n");
            reply(c, node, port, txn, msg, 0u, 0u, opt, sizeof opt);
            return;
        }
        if (msg == 0x22u) {
            uint32_t nb = have[1] && tl[1] >= 4u ? rd32(tv[1]) : 0, id = have[2] && tl[2] >= 4u ? rd32(tv[2]) : 0xffu;
            uint32_t proc = have[3] && tl[3] >= 4u ? rd32(tv[3]) : 0, seq = have[4] && tl[4] >= 4u ? rd32(tv[4]) : 0;
            hex("memshare: ALLOC_GENERIC client ", id); hex(" proc ", proc); hex(" bytes ", nb); hex(" seq ", seq);
            if (id != 0u || nb > size) { say(" -> refused\n"); reply(c, node, port, txn, msg, 1u, 3u, 0, 0); return; }
            uint8_t opt[7 + 16] = { 0x10u, 4u, 0u, 0, 0, 0, 0,  0x11u, 13u, 0u, 1u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            memcpy(opt + 3, &seq, 4); memcpy(opt + 11, &addr, 4); memcpy(opt + 19, &size, 4);
            hex(" -> ", addr); hex(" size ", size); say("\n");
            reply(c, node, port, txn, msg, 0u, 0u, opt, sizeof opt);
            return;
        }
        hex("memshare: msg ", msg); say(" -> NOT_SUPPORTED\n");
        reply(c, node, port, txn, msg, 1u, 0x5eu, 0, 0);
        return;
    }
    if (w[7] == 0x400eu) {
        /* v399: svc 0x10f inst 2 = the sensor registry (SNS_REG2) the modem's sensor hub reads
         * from the APPS at init (msg 4 = group read, TLV1 u16 group id). The sensor QMI framework
         * uses a TWO-byte result (sns_common_resp: result u8, err u8) in TLV 2, so the generic
         * 4-byte NOT_SUPPORTED of v397 was undecodable and the hub kept waiting -> dog.c:1522
         * "stalled initialization" at +40 s. Answer a well-formed failure (result 1, err 10
         * SNS_ERR_FAILED): the hub then falls back to its built-in defaults. */
        /* v402: serve the registry for real from the C2's own sns.reg (sns_reg_c2.h).
         * GROUP_READ 4: req TLV1 group_id u16; resp TLV2 {result u8, err u8}, TLV3 group_id u16,
         * TLV4 data (u16 count + bytes; max 256 -> 2-byte count). Writes (3 single, 5 group) are
         * acknowledged, not stored. Anything else -> sns failure. */
        uint32_t grp = have[1] && tl[1] >= 2u ? (uint32_t)tv[1][0] | (uint32_t)tv[1][1] << 8 : 0xffffu;
        uint8_t *q = s_tx + 32; uint32_t tlvlen; int found = -1;
        if (msg == 4u) for (unsigned g = 0; g < SNS_REG_NGROUPS; g++) if (k_sns_groups[g].id == grp) found = (int)g;
        hex("sns-reg: msg ", msg); hex(" group ", grp);
        q[7] = 2u; q[8] = 2u; q[9] = 0u;
        if (found >= 0) {
            uint32_t n = k_sns_groups[found].size;
            q[10] = 0u; q[11] = 0u;                                   /* SNS success */
            q[12] = 3u; q[13] = 2u; q[14] = 0u; q[15] = (uint8_t)grp; q[16] = (uint8_t)(grp >> 8);
            q[17] = 4u; q[18] = (uint8_t)(n + 2u); q[19] = (uint8_t)((n + 2u) >> 8); q[20] = (uint8_t)n; q[21] = (uint8_t)(n >> 8);
            memcpy(q + 22, k_sns_data + k_sns_groups[found].off, n);
            tlvlen = 5u + 5u + 3u + 2u + n;
            dec(" -> ", n); say(" bytes\n");
        } else if (msg == 3u || msg == 5u) { q[10] = 0u; q[11] = 0u; tlvlen = 5u; say(" write -> acknowledged (not stored)\n"); }
        else { q[10] = 1u; q[11] = 10u; tlvlen = 5u; say(" -> sns failure\n"); }
        uint32_t hdr[8] = { 1u, 1u, RMTFS_NODE_APPS, w[7], 0u, 7u + tlvlen, node, port };
        memcpy(s_tx, hdr, 32);
        q[0] = 2u; q[1] = (uint8_t)txn; q[2] = (uint8_t)(txn >> 8); q[3] = (uint8_t)msg; q[4] = (uint8_t)(msg >> 8);
        q[5] = (uint8_t)tlvlen; q[6] = (uint8_t)(tlvlen >> 8);
        smd_send(c, s_tx, 32u + 7u + tlvlen);
        return;
    }
    if (w[7] != RMTFS_PORT && w[7] != RFSA_PORT) {
        /* v397: announced-but-unserved stock APPS services (0x10f/2 = sensor registry, 0x35/0x1001,
         * 0x118/0x3202, diag 0x1001 sockets): answer NOT_SUPPORTED so the client fails fast instead
         * of waiting; nothing of ours needs them. Logged so a stalled modem stage can be traced. */
        hex("qmi-stub: port ", w[7]); hex(" msg ", msg); dec(" tlv len ", tlen); say(" -> NOT_SUPPORTED\n");
        reply(c, node, port, txn, msg, 1u, 0x5eu, 0, 0);
        return;
    }
    if (w[7] == RFSA_PORT) {
        /* v391: RFSA (remote filesystem access, svc 0x1c inst 0x101; kernel drivers/uio/msm_sharedmem/
         * sharedmem_qmi.c). GET_BUFF_ADDR 0x23: req TLV1 client_id u32, TLV2 size u32; resp TLV2 result,
         * TLV 0x10 address u64. The modem asks for client 1 = the rmtfs buffer itself (stock DT
         * qcom,rmtfs_sharedmem client-id 1), i.e. THIS is how it gets its EFS transfer buffer; the
         * other stock ids are rfsa_dsp 0x11013ec @87ce0000 and rfsa_mdm 0x11013ed @87cf0000 (64 KB). */
        uint32_t id = have[1] && tl[1] >= 4u ? rd32(tv[1]) : 0, size = have[2] && tl[2] >= 4u ? rd32(tv[2]) : 0;
        uint32_t addr = 0, rsize = 0;
        if (id == 1u) { addr = RMTFS_MEM_BASE; rsize = RMTFS_MEM_SIZE; }
        else if (id == 0x11013ecu) { addr = 0x87ce0000u; rsize = 0x10000u; }
        else if (id == 0x11013edu) { addr = 0x87cf0000u; rsize = 0x10000u; }
        hex("rfsa: msg ", msg); hex(" client ", id); hex(" size ", size);
        if (msg != 0x23u || !addr || size > rsize) { say(" -> refused\n"); reply(c, node, port, txn, msg, 1u, msg != 0x23u ? 0x5eu : 3u, 0, 0); return; }
        uint8_t opt[11] = { 0x10u, 8u, 0u, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24), 0u, 0u, 0u, 0u };
        hex(" -> ", addr); say("\n");
        reply(c, node, port, txn, msg, 0u, 0u, opt, sizeof opt);
        return;
    }
    switch (msg) {
    case 1u: {                                                 /* OPEN */
        char path[64]; uint32_t n = have[1] ? tl[1] : 0; if (n > 63u) n = 63u;
        if (have[1]) memcpy(path, tv[1], n); path[n] = 0;
        lookup_parts();
        int idx = -1;
        for (unsigned i = 0; i < 4u; i++) if (!strncmp(path, k_path[i], 63u) && s_f[i].nblk) idx = (int)i;
        say("rmtfs: OPEN \""); say(path); say("\"");
        if (idx < 0) { say(" -> unknown path, refused\n"); reply(c, node, port, txn, msg, 1u, 3u, 0, 0); return; }
        s_f[idx].open = 1;
        uint8_t opt[7] = { 0x10u, 4u, 0u, (uint8_t)(idx + 1), 0u, 0u, 0u };
        dec(" -> caller ", (uint32_t)idx + 1u); say(" ("); say(k_part[idx]); say(")");
        dec(" rc ", (uint32_t)(reply(c, node, port, txn, msg, 0u, 0u, opt, sizeof opt) < 0)); say("\n");
        return; }
    case 2u: {                                                 /* CLOSE */
        uint32_t id = have[1] && tl[1] >= 4u ? rd32(tv[1]) : 0;
        dec("rmtfs: CLOSE caller ", id); say("\n");
        if (caller_valid(id)) s_f[id - 1u].open = 0;
        reply(c, node, port, txn, msg, caller_valid(id) || id ? 0u : 1u, 0u, 0, 0);
        return; }
    case 3u: {                                                 /* RW_IOVEC */
        uint32_t id = have[1] && tl[1] >= 4u ? rd32(tv[1]) : 0;
        uint32_t dir = have[2] && tl[2] >= 1u ? tv[2][0] : 0;
        uint32_t cnt = have[3] && tl[3] >= 1u ? tv[3][0] : 0, tot = 0; int rc = 0;
        if (have[3] && 1u + cnt * 12u > tl[3]) cnt = (tl[3] - 1u) / 12u;
        say(dir ? "rmtfs: WRITE caller " : "rmtfs: READ caller "); con_putdec(id); dec(" entries ", cnt);
        if (have[4] && tl[4] >= 1u && tv[4][0]) say(" force-sync");
        if (!caller_valid(id)) { say(" -> bad caller\n"); reply(c, node, port, txn, msg, 1u, 3u, 0, 0); return; }
        for (uint32_t i = 0; i < cnt && rc == 0; i++) {
            const uint8_t *e = tv[3] + 1u + i * 12u;
            uint32_t sec = rd32(e), off = rd32(e + 4), ns = rd32(e + 8);
            if (i == 0) { hex(" first sector ", sec); hex(" off ", off); dec(" n ", ns); }
            tot += ns;
            rc = do_iovec(id, dir, sec, off, ns);
        }
        dec(" sectors ", tot);
        if (rc) { dec(" FAILED rc -", (uint32_t)-rc); say("\n"); reply(c, node, port, txn, msg, 1u, 3u, 0, 0); return; }
        if (dir && s_write_ok != 1) say(" (BLOCKED, acknowledged)");
        say("\n");
        reply(c, node, port, txn, msg, 0u, 0u, 0, 0);
        return; }
    case 4u: {                                                 /* ALLOC_BUFF */
        uint32_t id = have[1] && tl[1] >= 4u ? rd32(tv[1]) : 0;
        uint32_t size = have[2] && tl[2] >= 4u ? rd32(tv[2]) : 0;
        dec("rmtfs: ALLOC_BUFF caller ", id); hex(" size ", size);
        if (!caller_valid(id) || size > RMTFS_MEM_SIZE) { say(" -> refused\n"); reply(c, node, port, txn, msg, 1u, 3u, 0, 0); return; }
        uint8_t opt[11] = { 0x10u, 8u, 0u, (uint8_t)RMTFS_MEM_BASE, (uint8_t)(RMTFS_MEM_BASE >> 8), (uint8_t)(RMTFS_MEM_BASE >> 16), (uint8_t)(RMTFS_MEM_BASE >> 24), 0u, 0u, 0u, 0u };
        hex(" -> ", RMTFS_MEM_BASE); say("\n");
        reply(c, node, port, txn, msg, 0u, 0u, opt, sizeof opt);
        return; }
    case 5u: {                                                 /* GET_DEV_ERROR */
        uint32_t id = have[1] && tl[1] >= 4u ? rd32(tv[1]) : 0;
        dec("rmtfs: GET_DEV_ERROR caller ", id); say(" -> 0\n");
        uint8_t opt[4] = { 0x10u, 1u, 0u, 0u };
        reply(c, node, port, txn, msg, 0u, 0u, opt, sizeof opt);
        return; }
    default:
        hex("rmtfs: unknown msg ", msg); dec(" tlv len ", tlen); say(" -> NOT_SUPPORTED\n");
        reply(c, node, port, txn, msg, 1u, 0x5eu, 0, 0);
        return;
    }
}

void mss_rmtfs_stats(void)
{
    dec("rmtfs: sectors read ", s_stat_rd); dec(" written ", s_stat_wr); dec(" write-blocked ", s_stat_wr_blocked);
    say(" open"); for (unsigned i = 0; i < 4u; i++) if (s_f[i].open) { say(" "); say(k_part[i]); } say("\n");
}
