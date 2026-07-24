/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --bpp 4 --size 14 --no-compress --stride 1 --align 1 --font materialdesignicons-webfont.ttf --range 983414,984468 --format lvgl -o icons14.c
 ******************************************************************************/

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif



#ifndef ICONS14
#define ICONS14 1
#endif

#if ICONS14

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F0176 "󰅶" */
    0x3, 0x44, 0x44, 0x44, 0x44, 0x40, 0xc, 0xff,
    0xff, 0xff, 0xfe, 0xe9, 0xc, 0xff, 0xff, 0xff,
    0xf6, 0x6b, 0xc, 0xff, 0xff, 0xff, 0xf9, 0xab,
    0xc, 0xff, 0xff, 0xff, 0xfe, 0xc4, 0xc, 0xff,
    0xff, 0xff, 0xf6, 0x0, 0xb, 0xff, 0xff, 0xff,
    0xf6, 0x0, 0x8, 0xff, 0xff, 0xff, 0xf2, 0x0,
    0x0, 0x9d, 0xee, 0xec, 0x50, 0x0, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x10, 0xef, 0xff, 0xff, 0xff,
    0xff, 0x90,

    /* U+F0594 "󰖔" */
    0x0, 0x0, 0x0, 0x3, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x95, 0x0, 0x0, 0x0, 0x1d, 0x92,
    0xdf, 0xfa, 0x0, 0x0, 0xc, 0xeb, 0x4, 0xff,
    0x0, 0x0, 0x5, 0xd5, 0xd0, 0x52, 0x43, 0x0,
    0x0, 0xa7, 0x1f, 0x20, 0x0, 0x6, 0x0, 0xc,
    0x50, 0x9b, 0x0, 0x4, 0xfb, 0x0, 0xb7, 0x1,
    0xd8, 0x0, 0x7, 0x60, 0x8, 0xc0, 0x1, 0xdc,
    0x40, 0x0, 0x0, 0x2f, 0x50, 0x0, 0x6c, 0xff,
    0x90, 0x0, 0x6f, 0x70, 0x0, 0x7, 0xf7, 0x0,
    0x0, 0x5f, 0xfc, 0xcf, 0xf5, 0x0, 0x0, 0x0,
    0x5, 0x88, 0x50, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 224, .box_w = 12, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 66, .adv_w = 224, .box_w = 13, .box_h = 13, .ofs_x = 0, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x41e
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 983414, .range_length = 1055, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 2, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif

};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t icons14 = {
#else
lv_font_t icons14 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 13,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .static_bitmap = 0,
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if ICONS14*/
