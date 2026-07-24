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

void con_putc(char c) { uart_putc(c); ramlog_putc(c); }
void con_puts(const char *s) { while (*s) { if (*s == '\n') { uart_putc('\r'); ramlog_putc('\r'); } con_putc(*s++); } }

void con_puthex(uint32_t v) { uart_puthex(v); /* hex also into ramlog */
    ramlog_putc('0'); ramlog_putc('x');
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t n = (v >> i) & 0xf;
        ramlog_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}

void con_putdec(uint32_t v)
{
    char buf[11]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) con_putc(buf[i]);
}
