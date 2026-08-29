/* bootmark.c — boot-progress breadcrumbs in IMEM, readable from a rooted
 * Linux on the watch AFTER the boot attempt has already failed.
 *
 * WHY (the 2026-08-02 dead end): the real firmware booted, sat on aboot's logo
 * for ~15 s, rebooted to stock, and did NOT buzz — and changing the code
 * changed nothing observable. With one bit of output (the motor) that is
 * downstream of MMU + timer + SPMI, "no buzz" cannot distinguish "never
 * reached main()" from "SPMI is misprogrammed", and those need opposite fixes.
 * The DDR ramlog cannot help either: it links at ~0x811a2000, INSIDE the
 * region the next kernel loads into, so booting anything to read it destroys
 * it first.
 *
 * IMEM does not have that problem. It is on-die SRAM at 0x08600000 (DTB:
 * qcom,msm-imem@8600000, 4 KB), it keeps its contents across a warm reset, and
 * Linux only ever touches the specific offsets its DTB node claims:
 *     0x010 mem_dump_table   0x01c dload_type   0x0c8 diag_dload
 *     0x65c restart_reason   0x6b0 boot_stats   0x6d0 kaslr_offset
 *     0x94c pil
 * The 0x700..0x94b gap is unclaimed, so this file uses 0x800..0x83f.
 *
 * LAYOUT (little-endian words at 0x08600800):
 *   +0x00  magic 'OWFB' (0x4F574642) — written by startup.S before anything
 *   +0x04  stage: highest boot stage reached (see BOOTMARK_* below)
 *   +0x08  aux0 } stage-specific values, e.g. the framebuffer address and
 *   +0x0c  aux1 } geometry we detected, or an error code
 *   +0x10  aux2
 *   +0x14  aux3
 * Reading it back (AsteroidOS root shell, after the failed boot):
 *   devmem 0x08600800 ; devmem 0x08600804 ; devmem 0x08600808 ...
 *
 * Cost: a handful of stores on the boot path, no dependencies at all — no
 * MMU, no timer, no SPMI, no display. That is the entire point.
 */
#include "platform.h"
#if defined(PLAT_SOC_MSM)

/* DISABLED BY DEFAULT (2026-08-02). Two reasons, both learned the hard way:
 *   1. UNREADABLE. The watch's Linux is built CONFIG_DEVMEM=n and
 *      CONFIG_PSTORE=n (confirmed in the on-device config dump), so there is
 *      no /dev/mem, no devmem, no pstore — nothing can read IMEM back. The
 *      breadcrumbs would be written and never seen.
 *   2. POTENTIALLY HARMFUL. Qualcomm IMEM ranges can be XPU-protected against
 *      non-secure writes; a protected store faults, which would HANG the boot
 *      at the very first instruction and masquerade as "our code never ran".
 * Kept compiled-out so the mechanism is ready if a channel ever appears (a
 * kernel module, a rooted stock kernel with CONFIG_DEVMEM=y, or our own
 * firmware reading it after a warm reset). Build with -DUSE_IMEM_MARKS. */
#define BM_BASE   0x08600800u
#define BM_MAGIC  0x4F574642u   /* "OWFB" */

void bootmark(uint32_t stage)
{
#if defined(USE_IMEM_MARKS)
    mmio_write(BM_BASE + 0x00, BM_MAGIC);
    mmio_write(BM_BASE + 0x04, stage);
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)stage;
#endif
}

void bootmark_aux(unsigned idx, uint32_t val)
{
#if defined(USE_IMEM_MARKS)
    if (idx > 3) return;
    mmio_write(BM_BASE + 0x08 + 4u * idx, val);
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)idx; (void)val;
#endif
}

#endif /* PLAT_SOC_MSM */
