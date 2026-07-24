#pragma once

#include <sdkconfig.h>

#ifdef CONFIG_ENABLE_CHIPOBLE
#define CONFIG_NETWORK_LAYER_BLE 1
#else
#define CONFIG_NETWORK_LAYER_BLE 0
#endif

#ifndef CONFIG_CHIP_ENABLE_EXTERNAL_PLATFORM
#define BLE_PLATFORM_CONFIG_INCLUDE <platform/ESP32/BlePlatformConfig.h>
#endif
