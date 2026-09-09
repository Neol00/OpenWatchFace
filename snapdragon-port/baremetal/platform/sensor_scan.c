/* sensor_scan.c — find the accelerometer / optical HR sensor on the two
 * modem-owned I2C buses (msm8909w, both watches; -DSENSOR_SCAN).
 *
 * Both DTBs: i2c@78b5000 = BLSP1 QUP1 on gpio6/7 (blsp_i2c1), i2c@78b6000 =
 * BLSP1 QUP2 on gpio111/112 (blsp_i2c2), both "disabled" for the AP because
 * the modem DSP (sensor hub) drove them. Our firmware never boots the modem,
 * so the AP can own them outright. The TLMM function number for the i2c mux
 * is not in the DTB (it only names "blsp_i2c1"), so each pin function 1..3 is
 * tried in turn: a wrong mux just yields no ACKs.
 *
 * For every responding address the usual id registers are read:
 *   0x0F  ST WHO_AM_I (LSM6DSx, LISxx), 0x00 Bosch CHIP_ID (BMI/BMA),
 *   0x75  InvenSense WHO_AM_I, 0xFF Maxim PART_ID (MAX3010x/MAX8614x),
 *   0x92  TI AFE4404-style, 0x10 various.
 * A second pass repeats the scan with PM8916 L17 (the reference-design sensor
 * supply, 2.85 V) voted on, in case the parts sit unpowered after aboot. */
#include "platform.h"
#if defined(SENSOR_SCAN) && defined(PLAT_SOC_MSM8909)
#include "FreeRTOS.h"
#include "task.h"

struct bus { uintptr_t base; const char *name; uint32_t sda, scl; };
static const struct bus k_bus[2] = {
    { 0x078B5000u, "QUP1 (i2c@78b5000, gpio6/7)",     6u,   7u   },
    { 0x078B6000u, "QUP2 (i2c@78b6000, gpio111/112)", 111u, 112u },
};

static int probe(uintptr_t base, uint8_t addr)
{
    uint8_t b;
    return i2c_bus_xfer(base, addr, 0, 0, &b, 1) == 0;
}
static void idreg(uintptr_t base, uint8_t addr, uint8_t reg)
{
    uint8_t v = 0;
    con_puts(" r"); con_puthex(reg);
    if (i2c_bus_xfer(base, addr, &reg, 1, &v, 1) == 0) { con_puts("="); con_puthex(v); }
    else con_puts("=--");
}

static int scan_bus(const struct bus *b)
{
    static const uint8_t ids[] = { 0x0F, 0x00, 0x75, 0xFF, 0x92, 0x10 };
    int found_total = 0;
    for (uint32_t func = 1u; func <= 3u && !found_total; func++) {
        tlmm_cfg(b->sda, func, 0u, 2u, 0);
        tlmm_cfg(b->scl, func, 0u, 2u, 0);
        if (i2c_bus_init(b->base) < 0) { con_puts("sensor-scan: bus init failed\n"); return -1; }
        int found = 0;
        for (uint32_t a = 0x08u; a <= 0x77u; a++) {
            wdog_pet(); deadman_kick();
            if (!probe(b->base, (uint8_t)a)) continue;
            found++;
            con_puts("sensor-scan:   ACK at 0x"); con_puthex(a);
            for (unsigned i = 0; i < sizeof ids; i++) idreg(b->base, (uint8_t)a, ids[i]);
            con_puts("\n");
        }
        con_puts("sensor-scan: "); con_puts(b->name); con_puts(" func "); con_putdec(func);
        con_puts(": "); con_putdec((uint32_t)found); con_puts(" device(s)\n");
        found_total += found;
    }
    return found_total;
}

/* v54: the C2's modem sensor registry (persist/sensors/sns.reg, SSI SMGR
 * config) says the IMU is NOT an I2C part at all: sensor_id 0 (accel) and 10
 * (gyro), one driver UUID, "i2c" bus 6 with address 0x00 and the SPI flag,
 * interrupt on gpio96. Bus 6 = BLSP1 QUP6 = spi@78ba000, whose pinctrl in
 * both DTBs is gpio8 MOSI, gpio9 MISO, gpio11 CLK, gpio10 CS0. The HR sensor
 * (bus 1, addr 0x15, sensor_id 90, INT gpio110) matches pass 1's PAH8011.
 * Bit-bang the SPI here (mode 3, ~100 kHz) — no GCC clocks, no QUP SPI port
 * needed for a WHO_AM_I. Reads 0x0F (ST LSM6DS*: 0x69/0x6A/0x6C) and 0x00
 * (Bosch BMI160: 0xD1, BMI270: 0x24). */
