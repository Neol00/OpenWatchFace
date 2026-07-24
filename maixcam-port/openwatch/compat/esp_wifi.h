/* compat/esp_wifi.h — ESP-IDF WiFi control stub (TX-power only; no radio here). */
#pragma once
#include <cstdint>

typedef int esp_err_t;

static inline esp_err_t esp_wifi_set_max_tx_power(int8_t)   { return 0; }
static inline esp_err_t esp_wifi_get_max_tx_power(int8_t *p){ if (p) *p = 0; return 0; }
