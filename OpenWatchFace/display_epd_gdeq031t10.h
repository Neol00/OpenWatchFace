/* ============================================================================
 *  display_epd_gdeq031t10.h — GDEQ031T10 240x320 e-paper (UC8253) for the
 *  LilyGo T-Deck Pro, driven through GxEPD2.
 *
 *  This is the ONLY display in this firmware that does not paint from the LVGL
 *  flush. Understanding why is the whole file, so read this block before
 *  changing anything below it.
 *
 *  ============ THE PROBLEM THIS FILE SOLVES ============
 *  Every other board pushes pixels inside my_disp_flush(): LVGL hands over a
 *  dirty tile, the tile goes out over SPI, done in a few ms. An e-paper panel
 *  cannot work that way. A refresh is a physical process — particles migrating
 *  through fluid — and costs 0.7 s (partial) to 1.1 s (full). LVGL renders a
 *  frame as MANY tiles (BOARD_PARTIAL_BUF_LINES 90 => four bands per full
 *  screen, more when several widgets are dirty). Refreshing per tile would mean
 *  several seconds of flashing per frame, and the panel would still be mid-way
 *  through the previous refresh when the next tile arrived.
 *
 *  So the integration is INVERTED. Two decoupled halves:
 *
 *    1. my_disp_flush() -> epd_blit_tile()  — pure memory. Thresholds the
 *       RGB565 tile into a 1bpp shadow buffer, grows a dirty bounding box,
 *       returns. No SPI, no waiting. Costs microseconds, so LVGL keeps its
 *       normal frame rate and the UI stays responsive.
 *
 *    2. loop() -> epd_service()  — the only thing that ever touches the panel.
 *       Once per EPD_MIN_REFRESH_MS at most, it pushes the accumulated dirty
 *       box in ONE refresh, no matter how many tiles fed it.
 *
 *  That coalescing is the single biggest speed win here, and it is structural
 *  rather than a tuning knob: N dirty tiles cost ONE refresh instead of N.
 *
 *  ============ WHERE THE SPEED COMES FROM (in order of impact) ============
 *    1. ONE coalesced refresh per frame instead of one per dirty tile. Above.
 *    2. PARTIAL (~700 ms) instead of FULL (~1100 ms + a black/white flash).
 *       Partial is also the difference between "the clock ticked" and "the
 *       whole screen strobed", which matters more than the milliseconds.
 *    3. Refreshing only the DIRTY WINDOW rather than the whole panel. A minute
 *       change on the watch face is a small band, not 240x320.
 *    4. TSSET — the forced-temperature waveform dial. See below.
 *    5. SPI clock. A ~2% effect: the data is 9.6 KB and the wait is ~700 ms of
 *       panel physics. Do NOT go hunting here; there is nothing to win.
 *
 *  ============ THE WAVEFORM DIAL: PLL, NOT TSSET ============
 *  Stock partial refresh measures 646-700 ms (~1.5 fps) in this firmware.
 *
 *  An early bench sketch reported ~20 ms (~49 fps) for the fast waveform. That
 *  number is NOT trustworthy: it was measured in a configuration that left the
 *  glass BLANK, so it timed a waveform that moved no ink. Do not quote it, and
 *  do not treat any refresh timing as a result unless an image is verified on
 *  the panel at the same time.
 *
 *  TSSET is not a speed dial. TS_SET[7:0] is a SIGNED temperature in C that
 *  selects one of 16 OTP temperature-range banks. 0x79 (121 C) is already the
 *  practical ceiling; values above 0x7F are NEGATIVE and select COLDER, far
 *  SLOWER waveforms. EPD_TSSET_PART / EPD_TSSET_FULL and patch 09 are still
 *  correct and useful for tuning drive strength, but they buy almost no speed.
 *
 *  There IS a user LUT: PSR (R00H) bit D5 (REG) switches the LUT source from
 *  OTP to registers R20H-R24H, which puts the panel on a short waveform. The
 *  speed then comes from the PLL frame rate (R30H, FRS[4:0]), which sets how
 *  long one waveform frame lasts: 50 Hz default up to 200 Hz.
 *
 *  Full detail, the measured table, and what does NOT work are documented above
 *  EpdFastGDEQ031T10 below. EPD_FAST_WAVEFORM 0 builds the stock behaviour.
 *
 *  ============ GHOSTING, AND WHY EPD_FULL_EVERY EXISTS ============
 *  Partial refreshes leave residue. Every EPD_FULL_EVERY partials this file
 *  promotes one refresh to full, which flashes but resets the glass. Lower it
 *  for a cleaner image and more flashing; 0 disables full refreshes entirely
 *  (fastest, ghosts accumulate forever).
 *
 *  ============ THE BUSY CALLBACK ============
 *  A refresh blocks for the better part of a second inside GxEPD2's BUSY-pin
 *  wait. Without help, input sampled only in loop() would stall for that whole
 *  time and a tap starting or ending mid-refresh would look stretched. So the
 *  .ino registers cst328_poll() via epd_set_busy_callback() and this file calls
 *  it while waiting. THE RULES FOR THAT CALLBACK ARE STRICT, because it runs
 *  in the middle of a panel transaction:
 *      - I2C and plain variables only.
 *      - NO SPI (the bus is mid-transaction and shared with SD + LoRa).
 *      - NO LVGL calls (would re-enter the renderer from inside a flush).
 *      - NO blocking.
 *
 *  ============ SHARED SPI BUS ============
 *  E-paper (CS 34), microSD (CS 48) and the SX1262 (CS 3) share one host. The
 *  panel is touched ONLY from epd_service() in loop(), single-threaded, which
 *  is why no arbitration exists. If anything ever drives the panel from a task
 *  or an ISR, that assumption breaks and this board needs real locking.
 *
 *  ============ DEBUGGING: READ THE [epd] LINE FIRST ============
 *  Each refresh prints a profile line. `wait` is REAL PANEL TIME (GxEPD2 waits
 *  on the BUSY pin, not a fixed delay), and it is the first thing to look at:
 *      wait ~700 / ~1100 ms -> the refresh RAN. Any fault is in the image data
 *                              or the waveform, not the command sequence.
 *      wait a few ms        -> the refresh NEVER ran: command or wiring fault.
 *                              Nothing about TSSET or EPD_FULL_EVERY matters.
 *      NO [epd] LINE AT ALL -> epd_service() is returning at its first line,
 *                              i.e. nothing is dirty. The panel is frozen on
 *                              whatever epd_begin() left. Look at the
 *                              invalidate path in setup(), not at this file.
 * ========================================================================== */
#pragma once

#include <GxEPD2_BW.h>

/* ---- Tunables (overridable from the board header) ------------------------ */

/* Floor on how often the panel may be pushed. Below the panel's own ~700 ms
 * partial time this is a no-op — the refresh itself is the real limit. */
#ifndef EPD_MIN_REFRESH_MS
#define EPD_MIN_REFRESH_MS 1
#endif

/* Fast waveform: register LUT unlock + raised PLL frame rate.
 *
 * STATUS: under evaluation with BOARD_LVGL_FULL_PSRAM_FB. The custom register
 * LUT previously rendered only when the redraw area was large — small partial
 * windows completed on the BUSY pin but moved no ink. With the full-screen
 * framebuffer every refresh is now a whole-panel update, which removes partial
 * window addressing as a variable so the LUT can be judged on its own.
 *
 * If the panel renders correctly with this on, the LUT works and the remaining
 * question is how far EPD_PLL_RATE and EPD_LUT_DRIVE_FRAMES can be pushed.
 * If it is still blank, the register-LUT path is not viable on this panel and
 * this should go back to 0. */
#ifndef EPD_FAST_WAVEFORM
#define EPD_FAST_WAVEFORM 1
#endif

/* Raise the PLL frame rate on the STOCK OTP waveform. This is independent of
 * EPD_FAST_WAVEFORM and is the safe speed lever: the factory waveform still
 * runs, with all its phases, just clocked faster. The panel's own frame count
 * is unchanged, so the image is driven exactly as the vendor intended.
 *
 * 0 keeps the controller default (50 Hz). Otherwise an FRS[4:0] value:
 *   0x0F  80 Hz    0x13 100 Hz    0x17 120 Hz    0x1F 200 Hz
 * Expect refresh time to scale roughly as 50/rate: at 100 Hz a 646 ms partial
 * should approach ~320 ms. Raise it until contrast starts to wash out. */
