#pragma once

#define XPOWERS_CHIP_AXP2101

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_RESET 8
#define LCD_WIDTH 410
#define LCD_HEIGHT 502

// TOUCH
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 38
#define TP_RESET 9

// SD
// Arduino-ESP32 core 3.3.0 added SDMMC_CLK/CMD/CS as #defines in this board's
// variant (pins_arduino.h). On those cores, redeclaring them as const ints
// expands the macro into "const int 2 = 2;" -> compile error. Guard each one so
// we only declare the symbols the active core's variant does NOT already provide.
#ifndef SDMMC_CLK
const int SDMMC_CLK = 2;
#endif
#ifndef SDMMC_CMD
const int SDMMC_CMD = 1;
#endif
#ifndef SDMMC_DATA  // variant exposes this as SDMMC_D0, so we always define it
const int SDMMC_DATA = 3;
#endif
#ifndef SDMMC_CS
const int SDMMC_CS = 17;
#endif