/* ============================================================================
 *  owf_tuya_dma2d_backend.cpp — Beken BK7258 backend for LVGL's DMA2D draw unit.
 *
 *  LVGL's DMA2D draw unit (src/draw/dma2d/lv_draw_dma2d.c) is STM32-specific: it pokes
 *  STM32 DMA2D registers. We point LV_DRAW_DMA2D_HAL_INCLUDE at owf_tuya_dma2d_hal.h and
 *  wrap those register pokes in `#if BOARD_PLATFORM_TUYA` so they call the bridge
 *  functions implemented here. This file translates LVGL's lv_draw_dma2d_configuration_t
 *  into the BK7258 driver API (bk_dma2d_*, in libdriver.a) and starts the transfer.
 *
 *  Geometry note: LVGL expresses inter-line gaps as `*_offset = stride_pixels - w`, which
 *  is EXACTLY the Beken DMA2D "line offset" concept (init.output_offset / layer
 *  input_offset). So offsets map 1:1 — no geometry recomputation. Color-mode enum VALUES
 *  also coincide (RGB565=2, ARGB8888=0, RGB888=1) between LVGL and Beken, but we map
 *  explicitly rather than rely on that.
 *
 *  Compiled only for the Tuya build; the whole file is gated on BOARD_PLATFORM_TUYA.
 * ========================================================================== */
/* Include order matters: lv_conf.h (pulled by <lvgl.h>) is what DEFINES LV_USE_DRAW_DMA2D,
 * so we must include it BEFORE gating on that macro — otherwise the whole file compiles to
 * nothing and the LVGL TU's references to owf_dma2d_backend_* go unresolved at link. */
#include <cstring>   /* memset */
#include "board.h"
#include <lvgl.h>

#if BOARD_PLATFORM_TUYA && LV_USE_DRAW_DMA2D

/* Paths are relative to lvgl/src (the lvgl Arduino library's include root). */
#include "draw/dma2d/lv_draw_dma2d_private.h"   /* lv_draw_dma2d_configuration_t + enums */
#include "draw/dma2d/owf_tuya_dma2d_hal.h"       /* the bridge prototypes (canonical copy) */

extern "C" {
#include "driver/dma2d.h"        /* bk_dma2d_* driver API */
#include "driver/dma2d_types.h"  /* dma2d_config_t, enums */
}

/* DMA2D works entirely in PLAIN (normal-byte-order) RGB565: LVGL renders plain RGB565, and
 * the single panel-order byte-swap the CO5300 needs is done by the flush dirty-copy in
 * owf_tuya_lvgl_own.h — NOT here. So DMA2D output stays normal order (no out_byte_revese).
 *
 * WHY NOT swap in DMA2D output: the byte-swap can't live in DMA2D, because DMA2D BLENDING
 * also READS the framebuffer (background) and the BK7258 has no RGB565 input byte-swap
 * (only a YUV2RGB one). A swapped FB would be misread by blends -> purple edges. Keeping
 * the whole DMA2D pipeline in normal order and swapping once at flush is the only coherent
 * design. Leave OWF_DMA2D_OUT_BYTE_SWAP 0; it exists only as an escape hatch. */
#ifndef OWF_DMA2D_OUT_BYTE_SWAP
#define OWF_DMA2D_OUT_BYTE_SWAP 1
#endif
/* R/B channel swap (different from byte swap). Leave 0 unless colours come out R<->B flipped. */
#ifndef OWF_DMA2D_OUT_RB_SWAP
#define OWF_DMA2D_OUT_RB_SWAP 1
#endif

/* ---- enum translation: LVGL output/fgbg cf -> Beken color modes ------------------- */
static out_color_mode_t owf_out_cf(lv_draw_dma2d_output_cf_t cf)
{
    switch(cf) {
        case LV_DRAW_DMA2D_OUTPUT_CF_ARGB8888: return DMA2D_OUTPUT_ARGB8888;
        case LV_DRAW_DMA2D_OUTPUT_CF_RGB888:   return DMA2D_OUTPUT_RGB888;
        case LV_DRAW_DMA2D_OUTPUT_CF_RGB565:   return DMA2D_OUTPUT_RGB565;
        case LV_DRAW_DMA2D_OUTPUT_CF_ARGB1555: return DMA2D_OUTPUT_ARGB1555;
        case LV_DRAW_DMA2D_OUTPUT_CF_ARGB4444: return DMA2D_OUTPUT_ARGB4444;
        default:                               return DMA2D_OUTPUT_RGB565;
    }
}

