/* dsi_panel.c — panel init/on/off command tables + the fb_init() display path
 * for the Fossil Gen 4.
 *
 * The command table below is the AUO 400p command-mode panel from
 * arch/arm/boot/dts/qcom/dsi-panel-auo-400p-cmd.dtsi (qcom,mdss-dsi-on-command),
 * which HARDWARE.md identifies as the closest in-tree template. The REAL Fossil
 * panel table must be lifted from the stock DTB (HARDWARE.md open question #1);
 * this file is structured so that swapping the table is a data edit, nothing
 * more.
 *
 * DT command encoding (qcom,mdss-dsi-on-command), per byte:
 *   [0] dtype  [1] last  [2] vc  [3] ack  [4] wait(ms)  [5] dlen_hi  [6] dlen_lo
 *   [7..] payload
 * e.g. "15 01 00 00 00 00 02 FE 05" = DCS write1, wait 0ms, 2 bytes: FE 05.
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN4)

#include "msm_dsi_regs.h"

int  dsi_dcs_write(uint8_t cmd, const uint8_t *params, uint32_t len);
void dsi_init(void);
void mdp3_dma_config(const void *buf, uint32_t w, uint32_t h);

/* One entry per DT command row. `wait_ms` is the DT's byte [4]. */
struct panel_cmd {
    uint8_t  dtype;
    uint8_t  wait_ms;
    uint8_t  len;          /* payload length (DT dlen) */
    uint8_t  payload[8];
};

/* qcom,mdss-dsi-on-command from dsi-panel-auo-400p-cmd.dtsi.
 * The 0xFE writes are page/bank selects for the panel's register banks. */
static const struct panel_cmd panel_on_cmds[] = {
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x05 } },  /* select bank 5 */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x05, 0x00 } },
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x07 } },  /* select bank 7 */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x07, 0x6D } },
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x0A } },  /* select bank 10 */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x1C, 0x1B } },
    { DTYPE_DCS_WRITE1, 0,    2, { 0xFE, 0x00 } },  /* back to user bank */
    { DTYPE_DCS_WRITE1, 0,    2, { 0x35, 0x00 } },  /* tear on */
    { DTYPE_DCS_WRITE,  0,    2, { 0x11, 0x00 } },  /* exit sleep */
    /* DT row "32 01 00 00 FF 00 02 00 00" is a 255 ms wait after sleep-out. */
    { 0,                255,  0, { 0 } },
    { DTYPE_DCS_WRITE,  0,    2, { 0x29, 0x00 } },  /* display on */
};

/* qcom,mdss-dsi-off-command */
static const struct panel_cmd panel_off_cmds[] = {
    { DTYPE_DCS_WRITE, 0,   2, { 0x28, 0x00 } },    /* display off */
    { DTYPE_DCS_WRITE, 0x78, 2, { 0x10, 0x00 } },   /* enter sleep, wait 120ms */
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

/* Set the panel's write window to the full screen. Command-mode panels need
 * this before the first memory write; the MDP re-issues it per frame. */
static void panel_set_window(uint32_t w, uint32_t h)
{
    uint8_t col[4] = { 0, 0, (uint8_t)((w - 1) >> 8), (uint8_t)((w - 1) & 0xff) };
    uint8_t pg[4]  = { 0, 0, (uint8_t)((h - 1) >> 8), (uint8_t)((h - 1) & 0xff) };
    dsi_dcs_write(DCS_SET_COLUMN_ADDRESS, col, 4);
    dsi_dcs_write(DCS_SET_PAGE_ADDRESS,   pg,  4);
}

int panel_on(void)
{
    con_puts("panel: on sequence\n");
    if (panel_send(panel_on_cmds,
                   sizeof panel_on_cmds / sizeof panel_on_cmds[0]) < 0)
        return -1;
    panel_set_window(PLAT_PANEL_W, PLAT_PANEL_H);
    con_puts("panel: on\n");
    return 0;
}

int panel_off(void)
{
    return panel_send(panel_off_cmds,
                      sizeof panel_off_cmds / sizeof panel_off_cmds[0]);
}

/* --- framebuffer --------------------------------------------------------- */

/* Same contract as fb_ramfb.c on QEMU: hand back an XRGB8888 buffer of w*h.
 * Sized for the fleet's largest panel so the Gen 6 (416x416) reuses this. */
#define FB_MAX_W 480
#define FB_MAX_H 480
static uint32_t s_fb[FB_MAX_W * FB_MAX_H] __attribute__((aligned(4096)));

void *fb_init(uint32_t w, uint32_t h)
{
    if (w > FB_MAX_W || h > FB_MAX_H) return 0;

    dsi_init();
    if (panel_on() < 0) {
        con_puts("fb: panel_on failed\n");
        return 0;
    }

    /* Point the MDP3 DMA_P engine at the framebuffer. Nothing is pushed until
     * the first mdp3_flush() from LVGL's flush callback. */
    mdp3_dma_config(s_fb, w, h);

    con_puts("fb: "); con_putdec(w); con_puts("x"); con_putdec(h);
    con_puts(" @ "); con_puthex((uint32_t)(uintptr_t)s_fb); con_puts("\n");
    return s_fb;
}

#endif /* PLAT_BOARD_FOSSIL_GEN4 */
