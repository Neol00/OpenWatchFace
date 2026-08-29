/* ============================================================================
 *  tuya/compat/owf_tuya_psram_freq.h — PSRAM overclock on the T5 (BK7258).
 *
 *  WHY: our LVGL draw buffers live in PSRAM and the SW renderer does a per-pixel
 *  read-modify-write (blend/AA/text). Measured render cost is ~75 CPU cycles/pixel —
 *  far above a fill's true cost — i.e. the renderer is bound by PSRAM round-trip
 *  latency, not compute. The SDK boots PSRAM at 120 MHz; raising it is the biggest
 *  remaining render lever.
 *
 *  HOW (mirrors the CPU DVFS in owf_tuya_cpu_freq.h — voltage BEFORE clock):
 *  decoded from the prebuilt HAL (psram_hal.c.obj / sys_hal.c.obj). The bk_psram_set_clk enum
 *  maps to (clk_sel @0x44010024 bit5, clkdiv @bit4) — CONFIRMED by reading the jump table:
 *      PSRAM_240M -> clk_sel=0, div=0     PSRAM_120M -> clk_sel=0, div=1
 *      PSRAM_160M -> clk_sel=1, div=0     PSRAM_80M  -> clk_sel=1, div=1
 *  So there are TWO clock sources, each with a /1,/2 divider (div=1 halves):
 *      source0 ≈ 240 MHz : 240M=÷1, 120M=÷2   <- the BOOT clock (120M) already runs source0!
 *      source1 ≈ 160 MHz : 160M=÷1,  80M=÷2
 *  KEY CONSEQUENCE: 240 MHz is just source0 UNDIVIDED — the SAME source the chip already runs
 *  stably at 120M (÷2). So the source/PLL is ALREADY UP; 240M is purely a signal-integrity /
 *  voltage-margin problem at double the rate, which a higher PSRAM LDO voltage can fix. No
 *  DPLL bring-up is needed (an earlier version wrongly enabled it — removed).
 *
 *    - LDO voltage : bk_psram_set_voltage(psram_voltage_t) -> psldo_vset(swb @bit28, vsel @
 *                bits29-31). Decoded rungs: (swb1,vsel1)=2.0V (swb1,vsel2)=1.95V[boot]
 *                (swb1,vsel3)=1.90V (swb1,vsel0)=3.0V (swb0,vsel0)=3.20V. There is NO smooth
 *                2.x step — above 2.0V the next enum rung is 3.0V. 160M @ 2.0V is stable;
 *                240M needs more, so we use 3.0V (the next available rung).
 *
 *  SEQUENCE for 240 MHz: 1) bk_psram_set_voltage(3.0V)  2) bk_psram_set_clk(PSRAM_240M).
 *
 *  SAFETY: the WHOLE system heap is in PSRAM (CONFIG_PSRAM_AS_SYS_MEMORY) — a bad setting
 *  corrupts everything, recoverable only by reflash (flash is a separate chip on this board).
 *  3.0V is a high rail for PSRAM — verify the real clock with the crystal-baseline method,
 *  stress-test PSRAM read/write UNDER LOAD, and watch current/heat before trusting it.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_TUYA

extern "C" {
#include "driver/psram.h"     // bk_psram_set_clk, bk_psram_set_voltage, psram_clk_t, psram_voltage_t
/* bk_delay_us: a busy-wait microsecond delay (exported in libbk_system.a, but its header isn't
 * on the sketch include path). Used for a settle delay after the voltage change. */
void bk_delay_us(uint32_t us);
}

/* Master switch. 0 = decode/record only, NO hardware change (safe to flash). 1 = actually
 * apply. Keep 0 until the sequence is verified on a recoverable build. */
#ifndef OWF_PSRAM_FREQ_APPLY
#define OWF_PSRAM_FREQ_APPLY 1
#endif

/* Target PSRAM clock. PSRAM_160M (source1/÷1, ~160MHz) is stable at 2.0V (+10fps, verified).
 * PSRAM_240M (source0/÷1, ~240MHz) is the goal — the SAME source as the stable 120M boot clock,
 * just undivided — and needs the higher rail below. */
/* SETTLED: 160MHz @ 2.0V — rock-stable, +10fps over the 120M boot clock. 240M (source0 ÷1)
 * boots and renders but CRASHES under sustained load even at 3.0V; this RAM can't do 240
 * reliably, and the HW has no clock point between 160 and 240 (clk_sel+clkdiv are both 1-bit
 * -> only 80/120/160/240 exist). 3.2V would be the only thing left to try and isn't worth it.
 * So 160M @ 2.0V is the chosen operating point. */
