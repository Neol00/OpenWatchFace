/* ============================================================================
 *  tuya/owf_tuya_co5300_qspi.h — OUR OWN CO5300 QSPI panel driver for the T5-E1.
 *
 *  WHY THIS EXISTS: the firmware previously drove the panel through the TuyaOpen SDK's
 *  PRECOMPILED display stack (tdd_disp_qspi_co5300 + tdd_display_qspi + tdl_disp_*, in
 *  libtuyaos.a). That worked, but every quirk was opaque: the close() stub that never
 *  blanks the panel, the brightness floor at 5, the get_free_fb pool-index bug, and —
 *  the trigger for this rewrite — rotation. The SDK does rotation in SOFTWARE at the
 *  tdl/LVGL layer (its QSPI driver only adds x/y offset, never touches MADCTL); our
 *  render path is LVGL PARTIAL + zero-copy, where LVGL 9.5 software rotation is
 *  unavailable (it needs DIRECT/FULL + matrix rotation). So a hardware 180° flip was
 *  impossible to reach through the SDK.
 *
 *  This module reimplements ONLY the display path in our own source, on top of the
 *  tkl_qspi HAL (the SAME layer the SDK driver uses — tkl_qspi_init/comand/send/irq +
 *  the T5 force_cs extension). It is a faithful port of the upstream reference
 *  (TuyaOpen/src/peripherals/display/tdd_display/src/qspi/tdd_display_qspi.c and
 *  tdd_disp_qspi_co5300.c, both in Tuya-waveshare-provided-T5-E1/), with three things
 *  now under OUR control:
 *    1. the init sequence (authoritative cCO5300_INIT_SEQ) + an appended MADCTL (0x36)
 *       so rotation is done in HARDWARE,
 *    2. the per-rotation x/y offset (a 180° flip moves the panel's 6px margin to the
 *       far side, so the offset must change — the SDK never had to because it never
 *       set MADCTL),
 *    3. the async flush (queue + dedicated task + tx_sem completion), so we keep the
 *       zero-copy two-buffer render/DMA overlap that owf_tuya_lvgl_own.h relies on.
 *
 *  Touch (CST92xx) is NOT ported — it stays on the SDK tdl_tp layer (different chip
 *  from the S3's FT3168, nothing to reuse, and it works).
 *
 *  GATED by OWF_T5_OWN_PANEL (default 0 = keep the SDK path). Flip to 1 to use this.
 *  Included only on BOARD_PLATFORM_TUYA.
 * ========================================================================== */
#pragma once
#if BOARD_PLATFORM_TUYA

/* Master switch. 0 = keep the SDK display stack (tdl_disp_*). 1 = use this own-source
 * driver. Safe to merge at 0; flip to 1 to bring it up. */
#ifndef OWF_T5_OWN_PANEL
#define OWF_T5_OWN_PANEL 1
#endif

#if OWF_T5_OWN_PANEL

#include <string.h>   /* memset, size_t */

extern "C" {
#include "tuya_cloud_types.h"   /* TUYA_QSPI_* types, OPERATE_RET, SEM/QUEUE/THREAD handles */
#include "tkl_gpio.h"           /* panel reset pin */
#include "tal_api.h"            /* tal_semaphore_* / tal_queue_* / tal_thread_* / tal_mutex_* / tal_system_sleep */

/* ---- tkl_qspi HAL, forward-declared with CORRECT widths ---------------------------------
 * We do NOT #include tkl_qspi.h: the portable template header types tkl_qspi_send's size as
 * uint16_t, which would TRUNCATE our >64KB band length at the call site. The real T5 symbol
 * takes the full length (the SDK driver pushes 145KB bands through it). We also need
 * tkl_qspi_force_cs_pin, a T5 extension absent from the template header. Declaring the exact
 * symbols we use (linker binds them from the SDK libs) sidesteps both problems. */
OPERATE_RET tkl_qspi_init(TUYA_QSPI_NUM_E port, const TUYA_QSPI_BASE_CFG_T *cfg);
OPERATE_RET tkl_qspi_comand(TUYA_QSPI_NUM_E port, TUYA_QSPI_CMD_T *command);
OPERATE_RET tkl_qspi_send(TUYA_QSPI_NUM_E port, void *data, uint32_t size);   /* uint32_t, NOT uint16_t */
OPERATE_RET tkl_qspi_irq_init(TUYA_QSPI_NUM_E port, TUYA_QSPI_IRQ_CB cb);
OPERATE_RET tkl_qspi_irq_enable(TUYA_QSPI_NUM_E port);
OPERATE_RET tkl_qspi_force_cs_pin(TUYA_QSPI_NUM_E port, uint8_t level);       /* T5 extension */

/* PSRAM heap (same symbols lv_psram_alloc.cpp uses; bounce buffers live in PSRAM). */
void *tkl_system_psram_malloc(size_t size);
void  tkl_system_psram_free(void *ptr);
}

