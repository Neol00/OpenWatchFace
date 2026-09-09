/* bt_fw.c — read the Gen 6 Bluetooth firmware out of the `bluetooth` partition.
 *
 * That partition is a small FAT16 volume (4096-byte sectors) whose root holds
 * one directory:
 *     IMAGE/APBTFW11.TLV   220092 B   the RAM patch
 *     IMAGE/APNV11.BIN       4913 B   the NVM / calibration blob
 * Both go to the chip over HCI; see bt_wcn3990.c for the transfer itself.
 *
 * Streaming, not buffered: the patch is 220 KB and there is no reason to hold
 * it in RAM when it is consumed 243 bytes at a time.
 */
#include "platform.h"
#if defined(PLAT_BT_UART_BASE)
#include <string.h>

#define SECT_BYTES 512u                 /* eMMC block size, not the FAT's */

static uint32_t s_part_lba;             /* partition start, in eMMC blocks */
static uint32_t s_bps, s_spc;           /* FAT bytes/sector, sectors/cluster */
static uint32_t s_fat_sec, s_root_sec, s_data_sec, s_nroot;
static int      s_ok;

static uint8_t s_blk[SECT_BYTES];

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* Read `n` bytes at a byte offset inside the partition. */
static int part_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *out = (uint8_t *)dst;
    while (n) {
        uint32_t lba = s_part_lba + off / SECT_BYTES;
        uint32_t skip = off % SECT_BYTES;
        uint32_t take = SECT_BYTES - skip; if (take > n) take = n;
        if (emmc_read_block(lba, s_blk) < 0) return -1;
        memcpy(out, s_blk + skip, take);
        out += take; off += take; n -= take;
    }
    return 0;
}

int bt_fat_init(void)
{
    uint32_t nblk;
    if (s_ok) return 0;
    if (emmc_gpt_find("bluetooth", &s_part_lba, &nblk) < 0) {
        con_puts("bt-fw: no `bluetooth` partition in GPT\n");
        return -1;
    }
    if (part_read(0, s_blk, SECT_BYTES) < 0) return -1;
    s_bps  = rd16(s_blk + 11);
    s_spc  = s_blk[13];
    uint32_t rsv = rd16(s_blk + 14), nfat = s_blk[16], spf = rd16(s_blk + 22);
    s_nroot = rd16(s_blk + 17);
    if (!s_bps || !s_spc || !spf) { con_puts("bt-fw: bad FAT boot sector\n"); return -1; }
    s_fat_sec  = rsv;
    s_root_sec = s_fat_sec + nfat * spf;
    s_data_sec = s_root_sec + (s_nroot * 32u + s_bps - 1u) / s_bps;
    s_ok = 1;
    con_puts("bt-fw: bluetooth FAT16 @lba "); con_puthex(s_part_lba);
    con_puts(" bps "); con_putdec(s_bps);
    con_puts(" spc "); con_putdec(s_spc); con_puts("\n");
    return 0;
}

static uint32_t clus_off(uint32_t c) { return (s_data_sec + (c - 2u) * s_spc) * s_bps; }

static uint32_t fat_next(uint32_t c)
{
    uint8_t e[2];
    if (part_read(s_fat_sec * s_bps + c * 2u, e, 2) < 0) return 0xFFFFu;
    return rd16(e);
}

/* Find an 8.3 entry (name11 = "APBTFW11TLV", padded) in a directory. dir_clus
 * 0 means the fixed root directory. Returns 0 and fills cluster/size. */
