/* usb_ci.c — ChipIdea (ci13xxx) USB device controller + CDC-ACM log console.
 *
 * Step 3 of the USB bring-up: gcc_usb.c (clocks) -> usb_phy_msm.c (ULPI PHY)
 * -> THIS (device controller + gadget).
 *
 * WHY CDC-ACM AND NOT MASS STORAGE. The whole point is getting the log off a
 * watch with a 416x416 screen, no scrolling and no way to copy a file out.
 * Mass storage means a SCSI command set, a block cache and a filesystem the
 * host may cache or corrupt while we are writing it. CDC-ACM needs one bulk IN
 * endpoint, binds to the in-tree cdc_acm driver with zero host setup, and
 * gives a LIVE log rather than a snapshot:
 *
 *     cat /dev/ttyACM0            (or: picocom -b 115200 /dev/ttyACM0)
 *
 * The stream is the ramlog replayed from a cursor, so EVERYTHING already
 * written with con_puts comes out of the cable with no changes anywhere else,
 * including the log from before the cable was plugged in.
 *
 * CONTROLLER FACTS, from the device rather than memory:
 *   - asteroid dmesg: "msm_hsusb [ci13xxx_start] hw_ep_max = 32"
 *   - DT usb@78db000 compatible "qcom,hsusb-otg", mode <1> = peripheral
 *   - msm_otg register offsets (absolute from core base): USBCMD 0x140,
 *     USBINTR 0x148, ULPI_VIEWPORT 0x170, PORTSC 0x184. EHCI defines USBCMD at
 *     op+0x00 and PORTSC at op+0x44, so op = 0x140, hence the capability block
 *     is at core+0x100 and CAPLENGTH = 0x40. Three independent landmarks agree,
 *     so the map is not a guess -- but CAPLENGTH is still read at runtime.
 *   - The LPM register bank (USBMODE at 0xA8 instead of 0x68) is selected by
 *     HCCPARAMS bit 17, exactly as drivers/usb/chipidea does. Also detected at
 *     runtime rather than assumed.
 *
 * CACHE. The MMU is on with caches enabled and the queue heads / transfer
 * descriptors are read by the controller's DMA engine, so every structure
 * handed to hardware is cleaned out of the D-cache before priming and
 * invalidated before being read back. Getting this wrong produces a controller
 * that silently never completes a transfer, which is indistinguishable from a
 * dead PHY -- so the maintenance is explicit and unconditional.
 */
#include "platform.h"
#if defined(PLAT_HAVE_USB_CDC)

#include <string.h>

/* ---- register map -------------------------------------------------------- */
#define CAP_BASE        (PLAT_USB_BASE + 0x100u)
#define CAP_CAPLENGTH   (CAP_BASE + 0x00u)      /* 8-bit */
#define CAP_HCCPARAMS   (CAP_BASE + 0x08u)
#define HCCPARAMS_LEN   (1u << 17)              /* LPM register bank present */

static uintptr_t s_op;                          /* operational register base */
static int       s_lpm;

#define OP(off)         (s_op + (off))
#define USBCMD          0x00u
#define USBSTS          0x04u
#define USBINTR         0x08u
#define DEVICEADDR      0x14u
#define ENDPTLISTADDR   0x18u
#define PORTSC          0x44u
#define PORTSC_PHCD     (1u << 23)   /* PHY low-power suspend */
#define PORTSC_PTS_MASK (3u << 30)   /* parallel transceiver select */
#define PORTSC_PTS_ULPI (2u << 30)
#define PORTSC_PSPD(v)  (((v) >> 24) & 3u)  /* 0 FS, 1 LS, 2 HS */

#define USBCMD_RUN      (1u << 0)
#define USBCMD_RST      (1u << 1)
#define USBCMD_SUTW     (1u << 13)

#define USBSTS_UI       (1u << 0)
#define USBSTS_UEI      (1u << 1)
#define USBSTS_PCI      (1u << 2)
#define USBSTS_URI      (1u << 6)

#define USBMODE_DEVICE  0x2u
#define USBMODE_SLOM    (1u << 3)

#define DEVICEADDR_USBADRA (1u << 24)

