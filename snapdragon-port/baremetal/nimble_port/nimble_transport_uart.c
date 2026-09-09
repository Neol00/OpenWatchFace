/* nimble_transport_uart.c — NimBLE transport over H4 on a UART (Fossil Gen 6).
 *
 * The 8909w watches reach their controller through shared memory
 * (nimble_transport_smd.c). The Gen 6's controller is a separate WCN3990 on
 * blsp2_uart2, so the same host stack rides on H4 framing instead:
 *
 *     0x01 <opcode lo> <opcode hi> <plen> <params...>     command  (host -> ctrl)
 *     0x02 <handle lo> <handle hi> <len lo> <len hi> ...  ACL      (both ways)
 *     0x04 <event code> <plen> <params...>                event    (ctrl -> host)
 *
 * The controller must already be powered and running its firmware
 * (bt_wcn3990.c) before ble_transport_ll_init() is called.
 */
#include "syscfg/syscfg.h"
#include "os/os.h"
#include "os/os_mbuf.h"
#include "nimble/transport.h"
#include "nimble/hci_common.h"
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

static TaskHandle_t s_rx_task;
static volatile int s_rx_run;
static uint32_t s_drop_evt, s_drop_acl;

#if defined(BT_TRACE)
static void trace(const char *what, const uint8_t *p, uint32_t len)
{
    con_puts("bt: "); con_puts(what); con_puts(" ");
    for (uint32_t i = 0; i < len && i < 12u; i++) { bt_hex2(p[i]); con_putc(' '); }
    if (len > 12u) con_puts("..");
    con_puts("(len "); con_putdec(len); con_puts(")\n");
}
#else
#define trace(w, p, l) do { } while (0)
#endif

static void on_evt(const uint8_t *pkt, uint32_t len)
{
    int discardable = (pkt[0] == BLE_HCI_EVCODE_LE_META && len > 2 &&
                       (pkt[2] == BLE_HCI_LE_SUBEV_ADV_RPT || pkt[2] == BLE_HCI_LE_SUBEV_EXT_ADV_RPT));
    uint8_t *buf;
    trace("evt", pkt, len);
    if (len > MYNEWT_VAL(BLE_TRANSPORT_EVT_SIZE)) { s_drop_evt++; return; }
    buf = ble_transport_alloc_evt(discardable);
    if (!buf) { s_drop_evt++; return; }
    memcpy(buf, pkt, len);
    if (ble_transport_to_hs_evt(buf) != 0) { ble_transport_free(buf); con_puts("bt: host refused an event\n"); }
}

static void on_acl(const uint8_t *pkt, uint32_t len)
{
    struct os_mbuf *om = ble_transport_alloc_acl_from_ll();
    trace("acl<", pkt, len);
    if (!om) { s_drop_acl++; return; }
    if (os_mbuf_append(om, pkt, (uint16_t)len) != 0) { os_mbuf_free_chain(om); s_drop_acl++; return; }
    if (ble_transport_to_hs_acl(om) != 0) { os_mbuf_free_chain(om); con_puts("bt: host refused ACL\n"); }
}

/* Cooperative send: never spin on the transmitter. The controller throttles
 * with CTS, and a spin here would block this task (and, through it, the host)
 * for as long as the throttle lasts. */
static int uart_send(const uint8_t *b, uint32_t n)
{
    uint32_t i = 0, t0 = timer_ms();
    while (i < n) {
        if (bt_uart_try_putc(b[i])) { i++; t0 = timer_ms(); continue; }
        if ((uint32_t)(timer_ms() - t0) > 500u) return -1;    /* wedged */
        vTaskDelay(1);
    }
    return 0;
}

/* H4 receive state machine. Bytes arrive one at a time from the UART queue. */
static void rx_task(void *arg)
{
    static uint8_t pkt[1 + 4 + 1024];
    enum { WANT_TYPE, EVT_HDR, EVT_BODY, ACL_HDR, ACL_BODY } st = WANT_TYPE;
    uint32_t have = 0, need = 0;
    (void)arg;

    while (s_rx_run) {
        int c = bt_uart_getc();
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

        switch (st) {
        case WANT_TYPE:
            if (c == 0x04) { st = EVT_HDR; have = 0; need = 2; }
            else if (c == 0x02) { st = ACL_HDR; have = 0; need = 4; }
            /* 0xFD/0xFC/0xFE are in-band-sleep markers; the NVM patch disables
             * IBS, so anything else here is noise and is simply dropped. */
            break;

        case EVT_HDR:
            pkt[have++] = (uint8_t)c;
            if (have == need) {
                need = pkt[1];                       /* parameter length */
                if (!need) { on_evt(pkt, 2); st = WANT_TYPE; }
                else { st = EVT_BODY; }
            }
            break;

        case EVT_BODY:
            pkt[have++] = (uint8_t)c;
            if (have == 2u + need) { on_evt(pkt, have); st = WANT_TYPE; }
            break;

        case ACL_HDR:
            pkt[have++] = (uint8_t)c;
            if (have == need) {
                need = (uint32_t)pkt[2] | ((uint32_t)pkt[3] << 8);
                if (!need) { on_acl(pkt, 4); st = WANT_TYPE; }
                else if (need > sizeof pkt - 4u) { st = WANT_TYPE; s_drop_acl++; }
                else { st = ACL_BODY; }
            }
            break;

        case ACL_BODY:
            pkt[have++] = (uint8_t)c;
            if (have == 4u + need) { on_acl(pkt, have); st = WANT_TYPE; }
            break;
        }
    }
    s_rx_task = 0;
    vTaskDelete(NULL);
}

void ble_transport_ll_init(void)
{
    if (!s_rx_task) {
        s_rx_run = 1;
        xTaskCreate(rx_task, "bt-h4", 2048, 0, 4, &s_rx_task);
    }
}

int ble_transport_to_ll_cmd_impl(void *buf)
{
    const uint8_t *cmd = buf;
    uint32_t len = 3u + cmd[2];
    uint8_t type = 0x01u;
    int rc;
    trace("cmd", cmd, len);
    rc = uart_send(&type, 1);
    if (rc == 0) rc = uart_send(cmd, len);
    ble_transport_free(buf);
    return rc == 0 ? 0 : BLE_ERR_MEM_CAPACITY;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{
    static uint8_t flat[4 + 1024];
    uint16_t len = OS_MBUF_PKTLEN(om);
    uint8_t type = 0x02u;
    int rc = -1;
    if (len <= sizeof flat && os_mbuf_copydata(om, 0, len, flat) == 0) {
        trace("acl>", flat, len);
        rc = uart_send(&type, 1);
        if (rc == 0) rc = uart_send(flat, len);
    }
    os_mbuf_free_chain(om);
    return rc == 0 ? 0 : BLE_ERR_MEM_CAPACITY;
}

int ble_transport_to_ll_iso_impl(struct os_mbuf *om) { os_mbuf_free_chain(om); return BLE_ERR_UNSUPPORTED; }
