/* nvs.h — raw NVS iterator STUB (only used by the BLE bond-pruning path, off). */
#pragma once
#include <cstddef>
typedef void* nvs_iterator_t;
typedef int nvs_type_t;
#define NVS_TYPE_ANY 0x42
typedef struct { char namespace_name[16]; char key[16]; nvs_type_t type; } nvs_entry_info_t;
static inline nvs_iterator_t nvs_entry_find(const char*, const char*, nvs_type_t) { return nullptr; }
static inline nvs_iterator_t nvs_entry_next(nvs_iterator_t) { return nullptr; }
static inline void nvs_entry_info(nvs_iterator_t, nvs_entry_info_t*) {}
static inline void nvs_release_iterator(nvs_iterator_t) {}