#ifndef EPD_STOCK_PLL_RATE
#define EPD_STOCK_PLL_RATE 0x11
#endif

/* PLL frame rate (R30H FRS[4:0]) used by the fast waveform. Measured:
 *   0x09  50 Hz -> ~45 ms (22 fps)      0x13 100 Hz -> ~24 ms (42 fps)
 *   0x0F  80 Hz -> ~29 ms (34 fps)      0x17 120 Hz -> ~20 ms (49 fps)
 * 0x1B (160 Hz) and 0x1F (200 Hz) are untested; expect contrast to wash out. */
#ifndef EPD_PLL_RATE
#define EPD_PLL_RATE 0x11
#endif

/* Frames of drive per fast refresh (1..63). Higher = stronger, darker, slower.
 * Total time is roughly max(black, white) frames / PLL_rate.
 * Start high enough to SEE something, then lower it until the image degrades. */
#ifndef EPD_LUT_DRIVE_FRAMES
#define EPD_LUT_DRIVE_FRAMES 2
#endif

/* The two drive directions have INDEPENDENT LUTs, and they are not symmetric:
 * driving ink to black needs more charge-time than releasing it to white. The
 * refresh lasts as long as the LONGEST LUT, so raising only the black count
 * buys black density at half the cost of raising both (white pixels simply
 * finish early and hold).
 *
 * If black looks washed out / grey: raise EPD_LUT_BLACK_FRAMES only.
 * White frames can usually stay at the bare minimum that leaves no dark haze
 * where content was erased. */
#ifndef EPD_LUT_BLACK_FRAMES
#define EPD_LUT_BLACK_FRAMES 8
#endif
#ifndef EPD_LUT_WHITE_FRAMES
#define EPD_LUT_WHITE_FRAMES EPD_LUT_DRIVE_FRAMES
#endif

/* ============ CONTINUOUS-DRIVE MODE (PaperBoy-style field engine) ============
 * Ported from the ALGORITHM in Wenting Zhang's PaperBoy msg.c (his hardware
 * direct-drives the panel; ours can't, but the idea survives): the firmware —
 * not the controller — is the waveform engine. The display runs as a free
 * loop of very short "fields". Every pixel carries a counter of how many
 * fields it has been driven toward its target; a transition keeps receiving
 * drive across CONSECUTIVE fields until the counter saturates, and NEW image
 * content is accepted every field. Frame rate and transition time are thereby
 * DECOUPLED: a flip takes EPD_CONT_BLACK_FIELDS / EPD_CONT_WHITE_FIELDS
 * fields to complete (per direction), but the image can change every field.
 *
 * The UC8253 is demoted to a dumb one-field driver: each cycle we write BOTH
 * of its RAM planes ourselves (a mid-transition pixel gets old=source,
 * new=target again — we lie about "old" to extend the drive), run a 1-frame
 * register LUT at a high PLL rate, and immediately start the next field.
 *
 * EPD_CONT_BLACK_FIELDS / EPD_CONT_WHITE_FIELDS are the tuning knobs: how
 * many fields a transition is driven for, PER DIRECTION (1..63). Low = faster,
 * lighter/ghostier ink. High = darker, more complete transitions, more
 * trailing. Runtime-settable too: epd_cont_set_fields(black, white). */
/* Starting point maths: total drive = fields * frames / PLL rate. The tuned
 * one-shot waveform gives black ~130 ms of drive (12 fr @ 90 Hz) and white
 * ~67 ms — black consistently needs ~2x white's charge time on this glass, so
 * the field counts are PER DIRECTION. At 120 Hz with 1 frame per field the
 * defaults below reproduce those drive totals (16 fields ≈ 133 ms black,
 * 8 ≈ 67 ms white). Raise the BLACK count first if ink looks washed out;
 * white finishing early costs nothing extra. */
/* 140 Hz step (2026-08-18): each frame at 140 Hz carries 120/140 of the charge
 * it did at 120 Hz, so the field budgets scale UP to keep total drive time at
 * least what the proven 120 Hz tuning delivered: 8 fields x 4 fr / 120 Hz
 * ≈ 267 ms  ->  10 fields x 4 fr / 140 Hz ≈ 286 ms per direction. If ink still
 * looks washed out at 140 Hz, raise the BLACK count first; if it renders
 * blank, the charge pump is out of settle time — fall back to 0x17. */
#ifndef EPD_CONT_BLACK_FIELDS
#define EPD_CONT_BLACK_FIELDS 4 /* fields of drive per toward-BLACK transition */
#endif
#ifndef EPD_CONT_WHITE_FIELDS
#define EPD_CONT_WHITE_FIELDS 4 /* fields of drive per toward-WHITE transition */
#endif
#ifndef EPD_CONT_BLACK_FRAMES
#define EPD_CONT_BLACK_FRAMES 4  /* LUT frames per field, toward black */
#endif
#ifndef EPD_CONT_WHITE_FRAMES
#define EPD_CONT_WHITE_FRAMES 4  /* LUT frames per field, toward white */
#endif
/* PLL during fields. NOTE ON THE OLD "120 Hz ceiling": 0x1B/0x1F (160/200 Hz)
 * rendered blank on the ONE-SHOT path (2026-08-10), but that path had to fit
 * the whole transition into one refresh — at 200 Hz each frame delivers under
 * half the drive time it does at 90 Hz, so the ink plausibly just never got
 * enough total push (the user's hypothesis, and the math agrees). Continuous
 * mode accumulates drive ACROSS fields, so weak-but-many is exactly its
 * operating regime — 200 Hz is worth retesting here. If the panel is blank
 * even with EPD_CONT_FIELDS raised high, THEN it's genuinely the charge pump,
 * and 0x17 (120 Hz) is the proven fallback. */
#ifndef EPD_CONT_PLL
#define EPD_CONT_PLL 0x19        /* 140 Hz — field budgets scaled above */
#endif
/* Start in continuous mode at boot (it can also be toggled at runtime with
 * epd_continuous_set). Off by default: this is an experimental pipeline. */
#ifndef EPD_CONTINUOUS_BOOT
#define EPD_CONTINUOUS_BOOT 1
#endif
/* IDLE SETTLE PASS. The short per-field LUT is deliberately weak (that is what
 * makes fields fast), so a transition that exhausts its field budget — or one
 * whose target flipped mid-drive from a half-grey state — can stop short of
 * true black/white and leave a visible trace. The settle pass fixes that
 * structurally instead of by tuning: once the screen has been QUIET for this
 * many ms (no new content, every pixel at its budget), one STOCK-OTP-waveform
 * partial refresh runs over the union of everything touched since the last
 * settle. The factory waveform is the guaranteed-complete drive, so it snaps
 * every under-driven pixel to its final level and wipes the residue. It costs
 * one ~650 ms blocking refresh, but only ever at idle — animation frame rate
 * is untouched. 0 disables the pass entirely.
 *
 * TUNING HISTORY (2026-08-18), so nobody re-walks these dead ends:
 *   - 400 ms: WORKS. Clean, complete image; the cost is a visible beat of
 *     stillness before the final redraw.
 *   - 0 (off, finish kick instead): visible traces — the kick's single-
 *     polarity LUT only pushes ink toward its target; the OTP waveform's
 *     multi-phase drive is what actually ERASES residue.
 *   - 60 ms bare timer: DISASTER — permanently grey panel. Below the UI's
 *     inter-update gap the settle fires in the small pauses WITHIN an
 *     interaction, each ~650 ms stock refresh lands mid-animation, and the
 *     glass lives inside the stock waveform's grey inversion phases.
 * RESOLUTION: a timer alone cannot tell "pause within an interaction" from
 * "interaction over", so the settle is now ALSO gated on the interact probe
 * (epd_set_interact_probe — the .ino wires it to the live finger-down state).
 * While a finger is on the glass the settle NEVER fires, no matter how long
 * the pause; once it lifts and rendering stops, this short delay is all that
 * remains. That is what makes a low value safe here where 60 ms was not. */
