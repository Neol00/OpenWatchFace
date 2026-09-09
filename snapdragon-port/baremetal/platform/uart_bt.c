/* uart_bt.c — Fossil Gen 6 Bluetooth UART: BLSP2 UART2, UARTDM v1.4.
 *
 * WHY THIS EXISTS. On the Gen 6 (sdm429w / hoki) Bluetooth is NOT the WCNSS
 * core that WiFi uses — it is a separate QCA WCN3990 on a 4-wire UART. That
 * matters because WCNSS needs a TrustZone-authenticated firmware load (the
 * pas_init_image = -13 wall that stopped WiFi), and this path needs none of
 * it: the chip takes its firmware over HCI on this serial link.
 *
 * All values below come from the watch's OWN kernel + device tree, not from a
 * related SoC:
 *   fossil-engineering/kernel-msm-fossil-cw @ fossil-android-msm-hoki-lw1.2-4.14
 *
 *   port         blsp2_uart2 @ 0x07AF0000 ("qcom,msm-hsuart-v14"), the ONLY
 *                hsuart in the tree with status = "okay"
 *   pins         gpio20 TX / 21 RX / 22 CTS / 23 RFR, function slot 2
 *                (pinctrl-sdm429w.c: PINGROUP(20, blsp_spi6, blsp_uart6, ...)
 *                and the funcs[] array is {gpio, f1, f2, ...} so blsp_uart6 = 2),
 *                drive 2 mA, bias-disable — exactly blsp2_uart2_active in the DT
 *   clocks       gcc-sdm429w.c: BLSP2 AHB is a VOTED branch (0x45004 bit 20);
 *                blsp2_uart2_apps CBCR 0xd02c bit 0; its RCG cmd_rcgr 0xd034,
 *                mnd_width 16, parent map {XO=0, GPLL0=1}
 *   baud         msm_serial_hs.c msm_hs_set_bps_locked() is a FIXED-core-clock
 *                table: 115200 -> CSR 0xcc. Decoding the table (460800 -> 0xff
 *                = /16) pins the core clock at 7 372 800 Hz, which is exactly
 *                the F(7372800, GPLL0, 1, 144, 15625) row in the freq table.
 *   registers    msm_serial_hs_hwreg.h `enum msm_hs_regs` — the v1.4 map
 *                (SR 0xa4, CR 0xa8, TF 0x100, RF 0x140), the SAME map
 *                uart_msm.c already uses for the debug console. The older
 *                UARTDM_*_ADDR defines in that header (SR 0x08 ...) are the
 *                legacy layout and do NOT apply here.
 *
 * Polled, no BAM/DMA. HCI at 115200 is 11.5 KB/s; the firmware download is a
 * few hundred KB once. A faster link can come later by reprogramming the RCG.
 */
#include "platform.h"
#if defined(PLAT_BT_UART_BASE)

#define U(off)   (PLAT_BT_UART_BASE + (off))

/* UARTDM v1.4 register map (enum msm_hs_regs) */
#define UART_MR1            0x000u
#define UART_MR2            0x004u
#define UART_IPR            0x018u
#define UART_TFWR           0x01Cu
#define UART_RFWR           0x020u
#define UART_DMRX           0x034u
#define UART_DMEN           0x03Cu
#define UART_NCF_TX         0x040u
#define UART_RXFS           0x050u
#define UART_CSR            0x0A0u
#define UART_SR             0x0A4u
#define UART_CR             0x0A8u
#define UART_IMR            0x0B0u
#define UART_ISR            0x0B4u
#define UART_RX_TOTAL_SNAP  0x0BCu      /* bytes received in the current DMRX run */
#define UART_BCR            0x0C8u
#define UART_TF             0x100u
#define UART_RF             0x140u

