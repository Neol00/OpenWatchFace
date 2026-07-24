/* ============================================================================
 *  board_tuya_t5_amoled_175.h — Waveshare T5-E1-Touch-AMOLED-1.75
 *
 *  This is NOT an ESP32 board. The SoC is a Tuya T5-E1 (BK7258-class ARM) running
 *  TuyaOpen, built with the Arduino-TuyaOpen core.
 *
 *  Everything hardware-specific is OFF for bring-up: the firmware's existing
 *  BOARD_HAS_* feature-flag stubs compile the PMU/deep-sleep
 *  modules down to no-ops, so first light needs only display+touch (from the
 *  vendor) + the watch-face/menu UI. Subsystems are then enabled one at a time —
 *  RTC (PCF85063A) and IMU (QMI8658) first, since the firmware already knows those
 *  chips at the register level (only the I2C bus plumbing changes: Wire/tkl_i2c
 *  instead of ESP-IDF). Pin map below is from the board schematic, kept ready for
 *  those phases even though the flags start at 0.
 *
 * ========================================================================== */
#pragma once

#define BOARD_NAME "Waveshare T5-E1-Touch-AMOLED-1.75"

/* ---- Platform flag: gate ESP/Arduino runtime assumptions ------------------
 * Used in the .ino + board layer to skip MCU-only bring-up (Arduino_GFX panel,
 * the firmware's own lv_init/lv_display_create/indev/lv_task_handler, deep sleep,
 * ULP, esp_* calls, etc.). Mirrors BOARD_PLATFORM_MAIX. */
#define BOARD_PLATFORM_TUYA 1

/* ---- Display / touch come from the TuyaOpen SDK (lv_vendor_*), not Arduino --- */
#define BOARD_DISPLAY_CO5300_QSPI 0
#define BOARD_DISPLAY_JD9853_SPI  0
#define BOARD_DISPLAY_TUYA        1   /* external: SDK QSPI AMOLED via lv_vendor */
#define BOARD_TOUCH_FT3168        0
#define BOARD_TOUCH_AXS5106L      0
#define BOARD_TOUCH_TUYA          1   /* external: SDK capacitive touch indev */

/* ---- Capabilities: all hardware subsystems OFF for first light -------------
 * (Existing BOARD_HAS_* stubs no-op the corresponding modules.) Enable per the
 * phase plan in tuya-t5-port/README.md as each subsystem is re-bussed onto the
 * TuyaOpen driver layer. */
#define BOARD_HAS_PSRAM           1   /* T5-E1 module has 16 MB PSRAM */
/* BK7258 is physically 3 cores; the application FreeRTOS-SMP domain that runs our
 * code uses 2 (CONFIG_CPU_CNT=2; the 3rd is a separate coprocessor domain reached by
 * mailbox). So the app IS multi-core. NOTE: on Tuya the band-split render + the
 * firmware's own LVGL OS layer (the usual BOARD_DUAL_CORE consumers) are in the
 * firmware-owned render path, which is EXCLUDED here (the vendor lv_vendor task owns
 * LVGL + its LV_USE_OS). So this flag here only affects non-render task pinning, which
 * the Tuya xTaskCreate shim ignores anyway. Set true to reflect the real hardware. */
#define BOARD_DUAL_CORE           1   /* app SMP domain = 2 cores (BK7258) */
#define BOARD_HAS_PMU_AXP2101     0   /* no PMU — charger is ETA6098, battery via ADC */
#define BOARD_HAS_ADC_BATTERY     1   /* BAT_ADC on IO13 via tkl_adc (TUYA_ADC_NUM_0 ch11) */

/* ---- Battery ADC scaling (GPIO13 -> ADC channel 15) -----------------------
 * board_power.h reads the pin in millivolts (tkl_adc_read_voltage, microvolts/1000) and
 * scales up by the divider ratio:
 *   battery_mv = pin_mv * BOARD_BATT_ADC_MUL * BOARD_BATT_CAL_NUM / BOARD_BATT_CAL_DEN
 * Effective multiplier CALIBRATED against a multimeter: at a known 4.20 V full cell the pin
 * read ~871 mV (essentially the raw 2M/510K divider 2510/510 = 4.922, minus a little ADC offset
 * - NOT Tuya's x4, which under-read by ~0.7 V here). Expressed as MUL=1 * CAL_NUM/CAL_DEN = 4760/1000.
 * To re-trim on another unit: cell_actual / cell_shown * 4760, rounded, into CAL_NUM. The [batt] log 
 * prints pin+cell mV. */