#include "compat/owf_tuya_psram_freq.h"   /* owf_tuya_psram_cache_buffer (write-through read cache) */

/* ============================ panel geometry / wiring ============================
 * Mirrors the authoritative board config (board_com_api.c + owf_tuya_port.h): 466x466
 * CO5300 on QSPI0 @80MHz, RST=29, x_offset 6 in the un-rotated orientation. */
#define OWF_PNL_W           466
#define OWF_PNL_H           466
#define OWF_PNL_QSPI_PORT   TUYA_QSPI_NUM_0
#define OWF_PNL_QSPI_CLK    (80 * 1000000)
#define OWF_PNL_RST_PIN     TUYA_GPIO_NUM_29

/* ---- Rotation (0 or 180; 90/270 need a transposing flush, not supported here) -----------
 * 180° is a pure hardware MADCTL flip — ONLY the MADCTL byte changes; the offset is the same
 * for both orientations (see the offset note below).
 *   - MADCTL (0x36): the X+Y mirror bits. Standard MIPI is MX|MY = 0x40|0x80 = 0xC0.
 *     (Arduino_GFX's CO5300 class uses a non-standard 0x02|0x05=0x07; if 0xC0 comes out
 *     wrong, OWF_PNL_MADCTL is the first knob to try 0x07.) */
/* Rotation comes from the board header (single config point); fall back to 180. */
#ifndef OWF_PNL_ROTATION
#define OWF_PNL_ROTATION    OWF_T5_PANEL_ROTATION
#endif

/* stringify (for the bring-up log) */
#define OWF_STR_(x) #x
#define OWF_STR(x)  OWF_STR_(x)

/* OFFSETS (panel glass is hardwired to controller RAM cols [6..471], rows [0..465]):
 *   rotation 0  : x=6, y=0 (write directly into the wired window).
 *   rotation 180: MADCTL MX/MY mirror the scan, and EMPIRICALLY the two axes mirror around
 *                 DIFFERENT widths on this CO5300:
 *                   - MX mirrors around 480 (col -> 479-col): to land logical [0..465] on wired
 *                     cols [6..471] we need x=8  (479-8=471, 479-473=6). y handled separately.
 *                   - MY mirrors around 466 (row -> 465-row): logical [0..465] -> rows [0..465]
 *                     already, so y=0.
 *   (History: 8/14 wrapped the bottom to the top (y wrong); 6/0 left a ~3px strip on the right
 *    (x wrong). 8/0 is the pair that satisfies BOTH axes.) */
#if OWF_PNL_ROTATION == 180
  #define OWF_PNL_X_OFFSET  8
  #define OWF_PNL_Y_OFFSET  0
  #ifndef OWF_PNL_MADCTL
  #define OWF_PNL_MADCTL    0xC0   /* MX|MY (standard 180°). Alt if colors/mirror wrong: 0x07. */
  #endif
#else
  #define OWF_PNL_X_OFFSET  6
  #define OWF_PNL_Y_OFFSET  0
  #ifndef OWF_PNL_MADCTL
  #define OWF_PNL_MADCTL    0x00
  #endif
#endif