/* SR */
#define SR_RXRDY            (1u << 0)
#define SR_TXRDY            (1u << 2)
#define SR_TXEMT            (1u << 3)
#define ISR_RXSTALE         (1u << 3)      /* raw ISR: a stale burst has latched */
/* CR commands / enables */
#define CR_RX_EN            (1u << 0)
#define CR_RX_DIS           (1u << 1)
#define CR_TX_EN            (1u << 2)
#define CR_TX_DIS           (1u << 3)
#define CR_RESET_RX         0x10u
#define CR_RESET_TX         0x20u
#define CR_RESET_ERR        0x30u
#define CR_RESET_BRK        0x40u
#define CR_RESET_CTS        0x70u
#define CR_RESET_STALE_INT  0x80u
#define CR_PROTECTION_EN    0x100u
#define CR_FORCE_STALE      0x400u
#define CR_STALE_EVENT_EN   0x500u
/* MR1 / MR2 */
#define MR1_RX_RDY_CTL      (1u << 7)      /* assert RFR from the FIFO level */
#define MR1_CTS_CTL         (1u << 6)      /* honour CTS on transmit         */
#define MR2_BITS_PER_CHAR_8 (3u << 4)
#define MR2_STOP_BIT_ONE    (1u << 2)
#define MR2_LOOP_MODE       (1u << 7)
/* RXFS: how many bytes sit in the packing buffer, not yet in the FIFO */
#define RXFS_BUF_SHIFT      7u
#define RXFS_BUF_MASK       7u

/* ---- GCC (gcc-sdm429w.c) ------------------------------------------------ */
#define GCC_R(o)     mmio_read(PLAT_GCC_BASE + (o))
#define GCC_W(o, v)  mmio_write(PLAT_GCC_BASE + (o), (v))
#define GCC_VOTE            0x45004u
#define BLSP2_AHB_VOTE_BIT  (1u << 20)
#define BLSP2_AHB_CBCR      0x0B008u
#define UART2_APPS_CBCR     0x0D02Cu
#define UART2_APPS_CMD_RCGR 0x0D034u       /* +4 CFG, +8 M, +C N, +10 D */
#define CBCR_CLK_ENABLE     (1u << 0)
#define CBCR_CLK_OFF        (1u << 31)
#define RCG_ROOT_EN         (1u << 1)
#define RCG_UPDATE          (1u << 0)

/* Core-clock recipes from gcc-sdm429w.c's ftbl_blsp1_uart1_apps_clk_src, all
 * GPLL0 (src 1) with pre_div 1 and a dual-edge MND. CSR then divides again:
 * 0xff = /16 (so baud = core/16) and 0xcc = /64, per msm_hs_set_bps_locked().
 * 115200 uses 7.3728 MHz + 0xcc because core/16 for it (1.8432 MHz) is not in
 * the table; every faster rate uses core = baud*16 with CSR 0xff. */
struct bt_baud { uint32_t baud, core, m, n; uint8_t csr; };
static const struct bt_baud k_bauds[] = {
    {  115200u,  7372800u, 144u, 15625u, 0xCCu },
    {  460800u,  7372800u, 144u, 15625u, 0xFFu },
    {  921600u, 14745600u, 288u, 15625u, 0xFFu },
    { 3000000u, 48000000u,   3u,    50u, 0xFFu },
    { 3200000u, 51200000u,   8u,   125u, 0xFFu },
};
static const struct bt_baud *s_baud = &k_bauds[0];

static int s_up;

/* RX queue state (declared here: bt_uart_init() below resets it). */
static uint8_t  s_rxq[256];
static uint16_t s_rxh, s_rxt;


/* con_puthex prints all 32 bits; byte dumps want two digits. */
void bt_hex2(uint8_t v)
{
    static const char h[] = "0123456789abcdef";
    con_putc(h[v >> 4]); con_putc(h[v & 15u]);
}

static int branch_on(uint32_t cbcr)
{
    GCC_W(cbcr, GCC_R(cbcr) | CBCR_CLK_ENABLE);
    uint32_t t0 = timer_ms();
    while (GCC_R(cbcr) & CBCR_CLK_OFF)
        if ((uint32_t)(timer_ms() - t0) > 10u) return -1;
    return 0;
}

/* Clocks first and SEPARATELY reported: on this watch a register access into
 * an unclocked BLSP does not error, it hangs the bus and kills the boot (the
 * debug UART taught us that). Nothing below touches the UART until this
 * returns 0. */
static int bt_uart_clocks(void);
static int bt_uart_clocks_force(void) { return bt_uart_clocks(); }

