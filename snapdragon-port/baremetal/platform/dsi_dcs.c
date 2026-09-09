/* dsi_dcs.c — DCS command transmission over the Gen 6 DSI link (command DMA).
 *
 * Why this exists (2026-08-04): the UI was drifting sideways in hard steps
 * every 20-30 s, wrapping around the panel — the signature of the DDIC's RAM
 * window (CASET/PASET) no longer matching what aboot programmed. The panel's
 * real window is OFFSET: CASET columns 30..445, PASET rows 0..415 (the DDIC
 * RAM is wider than the 416-px glass; decoded from the dumped panel node's
 * on-command table). We inherited that window from aboot and had no way to
 * restore it after our DSI error-recovery resets. This file gives the port
 * arbitrary DCS writes, which also unlocks:
 *   - re-pinning CASET/PASET (the drift fix, dsi_dcs_repin_window)
 *   - panel brightness: this panel is bl_ctrl_dcs, i.e. DCS 0x51 (dsi_dcs_
 *     set_brightness) — brightness/dimming never worked on this watch before
 *
 * Mechanism, kernel-verbatim (mdss_dsi_cmd.c packing + mdss_dsi_cmd_dma_tx):
 *   packet buffer = 32-bit header word (+ payload padded to x4 for long pkts)
 *     short: LAST | DTYPE<<16 | param<<8 | cmd
 *     long:  LAST | LONG_PKT | DTYPE(0x39)<<16 | word-count, payload follows
 *   DSI_DMA_CMD_OFFSET 0x048 = buffer PHYSICAL addr (flat-mapped: VA == PA)
 *   DSI_DMA_CMD_LENGTH 0x04c = length (4-byte aligned)
 *   DSI_TRIG_CTRL      0x084 [3:0] = 0x04 (SW trigger; matches DT
 *                      "trigger_sw"). ONLY that field — bit 31 is TE_SEL and
 *                      [6:4] is MDP_TRIGGER_SEL; see dcs_dma_send().
 *   DSI_CMD_MODE_DMA_SW_TRIGGER 0x090 = 1
 *   DSI_INT_CTRL       0x110 bit 0 = CMD_DMA_DONE (W1C)
 * Buffer must be cache-cleaned before the DMA master reads it (same lesson
 * as the framebuffer: fb_flush_region).
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#define DSI_CTRL_BASE        PLAT_DSI_CTRL_BASE   /* 0x01A94000 */
#define DSI_TRIG_CTRL        0x084u
#define DSI_DMA_CMD_OFFSET   0x048u
#define DSI_DMA_CMD_LENGTH   0x04Cu
#define DSI_DMA_SW_TRIGGER   0x090u
#define DSI_INT_CTRL         0x110u
#define   INT_CMD_DMA_DONE   (1u << 0)

#define HDR_LAST             (1u << 31)
#define HDR_LONG_PKT         (1u << 30)
#define HDR_DTYPE(t)         (((t) & 0x3Fu) << 16)
#define DT_DCS_WRITE0        0x05u
#define DT_DCS_WRITE1        0x15u
#define DT_DCS_LWRITE        0x39u

#define D_R(off)     mmio_read(DSI_CTRL_BASE + (off))
#define D_W(off, v)  mmio_write(DSI_CTRL_BASE + (off), (v))

/* DMA source buffer. 64-byte aligned so one cache-clean covers it exactly. */
static uint8_t s_pkt[64] __attribute__((aligned(64)));

