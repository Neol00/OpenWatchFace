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
 *   CMD_REG_0    0x30  CHARGE_EN bit1 (polarity from CFG_REG_4 bit5),
 *                      bit7 volatile-write permission, bit2 USB suspend
 *   CFG_REG_2    0x02  fast-charge current, bits[2:0] index into
 *                      {100,250,300,370,500,600,700,1000} mA
 *   CFG_REG_3    0x03  float voltage, bits[4:0], mv = 3480 + 40 * n
 *
 * THE CELL IS A 4.4 V ONE AND THE CHARGER DOES NOT KNOW IT BY DEFAULT.
 * This file used to write nothing at all, leaving the SMB231 on its power-on
 * defaults -- which float at about 4.2 V. The stock DT says otherwise:
 *     qcom,float-voltage-mv = <0x1130>   = 4400 mV   (skipjack AND tunny)
 * so a watch on our firmware stopped around 4.2 V and reported "charging" that
 * never completed: roughly the top 15% of the cell was simply unreachable. The
 * stock kernel programs 4400 from the DT, per JEITA temperature zone. The two pinctrl states the DT asks for are reproduced: smb_stat
 * gpio49 input (the STAT open-drain line, logged only) and smb_susp gpio58
 * driven LOW (USB-suspend pin, low = charger allowed to run).
 *
 * The STC3117 shares the bus and, when the gauge engine is running, gives a
 * signed battery current (LSB 5.88 uV / Rsense, Rsense = 10 mOhm per the
 * vendor platform data) — the first real current sense on an 8909 watch.
 * Read-only as well; the coulomb counter keeps whatever state it has. */
#include "platform.h"
#if defined(PLAT_CHG_SMB231)

void smb231_apply_jeita(int force);   /* defined below; probe calls it once the parts answer */
static void smb231_apply_input_cfg(void);  /* defined below; probe calls it before the JEITA apply */
static int  smb_cfg_write(uint8_t reg, uint8_t mask, uint8_t val);
static uint8_t  s_jeita_zone = 0xFF;  /* last applied JEITA zone, 0xFF = none yet */
static uint32_t s_jeita_next_ms;      /* rate limit for the poll-path re-evaluation */

#define QUP4_BASE        0x078B8000u
#define SMB231_ADDR      0x12u
#define STC3117_ADDR     0x70u

#define SMB_CFG_REG_0    0x00u
#define   USBIN_ICL_MASK   0x1Cu   /* bits[4:2], index into k_usbin_icl_ma */
#define   USBIN_ICL_SHIFT  2u
#define   ITERM_MASK       0x03u   /* bits[1:0] -- NOT ours to touch */
#define SMB_CFG_REG_2    0x02u
#define SMB_CFG_REG_3    0x03u
#define SMB_CFG_REG_4    0x04u
#define   CHG_EN_ACTIVE_LOW_BIT (1u << 5)   /* cei,chg-en-active: 0 = active HIGH */
/* CFG_REG_7 and CMD_REG_1 are DELIBERATELY not written on this board -- see the
 * warning in smb231_apply_input_cfg(). Kept named so the next reader knows what
 * they are and why they are absent. */
