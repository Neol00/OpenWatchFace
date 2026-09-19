/* mss_diag.c -- read the modem's own debug messages (diag F3) during its boot (2026-09-15).
 *
 * v457 proved the modem's diag stack talks on DIAG_CNTL (feature mask reply, command
 * registrations, SSID/log/build range reports). This file turns that into the one
 * diagnostic the Wear 3100 stall needs: after the modem's SSID range report (ctrl pkt 24,
 * 24 ranges on the darter) it sends an F3 mask per range with every bit set (ctrl pkt 11,
 * DIAG_CTRL_MSG_F3_MASK_V2, status VALID), and decodes what then arrives on the DIAG data
 * channel: extended messages (cmd 0x79: ts_type, num_args, drop, ts64, line16, ssid16,
 * ss_mask32, args32[n], fmt\0, file\0) become "F3 file:line fmt(args)" lines; terse/qshrink
 * forms (0x92 / 0x7d) are printed as hashes. The modem's format strings are plain text
 * in its image (checked: modem.b12 carries rcinit/dog/task strings), so 0x79 is expected. */
#include "platform.h"
#include <string.h>
#include <stdio.h>

#if defined(MSS_BOOT) && (MSS_OPEN_MASK & 0x10u)

static void say(const char *s) { con_puts(s); }
static void dec(const char *s, uint32_t v) { con_puts(s); con_putdec(v); }
static void hex(const char *s, uint32_t v) { con_puts(s); con_puthex(v); }
static int s_masks_sent;
static uint32_t s_f3, s_other;

/* Control-channel packet from the modem. Returns 1 if it was consumed here. */
int mss_diag_cntl_rx(struct smd_chan *cntl, const uint8_t *p, uint32_t n)
{
    if (n < 8u) return 0;
    uint32_t id, len; memcpy(&id, p, 4u); memcpy(&len, p + 4u, 4u);
#if defined(MSS_DIAG_NO_F3)
    /* chime66 (user): the F3 flood starts at the sound bring-up and the sound never comes back.
     * Do not ask the modem for ANY F3 levels: its default masks stay as they are. */
    if (id == 24u && !s_masks_sent) { s_masks_sent = 1; say("mss-diag: SSID range report -- F3 masks NOT sent (MSS_DIAG_NO_F3)\n"); return 1; }
#else
    if (id == 24u && n >= 16u && !s_masks_sent) {               /* SSID range report */
        uint32_t cnt; memcpy(&cnt, p + 12u, 4u);
        if (cnt > 64u) cnt = 64u;
        dec("mss-diag: SSID range report: ", cnt); say(" ranges -> enabling every F3 level\n");
        uint32_t sent = 0, bad = 0;
        for (uint32_t i = 0; i < cnt && 16u + i * 4u + 4u <= n; i++) {
            uint16_t first, last; memcpy(&first, p + 16u + i * 4u, 2u); memcpy(&last, p + 18u + i * 4u, 2u);
            uint32_t k = (uint32_t)last - first + 1u;
            if (last < first || k > 200u) continue;      /* MAX_SSID_PER_RANGE 200; v461 skipped 0..0x79 with 100 = the rcinit/dog range */
#if defined(MSS_DIAG_AUDIO_ONLY)
            if (!(first <= 8500u && 8500u <= last)) continue;   /* chime69: only the range holding ssid 8500 (AVS / audio) */
#endif
            static uint8_t pk[8u + 11u + 200u * 4u];
            uint32_t data_len = 11u + k * 4u;
            uint32_t cmd = 11u; memcpy(pk, &cmd, 4u); memcpy(pk + 4u, &data_len, 4u);
            pk[8] = 1u;  /* stream_id */ pk[9] = 3u; /* DIAG_CTRL_MASK_VALID */ pk[10] = 0u; /* msg_mode */
            memcpy(pk + 11u, &first, 2u); memcpy(pk + 13u, &last, 2u); memcpy(pk + 15u, &k, 4u);
            memset(pk + 19u, 0xff, k * 4u);
            if (smd_send(cntl, pk, 19u + k * 4u) < 0) bad++; else sent++;
        }
        dec("mss-diag: F3 masks sent ", sent); dec(" failed ", bad); say("\n");
        s_masks_sent = 1;
        return 1;
    }
#endif
    if (id == 8u || id == 12u || id == 1u || id == 23u || id == 25u || id == 22u || id == 28u) return 0;  /* known, let the dump show it once */
    return 0;
}

static void put_str(const uint8_t *s, uint32_t max)
{
    char b[2] = { 0, 0 };
    for (uint32_t i = 0; i < max && s[i]; i++) { b[0] = (s[i] >= 32 && s[i] < 127) ? (char)s[i] : '?'; say(b); }
}

/* Data-channel packet: one or more diag packets. Decodes F3 extended messages. */
static void diag_one(const uint8_t *p, uint32_t n);
/* v462: what actually arrives (Gen 5 v461): frames "7e 01 <len u16> <payload> 7e", several per SMD
 * packet. Peel them and decode each payload as one diag packet. Anything else falls through. */