#define BOARD_BATT_ADC_GPIO 13
#define BOARD_BATT_ADC_MUL  1
#define BOARD_BATT_CAL_NUM  4760
#define BOARD_BATT_CAL_DEN  1000
#define BOARD_HAS_RTC_PCF85063    1   /* PCF85063A @0x51 on the shared I2C bus (Wire/tkl_i2c) */
#define BOARD_HAS_AUDIO_ES8311    0   /* not this board (that's the S3 watch's external ES8311) */
#define BOARD_HAS_AUDIO_PWM       0
/* T5 internal codec (analog AUDLP/AUDLN) -> NS4150B Class-D amp -> speaker (H2).
 * Output via the TuyaOpen low-level tkl_ao API; amp enable (NS4150B CTRL) on IO28,
 * active HIGH (10K pulldown holds it in shutdown by default). The backend owns IO28
 * so the amp+codec are fully OFF between sounds. See audio_alarm.h (BOARD_HAS_AUDIO_TUYA). */
#define BOARD_HAS_AUDIO_TUYA      1
#define AUDIO_PIN_CE              28  /* NS4150B CTRL / PA_CTRL enable (active high) */
/* Vibration motor — a MOD: the stock board has none, but every GPIO is broken out (引出),
 * so a motor is added via a low-side NPN switch (or a ready-made coin-motor driver module)
 * on IO14. IO14 is electrically free and the pad physically closest to a GND pad, so a 2-pin
 * header (IO14 + GND) lets the vibrator plug in/out. Active HIGH: GPIO HIGH -> transistor on
 * -> motor runs. A base->GND pull-down (10K, on the driver) holds it OFF while IO14 floats at
 * boot/reset/deep sleep — which matters because the Tuya build's gpio_hold_* are no-ops (the
 * platform retains pads / cold-boots), so the OFF state is enforced in HARDWARE, not by a hold.
 * Driver is board-neutral over pinMode/digitalWrite (TuyaOpen Arduino core) — see haptics.h. */
#define BOARD_HAS_HAPTICS         1
#define HAPTICS_MOTOR_GPIO        14  /* NPN base via 1K (or driver-module signal in); active HIGH */
#define HAPTICS_ACTIVE_HIGH       1
/* INTENSITY: reserved knob, NOT wired up yet. Full strength is too aggressive for this
 * small coin ERM, but IO14 has no hardware PWM (T5 PWM is only on 18/24/32/34/36/19/8/9/
 * 25/33/35/37) and the tkl_timer software-PWM attempt broke haptics entirely, so it was
 * reverted — see haptics.h. The motor runs full strength until intensity is re-done on
 * verified hardware. Left here so it's ready when that lands. */
#define HAPTICS_INTENSITY_PCT     40
/* Per-board button-tick length. haptics_pulse() is an EXACT blocking pulse now, and a coin
 * ERM needs ~20-40 ms to physically spin up enough to feel — anything shorter (the old 6)
 * is imperceptible. 25 = a light-but-felt tick on this (stronger) motor. Buzz strength is
 * length-only (no amplitude PWM), so this is the per-device knob: raise if too faint, lower
 * if too hard (but not below ~20 or it stops registering). Heartbeat is separate (H_DOT/DASH). */
#define HAPTICS_CLICK_MS          28
#define BOARD_HAS_SD_MMC          0   /* not the Arduino SD_MMC path (that's ESP-IDF) */
#define BOARD_HAS_SD_SPI          0
/* microSD on the BK7258 4-bit SDIO (CLK=IO2 CMD=IO3 D0-D3=IO4/5/10/11, CD=IO8), mounted via
 * the TuyaOpen SDK (tkl_fs_mount DEV_SDCARD). Its own backend in sd_card.h, NOT the Arduino
 * SD_MMC/SD libs — so it's a separate flag from BOARD_HAS_SD_MMC/SPI. */
