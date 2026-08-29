/* owf_meminfo.h — real memory accounting for the Fossil bare-metal port.
 *
 * There is no OS to ask, so every number here is derived from the three things
 * that actually own memory on this target:
 *
 *   1. DDR itself. 1 GB at 0x80000000 (two 512 MB banks, 0x80000000+0x20000000
 *      and 0xA0000000+0x20000000) — read from the DEVICE dtb, i.e. the bootloader-
 *      patched /memory node, not the stock source dtb whose reg is <0 0 0 0>.
 *      The SoC's only other RAM is 4 KB of system IMEM (0x8600000, boot cookies)
 *      and 32 KB of RPM message RAM (0x60000, RPM-owned mailbox) — neither is
 *      general-purpose, so DDR is effectively the whole memory budget.
 *   2. The newlib malloc arena, grown by _sbrk from `end` (owf_sbrk.cpp).
 *      This is where LVGL, String, and every heap_caps_* call land.
 *   3. The FreeRTOS heap_4 pool (ucHeap, configTOTAL_HEAP_SIZE in .bss) —
 *      task stacks, queues, timers.
 *
 * NOTE ON DOUBLE COUNTING: ucHeap lives inside .bss, so rtos_total is already
 * included in static_bytes. Report them as separate lines, never add them.
 */
#pragma once
#include <stdint.h>
#include "plat_soc_tier.h"   /* PLAT_SOC_* — derived, not -D'd; see that header */

/* Mirrors boards/fossil_gen<N>.h, which is NOT on the C++ include path in
 * build-owf-image.sh. Kept here (one place) rather than restated per-file.
 *
 * SAFE_END is the first no-map carveout: everything from the end of the image
 * up to here is contiguous DDR that belongs to us alone, and it is what bounds
 * the malloc arena. There IS more usable DDR above the carveouts (0x8B0-0x8FF
 * and 0x914-0xC00 MB), but reaching it needs a non-contiguous heap. */
/* SoC tier: both Wear 2100 watches. Before this the guard was board-only and
 * the TicWatch C2 fell through to the Gen 6 branch, so its About screen
 * claimed 1 GB of DDR on a 512 MB watch — and, more seriously, the malloc
 * arena was bounded by the wrong SAFE_END. The C2's own device tree confirms
 * the numbers are shared: splash_region@83000000 (+12 MB) and the same four
 * no-map carveouts, byte-identical to firefish's. */
#if defined(PLAT_SOC_MSM8909)
/* msm8909w (firefish, skipjack): 512 MB in one bank at 0x80000000 — its ramoops carveout
 * sits at 0x9FF00000, one megabyte below 0xA0000000, which pins the top.
 *
 * SAFE_END stops BELOW splash_region@83000000 (+12 MB), not at the first
 * no-map carveout. That region is aboot's own framebuffer and the MDP may
 * still be scanning out of it: the display path (platform/fb_mdp3.c) takes
 * the engine over and repoints it at our buffer, but if that takeover fails
 * the engine keeps reading the splash region forever — and a heap that had
 * grown into it would be rendered onto the glass, or worse, be written under
 * a live DMA read. Giving up the ~78 MB between the splash region and
 * external_image@87B00000 costs nothing today (the arena's high-water mark
 * is a few MB) and removes a whole class of failure. */
#define OWF_DDR_BASE      0x80000000u
#define OWF_DDR_SIZE      (512u * 1024u * 1024u)
#define OWF_DDR_SAFE_END  0x83000000u
#else
#define OWF_DDR_BASE      0x80000000u
#define OWF_DDR_SIZE      (1024u * 1024u * 1024u)
#define OWF_DDR_SAFE_END  0x85B00000u
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* --- physical facts (compile-time board constants, DTB-confirmed) --- */
    uint32_t ddr_base;        /* 0x80000000                                  */
    uint32_t ddr_total;       /* 1 GB                                        */
    uint32_t ddr_reserved;    /* TrustZone/modem/adsp/wcnss/splash carveouts  */

    /* --- what this firmware occupies --------------------------------------
     * static_bytes = __image_end - __image_start: text + rodata + data + bss
     * (ucHeap included) + ramlog + the SVC/IRQ stacks. */
    uint32_t image_base;
    uint32_t static_bytes;

    /* --- newlib malloc arena (_sbrk region above the image) --------------- */
    uint32_t malloc_cap;      /* hard ceiling _sbrk will grant                */
    uint32_t malloc_arena;    /* granted so far (sbrk high-water)             */
    uint32_t malloc_used;     /* live allocations                             */
    uint32_t malloc_free;     /* free-in-arena + not-yet-granted = allocatable */

    /* --- FreeRTOS heap_4 (ucHeap) ----------------------------------------- */
    uint32_t rtos_total;
    uint32_t rtos_free;
    uint32_t rtos_min_free;   /* worst case ever seen, for headroom tuning    */
} owf_meminfo_t;

void owf_meminfo(owf_meminfo_t *out);

/* Convenience for the shim headers (esp_system.h / esp_heap_caps.h). */
uint32_t owf_mem_free_heap(void);       /* allocatable malloc bytes           */
uint32_t owf_mem_largest_block(void);   /* best-case contiguous malloc bytes  */

/* owf_sbrk.cpp */
uint32_t owf_sbrk_arena(void);          /* bytes granted by _sbrk so far      */
uint32_t owf_sbrk_cap(void);            /* the ceiling                        */

#ifdef __cplusplus
}
#endif
