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
#if BOARD_SCREEN_ROUND_SMALL
  // SMALL ROUND face: the 180 px cap is already the binding one here (360-UI_PX(40)
  // = 325, capped to 180), so the button was never oversized for the WIDTH — it was
  // mispositioned for the HEIGHT. Centered at UI_PX(-24) = -21 it spanned y 69..249,
  // and the app title sits at UI_PX(40) = 35 and ends at ~69: they met exactly, which
  // is the reported "Ring button clips with the Find Phone title".
  // Trim to 160 and drop the center offset to UI_PX(-16) = -14 -> y 86..246, a clear
  // 17 px below the title, with room gained back for the status line underneath.
  if (ring_d > 160) ring_d = 160;
#endif
  bool connected = ble_phone_connected();      // no phone -> button greyed + inert
  lv_obj_t *btn = lv_btn_create(app_scr);
  lv_obj_set_size(btn, ring_d, ring_d);
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, UI_PX(-16));
#else
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, UI_PX(-24));
#endif
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  // Accent-filled when a phone is connected; greyed out (disabled) otherwise so it
  // visibly reads as un-pressable, matching the "No phone connected" status below.
  lv_obj_set_style_bg_color(btn, lv_color_hex(connected ? ui_accent_hex() : 0x3A3A3A), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  if (connected) {
    lv_obj_add_event_cb(btn, fmp_ring_cb, LV_EVENT_CLICKED, nullptr);
  } else {
    lv_obj_add_state(btn, LV_STATE_DISABLED);   // ignores presses; no ring with no phone
  }

  lv_obj_t *ic = lv_label_create(btn);
  // The icon glyph is a FIXED-size built-in font (not UI_PX-scaled), but the
  // icon/label offsets ARE UI_PX-scaled. On a narrow panel those offsets shrink
  // while a montserrat_34 glyph stays ~34 px tall -> icon and "Ring" collide.
  // Use a smaller glyph + wider fixed gaps on narrow so they stay separated.
#if BOARD_SCREEN_NARROW
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
#else
  lv_obj_set_style_text_font(ic, &UI_FONT(34), 0);
#endif
  lv_obj_set_style_text_color(ic, connected ? lv_color_black() : lv_color_hex(0x777777), 0);
  lv_label_set_text(ic, LV_SYMBOL_CALL);
#if BOARD_SCREEN_NARROW
  lv_obj_align(ic, LV_ALIGN_CENTER, 0, -18);
#else
  lv_obj_align(ic, LV_ALIGN_CENTER, 0, UI_PX(-16));
#endif

  fmp_btn_lbl = lv_label_create(btn);
  lv_obj_set_style_text_font(fmp_btn_lbl, &FONT_SMALL, 0);
  lv_obj_set_style_text_color(fmp_btn_lbl, connected ? lv_color_black() : lv_color_hex(0x777777), 0);
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
#if BOARD_SCREEN_ROUND_SMALL
  // The status line is the yellow text reported as clipping off both sides. LV_PCT(90)
  // is 324 px on the C2, but this label sits LOW — around y 262..312 — and the glass
  // there is a chord, not the full panel: at y=287 the circle is only ~289 px wide and
  // at y=312 only ~245. A percentage of the PANEL width is the wrong measure on a round
  // face; LV_PCT(66) = 238 px stays inside the arc even if the text wraps to a second
  // line, and still fits "Tap to ring your phone." (~205 px at 20 px) on one line.
  lv_obj_set_width(fmp_status, LV_PCT(66));
#else
  lv_obj_set_width(fmp_status, LV_PCT(90));   // percent so it fits any panel width
#endif
  // Anchor the status line a fixed gap BELOW the button's actual bottom edge,
  // not a guessed offset from screen-center: the button center is at -24 and its
  // radius shrinks on narrow panels, so a center-relative offset could land the
  // text on top of the button. ALIGN_OUT_BOTTOM_MID keeps it clear on any panel.
#if BOARD_SCREEN_ROUND_SMALL
  lv_obj_align_to(fmp_status, btn, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(18));
#else
  lv_obj_align_to(fmp_status, btn, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_PX(24));
#endif

  if (ble_phone_connected())
    fmp_set_status("Tap to ring your phone.", 0xAAAAAA);
  else
    fmp_set_status("No phone connected.\nOpen the companion app first.", 0xFF9F0A);
}