#define BOARD_HAS_SD_TUYA         1
/* No on-flash FAT partition on this T5 build — its 8 MB flash is fully allocated to
 * bootloaders / dual-app / OTA / tiny system+KV partitions (see the SDK partitions.csv);
 * there's no user FS partition, so tkl_fs_mount(DEV_INNER_FLASH) always fails. Persistence
 * uses Tuya's KV store (settings/WiFi via Preferences) + the microSD. So FFat is absent:
 * the Files app hides the "Flash" volume, and storage_fs falls back to SD only. */
#define BOARD_HAS_FFAT            0
#define BOARD_HAS_IMU_QMI8658     1   /* QMI8658 @0x6B on the shared I2C bus (Wire/tkl_i2c) */
#define IMU_QMI8658_ADDR          0x6B /* SDO/SA0 strapped high -> 0x6B (vs 0x6A). INT1=7 INT2=9 */
#define BOARD_HAS_BACKLIGHT_PWM   0   /* brightness via the SDK display API, not PWM */
#define BOARD_HAS_LP_STEPS        0
#define BOARD_WAKE_USE_EXT0       0
#define BOARD_HAS_BLE             0   /* 0 = not the ESP32 Arduino-BLEDevice/NimBLE path. Tuya BLE is
                                      /* implemented separately on the raw NimBLE host shipped in
                                      /* libtuyaos.a - the .inos #elif BOARD_PLATFORM_TUYA branch
                                      /* pulls tuya/ble_tuya.h + ble_gadgetbridge_tuya.h (Phase A:
                                      /* advertise + pair + Gadgetbridge). Keep this 0. */

/* BLE TX-power ladder. BLE is off here, but settings_store.h references these
 * unconditionally to build its radio-power tables — provide a plausible 7-tier
 * ladder (the esp_bt.h stub enums; never actually applied on this platform). */
#define BOARD_BLE_TXP_LVL  { ESP_PWR_LVL_N15, ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, \
                             ESP_PWR_LVL_N6,  ESP_PWR_LVL_N0,  ESP_PWR_LVL_P9, \
                             ESP_PWR_LVL_P20 }
#define BOARD_BLE_TXP_DBM  { -15, -12, -9, -6, 0, 9, 20 }

/* Hardware summary for Settings > About (one line per peripheral). */
#define BOARD_HW_SUMMARY \
  "SoC:     Tuya T5-E1\n" \
  "Display: CO5300 QSPI AMOLED 1.75\n" \
  "Touch:   CST9217 capacitive\n" \
  "RTC:     PCF85063A\n" \
  "IMU:     QMI8658\n" \
  "PMU:     ETA6098 (ADC battery)"

/* ---- Screen geometry ------------------------------------------------------
 * 466x466 ROUND AMOLED - confirmed from the authoritative native board config
 * (TuyaOpen/boards/T5AI/WAVESHARE_T5AI_TOUCH_AMOLED_1_75/board_com_api.c:
 * BOARD_LCD_WIDTH/HEIGHT = 466, x_offset 6). The firmware also reads the live size
 * from the vendor display at runtime; these macros drive STATIC layout decisions.
 * (The earlier "320x480" first-light reading was the WRONG panel — the generic
 * Arduino variant registered an ILI9488; we now register the CO5300 ourselves with
 * the correct 466x466, see tuya/owf_tuya_port.h::owf_tuya_register_panel.)
 *
 * ROUND panel: square framebuffer, corners clipped by the round bezel — use the
 * inscribed-circle layout (BOARD_SCREEN_ROUND), the same path as the GC9A01. */
#define LCD_WIDTH  466
#define LCD_HEIGHT 466
#define BOARD_SCREEN_ROUND 1
#define BOARD_LCD_EVEN_ALIGN 0

