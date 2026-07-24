/* compat/esp_bt.h — ESP-IDF BT controller / BLE TX-power stub (no radio here). */
#pragma once
#include <cstdint>

typedef int esp_err_t;

/* BLE TX power levels (full ESP enum range; boards reference a subset). */
typedef enum {
    ESP_PWR_LVL_N27 = 0, ESP_PWR_LVL_N24, ESP_PWR_LVL_N21, ESP_PWR_LVL_N18,
    ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9,  ESP_PWR_LVL_N6,
    ESP_PWR_LVL_N3,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P3,  ESP_PWR_LVL_P6,
    ESP_PWR_LVL_P9,  ESP_PWR_LVL_P12, ESP_PWR_LVL_P15, ESP_PWR_LVL_P18,
    ESP_PWR_LVL_P20, ESP_PWR_LVL_P21,
} esp_power_level_t;

typedef enum {
    ESP_BLE_PWR_TYPE_CONN_HDL0 = 0, ESP_BLE_PWR_TYPE_ADV = 9,
    ESP_BLE_PWR_TYPE_SCAN = 10, ESP_BLE_PWR_TYPE_DEFAULT = 11,
} esp_ble_power_type_t;

typedef enum {
    ESP_BT_CONTROLLER_STATUS_IDLE = 0,
    ESP_BT_CONTROLLER_STATUS_INITED,
    ESP_BT_CONTROLLER_STATUS_ENABLED,
} esp_bt_controller_status_t;

static inline esp_err_t esp_ble_tx_power_set(esp_ble_power_type_t, esp_power_level_t) { return 0; }
static inline esp_power_level_t esp_ble_tx_power_get(esp_ble_power_type_t) { return ESP_PWR_LVL_N0; }
static inline esp_bt_controller_status_t esp_bt_controller_get_status(void) { return ESP_BT_CONTROLLER_STATUS_IDLE; }