int bt_fat_find(uint32_t dir_clus, const char *name11,
                uint32_t *clus_out, uint32_t *size_out)
{
    uint8_t ent[32];
    if (!s_ok) return -1;
    if (dir_clus == 0u) {
        for (uint32_t i = 0; i < s_nroot; i++) {
            if (part_read(s_root_sec * s_bps + i * 32u, ent, 32) < 0) return -1;
            if (ent[0] == 0) break;
            if (ent[0] == 0xE5u || ent[11] == 0x0Fu) continue;
            if (memcmp(ent, name11, 11) == 0) {
                *clus_out = rd16(ent + 26);
                *size_out = (uint32_t)ent[28] | ((uint32_t)ent[29] << 8)
                          | ((uint32_t)ent[30] << 16) | ((uint32_t)ent[31] << 24);
                return 0;
            }
        }
        return -1;
    }
    for (uint32_t c = dir_clus; c >= 2u && c < 0xFFF8u; c = fat_next(c)) {
        uint32_t per = s_spc * s_bps / 32u;
        for (uint32_t i = 0; i < per; i++) {
            if (part_read(clus_off(c) + i * 32u, ent, 32) < 0) return -1;
            if (ent[0] == 0) return -1;
            if (ent[0] == 0xE5u || ent[11] == 0x0Fu) continue;
            if (memcmp(ent, name11, 11) == 0) {
                *clus_out = rd16(ent + 26);
                *size_out = (uint32_t)ent[28] | ((uint32_t)ent[29] << 8)
                          | ((uint32_t)ent[30] << 16) | ((uint32_t)ent[31] << 24);
                return 0;
            }
        }
    }
    return -1;
}

/* ---- resumable reader ----------------------------------------------------
 * The blocking whole-file stream below is fine for a report, but the download
 * must be able to stop and resume between main-loop iterations (see the note
 * on bt_dl_step): a 20 s blocking transfer starves the UI and, worse, starves
 * usb_poll() so the console host drops the device. */
int bt_file_open(uint32_t dir_clus, const char *name11, struct bt_file *f)
{
    uint32_t c = 0, sz = 0;
    if (bt_fat_find(dir_clus, name11, &c, &sz) < 0) return -1;
    f->clus = c; f->off = 0; f->left = sz; f->total = sz;
    return 0;
}

int bt_file_read(struct bt_file *f, uint8_t *buf, uint32_t max)
{
    uint32_t csz = s_spc * s_bps;
    if (!f->left) return 0;
    if (f->off >= csz) {
        f->clus = fat_next(f->clus);
        f->off = 0;
        if (f->clus < 2u || f->clus >= 0xFFF8u) return -1;
    }
    uint32_t n = max;
    if (n > csz - f->off) n = csz - f->off;
    if (n > f->left) n = f->left;
    if (part_read(clus_off(f->clus) + f->off, buf, n) < 0) return -1;
    f->off += n; f->left -= n;
    return (int)n;
}

/* Stream a file through `sink` in <= chunk-sized pieces. Returns 0 on success. */
int bt_fat_stream(uint32_t clus, uint32_t size,
                  int (*sink)(const uint8_t *b, uint32_t n, void *arg), void *arg)
{
    static uint8_t buf[512];
    uint32_t left = size;
    if (!s_ok) return -1;
    for (uint32_t c = clus; left && c >= 2u && c < 0xFFF8u; c = fat_next(c)) {
        uint32_t csz = s_spc * s_bps, off = 0;
        while (off < csz && left) {
            uint32_t n = sizeof buf; if (n > csz - off) n = csz - off; if (n > left) n = left;
            if (part_read(clus_off(c) + off, buf, n) < 0) return -1;
            if (sink(buf, n, arg) < 0) return -1;
            off += n; left -= n;
        }
    }
    return left ? -1 : 0;
}

/* ---- TLV download ---------------------------------------------------------
 * Each segment is an HCI vendor command (btqca.c qca_tlv_send_segment):
 *     01 00 FC <n+2> 1E <n> <n bytes>          opcode 0xFC00, param 0x1E
 * The WHOLE file goes over, its 4-byte TLV header included, in 243-byte pieces
 * (MAX_SIZE_PER_TLV_SEGMENT). The vendor fires the full segments without
 * waiting per-segment and only synchronises on the last one; we do the same but
 * still DRAIN the receive path each time, because this port's queue is finite
 * and an unread reply would eventually overflow it.
 *
 * 220 KB at 115200 is ~20 s of solid transfer and the watchdog bites at 31, so
 * the loop pets it. */
#define BT_TLV_SEG 243u

struct bt_dl {
    uint8_t  seg[BT_TLV_SEG];
    uint32_t fill, done, total, nseg;
};

