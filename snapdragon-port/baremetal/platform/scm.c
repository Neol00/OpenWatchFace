/* scm.c — Qualcomm SCM: calls into TrustZone (step 1 of WIFI-BRINGUP.md).
 *
 * WHY: the WCNSS firmware is signed. Only the secure world can copy-check it,
 * program the XPU to protect its memory and release the Pronto core from
 * reset ("PAS": pas_init_image / pas_mem_setup / pas_auth_and_reset). Those
 * are SCM calls, and this file is the wrapper they go through.
 *
 * TWO CONVENTIONS, chosen at run time (2026-09-03):
 *
 * SMCCC ("scm_call2", mainline SMC_CONVENTION_ARM_32, qcom_scm-smc.c) --
 * the Gen 6 / sda429w. Evidence: the Asteroid dmesg prints "scm_call failed:
 * func id 0x2000c16", an SCM_SIP_FNID(svc, cmd) value.
 *   r0 = 0x02000000 | (svc << 8) | cmd      STD call, SMC32, owner SIP
 *   r1 = arginfo   (nargs | type bits: 0=value, 1=RO buffer, 2=RW buffer)
 *   r2..r5 = args[0..3]; r6 = phys addr of a u32[] holding args[4..]
 *   returns r0 = status (0 ok, 1 = interrupted -> retry, -12 = busy -> retry),
 *           r1..r3 = results
 *
 * LEGACY (mainline qcom_scm-legacy.c, the 3.18 kernel's scm_call()) -- the
 * msm8909w watches (Gen 4, TicWatch C2). TZ is handed ONE physical address of
 * a command buffer in DDR, reads the arguments out of it and writes the
 * result back into a response header that follows the arguments:
 *   r0 = 1 (SCM_LEGACY_CMD), r1 = &context_id, r2 = phys(command)
 *   command  { len, buf_offset, resp_hdr_offset, id = (svc<<10)|cmd, args[] }
 *   response { len, buf_offset, is_complete }  + result words
 *   returns r0 = status (0 ok, 1 = interrupted -> retry); the TZ result is
 *   the u32 at response + response.buf_offset once is_complete is set.
 * The buffer is cacheable .bss here (VA == PA), so it is cleaned before the
 * SMC and invalidated before every read of what TZ wrote -- the 3.18 driver
 * does exactly that with __cpuc_flush_dcache_area / dmac_inv_range.
 *
 * DETECTION is the 3.18 kernel's own is_scm_armv8() probe: issue the SMCCC
 * INFO/IS_CALL_AVAIL for itself; a TZ that speaks SMCCC answers status 0 with
 * r1 = 1, a legacy TZ rejects the unknown function id. That call changes
 * nothing either way. A board that declares PLAT_SCM_SMCCC skips the probe
 * (the Gen 6, where the convention is proven) and never touches the legacy
 * path.
 *
 * VALIDATION FIRST, as always: the only call this file makes on its own is
 * INFO/IS_CALL_AVAIL, which asks TZ "do you implement call X?" and has no
 * side effects. Prove the convention with a harmless call before issuing
 * one that can reset the SoC.
 */
#include "platform.h"
#if defined(PLAT_SMEM_BASE)

#include <string.h>

#define SCM_OWNER_SIP        0x02000000u
#define SCM_STD_CALL         0x00000000u
#define SCM_FNID(svc, cmd)   (SCM_OWNER_SIP | SCM_STD_CALL | (((svc) & 0xFFu) << 8) | ((cmd) & 0xFFu))
#define SCM_LEGACY_FNID(svc, cmd) ((((svc) & 0xFFu) << 10) | ((cmd) & 0x3FFu))

#define SCM_SVC_INFO         0x06u
#define SCM_INFO_IS_CALL_AVAIL 0x01u
#define SCM_SVC_PIL          0x02u
#define SCM_PIL_PAS_INIT_IMAGE     0x01u
#define SCM_PIL_PAS_MEM_SETUP      0x02u
#define SCM_PIL_PAS_AUTH_AND_RESET 0x05u
#define SCM_PIL_PAS_SHUTDOWN       0x06u
#define SCM_PIL_PAS_IS_SUPPORTED   0x07u