#define SPI_MOSI 8u
#define SPI_MISO 9u
#define SPI_CS   10u
#define SPI_CLK  11u
static void spi_delay(void) { uint32_t t0 = timer_us32(); while ((uint32_t)(timer_us32() - t0) < 5u) ; }
static uint8_t spi_xfer_byte(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        tlmm_out(SPI_CLK, 0);                      /* mode 3: data changes on falling edge */
        tlmm_out(SPI_MOSI, (out >> i) & 1u);
        spi_delay();
        tlmm_out(SPI_CLK, 1);                      /* sampled on rising edge */
        spi_delay();
        in = (uint8_t)((in << 1) | (tlmm_in(SPI_MISO) & 1u));
    }
    return in;
}
static uint8_t spi_reg_read(uint8_t reg, int dummy)
{
    uint8_t v;
    tlmm_out(SPI_CS, 0); spi_delay();
    spi_xfer_byte((uint8_t)(0x80u | reg));
    if (dummy) spi_xfer_byte(0);                   /* BMA4xx-style dummy byte */
    v = spi_xfer_byte(0);
    spi_delay(); tlmm_out(SPI_CS, 1); spi_delay();
    return v;
}
static void spi_scan(void)
{
    static const uint8_t regs[] = { 0x0F, 0x00, 0x7F, 0x75, 0x01 };
    con_puts("sensor-scan: IMU on bit-banged SPI (gpio8 MOSI, 9 MISO, 11 CLK, 10 CS; sns.reg bus 6)\n");
    tlmm_cfg(SPI_CS,   0u, 0u, 2u, 1); tlmm_out(SPI_CS, 1);
    tlmm_cfg(SPI_CLK,  0u, 0u, 2u, 1); tlmm_out(SPI_CLK, 1);
    tlmm_cfg(SPI_MOSI, 0u, 0u, 2u, 1); tlmm_out(SPI_MOSI, 0);
    tlmm_cfg(SPI_MISO, 0u, 1u, 2u, 0);
    timer_delay_ms(2);
    (void)spi_reg_read(0x7F, 0);                   /* BMI160: first CS edge switches it to SPI */
    con_puts("sensor-scan:   spi");
    for (unsigned i = 0; i < sizeof regs; i++) { con_puts(" r"); con_puthex(regs[i]); con_puts("="); con_puthex(spi_reg_read(regs[i], 0)); }
    con_puts("\nsensor-scan:   spi with dummy byte: r0F="); con_puthex(spi_reg_read(0x0F, 1));
    con_puts(" r00="); con_puthex(spi_reg_read(0x00, 1));
    con_puts("\nsensor-scan:   MISO idle="); con_putdec((uint32_t)tlmm_in(SPI_MISO));
    con_puts(" INT gpio96="); tlmm_cfg(96u, 0u, 0u, 2u, 0); con_putdec((uint32_t)tlmm_in(96u)); con_puts("\n");
}

static void sensor_scan_body(void)
{
    int total = 0;
    spi_scan();
    /* The RPM SMD channel has no lock of its own; the WiFi bring-up on the
     * wlan-net task votes rails over it at about the same time. Hold the
     * radio lock for the whole scan so the two never interleave (v50). */
    con_puts("sensor-scan: modem-owned I2C buses (pass 1, rails as left by aboot)\n");
    for (unsigned i = 0; i < 2u; i++) total += scan_bus(&k_bus[i]);
    /* v52: NO RPM rail votes here — v49..v51 hung the watch right at the
     * first vote (reason unknown, console silent). Instead dump every PM8916
     * LDO's enable + voltage over SPMI, read-only: LDOn lives at
     * sid 1, 0x4000 + 0x100*(n-1); VOLTAGE_CTL1/2 at +0x40/+0x41, EN_CTL at
     * +0x46 (bit 7 = enabled). The rails that read OFF are the candidates
     * for the accelerometer's supply; enabling one is a separate step. */
    con_puts("sensor-scan: PM8916 LDO census (SPMI, read-only)\n");
    for (uint32_t n = 1u; n <= 18u; n++) {
        uint16_t base = (uint16_t)(0x4000u + 0x100u * (n - 1u));
        uint8_t en = 0, v1 = 0, v2 = 0, mode = 0;
        int rc = spmi_read8(1, base + 0x46u, &en);   /* regulators are on SID 1 (SID 0 = PON/charger/BMS) */
        spmi_read8(1, base + 0x40u, &v1); spmi_read8(1, base + 0x41u, &v2); spmi_read8(1, base + 0x45u, &mode);
        con_puts("sensor-scan:   L"); con_putdec(n);
        if (rc < 0) { con_puts(" (read failed)\n"); continue; }
        con_puts((en & 0x80u) ? " ON " : " off");
        /* PM8916 LDO: range in VOLTAGE_CTL1[0:2], step in CTL2; the vendor
         * driver's table: range 0..3 -> 375/1750/1750(?)/ ... — print raw. */
        con_puts(" ctl1="); con_puthex(v1); con_puts(" ctl2="); con_puthex(v2);
        con_puts(" mode="); con_puthex(mode); con_puts(" writable="); con_putdec((uint32_t)(spmi_writable(1, base + 0x46u) > 0));
        con_puts("\n");
    }
    con_puts("sensor-scan: done, "); con_putdec((uint32_t)(total < 0 ? 0 : total)); con_puts(" device hit(s) total\n");
    con_flush();
}

/* Own low-priority task (v51): the scan takes many seconds (3 pin functions
 * x 112 addresses x 2 buses, plus 2 s RPM waits), and on the UI task that
 * read as a hung watch. */
static void sensor_scan_task(void *arg) { (void)arg; sensor_scan_body(); vTaskDelete(0); }
void sensor_scan(void)
{
    if (xTaskCreate(sensor_scan_task, "sensor-scan", 2048, 0, 1, 0) != pdPASS)
        con_puts("sensor-scan: task create failed\n");
}
#else
void sensor_scan(void) {}
#endif
