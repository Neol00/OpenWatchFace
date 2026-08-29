/* console.c — shared text helpers + fan-out to UART and ramlog. */
#include "platform.h"

void uart_puts(const char *s) { while (*s) { if (*s == '\n') uart_putc('\r'); uart_putc(*s++); } }

void uart_puthex(uint32_t v)
{
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t n = (v >> i) & 0xf;
        uart_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}

void uart_putdec(uint32_t v)
{
    char buf[11]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) uart_putc(buf[i]);
}

/* REPEAT SUPPRESSION (2026-08-06).
 *
 * OpenWatchFace still carries its ESP32-era chatter -- "[wake] C6 GPIO0
 * deep-sleep wake arm -> OK", the sleep/power narration in board_sleep.h and
 * sleep_power.h -- all of it emitted through USBSerial, which lands here. None
 * of it means anything on this port (there is no C6, no deep sleep), but it
 * repeats fast enough to wrap the 64 KB ring and push out the lines that
 * matter. Rather than editing the firmware's own logging, drop repeats at the
 * sink: a line identical to one seen in the last few seconds is counted, not
 * stored, and the count is reported when the line is finally allowed through.
 *
 * Nothing is lost that was not already redundant, and the ring now holds
 * minutes of distinct output instead of seconds of the same sentence.
 *
 * Build with -DLOG_NO_DEDUP to disable and get the raw firehose back.
 */
#define LINE_MAX   192
#define DEDUP_SLOTS 24
#define DEDUP_MS   4000u

static char     s_line[LINE_MAX];
static uint32_t s_len;
static uint32_t s_line_t0;

#if !defined(LOG_NO_DEDUP)
static struct { uint32_t hash, last, count; } s_seen[DEDUP_SLOTS];
#endif

static void ramlog_str(const char *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] == '\n') ramlog_putc('\r');
        ramlog_putc(p[i]);
    }
}

static void ramlog_dec(uint32_t v)
{
    char b[11]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) ramlog_putc(b[i]);
}

/* Commit the buffered line to the ramlog, unless it is a recent repeat. */
static void line_commit(void)
{
    if (!s_len) return;
#if !defined(LOG_NO_DEDUP)
    uint32_t h = 2166136261u;                     /* FNV-1a over the line */
    for (uint32_t i = 0; i < s_len; i++) {
        h ^= (uint8_t)s_line[i];
        h *= 16777619u;
    }
    uint32_t now = timer_ms();
    int slot = -1, victim = 0;
    for (int i = 0; i < DEDUP_SLOTS; i++) {
        if (s_seen[i].hash == h) { slot = i; break; }
        if (s_seen[i].last < s_seen[victim].last) victim = i;
    }
    if (slot >= 0) {
        if ((uint32_t)(now - s_seen[slot].last) < DEDUP_MS) {
            s_seen[slot].count++;                 /* suppress */
            s_len = 0;
            return;
        }
        uint32_t reps = s_seen[slot].count;
        s_seen[slot].last = now;
        s_seen[slot].count = 0;
        ramlog_str(s_line, s_len);
        if (reps) {                               /* say what was dropped */
            ramlog_str(" [x", 3);
            ramlog_dec(reps + 1u);
            ramlog_str(" suppressed]", 12);
        }
        ramlog_putc('\r'); ramlog_putc('\n');
        s_len = 0;
        return;
    }
    s_seen[victim].hash = h;
    s_seen[victim].last = now;
    s_seen[victim].count = 0;
#endif
    ramlog_str(s_line, s_len);
    ramlog_putc('\r'); ramlog_putc('\n');
    s_len = 0;
}

static void line_put(char c)
{
    if (c == '\r') return;                        /* normalised on commit */
    if (!s_len) s_line_t0 = timer_ms();
    if (c == '\n') { line_commit(); return; }
    if (s_len < LINE_MAX) s_line[s_len++] = c;
    else { line_commit(); s_line[s_len++] = c; }  /* overlong: cut, keep going */
}

/* Push a partial line through if it has been sitting unterminated. Without
 * this a hang mid-line would lose the most interesting text in the log --
 * exactly the line you want after a crash. Called from the main loop. */
void con_flush(void)
{
    if (s_len && (uint32_t)(timer_ms() - s_line_t0) > 500u) {
        ramlog_str(s_line, s_len);
        s_len = 0;
    }
}

void con_putc(char c) { uart_putc(c); line_put(c); }
void con_puts(const char *s) { while (*s) { if (*s == '\n') uart_putc('\r'); con_putc(*s++); } }

void con_puthex(uint32_t v) { uart_puthex(v); /* hex also into ramlog */
    line_put('0'); line_put('x');
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t n = (v >> i) & 0xf;
        line_put((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}

void con_putdec(uint32_t v)
{
    char buf[11]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) con_putc(buf[i]);
}
