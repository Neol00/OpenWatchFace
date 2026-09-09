/* tlmm.c — Qualcomm TLMM (Top-Level Mode Mux) pin control, both watches.
 *
 * Why this exists (2026-08-04): the touch bring-up got the BLSP1 QUP4 clocks
 * running (gcc_blsp.c) and the QUP protocol engine answers, but the Raydium
 * stayed mute — aboot has no reason to route the I2C pins to the QUP, so
 * SDA/SCL were still in reset-default GPIO mode and no start condition ever
 * reached the chip.
 *
 * Ground truth from the device dump + kernel:
 *   pinctrl@1000000        compatible qcom,sdm429w-pinctrl (standard msm TLMM,
 *                          0x1000 bytes per pin)
 *   i2c_4_active           pins gpio14 gpio15, function blsp_i2c4,
 *                          drive-strength 2 mA, bias-pull-up
 *   pinctrl-sdm429w.c      PINGROUP(14, blsp_spi4, blsp_uart4, blsp_i2c4,...)
 *                          -> blsp_i2c4 is listed 3rd -> FUNC_SEL value 3
 *                          (slot 0 is plain GPIO)
 *   raydium@39             reset-gpio = TLMM 64 (active flags 0),
 *                          irq-gpio = TLMM 65, hard-reset-delay 5 ms
 *
 * Register layout (pinctrl-msm family):
 *   GPIO_CFG(n)    base + 0x1000*n + 0x0 : pull[1:0] func[5:2] drv[8:6] oe[9]
 *   GPIO_IN_OUT(n) base + 0x1000*n + 0x4 : in[0] out[1]
 */
#include "platform.h"
/* The Gen 4 (msm8909w) uses the identical block at the identical address —
 * its own dumped device tree has pinctrl@1000000 "qcom,msm8909-pinctrl", and
 * pinctrl-msm8909.c uses the same REG_BASE 0 / REG_SIZE 0x1000 geometry. Only
 * the pin numbers and function slots differ, so just tlmm_touch_setup() is
 * per-board. */
#if defined(PLAT_SOC_MSM)

#define TLMM_BASE      0x01000000u
#define TLMM_CFG(n)    (TLMM_BASE + 0x1000u * (n))
#define TLMM_IO(n)     (TLMM_BASE + 0x1000u * (n) + 4u)

#define TLMM_PULL_NONE 0u
#define TLMM_PULL_DOWN 1u
#define TLMM_PULL_KEEP 2u
#define TLMM_PULL_UP   3u

/* drv is in 2 mA steps: reg value = mA/2 - 1 */
void tlmm_cfg(uint32_t pin, uint32_t func, uint32_t pull, uint32_t drv_ma,
              int output)
{
    uint32_t v = (pull & 3u) | ((func & 0xFu) << 2)
               | (((drv_ma / 2u - 1u) & 7u) << 6)
               | (output ? (1u << 9) : 0u);
    mmio_write(TLMM_CFG(pin), v);
}

void tlmm_out(uint32_t pin, int hi)
{
    mmio_write(TLMM_IO(pin), hi ? 2u : 0u);
}

int tlmm_in(uint32_t pin)
{
    return (int)(mmio_read(TLMM_IO(pin)) & 1u);
}

#if defined(PLAT_SOC_MSM8909)
/* msm8909w touch wiring. Shared by the Fossil Gen 4 and the TicWatch C2, which
 * is less of a coincidence than it looks: the pin mux is a property of the SoC
 * and the bus (BLSP1 QUP5 is gpio18/19 function 2 on every msm8909), and both
 * watches' DTs place touch reset on GPIO 12 and touch INT on GPIO 13. The
 * CONTROLLERS differ completely — Raydium RM_TS at 0x39 on the Gen 4, ITE
 * IT7260 at 0x46 on the C2 — but that difference lives in the touch driver,
 * not in the pins. Everything below is parameterised through the board header,
 * so it needs no per-watch branch.
 *
 * Gen 4 facts, all four read out of that watch's own DTB:
 *   i2c_5_active   mux pins gpio19, gpio18, function "blsp_i2c5",
 *                  drive-strength 2 mA, BIAS-DISABLE (no internal pull — the
 *                  board carries its own I2C pull-ups; forcing a pull-up here
 *                  would fight them)
 *   pinctrl-msm8909.c PINGROUP(18, blsp_spi5, blsp_i2c5, ...) — slot 0 is
 *                  plain GPIO, so blsp_i2c5 is FUNC_SEL 2
 *   raydium@39     reset-gpio TLMM 12, irq-gpio TLMM 13 (interrupts
 *                  <0x0d 0x2002> = GPIO 13, active low)
 * The reset line is DRIVEN HIGH and not pulsed, for the reason the Gen 6
 * learned the hard way: the stock driver's probe only ever does
 * gpio_direction_output(rst, 1); the controller self-boots at power-on and
 * keeps running across OS boots, and an unnecessary reset can leave it in a
 * degraded scan state. touch_init() pulses it only if the chip stays mute. */
