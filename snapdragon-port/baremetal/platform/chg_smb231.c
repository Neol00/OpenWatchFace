/* chg_smb231.c — TicWatch C2 / S2 charger + gauge over BLSP1 QUP4 I2C.
 *
 * THE PMIC IS NOT THE CHARGER ON THESE WATCHES. The C2's own boot-partition
 * DTB (dtbs/skipjack-stock.dts) enables, on i2c@78b8000
 * (BLSP1 QUP4, gpio14/15 = blsp_i2c4, 375 kHz in the DT):
 *     smb231@12   compatible "qcom,smb231-charger"   (no status = enabled)
 *     st-fg@70    compatible "st,stc3117"            (no status = enabled)
 * VBUS from the charging pads goes to the SMB231's USBIN, not to the PM8916
 * linear charger, which is why the LBC's usbin-valid bit never followed the
 * cable (2026-09-04/06 logs) and why the PHY's session-valid comparator never
 * saw it either (msm_otg runs with PMIC OTG control on this SoC). Both of the
 * sources pmic_fg.c had been using were blind by construction.
 *
 * Register map and decisions are verbatim from the vendor driver
 * drivers/power/smb23x-charger.c (skipjack 3.18 tree):
 *   CHG_STATUS_A 0x3C  USBIN_OV bit6, USBIN_UV bit4, POWER_OK bit2
 *                      -> usb present = !(OV|UV)      (determine_initial_status)
 *   CHG_STATUS_B 0x3D  HOLD_OFF bit3, CHARGE_TYPE bits[2:1] (1 pre, 2 fast,
 *                      3 taper), CHARGE_EN_STS bit0
 *                      -> charging = CHARGE_TYPE != 0 && !HOLD_OFF
 *                                                      (get_prop_batt_status)
 *   IRQ_C_STATUS 0x3A  ITERM bit0, TAPER bit2, RECHG bit4, CHG_ERROR bit6
 *   CMD_REG_0    0x30  CHARGE_EN bit1 (polarity from CFG_REG_4 bit5)
 * Nothing here WRITES the charger: it is configured from its own NV defaults
 * at power-on and the stock kernel only ever toggles CHARGE_EN for thermal
 * reasons. The two pinctrl states the DT asks for are reproduced: smb_stat
 * gpio49 input (the STAT open-drain line, logged only) and smb_susp gpio58
 * driven LOW (USB-suspend pin, low = charger allowed to run).
 *
 * The STC3117 shares the bus and, when the gauge engine is running, gives a
 * signed battery current (LSB 5.88 uV / Rsense, Rsense = 10 mOhm per the
 * vendor platform data) — the first real current sense on an 8909 watch.
 * Read-only as well; the coulomb counter keeps whatever state it has. */
#include "platform.h"
#if defined(PLAT_CHG_SMB231)

#define QUP4_BASE        0x078B8000u
#define SMB231_ADDR      0x12u
#define STC3117_ADDR     0x70u

#define SMB_CMD_REG_0    0x30u
#define SMB_IRQ_C        0x3Au
#define SMB_STATUS_A     0x3Cu
#define SMB_STATUS_B     0x3Du
#define SMB_STATUS_C     0x3Eu
#define   USBIN_OV_BIT   (1u << 6)
#define   USBIN_UV_BIT   (1u << 4)
#define   POWER_OK_BIT   (1u << 2)
#define   HOLD_OFF_BIT   (1u << 3)
#define   CHARGE_TYPE_MASK 0x06u
#define   CHARGE_EN_STS  (1u << 0)

#define STC_REG_MODE     0x00u
#define STC_REG_CTRL     0x01u
#define STC_REG_SOC      0x02u
#define STC_REG_CURRENT  0x06u
#define STC_REG_VOLTAGE  0x08u
#define STC_REG_ID       0x18u
#define   STC3117_ID     0x16u
#define   STC_GG_RUN     0x10u

static int s_state = 0;         /* 0 unprobed, 1 ok, -1 absent/failed */
static int s_stc_ok = 0;
static void stc3117_engine_start(void);

static int rd8(uint8_t addr, uint8_t reg, uint8_t *v)
{
    return i2c_bus_xfer(QUP4_BASE, addr, &reg, 1u, v, 1u);
}

static int rd16(uint8_t addr, uint8_t reg, uint16_t *v)
{
    uint8_t b[2] = { 0, 0 };
    if (i2c_bus_xfer(QUP4_BASE, addr, &reg, 1u, b, 2u) < 0) return -1;
    *v = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
    return 0;
}

/* One-time bus + pin bring-up, then a probe of both parts. The pin function
 * number for blsp_i2c4 on gpio14/15 is 2 on this SoC (the same slot
 * blsp_i2c5 occupies on gpio18/19, which the touch bus already proves); the
 * neighbouring selects are tried as well, judged by a real register read. */
