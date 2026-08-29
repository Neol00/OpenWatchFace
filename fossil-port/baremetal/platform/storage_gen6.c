/* storage_gen6.c — persistent storage inside the userdata partition (Gen 6).
 *
 * THE ARCHITECTURE (2026-08-04, the post-WearOS pivot): this firmware becomes
 * the watch's permanent OS on the boot partition; Wear OS is abandoned. Its
 * userdata partition (p50, 4.65 GiB, the last on the GPT) is repurposed
 * WHOLESALE as this firmware's storage. The GPT entry itself is deliberately
 * left untouched — aboot never reads userdata, fastboot still addresses every
 * partition by name, and not rewriting the partition table removes the single
 * riskiest storage operation entirely.
 *
 * Layout inside userdata (LBAs relative to the partition start):
 *   0                superblock (magic + region table, 1 block)
 *   2048   +16 MiB   BLACKBOX   — periodic eMMC snapshot of the 64 KiB ramlog
 *                    (the port's first readable log channel: pull it with a
 *                    root adb `dd` from a temporary AsteroidOS on recovery,
 *                    later over this firmware's own USB console)
 *   65536  +8 MiB    NVS        — two ping-pong slots for the Preferences
 *                    key-value store (nvs_store.c)
 *   131072 +1 GiB    FFAT       — FAT volume for the Arduino FFat API
 *
 * Safety model: emmc_write_* refuses everything outside a window armed ONCE
 * per boot; the window is exactly the userdata extent found by GPT lookup.
 * A boot where GPT lookup fails arms nothing — the firmware runs volatile,
 * writes go nowhere, and nothing outside userdata is reachable ever.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#include <string.h>

/* STORAGE_DIAG rev 2 (2026-08-04): rev 1 painted at failure time — but
 * storage_init runs BEFORE fb_init and the first FFat.begin happens inside
 * setup(), so every color fired while the display couldn't show it ("No
 * color" verdict). Now the stage is RECORDED here and REPLAYED as one boot
 * color after setup() returns (arduino_main), when the display provably
 * works:
 *   MAGENTA emmc_init failed (controller/clock level)
 *   RED     reads work but GPT/userdata lookup failed
 *   YELLOW  write window / superblock WRITE failed (reads fine!)
 *   BLUE    storage fine, FFat mount+mkfs failed  (fs_glue.cpp)
 *   GREEN   whole chain incl. mount OK            (fs_glue.cpp)
 *   WHITE   storage_init OK but FFat.begin was never called at all */
static uint32_t s_diag_color;
void storage_diag_set(uint32_t c)   { s_diag_color = c; }
uint32_t storage_diag_color(void)   { return s_diag_color; }
#define STG_DIAG(c) storage_diag_set(c)

#define SB_MAGIC0  0x5346574Fu       /* "OWFS" */
#define SB_MAGIC1  0x31524F54u       /* "TOR1" */
#define SB_VERSION 1u

/* region table, in blocks relative to userdata start */
#define REG_BB_LBA    2048u
#define REG_BB_NBLK   32768u         /* 16 MiB */
#define REG_NVS_LBA   65536u
#define REG_NVS_NBLK  16384u         /* 8 MiB  */
#define REG_FFAT_LBA  131072u
#define REG_FFAT_NBLK 2097152u       /* 1 GiB  */

struct superblock {
    uint32_t magic0, magic1, version, boot_count;
    uint32_t bb_lba, bb_nblk, nvs_lba, nvs_nblk, ffat_lba, ffat_nblk;
    uint32_t reserved[6];
};

static uint32_t s_base_lba;          /* userdata absolute start LBA */
static uint32_t s_nblk;
static int      s_ok;
static uint32_t s_boot_count;

int storage_ok(void) { return s_ok; }

uint32_t storage_region_lba(unsigned id)
{
    static const uint32_t rel[3] = { REG_BB_LBA, REG_NVS_LBA, REG_FFAT_LBA };
    return (s_ok && id < 3u) ? s_base_lba + rel[id] : 0u;
}

uint32_t storage_region_nblk(unsigned id)
{
    static const uint32_t n[3] = { REG_BB_NBLK, REG_NVS_NBLK, REG_FFAT_NBLK };
    return (s_ok && id < 3u) ? n[id] : 0u;
}

