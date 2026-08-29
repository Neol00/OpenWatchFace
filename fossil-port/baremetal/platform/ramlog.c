/* ramlog.c — post-mortem ring log in the .ramlog section (NOLOAD, not zeroed).
 *
 * Why it exists: the watch has no guaranteed UART and no JTAG. DDR contents
 * survive a warm reboot, so everything printed here can be recovered after a
 * hang/crash by rebooting into fastboot and dumping the region around
 * __ramlog_start (address is printed at boot and recorded in the .map file).
 */
#include "platform.h"

#define RAMLOG_MAGIC 0x4C46574Fu   /* "OWFL" */

struct ramlog {
    uint32_t magic;
    uint32_t head;      /* next write index into buf */
    uint32_t wrapped;
    uint32_t reserved;
    char     buf[];
};

extern char __ramlog_start[], __ramlog_end[];
#define RL       ((struct ramlog *)__ramlog_start)
#define RL_CAP   ((uint32_t)(__ramlog_end - __ramlog_start) - (uint32_t)sizeof(struct ramlog))

static int s_had_previous;

void ramlog_init(void)
{
    if (RL->magic == RAMLOG_MAGIC && RL->head < RL_CAP) {
        s_had_previous = 1;              /* keep prior boot's contents */
        const char mark[] = "\n--- reboot ---\n";
        for (const char *p = mark; *p; p++) ramlog_putc(*p);
        return;
    }
    RL->magic = RAMLOG_MAGIC;
    RL->head = 0;
    RL->wrapped = 0;
}

void ramlog_putc(char c)
{
    if (RL->magic != RAMLOG_MAGIC) return;   /* init not run yet */
    RL->buf[RL->head++] = c;
    if (RL->head >= RL_CAP) { RL->head = 0; RL->wrapped = 1; }
}

int ramlog_had_previous(void) { return s_had_previous; }

/* Stream the ring from a cursor — the USB log console's source (usb_ci.c).
 *
 * The cursor is a monotonically increasing byte count, NOT a ring index, so a
 * reader that falls behind a wrap is detected rather than silently replaying
 * garbage: if more than a full buffer has gone by, jump to the oldest byte
 * still present. Returns bytes copied (0 when caught up). A fresh cursor of 0
 * therefore replays the WHOLE ring, including everything printed before the
 * cable was plugged in, which is the entire point of the exercise.
 */
uint32_t ramlog_read(uint32_t *cursor, char *out, uint32_t max)
{
    if (RL->magic != RAMLOG_MAGIC) return 0;

    uint32_t cap     = RL_CAP;
    uint32_t written = RL->wrapped ? (cap + RL->head) : RL->head;
    uint32_t oldest  = (written > cap) ? (written - cap) : 0u;

    if (*cursor < oldest) *cursor = oldest;      /* fell behind a wrap */
    if (*cursor >= written) return 0;            /* caught up */

    uint32_t n = written - *cursor;
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++)
        out[i] = RL->buf[(*cursor + i) % cap];
    *cursor += n;
    return n;
}