/* LPM shifts the device register bank by 0x40 (chipidea: hw_bank.lpm). */
static uint32_t off_usbmode(void)   { return s_lpm ? 0xA8u : 0x68u; }
static uint32_t off_setupstat(void) { return s_lpm ? 0xACu : 0x6Cu; }
static uint32_t off_prime(void)     { return s_lpm ? 0xB0u : 0x70u; }
static uint32_t off_flush(void)     { return s_lpm ? 0xB4u : 0x74u; }
static uint32_t off_complete(void)  { return s_lpm ? 0xBCu : 0x7Cu; }
static uint32_t off_epctrl(int n)   { return (s_lpm ? 0xC0u : 0x80u) + 4u * (uint32_t)n; }

/* ---- DMA structures ------------------------------------------------------ */
/* dQH: 64 bytes, the array must be 2048-byte aligned (ENDPTLISTADDR). */
struct dqh {
    uint32_t cfg;
    uint32_t cur;
    uint32_t next;
    uint32_t token;
    uint32_t page[5];
    uint32_t rsvd;
    uint8_t  setup[8];
    uint32_t pad[4];
};

/* dTD: 32 bytes, 32-byte aligned. */
struct dtd {
    uint32_t next;
    uint32_t token;
    uint32_t page[5];
    uint32_t rsvd;
};

#define DQH_CFG_IOS      (1u << 15)
#define DQH_CFG_ZLT      (1u << 29)
#define DQH_CFG_MAXPKT(n) (((uint32_t)(n)) << 16)

#define DTD_TERMINATE    1u
#define DTD_TOKEN_LEN(n) (((uint32_t)(n)) << 16)
#define DTD_TOKEN_IOC    (1u << 15)
#define DTD_TOKEN_ACTIVE (1u << 7)
#define DTD_TOKEN_HALTED (1u << 6)

#define NUM_EP    4                       /* ep0..ep2 used; 4 for headroom */
#define NUM_IDX   (NUM_EP * 2)
#define EP_IDX(num, in) ((num) * 2 + (in))

static struct dqh s_qh[NUM_IDX] __attribute__((aligned(2048)));
static struct dtd s_td[NUM_IDX] __attribute__((aligned(32)));

/* Transfer buffers must not share a cache line with anything else. */
static uint8_t s_ep0buf[64]  __attribute__((aligned(64)));
static uint8_t s_inbuf[512]  __attribute__((aligned(64)));
static uint8_t s_outbuf[64]  __attribute__((aligned(64)));

/* ---- cache maintenance --------------------------------------------------- */
#define CLINE 32u

static void dcache_clean(const void *p, uint32_t len)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(CLINE - 1u);
    uintptr_t e = ((uintptr_t)p + len + CLINE - 1u) & ~(uintptr_t)(CLINE - 1u);
    for (; a < e; a += CLINE)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory"); /* DCCMVAC */
    __asm__ volatile("dsb sy" ::: "memory");
}

static void dcache_inval(const void *p, uint32_t len)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(CLINE - 1u);
    uintptr_t e = ((uintptr_t)p + len + CLINE - 1u) & ~(uintptr_t)(CLINE - 1u);
    for (; a < e; a += CLINE)
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");  /* DCIMVAC */
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- USB descriptors (CDC-ACM) ------------------------------------------- */
#define EP_INT_IN   2
#define EP_BULK_IN  1
#define EP_BULK_OUT 1
#define BULK_MPS    64

static const uint8_t s_dev_desc[18] = {
    18, 1,                    /* bLength, DEVICE */
    0x00, 0x02,               /* bcdUSB 2.00 */
    0x02, 0x00, 0x00,         /* class CDC, subclass 0, protocol 0 */
    64,                       /* bMaxPacketSize0 */
    0x09, 0x12,               /* idVendor  0x1209 (pid.codes) */
    0x01, 0x00,               /* idProduct 0x0001 */
    0x01, 0x00,               /* bcdDevice 0.01 */
    1, 2, 3,                  /* iManufacturer, iProduct, iSerial */
    1                         /* bNumConfigurations */
};

