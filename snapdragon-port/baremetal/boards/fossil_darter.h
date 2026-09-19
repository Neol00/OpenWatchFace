/* fossil_darter.h -- Fossil Sport ("darter", Compal, Wear 3100 / APQ8009W + PM660 + QCC1110,
 * 390x390 AUO x120 AMOLED). Stock DT (dtbs/darter-stock.dts, from its own boot partition,
 * serial C2F9143C0864, 2026-09-15): model "Compal Electronics Inc, Darter, based on Qualcomm
 * Snapdragon Wear 3100", qcom,board-id <8 0x113>, qcom,msm-id <0x109 0 0x12d 0>, aboot matched
 * board-id <0x10008 0x113> msm-id <0x12d 0x20000> pmic-id <0x2001b>.
 * Against the sole (Gen 5E) tree the ONLY hardware differences are: the panel node (same 390x390
 * geometry and control gpios, different AUO init sequence and PHY timings -- irrelevant on the
 * framebuffer-takeover path), the panel AVDD enable wrapped as bob_vreg (same PM660 gpio 12),
 * a gpio_keys STEM_1 pusher on TLMM 90 (the Gen 5 header already has it), a bg-rsb node (the
 * darter HAS a rotating crown, l11 + l15 as on the Gen 5), pon_2 = a second PMIC key, touch
 * reset delays 5/360 (Gen 5 values) and a 355 mAh battery profile. Everything else is byte for
 * byte the sole tree, so the Gen 5 header is inherited whole. */
#pragma once
#include "fossil_gen5.h"
#undef PLAT_NAME
#define PLAT_NAME           "fossil-darter"        /* Fossil Sport, 41/43 mm */
#undef PLAT_PANEL_W
#undef PLAT_PANEL_H
#define PLAT_PANEL_W        390u
#define PLAT_PANEL_H        390u
/* Touch: raydium@39, display-coords 390x390, hard-reset 5 ms / soft-reset 360 ms (= Gen 5). The
 * 390p firmware on the 5E had no host cmd 0x33 (display-mode notify) and sending it corrupted
 * the frame; the darter's is a 390 panel too, so the command is withheld here as well. The 5E's
 * settle/stuck filters are kept: they only cost 600 ms after reset. */
#define PLAT_TOUCH_SETTLE_MS     600u
#define PLAT_TOUCH_STUCK_MS      3000u
#define PLAT_TOUCH_NO_DISPLAY_MODE_CMD 1
/* Crown: present (qcom,bg-rsb + RSB_CTRL glink channel in the tree), so the Gen 5 BG + RSB
 * bring-up stays enabled. bg-wear in this unit's modem image is the 2021-01-07 build
 * (490792 B), not the Gen 5's. */