static int bt_uart_clocks(void)
{
    GCC_W(GCC_VOTE, GCC_R(GCC_VOTE) | BLSP2_AHB_VOTE_BIT);
    uint32_t t0 = timer_ms();
    while (GCC_R(BLSP2_AHB_CBCR) & CBCR_CLK_OFF) {
        if ((uint32_t)(timer_ms() - t0) > 10u) {
            con_puts("bt-uart: BLSP2 AHB clk stuck\n");
            return -1;
        }
    }
    /* RCG: MND for the selected core rate, latch with UPDATE, root forced on. */
    GCC_W(UART2_APPS_CMD_RCGR + 0x08u, s_baud->m);
    GCC_W(UART2_APPS_CMD_RCGR + 0x0Cu, (~(s_baud->n - s_baud->m)) & 0xFFFFu);
    GCC_W(UART2_APPS_CMD_RCGR + 0x10u, (~s_baud->n) & 0xFFFFu);
    GCC_W(UART2_APPS_CMD_RCGR + 0x04u, (1u << 8) | 1u | (2u << 12));
    GCC_W(UART2_APPS_CMD_RCGR,
          GCC_R(UART2_APPS_CMD_RCGR) | RCG_ROOT_EN | RCG_UPDATE);
    t0 = timer_ms();
    while (GCC_R(UART2_APPS_CMD_RCGR) & RCG_UPDATE) {
        if ((uint32_t)(timer_ms() - t0) > 10u) {
            con_puts("bt-uart: RCG update stuck\n");
            return -1;
        }
    }
    if (branch_on(UART2_APPS_CBCR) < 0) {
        con_puts("bt-uart: apps clk stuck\n");
        return -1;
    }
    timer_delay_us(200);        /* settle, same rule as every other clock here */
    return 0;
}

static void bt_uart_pins(void)
{
    tlmm_cfg(PLAT_BT_UART_TX_GPIO,  PLAT_BT_UART_FUNC, 0u, 2u, 0);
    tlmm_cfg(PLAT_BT_UART_RX_GPIO,  PLAT_BT_UART_FUNC, 0u, 2u, 0);
    tlmm_cfg(PLAT_BT_UART_CTS_GPIO, PLAT_BT_UART_FUNC, 0u, 2u, 0);
    tlmm_cfg(PLAT_BT_UART_RFR_GPIO, PLAT_BT_UART_FUNC, 0u, 2u, 0);
}

/* csr_for_115200: the table in msm_hs_set_bps_locked() assumes the fixed
 * 7.3728 MHz core clock programmed above. 0xcc = divide by 64 on both edges. */
#define RXSTALE     31u

static void bt_uart_configure(int loopback)
{
    mmio_write(U(UART_CR), CR_PROTECTION_EN);        /* unlock the mode regs */
    mmio_write(U(UART_CR), CR_RESET_RX);
    mmio_write(U(UART_CR), CR_RX_DIS);
    mmio_write(U(UART_CR), CR_RESET_TX);
    mmio_write(U(UART_CR), CR_TX_DIS);
    mmio_write(U(UART_CR), CR_RESET_ERR);
    mmio_write(U(UART_CR), CR_RESET_BRK);
    mmio_write(U(UART_CR), CR_RESET_CTS);
    mmio_write(U(UART_CR), CR_RESET_STALE_INT);

    mmio_write(U(UART_DMEN), 0u);                    /* no BAM: polled PIO */
    mmio_write(U(UART_IMR), 0u);                     /* no interrupts       */
    mmio_write(U(UART_CSR), s_baud->csr);

    /* 8N1. Flow control ON for the real link (the firmware download relies on
     * it); OFF in loopback, where there is no peer to raise CTS and the TX
     * would stall forever waiting for it. */
    mmio_write(U(UART_MR2), MR2_BITS_PER_CHAR_8 | MR2_STOP_BIT_ONE
                            | (loopback ? MR2_LOOP_MODE : 0u));
    mmio_write(U(UART_MR1), loopback ? 0u
                                     : (MR1_RX_RDY_CTL | MR1_CTS_CTL | 0x10u));

    mmio_write(U(UART_IPR), RXSTALE & 0x1Fu); /* stale timeout       */
    mmio_write(U(UART_TFWR), 0u);
    mmio_write(U(UART_RFWR), 0u);
    mmio_write(U(UART_BCR), 0x1Eu);                  /* v1.4 compat bits    */

    mmio_write(U(UART_CR), CR_TX_EN);
    mmio_write(U(UART_CR), CR_RX_EN);
    /* Arm a large RX transfer and enable the stale event: without this the
     * packing buffer never flushes short bursts into the FIFO, which is
     * exactly what an HCI event is. */
    mmio_write(U(UART_DMRX), 0xFFFFFFu);
    mmio_write(U(UART_CR), CR_STALE_EVENT_EN);
    __asm__ volatile("dsb sy" ::: "memory");
}

