/* nvs.h — raw NVS iterator shim (IDF 5 signatures). Only ble_provision.h's
 * orphan-key pruning uses it; our key-value store has no iterator, so the
 * find reports "not found" and the prune is a no-op. */
#pragma once
#include <cstddef>
typedef void* nvs_iterator_t;
typedef int nvs_type_t;
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define NVS_TYPE_ANY 0x42
typedef struct { char namespace_name[16]; char key[16]; nvs_type_t type; } nvs_entry_info_t;
static inline esp_err_t nvs_entry_find(const char*, const char*, nvs_type_t, nvs_iterator_t *it) { if (it) *it = nullptr; return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_entry_next(nvs_iterator_t *it) { if (it) *it = nullptr; return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t*) { return ESP_OK; }
static inline void nvs_release_iterator(nvs_iterator_t) {}
