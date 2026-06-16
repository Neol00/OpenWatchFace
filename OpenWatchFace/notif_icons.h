/* ============================================================================
 *  notif_icons.h — per-category notification icons, backed by a Material Design
 *  Icons LVGL font.
 *
 *  ADDING / RETUNING ICONS: this header is the SINGLE place icons live. To change a
 *  category's glyph, edit its codepoint in NOTIF_CAT_CP[] below (look the icon up on
 *  pictogrammers.com/library/mdi — each shows its 0xF.... codepoint). To add a whole
 *  new category, append it to NotifCat (notif_store.h) + add its row here + a color.
 *
 *  INCLUDE AFTER notif_store.h (NotifCat) and lvgl.h. Header-only, in the .ino TU.
 * ========================================================================== */
#pragma once
#include <lvgl.h>

/* Flip to 1 to use icons22.c is in the sketch folder. 0 = use built-in symbols. */
#ifndef NOTIF_ICONS_HAVE_MDI
#define NOTIF_ICONS_HAVE_MDI 1
#endif

#if NOTIF_ICONS_HAVE_MDI
LV_FONT_DECLARE(icons22);                 // the converted MDI font (icons22.c)
#define NOTIF_ICON_FONT (&icons22)
#else
#define NOTIF_ICON_FONT (&lv_font_montserrat_20)   // fallback: built-in symbol font
#endif

/* ---- per-category MDI codepoints ----
 * Indexed by NotifCat. Verify/adjust each against your MDI version on
 * pictogrammers.com/library/mdi (codepoints shift between MDI releases). Only used
 * when NOTIF_ICONS_HAVE_MDI==1; otherwise NOTIF_CAT_SYMBOL[] (below) is used. */
