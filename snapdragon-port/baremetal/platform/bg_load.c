/* bg_load.c — bring the Gen 5's BG co-processor (QCC1110) firmware up.
 *
 * v253 established, on the watch: TZ answers QSEECOM from bare metal
 * (version 0x00405000, so the 32-bit request structs), "bgapp" is NOT loaded,
 * and the BG itself is off (bg2ap-status gpio97 low). So we must do what
 * Linux does, in this order (subsys-pil-bg.c + peripheral-loader.c):
 *
 *   1. APP_START "bgapp"          -> TZ loads and starts the signed TZ app
 *   2. BGPIL_GET_BG_VERSION       -> harmless: proves we can talk to the app
 *   3. BGPIL_AUTH_MDT             -> the app authenticates bg-wear's metadata
 *   4. BGPIL_IMAGE_LOAD           -> the app pushes the firmware over SPI
 *
 * Steps 1-3 are CONFIRMED on the watch (v256: app_id=3, status=0, metadata
 * AUTHENTICATED) and step 4 reports success too (v257/v258), but the BG stays
 * silent: v258 showed its RAILS WERE NEVER VOTED (rpm_ldo_on rc=-1 twice, the
 * RPM SMD channel is not open at 10 s), so the chip had no power. v259 runs
 * the whole sequence at 35 s, retries the votes, and drives the reset pad
 * through output source 0 rather than the DT's unapplied "func1".
 *
 * STEP 4 IS PIL\'s JOB, DONE HERE. The TZ app is only handed an address and a
 * size; laying the firmware out is the AP\'s work (peripheral-loader.c):
 *   - walk the .mdt program headers; a segment counts when it is PT_LOAD,
 *     is not the hash segment (p_flags bits 24-26 == 2) and has memsz != 0
 *   - region = [min p_paddr, max(p_paddr+p_memsz) rounded up to a page)
 *   - each segment\'s DATA is its own .bNN blob (blob i <-> program header i),
 *     copied to (p_paddr - min_paddr) inside the region, memsz > filesz
 *     zero-filled
 *   - bg-wear\'s only loadable segment says p_paddr 0x80000000, which is OUR
 *     DDR base, so the image MUST be relocated into a buffer we own; the
 *     kernel allows that only when the segment carries the relocatable flag
 *     (p_flags bit 27), and this build refuses to relocate without it.
 *
 * The app request/response structs are pil_bg_intf.h, packed:
 *   req { u8 cmd; u32 address_fw; u32 size_fw; }                = 9 bytes
 *   rsp { u32 cmd; u32 bg_info_len; s32 status; u32 bg_info[100]; }
 * status 0 = success; -2 = BG crashed in TWM (needs a ramdump + continue).
 */
#include "platform.h"
#if defined(PLAT_HAS_BG_QCC1110) && defined(HAVE_BG_FW)

#include <string.h>

extern const unsigned char bgapp_img[];
extern const unsigned int  bgapp_img_len, bgapp_img_mdt_len;
extern const unsigned char bg_wear_img[];
extern const unsigned int  bg_wear_img_len, bg_wear_img_mdt_len;
extern const unsigned int  bg_wear_img_nblob;
extern const unsigned int  bg_wear_img_blob_off[];
extern const unsigned int  bg_wear_img_blob_len[];

#define BGPIL_RAMDUMP          0u
#define BGPIL_IMAGE_LOAD       1u
#define BGPIL_AUTH_MDT         2u
#define BGPIL_DLOAD_CONT       3u
#define BGPIL_GET_BG_VERSION   4u

/* v265 + a disassembly of bgapp.b02 (its command handler at 0x213c):
 *     ldr r0,[r5,#0]   cmd       -- a full 32-bit word, must be <= 4
 *     ldr r7,[r5,#4]   address_fw
 *     ldr r5,[r5,#8]   size_fw
 * The kernel header says "__packed struct tzapp_bg_req { uint8_t cmd; ... }"
 * but with the attribute BEFORE the struct keyword GCC ignores it for a bare
 * definition, so Linux really sends u8 + 3 pad bytes, then two words: 12
 * bytes. Our 9-byte packed copy put the address in the command word
 * (0x5d500002, 0x52d00001), the app took the "unknown command" exit without
 * touching anything and our zeroed response read back as status 0. Every
 * "AUTHENTICATED"/"FIRMWARE LOADED" from v256 to v265 was that. */
struct tzapp_bg_req {
    uint32_t cmd;          /* u8 in Linux, padded; upper bytes must be 0 */
    uint32_t address_fw;
    uint32_t size_fw;
};

struct tzapp_bg_rsp {
    uint32_t cmd;
    uint32_t bg_info_len;
    int32_t  status;
    uint32_t bg_info[100];
} __attribute__((packed));

/* TZ reads these with the MMU off: physically contiguous, page aligned. */
static uint8_t s_app_buf[40 * 1024]   __attribute__((aligned(4096)));
static uint8_t s_mdt_buf[8 * 1024]    __attribute__((aligned(4096)));

/* THE SHARED BUFFER, laid out exactly as the kernel does it (v255 got -4 from
 * the app for every command with two separate buffers and unrounded lengths).
 * qseecom_start_app allocates ONE page and get_cmd_rsp_buffers() then places
 *     req = sbuf,  req_len = QSEECOM_ALIGN(sizeof req)   (0x40 granularity)
 *     rsp = sbuf + req_len,  rsp_len = QSEECOM_ALIGN(sizeof rsp)
 * so the response sits immediately after the request inside the same page and
 * both lengths are multiples of 64. bgapp evidently checks that shape. */