/* ---- CO5300 opcodes (from tdd_disp_co5300.h) ---- */
#define OWF_CO_WRITE_REG    0x02   /* QSPI prefix for a register write */
#define OWF_CO_WRITE_COLOR  0x32   /* QSPI prefix for a pixel (RAMWR) write */
#define OWF_CO_CASET        0x2A
#define OWF_CO_RASET        0x2B
#define OWF_CO_RAMWR        0x2C   /* mid byte of the 0x002C00 pixel address */
#define OWF_CO_MADCTL       0x36
#define OWF_CO_BL           0x51

/* ---- our own frame-buffer struct (field names match TDL_DISP_FRAME_BUFF_T so the LVGL
 * glue in owf_tuya_lvgl_own.h accesses ->frame/->x_start/... identically) ---------------- */
typedef struct owf_pnl_fb {
  uint8_t  *frame;
  uint16_t  x_start, y_start;
  uint16_t  width, height;   /* END+1 convention (== x2+1 / y2+1), matching the SDK flush */
  uint32_t  len;             /* byte count to clock out */
  void     (*free_cb)(struct owf_pnl_fb *fb);
  void     *free_arg;
} owf_pnl_fb_t;

/* ---- authoritative init sequence (cCO5300_INIT_SEQ) + our appended MADCTL --------------
 * Format: [byte_count, delay_ms, cmd, data...] per entry, terminated by a 0 byte_count.
 * byte_count counts cmd+data; delay_ms is applied AFTER the command. The MADCTL line is
 * OUR addition (the SDK seq has none → rotation 0); it sets the hardware flip. */
static const uint8_t owf_co5300_init_seq[] = {
    2,   0,   0xFE, 0x20,
    2,   0,   0x19, 0x10,
    2,   0,   0x1C, 0xA0,
    2,   0,   0xFE, 0x00,
    2,   0,   0xC4, 0x80,
    2,   0,   0x3A, 0x55,            /* 16bpp RGB565 */
    2,   0,   0x35, 0x00,            /* TE on */
    2,   0,   0x53, 0x20,
    2,   0,   0x51, 0xFF,            /* brightness max (runtime-overridden) */
    2,   0,   0x63, 0xFF,
    2,   0,   OWF_CO_MADCTL, OWF_PNL_MADCTL,   /* <-- hardware rotation (our addition) */
    1, 200,   0x11,                  /* sleep out, 200ms */
    1,   0,   0x29,                  /* display on */
    0,
};

/* ============================ QSPI sync state (mirrors the SDK qspi_task) ============================ */
/* The task serves two message kinds so EVERYTHING that touches the QSPI bus is serialized on one
 * thread: FRAME = push a bounce buffer; CMD = a single register write (brightness 0x51, display
 * on/off, sleep). Routing register writes through the SAME queue as frames is what prevents the
 * command-race (a brightness cmd firing mid-flush, between force_cs(0) and force_cs(1), corrupting
 * the transfer). */
typedef enum { OWF_PNL_MSG_FRAME = 0, OWF_PNL_MSG_CMD, OWF_PNL_MSG_EXIT } owf_pnl_evt_e;
typedef struct {
  owf_pnl_evt_e event;
  owf_pnl_fb_t *fb;        /* FRAME */
  uint8_t       cmd;       /* CMD: register opcode */
  uint8_t       data;      /* CMD: single data byte */
  uint8_t       has_data;  /* CMD: 1 if data is sent, 0 for a no-arg command */
} owf_pnl_msg_t;

static SEM_HANDLE    s_owf_pnl_tx_sem  = NULL;   /* posted by the TX-done IRQ */
static QUEUE_HANDLE  s_owf_pnl_queue   = NULL;   /* flush requests -> task */
static THREAD_HANDLE s_owf_pnl_task    = NULL;   /* the push task */
static MUTEX_HANDLE  s_owf_pnl_mutex   = NULL;
static volatile uint8_t s_owf_pnl_running = 0;

/* ---- park/unpark: quiesce the push task so the PSRAM reclock can flip the divider with NO DMA
 * in flight (see owf_tuya_psram_freq.h). s_owf_pnl_park_req asks the task to stop dequeuing;
 * s_owf_pnl_parked is the task's ack that it is idle BETWEEN frames (not mid-owf_pnl_send_frame,
 * so CS is released and the last DMA's tx_sem has already been waited on). The frame queue keeps
 * buffering pushes while parked; they drain after unpark. */
