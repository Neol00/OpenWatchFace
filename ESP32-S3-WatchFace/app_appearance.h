/* ============================================================================
 *  app_appearance.h — Appearance / theme settings (currently: accent color).
 *
 *  A scrollable settings screen (same shell + look as About/Power) holding a row
 *  of preset accent swatches. Tapping one applies the accent everywhere instantly:
 *  it persists to NVS, restyles the always-resident pull-down shade live, and
 *  recolors this screen's own header — every other screen picks the new accent up
 *  the next time it's opened (they rebuild on open).
 *
 *  Deliberately built as an extensible column: drop more sections below the
 *  swatches later (e.g. dark/light, font size, watch-face style) and they slot
 *  straight in.
 *
 *  Header-only; compiled into the .ino TU. INCLUDE AFTER app_menu.h (screen
 *  shell), settings_store.h (settings_get/set_accent + ui_accent_* helpers) and
 *  quick_shade.h (quick_shade_restyle). app_open_appearance is forward-declared
 *  in app_menu.h so app_settings.h can dispatch to it regardless of order.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

/* Preset accent palette. Magenta is intentionally Heliotrope (#DF73FF), a soft
 * violet, rather than a harsh magenta. Add/replace freely — the UI adapts. */
static const uint32_t APPR_COLORS[] = {
  0x00B0FF,   // Blue   (default)
  0x00C2A8,   // Teal
  0x32D74B,   // Green
  0xFF9F0A,   // Amber
  0xDF73FF,   // Heliotrope
  0xFF453A,   // Red
};
static const uint8_t APPR_COLOR_COUNT = sizeof(APPR_COLORS) / sizeof(APPR_COLORS[0]);

static lv_obj_t *appr_swatches[8];        // the swatch buttons (<= APPR_COLOR_COUNT)
static lv_obj_t *appr_hdr = nullptr;      // the "ACCENT COLOR" header (recolored live)
static lv_obj_t *appr_wf_hdr = nullptr;   // the "WATCH FACE" header (recolored live)
static lv_obj_t *appr_volt_sw = nullptr;  // the voltage-readout switch (accent indicator, recolored live)

/* Monochrome-accent toggle: when ON, every decorative color in the UI collapses to
 * the accent for a plain, uniform look (category icons, menu tile icons, graph/second
 * series, the WiFi/BLE indicators, the green pager, etc.). settings_set_mono_accent
 * persists it; we then restyle the live shade and rebuild THIS screen so the swatches
 * and headers reflect the change without leaving Appearance. Other screens rebuild on
 * open. Forward-declared so the callback (above app_open_appearance) can rebuild. */
static void app_open_appearance(void);
/* Deferred rebuild: deleting app_scr while the switch's VALUE_CHANGED event is still
 * dispatching is LVGL UB (same trap noted in app_notifications.h). lv_async_call runs
 * it after the event unwinds. */
static void appr_rebuild_async(void *p) {
  (void)p;
  if (app_scr) { lv_obj_del(app_scr); app_scr = nullptr; }
  app_open_appearance();
}
static void appr_mono_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  settings_set_mono_accent(lv_obj_has_state(sw, LV_STATE_CHECKED));
  // Recolor every always-resident surface live, so the change shows immediately
  // instead of only after a reboot/wake (these are built once, not rebuilt on open):
  quick_shade_restyle();                 // the pull-down shade
  menu_restyle();                        // the app-menu tile icons
  watchface_apply_indicator_layout();    // the watch-face WiFi/BLE indicators
  lv_async_call(appr_rebuild_async, nullptr);   // repaint this screen after the event
}

/* Voltage-readout toggle: same labelled-switch row as WiFi/BLE (settings_toggle_row),
 * so it matches the rest of settings and — unlike the old full-width button — doesn't
 * run flush to the screen bottom where the rounded corners would clip it. */
static void appr_volt_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  settings_set_show_volt(lv_obj_has_state(sw, LV_STATE_CHECKED));  // persists + applies to the face live
}

/* Swap-WiFi/BLE toggle: when on, the WiFi glyph moves to the top-right tray and the
 * BLE indicator takes the right stat column. settings_set_swap_wifi_ble persists the
 * choice and re-lays-out the face live. */
