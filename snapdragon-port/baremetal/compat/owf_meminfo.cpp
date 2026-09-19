/* owf_meminfo.cpp — see owf_meminfo.h for where each number comes from.
 *
 * Compiled HOSTED (nano.specs) because it calls mallinfo(): newlib-nano's
 * nano-malloc maintains uordblks/fordblks itself, so this is a plain struct
 * read, no arena walk and no allocation. Safe to call from the UI task.
 */
#include <malloc.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "owf_meminfo.h"
extern "C" uint32_t plat_ddr_size(void);
extern "C" uint32_t ddr_size_detect(void);   /* platform/ddr_size.c */

/* HOW MUCH DDR THIS WATCH ACTUALLY HAS — detected, never assumed.
 *
 * OWF_DDR_SIZE (owf_meminfo.h) is chosen by SOC TIER, and DDR size is not a
 * property of the SoC tier: every PLAT_SOC_MSM8909 board took the 512 MB
 * branch, so the Fossil Gen 5 reported 512 MB on a 1 GB watch even though its
 * own board header (boards/fossil_gen5.h) says 1 GB. Worse, the TicWatch C2
 * and C2+ are the SAME board key, the SAME dtb and the SAME image and differ
 * ONLY in RAM, 512 MB against 1 GB -- so no compile-time constant can be right
 * for both, on any board key we could invent. It has to be read at runtime.
 *
 * ddr_size.c does exactly that from two independent sources (aboot's device
 * tree, then the SMEM usable-RAM table) and was written for this, but nothing
 * ever called it. Called lazily HERE rather than at boot on purpose: this runs
 * from the UI task when the About screen asks, long after the failure window
 * that got the boot-time call removed in v83 (see arduino_main.cpp), and by
 * then SMEM is up because the radio bring-up has run.
 *
 * Cached on success only, so an early call that predates SMEM does not freeze
 * the fallback in. OWF_DDR_SIZE stays as the last resort. */
static uint32_t owf_ddr_total(void)
{
    static uint32_t s_cached = 0;
    if (s_cached) return s_cached;
    uint32_t b = ddr_size_detect();
    if (b < (128u * 1024u * 1024u)) return OWF_DDR_SIZE;   /* implausible: do not cache */
    if (b == OWF_DDR_SIZE) { s_cached = b; return b; }     /* agrees with the constant */
    s_cached = b;
    return b;
}

/* linker.ld */
extern "C" char __image_start[], __image_end[];

/* The no-map carveouts from the DEVICE dtb's /reserved-memory, mirrored from
 * mmu.c's k_tz_holes (which maps them Device-XN so the A53 cannot speculate
 * into them). DDR that exists but that we may never touch. */
static const struct { uint32_t first_mb, last_mb; } k_reserved[] = {
    { 0x85B, 0x867 },   /* other_ext: TZ apps / QSEE / smem */
    { 0x868, 0x885 },   /* modem firmware                   */
    { 0x886, 0x8A5 },   /* adsp firmware                    */
    { 0x8A6, 0x8AF },   /* wcnss firmware                   */
    { 0x900, 0x913 },   /* splash framebuffer               */
};

extern "C" void owf_meminfo(owf_meminfo_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);

    out->ddr_base  = OWF_DDR_BASE;
    out->ddr_total = owf_ddr_total();   /* detected at runtime; see above */
    for (unsigned i = 0; i < sizeof k_reserved / sizeof k_reserved[0]; i++)
        out->ddr_reserved += (k_reserved[i].last_mb - k_reserved[i].first_mb + 1u) << 20;

    out->image_base   = (uint32_t)(uintptr_t)__image_start;
    out->static_bytes = (uint32_t)(__image_end - __image_start);

    struct mallinfo mi = mallinfo();
    out->malloc_cap   = owf_sbrk_cap();
    out->malloc_arena = owf_sbrk_arena();
    out->malloc_used  = (uint32_t)mi.uordblks;
    /* Allocatable = free blocks inside the granted arena + the part of the cap
     * _sbrk has not handed out yet. Both are really available to malloc. */
    out->malloc_free  = (uint32_t)mi.fordblks +
                        (out->malloc_cap - out->malloc_arena);

    out->rtos_total    = (uint32_t)configTOTAL_HEAP_SIZE;
    out->rtos_free     = (uint32_t)xPortGetFreeHeapSize();
    out->rtos_min_free = (uint32_t)xPortGetMinimumEverFreeHeapSize();
}

extern "C" uint32_t owf_mem_free_heap(void)
{
    owf_meminfo_t m;
    owf_meminfo(&m);
    return m.malloc_free;
}

extern "C" uint32_t owf_mem_largest_block(void)
{
    /* nano-malloc does not track a largest-free-block. The ungranted tail of the
     * sbrk cap IS contiguous, so it is a true lower bound on what one allocation
     * can still get — honest, and the number that actually matters for "will
     * this framebuffer fit". */
    owf_meminfo_t m;
    owf_meminfo(&m);
    return m.malloc_cap - m.malloc_arena;
}
