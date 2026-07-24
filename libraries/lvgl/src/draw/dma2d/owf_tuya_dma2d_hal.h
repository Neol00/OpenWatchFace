/* ============================================================================
 *  owf_tuya_dma2d_hal.h — LVGL's LV_DRAW_DMA2D_HAL_INCLUDE target on the T5 (BK7258).
 *
 *  Lives inside the LVGL source tree (next to the DMA2D draw unit) because lv_draw_dma2d.c
 *  includes this via LV_DRAW_DMA2D_HAL_INCLUDE, and only LVGL's own src/ include roots are
 *  reliably on that compile's -I — not the sketch's tuya/compat/. Referenced from lv_conf.h
 *  as "draw/dma2d/owf_tuya_dma2d_hal.h" (relative to lvgl/src, which Arduino adds as -I).
 *
 *  WHY THIS EXISTS: LVGL's DMA2D draw unit is written for the STM32 DMA2D peripheral — it
 *  #include's an STM32 HAL header and writes STM32 registers directly (DMA2D->NLR,
 *  RCC->AHB1ENR, NVIC_EnableIRQ(DMA2D_IRQn), ...). The BK7258 has a DMA2D engine too, but
 *  a DIFFERENT register map driven through a driver API (bk_dma2d_*, in libdriver.a —
 *  verified present via nm). So LV_DRAW_DMA2D_HAL_INCLUDE points here instead of
 *  "stm32h7xx_hal.h", and the STM32 register pokes in lv_draw_dma2d.c are wrapped in
 *  `#if OWF_TUYA_DMA2D_BACKEND` blocks that call the bridge functions declared below
 *  (implemented in the sketch's owf_tuya_dma2d_backend.cpp).
 * ========================================================================== */
#ifndef OWF_TUYA_DMA2D_HAL_H
#define OWF_TUYA_DMA2D_HAL_H

#include <stdint.h>
#include <stdbool.h>

/* Marker the LVGL DMA2D draw unit gates on. Because lv_draw_dma2d.c includes this header
 * (via LV_DRAW_DMA2D_HAL_INCLUDE), defining it here makes the symbol visible inside the
 * LVGL library TU — where BOARD_PLATFORM_TUYA (a sketch-level define) is NOT visible. The
 * STM32 register code in lv_draw_dma2d.c is wrapped in `#if OWF_TUYA_DMA2D_BACKEND`. */
#define OWF_TUYA_DMA2D_BACKEND 1

#ifdef __cplusplus
extern "C" {
#endif

/* The LVGL config struct (lv_draw_dma2d_configuration_t) is defined in
 * lv_draw_dma2d_private.h, which is included AFTER this header. lv_draw_dma2d.c calls our
 * bridge from inside functions where that type is in scope, so we take it as an opaque
 * pointer here (void*) to avoid an include cycle, and cast in the backend .cpp (which
 * includes the LVGL private header). */

/* Bring up / tear down the Beken DMA2D engine (clock/power vote + ISR register). Called
 * from lv_draw_dma2d_init/deinit. */
void owf_dma2d_backend_init(void);
void owf_dma2d_backend_deinit(void);

/* Translate one LVGL DMA2D configuration into the Beken driver calls and START the
 * transfer (asynchronous — completion arrives via the DMA2D interrupt). `conf` is a
 * `const lv_draw_dma2d_configuration_t *`. */
void owf_dma2d_backend_start(const void *conf);

/* Poll: true while a transfer is still running (maps to bk_dma2d_is_transfer_busy). Used
 * by the non-async completion check. */
bool owf_dma2d_backend_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* OWF_TUYA_DMA2D_HAL_H */