static int dcs_dma_send(uint32_t len)
{
    fb_flush_region(s_pkt, sizeof(s_pkt));     /* clean to DDR for the DMA */

    /* DMA trigger select ONLY (bits [3:0]) — 2026-08-06, THE TE CLOBBER.
     *
     * This used to be a blind `D_W(DSI_TRIG_CTRL, 0x04)`. TRIG_CTRL is not a
     * dma-trigger register: it also carries MDP_TRIGGER_SEL [6:4] and, in
     * bit 31, TE_SEL — "gate the command-mode MDP stream transfer on the
     * panel's TE". (Layout per the vendor kernel's mdss_dsi_host_init; the
     * gen-4 port of that same function is in msm_dsi.c and builds the word as
     * (1<<31)|mdp_trigger<<4|dma_trigger.)
     *
     * dsi_dcs_repin_window() runs on EVERY frame from fb_kick(), so from the
     * first frame onward TE_SEL was permanently zero: the MDP streamed each
     * frame into the DDIC's RAM with no regard for where the DDIC's own
     * scan-out pointer was. On a single-port-RAM AMOLED the DDIC answers a
     * colliding write by stalling the link, and a stall mid-packet is exactly
     * DLN0_HS_FIFO_UNDERFLOW (bit 19 — the bit the census keeps reporting).
     * An underflowed line is truncated, so every line after it lands
     * displaced: wrapped image, byte-phase-shifted (wrong) colours, garbage
     * pixels, and the error census fires the full DSI+MDP reset recovery,
     * which is where the multi-second stalls come from.
     *
     * Preserve every other field; TE_SEL is programmed once in
     * dsi_host_12nm_reenable(). */
    D_W(DSI_TRIG_CTRL, (D_R(DSI_TRIG_CTRL) & ~0xFu) | 0x04u);
    D_W(DSI_INT_CTRL, D_R(DSI_INT_CTRL) | INT_CMD_DMA_DONE);  /* clear stale */
    D_W(DSI_DMA_CMD_OFFSET, (uint32_t)(uintptr_t)s_pkt);
    D_W(DSI_DMA_CMD_LENGTH, (len + 3u) & ~3u);
    D_W(DSI_DMA_SW_TRIGGER, 1u);

    uint32_t t0 = timer_ms();
    while (!(D_R(DSI_INT_CTRL) & INT_CMD_DMA_DONE)) {
        if ((uint32_t)(timer_ms() - t0) > 50u) {
            con_puts("dsi-dcs: DMA_DONE timeout\n");
            return -1;
        }
    }
    D_W(DSI_INT_CTRL, D_R(DSI_INT_CTRL) | INT_CMD_DMA_DONE);  /* ack */

    /* ARM_DRAIN (2026-08-06). CMD_DMA_DONE means the DMA engine finished
     * FETCHING the packet from DDR — not that the bytes have left the lane.
     * That is exactly the trap that truncated every pure I2C write in this
     * port (see the QUP OUTPUT_SERVICE vs MAX_OUTPUT_DONE bug). fb_kick calls
     * us from dsi_dcs_repin_window() and then immediately fires CTL_START, so
     * if a DCS packet is still in flight the MDP stream collides with it on
     * the one lane — a plausible source of the DLN0 HS FIFO underflow.
     * DSI_STATUS 0x008 bit2 = CMD_MODE_DMA_BUSY. Advisory, 10 ms cap. */
    if (g_dsi_dcs_drain) {
        uint32_t t1 = timer_ms();
        while (D_R(0x008u) & (1u << 2)) {
            if ((uint32_t)(timer_ms() - t1) > 10u) {
                /* Hitting this cap costs 10 ms, and repin_window sends FOUR
                 * packets per frame — 40 ms/frame would by itself explain the
                 * decay from ~50 fps to ~20. Count it so the log can say
                 * whether that is what is happening. */
                g_dsi_drain_timeouts++;
                break;
            }
        }
    }
    return 0;
}

/* Short DCS write, 0 or 1 parameter. */
int dsi_dcs_write(uint8_t cmd, uint8_t param, int has_param)
{
    uint32_t hdr = HDR_LAST
                 | HDR_DTYPE(has_param ? DT_DCS_WRITE1 : DT_DCS_WRITE0)
                 | ((uint32_t)param << 8) | cmd;
    *(volatile uint32_t *)s_pkt = hdr;
    return dcs_dma_send(4u);
}

/* Long DCS write: payload[0] = DCS command, rest = parameters. */
int dsi_dcs_lwrite(const uint8_t *payload, uint32_t len)
{
    if (len == 0 || len > sizeof(s_pkt) - 8u) return -1;
    uint32_t hdr = HDR_LAST | HDR_LONG_PKT | HDR_DTYPE(DT_DCS_LWRITE)
                 | (len & 0xFFFFu);
    *(volatile uint32_t *)s_pkt = hdr;
    uint32_t i;
    for (i = 0; i < len; i++) s_pkt[4 + i] = payload[i];
    for (; i & 3u; i++)       s_pkt[4 + i] = 0xFF;   /* kernel pads with FF */
    return dcs_dma_send(4u + i);
}

/* Re-program the panel RAM window to the values from the vendor on-command
 * table: CASET 0x001e..0x01bd (cols 30..445), PASET 0x0000..0x019f (rows
 * 0..415). This is THE canonical position — any drift/garble accumulated in
 * the DDIC is cancelled the moment these land. */