#if NOTIF_ICONS_HAVE_MDI
static const uint32_t NOTIF_CAT_CP[NCAT_COUNT] = {
  /* GENERIC  */ 0xF009A,   // bell
  /* CALL     */ 0xF03F2,   // phone
  /* MESSAGE  */ 0xF0361,   // message-text
  /* MAIL     */ 0xF01EE,   // email
  /* CALENDAR */ 0xF00ED,   // calendar
  /* REMINDER */ 0xF0139,   // checkbox-marked-circle (to-do)
  /* SOCIAL   */ 0xF0849,   // account-group
  /* MUSIC    */ 0xF075A,   // music
  /* VIDEO    */ 0xF0567,   // video
  /* PHOTO    */ 0xF02E9,   // image
  /* MAPS     */ 0xF034E,   // map-marker
  /* FINANCE  */ 0xF0070,   // bank
  /* SHOPPING */ 0xF0110,   // cart
  /* NEWS     */ 0xF0395,   // newspaper
  /* HEALTH   */ 0xF05F6,   // heart-pulse
  /* WEATHER  */ 0xF0590,   // weather-partly-cloudy
  /* GAME     */ 0xF0297,   // gamepad-variant
  /* SYSTEM   */ 0xF0493,   // cog
  /* SECURITY */ 0xF0CE5,   // shield-alert
  /* POWER    */ 0xF0241,   // lightning
  /* SAVE     */ 0xF0818,   // floppydrive
  /* TRASH    */ 0xF0A7A,   // trashcan
  /* TEMP     */ 0xF10C2,   // temp gauge
  /* IOS      */ 0xF0037,   // IOS logo
  /* BARCODE  */ 0xF0071,   // barcode
  /* PHONE    */ 0xF0952,   // smartphone
  /* PRINTER  */ 0xF1146,   // printer
  /* ERROR    */ 0xF11CE,   // error
  /* USB      */ 0xF129E,   // usbdrive
  /* CABLE    */ 0xF1394,   // cable
  /* VIRUS    */ 0xF13B6,   // virus
  /* CAR      */ 0xF010B,   // car
  /* FOOD     */ 0xF05D7,   // food / silverware-fork-knife
  /* COFFEE   */ 0xF0176,   // coffee
  /* FLIGHT   */ 0xF001D,   // airplane
  /* TRANSIT  */ 0xF052C,   // train
  /* PACKAGE  */ 0xF03D8,   // package-variant
  /* CLOUD    */ 0xF015F,   // cloud
  /* DOWNLOAD */ 0xF01DA,   // download
  /* BOOK     */ 0xF00BB,   // book-open-page-variant
  /* HOME     */ 0xF02DC,   // home
  /* BELL     */ 0xF1209,   // doorbell
  /* LOCK     */ 0xF033E,   // lock
  /* LIGHT    */ 0xF0335,   // lightbulb
  /* WIFI     */ 0xF05A9,   // wifi
  /* BLUETOOTH*/ 0xF00AF,   // bluetooth
  /* LOCATION */ 0xF034E,   // map-marker
  /* ALARM    */ 0xF0020,   // alarm
  /* NOTE     */ 0xF039B,   // note-text
  /* CRYPTO   */ 0xF0813,   // bitcoin
  /* SLEEP    */ 0xF04B2,   // sleep
  /* RUN      */ 0xF050E,   // run
  /* PET      */ 0xF011A,   // paw (cat)
  /* BABY     */ 0xF0E0D,   // baby-carriage
  /* GIFT     */ 0xF02A6,   // gift
  /* TICKET   */ 0xF0507,   // ticket
  /* UMBRELLA */ 0xF0560,   // umbrella
  /* WHATSAPP */ 0xF05A3,   // whatsapp
  /* MESSENGER*/ 0xF0364,   // facebook-messenger
  /* TELEGRAM */ 0xF052D,   // telegram
  /* SIGNAL   */ 0xF1098,   // message-lock (signal has no dedicated MDI glyph)
  /* DISCORD  */ 0xF066F,   // discord
  /* SLACK    */ 0xF04F4,   // slack
  /* INSTAGRAM*/ 0xF02FE,   // instagram
  /* FACEBOOK */ 0xF020C,   // facebook
  /* TWITTER  */ 0xF0544,   // twitter (X has no MDI glyph; using bird)
  /* SNAPCHAT */ 0xF04ED,   // snapchat
  /* TIKTOK   */ 0xF1337,   // music (tiktok deprecated; music note fallback)
  /* REDDIT   */ 0xF0455,   // reddit
  /* LINKEDIN */ 0xF033B,   // linkedin
  /* YOUTUBE  */ 0xF05C3,   // youtube
  /* SPOTIFY  */ 0xF04F8,   // spotify
  /* NETFLIX  */ 0xF0746,   // netflix
  /* GMAIL    */ 0xF02AB,   // gmail
  /* OUTLOOK  */ 0xF0D22,   // microsoft-outlook
  /* PAYPAL   */ 0xF0431,   // paypal
  /* UBER     */ 0xF0553,   // uber

};
#else
/* Fallback glyphs (built-in Montserrat symbols) used when NOTIF_ICONS_HAVE_MDI==0.
 * Coarser — several share an icon — but keeps the watch fully working pre-font. */