/* ---- non-blocking segment transmitter ------------------------------------
 * A segment is queued here and pushed out a few bytes at a time by tx_pump(),
 * which never waits on the transmitter. If the controller throttles us with
 * CTS the pump simply makes no progress this iteration and the caller's loop
 * keeps running - which is what keeps the UI drawing and USB alive. */
static uint8_t  s_tx[6u + BT_TLV_SEG];
static uint32_t s_txlen, s_txpos;

static void tx_queue(const uint8_t *d, uint32_t n)
{
    s_tx[0] = 0x01u; s_tx[1] = 0x00u; s_tx[2] = 0xFCu;
    s_tx[3] = (uint8_t)(n + 2u); s_tx[4] = 0x1Eu; s_tx[5] = (uint8_t)n;
    memcpy(s_tx + 6, d, n);
    s_txlen = n + 6u; s_txpos = 0;
}

/* 1 = the queued segment is fully out, 0 = more to do. Bounded work per call. */
static int tx_pump(void)
{
    uint32_t budget = 64u;                 /* ~5 ms of UART time at 115200 */
    while (s_txpos < s_txlen && budget--) {
        if (!bt_uart_try_putc(s_tx[s_txpos])) return 0;
        s_txpos++;
    }
    return s_txpos >= s_txlen;
}

static int bt_send_seg(const uint8_t *d, uint32_t n, int sync)
{
    uint8_t h[6];
    h[0] = 0x01u;                 /* H4: command  */
    h[1] = 0x00u; h[2] = 0xFCu;   /* opcode 0xFC00 */
    h[3] = (uint8_t)(n + 2u);     /* plen          */
    h[4] = 0x1Eu;                 /* EDL_PATCH_TLV_REQ_CMD */
    h[5] = (uint8_t)n;
    bt_uart_write(h, sizeof h);
    bt_uart_write(d, n);
    /* Keep the console host alive: the USB device controller is serviced from
     * the same loop this transfer runs in, and starving it for the length of a
     * download is what made the host drop the device (and left the bus in a
     * state that even fastboot could not recover). */
    usb_poll(); wdog_pet(); deadman_kick();
    if (sync) {
        uint8_t rsp[64];
        int r = bt_uart_read(rsp, sizeof rsp, 2000u);
        con_puts("bt-fw: final segment reply "); con_putdec((uint32_t)r); con_puts(" bytes:");
        for (int i = 0; i < r && i < 16; i++) { con_puts(" "); bt_hex2(rsp[i]); }
        con_puts("\n");
        return r > 0 ? 0 : -1;
    }
    while (bt_uart_getc() >= 0) { }    /* discard any per-segment chatter */
    return 0;
}

static int bt_dl_sink(const uint8_t *b, uint32_t n, void *arg)
{
    struct bt_dl *d = (struct bt_dl *)arg;
    while (n) {
        uint32_t take = BT_TLV_SEG - d->fill; if (take > n) take = n;
        memcpy(d->seg + d->fill, b, take);
        d->fill += take; b += take; n -= take; d->done += take;
        if (d->fill == BT_TLV_SEG) {
            int last = (d->done == d->total);
            if (bt_send_seg(d->seg, BT_TLV_SEG, last) < 0) return -1;
            d->fill = 0;
            if ((++d->nseg % 32u) == 0u) { wdog_pet(); deadman_kick(); }
            if ((d->nseg % 200u) == 0u) {
                con_puts("bt-fw:  ... "); con_putdec(d->done);
                con_puts("/"); con_putdec(d->total); con_puts("\n"); con_flush();
            }
        }
    }
    return 0;
}

/* ---- the NVM needs editing before it is sent ------------------------------
 * qca_tlv_check_data() in the kernel rewrites two fields of the calibration
 * blob's tag 17 (EDL_TAG_ID_HCI) before download, and sending the file
 * verbatim is why the controller went silent after v8/v9:
 *     data[0] bit 0x80 = in-band sleep. The file ships it SET (0x82), so the
 *                        chip starts the IBS handshake and spits 0xFD WAKE
 *                        indications at us - exactly the "noise" v9 saw.
 *     data[2]          = UART baud code. The file ships 0x11 = 17 =
 *                        3 200 000, so the chip changes speed out from under
 *                        a host still listening at 115200.
 * Clear both: no in-band sleep, stay at 115200. Higher speed can come later,
 * deliberately, once the link is proven.
 *
 * The file is under 5 KB, so it is buffered and patched whole rather than
 * fixed up mid-stream (a tag can straddle a 243-byte segment boundary).
 * NB the structural TLV walk does NOT parse this file - the first entry reads
 * as tag 2306 and the walk overruns - so tag 17 is located by signature
 * (id 17, len 9), verified at offset 43 of this exact APNV11.BIN. */