static void smb231_probe(void)
{
    static const uint32_t funcs[] = { 2u, 3u, 1u };
    uint8_t v = 0;

    s_state = -1;
    tlmm_cfg(PLAT_SMB_STAT_GPIO, 0u, 0u, 2u, 0);          /* STAT: input, no pull (DT) */
    tlmm_cfg(PLAT_SMB_SUSP_GPIO, 0u, 0u, 2u, 1);          /* SUSP: output LOW (DT)     */
    tlmm_out(PLAT_SMB_SUSP_GPIO, 0);

    for (unsigned i = 0; i < sizeof funcs / sizeof funcs[0]; i++) {
        tlmm_cfg(PLAT_SMB_I2C_SDA_GPIO, funcs[i], 0u, 2u, 0);
        tlmm_cfg(PLAT_SMB_I2C_SCL_GPIO, funcs[i], 0u, 2u, 0);
        if (i2c_bus_init(QUP4_BASE) < 0) { con_puts("smb231: QUP4 init failed\n"); return; }
        if (rd8(SMB231_ADDR, SMB_STATUS_B, &v) == 0) {
            s_state = 1;
            con_puts("smb231: found on QUP4 (gpio14/15 func ");
            con_putdec(funcs[i]); con_puts(") status_b="); con_puthex(v);
            if (rd8(STC3117_ADDR, STC_REG_ID, &v) == 0 && v == STC3117_ID) {
                uint8_t mode = 0;
                s_stc_ok = 1;
                (void)rd8(STC3117_ADDR, STC_REG_MODE, &mode);
                con_puts(" stc3117 id ok mode="); con_puthex(mode);
                con_puts("\n"); stc3117_engine_start(); con_puts("smb231:");
            } else {
                con_puts(" stc3117 absent");
            }
            con_puts("\n");
            return;
        }
    }
    con_puts("smb231: no answer at 0x12 on QUP4 (tried func 2/3/1)\n");
}

/* Charger SUSPEND via the SMB231's SUSP pin (gpio58, DT smb_susp default
 * output-low = charger running). High = the input path is cut and the system
 * runs from the battery while VBUS stays present for the USB PHY, so a log can
 * be streamed while the cell is actually discharging. Sleep-power diagnostics
 * only (-DSLEEP_BATT_DIAG); restored low at sleep exit. */
void smb231_charger_suspend(int on)
{
    if (s_state == 0) smb231_probe();
    tlmm_cfg(PLAT_SMB_SUSP_GPIO, 0u, 0u, 2u, 1);
    tlmm_out(PLAT_SMB_SUSP_GPIO, on ? 1 : 0);
    /* v164: the pin alone did NOT suspend the input (v163 ladder: the cell
     * kept charging at 307 mA through the whole sleep), so the register path
     * is used as well: CMD_REG_0 bit7 = volatile-write permission, bit2 =
     * USB suspend. Readback + status printed so the log proves which of the
     * two worked. */
    if (s_state == 1) {
        uint8_t cmd = 0, a = 0, b = 0;
        (void)rd8(SMB231_ADDR, SMB_CMD_REG_0, &cmd);
        { uint8_t w[2] = { SMB_CMD_REG_0, (uint8_t)((cmd | 0x80u) | (on ? 0x04u : 0u)) };
          if (!on) w[1] &= (uint8_t)~0x04u;
          (void)i2c_bus_xfer(QUP4_BASE, SMB231_ADDR, w, 2u, 0, 0u); }
        timer_delay_ms(20u);
        con_puts("smb231: suspend "); con_putdec((uint32_t)on); con_puts(": cmd0 "); con_puthex(cmd);
        (void)rd8(SMB231_ADDR, SMB_CMD_REG_0, &cmd); con_puts(" -> "); con_puthex(cmd);
        (void)rd8(SMB231_ADDR, SMB_STATUS_A, &a); (void)rd8(SMB231_ADDR, SMB_STATUS_B, &b);
        con_puts(" stA="); con_puthex(a); con_puts(" stB="); con_puthex(b); con_puts("\n");
    }
}

/* The STC3117 only converts current while its gauge engine runs (MODE.GG_RUN).
 * The stock kernel starts it; we do the same once, mixed mode (0x10), which
 * touches no calibration register. */
static void stc3117_engine_start(void)
{
    uint8_t mode = 0;
    if (!s_stc_ok) return;
    if (rd8(STC3117_ADDR, STC_REG_MODE, &mode) < 0 || (mode & STC_GG_RUN)) return;
    { uint8_t w[2] = { STC_REG_MODE, (uint8_t)STC_GG_RUN };
      if (i2c_bus_xfer(QUP4_BASE, STC3117_ADDR, w, 2u, 0, 0u) == 0) con_puts("stc3117: gauge engine started (GG_RUN)\n");
      else con_puts("stc3117: GG_RUN write failed\n"); }
}