int storage_init(void)
{
#if defined(STORAGE_STAIRS)
    /* STORAGE8 fix: STORAGE7's staircase never ran — setup()'s FFat.begin /
     * nvs_load lazily call storage_init and the crash fired mid-setup before
     * the first stair. Under the staircase flag this function is INERT; the
     * staircase (post-setup) is the only code allowed to touch the eMMC. */
    return -1;
#else
    uint8_t blk[512];

    if (emmc_init() < 0) {
        con_puts("storage: no eMMC\n");
        STG_DIAG(0xFF00FFu);                /* magenta: controller/emmc level */
        return -1;
    }
#if defined(PLAT_STORAGE_PROBEONLY)
    /* STORAGE5 bisect (STORAGE4 died with reads only): emmc_init's register
     * pokes (clock re-enable + INT_STATUS_ENABLE) have run — STOP before any
     * command reaches the card. Crash -> the pokes; boot -> command traffic. */
    con_puts("storage: PROBEONLY - stopping before first command\n");
    STG_DIAG(0x00FFFFu);                    /* cyan: probe-only boot */
    return -1;
#endif
    if (emmc_gpt_find("userdata", &s_base_lba, &s_nblk) < 0 &&
        emmc_gpt_find_largest(&s_base_lba, &s_nblk) < 0) {
        con_puts("storage: no userdata partition in GPT\n");
        STG_DIAG(0xFF0000u);                /* red: reads work, GPT/name fail */
        return -1;
    }
    if (s_nblk < REG_FFAT_LBA + REG_FFAT_NBLK) {
        con_puts("storage: userdata too small\n");
        STG_DIAG(0xFF0000u);
        return -1;
    }
    if (emmc_write_window(s_base_lba, s_nblk) < 0) { STG_DIAG(0xFFFF00u); return -1; }

    /* superblock: adopt or create */
    if (emmc_read_block(s_base_lba, blk) < 0) return -1;
    struct superblock *sb = (struct superblock *)blk;
    if (sb->magic0 == SB_MAGIC0 && sb->magic1 == SB_MAGIC1) {
        sb->boot_count++;
        s_boot_count = sb->boot_count;
    } else {
        memset(blk, 0, sizeof(blk));
        sb->magic0 = SB_MAGIC0; sb->magic1 = SB_MAGIC1;
        sb->version = SB_VERSION; sb->boot_count = 1;
        sb->bb_lba = REG_BB_LBA;     sb->bb_nblk = REG_BB_NBLK;
        sb->nvs_lba = REG_NVS_LBA;   sb->nvs_nblk = REG_NVS_NBLK;
        sb->ffat_lba = REG_FFAT_LBA; sb->ffat_nblk = REG_FFAT_NBLK;
        s_boot_count = 1;
        con_puts("storage: FIRST BOOT - superblock created\n");
    }
#if defined(PLAT_STORAGE_NOWRITE)
    /* read-only bisect boot: superblock not persisted, regions readable */
    emmc_write_block(s_base_lba, blk);
#else
    if (emmc_write_block(s_base_lba, blk) < 0) { STG_DIAG(0xFFFF00u); return -1; }
#endif

    s_ok = 1;
    STG_DIAG(0xFFFFFFu);   /* white: core OK; fs_glue upgrades to blue/green */
    con_puts("storage: userdata @"); con_puthex(s_base_lba);
    con_puts(" +"); con_putdec(s_nblk);
    con_puts(" blocks, boot #"); con_putdec(s_boot_count); con_puts("\n");
    return 0;
#endif /* STORAGE_STAIRS */
}

/* ---- BLACKBOX -------------------------------------------------------------
 * Snapshot the DDR ramlog (every con_puts byte since power-on, including the
 * previous boot after a warm reset) into the BB region. Header block first
 * content after it; the header is written LAST each flush so a torn flush
 * leaves the previous coherent snapshot readable.
 * Read it from a root shell:
 *   dd if=/dev/block/bootdevice/by-name/userdata skip=$((BASE+2048)) count=130
 * (the header block prints its own decode fields). */
#define BB_MAGIC 0x42464F57u          /* "OWFB" */

struct bb_hdr {
    uint32_t magic, seq, head, wrapped, cap, boot_count;
};

extern char __ramlog_start[], __ramlog_end[];

static uint32_t s_bb_seq;
static uint32_t s_bb_last_ms;

