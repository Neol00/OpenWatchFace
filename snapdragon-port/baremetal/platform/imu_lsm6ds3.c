/* imu_lsm6ds3.c — ST LSM6DS3 6-axis IMU on the msm8909w watches (Gen 4, C2).
 *
 * Found 2026-09-03 (v54): WHO_AM_I 0x69 over SPI on BLSP1 QUP6's pins,
 * exactly where the C2's modem sensor registry (persist/sensors/sns.reg,
 * SSI SMGR bus 6 / SPI / accel id 0 + gyro id 10) said it was:
 *   gpio8 MOSI, gpio9 MISO, gpio11 SCLK, gpio10 CS, INT1 gpio96.
 * The bus is bit-banged in SPI mode 3 (~100 kHz): a pedometer read is four
 * bytes, so the QUP SPI core is not worth its clock bring-up.
 *
 * What the app uses: the chip's HARDWARE step counter (STEP_COUNTER 0x4B/4C,
 * 16-bit), folded into a 32-bit software total so it never wraps. The chip
 * keeps counting on its own while the AP is suspended, so steps taken during
 * deep sleep land in the counter and are folded in at the next read. No
 * gyro, no wake-on-motion (deliberately: not wanted). */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909)

#define P_MOSI 8u
#define P_MISO 9u
#define P_CS   10u
#define P_CLK  11u

#define R_FUNC_CFG_ACCESS 0x01
#define R_WHO_AM_I        0x0F
#define R_CTRL1_XL        0x10
#define R_CTRL3_C         0x12
#define R_CTRL10_C        0x19
#define R_OUTX_L_XL       0x28
#define R_STEP_COUNTER_L  0x4B
#define R_TAP_CFG         0x58
#define WHO_AM_I_LSM6DS3  0x69

static int s_present, s_pins_ready;
static uint32_t s_total, s_last_hw;

static void dly(void) { uint32_t t0 = timer_us32(); while ((uint32_t)(timer_us32() - t0) < 4u) ; }

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        tlmm_out(P_CLK, 0); tlmm_out(P_MOSI, (out >> i) & 1u); dly();
        tlmm_out(P_CLK, 1); dly();
        in = (uint8_t)((in << 1) | (tlmm_in(P_MISO) & 1u));
    }
    return in;
}
static void pins(void)
{
    if (s_pins_ready) return;
    tlmm_cfg(P_CS, 0u, 0u, 2u, 1);  tlmm_out(P_CS, 1);
    tlmm_cfg(P_CLK, 0u, 0u, 2u, 1); tlmm_out(P_CLK, 1);
    tlmm_cfg(P_MOSI, 0u, 0u, 2u, 1); tlmm_out(P_MOSI, 0);
    tlmm_cfg(P_MISO, 0u, 1u, 2u, 0);
    s_pins_ready = 1;
}
static void wr(uint8_t reg, uint8_t v)
{
    tlmm_out(P_CS, 0); dly(); xfer(reg & 0x7Fu); xfer(v); dly(); tlmm_out(P_CS, 1); dly();
}
static void rd(uint8_t reg, uint8_t *buf, unsigned n)
{
    tlmm_out(P_CS, 0); dly(); xfer(0x80u | reg);
    for (unsigned i = 0; i < n; i++) buf[i] = xfer(0);
    dly(); tlmm_out(P_CS, 1); dly();
}

int lsm6ds3_present(void) { return s_present; }

/* Probe + reset. Leaves the accelerometer OFF. Returns 1 when the chip answers. */
int lsm6ds3_init(void)
{
    uint8_t id = 0;
    pins();
    rd(R_WHO_AM_I, &id, 1);
    if (id != WHO_AM_I_LSM6DS3) {
        con_puts("imu: LSM6DS3 not found, WHO_AM_I="); con_puthex(id); con_puts("\n");
        return 0;
    }
    wr(R_CTRL3_C, 0x05);                   /* SW_RESET | IF_INC */
    timer_delay_ms(20);
    wr(R_CTRL3_C, 0x44);                   /* BDU | IF_INC (4-wire SPI) */
    wr(R_CTRL1_XL, 0x00);                  /* accel power-down */
    s_present = 1;
    con_puts("imu: LSM6DS3 ready (SPI gpio8-11)\n");
    return 1;
}

/* Accelerometer at 26 Hz / 2 g plus the embedded pedometer. Resets the chip's
 * 16-bit step register; the software total is untouched. */
void lsm6ds3_steps_start(void)
{
    if (!s_present) return;
    wr(R_CTRL1_XL, 0x20);                  /* ODR 26 Hz, +-2 g */
    wr(R_TAP_CFG, 0x40);                   /* PEDO_EN */
    wr(R_CTRL10_C, 0x06);                  /* FUNC_EN | PEDO_RST_STEP */
    timer_delay_ms(5);
    wr(R_CTRL10_C, 0x04);                  /* FUNC_EN (reset bit self-clears anyway) */
    s_last_hw = 0;
}

/* Accelerometer only (26 Hz, 2 g), no pedometer: the sleep-tracking session. */
void lsm6ds3_accel_on(void)
{
    if (!s_present) return;
    wr(R_CTRL10_C, 0x00);
    wr(R_TAP_CFG, 0x00);
    wr(R_CTRL1_XL, 0x20);
}

/* Everything off: pedometer, accel. */
void lsm6ds3_stop(void)
{
    if (!s_present) return;
    wr(R_CTRL10_C, 0x00);
    wr(R_TAP_CFG, 0x00);
    wr(R_CTRL1_XL, 0x00);
}

/* Fold the hardware counter into the running total and return it. */
uint32_t lsm6ds3_steps_poll(void)
{
    uint8_t b[2];
    if (!s_present) return s_total;
    rd(R_STEP_COUNTER_L, b, 2);
    uint16_t hw = (uint16_t)(b[0] | (b[1] << 8));
    s_total += (uint16_t)(hw - (uint16_t)s_last_hw);
    s_last_hw = hw;
    return s_total;
}
uint32_t lsm6ds3_steps_total(void) { return s_total; }
void lsm6ds3_steps_set_total(uint32_t t) { s_total = t; }

/* Raw accel sample (LSB @ 2 g = 0.061 mg). 0 = ok. */
int lsm6ds3_accel_read(int16_t xyz[3])
{
    uint8_t b[6];
    if (!s_present) return -1;
    rd(R_OUTX_L_XL, b, 6);
    for (int i = 0; i < 3; i++) xyz[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8));
    return 0;
}

#endif /* PLAT_SOC_MSM8909 */