static input_color_mode_t owf_in_cf(lv_draw_dma2d_fgbg_cf_t cf)
{
    switch(cf) {
        case LV_DRAW_DMA2D_FGBG_CF_ARGB8888: return DMA2D_INPUT_ARGB8888;
        case LV_DRAW_DMA2D_FGBG_CF_RGB888:   return DMA2D_INPUT_RGB888;
        case LV_DRAW_DMA2D_FGBG_CF_RGB565:   return DMA2D_INPUT_RGB565;
        case LV_DRAW_DMA2D_FGBG_CF_ARGB1555: return DMA2D_INPUT_ARGB1555;
        case LV_DRAW_DMA2D_FGBG_CF_ARGB4444: return DMA2D_INPUT_ARGB4444;
        case LV_DRAW_DMA2D_FGBG_CF_L8:       return DMA2D_INPUT_L8;
        case LV_DRAW_DMA2D_FGBG_CF_AL44:     return DMA2D_INPUT_AL44;
        case LV_DRAW_DMA2D_FGBG_CF_AL88:     return DMA2D_INPUT_AL88;
        case LV_DRAW_DMA2D_FGBG_CF_L4:       return DMA2D_INPUT_L4;
        case LV_DRAW_DMA2D_FGBG_CF_A8:       return DMA2D_INPUT_A8;
        case LV_DRAW_DMA2D_FGBG_CF_A4:       return DMA2D_INPUT_A4;
        default:                             return DMA2D_INPUT_RGB565;
    }
}

/* LVGL alpha-mode enum -> Beken blend_alpha_mode_t (same ordering: 0/1/2). */
static blend_alpha_mode_t owf_alpha_mode(uint32_t m)
{
    switch(m) {
        case LV_DRAW_DMA2D_ALPHA_MODE_NO_MODIFY_IMAGE_ALPHA_CHANNEL: return DMA2D_NO_MODIF_ALPHA;
        case LV_DRAW_DMA2D_ALPHA_MODE_REPLACE_ALPHA_CHANNEL:         return DMA2D_REPLACE_ALPHA;
        case LV_DRAW_DMA2D_ALPHA_MODE_MULTIPLY_IMAGE_ALPHA_CHANNEL:  return DMA2D_COMBINE_ALPHA;
        default:                                                     return DMA2D_NO_MODIF_ALPHA;
    }
}

/* ---- bring-up / teardown ------------------------------------------------------------ */
static bool s_owf_dma2d_up = false;

#if LV_USE_DRAW_DMA2D_INTERRUPT
extern "C" void owf_dma2d_backend_register_isr(void);
#endif

void owf_dma2d_backend_init(void)
{
    if(s_owf_dma2d_up) return;
    if(bk_dma2d_driver_init() != BK_OK) return;
#if LV_USE_DRAW_DMA2D_INTERRUPT
    /* Async mode only: register our transfer-complete ISR (routes to LVGL) and enable the
     * interrupt. In sync mode the dispatcher polls bk_dma2d_is_transfer_busy() instead, so
     * no ISR is wired (and lv_draw_dma2d_transfer_complete_interrupt_handler doesn't exist). */
    owf_dma2d_backend_register_isr();
    bk_dma2d_int_enable((dma2d_int_type_t)(DMA2D_TRANS_COMPLETE | DMA2D_TRANS_ERROR), 1);
#endif
    s_owf_dma2d_up = true;
}

void owf_dma2d_backend_deinit(void)
{
    if(!s_owf_dma2d_up) return;
#if LV_USE_DRAW_DMA2D_INTERRUPT
    bk_dma2d_int_enable((dma2d_int_type_t)(DMA2D_TRANS_COMPLETE | DMA2D_TRANS_ERROR), 0);
#endif
    bk_dma2d_driver_deinit();
    s_owf_dma2d_up = false;
}

bool owf_dma2d_backend_busy(void)
{
    return bk_dma2d_is_transfer_busy();
}