int bt_uart_init(void)
{
    if (s_up) return 0;
    if (bt_uart_clocks() < 0) return -1;
    bt_uart_pins();
    bt_uart_configure(0);
    s_rxh = s_rxt = 0;
    s_up = 1;
    con_puts("bt-uart: blsp2_uart2 up at 115200 8N1, flow control on\n");
    return 0;
}

/* Re-point the link at a different speed: stop the UART, reprogram the clock,
 * reconfigure, restart. Used to hunt for the controller after a firmware
 * download, which can leave it running at the rate stored in its NVM. */
int bt_uart_set_baud(uint32_t baud)
{
    for (unsigned i = 0; i < sizeof k_bauds / sizeof k_bauds[0]; i++) {
        if (k_bauds[i].baud != baud) continue;
        s_baud = &k_bauds[i];
        mmio_write(U(UART_CR), CR_RX_DIS);
        mmio_write(U(UART_CR), CR_TX_DIS);
        if (bt_uart_clocks_force() < 0) return -1;
        bt_uart_configure(0);
        s_rxh = s_rxt = 0;
        con_puts("bt-uart: baud -> "); con_putdec(baud); con_puts("\n");
        return 0;
    }
    return -1;
}

/* One byte per NCF_TX write: the same single-character protocol uart_msm.c
 * uses on the debug console, which is bounded and needs no FIFO accounting. */
void bt_uart_putc(uint8_t c)
{
    uint32_t spin = 0;
    while (!(mmio_read(U(UART_SR)) & SR_TXEMT))
        if (++spin > 200000u) return;
    mmio_write(U(UART_NCF_TX), 1u);
    mmio_write(U(UART_TF), (uint32_t)c);
}

/* NON-BLOCKING single byte: 1 = handed to the transmitter, 0 = not ready now.
 * The blocking putc above spins until the transmitter drains, which is fine for
 * a few bytes but ruinous during a firmware download: the controller throttles
 * us with CTS once its buffer fills, every byte then burns the full spin
 * budget, and the caller's loop (which also drives the UI and usb_poll) stalls
 * for hundreds of ms. Callers that move bulk data must use this and come back. */
int bt_uart_try_putc(uint8_t c)
{
    if (!(mmio_read(U(UART_SR)) & SR_TXEMT)) return 0;   /* busy or CTS held off */
    mmio_write(U(UART_NCF_TX), 1u);
    mmio_write(U(UART_TF), (uint32_t)c);
    return 1;
}

void bt_uart_write(const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) bt_uart_putc(b[i]);
}

static void rxq_push(uint8_t b)
{
    uint16_t n = (uint16_t)((s_rxt + 1u) % sizeof s_rxq);
    if (n != s_rxh) { s_rxq[s_rxt] = b; s_rxt = n; }   /* full: drop, never wrap over unread */
}

/* Close out the current burst and arm the next one. */
static void rx_restart(void)
{
    mmio_write(U(UART_CR), CR_RESET_STALE_INT);
    mmio_write(U(UART_DMRX), 0xFFFFFFu);
    mmio_write(U(UART_CR), CR_STALE_EVENT_EN);
}

/* Polled receive, burst-based (rewritten 2026-09-06, third attempt).
 *
 * WHAT THE HARDWARE ACTUALLY DOES, learned the hard way from two wrong models:
 *   - RXFS's pending count is sampled BEFORE a flush and is stale by the time
 *     the data register is read; at 115200 more bytes land in between.
 *   - RX_TOTAL_SNAP is latched BY THE STALE EVENT. Reading it merely because
 *     the FIFO went ready gives nonsense (it read 1 while four bytes had
 *     already been taken), and mixing plain word reads with it desynchronises
 *     the count entirely - which produced `04 03 00 00 00 0c 00` from a reply
 *     that was really `04 0E 04 01 03 0C 00`.
 *
 * So do it the way the SoC's own bootloader UART driver does, and route
 * EVERYTHING through the stale mechanism, never reading a word outside it:
 *     1. wait for (or force) a stale event and confirm it in the raw ISR
 *     2. read RX_TOTAL_SNAP - now valid - for the burst's byte count
 *     3. read ceil(count/4) words and keep exactly `count` bytes
 *     4. clear the stale, re-arm DMRX, enable the next stale
 * The raw ISR is the right status to poll: IMR is 0 (no interrupts), so the
 * masked MISR the kernel uses would always read 0 here.
 */
