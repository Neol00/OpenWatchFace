/* bcb.c — Android's Bootloader Control Block, the way to reach fastboot on a
 * watch whose aboot does not honour the IMEM restart cookie.
 *
 * WHY THIS EXISTS. reboot_to_bootloader() writes LK's documented restart
 * cookie (0x77665500) to IMEM restart_reason, and on the Fossil Gen 4, the
 * TicWatch C2/S2 and the Gen 6 that is enough. On the Fossil Gen 5
 * (triggerfish) it is not: every reboot-to-fastboot lands in RECOVERY instead
 * (observed 2026-09-10, and still true after the TCSR/EDL write was removed as
 * a suspect).
 *
 * The evidence for what this device DOES use came out of its own backup. The
 * misc partition dumped from the running watch contained, in the first 32
 * bytes, the literal string:
 *
 *     bootonce-bootloader
 *
 * That is the Android BCB `command` field, and it is what the stock
 * `adb reboot bootloader` had left there. So on triggerfish the route to
 * fastboot is the BCB, not the IMEM cookie — and aboot's own binary carries
 * the matching strings ("bootonce-bootloader", "boot-recovery"), which is what
 * a bootloader that parses the BCB looks like.
 *
 * The struct (bootable/recovery/bootloader_message.h) is stable across every
 * Android generation:
 *     char command[32];       "bootonce-bootloader" / "boot-recovery" / ""
 *     char status[32];        written BY the bootloader, for recovery to read
 *     char recovery[768];     "recovery\n<args>"
 * Only `command` is touched here. `status` and `recovery` are left exactly as
 * found, because they belong to the stock recovery flow and clobbering them
 * would break a stock OTA that was mid-flight.
 *
 * "bootonce" is the right command rather than plain "bootloader": aboot CLEARS
 * it as it consumes it, so a single reboot goes to fastboot and the next one
 * boots normally. A sticky command on a watch with no reliable button
 * combination would be a trap — every subsequent boot would land in fastboot
 * with no way to say otherwise.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM) && defined(PLAT_HAVE_EMMC_STORAGE)

#include <string.h>

#define BCB_CMD_MAX 32u

/* Write `cmd` into the BCB command field. Returns 0 on success.
 *
 * Every step can fail on a watch whose eMMC we have not proven, so every step
 * is checked and narrated: a silent failure here would send the user to a
 * bootloader that never appears, which is exactly the situation this is meant
 * to end. */
static int bcb_write_command(const char *cmd)
{
    static uint8_t blk[512];
    uint32_t lba = 0, nblk = 0;

    if (emmc_init() < 0) { con_puts("bcb: eMMC not ready\n"); return -1; }
    if (emmc_gpt_find("misc", &lba, &nblk) < 0 || nblk == 0u) {
        con_puts("bcb: no 'misc' partition in the GPT\n");
        return -1;
    }
    if (emmc_read_block(lba, blk) < 0) { con_puts("bcb: misc read failed\n"); return -1; }

    /* Replace ONLY the command field. */
    memset(blk, 0, BCB_CMD_MAX);
    if (cmd) {
        size_t n = strlen(cmd);
        if (n >= BCB_CMD_MAX) n = BCB_CMD_MAX - 1u;
        memcpy(blk, cmd, n);
    }

    /* The write window is a deliberate one-shot fence (sdhci_msm.c): arming it
     * twice locks writes out for the rest of the boot. Nothing else arms it in
     * a build with FFAT off, and one block is all we need. */
    if (emmc_write_window(lba, 1u) < 0) { con_puts("bcb: write window refused\n"); return -1; }
    if (emmc_write_block(lba, blk) < 0)  { con_puts("bcb: misc write failed\n");   return -1; }

    con_puts("bcb: misc command = ");
    con_puts(cmd && cmd[0] ? cmd : "(cleared)");
    con_puts("\n");
    return 0;
}

int bcb_request_bootloader(void) { return bcb_write_command("bootonce-bootloader"); }
int bcb_clear(void)              { return bcb_write_command(""); }

#else
int bcb_request_bootloader(void) { return -1; }
int bcb_clear(void)              { return -1; }
#endif