#define SCM_INTERRUPTED      1
#define SCM_V2_EBUSY         (-12)

enum { CONV_UNKNOWN = 0, CONV_SMCCC, CONV_LEGACY };
#if defined(PLAT_SCM_SMCCC)
static int s_conv = CONV_SMCCC;
#else
static int s_conv = CONV_UNKNOWN;
#endif

/* ---- cache maintenance for buffers TZ reads/writes behind the MMU ------- */
static void dc_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    for (; a < end; a += 32u)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");   /* DCCMVAC */
    __asm__ volatile("dsb sy" ::: "memory");
}
static void dc_inval(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < end; a += 32u)
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");    /* DCIMVAC */
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- SMCCC convention --------------------------------------------------- */
/* Up to 4 register args; result r1 returned through *r1. */
static int32_t scm_smc(uint32_t fnid, uint32_t arginfo,
                       uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                       uint32_t *r1_out)
{
    int32_t st;
    uint32_t tries = 0, t0 = timer_ms();

    /* SCM_INTERRUPTED / V2_EBUSY are "call me again", not errors, and a long
     * PAS call (auth_and_reset hashes the whole image) returns them for every
     * IRQ or busy slot until it finishes. The Gen 4 needed 51 re-entries in
     * the legacy path; the 20-try cap here would have failed the same way.
     * Bound by wall clock instead, petting the watchdog. */
    do {
        register uint32_t r0 __asm__("r0") = fnid;
        register uint32_t r1 __asm__("r1") = arginfo;
        register uint32_t r2 __asm__("r2") = a0;
        register uint32_t r3 __asm__("r3") = a1;
        register uint32_t r4 __asm__("r4") = a2;
        register uint32_t r5 __asm__("r5") = a3;
        register uint32_t r6 __asm__("r6") = 0;      /* no extra-args buffer */
        __asm__ volatile(".arch_extension sec\n\tsmc #0"
                         : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                         : "r"(r4), "r"(r5), "r"(r6)
                         : "memory");
        st = (int32_t)r0;
        if (r1_out) *r1_out = r1;
        if (st == SCM_V2_EBUSY) timer_delay_ms(30);
        if (st == SCM_INTERRUPTED || st == SCM_V2_EBUSY) { tries++; if ((tries & 0xFFu) == 0u) wdog_pet(); }
    } while ((st == SCM_INTERRUPTED || st == SCM_V2_EBUSY) && timer_ms() - t0 < 30000u);
    if (tries) { con_dbg("[re-entered "); con_dbg_dec(tries); con_dbg("x] "); }
    if (st == SCM_INTERRUPTED || st == SCM_V2_EBUSY) return -110;

    return st;
}

/* ---- legacy convention -------------------------------------------------- */
struct legacy_cmd { uint32_t len, buf_offset, resp_hdr_offset, id; };
struct legacy_rsp { uint32_t len, buf_offset, is_complete; };

static uint8_t  s_legacy_buf[256] __attribute__((aligned(64)));
static uint32_t s_legacy_ctx __attribute__((aligned(64)));

/* One legacy call: `nargs` u32 arguments in, one u32 TZ result out.
 * Returns the SMC status (0 ok, <0 transport/TZ error). */
