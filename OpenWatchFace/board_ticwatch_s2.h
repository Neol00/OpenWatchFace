/* ============================================================================
 *  board_ticwatch_s2.h — Mobvoi TicWatch S2 / E2 (tunny), BARE-METAL on one
 *  Cortex-A7.
 *
 *  The C2's sibling: same Wear 2100 SoC, same radios, same sensors, same
 *  button, same storage. What differs is the glass (400x400 instead of
 *  360x360) and the chassis. Everything is inherited from the C2 header and
 *  only the identity and the geometry are overridden. SoC detail lives in
 *  snapdragon-port/baremetal/boards/ticwatch_s2.h.
 * ========================================================================== */
#pragma once
#include "board_ticwatch_c2.h"

#undef  BOARD_NAME
#define BOARD_NAME   "TicWatch S2"
#undef  BOARD_OTA_KEY
#define BOARD_OTA_KEY "ticwatch-s2-tunny"

#undef  BOARD_HW_SUMMARY
#define BOARD_HW_SUMMARY \
  "Host:    TicWatch S2 (Wear 2100, APQ8009W, bare-metal A7)\n" \
  "Display: 400x400 round AMOLED, command mode\n" \
  "         MDP3 DMA_P over 1-lane MIPI-DSI, 24bpp\n" \
  "Touch:   FocalTech FTS @0x38 on BLSP1 QUP5 (2 pt)\n" \
  "Power:   PM8916 VM-BMS (voltage-mode; no current sense)\n" \
  "Storage: eMMC 7824900.sdhci (userdata FFAT)"

/* 400x400 from the S2's own DTB (focaltech,display-coords 0x190). As on the
 * C2 the runtime sizes LVGL from fb_width()/fb_height(), i.e. from what aboot
 * programmed into DMA_P; this is the fallback hint. */
#undef  LCD_WIDTH
#undef  LCD_HEIGHT
#define LCD_WIDTH  400
#define LCD_HEIGHT 400
