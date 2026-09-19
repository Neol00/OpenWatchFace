/* bg_probe.c — first contact with the Wear 3100 "BG" co-processor (QCC1110).
 *
 * Every pin below is from the watch's own DTB (dtbs/triggerfish-stock.dts);
 * the protocol is from the triggerfish kernel's drivers/soc/qcom/bgcom_spi.c
 * and subsys-pil-bg.c (protocol code, not board data):
 *
 *   qcom,bg-spi on spi@78B8000 (BLSP1 QUP4), CS0, 16 MHz, mode 0 (no cpol/cpha)
 *       pins spi4_default: gpio12 MOSI, gpio13 MISO, gpio15 CLK, gpio14 CS
 *       qcom,irq-gpio gpio110 (level HIGH = BG has something for the AP)
 *   qcom,pil-blackghost
 *       bg2ap-status gpio97 (1 = "BG services are up and running")
 *       bg2ap-errfatal gpio95, ap2bg-status gpio17, ap2bg-errfatal gpio23
 *   qcom,bg-daemon  bg-reset-gpio = PM660 (sid 0) GPIO5 @ 0xC400
 *
 * bgcom register read: one byte = register address, three pad bytes, then
 * 4 bytes per register clocked back little-endian. SLAVE_STATUS (0x05):
 *   bit31 SPI slave awake   bit30 application running
 *   bit29 to-slave FIFO ready  bit28 to-master FIFO ready  bit27 AHB ready
 *
 * Firmware is NOT loaded by us: Linux does it through the "bgapp" TrustZone
 * app (QSEECOM), which drives this same SPI bus from the secure side. The BG
 * also keeps running while the AP is off (TWM), so it may already be alive
 * when our image boots. This probe answers exactly that, and is
 * READ-ONLY unless gpio97 says the BG is up: no pin is driven into a chip that
 * may be unpowered, the reset line is only read, and gpio17/23 are untouched. */
#include "platform.h"
#if defined(PLAT_HAS_BG_QCC1110)

#define TLMM_CFG_ADDR(n) (0x01000000u + 0x1000u * (n))
#define TLMM_IO_ADDR(n)  (0x01000000u + 0x1000u * (n) + 4u)

static void pin_report(const char *name, uint32_t pin)
{
    uint32_t cfg = mmio_read(TLMM_CFG_ADDR(pin));
    con_puts("bg:   gpio"); con_putdec(pin); con_puts(" "); con_puts(name);
    con_puts(" level="); con_putdec(tlmm_in(pin));
    con_puts(" func="); con_putdec((cfg >> 2) & 0xFu);
    con_puts(cfg & (1u << 9) ? " out" : " in");
    con_puts(" pull="); con_putdec(cfg & 3u);
    con_puts("\n");
}

static void dly(void) { uint32_t t0 = timer_us32(); while ((uint32_t)(timer_us32() - t0) < 20u) ; }

/* SPI mode 0: clock idles low, both sides sample on the rising edge. */
static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        tlmm_out(PLAT_BG_SPI_MOSI, (out >> i) & 1u); dly();
        tlmm_out(PLAT_BG_SPI_CLK, 1);
        in = (uint8_t)((in << 1) | (tlmm_in(PLAT_BG_SPI_MISO) & 1u));
        dly();
        tlmm_out(PLAT_BG_SPI_CLK, 0);
    }
    return in;
}

static void reg_read(uint8_t reg, uint32_t *words, unsigned n)
{
    tlmm_out(PLAT_BG_SPI_CS, 0); dly(); dly();
    xfer(reg); xfer(0); xfer(0); xfer(0);
    for (unsigned w = 0; w < n; w++) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4; b++) v |= (uint32_t)xfer(0) << (8u * b);
        words[w] = v;
    }
    dly(); tlmm_out(PLAT_BG_SPI_CS, 1); dly();
}

