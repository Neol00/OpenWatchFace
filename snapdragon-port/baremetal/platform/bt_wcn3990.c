/* bt_wcn3990.c — Fossil Gen 6 Bluetooth chip: power sequence + HCI probe.
 *
 * The WCN3990's power-up is two enable GPIOs, taken straight from the watch's
 * device tree and its kernel driver:
 *
 *   bt_wcn3990 { compatible = "qca,wcn3990";
 *                qca,bt-3P3-en-gpio = <&tlmm 47 0>;
 *                qca,bt-1P3-en-gpio = <&tlmm 57 0>; ... }
 *
 *   bluetooth-power.c, bt_configure_gpios_2wcn(on):
 *       3P3 low, 50 ms, 3P3 high, 50 ms, 1P3 low, 50 ms, 1P3 high, 50 ms
 *   off: 3P3 low, 100 ms, 1P3 low, 100 ms
 *
 * The DT also lists vdd-io / vdd-s5 regulator supplies. Those are PM660 rails
 * that aboot already brings up for its own use; the enable GPIOs are what the
 * driver actually toggles per power cycle, so we start with those alone and
 * only add rail votes if the chip stays silent.
 *
 * PROBE. After power-up the chip speaks HCI on the UART in its bootloader
 * ("EDL") mode, before any firmware download. The vendor's first transaction
 * (hci_qca.c qca_read_soc_version) is a vendor command:
 *     01 00 FC 01 19      H4 cmd, opcode 0xFC00, 1 param, EDL_PATCH_VER_REQ
 * and a plain HCI Reset (01 03 0C 00) is the other thing it will answer. Both
 * are sent and whatever comes back is dumped raw: at this stage ANY well-formed
 * reply is the milestone, because it proves power, clocks, pins, flow control
 * and framing all at once.
 */
#include "platform.h"
#if defined(PLAT_BT_UART_BASE)

static int s_powered;
static uint8_t s_bdaddr[6];
const uint8_t *bt_wcn3990_bdaddr(void) { return s_bdaddr; }
static uint32_t s_link_baud = 115200u;
uint32_t bt_wcn3990_link_baud(void) { return s_link_baud; }
static int s_ready;
int bt_wcn3990_ready(void) { return s_ready; }

void bt_wcn3990_power(int on)
{
    if (on) {
        tlmm_cfg(PLAT_BT_EN_3P3_GPIO, 0u, 0u, 2u, 1);   /* GPIO func, output */
        tlmm_cfg(PLAT_BT_EN_1P3_GPIO, 0u, 0u, 2u, 1);
        tlmm_out(PLAT_BT_EN_3P3_GPIO, 0); timer_delay_ms(50);
        tlmm_out(PLAT_BT_EN_3P3_GPIO, 1); timer_delay_ms(50);
        tlmm_out(PLAT_BT_EN_1P3_GPIO, 0); timer_delay_ms(50);
        tlmm_out(PLAT_BT_EN_1P3_GPIO, 1); timer_delay_ms(50);
        s_powered = 1;
        con_puts("bt: wcn3990 powered (3P3 gpio");
        con_putdec(PLAT_BT_EN_3P3_GPIO); con_puts(", 1P3 gpio");
        con_putdec(PLAT_BT_EN_1P3_GPIO); con_puts(")\n");
    } else {
        tlmm_out(PLAT_BT_EN_3P3_GPIO, 0); timer_delay_ms(100);
        tlmm_out(PLAT_BT_EN_1P3_GPIO, 0); timer_delay_ms(100);
        s_powered = 0;
        con_puts("bt: wcn3990 powered down\n");
    }
}

static void dump(const char *tag, const uint8_t *b, int n)
{
    con_puts(tag); con_putdec((uint32_t)(n < 0 ? 0 : n)); con_puts(" bytes:");
    for (int i = 0; i < n && i < 32; i++) { con_puts(" "); bt_hex2(b[i]); }
    con_puts("\n");
}

/* Send one H4 packet and dump whatever comes back. Returns bytes received. */
/* A reply only counts if it LOOKS like HCI: an event packet (0x04). v9 called
 * 64 bytes of 0xFD a success - that was the chip's in-band-sleep WAKE
 * indication, not an answer. */
