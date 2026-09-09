/* kmsg_forensics.c -- print the PREVIOUS kernel's printk ring after a warm
 * reboot from Wear OS into this image (-DKMSG_FORENSICS, Gen 6).
 *
 * startup.S copies [__bss_start, __bss_end) to KF_COPY before zeroing .bss;
 * the stock 4.14 kernel's .bss (and its log_buf) sits inside that range.
 * The range between our __bss_end and KF_COPY was never touched and is
 * scanned in place. Records are 4.14 printk_log: 16-byte header
 * {u64 ts_nsec; u16 len; u16 text_len; u16 dict_len; u8 facility; u8 flags}
 * followed by the text (no NUL). We do not trust the header: we find
 * printable runs and only use the header when it agrees with the run. */
#include "platform.h"
#if defined(KMSG_FORENSICS)
#include <stdint.h>
#include <string.h>

extern char __bss_start[], __bss_end[];
#define KF_COPY 0x84000000u   /* above splash (0x83000000+0xC00000), below PLAT_DDR_SAFE_END */

static const char *const k_keys[] = {
    "pil", "wcnss", "pronto", "scm", "subsys", "iris", "pas_", "PAS", "tz",
    "qsee", "Linux version", "Booting Linux", "wlan", "cnss", "ssr", "restart"
};

static int kf_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int kf_has_key(const volatile uint8_t *p, uint32_t n)
{
    for (unsigned k = 0; k < sizeof k_keys / sizeof k_keys[0]; k++) {
        const char *key = k_keys[k]; uint32_t kl = (uint32_t)strlen(key);
        if (kl > n) continue;
        for (uint32_t i = 0; i + kl <= n; i++) {
            uint32_t j = 0;
            while (j < kl && kf_lower(p[i + j]) == kf_lower((uint8_t)key[j])) j++;
            if (j == kl) return 1;
        }
    }
    return 0;
}

static uint32_t s_lines, s_runs;

static void kf_emit(const volatile uint8_t *base, uint32_t s, uint32_t n, uint32_t orig_base)
{
    uint32_t hdr_ok = 0; uint32_t ts_lo = 0, ts_hi = 0;
    if (s >= 16u && ((s & 3u) == 0u)) {
        const volatile uint8_t *h = base + s - 16u;
        uint16_t len = (uint16_t)(h[8] | (h[9] << 8));
        uint16_t tl  = (uint16_t)(h[10] | (h[11] << 8));
        if (tl == n && len >= n + 16u && len < n + 16u + 1024u) {
            hdr_ok = 1;
            ts_lo = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
            ts_hi = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
        }
    }
    con_puthex(orig_base + s); con_puts(hdr_ok ? " [" : " {");
    if (hdr_ok) {
        /* ts in ns -> seconds.milliseconds (64-bit divide via double is fine here) */
        uint64_t ts = ((uint64_t)ts_hi << 32) | ts_lo;
        uint32_t sec = (uint32_t)(ts / 1000000000ull), ms = (uint32_t)((ts % 1000000000ull) / 1000000ull);
        char b[24]; int i = 0; char tmp[16]; int t = 0;
        do { tmp[t++] = (char)('0' + sec % 10u); sec /= 10u; } while (sec);
        while (t) b[i++] = tmp[--t];
        b[i++] = '.'; b[i++] = (char)('0' + ms / 100u); b[i++] = (char)('0' + (ms / 10u) % 10u); b[i++] = (char)('0' + ms % 10u);
        b[i] = 0; con_puts(b);
    }
    con_puts(hdr_ok ? "] " : "} ");
    if (n > 240u) n = 240u;
    for (uint32_t i = 0; i < n; i++) con_putc((char)base[s + i]);
    con_putc('\n');
    s_lines++;
    if ((s_lines & 15u) == 0u) { con_flush(); usb_poll(); deadman_kick(); wdog_extend(60u); blackbox_sync(); }
}