/* ---- the translation: LVGL conf -> Beken transfer + start -------------------------- */
void owf_dma2d_backend_start(const void *conf_v)
{
    const lv_draw_dma2d_configuration_t *c = (const lv_draw_dma2d_configuration_t *)conf_v;

    dma2d_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.init.color_mode    = (uint32_t)owf_out_cf(c->output_cf);
    cfg.init.output_offset = c->output_offset;
    cfg.init.red_blue_swap = OWF_DMA2D_OUT_RB_SWAP ? DMA2D_RB_SWAP : DMA2D_RB_REGULAR;
    cfg.init.alpha_inverted = DMA2D_REGULAR_ALPHA;
    cfg.init.line_offset_mode = 0;   /* 0 = offsets expressed in PIXELS (matches LVGL's
                                      * output_offset = stride_pixels - w). */
    cfg.init.trans_ability = MAX_TRANS_256BYTES;
    /* Panel-order (byte-swapped) RGB565 output — see OWF_DMA2D_OUT_BYTE_SWAP note above. */
    cfg.init.out_byte_by_byte_reverse = OWF_DMA2D_OUT_BYTE_SWAP ? DMA2D_BYTE_REVERSE : DMA2D_NO_REVERSE;

    /* CRITICAL: bk_dma2d_init() is what actually writes init.* (color_mode, output_offset,
     * line_offset_mode, out_byte_reverse) to the hardware registers (via dma2d_hal_init).
     * Without it the output stage is unconfigured -> black/garbage. The canonical
     * dma2d_fill()/memcpy paths all call it before the per-transfer config. mode must be
     * set first (dma2d_hal_init branches on it for out_color_mode). */
    /* NOTE: plain DMA2D_M2M forces out_color_mode=ARGB8888 in dma2d_hal_init (it's a raw
     * 32-bit byte copy that ignores color_mode) — wrong for our RGB565. So map BOTH the
     * plain copy and the PFC copy to DMA2D_M2M_PFC, which honours color_mode. (LVGL's
     * opaque-image path already emits WITH_PFC; plain M2M effectively never occurs.) */
    cfg.init.mode = (c->mode == LV_DRAW_DMA2D_MODE_REGISTER_TO_MEMORY)      ? DMA2D_R2M
                  : (c->mode == LV_DRAW_DMA2D_MODE_MEMORY_TO_MEMORY)        ? DMA2D_M2M_PFC
                  : (c->mode == LV_DRAW_DMA2D_MODE_MEMORY_TO_MEMORY_WITH_PFC) ? DMA2D_M2M_PFC
                  : DMA2D_M2M_BLEND;
    bk_dma2d_init(&cfg);

#if LV_USE_DRAW_DMA2D_INTERRUPT
    /* RE-ENABLE the transfer-complete interrupt EVERY transfer. The int-enable bits live in
     * the SAME dma2d_control_reg that bk_dma2d_init() (dma2d_hal_init) rewrites via field
     * setters, and the once-at-init enable does not reliably survive across transfers (the
     * symptom was: a few frames render, then the completion IRQ stops arriving and
     * wait_finish_cb blocks forever -> late hang). Setting it per-transfer, after init and
     * before start, guarantees the IRQ fires for this transfer. */
    bk_dma2d_int_enable((dma2d_int_type_t)(DMA2D_TRANS_COMPLETE | DMA2D_TRANS_ERROR), 1);
#endif

    switch(c->mode) {
        case LV_DRAW_DMA2D_MODE_REGISTER_TO_MEMORY: {
            /* Solid colour fill: register-to-memory. pdata carries the colour. */
            cfg.init.mode = DMA2D_R2M;
            bk_dma2d_transfer_config(&cfg, c->reg_to_mem_mode_color,
                                     (uint32_t)(uintptr_t)c->output_address, c->w, c->h);
            bk_dma2d_start_transfer();
            break;
        }

        case LV_DRAW_DMA2D_MODE_MEMORY_TO_MEMORY:
        case LV_DRAW_DMA2D_MODE_MEMORY_TO_MEMORY_WITH_PFC: {
            /* Opaque image copy / pixel-format convert: foreground -> output. Always PFC
             * (plain M2M would force ARGB8888 output — see the mode mapping above). mode
             * was already set + bk_dma2d_init'd above; we just configure the fg layer. */
            cfg.layer_cfg[DMA2D_FOREGROUND_LAYER].input_offset     = c->fg_offset;
            cfg.layer_cfg[DMA2D_FOREGROUND_LAYER].input_color_mode = (uint32_t)owf_in_cf(c->fg_cf);
            cfg.layer_cfg[DMA2D_FOREGROUND_LAYER].alpha_mode       = owf_alpha_mode(c->fg_alpha_mode);
            cfg.layer_cfg[DMA2D_FOREGROUND_LAYER].input_alpha      = c->fg_alpha;
            cfg.layer_cfg[DMA2D_FOREGROUND_LAYER].red_blue_swap    = DMA2D_RB_REGULAR;
            cfg.layer_cfg[DMA2D_FOREGROUND_LAYER].alpha_inverted   = DMA2D_REGULAR_ALPHA;
            bk_dma2d_layer_config(&cfg, DMA2D_FOREGROUND_LAYER);
            bk_dma2d_transfer_config(&cfg, (uint32_t)(uintptr_t)c->fg_address,
                                     (uint32_t)(uintptr_t)c->output_address, c->w, c->h);
            bk_dma2d_start_transfer();
            break;
        }

        /* BLENDING modes are NOT supported on this backend and must never reach here:
         * evaluate_cb (lv_draw_dma2d.c, OWF_TUYA_DMA2D_BACKEND) rejects every alpha/blend
         * task so LVGL's software draw unit handles them. The reasons are fundamental, not
         * tuning: (1) DMA2D blending READS the framebuffer as the background layer, but the
         * BK7258 DMA2D has no RGB565 input byte-swap, so it can't correctly read our
         * panel-order FB; (2) LVGL encodes solid/alpha fills via an STM32-specific fake-A8
         * blend idiom that doesn't map to Beken's blend HW (dropped glyphs + faults).
         * If a blend mode somehow arrives, do NOTHING here (the task will be marked finished
         * with the software-rendered content intact) rather than run the broken path. */
        case LV_DRAW_DMA2D_MODE_MEMORY_TO_MEMORY_WITH_BLENDING:
        case LV_DRAW_DMA2D_MODE_MEMORY_TO_MEMORY_WITH_BLENDING_AND_FIXED_COLOR_FG:
        case LV_DRAW_DMA2D_MODE_MEMORY_TO_MEMORY_WITH_BLENDING_AND_FIXED_COLOR_BG:
        default:
            break;
    }
}

