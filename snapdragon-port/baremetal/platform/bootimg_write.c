/* bootimg_write.c — put a downloaded, verified Android boot image into the
 * `boot` partition. The over-the-air installer's last step on the Wear 2100
 * watches, where there is no Linux, no dd and no second slot: aboot loads
 * whatever sits in `boot`, so this file replaces our own image in place.
 *
 * ORDER OF OPERATIONS, because there is no A/B here:
 *   1. The caller has already checked size, hash and the board marker.
 *   2. Everything is checked against the partition BEFORE the write window is
 *      armed: magic, fit, GPT lookup.
 *   3. Blocks 1..n-1 are written first and block 0 (the "ANDROID!" header)
 *      LAST, so an interruption mid-way leaves a header that still describes
 *      the OLD image over new bytes — not bootable either, but it makes the
 *      failure obvious at aboot instead of running half an image.
 *   4. Every block is read back and compared. A mismatch is reported; nothing
 *      can be undone at that point, which is why step 1 and 2 are strict.
 * The watchdog is petted through the loop: 2.6 MB is ~5000 single-block
 * CMD24 writes, well over the 11 s bark on a slow card.
 *
 * Recovery if this ever leaves a bad image: fastboot on the C2/S2 (button
 * combo) or TWRP via `fastboot boot`; the Gen 4 has no combo, so the caller
 * must be conservative about what it hands in. */
#include "platform.h"
#include <string.h>

int bootimg_write(const void *img, uint32_t len, void (*progress)(uint32_t, uint32_t))
{
    const uint8_t *p = (const uint8_t *)img;
    uint32_t lba, nblk, need, i;
    static uint8_t rb[512] __attribute__((aligned(32)));

    if (!img || len < 2048u || memcmp(p, "ANDROID!", 8) != 0) {
        con_puts("bootimg: not an Android boot image\n"); return -1;
    }
    if (emmc_gpt_find("boot", &lba, &nblk) < 0) {
        con_puts("bootimg: no `boot` partition in GPT\n"); return -2;
    }
    need = (len + 511u) / 512u;
    if (need > nblk) { con_puts("bootimg: image larger than the boot partition\n"); return -3; }
    con_puts("bootimg: boot @"); con_puthex(lba); con_puts(" +"); con_putdec(nblk);
    con_puts(" blocks, image "); con_putdec(need); con_puts(" blocks\n");

    if (emmc_write_window_boot(lba, need) < 0) return -4;

    /* blocks 1..need-1, then 0 */
    for (i = 1; i <= need; i++) {
        uint32_t b = (i == need) ? 0u : i;
        uint32_t off = b * 512u;
        const uint8_t *src = p + off;
        if (off + 512u > len) {                 /* tail block: zero-pad */
            memset(rb, 0, sizeof rb);
            memcpy(rb, src, len - off);
            src = rb;
        }
        if (emmc_write_block(lba + b, src) < 0) {
            con_puts("bootimg: WRITE FAILED at block "); con_putdec(b); con_puts("\n");
            return -5;
        }
        if ((i & 31u) == 0u) { wdog_pet(); if (progress) progress(i, need * 2u); }
    }
    wdog_pet();

    /* read back */
    for (i = 0; i < need; i++) {
        uint32_t off = i * 512u, n = (off + 512u <= len) ? 512u : len - off;
        if (emmc_read_block(lba + i, rb) < 0 || memcmp(rb, p + off, n) != 0) {
            con_puts("bootimg: VERIFY FAILED at block "); con_putdec(i); con_puts("\n");
            return -6;
        }
        if ((i & 63u) == 0u) { wdog_pet(); if (progress) progress(need + i, need * 2u); }
    }
    if (progress) progress(need * 2u, need * 2u);
    con_puts("bootimg: written and verified\n");
    return 0;
}