#define BT_NVM_MAX 8192u
/* Baud code written into NVM tag 17 data[2]: must match the speed the host
 * will use after the controller restarts. 0 = 115200, 0x11 = 3 200 000. */
static uint8_t s_nvm_baud = 0x00u;
void bt_fw_set_nvm_baud(uint8_t code) { s_nvm_baud = code; }
static uint8_t s_nvm[BT_NVM_MAX];
static uint32_t s_nvm_len;

static int nvm_sink(const uint8_t *b, uint32_t n, void *arg)
{
    (void)arg;
    if (s_nvm_len + n > BT_NVM_MAX) return -1;
    memcpy(s_nvm + s_nvm_len, b, n); s_nvm_len += n;
    return 0;
}

static void nvm_patch(void)
{
    for (uint32_t i = 0; i + 12u + 9u <= s_nvm_len; i++) {
        uint32_t tag = (uint32_t)s_nvm[i] | ((uint32_t)s_nvm[i + 1] << 8);
        uint32_t len = (uint32_t)s_nvm[i + 2] | ((uint32_t)s_nvm[i + 3] << 8);
        if (tag != 17u || len != 9u) continue;
        uint8_t *d = s_nvm + i + 12u;
        con_puts("bt-fw: nvm tag17 @"); con_putdec(i);
        con_puts(" was sleep="); bt_hex2(d[0]); con_puts(" baud="); bt_hex2(d[2]);
        d[0] = (uint8_t)(d[0] & ~0x80u);       /* in-band sleep OFF */
        d[2] = s_nvm_baud;                     /* match the link we will run at */
        con_puts(" -> sleep="); bt_hex2(d[0]); con_puts(" baud="); bt_hex2(d[2]);
        con_puts("\n");
        return;
    }
    con_puts("bt-fw: nvm tag17 NOT FOUND - sending unpatched (expect a baud change)\n");
}

static int bt_fw_send_nvm(uint32_t img_c)
{
    struct bt_dl d;
    uint32_t c = 0, sz = 0;
    if (bt_fat_find(img_c, "APNV11  BIN", &c, &sz) < 0) {
        con_puts("bt-fw: missing APNV11.BIN\n"); return -1;
    }
    s_nvm_len = 0;
    if (bt_fat_stream(c, sz, nvm_sink, 0) < 0 || s_nvm_len != sz) {
        con_puts("bt-fw: nvm read failed\n"); return -1;
    }
    nvm_patch();
    memset(&d, 0, sizeof d);
    d.total = sz;
    con_puts("bt-fw: sending APNV11 (patched, "); con_putdec(sz); con_puts(" bytes)\n");
    con_flush();
    if (bt_dl_sink(s_nvm, sz, &d) < 0) return -1;
    if (d.fill && bt_send_seg(d.seg, d.fill, 1) < 0) {
        con_puts("bt-fw: last nvm segment not acknowledged\n"); return -1;
    }
    wdog_pet(); deadman_kick();
    con_puts("bt-fw: APNV11 sent\n");
    return 0;
}