static volatile uint8_t s_owf_pnl_park_req = 0;   /* 1 = caller wants the task parked */
static volatile uint8_t s_owf_pnl_parked   = 0;   /* 1 = task is idle & acknowledges parked */

/* TX-complete IRQ: release the per-frame wait (DMA finished clocking pixels out). */
static void owf_pnl_qspi_evt_cb(TUYA_QSPI_NUM_E port, TUYA_QSPI_IRQ_EVT_E event) {
  (void)port;
  if (event == TUYA_QSPI_EVENT_TX && s_owf_pnl_tx_sem) tal_semaphore_post(s_owf_pnl_tx_sem);
}

/* ---- one register write: 0x02 prefix (1-wire) + 24-bit addr 0x00,cmd,0x00 (1-wire) + data (1-wire) */
static void owf_pnl_send_cmd(uint8_t cmd, uint8_t *data, uint32_t data_len) {
  TUYA_QSPI_CMD_T c;
  memset(&c, 0, sizeof(c));
  c.op        = TUYA_QSPI_WRITE;
  c.cmd[0]    = OWF_CO_WRITE_REG;  c.cmd_lines  = TUYA_QSPI_1WIRE;  c.cmd_size  = 1;
  c.addr[0]   = 0x00; c.addr[1] = cmd; c.addr[2] = 0x00;
  c.addr_lines= TUYA_QSPI_1WIRE;   c.addr_size  = 3;
  c.data      = data; c.data_lines = TUYA_QSPI_1WIRE; c.data_size = data_len;
  c.dummy_cycle = 0;
  tkl_qspi_comand(OWF_PNL_QSPI_PORT, &c);
}

/* ---- CASET/RASET window (absolute coords; offsets added here) ---- */
static void owf_pnl_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  uint8_t d[4];
  x1 += OWF_PNL_X_OFFSET; x2 += OWF_PNL_X_OFFSET;
  y1 += OWF_PNL_Y_OFFSET; y2 += OWF_PNL_Y_OFFSET;
  d[0] = (x1 >> 8) & 0xFF; d[1] = x1 & 0xFF; d[2] = (x2 >> 8) & 0xFF; d[3] = x2 & 0xFF;
  owf_pnl_send_cmd(OWF_CO_CASET, d, 4);
  d[0] = (y1 >> 8) & 0xFF; d[1] = y1 & 0xFF; d[2] = (y2 >> 8) & 0xFF; d[3] = y2 & 0xFF;
  owf_pnl_send_cmd(OWF_CO_RASET, d, 4);
}

/* ---- push one frame buffer: pixel pre-command (0x32 + 0x002C00) then DMA the pixels on
 * 4 wires, holding CS across both, and block on tx_sem until the DMA completes. Pixels are
 * already in panel byte order (LVGL renders RGB565_SWAPPED) -> sent as-is, zero-copy. ---- */
static void owf_pnl_send_frame(owf_pnl_fb_t *fb) {
  TUYA_QSPI_CMD_T c;
  memset(&c, 0, sizeof(c));
  tkl_qspi_force_cs_pin(OWF_PNL_QSPI_PORT, 0);           /* assert CS for the whole transaction */
  c.op        = TUYA_QSPI_WRITE;
  c.cmd[0]    = OWF_CO_WRITE_COLOR; c.cmd_size = 1;      c.cmd_lines  = TUYA_QSPI_1WIRE;
  c.addr[0]   = 0x00; c.addr[1] = OWF_CO_RAMWR; c.addr[2] = 0x00;
  c.addr_size = 3;                  c.addr_lines = TUYA_QSPI_1WIRE;
  c.data_size = 0;                  c.dummy_cycle = 0;
  tkl_qspi_comand(OWF_PNL_QSPI_PORT, &c);               /* command + address phase (no data) */
  /* Drain any STALE tx_sem post before arming this frame's DMA wait. The TX-done IRQ posts tx_sem
   * for ANY write op — including a register write that carries data, e.g. a brightness 0x51 sent as
   * a CMD message. If such a post is left sitting, the wait below returns on it IMMEDIATELY, before
   * this frame's DMA finishes — freeing the buffer mid-transfer so the next push overlaps an
   * in-flight DMA on the shared QSPI bus and stalls. THIS is the brightness-drag lag (heavy only on
   * the T5, only while brightness changes, and present on the SDK qspi_task too). Non-blocking
   * (timeout 0 = poll) so we clear strays and then wait on THIS frame's completion alone. */
  while (tal_semaphore_wait(s_owf_pnl_tx_sem, 0) == OPRT_OK) { /* clear stray posts */ }
  tkl_qspi_send(OWF_PNL_QSPI_PORT, fb->frame, fb->len); /* DMA the pixels on 4 wires */
  tal_semaphore_wait(s_owf_pnl_tx_sem, SEM_WAIT_FOREVER);
  tkl_qspi_force_cs_pin(OWF_PNL_QSPI_PORT, 1);          /* release CS */
}

