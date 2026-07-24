/* compat/esp_heap_caps.h — PSRAM/heap-caps allocation -> plain malloc on Linux. */
#pragma once
#include <cstdlib>
#include <cstddef>

/* Capability flags are meaningless on a unified-memory Linux host; accept any. */
#define MALLOC_CAP_8BIT     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_DMA      (1 << 2)
#define MALLOC_CAP_SPIRAM   (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 4)
#define MALLOC_CAP_DEFAULT  (1 << 5)

static inline void *heap_caps_malloc(size_t size, uint32_t)          { return malloc(size); }
static inline void *heap_caps_calloc(size_t n, size_t s, uint32_t)   { return calloc(n, s); }
static inline void *heap_caps_realloc(void *p, size_t size, uint32_t){ return realloc(p, size); }
static inline void  heap_caps_free(void *p)                          { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t)               { return 16 * 1024 * 1024; }
static inline size_t heap_caps_get_largest_free_block(uint32_t)      { return 16 * 1024 * 1024; }

/* ps_malloc/ps_calloc (Arduino PSRAM helpers) map to malloc too. */
static inline void *ps_malloc(size_t size)        { return malloc(size); }
static inline void *ps_calloc(size_t n, size_t s) { return calloc(n, s); }
static inline void *ps_realloc(void *p, size_t s) { return realloc(p, s); }