void tlmm_touch_setup(void)
{
    tlmm_cfg(18u, 2u, TLMM_PULL_NONE, 2u, 0);   /* SDA */
    tlmm_cfg(19u, 2u, TLMM_PULL_NONE, 2u, 0);   /* SCL */

    tlmm_cfg(PLAT_TOUCH_RESET_GPIO, 0u, TLMM_PULL_NONE, 2u, 1);
    tlmm_out(PLAT_TOUCH_RESET_GPIO, 1);

    tlmm_cfg(PLAT_TOUCH_IRQ_GPIO, 0u, TLMM_PULL_UP, 2u, 0);

    bdiag_puts("tlmm: blsp_i2c5 on gpio18/19, touch reset gpio ");
    bdiag_putdec(PLAT_TOUCH_RESET_GPIO);
    bdiag_puts(" held HIGH, INT gpio "); bdiag_putdec(PLAT_TOUCH_IRQ_GPIO);
    bdiag_puts(" level="); bdiag_putdec((uint32_t)tlmm_in(PLAT_TOUCH_IRQ_GPIO));
    bdiag_puts("\n");
}

void tlmm_touch_reset_pulse(void)
{
    /* Kernel timing: raydium,hard-reset-delay 5 ms, then the chip's own boot.
     * The C2's ITE asks for less (ite,reset-delay = 20 ms total); 6 ms low
     * plus a 120 ms settle satisfies both, so the pulse stays shared until
     * some watch proves it needs its own. */
    tlmm_out(PLAT_TOUCH_RESET_GPIO, 0);
    timer_delay_ms(6u);
    tlmm_out(PLAT_TOUCH_RESET_GPIO, 1);
    timer_delay_ms(120u);
    bdiag_puts("tlmm: touch reset pulsed (fallback)\n");
}
#else
/* Route the touch I2C pins + pulse the Raydium's reset line. Called from
 * touch_init() before the first probe; idempotent. */
void tlmm_touch_setup(void)
{
    /* SDA/SCL -> blsp_i2c4 (func 3), pull-up, 2 mA — the i2c_4_active state */
    tlmm_cfg(14u, 3u, TLMM_PULL_UP, 2u, 0);
    tlmm_cfg(15u, 3u, TLMM_PULL_UP, 2u, 0);

    /* Raydium reset line: DRIVEN HIGH, NOT PULSED (2026-08-06). The stock
     * kernel probe only ever does gpio_direction_output(rst, 1) — it NEVER
     * resets the chip on a normal boot; the chip self-boots at power-on and
     * keeps running across OS boots. Our every-boot hard reset was the one
     * thing stock never does, and post-reset the chip lands in a degraded
     * ambient-only scan state no documented command gets it out of. Leave it
     * running; touch_init pulses reset (tlmm_touch_reset_pulse) only as a
     * fallback when the chip does not answer the probe. */
    tlmm_cfg(64u, 0u, TLMM_PULL_NONE, 2u, 1);
    tlmm_out(64u, 1);

    /* Raydium INT: raydium,irq-gpio = <&tlmm 0x41 0x2002> -> GPIO 65, active
     * low. Input, no drive, pull-up (the chip drives it low open-drain when a
     * report is ready and releases it once the report is read+acked). Never
     * wired until 2026-08-06, which is why the polled driver was racing the
     * chip's report-load instead of being told when one was ready. */
    tlmm_cfg(PLAT_TOUCH_INT_GPIO, 0u, TLMM_PULL_UP, 2u, 0);

    bdiag_puts("tlmm: i2c4 pins muxed, touch reset held HIGH (no pulse), INT gpio ");
    bdiag_putdec(PLAT_TOUCH_INT_GPIO);
    bdiag_puts(" level="); bdiag_putdec((uint32_t)tlmm_in(PLAT_TOUCH_INT_GPIO));
    bdiag_puts("\n");
}

/* Fallback only: kernel-timing hard reset pulse (1 -> 0 -> 5 ms -> 1, then
 * the chip's ~360 ms boot). Used when the un-reset chip does not answer. */
void tlmm_touch_reset_pulse(void)
{
    tlmm_out(64u, 0);
    timer_delay_ms(6u);
    tlmm_out(64u, 1);
    timer_delay_ms(120u);
    bdiag_puts("tlmm: touch reset pulsed (fallback)\n");
}

#endif
#endif /* PLAT_SOC_MSM */