static int32_t scm_legacy(uint32_t svc, uint32_t cmd, const uint32_t *args, uint32_t nargs,
                          uint32_t *result)
{
    struct legacy_cmd *c = (struct legacy_cmd *)s_legacy_buf;
    uint32_t arglen = nargs * 4u, rsp_off = (uint32_t)sizeof *c + arglen;
    struct legacy_rsp *r = (struct legacy_rsp *)(s_legacy_buf + rsp_off);
    uint32_t total = rsp_off + (uint32_t)sizeof *r + 4u, i, t0;
    int32_t st;
    int tries = 0;

    memset(s_legacy_buf, 0, sizeof s_legacy_buf);
    c->len = total;
    c->buf_offset = sizeof *c;
    c->resp_hdr_offset = rsp_off;
    c->id = SCM_LEGACY_FNID(svc, cmd);
    for (i = 0; i < nargs; i++) ((uint32_t *)(s_legacy_buf + sizeof *c))[i] = args[i];
    s_legacy_ctx = 0;
    dc_clean(s_legacy_buf, sizeof s_legacy_buf);
    dc_clean(&s_legacy_ctx, sizeof s_legacy_ctx);

    /* SCM_INTERRUPTED (1) is NOT an error in this convention: TZ returns it
     * every time a non-secure IRQ arrives mid-call and expects the SAME SMC
     * re-issued (context_id carries the resume state) until it answers 0.
     * pas_auth_and_reset hashes the whole 5 MB image and our 1 kHz tick
     * interrupts it hundreds of times; the Gen 4's v3 run gave up after 20
     * re-entries and reported -110 while TZ was still working. Re-enter for
     * as long as it takes (bounded only by a generous wall clock), petting
     * the watchdog on the way. */
    t0 = timer_ms();
    do {
        register uint32_t r0 __asm__("r0") = 1u;                                   /* SCM_LEGACY_CMD */
        register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&s_legacy_ctx;
        register uint32_t r2 __asm__("r2") = (uint32_t)(uintptr_t)s_legacy_buf;    /* VA == PA */
        __asm__ volatile(".arch_extension sec\n\tsmc #0"
                         : "+r"(r0), "+r"(r1), "+r"(r2)
                         :
                         : "r3", "memory");
        st = (int32_t)r0;
        if (st == SCM_INTERRUPTED) {
            tries++;
            if ((tries & 0xFFu) == 0u) wdog_pet();
            /* a livelock (IRQ never serviced between re-entries) shows up
             * here as an ever-growing count with no progress */
            if (tries == 1000 || tries == 10000 || (tries % 100000) == 0) {
                con_dbg("[scm: re-entered "); con_dbg_dec((uint32_t)tries); con_dbg("x, ctx=");
                dc_inval(&s_legacy_ctx, sizeof s_legacy_ctx); con_dbg_hex(s_legacy_ctx);
                con_dbg("] "); con_flush(); usb_poll();
            }
        }
    } while (st == SCM_INTERRUPTED && timer_ms() - t0 < 30000u);
    if (tries) { con_dbg("[re-entered "); con_dbg_dec((uint32_t)tries); con_dbg("x] "); }
    if (st < 0) return st;
    if (st == SCM_INTERRUPTED) return -110;   /* 30 s of re-entry: give up */

    /* TZ completes the response asynchronously in principle; poll like the
     * kernel does, invalidating our stale cache line before every look. */
    t0 = timer_ms();
    for (;;) {
        dc_inval(r, sizeof *r + 4u);
        if (r->is_complete) break;
        if (timer_ms() - t0 > 1000u) return -110;     /* ETIMEDOUT */
    }
    if (result) {
        uint32_t off = r->buf_offset;
        if (off > sizeof s_legacy_buf - rsp_off - 4u) off = sizeof *r;   /* implausible: assume it follows the header */
        dc_inval(s_legacy_buf + rsp_off + off, 4u);
        *result = *(volatile uint32_t *)(s_legacy_buf + rsp_off + off);
    }
    return st;
}

/* Legacy ATOMIC call (mainline scm_legacy_call_atomic): no command buffer,
 * arguments in registers, TZ masks interrupts for the duration so it never
 * returns INTERRUPTED. Only for short calls; mainline uses it for
 * INFO/IS_CALL_AVAIL exactly as done here.
 *   r0 = (FNID << 12) | CLASS_REGISTER (0x2<<8) | MASK_IRQS (bit 5) | nargs
 *   r1 = &context_id, r2.. = args; returns r0 status, r1 result */
static int32_t scm_legacy_atomic1(uint32_t svc, uint32_t cmd, uint32_t arg, uint32_t *result)
{
    uint32_t id = (SCM_LEGACY_FNID(svc, cmd) << 12) | (0x2u << 8) | (1u << 5) | 1u;
    register uint32_t r0 __asm__("r0") = id;
    register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&s_legacy_ctx;
    register uint32_t r2 __asm__("r2") = arg;
    s_legacy_ctx = 0;
    dc_clean(&s_legacy_ctx, sizeof s_legacy_ctx);
    __asm__ volatile(".arch_extension sec\n\tsmc #0"
                     : "+r"(r0), "+r"(r1), "+r"(r2)
                     :
                     : "r3", "memory");
    if (result) *result = r1;
    return (int32_t)r0;
}