static const char *NOTIF_CAT_SYMBOL[NCAT_COUNT] = {
  /* GENERIC  */ LV_SYMBOL_BELL,
  /* CALL     */ LV_SYMBOL_CALL,
  /* MESSAGE  */ LV_SYMBOL_ENVELOPE,
  /* MAIL     */ LV_SYMBOL_ENVELOPE,
  /* CALENDAR */ LV_SYMBOL_BELL,
  /* REMINDER */ LV_SYMBOL_BELL,
  /* SOCIAL   */ LV_SYMBOL_BELL,
  /* MUSIC    */ LV_SYMBOL_AUDIO,
  /* VIDEO    */ LV_SYMBOL_VIDEO,
  /* PHOTO    */ LV_SYMBOL_IMAGE,
  /* MAPS     */ LV_SYMBOL_GPS,
  /* FINANCE  */ LV_SYMBOL_BELL,
  /* SHOPPING */ LV_SYMBOL_BELL,
  /* NEWS     */ LV_SYMBOL_LIST,
  /* HEALTH   */ LV_SYMBOL_BELL,
  /* WEATHER  */ LV_SYMBOL_BELL,
  /* GAME     */ LV_SYMBOL_BELL,
  /* SYSTEM   */ LV_SYMBOL_SETTINGS,
  /* SECURITY */ LV_SYMBOL_WARNING,
  /* POWER    */ LV_SYMBOL_BELL,
  /* SAVE     */ LV_SYMBOL_BELL,
  /* TRASH    */ LV_SYMBOL_BELL,
  /* TEMP     */ LV_SYMBOL_BELL,
  /* IOS      */ LV_SYMBOL_BELL,
  /* BARCODE  */ LV_SYMBOL_BELL,
  /* PHONE    */ LV_SYMBOL_BELL,
  /* PRINTER  */ LV_SYMBOL_BELL,
  /* ERROR    */ LV_SYMBOL_ERROR,
  /* USB      */ LV_SYMBOL_BELL,
  /* CABLE    */ LV_SYMBOL_BELL,
  /* VIRUS    */ LV_SYMBOL_WARNING,
  /* CAR      */ LV_SYMBOL_BELL,
  /* FOOD     */ LV_SYMBOL_BELL,
  /* COFFEE   */ LV_SYMBOL_BELL,
  /* FLIGHT   */ LV_SYMBOL_BELL,
  /* TRANSIT  */ LV_SYMBOL_BELL,
  /* PACKAGE  */ LV_SYMBOL_BELL,
  /* CLOUD    */ LV_SYMBOL_UPLOAD,
  /* DOWNLOAD */ LV_SYMBOL_DOWNLOAD,
  /* BOOK     */ LV_SYMBOL_LIST,
  /* HOME     */ LV_SYMBOL_HOME,
  /* BELL     */ LV_SYMBOL_BELL,
  /* LOCK     */ LV_SYMBOL_BELL,
  /* LIGHT    */ LV_SYMBOL_BELL,
  /* WIFI     */ LV_SYMBOL_WIFI,
  /* BLUETOOTH*/ LV_SYMBOL_BLUETOOTH,
  /* LOCATION */ LV_SYMBOL_GPS,
  /* ALARM    */ LV_SYMBOL_BELL,
  /* NOTE     */ LV_SYMBOL_EDIT,
  /* CRYPTO   */ LV_SYMBOL_BELL,
  /* SLEEP    */ LV_SYMBOL_BELL,
  /* RUN      */ LV_SYMBOL_BELL,
  /* PET      */ LV_SYMBOL_BELL,
  /* BABY     */ LV_SYMBOL_BELL,
  /* GIFT     */ LV_SYMBOL_BELL,
  /* TICKET   */ LV_SYMBOL_BELL,
  /* UMBRELLA */ LV_SYMBOL_BELL,
  /* WHATSAPP */ LV_SYMBOL_ENVELOPE,
  /* MESSENGER*/ LV_SYMBOL_ENVELOPE,
  /* TELEGRAM */ LV_SYMBOL_ENVELOPE,
  /* SIGNAL   */ LV_SYMBOL_ENVELOPE,
  /* DISCORD  */ LV_SYMBOL_ENVELOPE,
  /* SLACK    */ LV_SYMBOL_ENVELOPE,
  /* INSTAGRAM*/ LV_SYMBOL_IMAGE,
  /* FACEBOOK */ LV_SYMBOL_BELL,
  /* TWITTER  */ LV_SYMBOL_BELL,
  /* SNAPCHAT */ LV_SYMBOL_IMAGE,
  /* TIKTOK   */ LV_SYMBOL_VIDEO,
  /* REDDIT   */ LV_SYMBOL_BELL,
  /* LINKEDIN */ LV_SYMBOL_BELL,
  /* YOUTUBE  */ LV_SYMBOL_VIDEO,
  /* SPOTIFY  */ LV_SYMBOL_AUDIO,
  /* NETFLIX  */ LV_SYMBOL_VIDEO,
  /* GMAIL    */ LV_SYMBOL_ENVELOPE,
  /* OUTLOOK  */ LV_SYMBOL_ENVELOPE,
  /* PAYPAL   */ LV_SYMBOL_BELL,
  /* UBER     */ LV_SYMBOL_GPS,

};
#endif  // NOTIF_ICONS_HAVE_MDI

