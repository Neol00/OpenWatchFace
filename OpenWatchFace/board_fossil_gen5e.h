/* board_fossil_gen5e.h -- Fossil Gen 5E "sole": the Gen 5 board with a 390x390 AUO AMOLED
 * (qcom,mdss_dsi_auo_390p_amoled_cmd). Generated from board_fossil_gen5.h 2026-09-15; keep in step. */
/* board_fossil_gen5.h — Fossil Gen 5 "Carlyle HR" (triggerfish), Wear 3100.
 *
 * App-side companion to snapdragon-port/baremetal/boards/fossil_gen5.h, which
 * holds the runtime addresses. Everything here is either FROM this watch's own
 * DTB (dumped 2026-09-10, ../snapdragon-port/dtbs/triggerfish-stock.dts) or
 * explicitly marked as unproven.
 *
 * WHAT IS ACTUALLY PROVEN ON HARDWARE, as of the first boot:
 *   - our code executes on the Wear 3100 (colour bands reached the glass);
 *   - MDP3 + DSI takeover and the command-mode kickoff work;
 *   - SPMI works, so the PM660 haptics block was selected correctly.
 * Everything else in this file is a transcription, not an observation.
 *
 * The Gen 5 is a hybrid: the Gen 4's msm8909 AP and Raydium touch, the Gen 6's
 * PM660 PMIC and haptics, and a 416x416 panel that is neither watch's. Where a
 * feature is unproven it is switched OFF here rather than inherited hopefully —
 * a subsystem that fails at runtime on a watch with no console is much more
 * expensive than one that is simply absent. */
#pragma once

#define BOARD_NAME   "Fossil Gen 5E"
#define BOARD_VENDOR "Fossil"          /* About screen; without it device_info.h says "Unknown" */
#define BOARD_OTA_KEY "fossil-gen5-triggerfish"

#define BOARD_PLATFORM_FOSSIL 1
#define BOARD_DISPLAY_MSM_DSI 1   /* MDP3/DSI command-mode panel — PROVEN */
#define BOARD_TOUCH_RAYDIUM   1   /* raydium@39 on i2c@78b9000, as the Gen 4 */

#define BOARD_HAS_PSRAM           0   /* plain malloc into DDR (1 GB here) */
#define BOARD_DUAL_CORE           1   /* CPU1 booted by smp_8909.c (2026-09-10); parked in WFI
                                       * with idle accounting, and serving the frame push */
#define BOARD_HAS_PMU_AXP2101     0
#define BOARD_HAS_ADC_BATTERY     0
#define BOARD_HAS_RTC_PCF85063    0   /* PMIC RTC over SPMI (reads only, see below) */
#define BOARD_HAS_AUDIO_Q6        1   /* BG amp + the modem's audio DSP (mss_apr.c / bgcom.c) */
#define BOARD_HAS_AUDIO_ES8311    0
#define BOARD_HAS_AUDIO_PWM       0
#define BOARD_HAS_HAPTICS         1   /* PM660 qpnp-haptics @0xc000 (pmic_vib.c),
                                         driven through the virtual motor pin */
#define BOARD_HAS_SD_MMC          0
#define BOARD_HAS_SD_SPI          0
/* NO SENSORS. The Gen 4 has an LSM6DS3 on bit-banged SPI and a PAH8011 PPG on
 * I2C; this watch's tree has NEITHER, and no accelerometer or PPG node of any
 * kind. That fits the rest of the picture: /dev on the running watch exposes
 * glink_pkt_bg_* channels (rsb, sso, display), so on the Wear 3100 the sensors
 * — like the crown — belong to the QCC1110 co-processor and are reached over
 * GLINK, not by the AP over a bus we can drive. */
#define BOARD_HAS_IMU_QMI8658     0
#define BOARD_HAS_IMU_LSM6DS3     0
/* PixArt PAH8011 PPG (2026-09-11): the same part and I2C address as the Gen 4,
 * per this watch's own sensor registry -- INT on gpio34, pins probed at boot
 * (snapdragon-port boards/fossil_gen5.h). */
#define BOARD_HAS_HR_PAH8011      1
#define BOARD_HAS_BACKLIGHT_PWM   0   /* AMOLED: brightness by panel command */
#define BOARD_HAS_LP_STEPS        0
#define BOARD_WAKE_USE_EXT0       0
/* BLE ON (2026-09-10). NimBLE host over HCI-on-SMD through the WCNSS core,
 * exactly as on the Gen 4. The only thing that ever held this off was that the
 * WCNSS firmware had never run on this watch; as of v245 it boots first time,
 * scans and completes a WPA2 connect (snapdragon-port notes 46-54).
 * Address and bonds persist in NVS once storage owns userdata (see below);
 * until then they are regenerated every boot. */
#define BOARD_HAS_BLE             1
/* FFAT ON (2026-09-10): the user is converting this watch. Mirrors the Gen 4.
 * This does NOT wipe Wear OS by itself: storage_gen6.c refuses a userdata
 * partition that still holds ext4 / f2fs / an encrypted Android volume and
 * boots read-only instead ("no storage", settings in RAM). Converting is an
 * explicit step -- `fastboot erase userdata` once -- after which the next boot
 * writes our superblock and formats the FFat region, and NVS (settings, WiFi
 * networks, BLE address and bonds) persists. The full stock eMMC backup is in
 * snapdragon-port/firmware/gen5-C3F9453E4746/. */
#define BOARD_HAS_FFAT            1

/* BLE TX-power ladder placeholder. Referenced UNCONDITIONALLY by
 * settings_store.h — it is a table the settings UI reads, not something gated
 * on BOARD_HAS_BLE — so it must exist even on a board whose BLE is off, or the
 * app TU will not compile. Never applied while BLE is off. */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9, \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -15, -12, -9, -6, 0, 9, 20 }