int smb231_ready(void)
{
    if (s_state == 0) smb231_probe();
    return s_state == 1;
}

/* 1 = VBUS on the pads, 0 = not, -1 = no charger / read failed. */
int smb231_usb_present(void)
{
    uint8_t a = 0;
    if (!smb231_ready()) return -1;
    if (rd8(SMB231_ADDR, SMB_STATUS_A, &a) < 0) return -1;
    return (a & (USBIN_OV_BIT | USBIN_UV_BIT)) ? 0 : 1;
}

/* 1 = the charger is pushing current (pre/fast/taper), 0 = not, -1 = unknown. */
int smb231_charging(void)
{
    uint8_t b = 0;
    if (!smb231_ready()) return -1;
    if (rd8(SMB231_ADDR, SMB_STATUS_B, &b) < 0) return -1;
    if (b & HOLD_OFF_BIT) return 0;
    return (b & CHARGE_TYPE_MASK) ? 1 : 0;
}

/* Battery current from the STC3117, + = discharging (fg_batt_ma convention;
 * the chip itself reports charge as positive). -32768 when the gauge engine
 * is not running or the part is absent. 14-bit two's complement, 5.88 uV per
 * LSB across 10 mOhm = 0.588 mA per LSB. */
int smb231_batt_ma(void)
{
    uint16_t raw = 0; uint8_t mode = 0; int32_t v;
    if (!smb231_ready() || !s_stc_ok) return -32768;
    if (rd8(STC3117_ADDR, STC_REG_MODE, &mode) < 0 || !(mode & STC_GG_RUN)) return -32768;
    if (rd16(STC3117_ADDR, STC_REG_CURRENT, &raw) < 0) return -32768;
    v = (int32_t)(raw & 0x3FFFu);
    if (v & 0x2000) v -= 0x4000;
    return (int)(-(v * 588) / 1000);
}

/* STC3117 state of charge in 1/512 % units (REG_SOC 0x02, 16-bit), -1 when
 * the gauge is not running. Survives a cluster collapse (the gauge keeps
 * counting while the SoC is off), so SOC before/after a sleep gives the true
 * average sleep current. */
/* Negative = why: -1 gauge not probed, -2 MODE read failed (bus), -3 GG_RUN
 * was clear (the chip lost power: engine restarted, SOC meaningless until it
 * settles), -4 SOC read failed. v198. */
int smb231_soc_x512(void)
{
    uint16_t raw = 0; uint8_t mode = 0;
    if (!smb231_ready() || !s_stc_ok) return -1;
    if (rd8(STC3117_ADDR, STC_REG_MODE, &mode) < 0) return -2;
    if (!(mode & STC_GG_RUN)) { con_puts("stc3117: GG_RUN clear (gauge lost power?) mode="); con_puthex(mode); con_puts(" -> restarting the engine\n"); stc3117_engine_start(); return -3; }
    if (rd16(STC3117_ADDR, STC_REG_SOC, &raw) < 0) return -4;
    return (int)raw;
}
/* Log line, printed by pmic_fg.c only when something changed. */
void smb231_log_state(void)
{
    uint8_t a = 0, b = 0, c = 0, irqc = 0, cmd = 0;
    if (s_state != 1) { con_puts("smb231: absent"); return; }
    (void)rd8(SMB231_ADDR, SMB_STATUS_A, &a);
    (void)rd8(SMB231_ADDR, SMB_STATUS_B, &b);
    (void)rd8(SMB231_ADDR, SMB_STATUS_C, &c);
    (void)rd8(SMB231_ADDR, SMB_IRQ_C, &irqc);
    (void)rd8(SMB231_ADDR, SMB_CMD_REG_0, &cmd);
    con_puts("smb231 stA="); con_puthex(a);
    con_puts(" stB="); con_puthex(b);
    con_puts(" stC="); con_puthex(c);
    con_puts(" irqC="); con_puthex(irqc);
    con_puts(" cmd0="); con_puthex(cmd);
    con_puts(" stat49="); con_putdec((uint32_t)tlmm_in(PLAT_SMB_STAT_GPIO));
    if (s_stc_ok) {
        uint16_t mv = 0, soc = 0;
        (void)rd16(STC3117_ADDR, STC_REG_VOLTAGE, &mv);
        (void)rd16(STC3117_ADDR, STC_REG_SOC, &soc);
        con_puts(" stc-mv="); con_putdec(((uint32_t)mv * 22u) / 10u);   /* 2.2 mV/LSB */
        con_puts(" stc-soc="); con_putdec((uint32_t)soc / 512u);        /* 1/512 % */
        con_puts(" stc-ma="); { int ma = smb231_batt_ma(); if (ma == -32768) con_puts("?"); else { if (ma < 0) { con_puts("-"); ma = -ma; } con_putdec((uint32_t)ma); } }
    }
}

#endif /* PLAT_CHG_SMB231 */
