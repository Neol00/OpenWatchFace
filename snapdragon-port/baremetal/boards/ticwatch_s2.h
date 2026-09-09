/* ticwatch_s2.h — Mobvoi TicWatch S2 / E2 (tunny), bootloader TUNNY.40010.
 *
 * Same Snapdragon Wear 2100 (APQ8009W / msm8909w) as the TicWatch C2, and the
 * S2's own DTB (snapdragon-port/firmware/s2/emmc/s2-boot-dtb0.dtb, pulled from the
 * boot partition 2026-09-04 via TWRP) differs from the C2's by exactly the
 * parts list: a 400x400 panel (alternative node AUO h1391b, FocalTech
 * display-coords 0x190) where the C2 has a 360x360 Tianma RM69330, an ST21NFC
 * node the C2 lacks, and one pinctrl gpio. Every SoC-tier address, the WCNSS
 * node, the sensors registry (same IMU on SPI gpio8-11, same PAH8011 on
 * gpio6/7), the partition table (boot = mmcblk0p33) and the button are the
 * same. The WCNSS NV blob is byte-identical to the C2's; only the MACs differ.
 *
 * The build therefore passes BOTH -DPLAT_BOARD_TICWATCH_C2 (so every driver
 * that guards on the C2 picks the S2 up unchanged) and -DPLAT_BOARD_TICWATCH_S2
 * (so platform.h lands here). This file is the C2 header plus the few
 * overrides below. fb_mdp3.c still believes DMA_P over these numbers; they
 * size the static takeover framebuffer and the touch coordinate scaling,
 * which is why a C2 image must NOT be flashed on the S2 (360^2 < 400^2). */
#pragma once
#include "ticwatch_c2.h"

#undef  PLAT_NAME
#define PLAT_NAME           "ticwatch-s2"     /* tunny */

/* 400x400 (S2 DTB: focaltech,display-coords 0 0 0x190 0x190; the 1.39"
 * panel's marketing figure agrees for once). UNCONFIRMED on hardware until
 * the first boot log prints fb's DMA_P geometry. */
#undef  PLAT_PANEL_W
#undef  PLAT_PANEL_H
#define PLAT_PANEL_W        400u
#define PLAT_PANEL_H        400u

#undef  PLAT_TOUCH_COORD_W
#undef  PLAT_TOUCH_COORD_H
#define PLAT_TOUCH_COORD_W  400u
#define PLAT_TOUCH_COORD_H  400u