#define BOARD_HW_SUMMARY \
  "Fossil Gen 5 Carlyle HR (triggerfish) - APQ8009W Wear 3100 + PM660, " \
  "416x416 AUO AMOLED, Raydium touch, 1 GB"

/* ---- Panel geometry — FROM-DTB and confirmed on the glass ------------------
 * qcom,mdss_dsi_auo_416p_amoled_cmd: 416x416 (0x1a0), 45 fps, 24 bpp. The
 * Raydium node's display-coords agree at 0x1a0, which is the independent
 * check. NOTE this is NOT the Gen 4's 454x454 — same AUO family, different
 * panel — so nothing here may be copied from board_fossil_gen4.h by size. */
#define LCD_WIDTH  390
#define LCD_HEIGHT 390
#define BOARD_SCREEN_ROUND   1
#define BOARD_LCD_EVEN_ALIGN 0
#define BOARD_PARTIAL_BUF_LINES 64

#if defined(WDOG_TRACE)
/* NOT wdog_stage(n) on this watch. That ladder (msm_wdog.c k_stage_sec) reprograms the APPS
 * watchdog to an ABSOLUTE few-second bite time per marker -- 7 s at stage 17, 15 s at stage 22 --
 * and its table says outright "these are reached ~2-3 s into the boot". Here the app does not
 * start until +85 s, because it waits for the modem, so stage 22 armed a 15 s bite partway
 * through setup() with nothing petting until loop(). setup() cannot cover the rest (LVGL, touch,
 * the IMU and HR probes) in 15 s while the bgcom codec test busy-waits on the same CPU, so the
 * watchdog bit every boot, always in the same place, and looked exactly like a hard crash late
 * in setup(). It cost gen5-modem-19..26. Pet instead: the safety net stays (main.c's 120 s
 * window, restarted at each marker) without the bogus clamp. */
#include <stdint.h>
extern "C" void owf_stage_mark(void);   /* arduino_main.cpp: wdog_extend(120) + console/USB pump + DSP write service */
#define OWF_STAGE(n) ((void)(n), owf_stage_mark())
#endif
#if defined(VISUAL_TRACE)
#include <cstdint>
extern "C" void fb_trace(uint32_t xrgb);
#define OWF_VTRACE(c) fb_trace(c)
#endif

/* Panel brightness: DCS 0x51, no PWM backlight on an AMOLED. */
extern "C" int dsi_dcs_set_brightness(unsigned char level);

/* ---- Dummy pin / bus constants (same contract as the Gen 4/Gen 6) --------- */
#define IIC_SDA          0
#define IIC_SCL          0
#define TP_INT           0
#define TP_RESET         0
#define HAPTICS_MOTOR_GPIO  200
#define HAPTICS_ACTIVE_HIGH 1
/* PULSE LENGTH. Back to 35 ms after 60 ms was tried and rejected (2026-09-12:
 * "stronger but slow, it feels delayed").
 *
 * The lesson, so it is not re-learned: on an ERM, LENGTH IS NOT STRENGTH past
 * the point the rotor is moving. A longer pulse mostly extends the spin-up and
 * the coast-down, and those tails are what a wearer feels as sluggishness --
 * so a longer, harder pulse reads as a slow "wrrrr" rather than a firmer tick.
 * Crispness comes from the hardware BRAKE (see pmic_vib.c, on since v296),
 * amplitude comes from PLAT_HAP_VMAX_MV
 * (snapdragon-port/baremetal/boards/fossil_gen5.h, at the block maximum), and
 * this value should stay short enough to read as a tap. Below ~25 ms the motor
 * does not spin up at all.
 *
 * It is also the blocking time of every UI click -- haptics_pulse() runs the
 * whole pulse inline (see haptics.h) -- which is a second reason not to grow
 * it. */
#define HAPTICS_CLICK_MS    35

/* ---- Rotating crown: the QCC1110's RSB ------------------------------------
 * There is no PixArt sensor on this watch. The crown is the co-processor's
 * "Rotary Side Button": events arrive over the bgcom SPI link as FIFO frames
 * (id 0xFFFE) once the BG firmware is loaded and the RSB_CTRL GLINK channel
 * has been configured + enabled (platform/bgcom.c, working since v272,
 * 2026-09-12). The driver exposes the SAME four calls as the Gen 4's PAT9126,
 * so crown_nav.h is unchanged; only the scale differs: the BG reports ONE
 * signed count per detent, where the optical sensor gave ~40 per flick.
 * The crown comes alive ~40 s after boot (the BG bring-up waits for the RPM
 * channel); until then crown_take_delta() simply returns 0. */
#define BOARD_HAS_CROWN          1
#define CROWN_SCROLL_PX_PER_CNT  48   /* pixels of scroll per detent */
#define CROWN_SHADE_OPEN_CNT     2    /* detents of "roll down" that open the shade */
#define CROWN_IDLE_RESET_MS      600  /* quiet gap that clears a part-made gesture */

extern "C" {
  int  crown_init(void);        /* 0 once the RSB is enabled, -1 before */
  void crown_poll(void);        /* IRQ-gated bgcom FIFO drain; call every loop */
  int  crown_take_delta(void);  /* signed detents since the last call */
  int  crown_present(void);
}

#define BOOT_BTN_GPIO    201
/* No extra side pushers on the Gen 5E: no Buttons app. */
#define BOARD_HAS_EXTRA_BUTTONS 0
#define BOARD_WAKE_GPIO  0
#define BOARD_LCD_BUS_HZ 0
#define BOARD_LOOP_IDLE_MS 10