#define CFG_TOTAL 67
static const uint8_t s_cfg_desc[CFG_TOTAL] = {
    /* configuration */
    9, 2, CFG_TOTAL & 0xFF, CFG_TOTAL >> 8, 2, 1, 0, 0xC0, 0x32,
    /* interface 0: communications, ACM */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    /* CDC header */
    5, 0x24, 0x00, 0x10, 0x01,
    /* CDC call management: no call mgmt, data iface 1 */
    5, 0x24, 0x01, 0x00, 1,
    /* CDC ACM: supports Set_Line_Coding etc. */
    4, 0x24, 0x02, 0x02,
    /* CDC union: master 0, slave 1 */
    5, 0x24, 0x06, 0, 1,
    /* notification endpoint (never used, but the class requires it) */
    7, 5, 0x80 | EP_INT_IN, 0x03, 16, 0, 9,
    /* interface 1: CDC data */
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    /* bulk IN (log data to host) */
    7, 5, 0x80 | EP_BULK_IN, 0x02, BULK_MPS, 0, 0,
    /* bulk OUT (host -> us; drained and discarded) */
    7, 5, EP_BULK_OUT, 0x02, BULK_MPS, 0, 0
};

static const uint8_t s_str0[4] = { 4, 3, 0x09, 0x04 };   /* en-US */

/* USB string descriptors are UTF-16LE. */
static uint8_t s_strbuf[64] __attribute__((aligned(64)));
static uint32_t make_string(const char *s)
{
    uint32_t n = 0;
    while (s[n] && n < 30u) n++;
    s_strbuf[0] = (uint8_t)(2u + 2u * n);
    s_strbuf[1] = 3;
    for (uint32_t i = 0; i < n; i++) {
        s_strbuf[2 + 2 * i] = (uint8_t)s[i];
        s_strbuf[3 + 2 * i] = 0;
    }
    return s_strbuf[0];
}

/* ---- state --------------------------------------------------------------- */
static int      s_inited, s_running, s_configured;
static uint32_t s_pending_addr;
static int      s_in_busy;
static uint32_t s_log_cursor;
static int      s_have_cursor;

/* ---- endpoint plumbing --------------------------------------------------- */
static void ep_prime(int idx, const void *buf, uint32_t len)
{
    struct dtd *td = &s_td[idx];
    struct dqh *qh = &s_qh[idx];
    uintptr_t b = (uintptr_t)buf;

    td->next  = DTD_TERMINATE;
    td->token = DTD_TOKEN_LEN(len) | DTD_TOKEN_IOC | DTD_TOKEN_ACTIVE;
    td->page[0] = (uint32_t)b;
    for (int i = 1; i < 5; i++)
        td->page[i] = (uint32_t)((b & ~0xFFFu) + (uintptr_t)i * 0x1000u);
    td->rsvd = 0;

    if (len) dcache_clean(buf, len);
    dcache_clean(td, sizeof *td);

    qh->next  = (uint32_t)(uintptr_t)td;
    qh->token = 0;
    dcache_clean(qh, sizeof *qh);

    uint32_t bit = (idx & 1) ? (1u << (16 + idx / 2)) : (1u << (idx / 2));
    mmio_write(OP(off_prime()), bit);
}

/* Wait for a primed transfer to retire. Bounded: a host that is not listening
 * must never wedge the watch's UI thread. */
static int ep_wait(int idx, uint32_t timeout_ms)
{
    struct dtd *td = &s_td[idx];
    uint32_t t0 = timer_ms();
    for (;;) {
        dcache_inval(td, sizeof *td);
        if (!(td->token & DTD_TOKEN_ACTIVE)) break;
        if ((uint32_t)(timer_ms() - t0) > timeout_ms) return -1;
    }
    uint32_t bit = (idx & 1) ? (1u << (16 + idx / 2)) : (1u << (idx / 2));
    mmio_write(OP(off_complete()), bit);
    return (td->token & DTD_TOKEN_HALTED) ? -1 : 0;
}