/* ---- msm8909w power collapse (legacy convention only) ---------------------
 * scm-boot.c: SCM_SVC_BOOT(1) / SCM_BOOT_ADDR(1) with {flags, addr}; flags
 * SCM_FLAG_WARMBOOT_CPU0 = 0x04. TZ then re-enters us at `addr` (MMU off,
 * ARM state) after every power collapse of cpu0.
 * msm-pm.c: SCM_SVC_BOOT / SCM_CMD_TERMINATE_PC(2), atomic, arg = L2 flag
 * (0 = L2 stays on, 1 = L2 off, 3 = GDHS). Does not return on success. */
#define SCM_SVC_BOOT           0x01u
#define SCM_BOOT_ADDR          0x01u
#define SCM_CMD_TERMINATE_PC   0x02u
#define SCM_FLAG_WARMBOOT_CPU0 0x04u
int scm_set_boot_addr(uint32_t addr, uint32_t flags)
{
    uint32_t args[2] = { flags, addr }, res = 0;
    int32_t st = scm_legacy(SCM_SVC_BOOT, SCM_BOOT_ADDR, args, 2, &res);
    return st == 0 ? (int)res : (int)st;
}
int scm_set_warmboot_addr(uint32_t addr) { return scm_set_boot_addr(addr, SCM_FLAG_WARMBOOT_CPU0); }
/* Multi-cluster boot API (scm-boot.c scm_set_boot_addr_mc, 3.18): the kernel
 * PREFERS this over the per-CPU flags whenever TZ offers it
 * (msm_pm_tz_boot_init -> scm_is_mc_boot_available). One address for every
 * core selected by affinity masks; flags WARMBOOT_MC 0x04 / COLDBOOT_MC 0x02,
 * SCM_FLAG_HLOS is 0 on arm32. Every cluster-off wake (v153..v159) reset the
 * watch before TZ re-entered the legacy-registered address, and CPU1 never
 * warm-booted through its legacy flag either: this is the API TZ honours. */
#define SCM_BOOT_ADDR_MC       0x11u
#define SCM_FLAG_WARMBOOT_MC   0x04u
#define SCM_FLAG_COLDBOOT_MC   0x02u
int scm_mc_boot_available(void) { return scm_is_call_available(SCM_SVC_BOOT, SCM_BOOT_ADDR_MC) == 1; }
int scm_set_boot_addr_mc(uint32_t addr, uint32_t flags)
{
    uint32_t args[6] = { addr, ~0u, ~0u, ~0u, ~0u, flags }, res = 0;
    int32_t st = scm_legacy(SCM_SVC_BOOT, SCM_BOOT_ADDR_MC, args, 6, &res);
    return st == 0 ? (int)res : (int)st;
}
int scm_set_warmboot_addr_mc_all(uint32_t addr) { return scm_set_boot_addr_mc(addr, SCM_FLAG_WARMBOOT_MC); }
#define SCM_FLAG_WARMBOOT_CPU1 0x02u
int scm_set_warmboot_addr_cpu1(uint32_t addr) { return scm_set_boot_addr(addr, SCM_FLAG_WARMBOOT_CPU1); }
int scm_terminate_pc(uint32_t l2_flag)
{
    uint32_t res = 0;
    return (int)scm_legacy_atomic1(SCM_SVC_BOOT, SCM_CMD_TERMINATE_PC, l2_flag, &res);
}

/* ---- convention probe --------------------------------------------------- */
static void scm_probe(void)
{
    uint32_t avail = 0;
    int32_t st;
    if (s_conv != CONV_UNKNOWN) return;
    /* is_scm_armv8(): can TZ answer an SMCCC-form "is INFO/IS_CALL_AVAIL
     * available" for itself? Harmless in both worlds. */
    st = scm_smc(SCM_FNID(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL), 1u,
                 SCM_FNID(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL), 0, 0, 0, &avail);
    s_conv = (st == 0 && avail == 1u) ? CONV_SMCCC : CONV_LEGACY;
    con_dbg("scm: probe r0="); con_dbg_hex((uint32_t)st); con_dbg(" r1="); con_dbg_hex(avail);
    con_dbg(s_conv == CONV_SMCCC ? " -> SMCCC (scm_call2) convention\n"
                                  : " -> LEGACY command-buffer convention\n");
    con_flush(); usb_poll();
}

