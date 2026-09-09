/* hr_pah8011.c — PixArt PAH8011 optical heart-rate (PPG) sensor, msm8909w
 * watches (Fossil Gen 4 / TicWatch C2).
 *
 * Wiring (C2 modem sensor registry, sns.reg SSI entry: bus 1, addr 0x15, INT
 * gpio110): I2C on the BLSP1 QUP1 pins gpio6 (SDA) / gpio7 (SCL). The bus is
 * BIT-BANGED here rather than driven through the QUP: the polled QUP driver is
 * limited to 56-byte reads and a FIFO drain is several hundred bytes.
 *
 * Protocol (recovered 2026-09-03 by disassembling the C2's modem DSP driver,
 * sns_dd_pah_driver_8011.c, in modem.b12 — there is no public datasheet):
 *   reg 0x7F              bank select (0..5)
 *   bank0 reg 0x00        product id, 0x11
 *   bank1 0x23/0x25 bit0  PPG enable (set to run, clear to stop)
 *         0x36/0x37       touch detect enable ((v&0xfc)|1, v|2)
 *         0x56/0x57       FIFO interrupt threshold, 9 bits: 1 + words per burst
 *         0x24 = 1        "update flag": latch the bank1 settings
 *   bank2 0x1B            interrupt status; bit0 FIFO ready, bit1 touch change,
 *                         bit2/bit3 error; write the value back to clear
 *         0x25 (16-bit)   FIFO word count
 *         0x1C (32-bit)   XOR checksum of the words read
 *         0x00 bit0       touch flag (finger/wrist on the window)
 *   bank3 0x00            FIFO stream: 2 x 32-bit words per sample (ch1, ch2),
 *                         PPG value in the upper 26 bits (>> 6)
 *   bank4 0x69            0 = wake, 1 = shutdown
 * Register tables are the ones the DSP driver loads (modem.b14 @ 0x13bcc0 init,
 * 0x13bd80 20 Hz, 0x13bda0 200 Hz). */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)
#include <string.h>

#define P_SDA 6u
#define P_SCL 7u
#define ADDR  0x15u

/* ---- bit-banged I2C (open drain: drive low, release = input + pull-up) ---- */
static void dly(void) { uint32_t t0 = timer_us32(); while ((uint32_t)(timer_us32() - t0) < 3u) ; }
static void sda_lo(void) { tlmm_cfg(P_SDA, 0u, 3u, 2u, 1); tlmm_out(P_SDA, 0); }
static void sda_hi(void) { tlmm_cfg(P_SDA, 0u, 3u, 2u, 0); }
static void scl_lo(void) { tlmm_cfg(P_SCL, 0u, 3u, 2u, 1); tlmm_out(P_SCL, 0); }
static void scl_hi(void) { tlmm_cfg(P_SCL, 0u, 3u, 2u, 0); uint32_t t0 = timer_us32(); while (!tlmm_in(P_SCL) && (uint32_t)(timer_us32() - t0) < 200u) ; }
static void i2c_start(void) { sda_hi(); scl_hi(); dly(); sda_lo(); dly(); scl_lo(); dly(); }
static void i2c_stop(void)  { sda_lo(); dly(); scl_hi(); dly(); sda_hi(); dly(); }
static int  i2c_wbyte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) { if ((b >> i) & 1u) sda_hi(); else sda_lo(); dly(); scl_hi(); dly(); scl_lo(); }
    sda_hi(); dly(); scl_hi(); dly();
    int ack = !tlmm_in(P_SDA);
    scl_lo(); dly();
    return ack;
}
static uint8_t i2c_rbyte(int ack)
{
    uint8_t b = 0;
    sda_hi();
    for (int i = 0; i < 8; i++) { scl_hi(); dly(); b = (uint8_t)((b << 1) | (tlmm_in(P_SDA) & 1u)); scl_lo(); dly(); }
    if (ack) sda_lo(); else sda_hi();
    dly(); scl_hi(); dly(); scl_lo(); sda_hi(); dly();
    return b;
}
static int wr(uint8_t reg, uint8_t v)
{
    i2c_start();
    int ok = i2c_wbyte((uint8_t)(ADDR << 1)) && i2c_wbyte(reg) && i2c_wbyte(v);
    i2c_stop();
    return ok;
}
static int rd(uint8_t reg, uint8_t *buf, unsigned n)
{
    i2c_start();
    int ok = i2c_wbyte((uint8_t)(ADDR << 1)) && i2c_wbyte(reg);
    if (ok) { i2c_start(); ok = i2c_wbyte((uint8_t)((ADDR << 1) | 1u)); }
    if (ok) for (unsigned i = 0; i < n; i++) buf[i] = i2c_rbyte(i + 1 < n);
    i2c_stop();
    return ok;
}