/* ---- Display driver + orientation (single config point) -------------------
 * OWF_T5_OWN_PANEL: 0 = drive the panel through the precompiled SDK display stack
 * (tdl_disp_*); 1 = use our OWN CO5300 QSPI driver (tuya/owf_tuya_co5300_qspi.h),
 * which owns the init sequence, the flush, and HARDWARE rotation. The SDK path can
 * only rotate in software at the tdl layer, which our LVGL PARTIAL+zero-copy render
 * path can't use — so a hardware flip (for the button-side swap) requires the own
 * driver. Touch (CST92xx) stays on the SDK either way.
 *
 * OWF_T5_PANEL_ROTATION: 0 or 180 (own driver only). 180 swaps the PWR/USER button
 * ends to match the S3 layout. NOTE: 180° does NOT correct the ~5-10° physical glue
 * tilt — a half-turn preserves line orientation, so the tilt looks identical; only
 * re-seating the panel fixes that. The flip is purely for the button-side swap. */
#ifndef OWF_T5_OWN_PANEL
#define OWF_T5_OWN_PANEL      1
#endif
#ifndef OWF_T5_PANEL_ROTATION
#define OWF_T5_PANEL_ROTATION 180
#endif
/* M1 bring-up: 1 = push R/G/B test bands forever (NO LVGL) to validate QSPI framing, the
 * init sequence, hardware rotation (MADCTL) and the offset in isolation. Set to 0 for the
 * full UI once the bands look right (3 colors top->bottom, full-bleed, upright). */
#ifndef OWF_T5_PANEL_SELFTEST
#define OWF_T5_PANEL_SELFTEST 0
#endif

/* Sleep mode. 0 = panel-off only (no PM sleep vote; everything keeps running). 1 = SUSPEND
 * sleep (the shipping mode): PM low-voltage vote, execution blocks in owf_tuya_suspend_sleep
 * until a wake, then RESUMES IN PLACE — never reboots, so the GPIO19 power latch is never
 * released (the battery-death mechanism of the mode-2 reboot wake). Wake sources: User
 * (GPIO12) / PWR (GPIO18) buttons, plus an AON-RTC TIMED wake for periodic background checks
 * and scheduled alarms/timers (interval NOT a macro — comes from the Power app's
 * background-check period, pulled earlier by a running countdown or armed alarm clock; see
 * the suspend loop in enter_deep_sleep(), sleep_power.h). */
#ifndef OWF_T5_DEEP_SLEEP
#define OWF_T5_DEEP_SLEEP 1
#endif

/* Unused on this platform (no firmware-owned partial render buffers — the SDK's
 * LVGL glue manages its own), but the .ino references it before the platform gate;
 * give it a sane value. */
#define BOARD_PARTIAL_BUF_LINES 156
#define BOARD_LCD_BUS_HZ 80000000

/* ---- Pin map (from the board schematic) -----------------------------------
 * Kept ready for the per-subsystem enable phases. Shared I2C bus carries the RTC,
 * IMU and touch. Pins are Tuya GPIO numbers (TUYA_GPIO_NUM_n in the SDK).
 *
 * The firmware references a few of these before the platform gate (BOOT button,
 * touch INT/RESET, I2C); the rest document the wiring for later phases. The GPIO/
 * I2C call sites are no-ops via the Tuya shim until their subsystem is enabled. */
#define IIC_SCL          20   /* shared: touch + RTC + IMU */
#define IIC_SDA          21
#define IIC_SDA_ALT      21
#define TP_INT           42
#define TP_RESET         43
#define BOOT_BTN_GPIO    12   /* USER button (Key3); PWR/RESET are separate */
#define BOARD_WAKE_GPIO  12
#define PWR_BTN_GPIO     18   /* PWR button (active-LOW); long-press = hardware power-off */

/* Display QSPI (driven by the SDK, listed for reference): CS=23 SCK=22
 * SIO0..3=24/25/26/27 RST=29 TE=31. Touch RESET/INT above.
 * RTC PCF85063A: INT=6.  IMU QMI8658 @0x6B: INT1=7 INT2=9.
 * Audio: PA_CTRL(NS4150B enable)=28; codec AUDLN/AUDLP internal to the T5.
 * microSD (4-bit SDIO): CLK=2 CMD=3 D0=4 D1=5 D2=10 D3=11 CD=8.
 * Battery: BAT_ADC=13 (divider 2.51/0.51); charger status BAT_CHG=30.
 * System power latch: SYS_OUT=18 SYS_EN=19. */