const char *scm_convention_name(void)
{
    scm_probe();
    return s_conv == CONV_SMCCC ? "smccc" : "legacy";
}

/* 1 = TZ implements svc/cmd, 0 = it does not, <0 = the query itself failed
 * (which on a first run means the calling convention is wrong). */
int scm_is_call_available(uint32_t svc, uint32_t cmd)
{
    uint32_t avail = 0;
    int32_t st;
    scm_probe();
    if (s_conv == CONV_SMCCC) {
        st = scm_smc(SCM_FNID(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL),
                     1u /* one value arg */, SCM_FNID(svc, cmd), 0, 0, 0, &avail);
    } else {
        /* v8: the atomic form. The v7 Gen 4 run hung inside the THIRD of
         * six buffer-form queries that had passed in every earlier build;
         * the atomic form has no buffer and no re-entry to get wrong. */
        st = scm_legacy_atomic1(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL, SCM_LEGACY_FNID(svc, cmd), &avail);
    }
    if (st != 0) return (int)st;
    return avail ? 1 : 0;
}

/* ---- PAS: Peripheral Authentication Service ------------------------------
 * The four calls that boot a signed co-processor image. Argument shapes are
 * from subsys-pil-tz.c and match mainline qcom_scm.c in both conventions:
 *   init_image(pas_id, mdt_phys)         SMCCC arginfo SCM_ARGS(2, VAL, RW) = 0x82
 *   mem_setup(pas_id, base, size)        arginfo SCM_ARGS(3)                = 0x03
 *   auth_and_reset(pas_id)               arginfo SCM_ARGS(1)                = 0x01
 *   shutdown(pas_id)                     arginfo SCM_ARGS(1)
 * Each returns two words: the SMC status and TZ's own result. Both must be
 * zero; we fold them into one int (0 ok, else the first non-zero, TZ's
 * result negated and offset by 1000 so the two can be told apart in a log). */
static int scm_pas_call(uint32_t cmd, uint32_t arginfo, uint32_t nargs,
                        uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t r1 = 0;
    int32_t st;
    scm_probe();
    if (s_conv == CONV_SMCCC) {
        st = scm_smc(SCM_FNID(SCM_SVC_PIL, cmd), arginfo, a0, a1, a2, 0, &r1);
    } else {
        uint32_t args[3] = { a0, a1, a2 };
        st = scm_legacy(SCM_SVC_PIL, cmd, args, nargs, &r1);
    }
    con_dbg("[r0="); con_dbg_hex((uint32_t)st); con_dbg(" r1="); con_dbg_hex(r1); con_dbg("] ");
    if (st != 0) return (int)st;
    if (r1 != 0) return -(1000 + (int)r1);
    return 0;
}
int scm_pas_is_supported(uint32_t pas_id)
{
    uint32_t r1 = 0;
    int32_t st;
    scm_probe();
    if (s_conv == CONV_SMCCC)
        st = scm_smc(SCM_FNID(SCM_SVC_PIL, SCM_PIL_PAS_IS_SUPPORTED), 1u, pas_id, 0, 0, 0, &r1);
    else
        st = scm_legacy(SCM_SVC_PIL, SCM_PIL_PAS_IS_SUPPORTED, &pas_id, 1u, &r1);
    con_dbg("[r0="); con_dbg_hex((uint32_t)st); con_dbg(" r1="); con_dbg_hex(r1); con_dbg("] ");
    if (st != 0) return (int)st;
    return (int)r1;                 /* 1 = TZ boots this pas-id */
}
int scm_pas_init_image(uint32_t pas_id, uint32_t mdt_phys)
{   return scm_pas_call(SCM_PIL_PAS_INIT_IMAGE, 0x82u, 2u, pas_id, mdt_phys, 0); }
int scm_pas_mem_setup(uint32_t pas_id, uint32_t base, uint32_t size)
{   return scm_pas_call(SCM_PIL_PAS_MEM_SETUP, 3u, 3u, pas_id, base, size); }
int scm_pas_auth_and_reset(uint32_t pas_id)
{   return scm_pas_call(SCM_PIL_PAS_AUTH_AND_RESET, 1u, 1u, pas_id, 0, 0); }
int scm_pas_shutdown(uint32_t pas_id)
{   return scm_pas_call(SCM_PIL_PAS_SHUTDOWN, 1u, 1u, pas_id, 0, 0); }

