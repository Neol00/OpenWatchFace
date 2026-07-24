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
#include "board.h"   // BOARD_PLATFORM_TUYA — must precede the gate below
#include <lvgl.h>

/* Two backends for the same nine lv_*_core symbols, picked by platform:
 *   - ESP boards: heap_caps_malloc(MALLOC_CAP_SPIRAM)  (esp_heap_caps.h)
 *   - Tuya T5:    tal_psram_malloc()                   (tal_memory.h)
 * Both target external PSRAM. The T5 distinction is CRITICAL: on the T5, plain
 * malloc()/tal_malloc() hits the small ~640 KB SRAM heap, while tal_psram_malloc()
 * hits the 16 MB PSRAM — they're SEPARATE heaps. LVGL must use PSRAM or a heavy
 * draw (layer buffers + caches) exhausts SRAM and the device silently resets. */
#if (LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM)

#if BOARD_PLATFORM_TUYA
/* ---- Tuya T5 backend: external PSRAM via the TKL psram heap ----------------
 * Use tkl_system_psram_malloc/free/realloc — the EXACT functions the SDK's own
 * LVGL allocator uses (src/liblvgl/v9/port/lv_port_mem.c). NOT tal_psram_*: the
 * tal_* abstraction layer wraps tkl_* with extra bookkeeping that is NOT safe to
 * call from the LVGL worker render thread concurrently with the loop task —
 * which is exactly what a ROUNDED/CIRCLE fill does (lv_draw_sw_fill allocates a
 * radius mask buffer per draw via lv_malloc, on the worker thread; plain rects
 * never allocate). That mismatch crashed every menu with circular buttons. tkl_*
 * is the raw heap the SDK trusts for this. Declared directly to avoid the
 * ENABLE_EXT_RAM-gated prototypes in tkl_memory.h; symbols link from the SDK. */
extern "C" {
  void *tkl_system_psram_malloc(size_t size);
  void  tkl_system_psram_free(void *ptr);
  void *tkl_system_psram_realloc(void *ptr, size_t size);
}

extern "C" {

void lv_mem_init(void)   { /* PSRAM heap is brought up by the SDK at boot */ }
void lv_mem_deinit(void) { /* nothing to tear down */ }

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  (void)mem; (void)bytes; return NULL;     /* one global PSRAM heap */
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size)               { return tkl_system_psram_malloc(size); }
void *lv_realloc_core(void *p, size_t new_size) { return tkl_system_psram_realloc(p, new_size); }
void lv_free_core(void *p)                      { tkl_system_psram_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  if (mon_p) { lv_mem_monitor_t z = {}; *mon_p = z; }
}
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

#else
/* ---- ESP backend: external PSRAM via heap_caps ---------------------------- */
#include "esp_heap_caps.h"

/* Caps for every LVGL allocation. On a board WITH PSRAM (S3) point them at the
 * external 8 MB so the big caches/layers live there and free up internal SRAM.
 *
 * On a board WITHOUT PSRAM (the C6 — BOARD_HAS_PSRAM 0) MALLOC_CAP_SPIRAM can
 * NEVER be satisfied: heap_caps_malloc(MALLOC_CAP_SPIRAM) returns NULL for every
 * request. That made the very first lv_malloc inside lv_init() (the layout list)
 * return NULL, and lv_flex_init() then stored through that NULL pointer -> the
 * "Store access fault @ 0xc" boot loop. So on a no-PSRAM board route LVGL to
 * INTERNAL SRAM instead. The caches/layers in lv_conf.h are already shrunk under
 * BOARD_HAS_PSRAM==0 so they fit the C6's ~512 KB SRAM. */
#if BOARD_HAS_PSRAM
#define LV_PSRAM_CAPS  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define LV_PSRAM_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

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

#endif  // BOARD_PLATFORM_TUYA

#endif  // LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
