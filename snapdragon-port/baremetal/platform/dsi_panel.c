/* dsi_panel.c — panel init/on/off command tables for the Fossil Gen 4.
 * for the Fossil Gen 4.
 *
 * REAL PANEL (2026-07-28): the table below is the actual Fossil panel, lifted
 * from the stock DTB dumped off a DW6F1 — node "AUO h139 AMOLED command mode
 * dsi panel" (pref-prim-pan phandle 0x9b), qcom,mdss-dsi-on-command. This
 * replaces the earlier AUO-400p template and closes HARDWARE.md open question #1.
 *   panel: 454x454 (0x1c6), 24bpp, command mode, framerate 45
 *   color order: rgb_swap_rgb  (the flush path must emit BGR / swap R and B)
 *   reset seq: low 12ms, high 22ms  (DT <0 0x0c 1 0x16>)
 *   DSI PHY timings: <0x4e0e0800 0x2e300e12 0x0a030400>  (consumed in msm_dsi.c)
 *   panel clockrate: 0x1265b500 = 308.4 MHz ; t-clk-post 5, t-clk-pre 0x12
 *
 * DT command encoding (qcom,mdss-dsi-on-command), per byte:
 *   [0] dtype  [1] last  [2] vc  [3] ack  [4] wait(ms)  [5] dlen_hi  [6] dlen_lo
 *   [7..] payload
 * e.g. "15 01 00 00 00 00 02 FE 05" = DCS write1, wait 0ms, 2 bytes: FE 05.
 */
#include "platform.h"
#include <stdio.h>
/* Widened from the Gen 4 board guard to the SoC tier: dsi_dcs_set_brightness()
 * below is generic MIPI DCS (0x51 WRITE_DISPLAY_BRIGHTNESS, 0x53
 * WRITE_CTRL_DISPLAY) over this SoC's DSI host, and the TicWatch C2's panel is
 * "bl_ctrl_dcs" with the same 1..255 range as the Gen 4's AUO h139 — so the
 * whole brightness and auto-dim path is inherited unchanged.
 *
 * The PANEL COMMAND TABLES are a different matter and stay Gen 4-only: they
 * are the AUO h139's own on/off sequences, and replaying them at an EDO
 * E1392AMC would be sending one panel's initialisation to another. Both
 * watches take the framebuffer-takeover path (fb_mdp3.c inherits whatever
 * aboot already configured) so neither normally needs a blind init at all;
 * on the C2 the blind path is simply refused rather than guessed. */
#if defined(PLAT_SOC_MSM8909)

#include "msm_dsi_regs.h"

/* One entry per DT command row. `wait_ms` is the DT's byte [4]. */
struct panel_cmd {
    uint8_t  dtype;
    uint8_t  wait_ms;
    uint8_t  len;          /* payload length (DT dlen) */
    uint8_t  payload[8];
};

/* qcom,mdss-dsi-on-command from the stock "AUO h139 AMOLED command mode dsi
 * panel" node (phandle 0x9b). The 0xFE writes are page/bank selects; the four
 * long (0x29) writes program the column/page address + partial-area windows.
 * Decoded verbatim from the DTB byte stream — do not "tidy" the values. */
#if defined(PLAT_BOARD_FOSSIL_GEN4)
static const struct panel_cmd panel_on_cmds[] = {
    { DTYPE_DCS_WRITE,  3,    2, { 0x00, 0x00 } },  /* dummy, wait 3ms */
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x01 } },  /* page 1 */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x0A, 0xF0 } },
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x0A } },  /* page 10 */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x29, 0x92 } },
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x00 } },  /* page 0 (user) */
    { DTYPE_DCS_LWRITE, 0,    5, { 0x2A, 0x00, 0x0A, 0x01, 0xCF } }, /* col 0x00a..0x1cf */
    { DTYPE_DCS_LWRITE, 0,    5, { 0x2B, 0x00, 0x00, 0x01, 0xC5 } }, /* page 0x000..0x1c5 */
    { DTYPE_DCS_LWRITE, 0,    5, { 0x30, 0x00, 0x00, 0x01, 0xC5 } }, /* partial rows */
    { DTYPE_DCS_LWRITE, 0,    5, { 0x31, 0x00, 0x0A, 0x01, 0xCF } }, /* partial cols */
    { DTYPE_DCS_WRITE,  0,    2, { 0x12, 0x00 } },  /* partial mode on */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x35, 0x00 } },  /* tear on */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x53, 0x20 } },  /* brightness ctrl block on */
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x07 } },  /* page 7 */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x00, 0x30 } },
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x00 } },  /* page 0 */
    { DTYPE_DCS_WRITE,  0x52, 2, { 0x11, 0x00 } },  /* exit sleep, wait 82ms */
    /* qcom,mdss-dsi-post-panel-on-command: display on */
    { DTYPE_DCS_WRITE,  0,    2, { 0x29, 0x00 } },
};

