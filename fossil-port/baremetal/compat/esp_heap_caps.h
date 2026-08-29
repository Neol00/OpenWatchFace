/* esp_heap_caps.h — ESP heap-caps allocation -> newlib malloc (single DDR heap). */
#pragma once
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include "owf_meminfo.h"
#define MALLOC_CAP_8BIT     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_DMA      (1 << 2)
#define MALLOC_CAP_SPIRAM   (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 4)
#define MALLOC_CAP_DEFAULT  (1 << 5)
static inline void  *heap_caps_malloc(size_t n, uint32_t)            { return malloc(n); }
static inline void  *heap_caps_calloc(size_t n, size_t s, uint32_t)  { return calloc(n, s); }
static inline void  *heap_caps_realloc(void *p, size_t n, uint32_t)  { return realloc(p, n); }
static inline void   heap_caps_free(void *p)                         { free(p); }
/* One DDR pool: every capability mask answers from the same newlib arena.
 * These were flat 8 MB / 4 MB constants until 2026-08-07 — the About screen was
 * printing the stub, not the watch. */
static inline size_t heap_caps_get_free_size(uint32_t)               { return owf_mem_free_heap(); }
static inline size_t heap_caps_get_largest_free_block(uint32_t)      { return owf_mem_largest_block(); }
static inline void  *ps_malloc(size_t n)         { return malloc(n); }
static inline void  *ps_calloc(size_t n, size_t s){ return calloc(n, s); }
static inline void  *ps_realloc(void *p, size_t n){ return realloc(p, n); }
static inline bool esp_ptr_internal(const void *)    { return false; }
static inline bool esp_ptr_dma_capable(const void *) { return false; }