/* ---- register tables from the DSP driver (bank switches inline) ---- */
static const uint8_t k_init[][2] = {
 {0x7F,0x00},{0x14,0x1B},{0x15,0x06},{0x16,0xA0},{0x17,0x00},{0x19,0x2E},{0x1D,0x08},{0x26,0x00},{0x27,0x0C},{0x28,0x00},{0x29,0x04},
 {0x33,0x1B},{0x34,0x06},{0x35,0xA0},{0x36,0x00},{0x38,0x2E},{0x3C,0x08},{0x45,0x00},{0x46,0x0C},{0x47,0x00},{0x48,0x04},
 {0x52,0x1B},{0x53,0x06},{0x54,0xA0},{0x55,0x00},{0x57,0x2E},{0x5B,0x08},{0x64,0x00},{0x65,0x0C},{0x66,0x00},{0x67,0x04},
 {0x70,0x01},{0x71,0x02},{0x72,0x04},{0x73,0x0F},{0x74,0x0C},{0x75,0x03},{0x2A,0x3F},{0x49,0x3F},{0x68,0x3F},
 {0x7F,0x04},{0x07,0x00},{0x08,0x00},{0x0B,0x01},{0x0C,0x01},{0x0F,0x00},{0x10,0x00},{0x15,0x69},{0x2B,0xFE},{0x34,0x01},
 {0x7F,0x05},{0x04,0x06},{0x5C,0x08},{0x5D,0x08},{0x5E,0x00},{0x60,0x35},{0x64,0x14},
 {0x7F,0x01},{0x00,0x03},{0x0B,0x00},{0x0E,0x10},{0x14,0x03},{0x1B,0x01},{0x38,0x01},{0x39,0x06},{0x40,0x01},{0x41,0x06},{0x48,0x01},{0x49,0x06},
 {0x59,0x81},{0x21,0x68},{0x23,0x69},{0x25,0x69},{0x74,0x01},{0x75,0x03},
};
static const uint8_t k_ppg20[][2] = {
 {0x7F,0x05},{0x44,0x1F},{0x7F,0x01},{0x1C,0x04},{0x1D,0x04},{0x26,0x40},{0x27,0x06},{0x12,0x14},{0x3C,0x80},{0x3D,0x02},{0x44,0x80},{0x45,0x02},{0x4C,0x80},{0x4D,0x02},{0x04,0x00},
};
static const uint8_t k_ppg200[][2] = {
 {0x7F,0x05},{0x44,0x04},{0x7F,0x01},{0x1C,0x01},{0x1D,0x01},{0x26,0xA0},{0x27,0x00},{0x12,0xC8},{0x3C,0xA0},{0x3D,0x00},{0x44,0xA0},{0x45,0x00},{0x4C,0xA0},{0x4D,0x00},{0x04,0x00},
};
/* The DSP's pah_comm_write() caches the bank and only rewrites 0x7F on change. */
static int s_bank = -1;
static int bank(int b) { if (s_bank == b) return 1; s_bank = b; return wr(0x7F, (uint8_t)b); }
static int wr_b(int b, uint8_t reg, uint8_t v) { return bank(b) && wr(reg, v); }
static int rd_b(int b, uint8_t reg, uint8_t *buf, unsigned n) { return bank(b) && rd(reg, buf, n); }
static int load(const uint8_t (*t)[2], unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (t[i][0] == 0x7F) { s_bank = t[i][1]; }
        if (!wr(t[i][0], t[i][1])) { con_puts("hr: table write failed at "); con_putdec(i); con_puts("\n"); return 0; }
    }
    return 1;
}
static int update_flag(void) { return wr_b(1, 0x24, 1); }
static int rmw(uint8_t reg, uint8_t clr, uint8_t set)
{
    uint8_t v = 0;
    if (!rd_b(1, reg, &v, 1)) return 0;
    return wr_b(1, reg, (uint8_t)((v & (uint8_t)~clr) | set));
}

