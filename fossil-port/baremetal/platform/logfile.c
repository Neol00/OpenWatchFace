/* logfile.c — mirror the ramlog into a READABLE FILE on the FFat volume.
 *
 * 2026-08-06, the end of blind debugging. Until now the only way to see what
 * the firmware thought was happening was to paint colours on the glass and
 * have the user decode them by eye — which cost us weeks and never once gave
 * a usable answer. With FFat mounting read/write (STORAGE34), the driver's own
 * narration can simply be a text file the user opens in the Files app.
 *
 * Design notes:
 *  - Source of truth is the existing ramlog ring (every con_puts already lands
 *    there), so NOTHING else in the port has to change to become loggable.
 *  - The file is (re)created once per boot, and the FIRST flush dumps whatever
 *    the ring already holds — which, because .ramlog is NOLOAD and survives a
 *    warm reboot, includes the PREVIOUS boot's tail. So a crash-and-reboot is
 *    still recoverable: its log is at the top of the next boot's file.
 *  - Every wait is bounded and every failure is silent-and-sticky: logging must
 *    never be able to take the watch down, and must never log about logging
 *    (that recurses through the very ring it is draining).
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#include "../fatfs/ff.h"

#define RAMLOG_MAGIC 0x4C46574Fu   /* "OWFL" — must match ramlog.c */

struct ramlog {
    uint32_t magic;
    uint32_t head;
    uint32_t wrapped;
    uint32_t reserved;
    char     buf[];
};

extern char __ramlog_start[], __ramlog_end[];
#define RL      ((volatile struct ramlog *)__ramlog_start)
#define RL_CAP  ((uint32_t)(__ramlog_end - __ramlog_start) - 16u)

#define LOGFILE_PATH   "/owf-log.txt"
#define FLUSH_PERIOD   1000u        /* ms between syncs (bounded write load) */

static FIL      s_fil;
static int      s_state;            /* 0 unopened, 1 open, -1 failed forever */
static uint32_t s_tail;             /* ring index already written to the file */
static uint32_t s_last_ms;

static int logfile_open(void)
{
    /* FA_CREATE_ALWAYS: one clean file per boot. The previous boot is not
     * lost — it is still in the ring and goes out in the first flush below. */
    if (f_open(&s_fil, LOGFILE_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        s_state = -1;               /* volume not mounted yet, or no space */
        return -1;
    }
    s_state = 1;
    return 0;
}

static int wr(const char *p, uint32_t n)
{
    UINT bw = 0;
    if (n == 0u) return 0;
    if (f_write(&s_fil, p, n, &bw) != FR_OK || bw != n) return -1;
    return 0;
}

/* Append everything the ring gained since the last call. Cheap and rate
 * limited; safe to call every loop() iteration. */
void logfile_flush(void)
{
    if (s_state < 0) return;
    if (RL->magic != RAMLOG_MAGIC) return;

    uint32_t now = timer_ms();
    if (s_state == 1 && (uint32_t)(now - s_last_ms) < FLUSH_PERIOD) return;

    uint32_t head = RL->head;
    if (head >= RL_CAP) return;                  /* ring not sane; stay quiet */

    if (s_state == 0) {
        if (logfile_open() < 0) return;          /* retried next call */
        /* First flush: emit the whole ring in CHRONOLOGICAL order. If it has
         * wrapped, the oldest bytes are the ones AFTER head. */
        if (RL->wrapped) {
            if (wr((const char *)RL->buf + head, RL_CAP - head) < 0) goto fail;
        }
        if (wr((const char *)RL->buf, head) < 0) goto fail;
        s_tail = head;
    } else if (head != s_tail) {
        if (head > s_tail) {
            if (wr((const char *)RL->buf + s_tail, head - s_tail) < 0) goto fail;
        } else {                                  /* wrapped since last flush */
            if (wr((const char *)RL->buf + s_tail, RL_CAP - s_tail) < 0) goto fail;
            if (wr((const char *)RL->buf, head) < 0) goto fail;
        }
        s_tail = head;
    } else {
        s_last_ms = now;
        return;                                   /* nothing new */
    }

    s_last_ms = now;
    if (f_sync(&s_fil) != FR_OK) goto fail;       /* survive an abrupt reboot */
    return;

fail:
    f_close(&s_fil);
    s_state = -1;                                 /* sticky: never retry-loop */
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