void bg_probe_report(void)
{
    con_puts("bg: --- QCC1110 co-processor probe (read-only unless status=1) ---\n");
    pin_report("bg2ap-status  ", PLAT_BG2AP_STATUS_GPIO);
    pin_report("bg2ap-errfatal", PLAT_BG2AP_ERRFATAL_GPIO);
    pin_report("bgcom-irq     ", PLAT_BG_IRQ_GPIO);
    pin_report("ap2bg-status  ", PLAT_AP2BG_STATUS_GPIO);
    pin_report("ap2bg-errfatal", PLAT_AP2BG_ERRFATAL_GPIO);
    pin_report("spi mosi      ", PLAT_BG_SPI_MOSI);
    pin_report("spi miso      ", PLAT_BG_SPI_MISO);
    pin_report("spi cs        ", PLAT_BG_SPI_CS);
    pin_report("spi clk       ", PLAT_BG_SPI_CLK);

    uint16_t base = (uint16_t)(0xC000u + 0x100u * (PLAT_BG_RESET_PM_GPIO - 1u));
    uint8_t st = 0, mode = 0, en = 0, sub = 0, src = 0, outc = 0;
    int r1 = spmi_read8(0, base + 0x05, &sub);
    int r2 = spmi_read8(0, base + 0x10, &st);   /* RT_STS: bit0 = live pin level */
    int r3 = spmi_read8(0, base + 0x40, &mode);
    int r4 = spmi_read8(0, base + 0x46, &en);
    (void)spmi_read8(0, base + 0x44, &src);   /* LV output source + value bit */
    (void)spmi_read8(0, base + 0x45, &outc);  /* buffer type + drive strength */
    con_puts("bg:   pm660 gpio"); con_putdec(PLAT_BG_RESET_PM_GPIO);
    con_puts(" (reset) subtype="); con_puthex(sub);
    con_puts(" rt_sts="); con_puthex(st);
    con_puts(" mode_ctl="); con_puthex(mode);
    con_puts(" src_ctl="); con_puthex(src);
    con_puts(" out_ctl="); con_puthex(outc);
    con_puts(" en_ctl="); con_puthex(en);
    con_puts((r1 | r2 | r3 | r4) ? " (SPMI read error)\n" : "\n");

    if (!tlmm_in(PLAT_BG2AP_STATUS_GPIO)) {
        con_puts("bg: status gpio LOW -> BG application not running; SPI left untouched\n");
        return;
    }

    con_puts("bg: status gpio HIGH -> BG is up; reading bgcom registers over SPI\n");
    tlmm_cfg(PLAT_BG_SPI_CS,   0u, 0u, 2u, 1); tlmm_out(PLAT_BG_SPI_CS, 1);
    tlmm_cfg(PLAT_BG_SPI_CLK,  0u, 0u, 2u, 1); tlmm_out(PLAT_BG_SPI_CLK, 0);
    tlmm_cfg(PLAT_BG_SPI_MOSI, 0u, 0u, 2u, 1); tlmm_out(PLAT_BG_SPI_MOSI, 0);
    tlmm_cfg(PLAT_BG_SPI_MISO, 0u, 0u, 2u, 0);
    timer_delay_us(1000u);

    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t w[5] = { 0 };
        reg_read(0x05, w, 5);   /* SLAVE_STATUS, TIMESTAMP, AUTO_CLEAR, FIFO_FILL, FIFO_SIZE */
        con_puts("bg:   bgcom[0x05..] status="); con_puthex(w[0]);
        con_puts(" ts="); con_puthex(w[1]);
        con_puts(" autoclr="); con_puthex(w[2]);
        con_puts(" fifo_fill="); con_puthex(w[3]);
        con_puts(" fifo_size="); con_puthex(w[4]);
        con_puts("\n");
        con_puts("bg:   awake="); con_putdec((w[0] >> 31) & 1u);
        con_puts(" app_running="); con_putdec((w[0] >> 30) & 1u);
        con_puts(" to_slave_fifo="); con_putdec((w[0] >> 29) & 1u);
        con_puts(" to_master_fifo="); con_putdec((w[0] >> 28) & 1u);
        con_puts(" ahb="); con_putdec((w[0] >> 27) & 1u);
        con_puts(" master_used="); con_putdec(w[3] & 0xFFFFu);
        con_puts(" slave_free="); con_putdec(w[3] >> 16);
        con_puts("\n");
        if (w[0] & (1u << 31)) break;
        timer_delay_us(50000u);
    }
    con_puts("bg: --- probe done ---\n");
}

#endif /* PLAT_HAS_BG_QCC1110 */
