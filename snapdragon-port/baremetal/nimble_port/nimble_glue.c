/* nimble_glue.c — what the NimBLE host needs from the snapdragon-port runtime:
 *   - printf/vprintf output (BLE_HS_LOG) on the console: newlib's _write
 *   - bond/CCCD persistence for host/store/config, in the Preferences NVS
 *     (namespace "ble"), so pairings survive a reboot
 *   - the host task + start/stop helpers used by compat/ble/BLEDevice.cpp */
#include "syscfg/syscfg.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <errno.h>

/* ---- console for printf ------------------------------------------------ */
int _write(int fd, const char *buf, int len)
{
    int i;
    (void)fd;
    for (i = 0; i < len; i++) con_putc(buf[i]);
    return len;
}

/* ---- bond persistence ---------------------------------------------------
 * host/store/config keeps three RAM arrays; with BLE_STORE_CONFIG_PERSIST=0 its
 * persist hooks are inline no-ops, so we mirror the arrays ourselves: after
 * every write/delete the store calls ble_store_config_persist_*() (below,
 * strong symbols overriding nothing -- we call them from a status callback
 * instead), and at init we load them back. Kept simple: whole arrays as
 * blobs under one NVS namespace. */
extern struct ble_store_value_sec ble_store_config_our_secs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
extern int ble_store_config_num_our_secs;
extern struct ble_store_value_sec ble_store_config_peer_secs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
extern int ble_store_config_num_peer_secs;
extern struct ble_store_value_cccd ble_store_config_cccds[MYNEWT_VAL(BLE_STORE_MAX_CCCDS)];
extern int ble_store_config_num_cccds;

#define NS "ble"
static void load_blob(const char *key, void *arr, uint32_t elem, int max, int *num)
{
    int got = nvs_get(NS, key, arr, elem * (uint32_t)max);
    *num = (got > 0 && (uint32_t)got % elem == 0) ? got / (int)elem : 0;
    if (*num > max) *num = max;
}
void owf_ble_store_load(void)
{
    load_blob("our", ble_store_config_our_secs, sizeof ble_store_config_our_secs[0], MYNEWT_VAL(BLE_STORE_MAX_BONDS), &ble_store_config_num_our_secs);
    load_blob("peer", ble_store_config_peer_secs, sizeof ble_store_config_peer_secs[0], MYNEWT_VAL(BLE_STORE_MAX_BONDS), &ble_store_config_num_peer_secs);
    load_blob("cccd", ble_store_config_cccds, sizeof ble_store_config_cccds[0], MYNEWT_VAL(BLE_STORE_MAX_CCCDS), &ble_store_config_num_cccds);
    con_puts("ble: store loaded, bonds "); con_putdec((uint32_t)ble_store_config_num_peer_secs); con_puts("\n");
}
void owf_ble_store_save(void)
{
    nvs_put(NS, "our", 3, ble_store_config_our_secs, sizeof ble_store_config_our_secs[0] * (uint32_t)ble_store_config_num_our_secs);
    nvs_put(NS, "peer", 3, ble_store_config_peer_secs, sizeof ble_store_config_peer_secs[0] * (uint32_t)ble_store_config_num_peer_secs);
    nvs_put(NS, "cccd", 3, ble_store_config_cccds, sizeof ble_store_config_cccds[0] * (uint32_t)ble_store_config_num_cccds);
    nvs_commit();
}

/* Wrap the store callbacks so every successful write/delete is persisted. */
static ble_store_write_fn *s_orig_write;
static ble_store_delete_fn *s_orig_delete;
static int store_write(int obj_type, const union ble_store_value *val)
{
    int rc = s_orig_write(obj_type, val);
    if (rc == 0) owf_ble_store_save();
    return rc;
}
static int store_delete(int obj_type, const union ble_store_key *key)
{
    int rc = s_orig_delete(obj_type, key);
    if (rc == 0) owf_ble_store_save();
    return rc;
}
void owf_ble_store_hook(void)
{
    if (s_orig_write) return;
    s_orig_write = ble_hs_cfg.store_write_cb;
    s_orig_delete = ble_hs_cfg.store_delete_cb;
    ble_hs_cfg.store_write_cb = store_write;
    ble_hs_cfg.store_delete_cb = store_delete;
}

/* ---- host task ----------------------------------------------------------- */
static TaskHandle_t s_host_task;
static void host_task(void *arg)
{
    struct ble_npl_eventq *q = nimble_port_get_dflt_eventq();
    uint32_t n = 0;
    (void)arg;
    con_puts("bt-host: started\n");
    for (;;) {
        struct ble_npl_event *ev = ble_npl_eventq_get(q, ble_npl_time_ms_to_ticks32(5000));
        if (!ev) {
#if defined(BT_TRACE)
            con_puts("bt-host: idle, events so far "); con_putdec(n); con_puts(", stack free "); con_putdec(uxTaskGetStackHighWaterMark(NULL)); con_puts(" words\n");
#endif
            continue;
        }
        n++;
#if defined(BT_TRACE)
        if (n <= 200u) { con_puts("bt-host: ev fn="); con_puthex((uint32_t)(uintptr_t)ble_npl_event_get_arg(ev)); con_puts("\n"); }
#endif
        ble_npl_event_run(ev);
    }
}

/* ---- make a runtime assert/abort visible instead of a silent spin --------- */
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    con_puts("ASSERT "); con_puts(expr ? expr : "?"); con_puts(" at "); con_puts(file ? file : "?"); con_puts(":"); con_putdec((uint32_t)line);
    con_puts(" in "); con_puts(func ? func : "?"); con_puts(" task "); con_puts(pcTaskGetName(NULL)); con_puts("\n");
    /* continue: the host's asserts here are diagnostics, not memory safety */
}
void abort(void)
{
    con_puts("ABORT in task "); con_puts(pcTaskGetName(NULL)); con_puts(" -- parking it\n");
    for (;;) vTaskDelay(1000);
}
void owf_ble_host_task_start(void)
{
    if (s_host_task) return;
    if (xTaskCreate(host_task, "bt-host", 6144, 0, 3, &s_host_task) != pdPASS) con_puts("bt-host: TASK CREATE FAILED\n");
}

/* ---- fake SCB for npl_os_freertos.c's in_isr() (see npl_hw.h) ---------- */
#include "npl_hw.h"
struct owf_fake_scb owf_fake_scb = { 0 };

/* Full newlib (the 8909w builds dropped nano.specs so printf handles %llu):
 * libc's fini walker and libstdc++'s static-init code want these two. */
void *__dso_handle = 0;
void _fini(void) {}