static int bt_try(const char *what, const uint8_t *cmd, uint32_t len, uint32_t wait_ms)
{
    uint8_t rsp[64];
    con_puts("bt: -> "); con_puts(what); con_puts(":");
    for (uint32_t i = 0; i < len; i++) { con_puts(" "); bt_hex2(cmd[i]); }
    con_puts("\n");
    while (bt_uart_getc() >= 0) { }              /* drain stale bytes */
    bt_uart_write(cmd, len);
    int n = bt_uart_read(rsp, sizeof rsp, wait_ms);
    dump("bt: <- ", rsp, n);
    if (n > 0 && rsp[0] != 0x04u) {
        con_puts("bt:    (not an HCI event - first byte ");
        bt_hex2(rsp[0]);
        con_puts(rsp[0] == 0xFDu ? ", that is an in-band-sleep WAKE_IND)\n" : ")\n");
        return 0;
    }
    return n;
}

/* Bring the controller all the way up, for the BLE-enable path. Runs the same
 * state machine as the probe but drives it to completion here, pumping USB and
 * the watchdog between slices so nothing starves (this is called from the app,
 * not from an interrupt). 0 = controller is up and running its firmware. */
int bt_wcn3990_bringup(void)
{
    uint32_t t0 = timer_ms();
    con_puts("bt: bringing the controller up for BLE\n");
    while (bt_probe_step() != 1) {
        usb_poll(); wdog_pet(); deadman_kick();
        if ((uint32_t)(timer_ms() - t0) > 120000u) {
            con_puts("bt: bring-up timed out\n");
            return -1;
        }
    }
    return bt_wcn3990_ready() ? 0 : -1;
}

/* ---- STEPPED bring-up ----------------------------------------------------
 * Called once per main-loop iteration. The blocking version froze the app task
 * for ~25 s, and because usb_poll() is driven from that same loop the console
 * host dropped the device and never got it back - which is exactly what the
 * blocking probe did to the USB bus. Nothing here blocks for more than the
 * time of one 248-byte segment (~21 ms at 115200).
 * Returns 0 while busy, 1 when finished (success or given up). */
