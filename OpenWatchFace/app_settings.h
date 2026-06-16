/* ============================================================================
 *  app_settings.h — the About screen (+ the generated library-version header).
 *
 *  The old Settings list is gone: its screens (Appearance/Power/WiFi&BLE/About)
 *  are now first-class tiles in the app grid (app_menu.h). This file is just the
 *  About screen now. Header-only, compiled into the .ino TU; INCLUDE AFTER
 *  app_menu.h (it uses the screen shell app_scr / app_screen_begin).
 * ========================================================================== */
#pragma once
#include <lvgl.h>
#include "sd_card.h"         // board-neutral SD: sd_present/sd_card_type/sd_*_bytes + CARD_* enums
#include "esp_heap_caps.h"   // heap_caps_get_free_size + MALLOC_CAP_* for the per-pool memory readout

/* Library versions for the About screen. LVGL / Espressif core / XPowersLib expose
 * compile-time macros (used directly below). The rest have NO version macro, so
 * tools/gen_lib_versions.py reads their library.properties into this generated
 * header. Guarded so the sketch still builds if it hasn't been generated yet. */
#if defined(__has_include)
#  if __has_include("lib_versions.h")
#    include "lib_versions.h"
#  endif
#endif
#ifndef LIBVER_GFX
#  define LIBVER_GFX       "?"
#endif
#ifndef LIBVER_SENSORLIB
#  define LIBVER_SENSORLIB "?"
#endif
#ifndef LIBVER_DRIVEBUS
#  define LIBVER_DRIVEBUS  "?"
#endif

/* ----------------------------- About screen -------------------------------
 * A scrollable identity/spec sheet: product name + version, the hardware vendor
 * and on-board chips, live system stats (chip/flash/psram/free mem/radio name),
 * credits and the software stack versions. */

/* One left-aligned, wrapping text line inside the About scroll column. */
static lv_obj_t *about_line(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_label_set_text(l, text);
  return l;
}

/* A dim, spaced section header (e.g. "DEVICE"). */
static void about_header(lv_obj_t *parent, const char *text) {
  lv_obj_t *h = about_line(parent, &FONT_ABOUT_BODY, ui_accent_soft_hex(), text);
  lv_obj_set_style_pad_top(h, UI_PX(12), 0);
}

static void app_open_about(void) {
  app_screen_begin("About");

  // Scrollable column from just below the title down to the screen bottom (no
  // Back button to leave room for anymore — BOOT backs out).
  lv_obj_t *col = lv_obj_create(app_scr);
  // Width/height as a PERCENT of the screen so it fits any panel (was a fixed
  // 360x400 tuned for the S3's 410x502 — far wider/taller than the C6's 172x320,
  // which overflowed the screen). Spacing scales via UI_PX().
#if BOARD_SCREEN_NARROW
  // Narrow panels: the shell title sits low (below the BOOT row), so the body
  // starts at the shared UI_PX(124) used by every app, clear of the header.
  lv_obj_set_width(col, LV_PCT(92));
  lv_obj_set_height(col, (int)screenHeight - UI_PX(124) - UI_PX(8));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, UI_PX(124));
#else
  // S3-2.06: start a fixed gap below the title baseline (title at UI_PX(40),
  // ~UI_PX(28) tall).
  const int ABOUT_BODY_TOP = UI_PX(40) + UI_PX(28) + UI_PX(10);  // ~UI_PX(78)
  lv_obj_set_width(col, LV_PCT(92));
  lv_obj_set_height(col, (int)screenHeight - ABOUT_BODY_TOP - UI_PX(8));
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, ABOUT_BODY_TOP);
#endif
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, UI_PX(16), 0);
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_pad_bottom(col, UI_PX(78), 0);
#else
  lv_obj_set_style_pad_bottom(col, 28, 0);