void blackbox_flush(void)
{
    if (!s_ok) return;
    uint32_t now = timer_ms();
    if ((uint32_t)(now - s_bb_last_ms) < 2000u) return;
    s_bb_last_ms = now;

    uint32_t bb = storage_region_lba(0);
    uint32_t cap = (uint32_t)(__ramlog_end - __ramlog_start);
    uint32_t nblk = (cap + 511u) / 512u;
    if (nblk + 1u > storage_region_nblk(0)) return;

    /* Incremental: write only the blocks the ring dirtied since last flush
     * (16-byte ramlog struct precedes buf, hence the +16). Full rewrite on
     * the first flush or after a wrap. */
    static uint32_t s_last_head; static int s_synced;
    uint32_t head = ((const volatile uint32_t *)__ramlog_start)[1];
    uint32_t j0 = 0, j1 = nblk - 1u;
    if (s_synced && head >= s_last_head) {
        j0 = (16u + s_last_head) / 512u;
        j1 = (16u + head) / 512u;
        if (j1 >= nblk) j1 = nblk - 1u;
        if (j0 > j1) j0 = j1;
    }
    for (uint32_t i = j0; i <= j1; i++) {
        if (emmc_write_block(bb + 1u + i,
                             (const uint8_t *)__ramlog_start + i * 512u) < 0)
            return;                    /* quiet: logging about logging loops */
    }
    s_last_head = head; s_synced = 1;

    uint8_t blk[512];
    memset(blk, 0, sizeof(blk));
    struct bb_hdr *h = (struct bb_hdr *)blk;
    h->magic = BB_MAGIC;
    h->seq = ++s_bb_seq;
    h->head = ((const uint32_t *)__ramlog_start)[1];   /* ramlog head */
    h->wrapped = ((const uint32_t *)__ramlog_start)[2];
    h->cap = cap;
    h->boot_count = s_boot_count;
    emmc_write_block(bb, blk);
}


/* storage_show_log — the ramlog tail as TEXT on the glass (fb_text_dump).
 * Filtered to the driver lines that matter (emmc/gcc-sdcc/storage), newest
 * last, sized to what the 8x8 font fits. No LVGL: the LVGL version silently
 * rendered nothing on hardware (2026-08-06). */
void storage_show_log(uint32_t hold_ms)
{
    static char out[1024];
    const uint32_t *h = (const uint32_t *)__ramlog_start;
    const char *buf = (const char *)__ramlog_start + 16;
    uint32_t cap = (uint32_t)(__ramlog_end - __ramlog_start) - 16u;
    uint32_t head = h[1], wrapped = h[2];
    uint32_t avail = wrapped ? cap : head;
    if (avail > 16384u) avail = 16384u;          /* scan window: recent history */

    /* collect matching lines into out[] (oldest first), dropping from the
     * front when full so the NEWEST lines always survive */
    uint32_t olen = 0;
    char line[128];
    uint32_t llen = 0;
    for (uint32_t i = 0; i < avail; i++) {
        uint32_t idx = (head + cap - avail + i) % cap;
        char c = buf[idx];
        if (c == '\r') continue;
        if (c != '\n' && llen < sizeof(line) - 1) { line[llen++] = c; continue; }
        line[llen] = 0;
        int keep = 0;
        for (uint32_t k = 0; k + 4 < llen; k++) {
            if (line[k] == 'e' && line[k+1] == 'm' && line[k+2] == 'm' && line[k+3] == 'c') keep = 1;
            if (line[k] == 'c' && line[k+1] == 'c' && line[k+2] == '-' && line[k+3] == 's') keep = 1;
            if (line[k] == 'E' && line[k+1] == ' ' && (line[k+2] == 'W' || line[k+2] == 'P' || line[k+2] == 'S')) keep = 1;
            if (line[k] == 's' && line[k+1] == 't' && line[k+2] == 'o' && line[k+3] == 'r') keep = 1;
            if (keep) break;
        }
        if (keep && llen) {
            if (olen + llen + 2u > sizeof(out)) {
                /* drop the oldest line to make room */
                uint32_t cut = 0;
                while (cut < olen && out[cut] != '\n') cut++;
                cut++;
                if (cut < olen) { for (uint32_t m = cut; m < olen; m++) out[m - cut] = out[m]; olen -= cut; }
                else olen = 0;
            }
            if (olen + llen + 2u <= sizeof(out)) {
                for (uint32_t m = 0; m < llen; m++) out[olen++] = line[m];
                out[olen++] = '\n';
            }
        }
        llen = 0;
    }
    out[olen] = 0;
    if (!olen) { const char *msg = "RAMLOG EMPTY\n"; uint32_t m = 0; while (msg[m]) { out[m] = msg[m]; m++; } out[m] = 0; }

    fb_text_dump(out);
    uint32_t t0 = timer_ms();
    while ((uint32_t)(timer_ms() - t0) < hold_ms) {
        wdog_pet(); deadman_kick();
        timer_delay_ms(100);
    }
}