static void appr_swap_wb_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  settings_set_swap_wifi_ble(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* Ring the selected swatch (white, thick) and give the rest a faint outline. */
static void appr_refresh_selection(void) {
  uint32_t cur = settings_get_accent();
  for (uint8_t i = 0; i < APPR_COLOR_COUNT; i++) {
    bool sel = (APPR_COLORS[i] == cur);
    lv_obj_set_style_border_color(appr_swatches[i],
        sel ? lv_color_white() : lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_border_width(appr_swatches[i], sel ? 4 : 2, 0);
  }
}

static void appr_swatch_cb(lv_event_t *e) {
  uint32_t rgb = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  settings_set_accent(rgb);               // apply + persist to NVS
  quick_shade_restyle();                  // recolor the live pull-down shade now
  menu_restyle();                         // recolor the app-menu tile icons live
  watchface_apply_indicator_layout();     // recolor the watch-face WiFi/BLE indicators
  appr_refresh_selection();               // move the selection ring
  // Recolor this screen's own accent-tinted widgets live (otherwise they keep the
  // old accent until the screen is rebuilt on next open).
  if (appr_hdr)
    lv_obj_set_style_text_color(appr_hdr, lv_color_hex(ui_accent_soft_hex()), 0);
  if (appr_wf_hdr)
    lv_obj_set_style_text_color(appr_wf_hdr, lv_color_hex(ui_accent_soft_hex()), 0);
  if (appr_volt_sw)
    lv_obj_set_style_bg_color(appr_volt_sw, lv_color_hex(ui_accent_hex()),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
}

static void app_open_appearance(void) {
  app_screen_begin("Appearance");

  // Scrollable column (matches About/Power), ready to grow more theme sections.
  lv_obj_t *col = lv_obj_create(app_scr);
  lv_obj_set_size(col, 374, 408);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 84);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(col, 22, 0);

  // ---- MONOCHROME ACCENT (toggle above the swatches, as requested) ----
  // When on, decorative colors across the UI collapse to the chosen accent for a
  // plain look. Sits directly above the accent picker since it governs how that
  // accent is applied everywhere.
  settings_toggle_row(col, LV_SYMBOL_TINT, "Monochrome",
                      settings_get_mono_accent(), appr_mono_toggle_cb);

  // ---- ACCENT COLOR ----
  appr_hdr = lv_label_create(col);
  lv_obj_set_style_text_font(appr_hdr, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(appr_hdr, lv_color_hex(ui_accent_soft_hex()), 0);
  lv_label_set_text(appr_hdr, "ACCENT COLOR");
  lv_obj_set_style_pad_top(appr_hdr, 4, 0);

  // Swatch grid: a wrapping row of circular color buttons.
  lv_obj_t *grid = lv_obj_create(col);
  lv_obj_set_width(grid, LV_PCT(100));
  lv_obj_set_height(grid, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 6, 0);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(grid, 22, 0);
  lv_obj_set_style_pad_column(grid, 16, 0);

  for (uint8_t i = 0; i < APPR_COLOR_COUNT; i++) {
    lv_obj_t *sw = lv_btn_create(grid);
    lv_obj_set_size(sw, 78, 78);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(APPR_COLORS[i]), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(sw, 0, 0);
    lv_obj_add_event_cb(sw, appr_swatch_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)APPR_COLORS[i]);
    appr_swatches[i] = sw;
  }
  appr_refresh_selection();

  // Caption (and a deliberate spot to grow into — more theme rows go below here).
  lv_obj_t *hint = lv_label_create(col);
  lv_obj_set_style_text_font(hint, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
  lv_obj_set_width(hint, LV_PCT(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_label_set_text(hint, "Tip: the accent colors headers, sliders, graphs and "
                          "the pull-down shade.");
  lv_obj_set_style_pad_top(hint, 10, 0);

  // ---- WATCH FACE ----
  appr_wf_hdr = lv_label_create(col);
  lv_obj_set_style_text_font(appr_wf_hdr, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(appr_wf_hdr, lv_color_hex(ui_accent_soft_hex()), 0);
  lv_label_set_text(appr_wf_hdr, "WATCH FACE");
  lv_obj_set_style_pad_top(appr_wf_hdr, 12, 0);

  // Toggle: show the little voltage reading in the top-right corner? Default ON.
  // Same switch row as WiFi/BLE; its toggle handler persists + applies to the face.
  appr_volt_sw = settings_toggle_row(col, LV_SYMBOL_EYE_OPEN, "Voltage reading",
                                     settings_get_show_volt(), appr_volt_toggle_cb);

  // Toggle: swap the WiFi and BLE indicators. OFF = WiFi in the right stat column,
  // BLE glyph in the top-right corner; ON = WiFi glyph in the corner, BLE in the
  // column. settings_set_swap_wifi_ble persists + re-lays-out the face live.
  settings_toggle_row(col, LV_SYMBOL_BLUETOOTH, "WiFi/BLE swap",
                      settings_get_swap_wifi_ble(), appr_swap_wb_toggle_cb);

  // Bottom spacer so the last row clears the screen's rounded corners (the scroll
  // column runs to the bottom; without this the final row clips into the curve).
  lv_obj_t *tail = lv_obj_create(col);
  lv_obj_remove_style_all(tail);
  lv_obj_set_size(tail, 1, 24);
}