static void ep0_send(const void *data, uint32_t len, uint32_t wlen)
{
    if (len > wlen) len = wlen;
    if (len) memcpy(s_ep0buf, data, len > sizeof s_ep0buf ? sizeof s_ep0buf : len);
    ep_prime(EP_IDX(0, 1), s_ep0buf, len);
    (void)ep_wait(EP_IDX(0, 1), 50u);
    /* status stage: zero-length OUT */
    ep_prime(EP_IDX(0, 0), s_ep0buf, 0);
    (void)ep_wait(EP_IDX(0, 0), 50u);
}

static void ep0_ack(void)
{
    ep_prime(EP_IDX(0, 1), s_ep0buf, 0);
    (void)ep_wait(EP_IDX(0, 1), 50u);
}

static void ep_enable(int num, int in, int type)
{
    uint32_t v = mmio_read(OP(off_epctrl(num)));
    if (in) {
        v &= ~(3u << 18);
        v |= ((uint32_t)type << 18) | (1u << 22) /* TXR */ | (1u << 23) /* TXE */;
    } else {
        v &= ~(3u << 2);
        v |= ((uint32_t)type << 2) | (1u << 6) /* RXR */ | (1u << 7) /* RXE */;
    }
    mmio_write(OP(off_epctrl(num)), v);
}

static void ep_setup_all(void)
{
    memset(s_qh, 0, sizeof s_qh);
    memset(s_td, 0, sizeof s_td);
    /* ep0: control, 64 B, IOS set on OUT so setup packets land in the dQH. */
    s_qh[EP_IDX(0, 0)].cfg = DQH_CFG_MAXPKT(64) | DQH_CFG_IOS;
    s_qh[EP_IDX(0, 1)].cfg = DQH_CFG_MAXPKT(64);
    s_qh[EP_IDX(0, 0)].next = DTD_TERMINATE;
    s_qh[EP_IDX(0, 1)].next = DTD_TERMINATE;
    /* ep1 bulk in/out, ep2 interrupt in */
    s_qh[EP_IDX(EP_BULK_IN, 1)].cfg  = DQH_CFG_MAXPKT(BULK_MPS) | DQH_CFG_ZLT;
    s_qh[EP_IDX(EP_BULK_OUT, 0)].cfg = DQH_CFG_MAXPKT(BULK_MPS) | DQH_CFG_ZLT;
    s_qh[EP_IDX(EP_INT_IN, 1)].cfg   = DQH_CFG_MAXPKT(16) | DQH_CFG_ZLT;
    for (int i = 0; i < NUM_IDX; i++) s_qh[i].next = DTD_TERMINATE;
    dcache_clean(s_qh, sizeof s_qh);
}