/* Send one firmware file. name11 is the 8.3 name inside IMAGE/. */
static int bt_fw_send(uint32_t img_c, const char *name11)
{
    struct bt_dl d;
    uint32_t c = 0, sz = 0;
    if (bt_fat_find(img_c, name11, &c, &sz) < 0) {
        con_puts("bt-fw: missing "); con_puts(name11); con_puts("\n"); return -1;
    }
    memset(&d, 0, sizeof d);
    d.total = sz;
    con_puts("bt-fw: sending "); con_puts(name11);
    con_puts(" ("); con_putdec(sz); con_puts(" bytes, ");
    con_putdec((sz + BT_TLV_SEG - 1u) / BT_TLV_SEG); con_puts(" segments)\n");
    con_flush();
    wdog_pet(); deadman_kick();
    if (bt_fat_stream(c, sz, bt_dl_sink, &d) < 0) {
        con_puts("bt-fw: read/stream failed\n"); return -1;
    }
    if (d.fill) {                                   /* trailing partial piece */
        if (bt_send_seg(d.seg, d.fill, 1) < 0) {
            con_puts("bt-fw: last segment not acknowledged\n"); return -1;
        }
        d.nseg++;
    }
    wdog_pet(); deadman_kick();
    con_puts("bt-fw: "); con_puts(name11); con_puts(" sent, ");
    con_putdec(d.nseg); con_puts(" segments\n");
    return 0;
}

/* ---- stepped download ----------------------------------------------------
 * One segment per call. A segment is ~248 bytes at 115200 = ~21 ms, which is
 * short enough that the caller can service USB and the watchdog between them
 * instead of disappearing for twenty seconds. Returns 0 busy, 1 done, <0 fail. */
static struct { int st, tx_last; struct bt_file f; struct bt_dl d; uint32_t img_c; uint8_t buf[BT_TLV_SEG]; } g;

int bt_dl_start(void)
{
    uint32_t img_sz = 0;
    memset(&g, 0, sizeof g);
    if (bt_fat_init() < 0) return -1;
    if (bt_fat_find(0u, "IMAGE      ", &g.img_c, &img_sz) < 0) {
        con_puts("bt-fw: no IMAGE directory\n"); return -1;
    }
    if (bt_file_open(g.img_c, "APBTFW11TLV", &g.f) < 0) {
        con_puts("bt-fw: missing APBTFW11.TLV\n"); return -1;
    }
    g.d.total = g.f.total;
    con_puts("bt-fw: sending APBTFW11TLV ("); con_putdec(g.f.total);
    con_puts(" bytes, "); con_putdec((g.f.total + BT_TLV_SEG - 1u) / BT_TLV_SEG);
    con_puts(" segments) - stepped, UI stays alive\n");
    g.st = 1;
    return 0;
}

