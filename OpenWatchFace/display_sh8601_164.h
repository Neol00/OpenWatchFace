/* ============================================================================
 *  display_sh8601_164.h — SH8601 vendor register init (S3-Touch-AMOLED-1.64)
 *
 *  WHY THIS FILE EXISTS
 *  --------------------
 *  Arduino_GFX's Arduino_SH8601 runs a generic SH8601 init in tftInit()
 *  (SLPOUT -> NORON -> INVOFF -> PIXFMT 16bpp -> DISPON -> brightness). That is
 *  enough for the S3-1.8's panel, but NOT for this 1.64 module: Waveshare's
 *  demo sends one extra command that the library NEVER sends —
 *
 *      0xC4 = 0x80    (SH8601_W_SPIMODECTL — SPI mode control)
 *
 *  — immediately after sleep-out. That register selects the panel's data
 *  interface mode. Without it this module stays in its power-on default and
 *  simply IGNORES the QSPI pixel writes: the panel never lights, while the rest
 *  of the firmware runs perfectly (serial log fine, touch fine, flush path
 *  reporting normally). The symptom is a COMPLETELY BLACK screen on an
 *  otherwise-healthy watch — which looks like a driver/pin bug but is not.
 *
 *  Note the library DOES define the constant (SH8601_W_SPIMODECTL 0xC4 in
 *  Arduino_SH8601.h) — it just never emits it, so this is a gap in the generic
 *  init rather than a wrong panel choice.
 *
 *  Same pattern as display_jd9853.h: apply the vendor table once, right after
 *  gfx->begin().
 *
 *  Values verbatim from Waveshare's 1.64 demo (identical in both copies:
 *  Arduino/examples/06_LVGL_Test/lcd_bsp.c and
 *  ESP-IDF/06_LVGL_Test/main/main.c, `lcd_init_cmds[]`):
 *
 *      {0x11, {0x00}, 0, 80}    sleep out, 80 ms
 *      {0xC4, {0x80}, 1, 0}     SPI mode control   <-- the missing one
 *      {0x35, {0x00}, 1, 0}     tearing effect line on
 *      {0x53, {0x20}, 1, 1}     CTRL display: brightness ctrl on
 *      {0x63, {0xFF}, 1, 1}     HBM brightness = max
 *      {0x51, {0x00}, 1, 1}     brightness 0 (raised at the end)
 *      {0x29, {0x00}, 0, 10}    display on, 10 ms
 *      {0x51, {0xFF}, 1, 0}     brightness full
 *
 *  We deliberately do NOT re-send 0x11/0x29 here — Arduino_SH8601::tftInit()
 *  has already done sleep-out and display-on by the time this runs, and a second
 *  SLPOUT would incur another 80 ms stall for no benefit. We send the registers
 *  the library MISSES, in the vendor's order.
 *
 *  Brightness is left at FULL (0x51 = 0xFF) to match the vendor end-state; the
 *  firmware immediately overrides it via board_display_set_brightness() from the
 *  saved setting, so this is just a sane starting point.
 * ========================================================================== */
#pragma once
#if BOARD_DISPLAY_SH8601_QSPI && defined(BOARD_WS_S3_TOUCH_AMOLED_164)

static void sh8601_164_reg_init(void) {
  static const uint8_t init_operations[] = {
    BEGIN_WRITE,

    /* SPI/QSPI interface mode — THE critical one. Must precede any pixel
     * traffic; without it the panel ignores the QSPI writes and stays dark. */
    WRITE_C8_D8, 0xC4, 0x80,

    /* Tearing-effect line on (vendor sends it; harmless if the TE pin is
     * unused, and it matches the vendor's end-state exactly). */
    WRITE_C8_D8, 0x35, 0x00,

    /* CTRL display: brightness control on. Vendor uses 0x20 where the library's
     * generic table used 0x28 (which also enables display DIMMING). Take the
     * vendor value — dimming would make our own brightness ramps fight the
     * panel's internal fade. */
    WRITE_C8_D8, 0x53, 0x20,

    /* HBM (high-brightness mode) value = max. */
    WRITE_C8_D8, 0x63, 0xFF,

    END_WRITE,

    DELAY, 10,

    BEGIN_WRITE,
    /* Normal-mode brightness to full; the firmware overrides this from the
     * saved brightness setting right after boot. */
    WRITE_C8_D8, 0x51, 0xFF,
    END_WRITE,

    DELAY, 10,
  };
  /* batchOperation() lives on Arduino_DataBus, not Arduino_GFX — so this goes
   * through `bus`, exactly like jd9853_reg_init() does. */
  bus->batchOperation(init_operations, sizeof(init_operations));
}

#endif