#define SMB_CFG_REG_7    0x07u
#define   USB1_5_PIN_CNTRL_BIT (1u << 7)
#define   USB_AC_PIN_CNTRL_BIT (1u << 6)
#define   CHG_EN_PIN_CNTRL_BIT (1u << 5)
#define   SUSPEND_SW_CNTRL_BIT (1u << 3)
#define SMB_CMD_REG_0    0x30u
#define   CHARGE_EN_BIT    (1u << 1)
#define   USB_SUSPEND_BIT  (1u << 2)
#define SMB_CMD_REG_1    0x31u
#define   USB500_MODE_BIT  (1u << 1)
#define   USBAC_MODE_BIT   (1u << 0)
#define SMB_IRQ_C        0x3Au
#define SMB_STATUS_A     0x3Cu
#define SMB_STATUS_B     0x3Du
#define SMB_STATUS_C     0x3Eu
#define SMB_AICL_STATUS  0x3Fu   /* bit6 AICL_DONE; bits[5:0] the input limit IN EFFECT */
#define   AICL_DONE_STS_BIT (1u << 6)
#define   USB1_LIMIT_BIT    (1u << 5)   /* USB100 mode: 100 mA, CFG_REG_0's ICL ignored */
#define   USB5_LIMIT_BIT    (1u << 4)   /* USB500 mode: 500 mA */
#define   AICL_RESULT_MASK  0x0Fu       /* HC mode: 0x8 75, 0x1 300, 0x2 500, 0x3 650, 0x4 900, 0x5 1000, 0x6 1500 */
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
#define STC_REG_TEMP     0x0Au   /* signed 8-bit, 1 C/LSB (stc3117_battery.c) */
#define STC_REG_ID       0x18u
#define   STC3117_ID     0x16u
#define   STC_GG_RUN     0x10u
#define   STC_FORCE_CC   0x20u   /* MODE: leave voltage mode now (stock STC311x_ForceCC) */
#define   STC_CTRL_GG_VM 0x04u   /* CTRL: 1 = voltage mode active, REG_CURRENT not converted */
#define STC_RELAX_THRES_STOCK 4u
#define   STC_CTRL_BATFAIL 0x08u  /* CTRL latched flags, stock M_RST 0x9800 >> 8 */
#define   STC_CTRL_PORDET  0x10u
#define   STC_CTRL_UVLOD   0x80u

static int s_state = 0;         /* 0 unprobed, 1 ok, -1 absent/failed */
static int s_stc_ok = 0;
static void stc3117_engine_start(void);

/* Every charger/gauge transfer goes through here. Sleep gates the QUP core clocks
 * (gcc_blsp_sleep); a transfer on the unclocked QUP4 while the USB log is attached
 * (chg_usb_present() logs this part's state) reset the watch. Refuse it instead. */
static int qup4_xfer(uint8_t addr, const uint8_t *w, uint32_t wn, uint8_t *r, uint32_t rn)
{
    if (gcc_blsp_asleep()) return -1;
    return i2c_bus_xfer(QUP4_BASE, addr, w, wn, r, rn);
}

static int rd8(uint8_t addr, uint8_t reg, uint8_t *v)
{
    return qup4_xfer( addr, &reg, 1u, v, 1u);
}

static int rd16(uint8_t addr, uint8_t reg, uint16_t *v)
{
    uint8_t b[2] = { 0, 0 };
    if (qup4_xfer( addr, &reg, 1u, b, 2u) < 0) return -1;
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
            s_jeita_zone = 0xFF; s_jeita_next_ms = 0;   /* force the first apply */
            if (rd8(STC3117_ADDR, STC_REG_ID, &v) == 0 && v == STC3117_ID) {
                uint8_t mode = 0;
                s_stc_ok = 1;
                (void)rd8(STC3117_ADDR, STC_REG_MODE, &mode);
                con_puts(" stc3117 id ok mode="); con_puthex(mode);
                con_puts("\n"); stc3117_engine_start(); con_puts("smb231:");
            } else {
                con_puts(" stc3117 absent (id read failed or wrong id)");
                s_state = -1;                        /* v415: retry the whole probe, the gauge is the point */
                con_puts("\n"); return;
            }
            con_puts("\n");
            /* How much the charger may draw FROM THE CABLE, before anything
             * about the cell. Nothing here programmed it until 2026-09-17 and
             * the part was left on its power-on default (USB100), which is the
             * whole input budget for BOTH the running watch and the charge:
             * once the system drew about that much, the charger stayed in a
             * real charge phase -- CHARGE_TYPE non-zero, so the watch honestly
             * reported "charging" -- while the cell quietly DISCHARGED at a
             * few mA on the cable. Stock programs cei,max-ac-current-ma = 500
             * (skipjack AND tunny) and firefish the same. */
            smb231_apply_input_cfg();
            /* Program the float voltage now: the charger's power-on default is
             * ~4.2 V and this cell is a 4.4 V one. After the gauge engine, so
             * the temperature register has a conversion to give us. */
            smb231_apply_jeita(1);
            return;
        }
    }
    con_puts("smb231: no answer at 0x12 on QUP4 (tried func 2/3/1) at +"); con_putdec(timer_ms()); con_puts(" ms\n");
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
          (void)qup4_xfer( SMB231_ADDR, w, 2u, 0, 0u); }
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
/* v412: the STC3117 needs the STOCK configuration, not just GG_RUN. drivers/power/stc3117_battery.c
 * STC311x_Startup(): read OCV; MODE=0x01 (GG_RUN off while changing parameters); CC_CNF=85 and
 * VM_CNF=197 (cell characterisation: Cnom 400 mAh, Rint 493 mOhm, Rsense 10 mOhm); CTRL=0x83
 * (clear POR/alarm flags); MODE=0x10 (GG_RUN, mixed mode); rewrite OCV so the SOC restarts from the
 * OCV curve. Without it the chip runs on power-on defaults after any power loss: wrong scale, SOC
 * pinned at its 125% ceiling, "re-estimated upward" over every sleep (v404..v411). Applied when
 * CC_CNF/VM_CNF differ from stock's, when GG_RUN is clear, or when the SOC reads above 100%. */