/* ---- control transfers --------------------------------------------------- */
static void handle_setup(const uint8_t *p)
{
    uint8_t  bmRequestType = p[0];
    uint8_t  bRequest      = p[1];
    uint16_t wValue        = (uint16_t)(p[2] | (p[3] << 8));
    uint16_t wLength       = (uint16_t)(p[6] | (p[7] << 8));

    if ((bmRequestType & 0x60u) == 0x00u) {          /* standard */
        switch (bRequest) {
        case 0x06: {                                  /* GET_DESCRIPTOR */
            uint8_t type = (uint8_t)(wValue >> 8), idx = (uint8_t)(wValue & 0xFF);
            if (type == 1) { ep0_send(s_dev_desc, sizeof s_dev_desc, wLength); return; }
            if (type == 2) { ep0_send(s_cfg_desc, sizeof s_cfg_desc, wLength); return; }
            if (type == 3) {
                if (idx == 0) { ep0_send(s_str0, sizeof s_str0, wLength); return; }
                const char *s = (idx == 1) ? "OpenWatchFace"
                              : (idx == 2) ? PLAT_NAME " log"
                                           : "0001";
                uint32_t n = make_string(s);
                ep0_send(s_strbuf, n, wLength);
                return;
            }
            break;                                    /* unsupported -> stall */
        }
        case 0x05:                                    /* SET_ADDRESS */
            /* ChipIdea latches the address after the status stage: set
             * USBADRA so the hardware applies it at the right moment. */
            s_pending_addr = (uint32_t)(wValue & 0x7Fu);
            mmio_write(OP(DEVICEADDR), (s_pending_addr << 25) | DEVICEADDR_USBADRA);
            ep0_ack();
            return;
        case 0x09:                                    /* SET_CONFIGURATION */
            s_configured = (wValue != 0);
            if (s_configured) {
                ep_enable(EP_BULK_IN, 1, 2);          /* bulk */
                ep_enable(EP_BULK_OUT, 0, 2);
                ep_enable(EP_INT_IN, 1, 3);           /* interrupt */
                ep_prime(EP_IDX(EP_BULK_OUT, 0), s_outbuf, sizeof s_outbuf);
                bdiag_puts("usb: configured\n");
            }
            ep0_ack();
            return;
        case 0x08: {                                  /* GET_CONFIGURATION */
            uint8_t c = (uint8_t)s_configured;
            ep0_send(&c, 1, wLength);
            return;
        }
        case 0x00: {                                  /* GET_STATUS */
            uint8_t st[2] = { 0, 0 };
            ep0_send(st, 2, wLength);
            return;
        }
        case 0x0A: {                                  /* GET_INTERFACE */
            uint8_t a = 0;
            ep0_send(&a, 1, wLength);
            return;
        }
        case 0x01: case 0x03: case 0x0B:              /* CLEAR/SET_FEATURE, SET_IF */
            ep0_ack();
            return;
        default:
            break;
        }
    } else if ((bmRequestType & 0x60u) == 0x20u) {    /* class: CDC */
        switch (bRequest) {
        case 0x20:                                    /* SET_LINE_CODING */
            /* Data stage arrives on ep0 OUT; accept and discard - we are a
             * one-way log pipe and the baud rate is meaningless here. */
            ep_prime(EP_IDX(0, 0), s_ep0buf, wLength > sizeof s_ep0buf
                                             ? sizeof s_ep0buf : wLength);
            (void)ep_wait(EP_IDX(0, 0), 50u);
            ep0_ack();
            return;
        case 0x21: {                                  /* GET_LINE_CODING */
            static const uint8_t lc[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };
            ep0_send(lc, sizeof lc, wLength);
            return;
        }
        case 0x22:                                    /* SET_CONTROL_LINE_STATE */
            ep0_ack();
            return;
        default:
            break;
        }
    }

    /* Unhandled: stall ep0 so the host retries or gives up cleanly rather
     * than waiting for a response that never comes. */
    mmio_write(OP(off_epctrl(0)), mmio_read(OP(off_epctrl(0))) | (1u << 16) | (1u << 0));
}

static void poll_setup(void)
{
    uint32_t st = mmio_read(OP(off_setupstat()));
    if (!(st & 1u)) return;

    uint8_t setup[8];
    /* ChipIdea setup lockout: re-read under USBCMD.SUTW until it survives. */
    for (int tries = 0; tries < 8; tries++) {
        mmio_write(OP(USBCMD), mmio_read(OP(USBCMD)) | USBCMD_SUTW);
        dcache_inval(&s_qh[EP_IDX(0, 0)], sizeof s_qh[0]);
        memcpy(setup, s_qh[EP_IDX(0, 0)].setup, 8);
        if (mmio_read(OP(USBCMD)) & USBCMD_SUTW) break;
    }
    mmio_write(OP(USBCMD), mmio_read(OP(USBCMD)) & ~USBCMD_SUTW);
    mmio_write(OP(off_setupstat()), 1u);              /* w1c */

    /* Cancel anything still primed on ep0 before the new control transfer. */
    mmio_write(OP(off_flush()), (1u << 0) | (1u << 16));

    handle_setup(setup);
}

/* ---- bus reset ----------------------------------------------------------- */
static void handle_reset(void)
{
    mmio_write(OP(DEVICEADDR), 0);
    s_configured = 0;
    s_in_busy = 0;
    /* ack all setup + endpoint state, then flush every endpoint */
    mmio_write(OP(off_setupstat()), mmio_read(OP(off_setupstat())));
    mmio_write(OP(off_complete()),  mmio_read(OP(off_complete())));
    mmio_write(OP(off_flush()), 0xFFFFFFFFu);
    ep_setup_all();
    bdiag_puts("usb: bus reset\n");
}