int bt_dl_step(void)
{
    if (g.st == 1 || g.st == 3) {              /* streaming a file */
        if (s_txpos < s_txlen) {               /* a segment is still going out */
            if (!tx_pump()) return 0;
            if (g.tx_last) {                   /* that was the final segment */
                uint8_t rsp[64];
                int r = bt_uart_read(rsp, sizeof rsp, 2000u);
                con_puts("bt-fw: final segment reply "); con_putdec((uint32_t)r);
                con_puts(" bytes:");
                for (int i = 0; i < r && i < 16; i++) { con_puts(" "); bt_hex2(rsp[i]); }
                con_puts("\n");
                if (r <= 0) return -1;
            } else {
                while (bt_uart_getc() >= 0) { }     /* drop per-segment chatter */
            }
            usb_poll(); wdog_pet(); deadman_kick();
            if ((++g.d.nseg % 200u) == 0u) {
                con_puts("bt-fw:  ... "); con_putdec(g.d.done);
                con_puts("/"); con_putdec(g.d.total); con_puts("\n"); con_flush();
            }
            if (g.tx_last) {
                con_puts("bt-fw: file sent, "); con_putdec(g.d.nseg); con_puts(" segments\n");
                g.tx_last = 0;
                if (g.st == 1) { g.st = 2; return 0; }
                g.st = 4; return 1;
            }
            return 0;
        }
        uint32_t need = BT_TLV_SEG - g.d.fill;
        int n = bt_file_read(&g.f, g.buf, need);
        if (n < 0) { con_puts("bt-fw: read error\n"); return -1; }
        if (n > 0) {
            memcpy(g.d.seg + g.d.fill, g.buf, (uint32_t)n);
            g.d.fill += (uint32_t)n; g.d.done += (uint32_t)n;
        }
        int eof = (g.f.left == 0);
        if (g.d.fill == BT_TLV_SEG || (eof && g.d.fill)) {
            tx_queue(g.d.seg, g.d.fill);       /* queued; pumped on later calls */
            g.tx_last = eof ? 1 : 0;
            g.d.fill = 0;
        }
        return 0;
    }

    if (g.st == 2) {                           /* NVM: buffer, patch, then send */
        uint32_t c = 0, sz = 0;
        if (bt_fat_find(g.img_c, "APNV11  BIN", &c, &sz) < 0) {
            con_puts("bt-fw: missing APNV11.BIN\n"); return -1;
        }
        s_nvm_len = 0;
        if (bt_fat_stream(c, sz, nvm_sink, 0) < 0 || s_nvm_len != sz) {
            con_puts("bt-fw: nvm read failed\n"); return -1;
        }
        nvm_patch();
        memset(&g.d, 0, sizeof g.d);
        g.d.total = sz;
        /* feed the patched buffer through the same stepped path */
        g.f.clus = 0; g.f.off = 0; g.f.left = sz; g.f.total = sz;
        g.st = 5;
        con_puts("bt-fw: sending APNV11 (patched, "); con_putdec(sz); con_puts(" bytes)\n");
        return 0;
    }

    if (g.st == 5) {                           /* send patched NVM from RAM */
        if (s_txpos < s_txlen) {
            if (!tx_pump()) return 0;
            if (g.tx_last) {
                uint8_t rsp[64];
                int r = bt_uart_read(rsp, sizeof rsp, 2000u);
                con_puts("bt-fw: nvm final reply "); con_putdec((uint32_t)r); con_puts(" bytes:");
                for (int i = 0; i < r && i < 16; i++) { con_puts(" "); bt_hex2(rsp[i]); }
                con_puts("\n");
                if (r <= 0) return -1;
                con_puts("bt-fw: APNV11 sent, "); con_putdec(g.d.nseg + 1u); con_puts(" segments\n");
                g.st = 4; return 1;
            }
            while (bt_uart_getc() >= 0) { }
            usb_poll(); wdog_pet(); deadman_kick();
            g.d.nseg++;
            return 0;
        }
        uint32_t off = g.d.done;
        uint32_t n = BT_TLV_SEG; if (n > g.d.total - off) n = g.d.total - off;
        g.tx_last = (off + n == g.d.total) ? 1 : 0;
        tx_queue(s_nvm + off, n);
        g.d.done += n;
        return 0;
    }
    return 1;
}

/* Patch then NVM, the vendor's order. */
int bt_fw_download(void)
{
    uint32_t img_c = 0, img_sz = 0;
    if (bt_fat_init() < 0) return -1;
    if (bt_fat_find(0u, "IMAGE      ", &img_c, &img_sz) < 0) {
        con_puts("bt-fw: no IMAGE directory\n"); return -1;
    }
    if (bt_fw_send(img_c, "APBTFW11TLV") < 0) return -1;
    if (bt_fw_send_nvm(img_c) < 0) return -1;
    return 0;
}

/* Locate both firmware files and report them (bring-up check). */
int bt_fw_report(void)
{
    uint32_t img_c = 0, img_sz = 0, c = 0, sz = 0;
    uint8_t head[16];
    if (bt_fat_init() < 0) return -1;
    if (bt_fat_find(0u, "IMAGE      ", &img_c, &img_sz) < 0) {
        con_puts("bt-fw: no IMAGE directory\n"); return -1;
    }
    int rc = 0;
    static const char *names[2] = { "APBTFW11TLV", "APNV11  BIN" };
    for (int i = 0; i < 2; i++) {
        if (bt_fat_find(img_c, names[i], &c, &sz) < 0) {
            con_puts("bt-fw: missing "); con_puts(names[i]); con_puts("\n"); rc = -1; continue;
        }
        con_puts("bt-fw: "); con_puts(names[i]);
        con_puts(" size "); con_putdec(sz); con_puts(" head");
        if (part_read(clus_off(c), head, sizeof head) == 0)
            for (unsigned k = 0; k < sizeof head; k++) { con_puts(" "); bt_hex2(head[k]); }
        con_puts("\n");
    }
    return rc;
}

#endif /* PLAT_BT_UART_BASE */