/* qcom,mdss-dsi-off-command (AUO h139): display off, then sleep-in wait 150ms.
 * DT: "05 01 00 00 00 00 02 28 00  05 01 00 00 96 00 02 10 00" (0x96 = 150). */
static const struct panel_cmd panel_off_cmds[] = {
    { DTYPE_DCS_WRITE, 0,    2, { 0x28, 0x00 } },   /* display off */
    { DTYPE_DCS_WRITE, 0x96, 2, { 0x10, 0x00 } },   /* enter sleep, wait 150ms */
};

static int panel_send(const struct panel_cmd *cmds, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        const struct panel_cmd *c = &cmds[i];
        if (c->dtype == 0) {                 /* pure delay row */
            timer_delay_ms(c->wait_ms);
            continue;
        }
        /* payload[0] is the DCS command, the rest are its parameters. */
        if (dsi_dcs_write(c->payload[0], &c->payload[1], c->len - 1) < 0) {
            con_puts("panel: cmd "); con_putdec(i); con_puts(" TIMEOUT\n");
            return -1;
        }
        if (c->wait_ms) timer_delay_ms(c->wait_ms);
    }
    return 0;
}

/* Panel active-area origin. The AUO h139 maps its 454-wide visible span at
 * COLUMN OFFSET 10, not 0 — the stock on-command sets 2A = 0x00a..0x1cf and
 * 2B = 0x000..0x1c5. A 0-origin window would shift the image left by 10px and
 * clip the right edge, so the per-frame window must reuse the panel's origin. */
#define PANEL_COL_START 0x00A
#define PANEL_ROW_START 0x000

/* Set the panel's write window to the full active area. Command-mode panels
 * need this before the first memory write; the MDP re-issues it per frame. */
static void panel_set_window(uint32_t w, uint32_t h)
{
    uint16_t c0 = PANEL_COL_START,      c1 = (uint16_t)(PANEL_COL_START + w - 1);
    uint16_t r0 = PANEL_ROW_START,      r1 = (uint16_t)(PANEL_ROW_START + h - 1);
    uint8_t col[4] = { (uint8_t)(c0 >> 8), (uint8_t)c0, (uint8_t)(c1 >> 8), (uint8_t)c1 };
    uint8_t pg[4]  = { (uint8_t)(r0 >> 8), (uint8_t)r0, (uint8_t)(r1 >> 8), (uint8_t)r1 };
    dsi_dcs_write(DCS_SET_COLUMN_ADDRESS, col, 4);
    dsi_dcs_write(DCS_SET_PAGE_ADDRESS,   pg,  4);
}

int panel_on(void)
{
    bdiag_puts("panel: on sequence\n");
    if (panel_send(panel_on_cmds,
                   sizeof panel_on_cmds / sizeof panel_on_cmds[0]) < 0)
        return -1;
    panel_set_window(PLAT_PANEL_W, PLAT_PANEL_H);
    bdiag_puts("panel: on\n");
    return 0;
}

int panel_off(void)
{
    return panel_send(panel_off_cmds,
                      sizeof panel_off_cmds / sizeof panel_off_cmds[0]);
}

/* --- blind bring-up ------------------------------------------------------
 * fb_init() used to live here and always ran this sequence. It no longer
 * does: platform/fb_mdp3.c owns the display path and inherits whatever aboot
 * left running, which is observable in a way a from-scratch DSI bring-up is
 * not. This entry point remains for the case where aboot handed us a dark
 * display, and is only reachable with -DGEN4_DSI_INIT. */
int panel_full_init(void)
{
    dsi_init();
    if (panel_on() < 0) {
        con_puts("panel: blind bring-up failed\n");
        return -1;
    }
    return 0;
}

#else  /* PLAT_BOARD_TICWATCH_C2: no panel command table for this panel */
/* The C2's EDO E1392AMC on/off sequences have not been transcribed, and
 * inventing them is worse than not having them: the takeover path never needs
 * one, and a wrong sequence can leave a command-mode AMOLED in a state only a
 * power cycle clears. Refuse loudly instead. */
int panel_on(void)        { con_puts("panel: no on-table for this panel\n");  return -1; }
int panel_off(void)       { con_puts("panel: no off-table for this panel\n"); return -1; }
int panel_full_init(void) { con_puts("panel: blind init unsupported (takeover only)\n"); return -1; }
#endif /* PLAT_BOARD_FOSSIL_GEN4 */