/* ---- public API ---------------------------------------------------------- */
int usb_dev_init(void)
{
    if (s_inited) return 0;

    if (gcc_usb_hs_up() < 0) {
        con_puts("usb: clocks failed\n");
        return -1;
    }

    uint8_t caplen = (uint8_t)(mmio_read(CAP_CAPLENGTH) & 0xFFu);
    uint32_t hcc   = mmio_read(CAP_HCCPARAMS);
    s_op  = CAP_BASE + caplen;
    s_lpm = (hcc & HCCPARAMS_LEN) ? 1 : 0;

    bdiag_puts("usb: caplen="); bdiag_puthex(caplen);
    bdiag_puts(" hcc=");        bdiag_puthex(hcc);
    bdiag_puts(" op=");         bdiag_puthex((uint32_t)s_op);
    bdiag_puts(" lpm=");        bdiag_putdec((uint32_t)s_lpm);
    bdiag_puts("\n");

    /* SANITY GATE BEFORE THE FIRST WRITE. Everything above is reads; every
     * line below drives a controller. If the GCC branch offsets were wrong
     * for this SoC the register file is unclocked and CAPLENGTH reads back
     * as 0x00 or 0xFF rather than a plausible capability length -- and
     * banging a block in a dead clock domain is precisely what hard-reset
     * this SoC on every previous bring-up (display, touch, eMMC, TSENS).
     * Bail with a log instead: no USB is a missing feature, a wedged watch
     * is a bricked debug session. */
    if (caplen < 0x10u || caplen > 0x7Fu || hcc == 0xFFFFFFFFu) {
        con_puts("usb: controller not responding (bad CAPLENGTH) - USB off\n");
        return -1;
    }

    /* KERNEL-FAITHFUL RESET ORDER (2026-08-06, phy-msm-usb.c msm_otg_reset
     * with phy_type = QUSB_ULPI_PHY, read from the real hoki source):
     *   msm_otg_phy_reset:  link clk BCR cycle (done in gcc_usb_hs_up) +
     *                       QUSB2_PHY_BCR pulse + PORTSC PTS=ULPI
     *   msm_otg_link_reset: USBCMD.RST, then PORTSC=0x80000000 (PTS ULPI
     *                       again, IMMEDIATELY after reset), AHBBURST=0,
     *                       AHBMODE=8
     *   msleep(100)
     *   msm_usb_phy_reset:  QUSB2 POR + CSR power-cycle   (usb_phy_qusb_por)
     *   ulpi_init:          all regs > 0x3F -> SKIPPED for this PHY type
     *   msm_usb_phy_reset:  QUSB2 POR again (latches overrides)
     * The old code did none of the CSR work and applied ULPI writes the
     * stock kernel refuses for this PHY. */
    gcc_usb_qusb2_phy_reset();
    mmio_write(OP(PORTSC),
               (mmio_read(OP(PORTSC)) & ~PORTSC_PTS_MASK) | PORTSC_PTS_ULPI);

    mmio_write(OP(USBCMD), USBCMD_RST);
    uint32_t t0 = timer_ms();
    while (mmio_read(OP(USBCMD)) & USBCMD_RST) {
        if ((uint32_t)(timer_ms() - t0) > 250u) {
            con_puts("usb: link reset timeout\n");
            return -1;
        }
    }
    mmio_write(OP(PORTSC), 0x80000000u);              /* PTS=ULPI, kernel-verbatim */
    mmio_write(PLAT_USB_BASE + 0x090u, 0x00u);        /* USB_AHBBURST */
    mmio_write(PLAT_USB_BASE + 0x098u, 0x08u);        /* USB_AHBMODE */

    timer_delay_ms(100);                              /* kernel msleep(100) */

    usb_phy_qusb_por();                               /* CSR power-cycle #1 */
    usb_phy_qusb_por();                               /* #2, kernel does both */
    (void)usb_phy_init_seq();                         /* liveness log only */

    {
        uint32_t pv = mmio_read(OP(PORTSC));
        bdiag_puts("usb: portsc pre="); bdiag_puthex(pv); bdiag_puts("\n");
        if (pv & PORTSC_PHCD) {
            mmio_write(OP(PORTSC), pv & ~PORTSC_PHCD);
            bdiag_puts("usb: cleared PORTSC.PHCD (PHY was in low-power)\n");
        }
        bdiag_puts("usb: portsc post="); bdiag_puthex(mmio_read(OP(PORTSC)));
        bdiag_puts("\n");
    }

    /* ci13xxx_msm_reset() parity (real hoki ci13xxx_msm.c): clear
     * GENCONFIG.TXFIFO_IDLE_FORCE_DISABLE (bit4) and GENCONFIG.ULPI_SERIAL_EN
     * (bit5). The latter is a SECOND place serial-vs-ULPI mode is selected —
     * left set it defeats the PORTSC PTS=ULPI fix from the other side. */
    mmio_write(PLAT_USB_BASE + 0x09Cu,
               mmio_read(PLAT_USB_BASE + 0x09Cu) & ~((1u << 4) | (1u << 5)));

    mmio_write(OP(off_usbmode()), USBMODE_DEVICE | USBMODE_SLOM);
    ep_setup_all();
    mmio_write(OP(ENDPTLISTADDR), (uint32_t)(uintptr_t)s_qh);
    mmio_write(OP(USBINTR), 0);                       /* polled, not IRQ-driven */

    /* THE ATTACH FIX (2026-08-06, measured CCS:0 with cable in). VBUS session
     * validity on this board is PMIC-owned (otg-control=2) and no PMIC driver
     * exists here, so the link never sees a valid session and never pulls up
     * D+ — port enabled, CCS stuck 0. The stock gadget UDC covers exactly
     * this in ci13xxx_msm_connect(): force session-valid in SOFTWARE.
     *   GENCONFIG_2 |= SESS_VLD_CTRL_EN   (session-valid comes from USBCMD)
     *   USBCMD      |= SESS_VLD_CTRL      (assert it)
     * Register/bit values verified from the real hoki msm_hsusb_hw.h. */
    mmio_write(PLAT_USB_BASE + 0x0A0u,
               mmio_read(PLAT_USB_BASE + 0x0A0u) | (1u << 7));
    mmio_write(OP(USBCMD), mmio_read(OP(USBCMD)) | (1u << 25));

    mmio_write(OP(USBCMD), mmio_read(OP(USBCMD)) | USBCMD_RUN);

    bdiag_puts("usb: running, portsc="); bdiag_puthex(mmio_read(OP(PORTSC)));
    bdiag_puts(" usbcmd=");             bdiag_puthex(mmio_read(OP(USBCMD)));
    bdiag_puts(" usbsts=");             bdiag_puthex(mmio_read(OP(USBSTS)));
    bdiag_puts("\n");

    s_inited = 1;
    s_running = 1;
    return 0;
}