#ifndef EPD_CONT_SETTLE_MS
#define EPD_CONT_SETTLE_MS 100
#endif
/* FINISH KICK. The instant the engine goes quiet (no new content, every
 * transition at its budget), rewind the drive counters of everything in the
 * touched window by this many fields. Those pixels then receive that much
 * extra reinforcement drive IMMEDIATELY, at normal field cadence (~43 ms per
 * field) — no waiting, no stock-waveform pause, no flash. Blacks get darker,
 * whites cleaner, mid-flip stragglers complete; an already-arrived pixel is
 * simply pushed harder toward the color it is already showing, which is
 * harmless ON THE GLASS. Cost: EPD_CONT_FINISH_FIELDS extra fields once per
 * quiet period. 0 disables.
 *
 * DEFAULT OFF (2026-08-18): the kick POISONS THE SETTLE PASS. Re-driving a
 * pixel writes lie-planes (old = opposite of target) into the controller, so
 * the settle's stock refresh right after sees EVERY kicked pixel as changed
 * and runs the full OTP transition — inversion phase included — over the
 * whole touched window. After a screen switch that window is the entire panel:
 * full-screen black flash, then grey, then correcting to white. Without the
 * kick the old plane stays honest (done pixels have old == new from their
 * last idle field) and the settle drives ONLY the genuine stragglers, which
 * is the quiet, clean behaviour. Only enable this if the settle pass is
 * disabled too. */
#ifndef EPD_CONT_FINISH_FIELDS
#define EPD_CONT_FINISH_FIELDS 0
#endif

/* Promote every Nth partial to a full refresh to clear ghosting. 0 disables.
 *
 * Kept at 0 (the value this firmware has always used). A full refresh costs
 * ~1030 ms of BLOCKING panel time and flashes the screen black — at any nonzero
 * value that lands in the middle of interaction as a visible freeze. Raise it
 * only if ghosting becomes intolerable, and prefer a large value (24+) or a
 * call to epd_service(true) at a natural idle point instead. */
#ifndef EPD_FULL_EVERY
#define EPD_FULL_EVERY 0
#endif

/* Forced-temperature waveform selection. Inert without patch 09. */
#ifndef EPD_TSSET_PART
#define EPD_TSSET_PART 0x79
#endif
#ifndef EPD_TSSET_FULL
#define EPD_TSSET_FULL 0x5A
#endif

/* SPI clock. 10 MHz is comfortably inside the UC8253's spec and, per the note
 * above, is NOT where the time goes. */
#ifndef EPD_SPI_HZ
#define EPD_SPI_HZ 20000000
#endif

/* Serial profile line per refresh. */
#ifndef EPD_PROFILE_SERIAL
#define EPD_PROFILE_SERIAL 1
#endif

/* ---- Panel object --------------------------------------------------------
 * Page height = HEIGHT: GxEPD2 is used as a full-screen buffer, but we never
 * use its paged drawing — the shadow buffer below is our own. The template
 * argument only sizes GxEPD2's internal buffer, which we bypass via
 * writeImage(). */
/* ---- Fast-waveform subclass ----------------------------------------------
 * The comment at the top of this file says the UC8253 has "no user LUT on this
 * path, so fast mode means lying about the temperature". That was measured to
 * be wrong on both counts, and the correction is worth stating because it moves
 * partial refresh from 646 ms to ~20 ms.
 *
 * MEASURED ON THIS BOARD (BUSY-pin timing, not datasheet numbers):
 *
 *   stock partial (TSSET 0x79)                    646 ms   1.5 fps
 *   register LUT unlocked, PLL  50 Hz              45 ms  22.2 fps
 *   register LUT unlocked, PLL  80 Hz              29 ms  34.2 fps
 *   register LUT unlocked, PLL 100 Hz              24 ms  41.7 fps
 *   register LUT unlocked, PLL 120 Hz              20 ms  49.3 fps
 *
 * TWO CORRECTIONS TO THE HEADER COMMENT ABOVE:
 *
 * 1. TSSET is NOT a speed dial. TS_SET[7:0] is a SIGNED temperature in C that
 *    selects one of 16 OTP temperature-range banks (UC8253c_A0.6, "TEMPERATURE
 *    RANGE"). 0x79 (121 C) is already effectively the ceiling, which is why
 *    raising it only ever bought a few percent. Values above 0x7F are NEGATIVE
 *    and select COLDER, much SLOWER waveforms — 0x8C (-116 C) takes so long it
 *    blows past a 3 s timeout. EPD_TSSET_PART/FULL and patch 09 remain correct
 *    and useful for tuning quality, but they are not where the speed is.
 *
 * 2. There IS a user LUT. PSR (R00H) bit D5 (REG) selects the LUT source:
 *    0 = OTP (what the stock driver uses), 1 = registers R20H-R24H. Setting it
 *    puts the panel on a short waveform.
 *
 * WHAT ACTUALLY PROVIDES THE SPEED: the PLL frame rate (R30H, FRS[4:0]) sets
 * how long ONE waveform frame lasts — 0x09 = 50 Hz (20 ms/frame) default, up to
 * 0x1F = 200 Hz (5 ms/frame). Every measurement above fits
 * "2 frames / rate + ~4 ms fixed overhead" exactly.
 *
 * A RETRACTED FINDING: an earlier sweep concluded the frame-count field inside
 * the LUT registers was ignored (1 and 63 frames both measured 45.1 ms at
 * 50 Hz). That sweep ran LUTs whose group/state REPEAT counts were 0 — per the
 * datasheet, "no repeat" means the group NEVER EXECUTES — so it timed an empty
 * waveform regardless of frame count. With the repeats fixed (see _writeLut),
 * expect timing to track lut_drive_frames / PLL_rate.
 *
 * GHOSTING: a 2-frame waveform leaves residue, and it accumulates. EPD_FULL_EVERY
 * (already in this file) is the cleanup schedule; Good Display's own guidance for
 * this panel is a full refresh after 5 consecutive fast updates. With fast mode
 * on, EPD_FULL_EVERY 0 (never clear) will smear badly — see the default below. */

class EpdFastGDEQ031T10 : public GxEPD2_310_GDEQ031T10 {
 public:
  EpdFastGDEQ031T10(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_310_GDEQ031T10(cs, dc, rst, busy) {}

  /* PLL frame rate (R30H FRS[4:0]). The speed dial. See table above.
   * 0x1B (160 Hz) and 0x1F (200 Hz) are untested — contrast is expected to wash
   * out as the charge pump loses settle time per scan. */
  uint8_t pll_frame_rate = EPD_PLL_RATE;

  /* Per-direction drive (1..63 frames each, 6-bit field). Black needs more
   * charge-time than white; the refresh lasts max(black, white) frames, so
   * white finishing early is free. See the tunables block. */
  uint8_t lut_black_frames = EPD_LUT_BLACK_FRAMES;
  uint8_t lut_white_frames = EPD_LUT_WHITE_FRAMES;

  /* False falls back to the stock ~646 ms waveform. Full refreshes always use
   * the stock path regardless: that is what clears ghosting. */
  bool fast_mode = (EPD_FAST_WAVEFORM != 0);

  /* Measured duration of the last refresh, in microseconds. */
  volatile uint32_t last_refresh_us = 0;
  volatile bool     timed_out = false;

  /* GxEPD2_BW calls epd2.refresh(...) internally. epd2 is a concrete member, so
   * these bind statically — no vtable needed. */
  void refresh(bool partial_update_mode = false) {
    if (!partial_update_mode || !fast_mode) {
      _stockRefresh(partial_update_mode);
      return;
    }
    /* The panel needs one full refresh before any partial can work: it is what
     * establishes the controller's previous-image buffer, and the stock driver
     * only clears _initial_refresh in the full path. Skipping this leaves every
     * partial differencing against undefined RAM, which shows up as a panel
     * that never renders anything.
     *
     * This bit was previously masked by EPD_FULL_EVERY: a periodic full refresh
     * happened to initialise the panel, so the missing guard went unnoticed
     * until that was (correctly) turned back off. */
    if (_initial_refresh) {
      _stockRefresh(false);
      return;
    }
    _fastRefresh();
  }

  void refresh(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!fast_mode) {
      uint32_t t0 = micros();
      _applyStockPll();
      GxEPD2_310_GDEQ031T10::refresh(x, y, w, h);
      last_refresh_us = micros() - t0;
      return;
    }
    /* Set the partial RAM window exactly as the stock windowed refresh does —
     * it does NOT inherit the window from writeImagePart(). Same clipping and
     * 8-alignment, then run the fast waveform in place of _Update_Part(). */
    if (_initial_refresh) { refresh(false); return; }
    int16_t w1 = x < 0 ? w + x : w;
    int16_t h1 = y < 0 ? h + y : h;
    int16_t x1 = x < 0 ? 0 : x;
    int16_t y1 = y < 0 ? 0 : y;
    w1 = x1 + w1 < int16_t(WIDTH) ? w1 : int16_t(WIDTH) - x1;
    h1 = y1 + h1 < int16_t(HEIGHT) ? h1 : int16_t(HEIGHT) - y1;
    if ((w1 <= 0) || (h1 <= 0)) return;
    w1 += x1 % 8;
    if (w1 % 8 > 0) w1 += 8 - w1 % 8;
    x1 -= x1 % 8;

    _writeCommand(0x91); /* partial in */
    _setFastRamArea(x1, y1, w1, h1);
    _fastRefresh();
  }