#ifndef OWF_PSRAM_CLK
#define OWF_PSRAM_CLK PSRAM_120M
#endif

/* PSRAM LDO voltage. 2.0V is the verified-stable rail for 160M. (Boot is 1.95V; 2.0V is one
 * fine step up. The next rung above 2.0V jumps to 3.0V — only needed for the unstable 240M.) */
#ifndef OWF_PSRAM_VOLT
#define OWF_PSRAM_VOLT PSRAM_OUT_1_80V
#endif

/* ============================================================================================
 *  DEFERRED, SRAM-SAFE PSRAM RECLOCK — the fix for the cold-boot green-line total freeze.
 *
 *  ROOT CAUSE (confirmed on-device): on this build the app is XIP'd FROM PSRAM (8 MB flash app
 *  copied into 8 MB PSRAM; CONFIG_PSRAM_AS_SYS_MEMORY), so the CPU fetches INSTRUCTIONS and the
 *  whole heap from the very PSRAM whose clock divider we flip. Flipping ckdiv_psram (80->160 =
 *  source0 sel=0, div 1->0 — a single clock-source-unchanged divider halving) while the CPU or a
 *  QSPI DMA is mid-transaction to PSRAM corrupts that fetch -> the machine wedges hard (serial
 *  stops too — "everything dies at once"). The green lines are just the last DMA before the hang.
 *  Voltage is NOT the issue (proven: 1.8V and 1.9V behave identically); it's a coherency race at
 *  the divider edge.
 *
 *  THE FIX (what actually worked): DEFER the switch, but do it with the VENDOR'S OWN call.
 *   1) DON'T flip at boot. Leave the vendor's stock 80 MHz through all of bring-up (panel, buffers,
 *      first frames). owf_tuya_psram_overclock() is now a no-op-at-boot stub; the real switch is
 *      deferred to owf_tuya_psram_overclock_deferred(), called ONCE from loop() after first paint.
 *   2) When we DO switch, PARK the QSPI flush task first (so no DMA is reading PSRAM), then call
 *      bk_psram_set_clk(OWF_PSRAM_CLK) — the SDK's COMPLETE switch (clk_sel + clkdiv, in order).
 *
 *  HISTORY / WARNING: an earlier version tried to be clever — a hand-rolled DIRECT write of ONLY
 *  the ckdiv_psram register field from an .itcm_sec_code (SRAM) routine with IRQs off. That CRASHED
 *  CONSISTENTLY every switch, because bk_psram_set_clk is a TWO-register op (clk_sel THEN clkdiv)
 *  and writing only the divider leaves an invalid clock config. The SRAM/IRQ apparatus was solving
 *  a problem the vendor call doesn't have — bk_psram_set_clk plainly works (it's what set the clock
 *  when warm). So: the ONLY change from the original working call is WHEN it runs (deferred +
 *  parked), NOT HOW. Do not reintroduce a partial register write.
 * ============================================================================================ */

/* 1 = defer the flip to first-paint (the safe path, default). 0 = old inline-at-boot behavior
 * (kept only as an escape hatch / for A-B testing the freeze). */
#ifndef OWF_PSRAM_DEFER_TO_FIRST_DRAW
#define OWF_PSRAM_DEFER_TO_FIRST_DRAW 0
#endif

/* BOOT-time entry: now a deliberate NO-OP when deferring (the whole point — leave stock 80 MHz
 * until first paint). Returns false so the bring-up log doesn't claim an overclock was applied.
 * With OWF_PSRAM_DEFER_TO_FIRST_DRAW=0 it falls back to the old inline-at-boot flip. */
static inline bool owf_tuya_psram_overclock(void) {
#if OWF_PSRAM_FREQ_APPLY && !OWF_PSRAM_DEFER_TO_FIRST_DRAW
  bk_psram_set_voltage(OWF_PSRAM_VOLT);     // 1) raise the rail first
  bk_delay_us(2000);                        //    let the LDO settle
  bk_psram_set_clk(OWF_PSRAM_CLK);          // 2) select the clock
  return true;
#else
  return false;                             // deferred (or record-only): no hardware change at boot
#endif
}

/* Forward decls for the flush-task park/unpark (defined in owf_tuya_co5300_qspi.h). Weak-linked
 * via the header include order; these let the reclock guarantee no DMA is in flight. */
