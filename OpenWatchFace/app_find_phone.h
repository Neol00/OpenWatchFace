/* ============================================================================
 *  app_find_phone.h — "Find My Phone" sub-app: ring the paired phone.
 *
 *  Header-only module compiled into the .ino TU, sharing the menu statics.
 *  INCLUDE AFTER app_menu.h (uses app_screen_begin / app_scr) and after
 *  ble_provision.h (uses ble_ping_phone / ble_phone_connected) and
 *  settings_store.h (ui_accent_hex) and the FONT_* macros.
 *
 *  A single big round button. Tapping it calls ble_ping_phone(), which notifies
 *  the connected companion app so the phone sounds a continuous alarm. The status
 *  line reflects the result; if no phone is connected it says so instead.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

static lv_obj_t *fmp_status = nullptr;   // status label (loop/LVGL thread only)
static lv_obj_t *fmp_btn_lbl = nullptr;  // "Ring"/"Stop" caption (reassigned each open)
static bool      fmp_ringing = false;    // we asked the phone to ring; next tap stops

/* Clear our pointer if the label is torn down with the screen, so a later rebuild
 * never writes through a freed object. */
static void fmp_status_deleted_cb(lv_event_t *e) {
  if (lv_event_get_target(e) == fmp_status) fmp_status = nullptr;
}

static void fmp_set_status(const char *msg, uint32_t color) {
  if (!fmp_status) return;
  lv_label_set_text(fmp_status, msg);
  lv_obj_set_style_text_color(fmp_status, lv_color_hex(color), 0);
}

/* Ring button: ping the phone (no-op + hint if nothing is connected). Gadgetbridge
 * rings until dismissed on the phone or told to stop, so the button toggles:
 * first tap rings, second tap stops. */
static void fmp_ring_cb(lv_event_t *e) {
  (void)e;
  if (fmp_ringing) {
    ble_stop_phone_ring();
    fmp_ringing = false;
    if (fmp_btn_lbl) lv_label_set_text(fmp_btn_lbl, "Ring");
    fmp_set_status("Tap to ring your phone.", 0xAAAAAA);
    return;
  }
  if (ble_ping_phone()) {
    fmp_ringing = true;
    if (fmp_btn_lbl) lv_label_set_text(fmp_btn_lbl, "Stop");
    fmp_set_status("Ringing your phone...\nTap again to stop.", ui_accent_hex());
  } else {
    fmp_set_status("No phone connected.\nOpen the companion app first.", 0xFF9F0A);
  }
}

static void app_open_find_phone(void) {
  app_screen_begin("Find Phone");

  // Big round "Ring" button, accent-filled (matches the watch's accent system).
  // Diameter is screen-relative so it never overflows a narrow panel (the old
  // fixed 180px was wider than the C6's 172px screen and clipped off both sides).
  // Cap at 180 so the S3 keeps its original look; on the C6 it shrinks to fit.
  int ring_d = (int)screenWidth - UI_PX(40);   // leave a margin both sides
  if (ring_d > 180) ring_d = 180;
  lv_obj_t *btn = lv_btn_create(app_scr);
  lv_obj_set_size(btn, ring_d, ring_d);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, UI_PX(-24));
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(ui_accent_hex()), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(btn, fmp_ring_cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *ic = lv_label_create(btn);
  // The icon glyph is a FIXED-size built-in font (not UI_PX-scaled), but the
  // icon/label offsets ARE UI_PX-scaled. On a narrow panel those offsets shrink
  // while a montserrat_34 glyph stays ~34 px tall -> icon and "Ring" collide.
  // Use a smaller glyph + wider fixed gaps on narrow so they stay separated.
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
#else
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_34, 0);
#endif
  lv_obj_set_style_text_color(ic, lv_color_black(), 0);
  lv_label_set_text(ic, LV_SYMBOL_CALL);
#if BOARD_SCREEN_NARROW
  lv_obj_align(ic, LV_ALIGN_CENTER, 0, -18);
#else
  lv_obj_align(ic, LV_ALIGN_CENTER, 0, UI_PX(-16));
#endif

  fmp_btn_lbl = lv_label_create(btn);
  lv_obj_set_style_text_font(fmp_btn_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(fmp_btn_lbl, lv_color_black(), 0);
  lv_label_set_text(fmp_btn_lbl, "Ring");
#if BOARD_SCREEN_NARROW
  lv_obj_align(fmp_btn_lbl, LV_ALIGN_CENTER, 0, 16);
#else
  lv_obj_align(fmp_btn_lbl, LV_ALIGN_CENTER, 0, UI_PX(28));
#endif
  fmp_ringing = false;   // fresh screen -> fresh toggle state (the phone self-stops
                         // when dismissed there; we just reset our side)

  // Status line below the button.
  fmp_status = lv_label_create(app_scr);
  lv_obj_add_event_cb(fmp_status, fmp_status_deleted_cb, LV_EVENT_DELETE, nullptr);
  lv_obj_set_style_text_font(fmp_status, &FONT_SMALL, 0);
  lv_obj_set_style_text_align(fmp_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(fmp_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(fmp_status, LV_PCT(90));   // percent so it fits any panel width
  // Anchor the status line a fixed gap BELOW the button's actual bottom edge,
  // not a guessed offset from screen-center: the button center is at -24 and its
  // radius shrinks on narrow panels, so a center-relative offset could land the
  // text on top of the button. ALIGN_OUT_BOTTOM_MID keeps it clear on any panel.
  lv_obj_align_to(fmp_status, btn, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(24));

  if (ble_phone_connected())
    fmp_set_status("Tap to ring your phone.", 0xAAAAAA);
  else
    fmp_set_status("No phone connected.\nOpen the companion app first.", 0xFF9F0A);
}