/* ---- ISR: route Beken DMA2D transfer-complete to LVGL (ASYNC mode only) ------------ */
/* lv_draw_dma2d_transfer_complete_interrupt_handler() only EXISTS when
 * LV_USE_DRAW_DMA2D_INTERRUPT is 1 (it's #if-guarded in lv_draw_dma2d.c). In sync mode
 * the dispatcher polls instead, so this whole ISR block is compiled out — referencing the
 * handler here without the guard is an undefined-symbol link error. */
#if LV_USE_DRAW_DMA2D_INTERRUPT
extern "C" void lv_draw_dma2d_transfer_complete_interrupt_handler(void);

static void owf_dma2d_complete_isr(void)
{
    /* The driver's dma2d_isr_common() clears DMA2D_TRANS_COMPLETE_STATUS itself, right
     * after invoking this callback — so we must NOT clear it here (double-clear). We only
     * notify LVGL that the transfer finished. */
    lv_draw_dma2d_transfer_complete_interrupt_handler();
}

/* If a transfer ERRORS (bad addr/alignment) the HW fires TRANS_ERROR, NOT TRANS_COMPLETE.
 * Without handling it, the completion IRQ never arrives and wait_finish_cb blocks forever
 * (a 'works then hangs' symptom). Route the error to the SAME LVGL signal so the wait is
 * released — the frame may be wrong, but the UI keeps running instead of dead-locking. */
static void owf_dma2d_error_isr(void)
{
    lv_draw_dma2d_transfer_complete_interrupt_handler();
}

extern "C" void owf_dma2d_backend_register_isr(void)
{
    bk_dma2d_register_int_callback_isr(DMA2D_TRANS_COMPLETE_ISR, owf_dma2d_complete_isr);
    bk_dma2d_register_int_callback_isr(DMA2D_TRANS_ERROR_ISR,    owf_dma2d_error_isr);
}
#endif /* LV_USE_DRAW_DMA2D_INTERRUPT */

#endif /* BOARD_PLATFORM_TUYA && LV_USE_DRAW_DMA2D */
