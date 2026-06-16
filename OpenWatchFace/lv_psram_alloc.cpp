/* ============================================================================
 *  lv_psram_alloc.cpp — route ALL LVGL heap allocations to external PSRAM.
 *
 *  With lv_conf.h set to `LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM`, LVGL calls
 *  the nine *_core functions below for every allocation it makes: widgets,
 *  styles, the image/style CACHES (LV_CACHE_DEF_SIZE / LV_OBJ_STYLE_CACHE), and
 *  internal draw/layer buffers. We back them with the ESP32-S3's 8 MB PSRAM via
 *  heap_caps_*(MALLOC_CAP_SPIRAM) instead of LVGL's old 64 KB static SRAM pool.
 *
 *  Why:
 *   - Frees the ~64 KB of internal SRAM the builtin pool used (LV_MEM_SIZE).
 *   - Lets the caches actually grow large (they were hard-capped by that 64 KB);
 *     PSRAM has ~7.5 MB free.
 *
 *  IMPORTANT — what does NOT move here:
 *   - The display framebuffer is allocated separately in the .ino with
 *     MALLOC_CAP_INTERNAL and is DMA'd to the panel — it MUST stay in SRAM. It
 *     does not go through lv_malloc, so it is unaffected by this file.
 *
 *  Tradeoff: LVGL object/style data now lives in slower PSRAM, and the software
 *  renderer reads styles while drawing — so this can cost a little frame rate.
 *  It's a pure win on SRAM/cache size, a possible small loss on render speed.
 *  To revert: set LV_USE_STDLIB_MALLOC back to LV_STDLIB_BUILTIN (this file then
 *  compiles to nothing) and rebuild.
 *
 *  These symbols must have C linkage to match LVGL (compiled as C), hence the
 *  extern "C" wrapper. Compiled out entirely unless the CUSTOM allocator is on.
 * ========================================================================== */
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "esp_heap_caps.h"

#define LV_PSRAM_CAPS  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

extern "C" {

void lv_mem_init(void)   { /* PSRAM heap is already initialized at boot */ }
void lv_mem_deinit(void) { /* nothing to tear down */ }

/* We use one global PSRAM heap, so the optional extra-pool API is a no-op. */
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  (void)mem; (void)bytes; return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size) {
  return heap_caps_malloc(size, LV_PSRAM_CAPS);
}

void *lv_realloc_core(void *p, size_t new_size) {
  return heap_caps_realloc(p, new_size, LV_PSRAM_CAPS);
}

void lv_free_core(void *p) {
  heap_caps_free(p);            /* safe on any heap_caps allocation */
}

/* Monitor/self-test aren't supported on the raw heap_caps backend. */
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  if (mon_p) { lv_mem_monitor_t z = {}; *mon_p = z; }
}
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

#endif  // LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