static int wr16(uint8_t addr, uint8_t reg, uint16_t v)
{
    uint8_t w[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    return qup4_xfer( addr, w, 3u, 0, 0u);
}
/* v493: SetParam EVERY boot, as stock. Stock GasGauge_Start() runs STC311x_SetParam() on both of
 * its paths (Startup and Restore): GG_RUN off (MODE 0x01), parameters incl. CURRENT_THRES written
 * while stopped, CTRL 0x83 (clears the latched PORDET / BATFAIL / UVLOD flags), GG_RUN on (0x10).
 * The OCV rewrite (SOC re-estimated from voltage) is Startup-only: stock does it when a reset flag
 * (M_RST = UVLOD|BATFAIL|PORDET) is set, and we keep our v412 triggers on top. Before v493 the
 * whole sequence was skipped whenever the config looked present, so the 0x15 write hit a running
 * engine and the flags latched by a flat battery were never cleared -- the suspected cause of the
 * constant ~3 mA REG_CURRENT after the C2 ran flat (2026-09-15..17). UNCONFIRMED. The CTRL value
 * read before the clear is logged so the next log shows whether a flag was set. */
static void stc3117_engine_start(void)
{
    uint16_t ocv = 0, cc = 0, vm = 0, soc = 0; uint8_t mode = 0, ctrl = 0;
    (void)rd8(STC3117_ADDR, STC_REG_MODE, &mode); (void)rd8(STC3117_ADDR, STC_REG_CTRL, &ctrl);
    (void)rd16(STC3117_ADDR, 0x0Fu, &cc); (void)rd16(STC3117_ADDR, 0x11u, &vm); (void)rd16(STC3117_ADDR, STC_REG_SOC, &soc);
    int rstflags = (ctrl & (STC_CTRL_UVLOD | STC_CTRL_PORDET | STC_CTRL_BATFAIL)) != 0;
    int need = rstflags || !(mode & STC_GG_RUN) || cc != 85u || vm != 197u || soc > 512u * 100u;
    con_puts("stc3117: mode="); con_puthex(mode); con_puts(" ctrl="); con_puthex(ctrl);
    if (ctrl & STC_CTRL_UVLOD)   con_puts(" [UVLOD]");
    if (ctrl & STC_CTRL_PORDET)  con_puts(" [PORDET]");
    if (ctrl & STC_CTRL_BATFAIL) con_puts(" [BATFAIL]");
    con_puts(" cc_cnf="); con_putdec(cc); con_puts(" vm_cnf="); con_putdec(vm);
    con_puts(" soc="); con_putdec((uint32_t)soc * 100u / 512u); con_puts("(x0.01%)");
    if (need) (void)rd16(STC3117_ADDR, 0x0Du, &ocv);
    /* SetParam: GG_RUN off before changing algorithm parameters (stock comment, verbatim intent). */
    uint8_t m1[2] = { STC_REG_MODE, 0x01u }; (void)qup4_xfer( STC3117_ADDR, m1, 2u, 0, 0u);
    /* CURRENT_THRES: stock RelaxCurrent 20 mA -> (20 << 9) / (24084 / 10) = 4. (v414 wrote 0, v488 4
     * but with the engine running.) */
    { uint8_t t[2] = { 0x15u, STC_RELAX_THRES_STOCK }; (void)qup4_xfer( STC3117_ADDR, t, 2u, 0, 0u); }
    (void)wr16(STC3117_ADDR, 0x0Fu, 85u); (void)wr16(STC3117_ADDR, 0x11u, 197u);
    uint8_t c1[2] = { STC_REG_CTRL, 0x83u }; (void)qup4_xfer( STC3117_ADDR, c1, 2u, 0, 0u);
    uint8_t m2[2] = { STC_REG_MODE, (uint8_t)STC_GG_RUN };
    /* Verify GG_RUN actually latched: a NACKed MODE write left the engine stopped with nothing in
     * the log, and the Draw line showed "gauge stopped" forever. Retry a few times, log the result. */
    { int rc = -1; uint8_t rb = 0;
      for (int t = 0; t < 3; t++) {
          rc = qup4_xfer( STC3117_ADDR, m2, 2u, 0, 0u);
          timer_delay_ms(2u);
          if (rc >= 0 && rd8(STC3117_ADDR, STC_REG_MODE, &rb) == 0 && (rb & STC_GG_RUN)) break;
      }
      con_puts(" run-write rc="); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts(rc < 0 ? "(err)" : "");
      con_puts(" mode readback="); con_puthex(rb); con_puts((rb & STC_GG_RUN) ? " RUNNING" : " STILL STOPPED"); }
    if (!need) { con_puts(" -> SetParam (stop, params, clear flags, run); SOC kept\n"); return; }
    if (ocv) (void)wr16(STC3117_ADDR, 0x0Du, ocv);
    timer_delay_ms(2u);
    (void)rd16(STC3117_ADDR, STC_REG_SOC, &soc);
    con_puts(" -> SetParam + STOCK STARTUP (SOC from OCV "); con_putdec((uint32_t)ocv * 55u / 100u); con_puts(" mV): soc now ");
    con_putdec((uint32_t)soc * 100u / 512u); con_puts("(x0.01%)\n");
}

/* ---- float voltage + JEITA ------------------------------------------------
 *
 * The zone table is the stock DT's, verbatim (skipjack and tunny carry the same
 * numbers). Each entry is the LOWER bound of its zone -- the vendor driver picks
 * zone i for temp in [zone[i], zone[i+1]) -- and the two zero-current zones are
 * where stock refuses to charge at all:
 *
 *     below 0 C  4400 mV    0 mA      cold: charging a Li-ion here damages it
 *     0..6       4400     100
 *     6..11      4400     100
 *     11..21     4400     250
 *     21..46     4400     370
 *     46..60     4100     250        hot: stock drops the float
 *     60+        4100       0
 *
 * TEMPERATURE SOURCE: the STC3117's own sensor (register 0x0A). Stock prefers
 * the PMIC NTC and falls back to exactly this register when the NTC read fails,
 * so it is a sanctioned source rather than a guess -- but it is the gauge chip's
 * temperature, not the cell's, so it lags a fast change.
 *
 * WHEN THE TEMPERATURE CANNOT BE READ AT ALL we use the ROOM-TEMPERATURE zone
 * (JEITA_ZONE_ROOM below), i.e. the full 4400 mV / 370 mA. This is a change of
 * policy, made deliberately on 2026-09-12 at the owner's instruction, and it
 * replaces a 4200 mV / 100 mA "safe ceiling" whose reasoning was "never raise
 * the ceiling on a cell whose temperature is unknown". Why it goes:
 *
 *   - It was not a rare path. The temperature comes from the STC3117, and it
 *     is unreadable whenever the gauge engine is not running -- so the watch
 *     would sit at a 4.2 V ceiling indefinitely, which is the SAME symptom
 *     ("stops at 4.2 V") the 4400 mV programming in this file was added to
 *     cure. The top ~15% of the cell stayed unreachable for a different reason
 *     than before.
 *   - The temperature assumption behind it does not hold on this firmware. It
 *     is guarding against a watch that is already hot; these watches run hot
 *     under Wear OS, and nowhere near it here.
 *
 * The trade is real and worth stating plainly: with no temperature reading,
 * nothing in this file will drop the float or stop the current on a cell that
 * IS hot or below freezing. The SMB231's own hard limits still apply.
 *
 * The zero-current zones are enforced with the USB-suspend bit, the same
 * mechanism smb231_charger_suspend() uses and the one proven to actually stop
 * the current (v164). The watch keeps running from the cell with VBUS present,
 * which is what stock's 0 mA zones amount to. */
static const struct { int16_t from_c; uint16_t fl_mv; uint16_t ma; } k_jeita[] = {
    { -128, 4400,   0 },   /* anything below zone 1 */
    {    0, 4400, 100 },
    {    6, 4400, 100 },
    {   11, 4400, 250 },
    {   21, 4400, 370 },
    {   46, 4100, 250 },
    {   60, 4100,   0 },
};
static const uint16_t k_fastchg_ma[8] = { 100, 250, 300, 370, 500, 600, 700, 1000 };

#define SMB_FLOAT_MIN_MV   3480u
#define SMB_FLOAT_STEP_MV  40u
#define JEITA_ZONE_ROOM    4u      /* 21..46 C: 4400 mV / 370 mA, the stock
                                    * room-temperature zone, and what an
                                    * unreadable temperature now falls back to */

/* Battery temperature in whole degrees C, or -128 when it cannot be read. */
int smb231_batt_temp_c(void)
{
    uint8_t v = 0, mode = 0;
    if (s_state != 1 || !s_stc_ok) return -128;
    if (rd8(STC3117_ADDR, STC_REG_MODE, &mode) < 0) return -128;
    if (rd8(STC3117_ADDR, STC_REG_TEMP, &v) < 0) return -128;
    return (v >= 0x80u) ? (int)v - 256 : (int)v;
}

/* Masked write into an SMB231 config register (volatile writes enabled first). */
static int smb_cfg_write(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t cur = 0, cmd = 0;
    if (rd8(SMB231_ADDR, SMB_CMD_REG_0, &cmd) < 0) return -1;
    { uint8_t w[2] = { SMB_CMD_REG_0, (uint8_t)(cmd | 0x80u) };   /* volatile-write permission */
      if (qup4_xfer( SMB231_ADDR, w, 2u, 0, 0u) < 0) return -1; }
    if (rd8(SMB231_ADDR, reg, &cur) < 0) return -1;
    { uint8_t w[2] = { reg, (uint8_t)((cur & (uint8_t)~mask) | (val & mask)) };
      if (qup4_xfer( SMB231_ADDR, w, 2u, 0, 0u) < 0) return -1; }
    return 0;
}

/* ---- the INPUT side: what may be taken from the cable ---------------------
 *
 * Everything else in this file is about the CELL -- how high to float it, how
 * much to push into it. None of that matters if the charger is not allowed to
 * pull enough through USBIN in the first place, and until 2026-09-17 nothing
 * here programmed the input at all.
 *
 * The failure that exposed it (user, C2): "refuses to charge, measuring 3 mA
 * draw instead of measuring into the cell... losing power while connected via
 * usb, even though it reads charging yes". All three at once is the signature
 * of an input limit that the SYSTEM has already spent: the charger really is
 * in a charge phase, so CHG_STATUS_B's CHARGE_TYPE is non-zero and
 * smb231_charging() correctly answers 1, but nothing is left over and the
 * coulomb counter sees the cell going backwards. It showed up when the watch's
 * own draw rose, not when the charger changed.
 *
 * Stock (drivers/power/smb23x-charger.c, the !QTI_SMB231 branch) programs
 * cei,max-ac-current-ma -- 500 on skipjack, tunny and firefish alike -- into
 * CFG_REG_0 bits[4:2] via find_closest_in_ascendant_list(), which picks the
 * first table entry >= the request, so 500 lands on index 2 exactly. */
static const uint16_t k_usbin_icl_ma[8] = { 100, 300, 500, 650, 900, 1000, 1500, 1500 };

static void smb231_apply_input_cfg(void)
{
    uint8_t rb0 = 0, rb4 = 0, rb7 = 0, cmd = 0;

    if (s_state != 1) return;

    /* WHAT NOT TO DO HERE -- v484 broke charging outright doing it (2026-09-17).
     *
     * v484 also cleared CFG_REG_7's USB1_5 / USB_AC / CHG_EN pin-control bits
     * and set SUSPEND_SW_CNTRL, copied out of smb23x-charger.c. That block sits
     * inside #ifdef QTI_SMB231, and QTI_SMB231 is defined NOWHERE in the
     * skipjack tree -- this watch builds the #else branch, which never touches
     * CFG_REG_7 at all. Clearing CHG_EN_PIN_CNTRL handed charge-enable from the
     * pin to CMD_REG_0's CHARGE_EN bit, which nothing then wrote, so the watch
     * sat on the cable reporting USB present and CHARGE_TYPE 0 -- charging
     * nothing at 3.83 V, and only fastboot charged, because LK programs the
     * part itself. DO NOT WRITE CFG_REG_7 ON THIS BOARD.
     *
     * The live (#else) branch does exactly three things, and so does this. */

    /* 1. CFG_REG_0 (input current limit) IS NOT WRITTEN -- v488 REMOVED IT.
     * v484..v488 wrote index 2 = 500 mA here (cei,max-ac-current-ma). User
     * (2026-09-17): the charge current DOUBLES to 1000 mA on every cable
     * unplug/replug with this write in place. The register is only read back
     * for the log line below. Do not program the input limit again without a
     * current reading that shows what it does on this part. */

    /* CMD_REG_1 (USB100/USB500/USBAC mode) IS NOT WRITTEN. v486 wrote USB500
     * here; the user reports the watch then charging at 1000 mA after a
     * cable unplug/replug (v487, 2026-09-17). The register's effect on this
     * part is evidently not what the vendor names suggest, and the input
     * side goes back to exactly the v485 programming (CFG_REG_0 ICL only).
     * Anything further on the mode select needs a current reading first. */

    /* 2. Enable polarity from cei,chg-en-active = 0 on skipjack, tunny AND
     * firefish = ACTIVE HIGH = CFG_REG_4 bit5 clear. Read back rather than
     * assumed, because the sense of the CHARGE_EN write depends on it. */
    (void)smb_cfg_write(SMB_CFG_REG_4, (uint8_t)CHG_EN_ACTIVE_LOW_BIT, 0u);
    (void)rd8(SMB231_ADDR, SMB_CFG_REG_4, &rb4);

    /* 3. CHARGE_EN itself -- the bit v484 left unwritten. Stock
     * smb23x_charging_enable(): active-low polarity means clear to enable,
     * active-high means set to enable. USB_SUSPEND is cleared in the same
     * write; smb231_apply_jeita() sets it again if a zero-current temperature
     * zone calls for it, and that call comes after this one. */
    if (rd8(SMB231_ADDR, SMB_CMD_REG_0, &cmd) == 0) {
        uint8_t v = (uint8_t)(cmd | 0x80u);            /* volatile-write permission */
        v &= (uint8_t)~USB_SUSPEND_BIT;
        if (rb4 & CHG_EN_ACTIVE_LOW_BIT) v &= (uint8_t)~CHARGE_EN_BIT;
        else                             v |= (uint8_t)CHARGE_EN_BIT;
        { uint8_t w[2] = { SMB_CMD_REG_0, v };
          (void)qup4_xfer( SMB231_ADDR, w, 2u, 0, 0u); }
    }

    (void)rd8(SMB231_ADDR, SMB_CFG_REG_0, &rb0);
    (void)rd8(SMB231_ADDR, SMB_CFG_REG_7, &rb7);
    (void)rd8(SMB231_ADDR, SMB_CMD_REG_0, &cmd);
    con_puts("smb231: input limit ");
    con_putdec((uint32_t)k_usbin_icl_ma[(rb0 & USBIN_ICL_MASK) >> USBIN_ICL_SHIFT]);
    con_puts(" mA readback (cfg0="); con_puthex(rb0);
    con_puts(" cfg4="); con_puthex(rb4);
    con_puts(" cfg7="); con_puthex(rb7);
    con_puts(" cmd0="); con_puthex(cmd);
    con_puts(")\n");
    con_flush();
}

/* v490: smb231_input_ma() and smb231_charge_phase() REMOVED, with the Power
 * app's "Input: N mA limit (phase)" line. The mA figure came from a decode
 * table for AICL_STATUS (0x3F) that I made up; smb23x-charger.c has no such
 * table (its USB5_LIMIT/USB1_LIMIT bits even overlap its 6-bit result mask).
 * The "1000 mA fast charging" and "doubles on replug" the user saw were that
 * decode, not a measured current. Do not put a derived mA on screen again. */

/* Program the float voltage and fast-charge current for the current battery
 * temperature. Cheap and idempotent: it only touches the charger when the zone
 * actually changes, and it is rate-limited so the poll path can call it. */
void smb231_apply_jeita(int force)
{
    int t, zone = 0;
    unsigned i, idx;
    uint16_t fl, ma;
    uint8_t rb = 0;

    if (s_state != 1) return;
    if (!force && s_jeita_next_ms && (int32_t)(timer_ms() - s_jeita_next_ms) < 0) return;
    s_jeita_next_ms = timer_ms() + 15000u;

    t = smb231_batt_temp_c();
    if (t == -128) {
        /* No temperature: charge as if at room temperature (see the header). */
        zone = (int)JEITA_ZONE_ROOM;
    } else {
        for (i = 0; i < sizeof k_jeita / sizeof k_jeita[0]; i++)
            if (t >= k_jeita[i].from_c) zone = (int)i;
    }
    fl = k_jeita[zone].fl_mv; ma = k_jeita[zone].ma;
    if (!force && (uint8_t)zone == s_jeita_zone) return;
    s_jeita_zone = (uint8_t)zone;

    /* Charging allowed at all? The zero-current zones use the input-suspend bit. */
    { uint8_t cmd = 0;
      if (rd8(SMB231_ADDR, SMB_CMD_REG_0, &cmd) == 0) {
          uint8_t w[2] = { SMB_CMD_REG_0, (uint8_t)(cmd | 0x80u) };
          if (ma == 0u) w[1] |= 0x04u; else w[1] &= (uint8_t)~0x04u;
          (void)qup4_xfer( SMB231_ADDR, w, 2u, 0, 0u);
      } }

    if (ma) {
        for (idx = 0; idx + 1u < 8u && k_fastchg_ma[idx + 1u] <= ma; idx++) { }
        (void)smb_cfg_write(SMB_CFG_REG_2, 0x07u, (uint8_t)idx);
    }
    (void)smb_cfg_write(SMB_CFG_REG_3, 0x1Fu,
                        (uint8_t)((fl - SMB_FLOAT_MIN_MV) / SMB_FLOAT_STEP_MV));

    (void)rd8(SMB231_ADDR, SMB_CFG_REG_3, &rb);
    con_puts("smb231: jeita t=");
    if (t == -128) con_puts("?"); else { if (t < 0) { con_puts("-"); con_putdec((uint32_t)-t); } else con_putdec((uint32_t)t); }
    con_puts("C zone="); con_putdec((uint32_t)zone);
    con_puts(" float="); con_putdec((uint32_t)fl);
    con_puts("mV readback="); con_putdec(SMB_FLOAT_MIN_MV + SMB_FLOAT_STEP_MV * (rb & 0x1Fu));
    con_puts("mV current="); con_putdec((uint32_t)ma); con_puts("mA\n");
    con_flush();
}

int smb231_ready(void)
{
    /* v415: the probe was one-shot -- one failed I2C exchange at boot (v407..v410, v414: "sleep-gauge:
     * soc unavailable rc 1") marked charger + gauge absent for the whole run. Retry up to 8 times,
     * 3 s apart, and log every attempt. */
    static uint32_t s_tries, s_next_ms;
    if (s_state == 0 || (s_state == -1 && s_tries < 8u && (int32_t)(timer_ms() - s_next_ms) >= 0)) {
        s_tries++; s_next_ms = timer_ms() + 3000u;
        if (s_tries > 1u) { con_puts("smb231: probe retry "); con_putdec(s_tries); con_puts("\n"); }
        smb231_probe();
    }
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
    /* Re-evaluate the JEITA zone here: this is the call the UI and the main
     * loop already poll, and smb231_apply_jeita() rate-limits itself to once
     * every 15 s and only writes when the zone actually changed. A charge takes
     * an hour, in which a watch can easily move between zones. */
    smb231_apply_jeita(0);
    if (rd8(SMB231_ADDR, SMB_STATUS_B, &b) < 0) return -1;
    if (b & HOLD_OFF_BIT) return 0;
    return (b & CHARGE_TYPE_MASK) ? 1 : 0;
}

/* Battery current from the STC3117, + = discharging (fg_batt_ma convention;
 * the chip itself reports charge as positive). -32768 when the gauge engine
 * is not running or the part is absent. 14-bit two's complement, 5.88 uV per
 * LSB across 10 mOhm = 0.588 mA per LSB. */
/* Why the last smb231_batt_ma() gave no value: 0 ok, 1 charger/gauge not probed, 2 MODE read failed,
 * 3 GG_RUN clear (engine restarted), 4 REG_CURRENT read failed. Shown in the Power app Draw line. */
static int s_batt_ma_why = 1;
int smb231_batt_ma_why(void) { return s_batt_ma_why; }

int smb231_batt_ma(void)
{
    uint16_t raw = 0; uint8_t mode = 0; int32_t v;
    if (!smb231_ready() || !s_stc_ok) { s_batt_ma_why = 1; return -32768; }
    if (rd8(STC3117_ADDR, STC_REG_MODE, &mode) < 0) { s_batt_ma_why = 2; return -32768; }
    /* v498: stock GG_Task restarts the gauge whenever M_RUN is clear ("stc311x in standby mode,
     * rewrite OCV" -> Restore). Ours only did that in smb231_soc_x512() (sleep path), so a gauge left
     * stopped gave -32768 on every current read forever. Rate-limited to one restart per 10 s. */
    if (!(mode & STC_GG_RUN)) {
        static uint32_t s_restart_ms; static int s_restarted;
        s_batt_ma_why = 3;
        if (!s_restarted || (int32_t)(timer_ms() - s_restart_ms) >= 10000) {
            s_restarted = 1; s_restart_ms = timer_ms();
            con_puts("stc3117: GG_RUN clear at current read, mode="); con_puthex(mode); con_puts(" -> restarting the engine\n");
            stc3117_engine_start();
            /* Don't throw away a successful restart: if the engine runs now, read the current. */
            if (rd8(STC3117_ADDR, STC_REG_MODE, &mode) < 0 || !(mode & STC_GG_RUN)) return -32768;
        } else {
            return -32768;
        }
    }
    /* v497: the v488 voltage-mode rule is REMOVED. It returned -32768 whenever CTRL.GG_VM was set and
     * wrote MODE |= FORCE_CC -- neither is stock: stc3117_battery.c never calls STC311x_ForceCC and
     * reports REG_CURRENT as GG->Current in VM mode too (zeroed only for the configured Vmode). Once
     * v493 made the stock 20 mA relaxation threshold take effect, the chip sat in VM mode and every
     * read came back -32768, so the Power app showed the model on every line. */
    if (rd16(STC3117_ADDR, STC_REG_CURRENT, &raw) < 0) { s_batt_ma_why = 4; return -32768; }
    s_batt_ma_why = 0;
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
        { uint8_t mode = 0; if (rd8(STC3117_ADDR, STC_REG_MODE, &mode) == 0) { con_puts(" stc-mode="); con_puthex(mode); con_puts(mode & 0x01u ? " (VOLTAGE mode: SOC is an OCV estimate)" : " (mixed: coulomb counter + OCV at low current)"); } }
    }
}

#endif /* PLAT_CHG_SMB231 */