/* _pah8011_turn_led() from the DSP driver (sns_dd_pah_driver_8011.c): the LED
 * enable lives in BANK 0 regs 0x70/0x71/0x72 (green/IR/ambient DAC gates =
 * 1/2/4 when on, 0 when off), latched by the bank1 0x24 update flag. The init
 * table sets these once, but the shutdown at the end of init (bank4 0x69=1)
 * powers the LED DAC down, so they MUST be re-asserted on every start or the
 * sensor runs dark (chip awake + configured, FIFO stuck at 0). This is the
 * exact bug that made the Heart app read nothing on every 8909w watch. */
static int turn_led(int on)
{
    int ok = 1;
    ok &= wr_b(0, 0x70, on ? 0x01u : 0x00u);
    ok &= wr_b(0, 0x71, on ? 0x02u : 0x00u);
    ok &= wr_b(0, 0x72, on ? 0x04u : 0x00u);
    ok &= update_flag();
    return ok;
}

/* _pah8011 timing-generator start (DSP enable tail, driver @c0603060). Setting
 * PPG-enable (0x23/0x25 bit0) alone does NOT sample: the engine only runs once
 * bank1 reg 0x30 is written 1. The vendor sequence is: mask bank1 0x37 (&0xf2),
 * write 0x30=1, wait, clear the bank2 0x1b int status (=0xff), mask 0x37 again
 * (&0xef). Without this the chip stays awake and configured but the FIFO never
 * fills and the LEDs never fire - the exact dead-sensor symptom. */
static int tg_start(void)
{
    int ok = 1;
    timer_delay_ms(1);
    ok &= rmw(0x37, 0x0Du, 0x00);       /* 0x37 &= 0xf2 (clear bits 0,2,3) */
    ok &= wr_b(1, 0x30, 0x01);          /* START the timing generator */
    timer_delay_ms(1);
    ok &= wr_b(2, 0x1B, 0xFF);          /* clear int status (write-1-clear) */
    ok &= rmw(0x37, 0x10u, 0x00);       /* 0x37 &= 0xef (clear bit4) */
    return ok;
}

/* GREEN LED DRIVE (2026-09-06). The vendor init sets each LED's DAC to 0x1b
 * (bank0 0x14 green / 0x33 / 0x52, ceiling 0x3f per the 0x2a/0x49/0x68 = 0x3f
 * limits) and then runs an exposure/LED-current loop the port never had. At
 * rest the raw level sits ~1000-1500 on a 26-bit scale, i.e. the LEDs are
 * barely lit, and the pulse on top of it is a few counts: the user found the
 * reading only becomes reliable when finger movement raises perfusion. Drive
 * the green LED harder. Doubling is the first step; the log's dc= field shows
 * whether the level follows, which proves the register. Tunable. */
#ifndef HR_PAH_LED_DAC
#define HR_PAH_LED_DAC  0x36u
#endif
static uint8_t s_led_dac = (uint8_t)HR_PAH_LED_DAC;   /* current green drive; the app's AGC moves it */
static int s_present, s_running, s_words;   /* s_words = words per burst (2 per sample) */
static uint8_t s_buf[8 * 48];

int pah8011_present(void) { return s_present; }