/* Per-category tint. Recognisable hues; not the user accent (so the unread accent
 * dot stays distinct). */
static const uint32_t NOTIF_CAT_RGB[NCAT_COUNT] = {
  /* GENERIC  */ 0x9090A0,
  /* CALL     */ 0x32D74B,   // green
  /* MESSAGE  */ 0x00B0FF,   // blue
  /* MAIL     */ 0x0A84FF,   // deep blue
  /* CALENDAR */ 0xFF9F0A,   // amber
  /* REMINDER */ 0xFFD60A,   // yellow
  /* SOCIAL   */ 0x5E5CE6,   // indigo
  /* MUSIC    */ 0xFF375F,   // pink-red
  /* VIDEO    */ 0xFF453A,   // red
  /* PHOTO    */ 0x64D2FF,   // cyan
  /* MAPS     */ 0x30D158,   // map green
  /* FINANCE  */ 0x30D158,   // money green
  /* SHOPPING */ 0xFF9F0A,   // amber
  /* NEWS     */ 0xAEAEB2,   // grey
  /* HEALTH   */ 0xFF2D55,   // health red
  /* WEATHER  */ 0x64D2FF,   // sky
  /* GAME     */ 0xBF5AF2,   // purple
  /* SYSTEM   */ 0x8E8E93,   // system grey
  /* SECURITY */ 0xFF9F0A,   // alert amber
  /* POWER    */ 0x00FF00,   // light green
  /* SAVE     */ 0x00B0FF,   // blue
  /* TRASH    */ 0xAEAEB2,   // grey
  /* TEMP     */ 0xE6004C,   // bright red
  /* IOS      */ 0xE6FFFF,   // white tint blue
  /* BARCODE  */ 0x8E8E93,   // system grey
  /* PHONE    */ 0x80BFFF,   // bright blue
  /* PRINTER  */ 0xEA80FF,   // Heliotrope
  /* ERROR    */ 0xFF1919,   // red
  /* USB      */ 0xFFEA80,   // jasmine
  /* CABLE    */ 0x8066FF,   // slate blue
  /* VIRUS    */ 0x8CFF19,   // chartreuse
  /* CAR      */ 0xFFD500,   // gold
  /* FOOD     */ 0xFF7A45,   // warm orange
  /* COFFEE   */ 0x8B5A2B,   // coffee brown
  /* FLIGHT   */ 0x4DA6FF,   // sky blue
  /* TRANSIT  */ 0x00C2A8,   // teal
  /* PACKAGE  */ 0xC58B4D,   // cardboard tan
  /* CLOUD    */ 0xB0C4DE,   // light steel blue
  /* DOWNLOAD */ 0x4CD964,   // green
  /* BOOK     */ 0xD9A066,   // paper tan
  /* HOME     */ 0x5AC8FA,   // home blue
  /* BELL     */ 0xFFCC00,   // doorbell yellow
  /* LOCK     */ 0xA0A0B0,   // steel grey
  /* LIGHT    */ 0xFFE680,   // warm bulb yellow
  /* WIFI     */ 0x34C759,   // connected green
  /* BLUETOOTH*/ 0x0A84FF,   // bluetooth blue
  /* LOCATION */ 0xFF453A,   // pin red
  /* ALARM    */ 0xFF9F0A,   // amber
  /* NOTE     */ 0xFFE066,   // note yellow
  /* CRYPTO   */ 0xF7931A,   // bitcoin orange
  /* SLEEP    */ 0x5E5CE6,   // indigo
  /* RUN      */ 0x30D158,   // active green
  /* PET      */ 0xC78F5C,   // tan
  /* BABY     */ 0xFFB6C1,   // soft pink
  /* GIFT     */ 0xFF2D55,   // gift red
  /* TICKET   */ 0xBF5AF2,   // event purple
  /* UMBRELLA */ 0x64D2FF,   // rain blue
  /* WHATSAPP */ 0x25D366,   // whatsapp green
  /* MESSENGER*/ 0x0084FF,   // messenger blue
  /* TELEGRAM */ 0x26A5E4,   // telegram blue
  /* SIGNAL   */ 0x3A76F0,   // signal blue
  /* DISCORD  */ 0x5865F2,   // blurple
  /* SLACK    */ 0x4A154B,   // slack aubergine
  /* INSTAGRAM*/ 0xE1306C,   // instagram magenta
  /* FACEBOOK */ 0x1877F2,   // facebook blue
  /* TWITTER  */ 0x1DA1F2,   // twitter blue (X is black; blue reads better)
  /* SNAPCHAT */ 0xFFFC00,   // snapchat yellow
  /* TIKTOK   */ 0xFF0050,   // tiktok red-pink
  /* REDDIT   */ 0xFF4500,   // reddit orange
  /* LINKEDIN */ 0x0A66C2,   // linkedin blue
  /* YOUTUBE  */ 0xFF0000,   // youtube red
  /* SPOTIFY  */ 0x1DB954,   // spotify green
  /* NETFLIX  */ 0xE50914,   // netflix red
  /* GMAIL    */ 0xEA4335,   // gmail red
  /* OUTLOOK  */ 0x0078D4,   // outlook blue
  /* PAYPAL   */ 0x003087,   // paypal blue
  /* UBER     */ 0xE0E0E0,   // uber is black; use near-white so it's visible on AMOLED

};