static uint8_t s_acc[4096]; static uint32_t s_acc_n;   /* v463: a frame can span SMD packets */
void mss_diag_data_rx(const uint8_t *p, uint32_t n)
{
    if (s_acc_n + n > sizeof s_acc) s_acc_n = 0;
    memcpy(s_acc + s_acc_n, p, n); s_acc_n += n;
    uint32_t off = 0;
    while (off + 4u <= s_acc_n && s_acc[off] == 0x7eu && s_acc[off + 1u] == 0x01u) {
        uint16_t len; memcpy(&len, s_acc + off + 2u, 2u);
        if (off + 4u + len + 1u > s_acc_n) break;                 /* incomplete: wait for the next packet */
        diag_one(s_acc + off + 4u, len);
        off += 4u + len + 1u;
    }
    if (off < s_acc_n && !(s_acc[off] == 0x7eu && s_acc[off + 1u] == 0x01u)) { diag_one(s_acc + off, s_acc_n - off); off = s_acc_n; }
    if (off) { memmove(s_acc, s_acc + off, s_acc_n - off); s_acc_n -= off; }
}
static void diag_one(const uint8_t *p, uint32_t n)
{
    uint32_t off = 0;
    while (off < n) {
        uint8_t cmd = p[off];
        if (cmd == 0x79u && off + 20u <= n) {
            uint8_t nargs = p[off + 2u];
            uint16_t line, ssid; memcpy(&line, p + off + 12u, 2u); memcpy(&ssid, p + off + 14u, 2u);
            uint32_t ts_lo; memcpy(&ts_lo, p + off + 4u, 4u);
            uint32_t a = off + 20u + (uint32_t)nargs * 4u;
            if (a > n) { dec("F3: truncated nargs ", nargs); say("\n"); return; }
            const uint8_t *fmt = p + a; uint32_t fl = 0; while (a + fl < n && fmt[fl]) fl++;
            const uint8_t *file = fmt + fl + 1u; uint32_t fil = 0; while ((uint32_t)(file - p) + fil < n && file[fil]) fil++;
            s_f3++;
            { uint32_t lv; memcpy(&lv, p + off + 16u, 4u); say("F3 "); put_str(file, fil); dec(":", line); dec(" ssid ", ssid); hex(" lvl 0x", lv); say(" \""); }
            put_str(fmt, fl); say("\"");
            for (uint8_t i = 0; i < nargs && i < 8u; i++) { uint32_t v; memcpy(&v, p + off + 20u + i * 4u, 4u); hex(i ? "," : " args ", v); }
            say("\n");
            off = (uint32_t)(file - p) + fil + 1u;
            continue;
        }
        if ((cmd == 0x92u || cmd == 0x7du) && off + 24u <= n) {   /* QSR terse: hash instead of text */
            uint8_t nargs = p[off + 2u]; uint32_t h; memcpy(&h, p + off + 20u, 4u);
            uint16_t line, ssid; memcpy(&line, p + off + 12u, 2u); memcpy(&ssid, p + off + 14u, 2u);
            uint32_t mask; memcpy(&mask, p + off + 16u, 4u);
            s_f3++; hex("F3q ssid ", ssid); dec(" line ", line); hex(" lvl 0x", mask); hex(" hash 0x", h);
            for (uint8_t i = 0; i < nargs && i < 8u && off + 28u + i * 4u <= n; i++) { uint32_t v; memcpy(&v, p + off + 24u + i * 4u, 4u); hex(i ? "," : " args ", v); }
            say("\n");
            off += 24u + (uint32_t)nargs * 4u;
            continue;
        }
        if (cmd == 0x60u) { s_other++; dec("diag: event report len ", n - off); say("\n"); return; }
        if (cmd == 0x7eu && off + 20u <= n) {                     /* DIAG_EXT_MSG_TERSE: hash, no text */
            uint8_t nargs = p[off + 2u];
            uint16_t line, ssid; memcpy(&line, p + off + 12u, 2u); memcpy(&ssid, p + off + 14u, 2u);
            uint32_t h = 0; if (off + 24u <= n) memcpy(&h, p + off + 20u, 4u);
            s_f3++; hex("F3(terse 7e) hash 0x", h); dec(" line ", line); dec(" ssid ", ssid); dec(" nargs ", nargs);
            for (uint8_t i = 0; i < nargs && i < 8u && off + 28u + i * 4u <= n; i++) { uint32_t v; memcpy(&v, p + off + 24u + i * 4u, 4u); hex(i ? "," : " args ", v); }
            say("\n");
        }
        s_other++;
        hex("diag: pkt cmd 0x", cmd); dec(" len ", n - off); say(":");
        for (uint32_t k = off; k < n && k < off + 64u; k++) { static const char hx[] = "0123456789abcdef"; char c2[3] = { hx[p[k] >> 4], hx[p[k] & 15], 0 }; if (!((k - off) % 4u)) say(" "); say(c2); }
        say("\n");
        return;                                                    /* unknown framing: drop the rest */
    }
}

void mss_diag_stats(void) { dec(" f3 ", s_f3); dec(" diag-other ", s_other); }
#else
int  mss_diag_cntl_rx(struct smd_chan *c, const uint8_t *p, uint32_t n) { (void)c; (void)p; (void)n; return 0; }
void mss_diag_data_rx(const uint8_t *p, uint32_t n) { (void)p; (void)n; }
void mss_diag_stats(void) {}
#endif