/* ---- the push task: drains the queue, pushes each frame, then fires its free_cb (which
 * calls lv_display_flush_ready -> LVGL may reuse that bounce buffer). Same model as the
 * SDK's __display_qspi_task; this is what preserves the render/DMA overlap. ---- */
static void owf_pnl_task(void *arg) {
  (void)arg;
  owf_pnl_msg_t msg;
  s_owf_pnl_running = 1;
  while (s_owf_pnl_running) {
    /* Park point: if a reclock asked us to stop, acknowledge idle HERE — between frames, with no
     * DMA in flight (the previous owf_pnl_send_frame already waited its tx_sem and released CS) —
     * and spin until released. We poll the queue with a short timeout instead of WAIT_FOREVER so
     * this check runs even when no frames arrive. Queued pushes stay buffered and drain on unpark. */
    if (s_owf_pnl_park_req) {
      s_owf_pnl_parked = 1;
      while (s_owf_pnl_park_req && s_owf_pnl_running) tal_system_sleep(1);
      s_owf_pnl_parked = 0;
    }
    if (tal_queue_fetch(s_owf_pnl_queue, &msg, 5 /*ms*/) != OPRT_OK) continue;
    if (msg.event == OWF_PNL_MSG_EXIT) { s_owf_pnl_running = 0; break; }
    if (msg.event == OWF_PNL_MSG_CMD) {
      owf_pnl_send_cmd(msg.cmd, msg.has_data ? &msg.data : NULL, msg.has_data ? 1 : 0);
      continue;
    }
    if (msg.fb) {
      owf_pnl_set_window(msg.fb->x_start, msg.fb->y_start,
                         msg.fb->width - 1, msg.fb->height - 1);
      owf_pnl_send_frame(msg.fb);
      if (msg.fb->free_cb) msg.fb->free_cb(msg.fb);
    }
  }
}

/* ---- park/unpark API (used by the deferred PSRAM reclock in owf_tuya_psram_freq.h) ----------
 * park_and_wait_idle: ask the push task to stop between frames and block until it acknowledges it
 * is idle with NO DMA in flight. Returns true once parked; false on timeout (caller then MUST NOT
 * flip the PSRAM divider). If the task was never started (early boot), returns true — nothing is
 * touching the bus yet, so the caller may proceed. */
bool owf_t5_panel_park_and_wait_idle(void) {
  if (!s_owf_pnl_running) return true;           /* no task yet -> bus is quiet, safe to proceed */
  s_owf_pnl_park_req = 1;
  /* Wait up to ~500 ms for the task to reach its park point (it polls the queue every 5 ms and
   * only parks BETWEEN frames, so at worst we wait out one in-flight frame's DMA). */
  for (int i = 0; i < 500; i++) {
    if (s_owf_pnl_parked) return true;
    tal_system_sleep(1);
  }
  s_owf_pnl_park_req = 0;                         /* give up cleanly -> let it run again */
  return false;
}

/* Release the park; the task resumes and drains any frames queued while parked. */
void owf_t5_panel_unpark(void) {
  s_owf_pnl_park_req = 0;
  /* Wait briefly for the ack to clear so a caller can't immediately re-park into a stale state. */
  for (int i = 0; i < 50 && s_owf_pnl_parked; i++) tal_system_sleep(1);
}