int pah8011_init(void)
{
    uint8_t id = 0;
    s_bank = -1;
    sda_hi(); scl_hi(); timer_delay_ms(2);
    /* The chip keeps its state across an AP reboot: whoever ran last (our own
     * init/stop, or the Wear OS modem driver) left it in shutdown (bank4 0x69
     * = 1), and in shutdown the id reads 0. Wake it first, then retry the id a
     * few times (S2 2026-09-04: "not found" on every warm boot after v66). */
    for (int attempt = 0; attempt < 4; attempt++) {
        s_bank = -1;
        int w1 = wr(0x7F, 0x04), w2 = wr(0x69, 0x00);
        s_bank = 4;
        timer_delay_ms(5);
        if (rd_b(0, 0x00, &id, 1) && id == 0x11) break;
        con_puts("hr: id read "); con_puthex(id); con_puts(" (wake acks "); con_putdec((uint32_t)w1); con_putdec((uint32_t)w2); con_puts("), retrying\n");
        timer_delay_ms(20);
    }
    if (id != 0x11) {
        con_puts("hr: PAH8011 not found (id="); con_puthex(id); con_puts(")\n");
        return 0;
    }
    wr_b(4, 0x69, 0);                       /* wake */
    if (!load(k_init, sizeof k_init / 2)) return 0;
    wr_b(0, 0x14, s_led_dac);               /* green LED DAC: brighter than the vendor default */
    wr_b(4, 0x70, 0x18);
    wr_b(4, 0x69, 1);                       /* shutdown until a measurement starts */
    s_present = 1;
    con_puts("hr: PAH8011 ready (I2C gpio6/7)\n");
    return 1;
}

/* Start streaming. hz200 selects the 200 Hz table (else 20 Hz). samples_per_burst
 * sets the FIFO interrupt threshold (<= 48). */
static void set_threshold(void)
{
    uint32_t th = 1u + (uint32_t)s_words;              /* FIFO interrupt threshold (DSP: 1 + words) */
    wr_b(1, 0x57, (uint8_t)((th >> 8) & 1u)); wr_b(1, 0x56, (uint8_t)(th & 0xFFu));
    update_flag();
}
static void clear_int(void)
{
    uint8_t v = 0;
    if (rd_b(2, 0x1B, &v, 1)) wr_b(2, 0x1B, v);        /* write-back clears */
}

/* Start streaming. hz200 selects the 200 Hz table (else 20 Hz). samples_per_burst
 * sets the FIFO interrupt threshold (<= 48).
 *
 * ORDER MATTERS and is the DSP driver's mode-switch path (modem.b12
 * 0xc0602dd4..0xc0602ea4 with helpers 0xc0603204 / 0xc06032cc / 0xc0603cd4 /
 * 0xc0603154 / 0xc0602ec8), which is the path that applies here because the
 * chip keeps its previous configuration across an AP reboot:
 *   threshold -> touch on -> PPG off -> bank1 0x70 pulse (1, update, 60 ms,
 *   0, update; then clear bank2 0x1B) -> load the rate table -> threshold
 *   -> update -> PPG on -> update -> clear int.
 * v57..v70 loaded the table first and never pulsed 0x70: every register read
 * back as configured (S2 diag 2026-09-04) but the FIFO count stayed 0. */
int pah8011_start(int hz200, int samples_per_burst)
{
    uint8_t v = 0;
    if (!s_present) return 0;
    if (samples_per_burst < 1) samples_per_burst = 1;
    if (samples_per_burst > 48) samples_per_burst = 48;
    s_words = 2 * samples_per_burst;
    s_bank = -1;
    wr_b(4, 0x69, 0);                       /* wake */
    timer_delay_ms(2);
    set_threshold();
    rmw(0x36, 0x03, 0x01); rmw(0x37, 0x00, 0x02);       /* touch detect on (informational) */
    rmw(0x23, 0x01, 0x00); rmw(0x25, 0x01, 0x00);       /* PPG off while configuring */
    wr_b(1, 0x70, 1); update_flag();                    /* engine/FIFO reset pulse */
    timer_delay_ms(60);
    wr_b(1, 0x70, 0); update_flag();
    clear_int();
    if (hz200) load(k_ppg200, sizeof k_ppg200 / 2); else load(k_ppg20, sizeof k_ppg20 / 2);
    /* device-package variant: the DSP applies these when bank2 0x4A bit0 is clear */
    if (rd_b(2, 0x4A, &v, 1) && !(v & 1u)) { wr_b(1, 0x05, 0x04); wr_b(1, 0x08, 0x04); wr_b(1, 0x14, 0x06); wr_b(1, 0x75, 0x00); }
    set_threshold();
    update_flag();
    turn_led(1);                                        /* re-enable the LEDs (bank0 0x70/71/72) */
    wr_b(0, 0x14, s_led_dac);                           /* and the green drive level (AGC-learned) */
    rmw(0x23, 0x00, 0x01); rmw(0x25, 0x00, 0x01);       /* PPG on */
    update_flag();
    tg_start();                                         /* START the sampling engine (bank1 0x30=1) */
    s_running = 1;
    con_puts("hr: streaming at "); con_puts(hz200 ? "200" : "20"); con_puts(" Hz, "); con_putdec((uint32_t)samples_per_burst); con_puts(" samples/burst\n");
    return 1;
}