int bt_probe_step(void)
{
    static const uint8_t edl_ver[]   = { 0x01, 0x00, 0xFC, 0x01, 0x19 };
    static const uint8_t hci_reset[] = { 0x01, 0x03, 0x0C, 0x00 };
    static int st, tries, last = -1;
    static uint32_t t0;

    /* Announce every state change and FLUSH it. If the watch resets during
     * bring-up, the last line in the log names the exact step that did it. */
    if (st != last) {
        con_puts("bt: [state "); con_putdec((uint32_t)st); con_puts("]\n");
        con_flush(); usb_poll();
        last = st;
    }

    switch (st) {
    case 0:
        con_puts("bt: ---- WCN3990 bring-up (stepped) ----\n");
        if (bt_uart_loopback_test() < 0) {
            con_puts("bt: UART loopback FAILED\n"); st = 90; return 1;
        }
        if (bt_uart_init() < 0) { con_puts("bt: UART init failed\n"); st = 90; return 1; }
        bt_wcn3990_power(1);
        t0 = timer_ms(); st = 1; return 0;

    case 1:                                     /* settle after power */
        if ((uint32_t)(timer_ms() - t0) < 500u) return 0;
        tries = 0; st = 2; return 0;

    case 2:                                     /* reset until it answers */
        if (bt_try("HCI Reset", hci_reset, sizeof hci_reset, 600u) > 0) {
            con_puts("bt: CHIP ANSWERED\n"); st = 3; return 0;
        }
        if (++tries >= 4) { con_puts("bt: no reply from the chip\n"); st = 90; return 1; }
        return 0;

    case 3:
        if (bt_try("EDL version request", edl_ver, sizeof edl_ver, 1000u) <= 0) {
            con_puts("bt: EDL silent - cannot download\n"); st = 90; return 1;
        }
        st = 7; return 0;

    case 7: {
        /* RAISE THE LINK BEFORE MOVING 220 KB. At 115200 the download takes
         * ~20 s, which is the whole reason enabling BLE froze the UI. The
         * controller takes a vendor baud-change command (opcode 0xFC48, one
         * baud code); 0x11 = 3 200 000, and our 3.2 Mbps clock is already
         * proven (it decoded the chip's wake bytes correctly). If the chip
         * ignores the command we simply stay at 115200 and take the slow road. */
        static const uint8_t setbaud[] = { 0x01, 0x48, 0xFC, 0x01, 0x11 };
        con_puts("bt: raising the link to 3.2 Mbps for the download\n");
        bt_uart_write(setbaud, sizeof setbaud);
        t0 = timer_ms(); st = 8; return 0;
    }

    case 8:
        if ((uint32_t)(timer_ms() - t0) < 100u) return 0;
        if (bt_uart_set_baud(3200000u) == 0) {
            timer_delay_ms(20);
            if (bt_try("EDL version (3.2M)", edl_ver, sizeof edl_ver, 500u) > 0) {
                s_link_baud = 3200000u;
                bt_fw_set_nvm_baud(0x11u);       /* keep the chip here after reset */
                con_puts("bt: link is 3.2 Mbps - download will take ~1 s\n");
                st = 9; return 0;
            }
        }
        con_puts("bt: chip stayed at 115200; continuing slowly\n");
        bt_uart_set_baud(115200u);
        s_link_baud = 115200u;
        bt_fw_set_nvm_baud(0x00u);
        st = 9; return 0;

    case 9:
        bt_fw_report();
        if (bt_dl_start() < 0) { st = 90; return 1; }
        st = 4; return 0;

    case 4: {                                   /* one segment per iteration */
        int r = bt_dl_step();
        if (r < 0) { con_puts("bt: download failed\n"); st = 90; return 1; }
        if (r == 1) { t0 = timer_ms(); tries = 0; st = 5; }
        return 0;
    }

    case 5:                                     /* let it restart on the new fw */
        if ((uint32_t)(timer_ms() - t0) < 1000u) return 0;
        st = 6; return 0;

    case 6:
        if (bt_try("HCI Reset (post-download)", hci_reset, sizeof hci_reset, 1000u) > 0) {
            con_puts("bt: FIRMWARE RUNNING - controller is up\n");
            s_ready = 1; st = 10; return 0;
        }
        if (++tries >= 6) {
            con_puts("bt: silent after download at 115200\n"); st = 90; return 1;
        }
        return 0;

    case 10: {
        /* Ask the controller for its own address. The advertised name takes its
         * unique suffix from the WiFi MAC, which is all zeros here because
         * WCNSS never boots on this watch - hence "WatchFace-0001". The BT
         * address comes from the NVM and is per-unit. */
        static const uint8_t rd_bd[] = { 0x01, 0x09, 0x10, 0x00 };
        uint8_t rsp[32];
        while (bt_uart_getc() >= 0) { }
        bt_uart_write(rd_bd, sizeof rd_bd);
        int n = bt_uart_read(rsp, sizeof rsp, 500u);
        if (n >= 13 && rsp[0] == 0x04u && rsp[1] == 0x0Eu && rsp[6] == 0x00u) {
            for (int i = 0; i < 6; i++) s_bdaddr[i] = rsp[7 + i];   /* LSB first */
            con_puts("bt: BD_ADDR ");
            for (int i = 5; i >= 0; i--) { bt_hex2(s_bdaddr[i]); if (i) con_puts(":"); }
            con_puts("\n");
        } else {
            con_puts("bt: BD_ADDR read failed (name keeps its old suffix)\n");
        }
        st = 90; return 1;
    }

    default:
        return 1;
    }
}

/* One-shot bring-up probe. Order matters: the loopback runs BEFORE the chip is
 * powered, so a failure there is unambiguously our UART and not the radio. */
