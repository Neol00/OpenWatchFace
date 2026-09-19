/* qseecom.c — the TrustZone application service, from bare metal.
 *
 * WHY THIS EXISTS: the Gen 5's BG co-processor (QCC1110) firmware is loaded by
 * a SIGNED TRUSTZONE APP called "bgapp", not by the AP directly. Linux does:
 *     qseecom_start_app(&handle, "bgapp", 4K)   -> TZ loads the app
 *     qseecom_send_command(handle, req, rsp)    -> BGPIL_AUTH_MDT,
 *                                                  BGPIL_IMAGE_LOAD, ...
 * and the app drives the BG's SPI bus from the secure side (that is what
 * qcom,shared_ee on spi@78B8000 means). So bringing the co-processor up
 * ourselves means speaking QSEECOM: this file.
 *
 * PROTOCOL (drivers/misc/qseecom.c, include/soc/qcom/qseecomi.h). Every call
 * is the legacy command-buffer SCM (our TZ answers the legacy convention on
 * the msm8909w watches):
 *   version:  svc 6 cmd 3, request = u32 feature (10), response.result = the
 *             QSEE version (0x1000000 = TZ.BF.4.0; below that the 32-bit
 *             request structs are the right ones, which is our case).
 *   app work: svc 0xFC (SCM_SVC_TZSCHEDULER) cmd 1, request struct starts
 *             with a u32 command id:
 *               3  APP_LOOKUP  { id, char name[100] }  -> is it loaded?
 *               1  APP_START   { id, mdt_len, img_len, phys, name[100] }
 *               6  CLIENT_SEND_DATA { id, app_id, req_ptr, req_len,
 *                                     rsp_ptr, rsp_len, sglist_ptr, len }
 *             response is always { result, resp_type, data }:
 *               result 0 = success, 1 = INCOMPLETE (TZ wants a LISTENER
 *               service — a Linux userspace thing we do not have), 2 =
 *               blocked on listener, 0xFFFFFFFF = failure.
 *               resp_type 0xEE01 = data is an app id, 0xEE02 = listener id.
 *
 * WHAT IS HERE: the version query and app lookup (both read-only, proven on
 * the watch in v253: version 0x00405000, bgapp not loaded), plus APP_START
 * and CLIENT_SEND_DATA, which do load and drive the app. The BG-specific
 * sequence built on top lives in bg_load.c.
 *
 * A listener request (result 1 = INCOMPLETE) is a HARD STOP here: listeners
 * are Linux userspace services (qseecomd) and this firmware has none, so
 * there is nothing to retry.
 */
#include "platform.h"
#if defined(PLAT_HAS_BG_QCC1110)

#include <string.h>

void scm_dcache_clean(const void *p, uint32_t n);
void scm_dcache_inval(const void *p, uint32_t n);

#define SCM_SVC_INFO_SVC        0x06u
#define SCM_INFO_GET_FEATURE    0x03u
#define QSEE_FEATURE_VERSION    10u

#define SCM_SVC_TZSCHEDULER     0xFCu
#define TZSCHED_CMD             0x01u

#define QSEOS_APP_START         0x01u
#define QSEOS_APP_LOOKUP        0x03u
#define QSEOS_CLIENT_SEND_DATA  0x06u

#define QSEOS_RESULT_SUCCESS    0u
#define QSEOS_RESULT_INCOMPLETE 1u
#define QSEOS_RESULT_BLOCKED    2u

#define QSEOS_APP_ID_RESP       0xEE01u
#define QSEOS_LISTENER_ID_RESP  0xEE02u

#define MAX_APP_NAME_SIZE       100

struct qsee_resp { uint32_t result, resp_type, data; };

struct qsee_lookup_req {
    uint32_t cmd_id;
    char     app_name[MAX_APP_NAME_SIZE];
} __attribute__((packed));

int qsee_version(uint32_t *ver)
{
    struct qsee_resp resp;
    uint32_t feature = QSEE_FEATURE_VERSION;
    int rc;

    memset(&resp, 0, sizeof resp);
    rc = scm_legacy_buf(SCM_SVC_INFO_SVC, SCM_INFO_GET_FEATURE,
                        &feature, sizeof feature, &resp, sizeof resp);
    if (rc) return rc;
    if (ver) *ver = resp.result;
    return 0;
}

int qsee_app_lookup(const char *name, uint32_t *app_id)
{
    struct qsee_lookup_req req;
    struct qsee_resp resp;
    int rc;
    unsigned i;

    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.cmd_id = QSEOS_APP_LOOKUP;
    for (i = 0; i + 1 < MAX_APP_NAME_SIZE && name[i]; i++) req.app_name[i] = name[i];

    rc = scm_legacy_buf(SCM_SVC_TZSCHEDULER, TZSCHED_CMD,
                        &req, sizeof req, &resp, sizeof resp);
    if (rc) return rc;

    if (app_id) *app_id = 0;
    /* FAILURE here is not an error: it is how TZ says "no such app". */
    if (resp.result == 0xFFFFFFFFu) return 0;
    if (resp.result != QSEOS_RESULT_SUCCESS) return -(int)(1000u + resp.result);
    if (resp.resp_type == QSEOS_APP_ID_RESP) {
        if (app_id) *app_id = resp.data;
        return 0;
    }
    return -(int)(2000u + resp.resp_type);
}

/* ---- APP_START: hand TZ a signed app image and have it loaded ------------
 * The buffer must be physically contiguous and cache-clean: TZ reads it with
 * the MMU off. Ours is ordinary .bss (VA == PA here), cleaned below. */