/* ---- panel reset (RST=29): high, 20ms, low, 200ms, high, 120ms (per the SDK reset) ---- */
static void owf_pnl_reset(void) {
  TUYA_GPIO_BASE_CFG_T cfg;
  cfg.mode = TUYA_GPIO_PUSH_PULL; cfg.direct = TUYA_GPIO_OUTPUT; cfg.level = TUYA_GPIO_LEVEL_LOW;
  tkl_gpio_init(OWF_PNL_RST_PIN, &cfg);
  tkl_gpio_write(OWF_PNL_RST_PIN, TUYA_GPIO_LEVEL_HIGH); tal_system_sleep(20);
  tkl_gpio_write(OWF_PNL_RST_PIN, TUYA_GPIO_LEVEL_LOW);  tal_system_sleep(200);
  tkl_gpio_write(OWF_PNL_RST_PIN, TUYA_GPIO_LEVEL_HIGH); tal_system_sleep(120);
}

/* ---- run the init sequence (same walker as __tdd_disp_init_seq) ---- */
static void owf_pnl_run_init_seq(void) {
  const uint8_t *p = owf_co5300_init_seq;
  while (*p) {
    uint8_t n = p[0], delay_ms = p[1], cmd = p[2];
    uint8_t *data = (n - 1) ? (uint8_t *)&p[3] : NULL;
    if (cmd) owf_pnl_send_cmd(cmd, data, n - 1);
    if (delay_ms) tal_system_sleep(delay_ms);
    p += p[0] + 2;
  }
}

/* ============================ public API ============================ */

/* Bring up QSPI + IRQ + reset + init sequence (incl. hardware rotation). Returns true on ok.
 * Call ONCE before creating buffers / flushing. */
static inline bool owf_t5_panel_open(void) {
  TUYA_QSPI_BASE_CFG_T qcfg;
  memset(&qcfg, 0, sizeof(qcfg));
  qcfg.role           = TUYA_QSPI_ROLE_MASTER;
  qcfg.mode           = TUYA_QSPI_MODE0;
  qcfg.type           = TUYA_QSPI_TYPE_LCD;
  qcfg.freq_hz        = OWF_PNL_QSPI_CLK;
  qcfg.use_dma        = true;
  qcfg.dma_data_lines = TUYA_QSPI_4WIRE;
  if (tkl_qspi_init(OWF_PNL_QSPI_PORT, &qcfg) != OPRT_OK) { Serial.println("[own-pnl] qspi init failed"); return false; }
  tkl_qspi_irq_init(OWF_PNL_QSPI_PORT, owf_pnl_qspi_evt_cb);
  tkl_qspi_irq_enable(OWF_PNL_QSPI_PORT);

  owf_pnl_reset();
  owf_pnl_run_init_seq();

  if (tal_semaphore_create_init(&s_owf_pnl_tx_sem, 0, 1) != OPRT_OK) return false;
  if (tal_mutex_create_init(&s_owf_pnl_mutex) != OPRT_OK) return false;
  tal_queue_create_init(&s_owf_pnl_queue, sizeof(owf_pnl_msg_t), 4);
  THREAD_CFG_T tcfg = { 4096, THREAD_PRIO_0, "owf_pnl" };
  if (tal_thread_create_and_start(&s_owf_pnl_task, NULL, NULL, owf_pnl_task,
                                  NULL, &tcfg) != OPRT_OK) return false;
  Serial.println("[own-pnl] CO5300 up (own QSPI driver, hardware rotation "
                 OWF_STR(OWF_PNL_ROTATION) ")");
  return true;
}

/* Allocate a bounce buffer of `bytes` in PSRAM (write-through cached, like the SDK path). */
static inline owf_pnl_fb_t *owf_t5_panel_create_fb(uint32_t bytes) {
  owf_pnl_fb_t *fb = (owf_pnl_fb_t *)tkl_system_psram_malloc(sizeof(owf_pnl_fb_t));
  if (!fb) return NULL;
  memset(fb, 0, sizeof(*fb));
  fb->frame = (uint8_t *)tkl_system_psram_malloc(bytes);
  if (!fb->frame) { tkl_system_psram_free(fb); return NULL; }
  fb->len = bytes;
  owf_tuya_psram_cache_buffer(fb->frame, bytes);   /* read-cache reads; writes pass through (DMA-coherent) */
  return fb;
}