#define QSEE_ALIGN(x)   (((x) + 0x3Fu) & ~0x3Fu)
#define BG_REQ_OFF      0u
#define BG_REQ_LEN      QSEE_ALIGN(sizeof(struct tzapp_bg_req))   /* 12 -> 64  */
#define BG_RSP_OFF      BG_REQ_LEN
#define BG_RSP_LEN      QSEE_ALIGN(sizeof(struct tzapp_bg_rsp))   /* 412 -> 448 */
static uint8_t s_sb[4096] __attribute__((aligned(4096)));

/* The relocated bg-wear image. bg-wear's single loadable segment is 0xa60dc
 * bytes at paddr 0x80000000, so the page-rounded region is 0xa7000. */
static uint8_t s_fw[0xA7000] __attribute__((aligned(4096)));
static uint8_t *s_fw_base = s_fw;   /* where the image really goes (v267 sweep) */

#define MDT_TYPE_MASK    (7u << 24)
#define MDT_TYPE_HASH    (2u << 24)
#define MDT_RELOCATABLE  (1u << 27)
#define PT_LOAD_TYPE     1u

static uint32_t rd32le(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* elf32_phdr: type 0, offset 4, vaddr 8, paddr 12, filesz 16, memsz 20, flags 24 */
static int seg_loadable(const uint8_t *p)
{
    return rd32le(p) == PT_LOAD_TYPE &&
           (rd32le(p + 24) & MDT_TYPE_MASK) != MDT_TYPE_HASH &&
           rd32le(p + 20) != 0u;
}

#define S_REQ ((struct tzapp_bg_req *)(s_sb + BG_REQ_OFF))
#define S_RSP ((struct tzapp_bg_rsp *)(s_sb + BG_RSP_OFF))

static void bg_qup_report(const char *tag);

/* v264 showed the QUP configured for ONE 40-byte exchange after IMAGE_LOAD,
 * i.e. the size of the GET_BG_VERSION register read that ran before it. So
 * every TZ command is now timed (684 KB at 9.6 MHz cannot finish in under
 * ~570 ms) and followed by a controller snapshot: a millisecond-long
 * IMAGE_LOAD is an authentication, not a transfer. */
static int bg_tz_cmd(uint32_t app_id, uint8_t cmd, uint32_t addr, uint32_t size)
{
    int rc;
    uint32_t t0, dt;

    memset(s_sb, 0, sizeof s_sb);
    S_REQ->cmd = cmd;
    S_REQ->address_fw = addr;
    S_REQ->size_fw = size;

    /* A real IMAGE_LOAD keeps the CPU inside TZ (IRQs off) for the whole
     * 684 KB transfer; give the watchdog a fresh window first. */
    wdog_pet();
    t0 = timer_ms();
    rc = qsee_send_cmd(app_id, S_REQ, BG_REQ_LEN, S_RSP, BG_RSP_LEN);
    dt = timer_ms() - t0;
    wdog_pet();
    con_puts("\nbgload:   tz cmd "); con_putdec(cmd); con_puts(" took ");
    con_putdec(dt); con_puts(" ms, status="); con_putdec((uint32_t)S_RSP->status);
    con_puts(" info_len="); con_putdec(S_RSP->bg_info_len); con_puts("\n");
    bg_qup_report("after cmd");
    con_puts("bgload:   ");
    if (rc) return rc;
    if (S_RSP->status != 0) return -(int)(4000 - S_RSP->status);
    return 0;
}

/* ---- the SPI bus itself ---------------------------------------------------
 * v259 did everything right on paper -- rails voted (rc=0), reset released,
 * ap2bg handshake set, IMAGE_LOAD reported ok -- and the BG never answered.
 * The probe line above it says why: gpio12/13/14/15 were STILL func=0, plain
 * GPIOs with pull-downs. Nothing had muxed them to the SPI controller and
 * nothing had clocked it, so the TZ app pushed 684 KB into a disconnected
 * bus and, seeing no error from the QUP, called it success.
 *
 * In Linux that setup belongs to the SPI controller driver, not to PIL: the
 * spi-qup driver applies pinctrl "spi_default"/"spi4_cs0_active" and enables
 * the QUP's iface + core clocks when the bg-spi device probes. FROM-DTB
 * (triggerfish-stock.dts spi4 pinctrl): gpio12/13/15 function blsp_spi4 with
 * drive-strength 0x0c (12 mA), bias-disable; gpio14 the same function at
 * 2 mA. pinctrl-msm8909.c PINGROUP(12..15, blsp_spi4, ...) puts blsp_spi4
 * first after gpio, so the mux value is 1.
 * Clocks (gcc-msm8916.c, same GCC layout): BLSP1 AHB is the voted branch we
 * already use, the QUP4 SPI apps RCG is cmd_rcgr 0x05024 and its branch CBCR
 * is 0x0501C. We pick XO/2 = 9.6 MHz: no M/N/D to get wrong, and below the
 * 16 MHz this slave is rated for. TZ is free to reprogram the rate. */
#define GCC_QUP4_SPI_CMD_RCGR  0x05024u
#define GCC_QUP4_SPI_CBCR      0x0501Cu
#define GCC_RCG_CFG_XO_DIV2    0x0003u   /* src_sel 0 (XO) << 8 | (2*2-1) */
#define BG_SPI_MUX_FUNC        1u        /* blsp_spi4 */

static void bg_pins_report(const char *tag)
{
    static const uint32_t pins[4] = { PLAT_BG_SPI_MOSI, PLAT_BG_SPI_MISO,
                                      PLAT_BG_SPI_CS,   PLAT_BG_SPI_CLK };
    unsigned i;
    con_puts("bgload: spi pins "); con_puts(tag); con_puts(":");
    for (i = 0; i < 4u; i++) {
        uint32_t cfg = mmio_read(0x01000000u + 0x1000u * pins[i]);
        con_puts(" gpio"); con_putdec(pins[i]);
        con_puts("(f"); con_putdec((cfg >> 2) & 0xFu);
        con_puts(cfg & (1u << 9) ? ",oe" : ",-");
        con_puts(",lvl"); con_putdec((uint32_t)tlmm_in(pins[i]));
        con_puts(",cfg="); con_puthex(cfg);
        con_puts(")");
    }
    con_puts("\n");
}

/* Snapshot of the BLSP1 QUP4 controller (0x078B8000) and its clock. The TZ
 * app is supposed to push the image through this QUP; if these registers are
 * still at reset values after IMAGE_LOAD, TZ never ran a transfer and the
 * "ok" was only an authentication result. QUP v2 map from spi_qsd.h. */
#define QUP4_BASE 0x078B8000u
#define GCC_BASE  0x01800000u
static void bg_qup_report(const char *tag)
{
    static const struct { uint16_t off; const char *n; } r[] = {
        { 0x000, "cfg" }, { 0x004, "state" }, { 0x008, "iomode" },
        { 0x018, "oper" }, { 0x01C, "err" },  { 0x030, "hwver" },
        { 0x100, "mxout" }, { 0x104, "mxout_cur" }, { 0x150, "mxwr" },
        { 0x200, "mxin" }, { 0x250, "mxrd" },
        { 0x300, "spicfg" }, { 0x304, "spiio" },
    };
    unsigned i;
    con_puts("bgload: qup4 "); con_puts(tag); con_puts(":");
    for (i = 0; i < sizeof r / sizeof r[0]; i++) {
        con_puts(" "); con_puts(r[i].n); con_puts("=");
        con_puthex(mmio_read(QUP4_BASE + r[i].off));
    }
    con_puts(" rcg_cfg="); con_puthex(mmio_read(GCC_BASE + GCC_QUP4_SPI_CMD_RCGR + 4u));
    con_puts(" cbcr=");    con_puthex(mmio_read(GCC_BASE + GCC_QUP4_SPI_CBCR));
    con_puts("\n");
}

static void bg_spi_bus_up(void)
{
    int rc;

    tlmm_cfg(PLAT_BG_SPI_MOSI, BG_SPI_MUX_FUNC, 0u, 12u, 0);
    tlmm_cfg(PLAT_BG_SPI_MISO, BG_SPI_MUX_FUNC, 0u, 12u, 0);
    tlmm_cfg(PLAT_BG_SPI_CLK,  BG_SPI_MUX_FUNC, 0u, 12u, 0);
    tlmm_cfg(PLAT_BG_SPI_CS,   BG_SPI_MUX_FUNC, 3u,  2u, 0);   /* pull-up, as DT */

    rc = gcc_blsp_qup_spi_up(GCC_QUP4_SPI_CMD_RCGR, GCC_QUP4_SPI_CBCR,
                             GCC_RCG_CFG_XO_DIV2, "QUP4 SPI (bg)");
    con_puts("bgload: spi clocks rc="); con_putdec((uint32_t)rc); con_puts("\n");
    bg_pins_report("after mux");
    bg_qup_report("after mux");
}

#if defined(MSS_BG_AP_RELEASE) || defined(MSS_BG_PIN_HANDOFF)
/* v471: hand gpio12-15 back to blsp_spi4 (QUP4, qcom,shared_ee in the stock DT: AP kernel, TZ bgapp
 * AND the modem's bgcom_spi_mpss all drive the BG through this one QUP). While the AP bit-bangs the
 * pads as GPIOs the modem's QUP4 has no path to the BG. Called by mss_boot before the modem loads. */
void bg_bus_release_to_modem(void)
{
    extern volatile int g_bgcom_ap_released;
    g_bgcom_ap_released = 1;
    bg_spi_bus_up();
    con_puts("bgload: [v471] gpio12-15 muxed to blsp_spi4 + QUP4 clocked -- the BG bus now belongs to the modem (crown OFF)\n");
}
#endif

/* ---- powering the co-processor -------------------------------------------
 * v257 loaded the firmware and the BG still did not raise bg2ap-status. No
 * wonder: nothing had powered it, released its reset, or told it the AP is
 * up. Linux does all three OUTSIDE the PIL path, at driver probe:
 *   bgcom_interface.c (qcom,bg-daemon): enables ssr-reg1 = pm660_l3 and
 *       ssr-reg2 = pm660_l9, and drives qcom,bg-reset-gpio (PM660 GPIO5)
 *       as an output HIGH -- high = out of reset (bg_soft_reset pulses
 *       1 -> 0 -> 1 with 50 ms each way).
 *   subsys-pil-bg.c: drives ap2bg-errfatal (gpio23) LOW and ap2bg-status
 *       (gpio17) HIGH -- "inform BG that AP is up".
 * PM660 GPIO5 is subtype 0x10 (GPIO_LV), where the OUTPUT VALUE is not in
 * MODE_CTL as on older PMICs but in LV_MV_DIG_OUT_SOURCE_CTL (0x44) as bit 7
 * next to the source selector; the DT gives this pin function "func1", which
 * is source index 2 (pinctrl-spmi-gpio.c pmic_gpio_func_index).
 *   0x40 MODE_CTL     = 1 (digital output)
 *   0x44 SOURCE       = (value << 7) | 2
 *   0x45 DIG_OUT_CTL  = (CMOS << 4) | strength 2   (DT qcom,drive-strength 2)
 *   0x46 EN_CTL       = 0x80 (already set on this watch)
 * Registers are read back and logged, so a PMIC that ignores us is visible
 * rather than assumed. */
#define PM_GPIO_BASE(n)  ((uint16_t)(0xC000u + 0x100u * ((n) - 1u)))
#define PM_GPIO_MODE_CTL 0x40u
#define PM_GPIO_SRC_CTL  0x44u
#define PM_GPIO_OUT_CTL  0x45u
#define PM_GPIO_EN_CTL   0x46u
/* The pad's LIVE VALUE is RT_STS 0x10 bit0 (pinctrl-spmi-gpio.c
 * PMIC_MPP_REG_RT_STS / _VAL_MASK), NOT 0x08 -- 0x08 is the peripheral's
 * generic STATUS1 and says nothing about the pin level. */
#define PM_GPIO_RT_STS   0x10u

/* Output SOURCE selector for the LV pad. v258 used 2 ("func1", from the DT
 * pinctrl node) and the pin did not come up. But NOTHING references that
 * pinctrl state -- bg_daemon_reset has no phandle and qcom,bg-daemon has no
 * pinctrl-0 -- so it is dead config the stock kernel never applies. The pad
 * therefore keeps source 0 (NORMAL, "output the value bit"), which is what
 * gpio_direction_output() writes. Source 2 routes a HARDWARE signal to the
 * pin instead of a constant, which would explain a pin that never went high.
 * s_src lets the code fall back to 2 if 0 does not work. */
static uint8_t s_src = 0u;

/* RESET LINE -- and a correction to what v260 seemed to prove.
 *
 * v260 read "pin level 1" before our write and "0" after, and I concluded we
 * were driving the line low. That conclusion was UNSOUND: pinctrl-spmi-gpio.c
 * only believes RT_STS when the pad's INPUT BUFFER is on, i.e. in mode 2
 * (DIGITAL_INPUT_OUTPUT). We were writing mode 1 (DIGITAL_OUTPUT), where the
 * input buffer is off and RT_STS can read 0 whatever the pin does. v261 then
 * saw 0 for BOTH invert encodings, which is exactly what a dead read looks
 * like. So the pin level was never actually measured while driving.
 *
 * What IS solid: with mode 0 (input) the pad reads 1. The line is externally
 * held high, and high = out of reset. So the best "release" is to stop
 * driving it at all and let the external pull do its job -- which is also the
 * pad's power-on state, the one the watch had before we ever touched it.
 * (The PMIC keeps this config across reboots, so every run since v258 has
 * started from our leftover output setting rather than a clean pad.)
 *
 * Hence: release = mode 0 (input) and verify RT_STS reads 1.
 *        assert  = mode 2 (input+output) so the level is readable, trying
 *                  both invert encodings until RT_STS reads 0.
 */
#define PM_GPIO_MODE_INPUT      0u
#define PM_GPIO_MODE_OUTPUT     1u
#define PM_GPIO_MODE_IN_OUT     2u

static int bg_reset_pin_level(void)
{
    uint8_t st = 0;
    (void)spmi_read8(0, PM_GPIO_BASE(PLAT_BG_RESET_PM_GPIO) + PM_GPIO_RT_STS, &st);
    return (int)(st & 1u);
}

/* Stop driving: the external pull takes the line high = out of reset. */
static int bg_reset_release(void)
{
    uint16_t b = PM_GPIO_BASE(PLAT_BG_RESET_PM_GPIO);
    (void)spmi_write8(0, b + PM_GPIO_MODE_CTL, PM_GPIO_MODE_INPUT);
    (void)spmi_write8(0, b + PM_GPIO_EN_CTL, 0x80u);
    timer_delay_us(5000u);
    return bg_reset_pin_level();
}

/* Drive low (assert reset). Mode 2 keeps the input buffer on so the level we
 * read back is the pin's, not a stale zero. */
static int bg_reset_assert(void)
{
    uint16_t b = PM_GPIO_BASE(PLAT_BG_RESET_PM_GPIO);
    unsigned attempt;
    for (attempt = 0; attempt < 2u; attempt++) {
        uint8_t inv = (attempt == 0u) ? 0x80u : 0x00u;
        (void)spmi_write8(0, b + PM_GPIO_MODE_CTL, PM_GPIO_MODE_IN_OUT);
        (void)spmi_write8(0, b + PM_GPIO_OUT_CTL, 0x02u);
        (void)spmi_write8(0, b + PM_GPIO_SRC_CTL, (uint8_t)(inv | s_src));
        (void)spmi_write8(0, b + PM_GPIO_EN_CTL, 0x80u);
        timer_delay_us(5000u);
        if (bg_reset_pin_level() == 0) return 0;
    }
    return bg_reset_pin_level();
}

static void bg_regs_report(const char *tag)
{
    uint16_t b = PM_GPIO_BASE(PLAT_BG_RESET_PM_GPIO);
    uint8_t mode = 0, src = 0, out = 0, en = 0, st = 0, vin = 0;
    (void)spmi_read8(0, b + PM_GPIO_RT_STS, &st);
    (void)spmi_read8(0, b + 0x41u, &vin);          /* DIG_VIN_CTL: pad supply */
    (void)spmi_read8(0, b + PM_GPIO_MODE_CTL, &mode);
    (void)spmi_read8(0, b + PM_GPIO_SRC_CTL, &src);
    (void)spmi_read8(0, b + PM_GPIO_OUT_CTL, &out);
    (void)spmi_read8(0, b + PM_GPIO_EN_CTL, &en);
    con_puts("bgload: reset gpio "); con_puts(tag);
    con_puts(" mode="); con_puthex(mode);
    con_puts(" src="); con_puthex(src);
    con_puts(" out="); con_puthex(out);
    con_puts(" vin="); con_puthex(vin);
    con_puts(" en="); con_puthex(en);
    con_puts(" rt_sts="); con_puthex(st);
    con_puts(" (pin level "); con_putdec(st & 1u); con_puts(")\n");
}

static void bg_power_up(void)
{
    int rc;

    /* pm660_l3 1.05 V and pm660_l9 1.8 V (DT ssr-reg1 / ssr-reg2). These are
     * exactly the rails PLAT_SYS_PC_SLEEP_LDOS deliberately does NOT vote
     * because they belong to the BG -- now we are the BG's owner. */
    {
        unsigned try_n;
        int rc3 = -1, rc9 = -1;
        /* v258 called these at 10 s and got rc=-1 twice, with "smd: no
         * info/fifo items for cid 4" above them: the RPM SMD channel was not
         * open yet. Retry rather than assume, and say so in the log. */
        for (try_n = 0; try_n < 10u && (rc3 || rc9); try_n++) {
            if (rc3) rc3 = rpm_ldo_on(3u, 1050000u, 100u);
            if (rc9) rc9 = rpm_ldo_on(9u, 1800000u, 100u);
            if (rc3 || rc9) timer_delay_us(200000u);
        }
        con_puts("bgload: pm660_l3 1.05V rc="); con_putdec((uint32_t)rc3);
        con_puts("  pm660_l9 1.8V rc="); con_putdec((uint32_t)rc9);
        con_puts(" after "); con_putdec(try_n); con_puts(" attempt(s)\n");
        if (rc3 || rc9) {
            con_puts("bgload: WARNING -- the BG's rails are NOT voted; it has no power\n");
        }
        rc = rc3 | rc9;
        (void)rc;
    }

    /* "AP is up": errfatal low first, then status high (pil-bg probe order). */
    tlmm_cfg(PLAT_AP2BG_ERRFATAL_GPIO, 0u, 0u, 2u, 1); tlmm_out(PLAT_AP2BG_ERRFATAL_GPIO, 0);
    tlmm_cfg(PLAT_AP2BG_STATUS_GPIO,   0u, 0u, 2u, 1); tlmm_out(PLAT_AP2BG_STATUS_GPIO, 1);
    con_puts("bgload: ap2bg errfatal low, ap2bg status high\n");

    /* The bus the TZ app will drive: pins muxed and clocked BEFORE the chip
     * comes out of reset, which is the order the stock stack ends up in. */
    bg_pins_report("before mux");
    bg_spi_bus_up();

    bg_regs_report("before");
    {
        int lvl = bg_reset_release();
        timer_delay_us(50000u);
        con_puts("bgload: reset line released to input -> pin level ");
        con_putdec((uint32_t)lvl);
        con_puts(lvl ? " (HIGH = out of reset)\n"
                     : " (LOW -- something else holds this line down)\n");
    }
    bg_regs_report("after release");
    con_puts("bgload: bg2ap-status now ");
    con_putdec((uint32_t)tlmm_in(PLAT_BG2AP_STATUS_GPIO)); con_puts("\n");
}

void bg_load_report(void)
{
    uint32_t app_id = 0;
    int rc;

    con_puts("bgload: --- BG firmware bring-up (steps 1-3 of 4) ---\n");

    if (bgapp_img_len > sizeof s_app_buf || bg_wear_img_mdt_len > sizeof s_mdt_buf) {
        con_puts("bgload: FAILED -- built-in buffers too small for the blobs\n");
        return;
    }

    /* 0. power the chip, release its reset, tell it the AP is up */
    bg_power_up();

    /* v470: crypto-engine clocks + bus vote BEFORE the first TZ call (wcnss.c pas_crypto_up):
     * TZ hashes the bgapp image with the CE; without WiFi having run first the CE is unclocked
     * and the watch reboots inside qsee_app_lookup/app_start (v465/v468/v469). */
    { extern int pas_crypto_up(void);
      int crc = pas_crypto_up();
      con_puts("bgload: crypto engine for the TZ app load rc="); con_putdec((uint32_t)crc); con_puts("\n");
      con_flush(); }

    /* The BG's boot ROM listens for the SPI download for a window after
     * RESET, then gives up. v262 released reset at 35 s and only pushed the
     * image seconds later, with the chip long past that window; Wear OS
     * instead pulses reset (bg_soft_reset) and loads right after. So put the
     * chip into a freshly-reset state HERE, just before the load. */
    {
        int lo = bg_reset_assert();
        timer_delay_us(50000u);
        {
            int hi = bg_reset_release();
            con_puts("bgload: pre-load reset pulse: low="); con_putdec((uint32_t)lo);
            con_puts(" released="); con_putdec((uint32_t)hi); con_puts("\n");
        }
        timer_delay_us(20000u);
    }

    /* 1. load the TZ app */
    rc = qsee_app_lookup("bgapp", &app_id);
    if (rc == 0 && app_id) {
        con_puts("bgload: bgapp already loaded, app_id="); con_putdec(app_id); con_puts("\n");
    } else {
        memcpy(s_app_buf, bgapp_img, bgapp_img_len);
        con_puts("bgload: APP_START bgapp (mdt "); con_putdec(bgapp_img_mdt_len);
        con_puts(" img "); con_putdec(bgapp_img_len);
        con_puts(" @"); con_puthex((uint32_t)(uintptr_t)s_app_buf); con_puts(") ... ");
        rc = qsee_app_start("bgapp", s_app_buf, bgapp_img_mdt_len, bgapp_img_len, &app_id);
        if (rc || !app_id) {
            con_puts("FAILED rc="); con_putdec((uint32_t)rc);
            con_puts(" app_id="); con_putdec(app_id); con_puts("\n");
            con_puts("bgload: stopping -- the co-processor cannot be loaded without this app\n");
            return;
        }
        con_puts("ok, app_id="); con_putdec(app_id); con_puts("\n");
    }

    /* 2. ask the app for the BG firmware version (no side effects) */
    rc = bg_tz_cmd(app_id, BGPIL_GET_BG_VERSION, 0, 0);
    con_puts("bgload: GET_BG_VERSION rc="); con_putdec((uint32_t)rc);
    con_puts(" status="); con_putdec((uint32_t)S_RSP->status);
    con_puts(" info_len="); con_putdec(S_RSP->bg_info_len);
    if (rc == 0 && S_RSP->bg_info_len && S_RSP->bg_info_len <= 100u) {
        char ver[4 * 16 + 1];
        unsigned n = S_RSP->bg_info_len > 16u ? 16u : S_RSP->bg_info_len, i, j = 0;
        for (i = 0; i < n; i++) {
            uint32_t w = S_RSP->bg_info[i];
            ver[j++] = (char)(w & 0xFFu);         ver[j++] = (char)((w >> 8) & 0xFFu);
            ver[j++] = (char)((w >> 16) & 0xFFu); ver[j++] = (char)((w >> 24) & 0xFFu);
        }
        ver[j] = '\0';
        for (i = 0; i < j; i++) if (ver[i] < 0x20 || ver[i] > 0x7E) ver[i] = '.';
        con_puts(" version=\""); con_puts(ver); con_puts("\"");
    }
    con_puts("\n");

    /* 3. authenticate the bg-wear metadata (the .mdt alone, as PIL does).
     * v266 (first REAL AUTH_MDT): status -7. In the handler -7 is the preset
     * that survives only when the app's range check (a TZ syscall, id -16,
     * on address+size -- the "is this non-secure memory" shape) returns 0.
     * So TZ refuses OUR buffer's physical range, at 0x805d5000 inside the
     * fastboot-loaded image, before looking at the bytes. Linux hands it a
     * CMA/DMA page from anywhere in DDR. Sweep a few physical bases the
     * MMU has mapped (all VA == PA) and see which one TZ accepts; the
     * firmware region then follows the same base (+1 MB). */
    {
        static const uint32_t k_bases[] = {
            0u,            /* our own static buffer (v266: refused)          */
            0x83000000u,   /* splash framebuffer region: aboot writes it     */
            0x85000000u,   /* plain DDR below the first reserved region      */
            0x8B000000u,   /* just above pheripheral_region (0x8A300000+6MB) */
            0x90000000u,   /* mid DDR                                        */
            0x9C000000u,   /* high DDR (1 GB part)                           */
        };
        unsigned b;
        rc = -1;
        for (b = 0; b < sizeof k_bases / sizeof k_bases[0]; b++) {
            uint8_t *mdt = k_bases[b] ? (uint8_t *)(uintptr_t)k_bases[b] : s_mdt_buf;
            memcpy(mdt, bg_wear_img, bg_wear_img_mdt_len);
            scm_dcache_clean(mdt, bg_wear_img_mdt_len);
            con_puts("bgload: AUTH_MDT bg-wear (mdt "); con_putdec(bg_wear_img_mdt_len);
            con_puts(" @"); con_puthex((uint32_t)(uintptr_t)mdt); con_puts(") ... ");
            rc = bg_tz_cmd(app_id, BGPIL_AUTH_MDT, (uint32_t)(uintptr_t)mdt,
                           bg_wear_img_mdt_len);
            if (rc == 0) { s_fw_base = k_bases[b] ? (uint8_t *)(uintptr_t)(k_bases[b] + 0x100000u) : s_fw; break; }
            con_puts("FAILED rc="); con_putdec((uint32_t)rc);
            con_puts(" status="); con_putdec((uint32_t)S_RSP->status);
            con_puts(S_RSP->status == -7 ? " (range refused by TZ)\n" : "\n");
        }
        if (rc) {
            con_puts("bgload: --- done (firmware not loaded: no base accepted) ---\n");
            return;
        }
    }
    con_puts("ok -- metadata AUTHENTICATED; fw region will be @");
    con_puthex((uint32_t)(uintptr_t)s_fw_base); con_puts("\n");

    /* 4. lay the firmware out and have the app push it to the BG */
    {
        const uint8_t *e = bg_wear_img;
        uint32_t phoff = rd32le(e + 28), phnum = rd16le(e + 44);
        uint32_t lo = 0xFFFFFFFFu, hi = 0, size, i;
        int relocatable = 0, nseg = 0;

        if (phnum > 16u || phoff + phnum * 32u > bg_wear_img_mdt_len) {
            con_puts("bgload: bg-wear program headers out of range\n"); return;
        }
        for (i = 0; i < phnum; i++) {
            const uint8_t *p = e + phoff + i * 32u;
            if (!seg_loadable(p)) continue;
            if (rd32le(p + 24) & MDT_RELOCATABLE) relocatable = 1;
            if (rd32le(p + 12) < lo) lo = rd32le(p + 12);
            if (rd32le(p + 12) + rd32le(p + 20) > hi) hi = rd32le(p + 12) + rd32le(p + 20);
            nseg++;
        }
        if (!nseg) { con_puts("bgload: no loadable segments in bg-wear\n"); return; }
        hi = (hi + 0xFFFu) & ~0xFFFu;
        size = hi - lo;

        con_puts("bgload: bg-wear "); con_putdec((uint32_t)nseg);
        con_puts(" loadable seg(s), span "); con_puthex(lo); con_puts("..");
        con_puthex(hi); con_puts(" size "); con_putdec(size);
        con_puts(relocatable ? " (relocatable)\n" : " (FIXED address)\n");

        if (!relocatable) {
            con_puts("bgload: STOPPING -- image wants a fixed load at ");
            con_puthex(lo); con_puts(", which is our own DDR; not relocating blind\n");
            return;
        }
        if (size > sizeof s_fw) {
            con_puts("bgload: STOPPING -- region "); con_putdec(size);
            con_puts(" B exceeds our buffer "); con_putdec((uint32_t)sizeof s_fw); con_puts("\n");
            return;
        }

        memset(s_fw_base, 0, size);
        for (i = 0; i < phnum; i++) {
            const uint8_t *p = e + phoff + i * 32u;
            uint32_t paddr, filesz, off;
            if (!seg_loadable(p)) continue;
            paddr  = rd32le(p + 12);
            filesz = rd32le(p + 16);
            if (i >= bg_wear_img_nblob) {
                con_puts("bgload: segment "); con_putdec(i); con_puts(" has no blob\n"); return;
            }
            off = bg_wear_img_blob_off[i];
            if (filesz > bg_wear_img_blob_len[i] || off + filesz > bg_wear_img_len ||
                (paddr - lo) + filesz > size) {
                con_puts("bgload: segment "); con_putdec(i); con_puts(" does not fit\n"); return;
            }
            memcpy(s_fw_base + (paddr - lo), bg_wear_img + off, filesz);
        }

        con_puts("bgload: IMAGE_LOAD @"); con_puthex((uint32_t)(uintptr_t)s_fw_base);
        con_puts(" size "); con_putdec(size); con_puts(" ... ");
        scm_dcache_clean(s_fw_base, size);
        rc = bg_tz_cmd(app_id, BGPIL_IMAGE_LOAD, (uint32_t)(uintptr_t)s_fw_base, size);
        if (rc && S_RSP->status == -2) {
            /* v267: the app polled the BG's bootloader over SPI, it ANSWERED,
             * and reported itself "in RESET" (BL_CRASH_IN_TWM). The kernel's
             * bg_auth_and_xfer() then sends BGPIL_DLOAD_CONT with the same
             * buffer (ramdump only after a TWM exit); in the app that is the
             * IMAGE_LOAD path without the poll: PIL init, then the download. */
            con_puts("BL in reset (status -2) -> DLOAD_CONT ... ");
            rc = bg_tz_cmd(app_id, BGPIL_DLOAD_CONT, (uint32_t)(uintptr_t)s_fw_base, size);
        }
        if (rc) {
            con_puts("FAILED rc="); con_putdec((uint32_t)rc);
            con_puts(" status="); con_putdec((uint32_t)S_RSP->status);
            if (S_RSP->status == -2) con_puts(" (BG crashed in TWM -- needs ramdump + DLOAD_CONT)");
            con_puts("\n");
            con_puts("bgload: --- done (firmware not loaded) ---\n");
            return;
        }
        con_puts("ok -- FIRMWARE LOADED\n");
        bg_pins_report("after load");
        bg_qup_report("after load");

        /* Kernel shape (subsys-pil-bg.c bg_auth_and_xfer + bg_powerup):
         * GET_BG_VERSION straight after IMAGE_LOAD, then wait_for_err_ready
         * = up to 10 s for bg2ap-status, with NO further reset activity.
         * v257..v263 pulsed reset 1 s after the load, which wipes the image
         * just pushed into the BG's RAM and returns its ROM to waiting. */
        rc = bg_tz_cmd(app_id, BGPIL_GET_BG_VERSION, 0, 0);
        con_puts("bgload: GET_BG_VERSION (after load) rc="); con_putdec((uint32_t)rc);
        con_puts(" info_len="); con_putdec(S_RSP->bg_info_len);
        if (rc == 0 && S_RSP->bg_info_len && S_RSP->bg_info_len <= 100u) {
            char ver[4 * 16 + 1];
            unsigned n = S_RSP->bg_info_len > 16u ? 16u : S_RSP->bg_info_len, k, j = 0;
            for (k = 0; k < n; k++) {
                uint32_t w = S_RSP->bg_info[k];
                ver[j++] = (char)(w & 0xFFu);         ver[j++] = (char)((w >> 8) & 0xFFu);
                ver[j++] = (char)((w >> 16) & 0xFFu); ver[j++] = (char)((w >> 24) & 0xFFu);
            }
            ver[j] = '\0';
            for (k = 0; k < j; k++) if (ver[k] < 0x20 || ver[k] > 0x7E) ver[k] = '.';
            con_puts(" version=\""); con_puts(ver); con_puts("\"");
        }
        con_puts("\n");

        {
            uint32_t t0 = timer_ms();
            for (i = 0; i < 500u; i++) {
                if (tlmm_in(PLAT_BG2AP_STATUS_GPIO)) break;
                timer_delay_us(20000u);
            }
            con_puts("bgload: bg2ap-status gpio"); con_putdec(PLAT_BG2AP_STATUS_GPIO);
            if (tlmm_in(PLAT_BG2AP_STATUS_GPIO)) {
                con_puts(" HIGH after "); con_putdec(timer_ms() - t0); con_puts(" ms -- BG IS RUNNING\n");
                /* v269: first look at bgcom from OUR side. The probe re-muxes
                 * gpio12-15 to plain GPIO and bit-bangs a SLAVE_STATUS read
                 * (awake / app_running / FIFO ready bits + FIFO fill/size). */
                bg_probe_report();
                /* v270: drain the FIFO, GLINK version handshake, listen. */
                bgcom_bringup_report();
            } else {
                con_puts(" still low after 10 s (kernel's own timeout)\n");
            }
        }
        /* NO TrustZone command from here on (v274 lesson): each one re-muxes
         * gpio12-15 to the QUP and shuts its clock, which silently takes the
         * bit-banged bgcom bus away from the crown driver. The read-only
         * register reports are fine. */
        bg_pins_report("after wait");
        bg_qup_report("after wait");
        bg_regs_report("final");
    }
    con_puts("bgload: --- done ---\n");
}

/* ---- boot-time bring-up (v277) ----------------------------------------------
 * Until v276 the whole chain ran from a 35 s hook on the UI task and blocked
 * it for ~3 s. Now: its own low-priority task that
 *   1. stays clear of the CPU1 SCM boot at 8 s (scm_legacy_buf is locked
 *      too, this is belt and braces),
 *   2. brings SMEM up itself instead of waiting for the WiFi path to,
 *   3. polls for the RPM SMD channel (the rails need it; v258: "no
 *      info/fifo items for cid 4" when asked too early),
 *   4. runs bg_load_report() -> TZ load -> bgcom/GLINK -> RSB -> crown live.
 * The UI never waits: crown_take_delta() returns 0 until the end. */
#include "FreeRTOS.h"
#include "task.h"
static void bg_boot_task(void *arg)
{
    uint32_t t0;
    (void)arg;
    while (timer_ms() < 10000u) vTaskDelay(pdMS_TO_TICKS(100));
    /* v277 died inside DLOAD_CONT exactly as "smp: cpu1 up" printed: the
     * CPU1 bring-up is a multi-second SCM poll that starts at 8 s. Wait for
     * it to land (25 s cap for single-core builds, where it never does). */
    t0 = timer_ms();
    while (!smp_flush_available() && timer_ms() - t0 < 25000u) vTaskDelay(pdMS_TO_TICKS(100));
    con_puts(smp_flush_available() ? "bg-boot: cpu1 is up\n" : "bg-boot: cpu1 not up (single core?) -- going ahead\n");
    if (!smem_ok()) con_puts(smem_init() == 0 ? "bg-boot: smem: header valid\n" : "bg-boot: smem: init FAILED\n");
    t0 = timer_ms();
    while (rpm_smd_init() < 0 && timer_ms() - t0 < 60000u) vTaskDelay(pdMS_TO_TICKS(250));
    if (rpm_smd_init() < 0) { con_puts("bg-boot: RPM channel never opened -- co-processor left off\n"); vTaskDelete(0); return; }
    con_puts("bg-boot: RPM channel open at "); con_putdec(timer_ms()); con_puts(" ms -- starting the co-processor\n");
    bg_load_report();
    con_puts("bg-boot: done at "); con_putdec(timer_ms()); con_puts(" ms\n");
    vTaskDelete(0);
}

void bg_boot_start(void)
{
    static int started;
    if (started) return;
    started = 1;
    if (xTaskCreate(bg_boot_task, "bg-boot", 4096, 0, 1, 0) != pdPASS)
        con_puts("bg-boot: task create failed\n");
}

#else
#include "platform.h"
void bg_load_report(void)
{
#if defined(PLAT_HAS_BG_QCC1110)
    con_puts("bgload: no bg_fw.c compiled in (run tools/mk-bgfw.sh) -- skipped\n");
#endif
}
void bg_boot_start(void) { bg_load_report(); }
#endif