#endif
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  ui_apply_scrollbar_nudge(col);  // narrow-panel: shift scrollbar toward the edge (no-op on S3)
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  // A touch airier: fewer text lines share the viewport per scroll frame, so each
  // frame rasterizes fewer glyphs. (Same spread-out trick as the Power list.)
  lv_obj_set_style_pad_row(col, UI_PX(24), 0);

  // ---- Identity ----
  about_line(col, &FONT_LABEL, 0xFFFFFF, DEVICE_NAME);
  about_line(col, &FONT_ABOUT_BODY, 0x888888, "v" DEVICE_VERSION "  -  built " __DATE__);

  // ---- Credits ----
  about_header(col, "AUTHOR");
  about_line(col, &FONT_ABOUT_BODY, 0xCCCCCC, "Made by:  " DEVICE_AUTHOR);

  // ---- Device (static specs) ----
  // One clustered label (\n-separated) instead of 8 separate rows, so the specs read
  // as a single tight block — like the SYSTEM/STORAGE blocks below — rather than being
  // spread apart by the column's wide pad_row.
  about_header(col, "DEVICE");
  about_line(col, &FONT_ABOUT_BODY, 0xCCCCCC,
             "Vendor:  " DEVICE_VENDOR "\n"
             "Board:   " DEVICE_BOARD "\n"
             BOARD_HW_SUMMARY);   // per-board display/touch/RTC/IMU/PMU lines

  // ---- System (live, read at open) ----
  about_header(col, "SYSTEM");
  lv_obj_t *sys = about_line(col, &FONT_ABOUT_BODY, 0xCCCCCC, "");
  // Report INTERNAL SRAM and PSRAM SEPARATELY. ESP.getFreeHeap() is the MERGED
  // internal+PSRAM total (the Arduino core adds PSRAM to the malloc heap), so it
  // can't show a buffer moving between the two pools. heap_caps_get_free_size with
  // an explicit capability is per-pool, so the framebuffer living in PSRAM vs SRAM
  // is visible here: SRAM-free jumps by ~410 KB when the framebuffer is in PSRAM.
  size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t ps_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  lv_label_set_text_fmt(sys,
      "Chip:    %s @ %u MHz\n"
      "Flash:   %u MB     PSRAM: %u MB\n"
      "SRAM:    %u KB free\n"
      "PSRAM:   %u KB free\n"
      "Radio:   %s",
      ESP.getChipModel(), (unsigned)ESP.getCpuFreqMHz(),
      (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
      (unsigned)(ESP.getPsramSize() / (1024 * 1024)),
      (unsigned)(sram_free / 1024),
      (unsigned)(ps_free / 1024),
      deviceRadioName().c_str());

  // Battery-health proxy (only meaningful once a cycle has been learned).
  if (s_health.cycle_count > 0) {
    int hpct = (int)(s_health.avg_mah / (float)BATT_DESIGN_MAH * 100.0f + 0.5f);
    lv_obj_t *bh = about_line(col, &FONT_ABOUT_BODY, 0xCCCCCC, "");
    lv_label_set_text_fmt(bh, "Battery: ~%d%% health (%u cycles)",
                          hpct, (unsigned)s_health.cycle_count);
  } else {
    about_line(col, &FONT_ABOUT_BODY, 0xCCCCCC, "Battery: health learning...");
  }

  // ---- Storage (microSD, live at open) ----
  about_header(col, "STORAGE");
  lv_obj_t *sd = about_line(col, &FONT_ABOUT_BODY, 0xCCCCCC, "");
  if (sd_present()) {
    // Card type label + sizes. cardSize() is the raw card capacity; totalBytes()/
    // usedBytes() are the FAT filesystem's view. Show used/total in GB with a %.
    const char *type;
    switch (sd_card_type()) {
      case CARD_MMC:  type = "MMC";     break;
      case CARD_SD:   type = "SDSC";    break;
      case CARD_SDHC: type = "SDHC/XC"; break;
      default:        type = "SD";      break;
    }
    uint64_t cap   = sd_card_size_bytes();   // raw capacity (bytes)
    uint64_t total = sd_total_bytes();       // filesystem size (bytes)
    uint64_t used  = sd_used_bytes();        // filesystem used (bytes)
    // Format as GB (decimal, like card labels: 1 GB = 1e9 bytes) with one decimal.
    // Integer math (x10 then split) avoids pulling in float formatting.
    uint32_t cap_g10 = (uint32_t)((cap   * 10ULL) / 1000000000ULL);
    uint32_t tot_g10 = (uint32_t)((total * 10ULL) / 1000000000ULL);
    uint32_t use_g10 = (uint32_t)((used  * 10ULL) / 1000000000ULL);
    uint32_t pct = total ? (uint32_t)((used * 100ULL) / total) : 0;
    lv_label_set_text_fmt(sd,
        "Card:    %s  %u.%u GB\n"
        "Used:    %u.%u / %u.%u GB  (%u%%)",
        type, (unsigned)(cap_g10 / 10), (unsigned)(cap_g10 % 10),
        (unsigned)(use_g10 / 10), (unsigned)(use_g10 % 10),
        (unsigned)(tot_g10 / 10), (unsigned)(tot_g10 % 10),
        (unsigned)pct);
  } else {
    lv_label_set_text(sd, "Card:    none detected");
  }

  // ---- Software stack ----
  // ALL versions are fetched automatically. LVGL / Espressif core / XPowersLib
  // come from their compile-time macros (always match what was compiled). The
  // three that ship no macro (Arduino-GFX / SensorLib / Arduino_DriveBus) come
  // from lib_versions.h, generated from their library.properties by
  // tools/gen_lib_versions.py — rerun it (or hook it into the build) after a bump.
  about_header(col, "SOFTWARE");
  lv_obj_t *sw = about_line(col, &FONT_ABOUT_BODY, 0xCCCCCC, "");
  // Core libraries are on every build; the PMU (XPowersLib) and FT3168 touch
  // (Arduino_DriveBus) libraries are only compiled in on the boards that have
  // that hardware, so their lines are board-conditional.
  char vb[256];
  int n = snprintf(vb, sizeof(vb),
      "LVGL:             %d.%d.%d\n"
      "Espressif ESP32:  %d.%d.%d\n"
      "Arduino-GFX:      %s\n"
      "SensorLib:        %s",
      LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
      ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH,
      LIBVER_GFX, LIBVER_SENSORLIB);
#if BOARD_HAS_PMU_AXP2101
  n += snprintf(vb + n, sizeof(vb) - n, "\nXPowersLib:       %d.%d.%d",
      XPOWERSLIB_VERSION_MAJOR, XPOWERSLIB_VERSION_MINOR, XPOWERSLIB_VERSION_PATCH);
#endif
#if BOARD_TOUCH_FT3168
  n += snprintf(vb + n, sizeof(vb) - n, "\nArduino_DriveBus: %s", LIBVER_DRIVEBUS);
#endif
  lv_label_set_text(sw, vb);
}
