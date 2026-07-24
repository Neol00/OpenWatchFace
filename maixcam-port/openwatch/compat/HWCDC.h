/* compat/HWCDC.h — the ESP32-S3 USB-CDC serial type. USBSerial is provided by the
 * Arduino shim (compat/Arduino.h) as a HardwareSerial; nothing else needed here. */
#pragma once
#include "Arduino.h"
typedef HardwareSerial HWCDC;