#if defined(STORAGE_STAIRS)
/* STORAGE STAIRCASE (2026-08-04). STORAGE5 proved: register pokes boot,
 * command traffic dies ~4-6 s, mechanism unknown — and every crash killed
 * the diag replay too. So: run the WHOLE storage bring-up AFTER setup()
 * (display provably live), one step per ~1.5 s, painting a color BEFORE
 * each step. The last color visible when the watch dies NAMES the killer:
 *   DARK GRAY  gcc_sdcc1_up (GCC clocks)
 *   LIGHT GRAY emmc_init register pokes
 *   BLUE       first CMD17: read LBA 1 (GPT header)
 *   CYAN       full GPT scan (name + largest fallback)
 *   GREEN      64-read burst across the userdata regions (mount-scale load)
 *   YELLOW     superblock read
 *   ORANGE     superblock WRITE (the first write ever)
 *   MAGENTA    64-block write burst into the blackbox region (mkfs-scale)
 *   WHITE      EVERYTHING PASSED (storage verified end to end)
 *   solid RED  a step FAILED SOFTLY (returned error, no crash) — boot goes on
 * The normal pre-setup storage_init is compiled out under this flag; this
 * staircase is the only code that touches the eMMC. */
static int stairs_fail(void)
{
    /* STORAGE33 — THE REASON NO TEXT EVER APPEARED: this function used to
     * paint ~11 s of verdict colors with NO watchdog petting between them,
     * so the dog bit and the watch rebooted before storage_show_log() ever
     * ran. (The renderer itself is verified working — QEMU screenshot,
     * tools/qemu-screenshot.sh with -DGFX_TEXT_TEST.) Now: disarm both
     * watchdogs, log the verdicts AS TEXT, and put the log on the glass
     * immediately. No colors, no timing, no decoding. */
    wdog_pet();
    deadman_kick();
    deadman_disarm();
    wdog_disable();

    uint8_t en = 0;
    int l19 = (spmi_read8(1, 0x5246u, &en) == 0) ? ((en & 0x80u) ? 1 : 0) : -1;

    con_puts("E WHERE ");  con_puthex(emmc_fail_where());
    con_puts(" ERR ");     con_puthex(emmc_last_error());
    con_puts("\n");
    con_puts("E PRES ");
    con_putdec((mmio_read(0x07824900u + 0x24u) >> 16) & 1u);
    con_puts(" CLK ");
    con_putdec((mmio_read((uintptr_t)PLAT_GCC_BASE + 0x42018u) & (1u << 31))
               ? 0u : 1u);
    con_puts(" L19 ");     con_putdec((uint32_t)(l19 + 1));
    con_puts("\n");
    con_puts("E STATE ");  con_puthex(mmio_read(0x07824900u + 0x24u));
    con_puts("\n");

    storage_show_log(600000u);          /* 10 min: read it, photograph it */
    return -1;
}

int storage_stairs(void)
{
    uint8_t blk[512];
#define STAIR(c) do { wdog_pet(); deadman_kick(); fb_trace(c); timer_delay_ms(300); } while (0)

    STAIR(0x303030u);
    gcc_sdcc1_up();                              /* tolerant: halt != crash */

    STAIR(0x909090u);
    if (emmc_init() < 0) return stairs_fail();

    STAIR(0x0000FFu);
    if (emmc_read_block(1, blk) < 0) return stairs_fail();

    STAIR(0x00FFFFu);
    if (emmc_gpt_find("userdata", &s_base_lba, &s_nblk) < 0 &&
        emmc_gpt_find_largest(&s_base_lba, &s_nblk) < 0)
        return stairs_fail();
    if (s_nblk < REG_FFAT_LBA + REG_FFAT_NBLK) return stairs_fail();

    STAIR(0x00FF00u);
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t lba = s_base_lba + (i & 1u ? REG_FFAT_LBA : REG_NVS_LBA) + i;
        if (emmc_read_block(lba, blk) < 0) return stairs_fail();
    }

    STAIR(0xFFFF00u);
    if (emmc_read_block(s_base_lba, blk) < 0) return stairs_fail();

    STAIR(0xFF8000u);
    if (emmc_write_window(s_base_lba, s_nblk) < 0) return stairs_fail();
    if (emmc_write_block(s_base_lba, blk) < 0) return stairs_fail();

    STAIR(0xFF00FFu);
    for (uint32_t i = 0; i < 64; i++) {
        if (emmc_write_block(s_base_lba + REG_BB_LBA + 1u + i, blk) < 0)
            return stairs_fail();
    }

    STAIR(0xFFFFFFu); timer_delay_ms(1000);
    s_ok = 1;                                    /* storage verified live */
    return 0;
}
#endif /* STORAGE_STAIRS */

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