/* ---- TrustZone diagnostic log ------------------------------------------
 * After a PAS call fails, TZ has (usually) written a reason into its diag
 * ring buffer. PLAT_TZLOG_PTR is the tz-log@ address from the DT. Two eras:
 *   - Gen 6 (4.14 tz_log): the IMEM word holds the PHYSICAL ADDRESS of the
 *     tzdbg_t buffer (measured: 0x08600720 -> 0x866fb000).
 *   - msm8909w (3.18 tz_log): tzdbg_t sits AT the address itself.
 * tz_diag_base() tells them apart by the magic ('tzda' = 0x747a6461).
 * tzdbg_t layout (arch/arm/mach-msm/tz_log.c, both eras):
 *   0x00 magic 0x04 version 0x08 cpu_count 0x0C vmid_off 0x10 boot_off
 *   0x14 reset_off 0x18 int_off 0x1C ring_off 0x20 ring_len
 * ring buffer at diag+ring_off: {u16 wrap; u16 offset; u8 log_buf[]}.
 * Never read past PLAT_TZLOG_SIZE from the diag base: the mapping is exactly
 * that big and the next page reset the Gen 6 (seq v15). */
#if defined(PLAT_TZLOG_PTR)
#ifndef PLAT_TZLOG_SIZE
#define PLAT_TZLOG_SIZE 0x2000u
#endif
#define TZ_MAGIC 0x747a6461u
static uint32_t tzrd(uint32_t phys) { return *(volatile uint32_t *)(uintptr_t)phys; }

static uint32_t tz_diag_base(void)
{
    uint32_t w = mmio_read(PLAT_TZLOG_PTR);
    if (w == TZ_MAGIC) return PLAT_TZLOG_PTR;               /* header in place (8909w) */
    if ((w >= 0x08600000u && w < 0x08700000u) ||          /* pointer into IMEM ... */
        (w >= 0x80000000u && w < 0x90000000u))             /* ... or DDR (Gen 6)   */
        return w;
    return 0;
}

void tz_log_dump(void)
{
    uint32_t diag, magic, ver, ring_off, ring_len, i, run = 0;
    volatile uint8_t *p;

    diag = tz_diag_base();
    con_dbg("tz_log: @"); con_dbg_hex(PLAT_TZLOG_PTR); con_dbg(" word="); con_dbg_hex(mmio_read(PLAT_TZLOG_PTR));
    con_dbg(" -> diag "); con_dbg_hex(diag); con_dbg("\n"); con_flush(); usb_poll();
    blackbox_sync();   /* the pointer is on eMMC before we dereference it */

    if (!diag) { con_dbg("tz_log: no tzdbg header found, skipping\n"); con_flush(); return; }
    magic = tzrd(diag); ver = tzrd(diag + 4u);
    ring_off = tzrd(diag + 0x1Cu); ring_len = tzrd(diag + 0x20u);
    con_dbg("tz_log: magic="); con_dbg_hex(magic);
    con_dbg(" ver="); con_dbg_hex(ver);
    con_dbg(" ring_off="); con_dbg_hex(ring_off);
    con_dbg(" ring_len="); con_dbg_hex(ring_len); con_dbg("\n"); con_flush(); usb_poll();
    blackbox_sync();

    if (magic != TZ_MAGIC || ring_off > PLAT_TZLOG_SIZE || ring_len == 0u ||
        ring_off + ring_len > PLAT_TZLOG_SIZE) {
        con_puts("tz_log: ring geometry implausible; header hexdump:\n");
        for (i = 0; i < 0x40u; i += 4u) { con_dbg_hex(tzrd(diag + i)); con_dbg((i % 16u) == 12u ? "\n" : " "); }
        con_flush(); return;
    }
    /* ASCII dump of the ring (skip the 4-byte pos header). ring_len COUNTS
     * that header, so stop 4 bytes early to stay inside the mapping. */
    con_dbg("tz_log: ---- ring buffer ----\n");
    p = (volatile uint8_t *)(uintptr_t)(diag + ring_off + 4u);
    for (i = 0; i + 4u < ring_len; i++) {
        uint8_t c = p[i];
        if (c == '\n' || (c >= 0x20u && c < 0x7Fu)) { con_dbg_c((char)c); run++; }
        else if (run) { con_dbg_c('\n'); run = 0; }   /* collapse binary gaps to a newline */
        if ((i & 0x3FFu) == 0x3FFu) { con_flush(); usb_poll(); }
    }
    con_dbg("\ntz_log: ---- end ----\n"); con_flush(); usb_poll();
}

