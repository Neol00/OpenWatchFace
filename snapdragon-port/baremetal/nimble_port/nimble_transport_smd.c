/* nimble_transport_smd.c — NimBLE "custom LL" transport over platform/bt_hci.c.
 *
 * Host -> controller: commands and ACL go straight to the two SMD channels.
 * Controller -> host: a small task ("bt-hci") polls bt_hci_poll(); its
 * callbacks copy each event into a transport event buffer and each ACL packet
 * into an mbuf, then hand them to the host via ble_transport_to_hs_*(). */
#include "syscfg/syscfg.h"
#include "os/os.h"
#include "os/os_mbuf.h"
#include "nimble/transport.h"
#include "nimble/hci_common.h"
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"

static TaskHandle_t s_rx_task;
static volatile int s_rx_run;
static uint32_t s_drop_evt, s_drop_acl;

/* -DBT_TRACE: one line per HCI packet for the first few hundred, so a log shows
 * what the controller reports when a phone connects. */
#if defined(BT_TRACE)
static uint32_t s_trace;
static void trace(const char *what, const uint8_t *p, uint32_t len)
{
    uint32_t i;
    if (s_trace++ > 400u && what[0] == 'a') return;     /* ACL dumps stop after 400 packets; cmd/evt stay */
    con_puts("bt: "); con_puts(what); con_puts(" ");
    for (i = 0; i < len && i < 12u; i++) { static const char h[] = "0123456789abcdef"; con_putc(h[p[i] >> 4]); con_putc(h[p[i] & 15u]); con_putc(' '); }
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

static void rx_task(void *arg)
{
    (void)arg;
    while (s_rx_run) {
        bt_hci_poll();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_rx_task = 0;
    vTaskDelete(NULL);
}

void ble_transport_ll_init(void)
{
    bt_hci_set_rx(on_evt, on_acl);
    if (!s_rx_task) {
        s_rx_run = 1;
        xTaskCreate(rx_task, "bt-hci", 2048, 0, 4, &s_rx_task);
    }
}

int ble_transport_to_ll_cmd_impl(void *buf)
{
    const uint8_t *cmd = buf;
    uint32_t len = 3u + cmd[2];
    int rc;
    trace("cmd", cmd, len);
    rc = bt_hci_send_cmd(cmd, len);
    ble_transport_free(buf);
    return rc == 0 ? 0 : BLE_ERR_MEM_CAPACITY;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{
    static uint8_t flat[4 + 1024];
    uint16_t len = OS_MBUF_PKTLEN(om);
    int rc = -1;
    if (len <= sizeof flat && os_mbuf_copydata(om, 0, len, flat) == 0) {
        trace("acl>", flat, len);
        rc = bt_hci_send_acl(flat, len);
    }
    os_mbuf_free_chain(om);
    return rc == 0 ? 0 : BLE_ERR_MEM_CAPACITY;
}

int ble_transport_to_ll_iso_impl(struct os_mbuf *om) { os_mbuf_free_chain(om); return BLE_ERR_UNSUPPORTED; }