int dsi_dcs_repin_window(void)
{
    static const uint8_t caset[5] = { 0x2A, 0x00, 0x1E, 0x01, 0xBD };
    static const uint8_t paset[5] = { 0x2B, 0x00, 0x00, 0x01, 0x9F };
    /* 2026-08-04, the REG_BARS verdict: frames are addressed correctly
     * (0x44/stream readbacks exact) yet the DISPLAY mapping walks per
     * transfer, and chronic FIFO underflows can truncate packets into
     * stray single-byte commands — like 0x39 IDLE ON, the mode whose whole
     * point is a shifting display mapping (vendor's idle-off re-sends every
     * positional register for a reason). So the cure must include the
     * vendor's IDLE-OFF, not just the window. */
    if (dsi_dcs_write(0xFE, 0x00, 1) < 0) return -1;   /* page 0 */
    if (dsi_dcs_write(0x38, 0x00, 0) < 0) return -1;   /* IDLE MODE OFF */
    if (dsi_dcs_lwrite(caset, 5) < 0) return -1;
    return dsi_dcs_lwrite(paset, 5);
}

/* FULL positional re-pin: window + partial area + partial mode, i.e. every
 * positional state the vendor on-command table programs. Heavier than
 * dsi_dcs_repin_window — used on error recovery and the periodic safety tick,
 * where a deeper DDIC state loss is on the table. */
int dsi_dcs_repin_full(void)
{
    static const uint8_t ptlar_rows[5] = { 0x30, 0x00, 0x00, 0x01, 0x9F };
    static const uint8_t ptlar_cols[5] = { 0x31, 0x00, 0x1E, 0x01, 0xBD };
    if (dsi_dcs_repin_window() < 0) return -1;
    if (dsi_dcs_lwrite(ptlar_rows, 5) < 0) return -1;
    if (dsi_dcs_lwrite(ptlar_cols, 5) < 0) return -1;
    /* DCS 0x12 PTLON. The vendor on-command table sends it exactly ONCE at
     * init; we re-send it every 8 kicks, which is not something the vendor
     * ever does to a running panel. Arm-controlled so it can be ruled in/out. */
    if (g_dsi_partial_on && dsi_dcs_write(0x12u, 0, 0) < 0) return -1;
    /* TEAR_ON is positional state too: a DDIC that lost the window may have
     * lost its TE enable, and the recovery path is exactly where that would
     * bite. Re-assert it here alongside the rest of the vendor on-state. */
    return g_dsi_te_on ? dsi_dcs_set_tear(1) : 0;
}

/* ===== DCS 0x35 SET_TEAR_ON — THE MISSING COMMAND (2026-08-06) =============
 *
 * Decoded from the panel node's own qcom,mdss-dsi-on-command table in the
 * device dump. The vendor's full on sequence is:
 *
 *   05 00 00 | 15 fe 01 | 15 0a f0 | 15 fe 00        page select
 *   29 2a 00 1e 01 bd    CASET  cols 30..445    <- we replicate
 *   29 2b 00 00 01 9f    PASET  rows 0..415     <- we replicate
 *   29 30 00 00 01 9f    PTLAR rows            <- we replicate
 *   29 31 00 1e 01 bd    PTLAR cols            <- we replicate
 *   05 12 00             partial mode on       <- we replicate
 *   15 35 02             SET_TEAR_ON, param 2  <- WE NEVER SENT THIS
 *   15 53 20 | 15 51 00 | 15 66 00 | 15 63 ff | 05 11 00 | 05 29 00
 *
 * Without 0x35 the DDIC simply does not drive its TE line. That is why
 * PP0 LINE_COUNT (`telc` in the census) has never once advanced past its
 * init value in any run on this watch, in every arm, across every image: we
 * armed TE gating on BOTH sides — the MDP pingpong tear check and the DSI's
 * TRIG_CTRL TE_SEL — onto a signal the panel was never asked to produce.
 *
 * It also explains the round-3 measurement that finally broke the deadlock:
 * NOTEAR was the only consistently-low arm (238, 230 vs 385-841 everywhere
 * else), because switching the broken gating off is better than gating on a
 * dead wire — but it leaves the transfer unsynchronised against the DDIC's
 * own scan-out, which IS the tearing/wrap/flicker the glass shows.
 *
 * param 0x02 is the vendor's value, used verbatim rather than the usual
 * 0x00 (V-blank only) / 0x01 (V+H blank). */
int dsi_dcs_set_tear(int on)
{
    if (!on) return dsi_dcs_write(0x34u, 0, 0);   /* SET_TEAR_OFF */
    return dsi_dcs_write(0x35u, 0x02u, 1);        /* SET_TEAR_ON, vendor param */
}

/* Panel brightness 0..255 — this panel is qcom,mdss-dsi-bl-pmic-control-type
 * "bl_ctrl_dcs": brightness IS DCS 0x51 (bl-min 1, bl-max 255 per the DT). */
int dsi_dcs_set_brightness(uint8_t level)
{
    if (level == 0) level = 1;    /* 0 = panel's "off"; floor at bl-min */
    return dsi_dcs_write(0x51u, level, 1);
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