/* The last `n` bytes TZ wrote to its ring. Cheap enough to call after every
 * attempt so each one's TZ-side entries are seen next to its result. */
void tz_log_tail(uint32_t n)
{
    uint32_t diag = tz_diag_base(), ring_off, ring_len, pos, wrap, off, cap, k;
    volatile uint8_t *buf;

    if (!diag) return;
    ring_off = tzrd(diag + 0x1Cu); ring_len = tzrd(diag + 0x20u);
    if (tzrd(diag) != TZ_MAGIC || ring_off > PLAT_TZLOG_SIZE || ring_len < 8u ||
        ring_off + ring_len > PLAT_TZLOG_SIZE) return;
    pos  = tzrd(diag + ring_off);
    wrap = pos & 0xFFFFu; off = pos >> 16;
    buf  = (volatile uint8_t *)(uintptr_t)(diag + ring_off + 4u);
    cap  = ring_len - 4u;
    /* Do not trust the position word (v17 printed blanks): find the last
     * printable byte in the ring and show the text before it. */
    con_dbg("  tz tail (pos word "); con_dbg_hex(pos); con_dbg(" wrap="); con_dbg_hex(wrap);
    con_dbg(" off="); con_dbg_hex(off); con_dbg("): ");
    for (off = cap; off > 0u; off--) { uint8_t c = buf[off - 1u]; if (c >= 0x20u && c < 0x7Fu) break; }
    if (n > off) n = off;
    for (k = 0; k < n; k++) {
        uint8_t c = buf[off - n + k];
        con_dbg_c((c >= 0x20u && c < 0x7Fu) ? (char)c : ' ');
    }
    con_dbg("\n"); con_flush(); usb_poll(); blackbox_sync();
}

/* tzdbg boot/reset counters (2026-09-08). The rooted stock C2+ prints these
 * from /d/tzdbg/boot: per CPU {wb_entry, wb_exit, pc_entry, pc_exit,
 * warm_jmp_addr, spare} at diag+boot_off, and {reset_type, reset_cnt} at
 * diag+reset_off. Stock: cpu0 wb entry == exit (0x2453) and every reset
 * counter 0 across thousands of l2-pc wakes. After one of OUR failed
 * system-collapse wakes, this line at the next boot says whether TZ ever
 * started the warm boot (wb_entry advanced, wb_exit did not) or never got the
 * wake at all (neither moved). IMEM survives the PMIC reset (v158). */
