/* fossil_gen5e.h -- Fossil Gen 5E ("sole", Wear 3100 / APQ8009W + PM660 + QCC1110, 42 mm,
 * 390x390 AUO AMOLED). Same platform as the Gen 5 (triggerfish): every PM660 / BG / modem /
 * WiFi decision of fossil_gen5.h applies, only the panel and the identity differ. Stock DT
 * (dtbs/sole-stock.dts, from its own boot partition): model "Fossil Group, Inc. based on
 * APQ8009W-PM660 BG Alpha SDW3100", qcom,board-id <8 0x1e>, qcom,msm-id <0x109 0x20000
 * 0x12d 0x20000>, panel qcom,mdss_dsi_auo_390p_amoled_cmd (0x186 x 0x186). Backup:
 * firmware/gen5e-K4F3040524A0679/ (2026-09-15, via an AsteroidOS root shell). */
#pragma once
#include "fossil_gen5.h"
#undef PLAT_NAME
#define PLAT_NAME           "fossil-sole"          /* Gen 5E, 42 mm */
#undef PLAT_PANEL_W
#undef PLAT_PANEL_H
#define PLAT_PANEL_W        390u
#define PLAT_PANEL_H        390u
/* v442: touch frame is rotated 180 degrees against the panel on this unit (user: down scrolls up) */
/* v448: sole tree raydium,hard-reset-delay-ms 100 / soft-reset-delay-ms 50 (Gen 5: 5 / 360); the chip
 * reports a creeping phantom right after reset and its frame differed between boots. */
#define PLAT_TOUCH_HARD_RESET_MS 100u
#define PLAT_TOUCH_POST_RESET_MS 200u
#define PLAT_TOUCH_SETTLE_MS     600u
#define PLAT_TOUCH_STUCK_MS      3000u
#define PLAT_TOUCH_NO_DISPLAY_MODE_CMD 1   /* v449: host cmd 0x33 does not exist in the 390p firmware */
#define PLAT_NO_ROTARY_CROWN 1   /* v451: the 5E has a plain pusher, no RSB crown */
