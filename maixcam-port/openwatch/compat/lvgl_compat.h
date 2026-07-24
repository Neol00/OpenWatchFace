/* ============================================================================
 *  lvgl_compat.h — shims for LVGL API differences between the firmware's LVGL
 *  (9.5, with local patches) and MaixCDK's bundled LVGL. Included right after
 *  <lvgl.h> in the .ino on the Maix build. Add shims here as the build surfaces
 *  more API gaps.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_MAIX
#include "lvgl.h"

/* lv_label_set_recolor() was removed in this LVGL build. Provide a no-op so the
 * firmware's single recolor call compiles; that label just loses inline #color#
 * markup (cosmetic). */
static inline void lv_label_set_recolor(lv_obj_t *obj, bool en) { (void)obj; (void)en; }

#endif /* BOARD_PLATFORM_MAIX */