static void kf_scan(uint32_t addr, uint32_t len, uint32_t orig_base, const char *label)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    uint32_t i, run_start = 0, in_run = 0;
    con_puts("kf: scanning "); con_puts(label); con_puts(" orig "); con_puthex(orig_base);
    con_puts(" len "); con_puthex(len); con_puts("\n"); con_flush(); usb_poll();
    for (i = 0; i < len; i++) {
        uint8_t c = p[i];
        int pr = (c >= 0x20u && c < 0x7Fu);
        if (pr && !in_run) { in_run = 1; run_start = i; }
        else if (!pr && in_run) {
            uint32_t n = i - run_start;
            in_run = 0;
            if (n >= 8u) {
                s_runs++;
                if (kf_has_key(p + run_start, n)) kf_emit(p, run_start, n, orig_base);
            }
        }
        if ((i & 0xFFFFFu) == 0xFFFFFu) { con_flush(); usb_poll(); deadman_kick(); wdog_extend(60u); }
    }
}

/* TrustZone ring, whole buffer including stale bytes past the write position
 * (a warm reboot re-inits the header but does not scrub the ring). Plain
 * con_puts so no LOG_VERBOSE is needed. */
#define KF_TZ_MAGIC 0x747a6461u
void kf_tz_ring_dump(void)
{
    uint32_t w = mmio_read(PLAT_TZLOG_PTR), diag = 0, ring_off, ring_len, i, run = 0;
    const volatile uint8_t *p;
    if (w == KF_TZ_MAGIC) diag = PLAT_TZLOG_PTR;
    else if ((w >= 0x08600000u && w < 0x08700000u) || (w >= 0x80000000u && w < 0x90000000u)) diag = w;
    con_puts("kf: tz diag ptr "); con_puthex(w); con_puts("\n"); con_flush(); usb_poll();
    if (!diag) { con_puts("kf: no tz diag header\n"); return; }
    ring_off = *(volatile uint32_t *)(uintptr_t)(diag + 0x1Cu);
    ring_len = *(volatile uint32_t *)(uintptr_t)(diag + 0x20u);
    con_puts("kf: tz ring_off "); con_puthex(ring_off); con_puts(" ring_len "); con_puthex(ring_len);
    con_puts(" pos "); con_puthex(*(volatile uint32_t *)(uintptr_t)(diag + ring_off)); con_puts("\n");
    if (*(volatile uint32_t *)(uintptr_t)diag != KF_TZ_MAGIC || ring_off > PLAT_TZLOG_SIZE ||
        ring_len == 0u || ring_off + ring_len > PLAT_TZLOG_SIZE) { con_puts("kf: tz ring geometry implausible\n"); return; }
    con_puts("kf: ---- tz ring ----\n");
    p = (const volatile uint8_t *)(uintptr_t)(diag + ring_off + 4u);
    for (i = 0; i + 4u < ring_len; i++) {
        uint8_t c = p[i];
        if (c == '\n' || (c >= 0x20u && c < 0x7Fu)) { con_putc((char)c); run++; }
        else if (run) { con_putc('\n'); run = 0; }
        if ((i & 0x3FFu) == 0x3FFu) { con_flush(); usb_poll(); deadman_kick(); wdog_extend(60u); }
    }
    con_puts("\nkf: ---- tz end ----\n"); con_flush(); usb_poll(); blackbox_sync();
}

void kmsg_forensics_report(void)
{
    uint32_t bss_len = (uint32_t)(__bss_end - __bss_start);
    s_lines = 0; s_runs = 0;
    con_puts("kf: ---- previous kernel log (keyword lines) ----\n");
    kf_scan(KF_COPY, bss_len, (uint32_t)(uintptr_t)__bss_start, "bss-copy");
    kf_scan((uint32_t)(uintptr_t)__bss_end, 0x83000000u - (uint32_t)(uintptr_t)__bss_end,
            (uint32_t)(uintptr_t)__bss_end, "in-place");
    con_puts("kf: ---- end: "); con_puthex(s_runs); con_puts(" text runs, ");
    con_puthex(s_lines); con_puts(" keyword lines ----\n"); con_flush(); usb_poll(); blackbox_sync();
}
#else
void kmsg_forensics_report(void) {}
void kf_tz_ring_dump(void) {}
#endif