void bt_wcn3990_probe(void)
{
    static const uint8_t edl_ver[] = { 0x01, 0x00, 0xFC, 0x01, 0x19 };
    static const uint8_t hci_reset[] = { 0x01, 0x03, 0x0C, 0x00 };

    con_puts("bt: ---- WCN3990 bring-up probe ----\n");
    if (bt_uart_loopback_test() < 0) {
        con_puts("bt: UART loopback FAILED - fix the port before blaming the chip\n");
        return;
    }
    if (bt_uart_init() < 0) { con_puts("bt: UART init failed\n"); return; }

    bt_wcn3990_power(1);
    /* SETTLE. 100 ms was not enough: v5 only got a reply because a failed
     * command's 500 ms timeout happened to sit in front of the reset, and v6
     * (reset first, 100 ms after power) got silence. The chip's own boot takes
     * longer than the power sequence, so wait properly and retry rather than
     * depend on the accident of what ran before. */
    timer_delay_ms(500);

    /* RESET FIRST. The first probe sent the vendor EDL command before any
     * reset and got nothing back, while the reset itself answered perfectly;
     * the download uses that same EDL opcode, so establish whether it works
     * on a freshly reset chip before building the transfer on top of it. */
    int n = 0;
    for (int attempt = 1; attempt <= 4 && n <= 0; attempt++) {
        con_puts("bt: reset attempt "); con_putdec((uint32_t)attempt); con_puts("\n");
        n = bt_try("HCI Reset", hci_reset, sizeof hci_reset, 600u);
        if (n <= 0) timer_delay_ms(300);
    }
    if (n > 0) con_puts("bt: CHIP ANSWERED - power, clocks, pins and framing all good\n");
    else     { con_puts("bt: no reply (check: CTS level, chip rails, baud)\n"); return; }

    int v = bt_try("EDL version request", edl_ver, sizeof edl_ver, 1000u);
    con_puts(v > 0 ? "bt: EDL responds after a reset - the firmware download path is open\n"
                   : "bt: EDL still silent after a reset - download needs another way in\n");

    /* Firmware side: prove we can find and read the files before moving 220 KB. */
    bt_fw_report();

#if defined(BT_DOWNLOAD)
    if (v > 0) {
        con_puts("bt: downloading firmware (this takes ~20 s at 115200)\n"); con_flush();
        if (bt_fw_download() == 0) {
            /* The chip runs the patch after a reset; a Command Complete here
             * means it came back up on the downloaded firmware. */
            /* The controller restarts on the downloaded firmware. Two things
             * can make it look dead here, so test them in order:
             *  1. it is simply slow to come back  - the vendor allows 10 s for
             *     this reset, the first attempt allowed 2, so be patient;
             *  2. it changed speed - the kernel REWRITES the baud rate inside
             *     the NVM before sending it and we sent the file verbatim, so
             *     the chip may now be at whatever rate that file specifies.
             *     Sweep the plausible rates and see which one answers. */
            /* The NVM is patched to keep 115200, so the first rate should
             * answer; the rest stay as a fallback in case a chip ignores it. */
            static const uint32_t rates[] = { 115200u, 3200000u, 3000000u, 921600u, 460800u };
            int r = 0;
            timer_delay_ms(1000);
            for (int a2 = 1; a2 <= 5 && r <= 0; a2++) {
                r = bt_try("HCI Reset (post-download)", hci_reset, sizeof hci_reset, 1500u);
                if (r <= 0) { wdog_pet(); deadman_kick(); timer_delay_ms(400); }
            }
            for (unsigned i = 1; i < sizeof rates / sizeof rates[0] && r <= 0; i++) {
                if (bt_uart_set_baud(rates[i]) < 0) continue;
                timer_delay_ms(50);
                for (int a2 = 0; a2 < 2 && r <= 0; a2++)
                    r = bt_try("HCI Reset (rate probe)", hci_reset, sizeof hci_reset, 800u);
                wdog_pet(); deadman_kick();
            }
            if (r > 0) con_puts("bt: FIRMWARE RUNNING - controller is up\n");
            else { con_puts("bt: silent at every rate after download\n");
                   bt_uart_set_baud(115200u); }
        }
    }
#endif
    con_puts("bt: ---- end probe ----\n");
    con_flush();
}

#endif /* PLAT_BT_UART_BASE */