/* Queue a buffer for async push; returns immediately. free_cb fires from the task on DMA-done. */
static inline void owf_t5_panel_flush(owf_pnl_fb_t *fb) {
  if (!s_owf_pnl_queue || !fb) return;
  tal_mutex_lock(s_owf_pnl_mutex);
  owf_pnl_msg_t msg = { OWF_PNL_MSG_FRAME, fb };
  tal_queue_post(s_owf_pnl_queue, &msg, SEM_WAIT_FOREVER);
  tal_mutex_unlock(s_owf_pnl_mutex);
}

/* Serialized register write: routed through the qspi task queue so it can NEVER land mid-flush
 * (closes the command-race). Before the task exists (early bring-up), send directly — nothing
 * else is touching the bus yet. `has_data`=0 sends a no-arg command (e.g. display on/off/sleep). */
static inline void owf_t5_panel_cmd(uint8_t cmd, uint8_t data, bool has_data) {
  if (s_owf_pnl_queue && s_owf_pnl_mutex) {
    owf_pnl_msg_t msg = { OWF_PNL_MSG_CMD, NULL, cmd, data, (uint8_t)(has_data ? 1 : 0) };
    tal_mutex_lock(s_owf_pnl_mutex);
    tal_queue_post(s_owf_pnl_queue, &msg, SEM_WAIT_FOREVER);
    tal_mutex_unlock(s_owf_pnl_mutex);
  } else {
    owf_pnl_send_cmd(cmd, has_data ? &data : NULL, has_data ? 1 : 0);
  }
}

/* Runtime brightness 0..255 via 0x51 (our own path; serialized; no floor-at-5 like the SDK, so 0
 * is a true blank). */
static inline void owf_t5_panel_set_brightness(uint8_t v) {
  owf_t5_panel_cmd(OWF_CO_BL, v, true);
}

/* ---- M1 self-test: fill the screen with solid color bands so we can confirm QSPI init,
 * command framing, the init sequence, rotation (MADCTL) and the offset BEFORE wiring LVGL.
 * Pushes `band_lines`-tall full-width bands top to bottom, cycling R/G/B. Synchronous. ---- */
static inline void owf_t5_panel_selftest(uint16_t band_lines) {
  if (band_lines == 0) band_lines = 32;
  uint32_t bytes = (uint32_t)OWF_PNL_W * band_lines * 2;
  owf_pnl_fb_t *fb = owf_t5_panel_create_fb(bytes);
  if (!fb) { Serial.println("[own-pnl] selftest alloc failed"); return; }
  /* RGB565_SWAPPED solid colors (panel byte order): R=0x00F8, G=0xE007, B=0x1F00 */
  const uint16_t colors[3] = { 0x00F8, 0xE007, 0x1F00 };
  uint16_t *px = (uint16_t *)fb->frame;
  uint8_t ci = 0;
  for (uint16_t y = 0; y < OWF_PNL_H; y += band_lines) {
    uint16_t h = (y + band_lines > OWF_PNL_H) ? (OWF_PNL_H - y) : band_lines;
    uint16_t c = colors[ci++ % 3];
    for (uint32_t i = 0; i < (uint32_t)OWF_PNL_W * h; i++) px[i] = c;
    fb->x_start = 0; fb->y_start = y;
    fb->width = OWF_PNL_W;     /* END+1 convention */
    fb->height = y + h;
    fb->len = (uint32_t)OWF_PNL_W * h * 2;
    owf_pnl_set_window(fb->x_start, fb->y_start, fb->width - 1, fb->height - 1);
    owf_pnl_send_frame(fb);   /* synchronous (selftest runs before the task matters) */
  }
  tkl_system_psram_free(fb->frame);
  tkl_system_psram_free(fb);
  Serial.println("[own-pnl] selftest done (expect R/G/B bands top->bottom, full-bleed, upright)");
}

#endif /* OWF_T5_OWN_PANEL */
#endif /* BOARD_PLATFORM_TUYA */