bool owf_t5_panel_park_and_wait_idle(void);   /* park the flush task; true once no DMA in flight */
void owf_t5_panel_unpark(void);               /* resume the flush task */

/* DEFERRED reclock — call ONCE from loop() after the first frame has painted. Quiesces the QSPI
 * flush task (so no DMA reads PSRAM during the edge), optionally raises the rail, does the
 * SRAM-safe divider flip, unparks, then reads back the achieved clock and logs it. Returns true
 * if the flip was applied. Safe to call repeatedly — the loop() one-shot guard calls it once. */
static inline bool owf_tuya_psram_overclock_deferred(void) {
#if OWF_PSRAM_FREQ_APPLY && OWF_PSRAM_DEFER_TO_FIRST_DRAW
  /* Rail: we keep the boot rail — 1.8/1.9V behave identically for 160M on this part, so we avoid
   * an extra analog write near the switch. Uncomment to add headroom purely for the edge:
   *   bk_psram_set_voltage(OWF_PSRAM_VOLT); bk_delay_us(2000); */
  if (!owf_t5_panel_park_and_wait_idle()) {
    /* NOTE: this header is included (via owf_tuya_co5300_qspi.h) BEFORE the .ino declares
     * USBSerial/HWCDC, so we log via the core's Serial (matching the neighbouring co5300 code),
     * which lacks printf() — hence print()/println() only. */
    Serial.println("[psram] deferred reclock ABORTED — flush task did not reach idle");
    return false;                            /* never flip while a DMA might be reading PSRAM */
  }
  /* Use the VENDOR'S OWN switch — the COMPLETE two-write sequence (clk_sel then clkdiv). This is
   * the same call that reliably set the clock when warm; the only change vs the original is WHEN
   * we call it (after first paint, task parked) instead of at boot. Do NOT hand-roll the register
   * write — the earlier direct ckdiv-only write was a partial of a two-register op and crashed. */
  bk_psram_set_clk(OWF_PSRAM_CLK);
  owf_t5_panel_unpark();

  Serial.println("[psram] deferred reclock applied (bk_psram_set_clk)");
  return true;
#else
  return false;
#endif
}

/* ---- PSRAM write-through CACHE for a draw buffer --------------------------------------
 * By default the PSRAM region is mapped NON-CACHEABLE by the MPU (mpu_cfg.c attr 1), so every
 * per-pixel read-modify-write the SW renderer does on a draw buffer pays full PSRAM latency
 * (the ~75 cycles/px we measured). The BK7258 has up to 4 hardware "write-through" channels
 * (bk_psram_*_write_through) that mark a PSRAM ADDRESS RANGE cacheable WRITE-THROUGH at runtime:
 *   - READS get cached -> the blend's background reads hit cache instead of raw PSRAM (the win).
 *   - WRITES go straight through to PSRAM -> the QSPI DMA always sees current pixels, so there
 *     is NO cache-coherency hazard on the push (the reason this is safe where write-back isn't).
 * Scoped to just our buffers, so WiFi/BT/system PSRAM is untouched. This is the SDK's own idiom
 * for fast frame buffers (see h264_encode_pipeline.c / lcd_scale_pipeline.c). No rebuild — the
 * APIs are exported in libdriver.a (declared via driver/psram.h above).
 *
 * Range must be 32-byte aligned. Returns true if a channel was allocated + enabled. Toggle with
 * OWF_PSRAM_CACHE_APPLY. */
#ifndef OWF_PSRAM_CACHE_APPLY
#define OWF_PSRAM_CACHE_APPLY 1
#endif

static inline bool owf_tuya_psram_cache_buffer(void *buf, uint32_t len) {
#if OWF_PSRAM_CACHE_APPLY
  if (!buf || !len) return false;
  uint32_t start = (uint32_t)buf & ~31u;                 // round start DOWN to 32B
  uint32_t end   = ((uint32_t)buf + len + 31u) & ~31u;   // round end   UP to 32B
  psram_write_through_area_t ch = bk_psram_alloc_write_through_channel();
  if (ch >= PSRAM_WRITE_THROUGH_AREA_COUNT) return false;  // no free channel
  return bk_psram_enable_write_through(ch, start, end) == BK_OK;
#else
  (void)buf; (void)len; return false;
#endif
}

#endif /* BOARD_PLATFORM_TUYA */
