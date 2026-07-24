/* ============================================================================
 *  tuya/compat/esp_heap_caps.h - ESP heap-caps allocation -> the T5 heap.
 *
 *  The firmware uses heap_caps_malloc(size, MALLOC_CAP_SPIRAM|INTERNAL|DMA) for its
 *  caches / snapshot buffers. On the T5 the Arduino malloc already routes to the SDK
 *  heap (PSRAM-backed where present), so the capability flags are advisory here and
 *  every alloc goes through malloc/calloc/realloc. The vendor LVGL task owns its own
 *  display buffers, so the firmware-owned render-buf path (which wants DMA-capable
 *  internal SRAM) is gated OUT on this platform anyway.
 *
 *  esp_ptr_internal / esp_ptr_dma_capable report false so any code that probes "did
 *  this land in fast internal SRAM?" takes the conservative (non-async) path.
 *
 *  Included only on the BOARD_PLATFORM_TUYA build (the .ino routes <esp_heap_caps.h>
 *  here, gated per-include).
 * ========================================================================== */
#pragma once
#include <cstdlib>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "tuya_cloud_types.h"
#include "tkl_memory.h"   // tkl_system_malloc/free + tkl_system_psram_malloc/free
/* Free-heap queries: tkl_system_get_free_heap_size() = internal SRAM heap;
 * tkl_system_psram_get_free_heap_size() = the PSRAM heap (separate pool). Declared
 * directly in case the ENABLE_EXT_RAM-gated prototype in tkl_memory.h is absent;
 * symbols link from the SDK. */
INT_T tkl_system_get_free_heap_size(VOID_T);
INT_T tkl_system_psram_get_free_heap_size(VOID_T);
}

/* Capability flags - used to ROUTE the allocation to internal RAM vs the 16 MB PSRAM
 * (T5-E1). MALLOC_CAP_SPIRAM -> tkl_system_psram_*; everything else -> internal heap. */
#define MALLOC_CAP_8BIT     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_DMA      (1 << 2)
#define MALLOC_CAP_SPIRAM   (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 4)
#define MALLOC_CAP_DEFAULT  (1 << 5)

/* IMPORTANT: the T5 build sets CONFIG_PSRAM_AS_SYS_MEMORY=y - the 16 MB PSRAM is mapped
 * INTO the normal system heap. So plain malloc/calloc/realloc already draw from
 * PSRAM-backed memory; we do NOT need (and must not use) the separate
 * tkl_system_psram_* alloc/free pair. Routing everything through malloc keeps a SINGLE
 * heap, so heap_caps_free()/free() can never cross heaps and corrupt - and the firmware
 * still gets its 16 MB. The capability flags are therefore advisory (accept any). */
static inline void  *heap_caps_malloc(size_t size, uint32_t)           { return malloc(size); }
static inline void  *heap_caps_calloc(size_t n, size_t s, uint32_t)    { return calloc(n, s); }
static inline void  *heap_caps_realloc(void *p, size_t size, uint32_t) { return realloc(p, size); }
static inline void   heap_caps_free(void *p)                           { free(p); }
/* Route by capability: MALLOC_CAP_SPIRAM reports the PSRAM pool's free bytes, anything
 * else reports the internal SRAM heap. Without this the About app's "PSRAM free" and
 * "SRAM free" both showed the SAME number (the old code ignored the flag). */
static inline size_t heap_caps_get_free_size(uint32_t caps) {
  if (caps & MALLOC_CAP_SPIRAM) return (size_t)tkl_system_psram_get_free_heap_size();
  return (size_t)tkl_system_get_free_heap_size();
}
static inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
  if (caps & MALLOC_CAP_SPIRAM) return (size_t)tkl_system_psram_get_free_heap_size();
  return (size_t)tkl_system_get_free_heap_size();
}

/* ps_malloc/ps_calloc (Arduino PSRAM helpers) - same unified heap. */
static inline void *ps_malloc(size_t size)        { return malloc(size); }
static inline void *ps_calloc(size_t n, size_t s) { return calloc(n, s); }
static inline void *ps_realloc(void *p, size_t s) { return realloc(p, s); }

/* Pointer-property probes: on the T5 we don't classify pointers, so report "not
 * internal SRAM / not DMA-capable" -> callers fall back to the safe (sync) path. */
static inline bool esp_ptr_internal(const void *)     { return false; }
static inline bool esp_ptr_dma_capable(const void *)  { return false; }
