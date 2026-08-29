/* owf_sbrk.cpp — newlib heap backend for the fossil bare-metal port.
 *
 * WHY THIS EXISTS (2026-08-03, found via the lv_indev_create boot hang):
 * with --specs=nosys.specs, newlib's default _sbrk grows the heap from the
 * linker `end` symbol and REFUSES any growth that passes the CURRENT STACK
 * POINTER — a sane check on a flat bare-metal system, and exactly wrong on
 * this one: our FreeRTOS task stacks are allocated inside ucHeap in .bss, at
 * addresses BELOW `end`. The moment code runs on a task stack, every fresh
 * arena extension fails with ENOMEM. Allocations that fit already-granted
 * arena keep working, so the failure surfaces arbitrarily deep into boot
 * (LVGL's display create squeaked through; lv_indev_create got NULL and
 * LVGL's assert handler spun forever = the 21.5 s watchdog reading), and
 * String operations can fail silently-empty long before that.
 *
 * This _sbrk serves the region the linker set aside for it: from `end` upward,
 * to the first TrustZone carveout at 0x85B00000 (see mmu.c k_tz_holes). Object
 * files beat archive members at link time, so this cleanly replaces the nosys
 * version.
 *
 * THE CEILING (2026-08-07): this was a flat +8 MB. That was a conservative
 * round number, not a hardware limit — it left ~64 MB of contiguous, uncontested
 * DDR unusable and would have been tight under XIP. The bound is now derived
 * from OWF_DDR_SAFE_END, so it tracks the image growing rather than needing a
 * hand-edit. Everything between `end` and SAFE_END is ours: the FreeRTOS task
 * stacks live in ucHeap in .bss (BELOW `end`), not up here.
 *
 * __malloc_lock/unlock: newlib malloc is not reentrant and both the UI task
 * and the net task allocate; back the locks with the FreeRTOS scheduler
 * suspension (safe pre-scheduler too — it is a no-op counter then).
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "owf_meminfo.h"        /* OWF_DDR_SAFE_END */

extern "C" char end[];              /* linker: first free byte after the image */
static char *s_cur;

/* Runtime, not a constant: `end` moves whenever the image or ucHeap changes. */
static inline uint32_t sbrk_cap(void)
{
    uintptr_t e = (uintptr_t)end;
    return e < OWF_DDR_SAFE_END ? (uint32_t)(OWF_DDR_SAFE_END - e) : 0u;
}

/* Reported by the About screen (owf_meminfo.cpp). "arena" is the sbrk
 * high-water mark, NOT live usage — malloc keeps freed blocks in the arena. */
extern "C" uint32_t owf_sbrk_arena(void) { return s_cur ? (uint32_t)(s_cur - end) : 0u; }
extern "C" uint32_t owf_sbrk_cap(void)   { return sbrk_cap(); }

extern "C" void *_sbrk(ptrdiff_t incr)
{
    char *lim = end + sbrk_cap();
    if (!s_cur) s_cur = end;
    if (incr > 0 ? (s_cur + incr > lim || s_cur + incr < s_cur)
                 : (s_cur + incr < end)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *p = s_cur;
    s_cur += incr;
    return p;
}

extern "C" void __malloc_lock(struct _reent *)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) vTaskSuspendAll();
}

extern "C" void __malloc_unlock(struct _reent *)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) (void)xTaskResumeAll();
}