  /* One continuous-drive FIELD: run the fast waveform once, full-screen, with
   * the given (very short) frame counts and PLL. The caller has already put
   * the two RAM planes in the state it wants (old != new exactly on the pixels
   * that must receive drive this field). Restores the normal fast-mode tuning
   * afterwards so ordinary partial refreshes are unaffected. */
  void continuousField(uint8_t black_frames, uint8_t white_frames, uint8_t pll) {
    const uint8_t sb = lut_black_frames, sw = lut_white_frames, sp = pll_frame_rate;
    lut_black_frames = black_frames;
    lut_white_frames = white_frames;
    pll_frame_rate   = pll;
    _fastRefresh();
    lut_black_frames = sb;
    lut_white_frames = sw;
    pll_frame_rate   = sp;
  }

  /* Put the controller back on factory waveforms and the default PLL rate.
   * Called before every full refresh. */
  void restoreStockWaveform() {
    _writeCommand(0x00);
    _writeData(0x1f); /* PSR, REG=0: LUT from OTP */
    _writeData(0x0d);
    _writeCommand(0x30);
    _writeData(0x09); /* 50 Hz */
    _init_display_done = false;
  }

 private:
  void _stockRefresh(bool partial_update_mode) {
    restoreStockWaveform();
    uint32_t t0 = micros();
    _applyStockPll();
    GxEPD2_310_GDEQ031T10::refresh(partial_update_mode);
    last_refresh_us = micros() - t0;
  }

  /* Raise the PLL on the factory waveform.
   *
   * MEASURED RESULT: this does NOT work. With EPD_STOCK_PLL_RATE 0x13 (100 Hz)
   * the partial refresh still takes 700 ms, exactly the unaccelerated time.
   *
   * The reason is that the OTP temperature-range banks carry their OWN frame
   * rate: UC8253c_A0.6 "LUT FORMAT IN OTP" shows address 0x002A holding
   * Frame Rate[4:0] per TR, alongside that bank's voltages and LUTs. When the
   * controller selects an OTP bank it loads that bank's frame rate, overwriting
   * whatever was written to R30H. So the PLL is only honoured while the panel
   * is on a REGISTER LUT (PSR REG=1) — exactly the configuration where the
   * frame rate visibly scaled during the bench sweep.
   *
   * Kept, and still written before DRF, so that raising it has an effect if the
   * register-LUT path is ever enabled. On the stock path it is inert. */
  void _applyStockPll() {
#if EPD_STOCK_PLL_RATE
    _writeCommand(0x30);
    _writeData(EPD_STOCK_PLL_RATE & 0x1f);
#endif
  }

  /* Byte-for-byte copy of the driver's own _setPartialRamArea(), which is
   * private and so unreachable from here. Keep in step with it if GxEPD2 is
   * updated. */
  void _setFastRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t xe = (x + w - 1) | 0x0007; /* byte boundary inclusive */
    uint16_t ye = y + h - 1;
    x &= 0xFFF8;                        /* byte boundary */
    _writeCommand(0x90);                /* partial window */
    _writeData(x);
    _writeData(xe);
    _writeData(y / 256);
    _writeData(y % 256);
    _writeData(ye / 256);
    _writeData(ye % 256);
    _writeData(0x01);
  }

  void _fastRefresh() {
    uint32_t t0 = micros();
    timed_out = false;

    /* PSR must be rewritten every time: writeImagePart() can go through
     * _InitDisplay(), which rewrites PSR as 0x1f and clears the REG bit.
     * RES=00, REG=1, KW/R=1 (mono), UD=1, SHL=1, SHD_N=1, RST_N=1 -> 0x3f. */
    _writeCommand(0x00);
    _writeData(0x3f);
    _writeData(0x0d);

    _writeCommand(0x30); /* PLL: the speed lever */
    _writeData(pll_frame_rate & 0x1f);

    /* LUT OPTION (R2AH). EOPT (D7 of the first data byte) is the master enable
     * for the LUT sequence, and its power-on default is 0 = DISABLED.
     *
     * This is almost certainly why the register LUT never rendered: the panel
     * accepted every LUT write, then ran a fixed short default sequence instead
     * of ours. The evidence is that refresh time did not track the frame count
     * at all — 12 frames at 120 Hz should take 100 ms, but measured 38 ms
     * (~4 frames), the same as 16 and 63 frames.
     *
     * Bytes: [EOPT|ESO|...], STATE_XON[7:0], STATE_XON[15:8], GROUP_KWE[7:0],
     * [.....|ATRED|NORED]. GROUP_KWE 0xFF = all groups execute sequentially. */
    _writeCommand(0x2A);
    _writeData(0x80); /* EOPT = 1: enable the LUT sequence */
    _writeData(0x00); /* STATE_XON[7:0]  : no all-gate-on */
    _writeData(0x00); /* STATE_XON[15:8] */
    _writeData(0xFF); /* GROUP_KWE       : all groups execute */
    _writeData(0x00); /* ATRED/NORED     : KWR-only, unused in KW mode */

    _writeLuts();

    _writeCommand(0x50); /* VCOM / data interval */
    _writeData(0xD7);
    _fastPowerOn();
    _writeCommand(0x12); /* DRF */
    _fastWaitBusy();
    last_refresh_us = micros() - t0;
    _writeCommand(0x92); /* partial out */

    /* Match the stock _Update_Full()/_Update_Part(), which both end with this
     * ("needed, reason unknown" per the GxEPD2 author): after a DRF the UC8253
     * must go through _InitDisplay() again before it accepts image RAM writes.
     * Without it, the writeImagePartAgain() that keeps the PREVIOUS-image
     * buffer (0x10) in step with the glass is silently ignored, the old buffer
     * stays at its boot state (all white), and every black pixel on the glass
     * reads as "white->white, hold" — the UI freezes in place, accumulating
     * ink until reboot. The next _fastRefresh() rewrites PSR/PLL/LUTs anyway,
     * so the re-init costs nothing on the fast path. */
    _init_display_done = false;
  }

  /* Group counts are NOT uniform. Per UC8253c_A0.6 byte counts:
   * LUTC 57 (8 groups), LUTWW 43 (6), LUTKW/LUTWK/LUTKK 57 (8 each).
   * Those counts INCLUDE the command byte: 1 + groups*7. A short write
   * desynchronizes the SPI stream and corrupts the LUTs. */
  /* WHICH LUT DRIVES WHICH PIXEL depends on CDI's DDX[1:0] (R50H). With the
   * 0xD7 this driver writes, DDX = 01 (the default), and the KW-with-NEW/OLD
   * table in UC8253c_A0.6 p.29 maps {NEW, OLD} -> LUT as:
   *
   *     00 -> LUTKK    01 -> LUTWK    10 -> LUTKW    11 -> LUTWW
   *
   * With data bit 1 = white (this driver's convention, unchanged from the
   * stock path), that is exactly the NAIVE reading of the register names:
   * LUTWW = white->white, LUTKW = K2W = black->white, LUTWK = W2K =
   * white->black, LUTKK = black->black. (An earlier comment here claimed the
   * mapping was crossed; the datasheet table says otherwise.)
   *
   * Levels are the [D7:D6] selector: 00 = 0 V, 01 = VSH, 10 = VSL, 11 = VDHR.
   * POLARITY: the panel datasheet gives VSH = positive source (+16 V), VSL =
   * negative (-16 V). Black particles are the positively charged ones and the
   * viewing side is the VCOM plane, so a positive pixel electrode pushes black
   * to the viewing surface: VSH (0x40) drives BLACK, VSL (0x80) drives WHITE.
   * If the image renders inverted anyway, swap the 0x80/0x40 below. */
  void _writeLuts() {
    /* Hold (0 V) on the no-change transitions is deliberate: an unchanged
     * pixel needs no push, and driving it would waste the tiny frame budget
     * and add flicker.
     *
     * Frame counts are PER DIRECTION: the DRF lasts as long as the longest
     * LUT, so black can be driven longer than white without slowing white
     * down — white pixels just finish and hold. VCOM and the hold LUTs get
     * the max so they stay defined for the whole refresh. */
    const uint8_t fb = (uint8_t)(lut_black_frames & 0x3f);
    const uint8_t fw = (uint8_t)(lut_white_frames & 0x3f);
    const uint8_t fmax = fb > fw ? fb : fw;
    _writeLut(0x20, 8, 0x00, fmax); /* LUTC  : VCOM held at VCOM_DC */
    _writeLut(0x21, 6, 0x00, fmax); /* LUTWW : white -> white, hold */
    _writeLut(0x22, 8, 0x80, fw);   /* LUTKW : black -> white, drive VSL */
    _writeLut(0x23, 8, 0x40, fb);   /* LUTWK : white -> black, drive VSH */
    _writeLut(0x24, 8, 0x00, fmax); /* LUTKK : black -> black, hold */
  }

  /* One active group then zero groups.
   *
   * Group layout per UC8253c_A0.6 (R20H..R24H, 7 bytes per group; a group is
   * 2 states of 2 phases each):
   *
   *   [0] group repeat times      (0x00 = NO REPEAT: the group NEVER RUNS)
   *   [1] level[1:0]<<6 | frames[5:0]   state 1, phase 1
   *   [2] level<<6 | frames             state 1, phase 2
   *   [3] level<<6 | frames             state 2, phase 1
   *   [4] level<<6 | frames             state 2, phase 2
   *   [5] state 1 repeat times    (0x00 = state never runs)
   *   [6] state 2 repeat times
   *
   * HISTORY OF THE BLANK PANEL, so nobody repeats it: the original encoding had
   * this layout right but wrote 0x00 for BOTH repeat counts — a group repeated
   * zero times is skipped, so the waveform drove nothing and the glass stayed
   * white while BUSY behaved normally. (An intermediate "fix" put the level in
   * byte [0] instead — 0x80 there means repeat 128x, which blew the 3 s BUSY
   * timeout.) Byte [0] and byte [5] must both be nonzero for phase 1-1 to run. */
  void _writeLut(uint8_t command, uint8_t groups, uint8_t level, uint8_t frames) {
    _writeCommand(command);
    _startTransfer();
    _transfer(0x01);                                     /* group: run once */
    _transfer((uint8_t)(level | (frames & 0x3f)));       /* state1 phase1 */
    _transfer(0x00);                                     /* state1 phase2 */
    _transfer(0x00);                                     /* state2 phase1 */
    _transfer(0x00);                                     /* state2 phase2 */
    _transfer(0x01);                                     /* state1: run once */
    _transfer(0x00);                                     /* state2: unused */
    for (uint8_t g = 1; g < groups; g++)
      for (int i = 0; i < 7; i++) _transfer(0x00);
    _endTransfer();
  }

  void _fastPowerOn() {
    if (!_power_is_on) {
      _writeCommand(0x04);
      _fastWaitBusy();
    }
    _power_is_on = true;
  }

  /* Runs the busy callback (touch polling) like GxEPD2 does, so a 20 ms refresh
   * still cannot swallow a tap. */
  void _fastWaitBusy() {
    const uint32_t timeout_us = 3000000UL;
    uint32_t start = micros();
    delayMicroseconds(200);
    while (digitalRead(_busy) == _busy_level) {
      if (_busy_callback) _busy_callback(_busy_callback_parameter);
      if (micros() - start > timeout_us) { timed_out = true; break; }
      yield();
    }
  }
};