#if NOTIF_ICONS_HAVE_MDI
/* Encode a Unicode codepoint as UTF-8 into `out` (>=5 bytes), NUL-terminated.
 * MDI codepoints sit in the SMP/PUA-A range (0xF0000+), needing 4 UTF-8 bytes.
 * Only compiled when the MDI font is present (the fallback path uses ASCII symbols). */
static const char *notif_icon_utf8(uint32_t cp, char out[5]) {
  if (cp < 0x80) {
    out[0] = (char)cp; out[1] = '\0';
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    out[2] = '\0';
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
  } else {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = '\0';
  }
  return out;
}
#endif  // NOTIF_ICONS_HAVE_MDI

/* Set `label` to category `cat`'s icon + tint, using the MDI font when available
 * (else the built-in symbol). `cat` is clamped to a valid category. The font is set
 * on the label so a single list can mix the icon font (rows) with text fonts. */
static void notif_icon_apply(lv_obj_t *label, uint8_t cat) {
  if (cat >= NCAT_COUNT) cat = NCAT_GENERIC;
  // Decorative tint: collapses to the accent in monochrome-accent mode (the plain
  // look), else uses the category's distinct color.
  lv_obj_set_style_text_color(label, lv_color_hex(ui_deco_hex(NOTIF_CAT_RGB[cat])), 0);
#if NOTIF_ICONS_HAVE_MDI
  lv_obj_set_style_text_font(label, NOTIF_ICON_FONT, 0);
  char u[5];
  lv_label_set_text(label, notif_icon_utf8(NOTIF_CAT_CP[cat], u));
#else
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_label_set_text(label, NOTIF_CAT_SYMBOL[cat]);
#endif
}

/* The category's color, for callers that draw the icon themselves (e.g. the big
 * call glyph in the reader). */
static uint32_t notif_icon_color(uint8_t cat) {
  if (cat >= NCAT_COUNT) cat = NCAT_GENERIC;
  return ui_deco_hex(NOTIF_CAT_RGB[cat]);   // accent in mono mode, else the category color
}