/* Snapshot for the on-glass diagnostic screen (GLASS_DIAG builds): lets the
 * user READ the USB state off the watch with no log pull at all. */
/* Cheap "is a host actually listening?" — a plain flag read, no ULPI traffic,
 * so it is safe to call per suspend chunk. suspend_msm.c uses it to decide
 * whether it must keep polling the CDC endpoint often enough to stay alive. */
int usb_is_configured(void)
{
    return s_running && s_configured;
}

void usb_diag(uint32_t *portsc, uint32_t *usbsts, uint32_t *vid, int *cfg)
{
    if (!s_running) { *portsc = 0; *usbsts = 0; *vid = 0xDEAD; *cfg = -1; return; }
    *portsc = mmio_read(OP(PORTSC));
    *usbsts = mmio_read(OP(USBSTS));
    uint8_t v0 = 0xFFu, v1 = 0xFFu;
    (void)usb_ulpi_read(0x00u, &v0);
    (void)usb_ulpi_read(0x01u, &v1);
    *vid = ((uint32_t)v1 << 8) | v0;
    /* Kernel header ground truth (msm_hsusb_hw.h): USB_USBMODE = 0x1A8 =
     * op + 0x68, the NON-LPM bank — s_lpm must be 0 on this part. Encode a
     * wrong runtime detection as cfg+8 so it is visible on the glass
     * ("USB CFG:8/9" = LPM misdetected, device-mode writes hit the wrong
     * offsets and enumeration can never work). */
    *cfg = s_configured + (s_lpm ? 8 : 0);
}