void tz_boot_counters(const char *tag)
{
    uint32_t diag = tz_diag_base(), boot_off, reset_off, ncpu, c;
    if (!diag || tzrd(diag) != TZ_MAGIC) { con_puts("tzdbg["); con_puts(tag); con_puts("]: no header\n"); return; }
    ncpu = tzrd(diag + 0x08u); boot_off = tzrd(diag + 0x10u); reset_off = tzrd(diag + 0x14u);
    if (ncpu > 4u) ncpu = 4u;
    con_puts("tzdbg["); con_puts(tag); con_puts("] ver "); con_puthex(tzrd(diag + 4u));
    if (boot_off + ncpu * 24u <= PLAT_TZLOG_SIZE) {
        for (c = 0; c < ncpu; c++) {
            uint32_t b = diag + boot_off + c * 24u;
            con_puts(" cpu"); con_putdec(c); con_puts(": wb "); con_puthex(tzrd(b)); con_puts("/"); con_puthex(tzrd(b + 4u));
            con_puts(" pc "); con_puthex(tzrd(b + 8u)); con_puts("/"); con_puthex(tzrd(b + 12u));
            con_puts(" jmp "); con_puthex(tzrd(b + 16u));
        }
    } else con_puts(" boot_off implausible");
    if (reset_off + ncpu * 8u <= PLAT_TZLOG_SIZE) {
        con_puts(" | reset");
        for (c = 0; c < ncpu; c++) { uint32_t r = diag + reset_off + c * 8u; con_puts(" "); con_puthex(tzrd(r)); con_puts("x"); con_putdec(tzrd(r + 4u)); }
    }
    con_puts("\n"); con_flush(); usb_poll();
}
#else
void tz_log_dump(void) { con_dbg("tz_log: no PLAT_TZLOG_PTR on this board\n"); }
void tz_boot_counters(const char *tag) { (void)tag; }
void tz_log_tail(uint32_t n) { (void)n; }
#endif

void scm_diag(void)
{
    static const struct { const char *name; uint32_t svc, cmd; } calls[] = {
        { "info/is_call_avail", SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL },
        { "pil/pas_init_image",  SCM_SVC_PIL, SCM_PIL_PAS_INIT_IMAGE },
        { "pil/pas_mem_setup",   SCM_SVC_PIL, SCM_PIL_PAS_MEM_SETUP },
        { "pil/pas_auth_reset",  SCM_SVC_PIL, SCM_PIL_PAS_AUTH_AND_RESET },
        { "pil/pas_shutdown",    SCM_SVC_PIL, SCM_PIL_PAS_SHUTDOWN },
        { "pil/pas_is_supported",SCM_SVC_PIL, SCM_PIL_PAS_IS_SUPPORTED },
    };
    unsigned i;

    con_dbg("scm: probing TZ calling convention ...\n");
    con_flush(); usb_poll(); timer_delay_ms(50);
    scm_probe();
    con_dbg("scm: is_call_available sweep ("); con_dbg(scm_convention_name()); con_dbg("):\n");

    for (i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        int r = scm_is_call_available(calls[i].svc, calls[i].cmd);
        con_dbg("  "); con_dbg(calls[i].name); con_dbg(": ");
        if (r < 0) { con_puts("query failed, status "); con_putdec((uint32_t)-r); con_puts(" (negated)\n"); }
        else con_dbg(r ? "available\n" : "NOT available\n");
        con_flush(); usb_poll();
    }
}

#else  /* boards without SMEM (qemu): no TrustZone to talk to */

int scm_is_call_available(uint32_t svc, uint32_t cmd) { (void)svc; (void)cmd; return -1; }
int scm_mc_boot_available(void) { return 0; }
int scm_set_warmboot_addr_mc_all(uint32_t a) { (void)a; return -1; }
const char *scm_convention_name(void) { return "none"; }
void scm_diag(void) { con_dbg("scm: no TrustZone on this board\n"); }
void tz_log_dump(void) { con_dbg("tz_log: not available on this board\n"); }
void tz_log_tail(uint32_t n) { (void)n; }
int scm_pas_init_image(uint32_t a, uint32_t b) { (void)a; (void)b; return -1; }
int scm_pas_mem_setup(uint32_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; return -1; }
int scm_pas_auth_and_reset(uint32_t a) { (void)a; return -1; }
int scm_pas_shutdown(uint32_t a) { (void)a; return -1; }
int scm_set_warmboot_addr(uint32_t a) { (void)a; return -1; }
int scm_set_boot_addr(uint32_t a, uint32_t f) { (void)a; (void)f; return -1; }
int scm_terminate_pc(uint32_t a) { (void)a; return -1; }
int scm_pas_is_supported(uint32_t a) { (void)a; return -1; }

#endif /* PLAT_SMEM_BASE */