static GxEPD2_BW<EpdFastGDEQ031T10, EpdFastGDEQ031T10::HEIGHT> s_epd(
    EpdFastGDEQ031T10(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

/* ---- 1bpp shadow buffer --------------------------------------------------
 * The panel's native format: 1 bit per pixel, MSB-first, 8 pixels per byte,
 * row-major, stride = 240/8 = 30 bytes. 30 * 320 = 9600 bytes. Small enough to
 * live in internal RAM, which keeps epd_blit_tile() off the PSRAM bus.
 *
 * Convention here matches the panel: bit SET = WHITE, bit CLEAR = BLACK. */
#define EPD_STRIDE (LCD_WIDTH / 8)        /* 30 bytes */
#define EPD_BUF_BYTES (EPD_STRIDE * LCD_HEIGHT)

static uint8_t s_epd_fb[EPD_BUF_BYTES];

/* Dirty bounding box, in panel coordinates. Valid only while s_epd_dirty. */
static bool     s_epd_dirty = false;
static int16_t  s_dx1 = 0, s_dy1 = 0, s_dx2 = 0, s_dy2 = 0;

static uint32_t s_epd_last_ms   = 0;   /* millis() of the last refresh */
static uint16_t s_epd_partials  = 0;   /* partials since the last full */
static bool     s_epd_ready     = false;
static bool     s_epd_powered   = false;

/* Busy-wait callback — see the strict rules in the header block. */
static void (*s_epd_busy_cb)(void) = nullptr;

static inline void epd_set_busy_callback(void (*cb)(void)) {
  s_epd_busy_cb = cb;
}

/* Interact probe: returns true while the user is mid-interaction (finger on
 * the glass). Gates the continuous-mode settle pass — see EPD_CONT_SETTLE_MS.
 * Unset (nullptr) = no gating, pure timer. */
static bool (*s_epd_interact_cb)(void) = nullptr;

static inline void epd_set_interact_probe(bool (*cb)(void)) {
  s_epd_interact_cb = cb;
}

/* GxEPD2 calls this repeatedly while waiting on the BUSY pin. Its callback type
 * carries an opaque user parameter which we do not use — the callback we
 * install (cst328_poll) is argument-less. */
static void epd_busy_trampoline(const void *) {
  if (s_epd_busy_cb) s_epd_busy_cb();
}

/* ---- Dirty tracking ------------------------------------------------------ */

static inline void epd_dirty_reset() {
  s_epd_dirty = false;
  s_dx1 = s_dy1 = 0;
  s_dx2 = s_dy2 = 0;
}

static inline void epd_dirty_add(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
  if (!s_epd_dirty) {
    s_dx1 = x1; s_dy1 = y1; s_dx2 = x2; s_dy2 = y2;
    s_epd_dirty = true;
    return;
  }
  if (x1 < s_dx1) s_dx1 = x1;
  if (y1 < s_dy1) s_dy1 = y1;
  if (x2 > s_dx2) s_dx2 = x2;
  if (y2 > s_dy2) s_dy2 = y2;
}

/* Mark the whole panel dirty. Used at the end of setup() so the first
 * epd_service() paints the UI even though LVGL considers itself clean. */
static inline void epd_invalidate_all() {
  epd_dirty_add(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
}

/* ---- Bus pre-deselect ----------------------------------------------------
 * Three devices share this SPI host (e-paper CS 34, microSD CS 48, SX1262
 * CS 3). A floating CS lets one device answer another's traffic, so every CS
 * is driven HIGH before the bus is used. LilyGo's own EPD example does exactly
 * this; the .ino calls this first in setup(). Do not remove it. */
static inline void epd_predeselect_bus() {
  pinMode(EPD_CS, OUTPUT);    digitalWrite(EPD_CS, HIGH);
  pinMode(SD_SPI_CS, OUTPUT); digitalWrite(SD_SPI_CS, HIGH);
  pinMode(LORA_CS, OUTPUT);   digitalWrite(LORA_CS, HIGH);
}

#include "esp_heap_caps.h"   /* PSRAM allocs for the continuous-drive state */

/* ---- Continuous-drive engine (see the tunables block up top) --------------
 * Firmware-side waveform state, one byte per pixel:
 *   bit 7    : the color this pixel is being driven TOWARD (1 = white)
 *   bits 5..0: fields of drive received so far (saturates at s_cont_fields)
 * A target change (LVGL drew something new) resets the counter and flips the
 * direction — exactly PaperBoy's state machine, minus his 2-pixel packing.
 * Buffers live in PSRAM (96 KB total); allocated on first enable. */
static uint8_t *s_cont_state = nullptr;   /* [LCD_WIDTH * LCD_HEIGHT] */
static uint8_t *s_cont_old   = nullptr;   /* controller OLD plane, 1bpp */
static uint8_t *s_cont_new   = nullptr;   /* controller NEW plane, 1bpp */
static bool     s_cont_on       = false;
static uint8_t  s_cont_fields_k = EPD_CONT_BLACK_FIELDS;  /* toward black */
static uint8_t  s_cont_fields_w = EPD_CONT_WHITE_FIELDS;  /* toward white */
static uint32_t s_cont_active   = 0;      /* pixels still mid-transition */

/* Settle-pass bookkeeping: bounding box of everything the field engine has
 * driven since the last full-strength refresh, i.e. the region that may carry
 * under-driven (grey/trace) pixels. Valid only while s_cont_touched. */
static bool     s_cont_touched = false;
static int16_t  s_cx1 = 0, s_cy1 = 0, s_cx2 = 0, s_cy2 = 0;
static bool     s_cont_finish_done = false;   /* kick fired for this quiet period */

/* Live tuning knobs: fields of drive per transition DIRECTION (clamped 1..63).
 * Black needs roughly 2x white's total drive on this glass. */
static inline void epd_cont_set_fields(uint8_t black, uint8_t white) {
  s_cont_fields_k = black < 1 ? 1 : (black > 63 ? 63 : black);
  s_cont_fields_w = white < 1 ? 1 : (white > 63 ? 63 : white);
}

/* Declare every pixel "arrived": state color = shadow-buffer color, counter
 * saturated. Called when glass and shadow buffer are KNOWN to match (after a
 * full/stock refresh or clear) so the engine doesn't re-drive the whole screen. */
static void epd_cont_sync_state(void) {
  if (!s_cont_state) return;
  for (int32_t i = 0; i < (int32_t)EPD_BUF_BYTES; i++) {
    const uint8_t tb = s_epd_fb[i];
    uint8_t *st8 = &s_cont_state[(int32_t)i * 8];
    for (int b = 0; b < 8; b++)
      st8[b] = (uint8_t)((((tb >> (7 - b)) & 1u) << 7) | 0x3F); /* counter maxed = done */
  }
  s_cont_active = 0;
  /* A sync means a FULL-STRENGTH refresh just ran (full/clear/settle), so no
   * under-driven pixels remain anywhere: nothing left to settle. */
  s_cont_touched = false;
}

/* The settle pass itself (see EPD_CONT_SETTLE_MS). Called only from the idle
 * branch of epd_cont_service(): no new content, no mid-transition pixels.
 * Pushes the touched window with the STOCK OTP waveform — the same ~650 ms
 * partial the classic pipeline uses — then re-syncs the engine state.
 * LVGL cannot render during the blocking refresh (the busy callback is
 * touch-poll only), so s_epd_fb is stable across it. */
static void epd_cont_settle(void) {
#if EPD_CONT_SETTLE_MS > 0
  if (!s_cont_touched) return;
  /* Never settle mid-interaction: a finger on the glass means more frames are
   * coming, and a ~650 ms stock refresh landing between them is what produced
   * the all-grey panel. The timer below only measures POST-interaction quiet. */
  if (s_epd_interact_cb && s_epd_interact_cb()) return;
  if ((uint32_t)(millis() - s_epd_last_ms) < EPD_CONT_SETTLE_MS) return;

  /* 8-align x like the classic pipeline does (controller RAM is 8-px columns). */
  int16_t x1 = (int16_t)(s_cx1 & ~7), y1 = s_cy1;
  int16_t x2 = (int16_t)(s_cx2 | 7),  y2 = s_cy2;
  if (x2 > LCD_WIDTH - 1) x2 = LCD_WIDTH - 1;
  const int16_t w = x2 - x1 + 1;
  const int16_t h = y2 - y1 + 1;
  if (w <= 0 || h <= 0) { s_cont_touched = false; return; }

  if (!s_epd_powered) {
    s_epd.init(0, false, 10, false);
    s_epd.epd2.setBusyCallback(epd_busy_trampoline);
    s_epd_powered = true;
  }

#if EPD_PROFILE_SERIAL
  const uint32_t t0 = micros();
#endif
  s_epd.epd2.writeImagePart(s_epd_fb, x1, y1, LCD_WIDTH, LCD_HEIGHT,
                            x1, y1, w, h, false, false, false);
  /* fast_mode off for this one refresh -> the windowed refresh takes the stock
   * path (OTP LUT, factory frame count): the guaranteed-complete drive. The
   * preceding writeImagePart re-ran _InitDisplay (the last field left
   * _init_display_done false), so PSR is back on OTP (REG=0) already. */
  const bool fm = s_epd.epd2.fast_mode;
  s_epd.epd2.fast_mode = false;
  s_epd.epd2.refresh(x1, y1, w, h);
  s_epd.epd2.fast_mode = fm;
  /* Keep the controller's previous-image plane in step with the glass, exactly
   * like the classic pipeline — the next field diffs against it. */
  s_epd.epd2.writeImagePartAgain(s_epd_fb, x1, y1, LCD_WIDTH, LCD_HEIGHT,
                                 x1, y1, w, h, false, false, false);
  epd_cont_sync_state();          /* every pixel now truly arrived (+ touched=false) */
  s_epd_last_ms = millis();
#if EPD_PROFILE_SERIAL
  USBSerial.printf("[epd] settle %dx%d @%d,%d  refresh=%lums total=%lums\n",
                   w, h, x1, y1,
                   (unsigned long)(s_epd.epd2.last_refresh_us / 1000),
                   (unsigned long)((micros() - t0) / 1000));
#endif
#endif /* EPD_CONT_SETTLE_MS */
}

/* The finish kick (see EPD_CONT_FINISH_FIELDS): rewind the drive counter of
 * every pixel in the touched window so the engine drives it for up to that many
 * extra fields toward the color it already targets. Runs ONCE per quiet period,
 * the moment the engine idles — the extra fields start on this very service
 * call. Returns true if it rewound anything (caller falls through into the
 * field loop instead of returning). */
static bool epd_cont_finish_kick(void) {
#if EPD_CONT_FINISH_FIELDS > 0
  if (!s_cont_touched || s_cont_finish_done) return false;
  s_cont_finish_done = true;
  for (int16_t y = s_cy1; y <= s_cy2; y++) {
    uint8_t *st = &s_cont_state[(int32_t)y * LCD_WIDTH + s_cx1];
    for (int16_t x = s_cx1; x <= s_cx2; x++, st++) {
      const uint8_t col    = (uint8_t)(*st >> 7);
      const uint8_t cnt    = (uint8_t)(*st & 0x3F);
      const uint8_t budget = col ? s_cont_fields_w : s_cont_fields_k;
      const uint8_t floor_ = budget > EPD_CONT_FINISH_FIELDS
                                 ? (uint8_t)(budget - EPD_CONT_FINISH_FIELDS) : 0;
      if (cnt > floor_) *st = (uint8_t)((col << 7) | floor_);
    }
  }
  s_cont_active = 1;   /* provisional: the field loop recounts for real */
  return true;
#else
  return false;
#endif
}

/* One field: advance every pixel's state machine, build the two lie-planes,
 * hand them to the controller, run one short LUT pass. Blocks for the field
 * duration (~(frames+2)/PLL + DRF overhead); the busy callback keeps touch
 * polled inside it, same as every other refresh in this file. */
static void epd_cont_service(void) {
  if (!s_epd_dirty && s_cont_active == 0) {         /* idle: fields cost nothing */
    if (!epd_cont_finish_kick()) {                  /* first: immediate top-up */
      epd_cont_settle();                            /* optional deep clean */
      return;
    }
    /* Kick rewound counters — fall through and drive them THIS field. */
  }
  if (s_epd_dirty) {
    s_cont_finish_done = false;   /* new content: the quiet period ended */
    /* Fold the new content into the settle window before consuming it. */
    if (!s_cont_touched) {
      s_cx1 = s_dx1; s_cy1 = s_dy1; s_cx2 = s_dx2; s_cy2 = s_dy2;
      s_cont_touched = true;
    } else {
      if (s_dx1 < s_cx1) s_cx1 = s_dx1;
      if (s_dy1 < s_cy1) s_cy1 = s_dy1;
      if (s_dx2 > s_cx2) s_cx2 = s_dx2;
      if (s_dy2 > s_cy2) s_cy2 = s_dy2;
    }
  }
  epd_dirty_reset();                                /* fb consumed this field */

  const uint8_t fields_k = s_cont_fields_k;   /* toward black */
  const uint8_t fields_w = s_cont_fields_w;   /* toward white */
  uint32_t active = 0;

  for (int32_t i = 0; i < (int32_t)EPD_BUF_BYTES; i++) {
    const uint8_t tb = s_epd_fb[i];
    uint8_t *st8 = &s_cont_state[(int32_t)i * 8];
    uint8_t ob = 0, nb = 0;
    for (int b = 0; b < 8; b++) {
      const uint8_t mask = (uint8_t)(0x80 >> b);
      const uint8_t t   = (tb & mask) ? 1 : 0;
      uint8_t col = st8[b] >> 7;
      uint8_t cnt = st8[b] & 0x3F;
      if (t != col) { col = t; cnt = 0; }           /* new target: restart drive */
      const uint8_t fields = t ? fields_w : fields_k;  /* per-direction budget */
      if (cnt < fields) {
        cnt++; active++;
        /* DRIVE: old = source color, new = target -> the differential LUT
         * pushes one more field in the target direction. */
        if (t) nb |= mask; else ob |= mask;
      } else {
        /* DONE: old == new -> hold LUT, zero drive, no flicker. */
        if (t) { ob |= mask; nb |= mask; }
      }
      st8[b] = (uint8_t)((col << 7) | cnt);
    }
    s_cont_old[i] = ob;
    s_cont_new[i] = nb;
  }
  s_cont_active = active;
  if (active == 0) return;      /* everything already arrived: skip the field */

  if (!s_epd_powered) {         /* woken from powerOff: re-init like epd_service */
    s_epd.init(0, false, 10, false);
    s_epd.epd2.setBusyCallback(epd_busy_trampoline);
    s_epd_powered = true;
  }

  /* writeImagePart -> controller NEW plane (0x13); writeImagePartAgain -> OLD
   * plane (0x10). Full-screen both, then one short field. */
  s_epd.epd2.writeImagePart(s_cont_new, 0, 0, LCD_WIDTH, LCD_HEIGHT,
                            0, 0, LCD_WIDTH, LCD_HEIGHT, false, false, false);
  s_epd.epd2.writeImagePartAgain(s_cont_old, 0, 0, LCD_WIDTH, LCD_HEIGHT,
                                 0, 0, LCD_WIDTH, LCD_HEIGHT, false, false, false);
  s_epd.epd2.continuousField(EPD_CONT_BLACK_FRAMES, EPD_CONT_WHITE_FRAMES,
                             EPD_CONT_PLL);
  s_epd_last_ms = millis();

#if EPD_PROFILE_SERIAL
  /* Rate-limited: one line per second with the achieved field cadence. */
  static uint32_t s_prof_t0 = 0, s_prof_n = 0;
  s_prof_n++;
  const uint32_t now_ms = millis();
  if ((uint32_t)(now_ms - s_prof_t0) >= 1000) {
    USBSerial.printf("[epd] cont: %lu fields/s  field=%lums  active=%lu  Xk=%u Xw=%u\n",
                     (unsigned long)s_prof_n,
                     (unsigned long)(s_epd.epd2.last_refresh_us / 1000),
                     (unsigned long)s_cont_active,
                     (unsigned)s_cont_fields_k, (unsigned)s_cont_fields_w);
    s_prof_t0 = now_ms;
    s_prof_n = 0;
  }
#endif
}

/* Enter/leave continuous mode at runtime. Enabling allocates the PSRAM state
 * on first use and assumes glass == shadow buffer (true whenever the normal
 * pipeline is idle, which it is between epd_service calls). Disabling falls
 * back to the ordinary partial-refresh pipeline and repaints once so any
 * half-driven (grey) pixels get a clean refresh. */
static bool epd_continuous_set(bool on) {
  if (!on) {
    if (s_cont_on) { s_cont_on = false; epd_invalidate_all(); }
    return true;
  }
  if (!s_cont_state) {
    s_cont_state = (uint8_t *)heap_caps_malloc(EPD_BUF_BYTES * 8,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cont_old   = (uint8_t *)heap_caps_malloc(EPD_BUF_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cont_new   = (uint8_t *)heap_caps_malloc(EPD_BUF_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cont_state || !s_cont_old || !s_cont_new) {
      free(s_cont_state); free(s_cont_old); free(s_cont_new);
      s_cont_state = s_cont_old = s_cont_new = nullptr;
      return false;
    }
  }
  epd_cont_sync_state();
  s_cont_on = true;
  return true;
}

/* ---- Bring-up ------------------------------------------------------------ */

static bool epd_begin() {
  SPI.begin(EPD_SCLK, EPD_MISO, EPD_MOSI, EPD_CS);
  s_epd.epd2.selectSPI(SPI, SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));

  /* init(serial_diag, initial, reset_duration, pulldown_rst_mode).
   * initial=true forces the first update to be a full one, which is correct:
   * we do not know what the glass is showing at power-on. */
  s_epd.init(0 /* no serial diag */, true /* initial */, 10, false);
  s_epd.epd2.setBusyCallback(epd_busy_trampoline);

  /* Shadow buffer starts white, matching the clear below, so the first blit
   * diffs against a known state. 0xFF = all bits set = all white. */
  memset(s_epd_fb, 0xFF, sizeof(s_epd_fb));

  s_epd.clearScreen(0xFF);   /* one full clear so the glass is in a known state */

  s_epd_ready    = true;
  s_epd_powered  = true;
  s_epd_partials = 0;
  s_epd_last_ms  = millis();

  /* Deliberately reset AFTER the clear: nothing is pending, the glass matches
   * the buffer. setup() calls epd_invalidate_all() later to paint the real UI —
   * see the long comment at that call site for why that is load-bearing. */
  epd_dirty_reset();

#if EPD_CONTINUOUS_BOOT
  /* Glass and shadow buffer were just synced (both all-white), which is the
   * precondition epd_continuous_set() assumes. */
  epd_continuous_set(true);
#endif
  return true;
}

/* ---- RGB565 tile -> 1bpp shadow buffer ----------------------------------
 * Called from my_disp_flush() for every dirty tile. Pure memory; no SPI.
 *
 * The threshold is luminance against mid-grey. Green is weighted heaviest
 * (the standard ~2:5:1 split, done in integer arithmetic on the already
 * unpacked 5/6/5 fields) because the UI uses coloured accents that must not
 * all collapse to the same ink value.
 *
 * BOARD_EPD_INVERT flips the polarity. The UI is authored light-on-dark; on a
 * reflective panel that would flood the glass with black — slow, and it throws
 * away the paper-like readability that is the point of e-paper. Inverting maps
 * the UI's dark background to white paper and its light text to black ink. */
static void epd_blit_tile(const lv_area_t *area, uint8_t *px_map) {
  const uint16_t *src = (const uint16_t *)px_map;

  int16_t x1 = area->x1, y1 = area->y1;
  int16_t x2 = area->x2, y2 = area->y2;

  /* Clip to the panel. LVGL should never exceed it, but a tile that did would
   * corrupt memory outside s_epd_fb rather than merely look wrong. */
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > LCD_WIDTH  - 1) x2 = LCD_WIDTH  - 1;
  if (y2 > LCD_HEIGHT - 1) y2 = LCD_HEIGHT - 1;
  if (x2 < x1 || y2 < y1) return;

  const int32_t tile_w = (int32_t)(area->x2 - area->x1 + 1);  /* source stride */

  for (int16_t y = y1; y <= y2; y++) {
    const uint16_t *row = src + (int32_t)(y - area->y1) * tile_w + (x1 - area->x1);
    uint8_t *dst_row = s_epd_fb + (int32_t)y * EPD_STRIDE;

    for (int16_t x = x1; x <= x2; x++) {
      const uint16_t c = *row++;

      /* Unpack RGB565 and scale each field to 0..255. */
      const uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
      const uint8_t g = (uint8_t)(((c >>  5) & 0x3F) * 255 / 63);
      const uint8_t b = (uint8_t)(( c        & 0x1F) * 255 / 31);

      /* Integer luminance, ~ (2r + 5g + b) / 8. */
      const uint16_t lum = (uint16_t)((2 * r + 5 * g + b) >> 3);

      bool white = (lum >= 128);
#if BOARD_EPD_INVERT
      white = !white;
#endif

      uint8_t *cell = dst_row + (x >> 3);
      const uint8_t mask = (uint8_t)(0x80 >> (x & 7));
      if (white) *cell |= mask;      /* bit set   = white */
      else       *cell &= ~mask;     /* bit clear = black */
    }
  }

  epd_dirty_add(x1, y1, x2, y2);
}

/* ---- Power --------------------------------------------------------------
 * powerOff() only stops the panel driving voltages; the IMAGE REMAINS on the
 * glass with zero power. That is why the deep-sleep path leaves the watch face
 * up instead of blanking. hibernate() is not used: it needs a reset line to
 * wake, and this board has none (EPD_RST is -1). */
static inline void epd_power_off() {
  if (!s_epd_ready || !s_epd_powered) return;
  s_epd.powerOff();
  s_epd_powered = false;
}

/* Clear the glass to white. Used on the critical-battery shutdown path, where
 * a stale UI would read as a hung device. Costs one full refresh. */
static inline void epd_clear_white() {
  if (!s_epd_ready) return;
  memset(s_epd_fb, 0xFF, sizeof(s_epd_fb));
  s_epd.clearScreen(0xFF);
  s_epd_powered  = true;
  s_epd_partials = 0;
  s_epd_last_ms  = millis();
  epd_dirty_reset();
  if (s_cont_on) epd_cont_sync_state();   /* glass == buffer again */
}

/* ---- The refresh itself --------------------------------------------------
 * Called every loop iteration. Returns immediately when there is nothing to do,
 * so it is cheap and safe to call unconditionally.
 *
 * force_full: used by the sleep path — the SoC is about to stop, so this is the
 * last chance to render, and a full refresh leaves the glass ghost-free for what
 * may be a long time. It also bypasses the rate limit, since there is no "next
 * iteration" coming to retry. */
static void epd_service(bool force_full = false) {
  if (!s_epd_ready) return;
  /* Continuous mode replaces the whole partial-refresh pipeline below with the
   * field engine. force_full still takes the classic path (sleep/shutdown wants
   * one clean ghost-free stock refresh, not a stream of fields) — the state
   * resync at the bottom of this function keeps the engine consistent after it. */
  if (s_cont_on && !force_full) { epd_cont_service(); return; }
  if (!s_epd_dirty) return;

  const uint32_t now = millis();
  if (!force_full && (uint32_t)(now - s_epd_last_ms) < EPD_MIN_REFRESH_MS) return;

  /* Full when forced, on the ghosting schedule, or when the dirty box is large
   * enough that a partial saves nothing. */
  bool full = force_full;
#if EPD_FULL_EVERY > 0
  if (s_epd_partials >= EPD_FULL_EVERY) full = true;
#endif

  /* Snapshot and clear the dirty box BEFORE the refresh. The refresh blocks for
   * the better part of a second and runs the busy callback inside it; clearing
   * first means anything that dirties the screen during that window is recorded
   * for the NEXT service call rather than being silently dropped. */
  int16_t x1 = s_dx1, y1 = s_dy1, x2 = s_dx2, y2 = s_dy2;
  epd_dirty_reset();

  if (full) { x1 = 0; y1 = 0; x2 = LCD_WIDTH - 1; y2 = LCD_HEIGHT - 1; }

  /* Expand the window to byte boundaries in x. The controller addresses RAM in
   * 8-pixel columns, so a partial window must be 8-aligned. GxEPD2 aligns the
   * refresh window itself, but writeImage() needs x and w already aligned or it
   * would shift the image sideways. */
  x1 &= ~7;
  x2 |= 7;
  if (x2 > LCD_WIDTH - 1) x2 = LCD_WIDTH - 1;

  const int16_t w = x2 - x1 + 1;
  const int16_t h = y2 - y1 + 1;

#if EPD_PROFILE_SERIAL
  const uint32_t t0 = micros();
#endif

  if (!s_epd_powered) {
    /* Woken from a powerOff(). The controller needs its init sequence again
     * before it will accept image data. */
    s_epd.init(0, false /* not initial: the glass content is known */, 10, false);
    s_epd.epd2.setBusyCallback(epd_busy_trampoline);
    s_epd_powered = true;
  }

  /* Push the sub-rectangle out of the shadow buffer. writeImagePart() takes the
   * source as a full-width bitmap and crops, which is exactly our layout: the
   * shadow buffer is always the full 240x320, and we send a window of it.
   *
   * invert=false — the polarity was already applied per-pixel in epd_blit_tile()
   * (BOARD_EPD_INVERT), and doing it here as well would cancel it out. */
  s_epd.epd2.writeImagePart(s_epd_fb,
                            x1, y1,                    /* x_part, y_part */
                            LCD_WIDTH, LCD_HEIGHT,     /* w_bitmap, h_bitmap */
                            x1, y1, w, h,              /* destination window */
                            false /* invert */, false /* mirror_y */, false /* pgm */);

#if EPD_PROFILE_SERIAL
  const uint32_t t_write = micros();
#endif

  if (full) {
    s_epd.epd2.refresh(false /* full */);
    s_epd_partials = 0;
  } else {
    s_epd.epd2.refresh(x1, y1, w, h);
    s_epd_partials++;
  }

  /* Keep the controller's PREVIOUS-image buffer in step with what is now on the
   * glass. The UC8253 drives a DIFFERENTIAL waveform: it transitions from the
   * previous buffer to the current one. Without this, the next partial refresh
   * would diff against a stale previous image and leave the old content
   * smeared into the new — the classic "text from two frames ago is still
   * faintly there" artefact, which no amount of TSSET tuning fixes. */
  s_epd.epd2.writeImagePartAgain(s_epd_fb,
                                 x1, y1,
                                 LCD_WIDTH, LCD_HEIGHT,
                                 x1, y1, w, h,
                                 false, false, false);

  s_epd_last_ms = millis();

  /* A stock refresh ran while continuous mode is on (force_full path): glass
   * now matches the shadow buffer, so mark every engine pixel as arrived. */
  if (s_cont_on) epd_cont_sync_state();

#if EPD_PROFILE_SERIAL
  const uint32_t t_end = micros();
  /* `wait` is REAL PANEL TIME (GxEPD2 blocks on the BUSY pin, not a fixed
   * delay). It is the first number to read when debugging — see the header. */
  /* Expected fast-path duration from the LUT: frames / rate. If the measured
   * wait does not track this, the panel is IGNORING our LUT and running its own
   * default sequence — which shows up as a blank screen. */
  const bool fast_now = (!full && s_epd.epd2.fast_mode);
  static const uint16_t k_frs_hz[32] = {
    5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,
    85,90,95,100,105,110,115,120,130,140,150,160,170,180,190,200
  };
  const uint16_t hz = k_frs_hz[s_epd.epd2.pll_frame_rate & 0x1f];
  /* The refresh lasts as long as the LONGEST LUT — black and white counts are
   * independent, so expect from the max of the two, PLUS the 2 discharge
   * frames the controller always appends (UC8253c_A0.6 "Display Refresh
   * Waveform": after the LUT completes "the driver will send 2 frames VCOM
   * and data to 0 V"). The remaining measured excess over expect is DRF
   * housekeeping (booster/VCOM settle, CDI interval, oscillator tolerance)
   * plus BUSY-release detection latency through the touch-poll callback. */
  const unsigned fr_k = (unsigned)(s_epd.epd2.lut_black_frames & 0x3f);
  const unsigned fr_w = (unsigned)(s_epd.epd2.lut_white_frames & 0x3f);
  const unsigned fr_max = fr_k > fr_w ? fr_k : fr_w;
  const unsigned expect_ms = hz ? (unsigned)(((fr_max + 2) * 1000u) / hz) : 0;

  /* refresh = pure panel time (measured inside the refresh call around the
   * BUSY wait). sync = the old-buffer writeImagePartAgain() + its re-init,
   * which the t_write..t_end bracket also contains. */
  const unsigned long refresh_ms = (unsigned long)(s_epd.epd2.last_refresh_us / 1000);
  const unsigned long bracket_ms = (unsigned long)((t_end - t_write) / 1000);
  const unsigned long sync_ms = bracket_ms > refresh_ms ? bracket_ms - refresh_ms : 0;

  USBSerial.printf("[epd] %s %dx%d @%d,%d  write=%lums refresh=%lums sync=%lums total=%lums"
                   "  [%s pll=0x%02X/%uHz frK=%u frW=%u expect=%ums]\n",
                   full ? "FULL" : "part", w, h, x1, y1,
                   (unsigned long)((t_write - t0) / 1000),
                   refresh_ms, sync_ms,
                   (unsigned long)((t_end - t0) / 1000),
                   fast_now ? "fast" : "stock",
                   s_epd.epd2.pll_frame_rate, (unsigned)hz,
                   fr_k, fr_w,
                   fast_now ? expect_ms : 0u);
#endif
}