struct qsee_load_req {
    uint32_t cmd_id;
    uint32_t mdt_len;
    uint32_t img_len;
    uint32_t phy_addr;
    char     app_name[MAX_APP_NAME_SIZE];
} __attribute__((packed));

int qsee_app_start(const char *name, const void *img, uint32_t mdt_len,
                   uint32_t img_len, uint32_t *app_id)
{
    struct qsee_load_req req;
    struct qsee_resp resp;
    int rc;
    unsigned i;

    if (app_id) *app_id = 0;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.cmd_id   = QSEOS_APP_START;
    req.mdt_len  = mdt_len;
    req.img_len  = img_len;
    req.phy_addr = (uint32_t)(uintptr_t)img;
    for (i = 0; i + 1 < MAX_APP_NAME_SIZE && name[i]; i++) req.app_name[i] = name[i];

    scm_dcache_clean(img, img_len);
    rc = scm_legacy_buf(SCM_SVC_TZSCHEDULER, TZSCHED_CMD,
                        &req, sizeof req, &resp, sizeof resp);
    if (rc) return rc;

    if (resp.result == QSEOS_RESULT_INCOMPLETE ||
        resp.result == QSEOS_RESULT_BLOCKED) {
        /* TZ wants a listener service (Linux userspace qseecomd). We have
         * none, so this is a wall, not something to retry. */
        con_puts("qsee: TZ returned INCOMPLETE (wants listener id ");
        con_putdec(resp.data); con_puts(") -- no listener exists in this firmware\n");
        return -(int)(3000u + resp.data);
    }
    if (resp.result != QSEOS_RESULT_SUCCESS) return -(int)(1000u + resp.result);
    if (app_id) *app_id = resp.data;
    return 0;
}

/* ---- CLIENT_SEND_DATA: a request/response pair into a loaded app --------- */
struct qsee_send_req {
    uint32_t cmd_id;
    uint32_t app_id;
    uint32_t req_ptr;
    uint32_t req_len;
    uint32_t rsp_ptr;
    uint32_t rsp_len;
    uint32_t sglistinfo_ptr;
    uint32_t sglistinfo_len;
} __attribute__((packed));

/* struct sglist_info[MAX_ION_FD] = {u32,u32} * 4 = 32 bytes, zeroed: we pass
 * no file descriptors, so the table is empty but must still be supplied. */
#define SGLISTINFO_TABLE_SIZE 32u
static uint8_t s_sglist[SGLISTINFO_TABLE_SIZE] __attribute__((aligned(64)));

int qsee_send_cmd(uint32_t app_id, const void *req, uint32_t req_len,
                  void *rsp, uint32_t rsp_len)
{
    struct qsee_send_req sreq;
    struct qsee_resp resp;
    int rc;

    memset(&sreq, 0, sizeof sreq);
    memset(&resp, 0, sizeof resp);
    memset(s_sglist, 0, sizeof s_sglist);
    sreq.cmd_id         = QSEOS_CLIENT_SEND_DATA;
    sreq.app_id         = app_id;
    sreq.req_ptr        = (uint32_t)(uintptr_t)req;
    sreq.req_len        = req_len;
    sreq.rsp_ptr        = (uint32_t)(uintptr_t)rsp;
    sreq.rsp_len        = rsp_len;
    sreq.sglistinfo_ptr = (uint32_t)(uintptr_t)s_sglist;
    sreq.sglistinfo_len = SGLISTINFO_TABLE_SIZE;

    scm_dcache_clean(req, req_len);
    scm_dcache_clean(rsp, rsp_len);
    scm_dcache_clean(s_sglist, sizeof s_sglist);

    rc = scm_legacy_buf(SCM_SVC_TZSCHEDULER, TZSCHED_CMD,
                        &sreq, sizeof sreq, &resp, sizeof resp);
    if (rc) return rc;

    if (resp.result == QSEOS_RESULT_INCOMPLETE ||
        resp.result == QSEOS_RESULT_BLOCKED) {
        con_puts("qsee: send_cmd INCOMPLETE (listener ");
        con_putdec(resp.data); con_puts(") -- unsupported here\n");
        return -(int)(3000u + resp.data);
    }
    if (resp.result != QSEOS_RESULT_SUCCESS) return -(int)(1000u + resp.result);
    scm_dcache_inval(rsp, rsp_len);
    return 0;
}

void qsee_report(void)
{
    uint32_t ver = 0, app_id = 0;
    int rc;

    con_puts("qsee: convention "); con_puts(scm_convention_name()); con_puts("\n");

    rc = qsee_version(&ver);
    con_puts("qsee: version query rc="); con_putdec((uint32_t)rc);
    con_puts(" version="); con_puthex(ver);
    con_puts(ver >= 0x1000000u ? " (>= TZ.BF.4.0: 64-bit request structs)\n"
                               : " (< TZ.BF.4.0: 32-bit request structs)\n");
    if (rc) { con_puts("qsee: TZ did not answer the version query -- app loading not attempted\n"); return; }

    rc = qsee_app_lookup("bgapp", &app_id);
    con_puts("qsee: bgapp lookup rc="); con_putdec((uint32_t)rc);
    con_puts(" app_id="); con_putdec(app_id);
    con_puts(app_id ? " (ALREADY LOADED -- can send commands straight away)\n"
                    : " (not loaded -- APP_START needed)\n");
}

#endif /* PLAT_HAS_BG_QCC1110 */