/* --- panel brightness (DCS 0x51) -----------------------------------------
 * The AUO h139 is qcom "bl_ctrl_dcs" with qcom,mdss-dsi-bl-max-level = <0xff>
 * (both from this watch's DTB), so brightness is one DCS short-write:
 * WRITE_DISPLAY_BRIGHTNESS, one byte, 0..255. There is no PWM backlight on an
 * AMOLED to drive instead.
 *
 * THE CAREFUL PART. Everything else in the Gen 4 display path deliberately
 * avoids DSI: platform/fb_mdp3.c inherits the link aboot set up and touches
 * only the MDP3 DMA_P block, precisely so no DSI mistake is possible. This is
 * the one exception, and it is safe for a specific reason rather than by luck:
 *
 *   - aboot itself sent this panel's whole 18-command power-on table through
 *     this same DMA command engine to put its splash up, so the engine is
 *     configured and working at handoff. We are using it, not initialising it.
 *   - the transfer is bounded: dsi_cmd_tx waits on CMD_MODE_DMA_BUSY with a
 *     50 ms timeout and returns -1, so a dead engine costs one failed call,
 *     not a hung UI.
 *   - it cannot collide with a frame push. mdp3_flush() is a BLOCKING
 *     full-frame transfer and LVGL's flush callback and this call both run on
 *     the UI task, so they are serialised by construction. (If the flush ever
 *     goes async — HARDWARE.md perf item #1 — this needs a real interlock,
 *     the equivalent of the kernel's mdss_dsi_cmd_mdp_busy wait.)
 *
 * The guard below is the belt: if DSI_CTRL does not show the controller
 * enabled in command mode, the link is not in the state we are assuming, so
 * refuse rather than poke it. */
int dsi_dcs_set_brightness(uint8_t level)
{
    uint32_t ctrl = mmio_read(PLAT_DSI_CTRL_BASE + DSI_CTRL);
    if (!(ctrl & DSI_CTRL_ENABLE) || !(ctrl & DSI_CTRL_CMD_MODE_EN)) {
        con_puts("bl: DSI not enabled in cmd mode (ctrl=");
        con_puthex(ctrl); con_puts("), refusing\n");
        return -1;
    }

    /* WRITE_CTRL_DISPLAY (0x53) bit 5 = brightness-control block on. Value
     * 0x20 is verbatim from the panel's own qcom,mdss-dsi-on-command table
     * above, so this is a replay of what the panel was already told, not an
     * invention. Sent once: if aboot's on-sequence already ran it, writing it
     * again is harmless; if the panel came up some other way, 0x51 would
     * otherwise be ignored. */
    static int s_ctrl_display_sent;
    if (!s_ctrl_display_sent) {
        uint8_t on = 0x20;
        if (dsi_dcs_write(0x53, &on, 1) < 0) {
            con_puts("bl: 0x53 write failed\n");
            return -1;
        }
        s_ctrl_display_sent = 1;
    }

    if (dsi_dcs_write(DCS_WRITE_DISPLAY_BRIGHTNESS, &level, 1) < 0) {
        con_puts("bl: 0x51 write failed\n");
        return -1;
    }
    return 0;
}

/* --- brightness self-test (-DBL_TEST) ------------------------------------
 * "The slider does nothing" has three possible causes needing three different
 * fixes: the call never reaches this file; it reaches it and the DSI write
 * fails; or the write succeeds and the PANEL ignores it. Nothing about the
 * symptom distinguishes them.
 *
 * So: paint the screen white and sweep 0x51 across the full range with the
 * display held still, entirely independent of the UI and the slider. If the
 * white visibly changes, the DCS path works and the bug is in the plumbing
 * above. If it does not, the command is not taking effect and the register
 * codes reported alongside say whether it was even accepted. */
void dsi_bl_selftest(char *buf, unsigned cap)
{
    static const uint8_t levels[] = { 0xFF, 0x80, 0x30, 0x08, 0xFF };
    uint32_t ctrl = mmio_read(PLAT_DSI_CTRL_BASE + DSI_CTRL);
    uint32_t stat = mmio_read(PLAT_DSI_CTRL_BASE + DSI_STATUS);
    uint32_t trig0 = mmio_read(PLAT_DSI_CTRL_BASE + DSI_TRIG_CTRL);
    int rc53, rc51[sizeof levels / sizeof levels[0]];

    /* Enable the brightness-control block, then sweep. Each step holds long
     * enough to be seen by eye. */
    {
        uint8_t on = 0x20;
        rc53 = dsi_dcs_write(0x53, &on, 1);
    }
    for (unsigned i = 0; i < sizeof levels / sizeof levels[0]; i++) {
        uint8_t lv = levels[i];
        rc51[i] = dsi_dcs_write(DCS_WRITE_DISPLAY_BRIGHTNESS, &lv, 1);
        timer_delay_ms(1200);
    }

    uint32_t trig1 = mmio_read(PLAT_DSI_CTRL_BASE + DSI_TRIG_CTRL);
    snprintf(buf, cap,
             "BL TEST\n"
             "CTRL %08X\n"
             "TRIG %08X\n"
             "  -> %08X\n"
             "R53 %d\n"
             "R51 %d %d %d\n"
             "%d %d\n"
             "STAT %08X",
             (unsigned)ctrl, (unsigned)trig0, (unsigned)trig1, rc53,
             rc51[0], rc51[1], rc51[2], rc51[3], rc51[4], (unsigned)stat);
}

#endif /* PLAT_SOC_MSM8909 */