static void bt_uart_pump(void)
{
    if (!(mmio_read(U(UART_ISR)) & ISR_RXSTALE)) {
        /* Nothing has completed on its own. If anything is in flight, force
         * the burst closed so it becomes readable; otherwise there is no data. */
        uint32_t pend = (mmio_read(U(UART_RXFS)) >> RXFS_BUF_SHIFT) & RXFS_BUF_MASK;
        if (!pend && !(mmio_read(U(UART_SR)) & SR_RXRDY)) return;
        mmio_write(U(UART_CR), CR_FORCE_STALE);
        uint32_t t0 = timer_ms();
        while (!(mmio_read(U(UART_ISR)) & ISR_RXSTALE)) {
            if ((uint32_t)(timer_ms() - t0) > 5u) { rx_restart(); return; }
        }
    }

    uint32_t total = mmio_read(U(UART_RX_TOTAL_SNAP)) & 0x00FFFFFFu;
#if defined(BT_UART_DIAG)
    con_puts("bt-uart: burst "); con_putdec(total); con_puts(" bytes\n");
#endif
    if (total > sizeof s_rxq) total = sizeof s_rxq;      /* never overrun */

    uint32_t got = 0;
    while (got < total) {
        uint32_t t0 = timer_ms();
        while (!(mmio_read(U(UART_SR)) & SR_RXRDY)) {
            if ((uint32_t)(timer_ms() - t0) > 5u) { total = got; break; }
        }
        if (got >= total) break;
        uint32_t w = mmio_read(U(UART_RF));
        for (unsigned i = 0; i < 4u && got < total; i++, got++)
            rxq_push((uint8_t)(w >> (8u * i)));
    }
    rx_restart();
}

int bt_uart_getc(void)
{
    if (s_rxh == s_rxt) bt_uart_pump();
    if (s_rxh == s_rxt) return -1;
    uint8_t b = s_rxq[s_rxh];
    s_rxh = (uint16_t)((s_rxh + 1u) % sizeof s_rxq);
    return (int)b;
}

int bt_uart_read(uint8_t *buf, uint32_t max, uint32_t timeout_ms)
{
    uint32_t n = 0, t0 = timer_ms();
    while (n < max) {
        int c = bt_uart_getc();
        if (c >= 0) { buf[n++] = (uint8_t)c; t0 = timer_ms(); continue; }
        if ((uint32_t)(timer_ms() - t0) > timeout_ms) break;
    }
    return (int)n;
}

/* INTERNAL LOOPBACK SELF-TEST. Proves clocks, the register map, TX and the
 * (fiddly) RX packing path with the WCN3990 still powered down and the pins
 * irrelevant — so a failure here is OURS, not the chip's. */
int bt_uart_loopback_test(void)
{
    static const uint8_t pat[] = { 0x01, 0x03, 0x0C, 0x00, 0x55, 0xAA, 0x5A };
    uint8_t got[sizeof pat];
    if (bt_uart_clocks() < 0) return -1;
    bt_uart_pins();
    bt_uart_configure(1);
    bt_uart_write(pat, sizeof pat);
    int n = bt_uart_read(got, sizeof got, 50u);
    int ok = (n == (int)sizeof pat);
    for (int i = 0; ok && i < n; i++) if (got[i] != pat[i]) ok = 0;
    con_puts("bt-uart: loopback sent "); con_putdec(sizeof pat);
    con_puts(" got "); con_putdec((uint32_t)n); con_puts(" [");
    for (int i = 0; i < n; i++) { bt_hex2(got[i]); con_puts(" "); }
    con_puts(ok ? "] PASS\n" : "] FAIL\n");
    s_up = 0;
    bt_uart_configure(0);            /* leave it in real (non-loopback) mode */
    s_rxh = s_rxt = 0;
    s_up = 1;
    return ok ? 0 : -1;
}

#endif /* PLAT_BT_UART_BASE */
