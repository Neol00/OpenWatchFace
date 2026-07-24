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