/* Live LED-drive control for the app's AGC. Bank0 0x14 = green DAC, 0..0x3f
 * (the vendor tables cap it at 0x3f via 0x2a). Latched with the update flag
 * like every other bank-0 change. The value persists across measurements so
 * the next one starts where the last one settled. */
int pah8011_set_led_dac(uint8_t v)
{
    if (v > 0x3Fu) v = 0x3Fu;
    if (v < 0x04u) v = 0x04u;
    s_led_dac = v;
    if (!s_present) return 0;
    int ok = wr_b(0, 0x14, v);
    ok &= update_flag();
    return ok;
}
uint8_t pah8011_get_led_dac(void) { return s_led_dac; }

/* Unconditional OFF for suspend: engine stopped, PPG off, LED gates cleared,
 * chip in shutdown - whatever state the app left it in. A sleep entered from
 * the Heart screen used to leave the sampling engine and its LEDs running
 * for the whole sleep (C2 warm at the sensor after 3 h, 2026-09-06). */
int pah8011_force_off(void)
{
    if (!s_present) return 1;               /* nothing to turn off */
    s_bank = -1;
    /* CHECK THE WRITES (2026-09-06). This used to discard every return code,
     * so a bus that was busy or arbitrated away at sleep entry left the PPG
     * LEDs burning for the whole sleep and logged nothing. The caller reports
     * a non-zero result once per boot. */
    int ok = 1;
    ok &= wr_b(1, 0x30, 0x00);              /* timing generator off */
    ok &= rmw(0x23, 0x01, 0x00); ok &= rmw(0x25, 0x01, 0x00);
    ok &= turn_led(0);
    ok &= update_flag();
    ok &= wr_b(4, 0x69, 1);                 /* shutdown */
    s_running = 0;
    return ok;
}

void pah8011_stop(void)
{
    if (!s_present) return;
    wr_b(1, 0x30, 0x00);                    /* stop the timing generator */
    rmw(0x23, 0x01, 0x00); rmw(0x25, 0x01, 0x00);
    turn_led(0);                            /* LEDs off */
    update_flag();
    wr_b(4, 0x69, 1);                       /* shutdown */
    s_running = 0;
    con_puts("hr: stopped\n");
}

/* One-line register snapshot for bring-up: bank1 PPG enables / rate / FIFO
 * threshold, bank2 int status / FIFO count / touch / package, bank4 power,
 * and the INT pin (gpio110). Restores the bank cache afterwards. */