/* Pump the device stack + stream the ramlog. Cheap when no host is attached:
 * one register read. Safe to call every loop() iteration. */
void usb_poll(void)
{
    if (!s_running) return;

    /* Heartbeat: without this a failed enumeration produces NO evidence at
     * all, and the log is the only channel we have. PORTSC bit 0 (CCS) shows
     * whether the controller sees a connection at all -- that single bit
     * separates "PHY never attached" from "attached but enumeration broke".
     * 2026-08-06: USB is fully working, so the heartbeat is opt-in now —
     * build with -DUSB_DIAG to get it back (see BUILD-GEN6.md). */
#if defined(USB_DIAG)
    {
        static uint32_t s_win;
        uint32_t t = timer_ms();
        if (!s_win) s_win = t;
        if ((uint32_t)(t - s_win) >= 5000u) {
            s_win = t;
            uint32_t pv = mmio_read(OP(PORTSC));
            uint8_t v0 = 0xFFu, v1 = 0xFFu;
            (void)usb_ulpi_read(0x00u, &v0);
            (void)usb_ulpi_read(0x01u, &v1);
            bdiag_puts("USB t=");      bdiag_putdec(t);
            bdiag_puts(" portsc=");    bdiag_puthex(pv);
            bdiag_puts(" pts=");       bdiag_putdec((pv >> 30) & 3u);
            bdiag_puts(" spd=");       bdiag_putdec(PORTSC_PSPD(pv));
            bdiag_puts(" ccs=");       bdiag_putdec(pv & 1u);
            bdiag_puts(" usbsts=");    bdiag_puthex(mmio_read(OP(USBSTS)));
            bdiag_puts(" cmd=");       bdiag_puthex(mmio_read(OP(USBCMD)));
            bdiag_puts(" op=");        bdiag_puthex((uint32_t)s_op);
            bdiag_puts(" lpm=");       bdiag_putdec((uint32_t)s_lpm);
            bdiag_puts(" vid=");       bdiag_puthex(((uint32_t)v1 << 8) | v0);
            bdiag_puts(" cfg=");       bdiag_putdec((uint32_t)s_configured);
            bdiag_puts("\n");
        }
    }
#endif /* USB_DIAG */

    uint32_t sts = mmio_read(OP(USBSTS));
    if (sts) mmio_write(OP(USBSTS), sts);             /* w1c */

    if (sts & USBSTS_URI) handle_reset();
    if (sts & (USBSTS_UI | USBSTS_UEI)) poll_setup();

    if (!s_configured) return;

    /* Bulk IN: non-blocking. If a transfer is outstanding, retire it when the
     * controller is done; never wait on the host. */
    if (s_in_busy) {
        dcache_inval(&s_td[EP_IDX(EP_BULK_IN, 1)], sizeof s_td[0]);
        if (s_td[EP_IDX(EP_BULK_IN, 1)].token & DTD_TOKEN_ACTIVE) return;
        mmio_write(OP(off_complete()), 1u << (16 + EP_BULK_IN));
        s_in_busy = 0;
    }

    uint32_t n = ramlog_read(&s_log_cursor, (char *)s_inbuf, sizeof s_inbuf);
    if (n) {
        ep_prime(EP_IDX(EP_BULK_IN, 1), s_inbuf, n);
        s_in_busy = 1;
    }

    /* Drain anything the host sends so the OUT endpoint never stalls. */
    dcache_inval(&s_td[EP_IDX(EP_BULK_OUT, 0)], sizeof s_td[0]);
    if (!(s_td[EP_IDX(EP_BULK_OUT, 0)].token & DTD_TOKEN_ACTIVE)) {
        mmio_write(OP(off_complete()), 1u << EP_BULK_OUT);
        ep_prime(EP_IDX(EP_BULK_OUT, 0), s_outbuf, sizeof s_outbuf);
    }

    (void)s_have_cursor;
}

#endif /* PLAT_HAVE_USB_CDC */