void pah8011_diag(void)
{
    uint8_t b1[8] = {0}, st = 0, cnt[2] = {0}, tch = 0, pkg = 0, pwr = 0, id = 0;
    int ok = 1;
    ok &= rd_b(0, 0x00, &id, 1);
    ok &= rd_b(1, 0x23, &b1[0], 1); ok &= rd_b(1, 0x25, &b1[1], 1); ok &= rd_b(1, 0x12, &b1[2], 1);
    ok &= rd_b(1, 0x56, &b1[3], 1); ok &= rd_b(1, 0x57, &b1[4], 1); ok &= rd_b(1, 0x36, &b1[5], 1);
    ok &= rd_b(1, 0x24, &b1[6], 1); ok &= rd_b(1, 0x00, &b1[7], 1);
    uint8_t r30 = 0, r37 = 0; ok &= rd_b(1, 0x30, &r30, 1); ok &= rd_b(1, 0x37, &r37, 1);
    ok &= rd_b(2, 0x1B, &st, 1); ok &= rd_b(2, 0x25, cnt, 2); ok &= rd_b(2, 0x00, &tch, 1); ok &= rd_b(2, 0x4A, &pkg, 1);
    uint8_t led[3] = {0};
    ok &= rd_b(0, 0x70, &led[0], 1); ok &= rd_b(0, 0x71, &led[1], 1); ok &= rd_b(0, 0x72, &led[2], 1);
    ok &= rd_b(4, 0x69, &pwr, 1);
    con_puts("hr: diag ok="); con_putdec((uint32_t)ok);
    con_puts(" id="); con_puthex(id);
    con_puts(" b1[00="); con_puthex(b1[7]); con_puts(" 23="); con_puthex(b1[0]); con_puts(" 25="); con_puthex(b1[1]);
    con_puts(" 12="); con_puthex(b1[2]); con_puts(" 56/57="); con_puthex(b1[3]); con_puts("/"); con_puthex(b1[4]);
    con_puts(" 36="); con_puthex(b1[5]); con_puts(" 24="); con_puthex(b1[6]); con_puts(" 30="); con_puthex(r30); con_puts(" 37="); con_puthex(r37);
    con_puts("] b2[int="); con_puthex(st); con_puts(" fifo="); con_putdec((uint32_t)(cnt[0] | (cnt[1] << 8)));
    con_puts(" touch="); con_puthex(tch); con_puts(" pkg="); con_puthex(pkg);
    con_puts(" led70/71/72="); con_puthex(led[0]); con_puts("/"); con_puthex(led[1]); con_puts("/"); con_puthex(led[2]);
    con_puts("] b4[69="); con_puthex(pwr); con_puts("] int_pin="); con_putdec((uint32_t)tlmm_in(110u));
    con_puts("\n");
}

/* Drain one burst if ready. Fills ch1/ch2 (max entries each) with 26-bit PPG
 * values, sets *touch. Returns the sample count, 0 when nothing is ready, <0
 * on a bus/checksum error. */
int pah8011_poll(int32_t *ch1, int32_t *ch2, int max, int *touch)
{
    uint8_t st = 0, b[4];
    int n = 0;
    if (!s_running) return 0;
    if (!rd_b(2, 0x1B, &st, 1)) return -1;
    if (touch) { uint8_t t = 0; if (rd_b(2, 0x00, &t, 1)) *touch = t & 1u; }
    if (st == 0) return 0;
    if (st & 0x01u) {
        uint16_t cnt = 0;
        if (rd_b(2, 0x25, b, 2)) cnt = (uint16_t)(b[0] | (b[1] << 8));
        if (cnt >= (uint16_t)s_words) {
            uint32_t xr = 0, chk = 0;
            unsigned bytes = (unsigned)s_words * 4u;
            if (!rd_b(3, 0x00, s_buf, bytes)) { wr_b(2, 0x1B, st); return -1; }
            if (rd_b(2, 0x1C, b, 4)) chk = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
            for (unsigned i = 0; i < bytes; i += 4) {
                uint32_t w = (uint32_t)s_buf[i] | ((uint32_t)s_buf[i+1] << 8) | ((uint32_t)s_buf[i+2] << 16) | ((uint32_t)s_buf[i+3] << 24);
                xr ^= w;
                if ((i / 8) < (unsigned)max) {
                    int32_t v = (int32_t)(w >> 6);
                    if (i & 4u) ch2[i / 8] = v; else ch1[i / 8] = v;
                }
            }
            n = s_words / 2; if (n > max) n = max;
            if (xr != chk) { con_puts("hr: fifo checksum mismatch\n"); n = -2; }
        }
    }
    wr_b(2, 0x1B, st);                      /* clear what we saw */
    if (st & 0x0Cu) { con_puts("hr: sensor error status="); con_puthex(st); con_puts("\n"); }
    return n;
}

#endif /* PLAT_SOC_MSM8909 */
