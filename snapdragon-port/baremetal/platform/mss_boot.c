/* mss_boot.c — boot the REAL modem (MPSS) on the msm8909w watches, the way
 * the stock kernel does (drivers/soc/qcom/pil-msa.c + pil-q6v5.c, self-auth
 * MBA flow), so that its firmware idles into the SAW->RPM sleep handshake
 * and the RPM can finally enter vmin (stock <2 mA; ours ~6 mA because MPSS
 * is the last master the RPM sees as awake -- 2026-09-13 notes section 5).
 *
 * Stock order (pil_boot -> pil_msa_mss_ops_selfauth), reproduced 1:1:
 *   1. modem.mdt read; proxy votes (mx corner 3, cx corner 7, xo, l7 1.8 V)
 *   2. init_image = pil_mss_reset_load_mba + pil_msa_auth_modem_mdt:
 *        mba.mbn -> 1 MB buffer (plain DDR, 4 KB aligned)
 *        power up (restart_reg 0, clocks ahb/axi/rom), RMB_MBA_IMAGE = buffer
 *        RMB_PMI_CODE_START/LENGTH = 0, then the Q6v55 reset sequence
 *        PBL status == 1, MBA status == 1|2 (XPU unlocked)
 *        RMB_PMI_META_DATA = mdt copy, COMMAND = META_DATA_READY, status == 3
 *   3. segments: modem.bNN -> p_paddr (fixed, 0x88000000.. = modem_adsp_region),
 *        zero the tail, verify_blob: first one sets CODE_START + LOAD_READY,
 *        LENGTH accumulates memsz
 *   4. auth_and_reset = wait MBA status == 4 (AUTH_COMPLETE)
 * No PAS, no mem_setup, no hyp assign on this SoC (no qcom,pil-mss-memsetup,
 * no subsys_vmid). The modem's region is XPU-locked until the MBA reports
 * XPU_UNLOCKED, so nothing touches 0x88000000 before step 2 completes.
 *
 * Every step is announced and synced to the blackbox BEFORE it runs: an XPU
 * or NoC error is a silent TZ reset, and the last line then names the step.
 * -DMSS_BOOT, called once from sys_pc8909_init (the 20 s one-shot). */
#include "platform.h"
#if defined(PLAT_SOC_MSM8909) && defined(MSS_BOOT)
#include <string.h>

#define QDSP6SS_BASE     0x04080000u
#define QDSP6SS_RESET    (QDSP6SS_BASE + 0x014u)
#define QDSP6SS_GFMUX    (QDSP6SS_BASE + 0x020u)
#define QDSP6SS_PWR_CTL  (QDSP6SS_BASE + 0x030u)
#define QDSP6SS_XO_CBCR  (QDSP6SS_BASE + 0x038u)
#define Q6SS_STOP_CORE   (1u << 0)
#define Q6SS_CORE_ARES   (1u << 1)
#define Q6SS_BUS_ARES_ENA (1u << 2)
#define Q6SS_CLK_ENA     (1u << 1)
#define Q6SS_CLAMP_IO    (1u << 20)
#define Q6SS_CLAMP_WL    (1u << 21)
#define Q6SS_BHS_ON      (1u << 24)
#define Q6SS_LDO_BYP     (1u << 25)

#define RMB_BASE         0x04020000u
#define RMB_MBA_IMAGE    (RMB_BASE + 0x00u)
#define RMB_PBL_STATUS   (RMB_BASE + 0x04u)
#define RMB_MBA_COMMAND  (RMB_BASE + 0x08u)
#define RMB_MBA_STATUS   (RMB_BASE + 0x0Cu)
#define RMB_PMI_META     (RMB_BASE + 0x10u)
#define RMB_PMI_START    (RMB_BASE + 0x14u)
#define RMB_PMI_LENGTH   (RMB_BASE + 0x18u)
#define RMB_PROTO_VER    (RMB_BASE + 0x1Cu)
#define RMB_DEBUG_INFO   (RMB_BASE + 0x20u)
#define CMD_META_DATA_READY 1u
#define CMD_LOAD_READY      2u
#define ST_PBL_SUCCESS      1
#define ST_META_AUTH_OK     3
#define ST_AUTH_COMPLETE    4

#define MSS_RESTART_REG          0x0183e000u
#define GCC_MSS_CFG_AHB_CBCR     0x01849000u
#define GCC_MSS_Q6_BIMC_AXI_CBCR 0x01849004u
#define GCC_APCS_BRANCH_ENA_VOTE 0x01845004u
#define GCC_BOOT_ROM_AHB_CBCR    0x0181300cu

/* MBA gets 1 MB (stock: SZ_1M dma buffer, the MBA uses the rest as its own
 * RAM); the metadata copy must be 4 KB aligned. Both plain cached DDR, cleaned
 * before the Q6 reads them. */
/* v345: stock C2+ dmesg "Loading MBA and DP from 0xbec00000 to 0xbed00000" -- a 1 MB aligned buffer
 * (CMA order-8). Ours at 0x80436000 got PBL 0xef709ed0; align to 1 MB like stock. */
static uint8_t s_mba[1024u * 1024u] __attribute__((aligned(0x100000)));
static uint8_t s_mdt[16384u]        __attribute__((aligned(4096)));
static uint32_t s_mba_len, s_mdt_len;

struct elf32_phdr_m { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; };

/* v341: v340 reset with an empty blackbox -- every step now syncs, settles 300 ms
 * (eMMC commit) and syncs again, so the last persisted line is the killer. */
static uint32_t crc32_buf(const uint8_t *p, uint32_t n)
{ uint32_t c = 0xFFFFFFFFu; while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); } return ~c; }
/* v349: runs in its own FreeRTOS task (user: v345-v348 froze the whole OS for a minute and the
 * watchdog threw it to fastboot). No blocking: say() only queues console text, delays yield. */
#include "FreeRTOS.h"
#include "task.h"
void wdog_pet(void);

/* Per-board modem proxy rails (stock mss node: vdd_cx-supply / vdd_mx-supply / vdd_pll-supply).
 * PM8916 boards (C2/S2/Gen 4): cx = 8916_s1_corner (smpa 1), mx = 8916_l3_corner_ao (ldoa 3),
 * pll = l7 1.8 V (already on). Fossil Gen 5 (PM660) overrides these in boards/fossil_gen5.h:
 * cx = pm660_s2_corner (smpa 2), mx = pm660_s3_corner_ao (smpa 3), pll = pm660_l12 1.8 V. */
#ifndef PLAT_MSS_CX_TYPE
#define PLAT_MSS_CX_TYPE 0x61706d73u   /* "smpa" */
#define PLAT_MSS_CX_ID   1u
#endif
#ifndef PLAT_MSS_MX_TYPE
#define PLAT_MSS_MX_TYPE 0x616f646cu   /* "ldoa" */
#define PLAT_MSS_MX_ID   3u
#endif
#ifndef PLAT_MSS_MX_WIRE
#define PLAT_MSS_MX_WIRE 2u            /* stock vdd_mx-uV 3 (SVS_KRAIT) -> wire 2 */
#endif
#ifndef PLAT_MSS_CX_WIRE
#define PLAT_MSS_CX_WIRE 6u            /* stock vdd_cx-voltage 7 (SUPER_TURBO) -> wire 6 */
#endif
static void mss_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1u)); }
/* 2026-09-17: the monitor's 200 ms / 10 s sleeps, but serviced every 20 ms while mss_apr.c has an
 * APR command or the speaker tone in flight (a 100 ms PCM buffer cannot wait 10 s for its refill). */
static void mss_mon_sleep(uint32_t ms, struct smd_chan *apr, int apr_ok)
{
    while (ms) {
        uint32_t d = (apr_ok && mss_apr_busy()) ? 20u : ms;
        if (d > ms) d = ms;
        mss_delay_ms(d); ms -= d;
        if (apr_ok && mss_apr_busy()) mss_apr_poll(apr);
    }
}
static void say(const char *s) { con_puts(s); }
static void hex(const char *s, uint32_t v) { con_puts(s); con_puthex(v); }
static void dec(const char *s, uint32_t v) { con_puts(s); con_putdec(v); }
static void cache_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    for (; a < end; a += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static void sink_buf(void *ctx, const uint8_t *p, uint32_t off, uint32_t n)
{   memcpy((uint8_t *)ctx + off, p, n); }

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { crc ^= p[i]; for (unsigned b = 0; b < 8u; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    return crc;
}
static volatile int s_first_chunk;
static uint32_t s_stream_crc;
static uint8_t s_stage[13u << 20] __attribute__((aligned(64)));   /* v430: whole modem image staged in RAM (Gen 5: 12.2 MB) */
static uint32_t s_stage_off[64];
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);
static void sink_dev(void *ctx, const uint8_t *p, uint32_t off, uint32_t n)
{
    if (s_first_chunk) { s_first_chunk = 0; dec("mss-boot:   first chunk ", n); hex(" B -> ", (uint32_t)(uintptr_t)ctx + off); say("\n"); con_flush(); }
    if (off && !(off & 0x7FFFFu) ) { dec("mss-boot:   ... ", off >> 10); dec(" KB at +", timer_ms()); say(" ms\n"); }   /* v417: 512 KB heartbeat */
    /* v419: the Gen 5 died at random points inside the long copies = watchdog: this task never
     * yielded during a copy, and the gate loop (the only thing petting the dog) starved. Pet and
     * yield every 64 KB. The C2 only survived because its whole load fit in the dog's window. */
    if (!(off & 0xFFFFu)) { wdog_pet(); vTaskDelay(1); }
    /* v427: EVERY Gen 5 load died 13.4 s after CMD_LOAD_READY = the copy time of seg 12, during
     * which RMB_PMI_LENGTH never moved (we only wrote it after each whole segment; the C2's faster
     * eMMC never left the MBA waiting that long). Nothing in the PMIC recorded the reset = SoC
     * internal. Report progress to the MBA every 64 KB so it never sees a stalled load. */
    /* v428: v427 proved a PMI_LENGTH write the MBA cannot verify RESETS THE SOC on the Gen 5 (the C2
     * returns status -19 instead) -> every earlier Gen 5 death = the MBA rejecting seg 12's hash.
     * No progressive writes; instead CRC what streams off the eMMC (here) and what is read back
     * from the target (after the copy) against the file CRCs from the partition image. */
    s_stream_crc = crc32_update(s_stream_crc, p, n);

    volatile uint32_t *dst = (volatile uint32_t *)((uintptr_t)ctx + off);
    uint32_t i, w = (n + 3u) / 4u;
    for (i = 0; i < w; i++) { uint32_t v; memcpy(&v, p + 4u * i, 4); dst[i] = v; }
    taskYIELD();
}
static void rmb_dump(const char *tag)
{
    hex("mss-boot: rmb[", 0); con_puts(tag); hex("] image ", mmio_read(RMB_MBA_IMAGE)); hex(" pbl ", mmio_read(RMB_PBL_STATUS));
    hex(" cmd ", mmio_read(RMB_MBA_COMMAND)); hex(" status ", mmio_read(RMB_MBA_STATUS)); hex(" meta ", mmio_read(RMB_PMI_META));
    hex(" start ", mmio_read(RMB_PMI_START)); hex(" len ", mmio_read(RMB_PMI_LENGTH)); hex(" ver ", mmio_read(RMB_PROTO_VER));
    hex(" dbg ", mmio_read(RMB_DEBUG_INFO)); say("\n");
}
static void q6_dump(const char *tag)
{
    con_puts("mss-boot: q6["); con_puts(tag); hex("] restart ", mmio_read(MSS_RESTART_REG)); hex(" reset ", mmio_read(QDSP6SS_RESET));
    hex(" gfmux ", mmio_read(QDSP6SS_GFMUX)); hex(" pwr ", mmio_read(QDSP6SS_PWR_CTL)); hex(" xo ", mmio_read(QDSP6SS_XO_CBCR)); say("\n");
}
/* poll a status register: until pred(status) or timeout; returns status */
static int32_t poll_status(uint32_t reg, int want_nonzero, int32_t want, uint32_t ms)
{
    uint32_t t0 = timer_ms(); int32_t st;
    for (;;) {
        st = (int32_t)mmio_read(reg);
        if (want_nonzero ? (st != 0) : (st == want || st < 0)) return st;
        if ((uint32_t)(timer_ms() - t0) > ms) return st;
        mss_delay_ms(1u);
    }
}
static int cbcr_on(uint32_t reg)
{
    uint32_t v = mmio_read(reg);
    if (!(v & 1u)) mmio_write(reg, v | 1u);
    for (unsigned i = 0; i < 200u; i++) { if (!(mmio_read(reg) & 0x80000000u)) return 0; timer_delay_us(10u); }
    return -1;
}

static int q6v55_reset(void)
{
    uint32_t v;
    /* assert resets, stop core */
    v = mmio_read(QDSP6SS_RESET) | Q6SS_CORE_ARES | Q6SS_BUS_ARES_ENA | Q6SS_STOP_CORE;
    mmio_write(QDSP6SS_RESET, v);
    /* BHS require xo cbcr to be enabled */
    if (cbcr_on(QDSP6SS_XO_CBCR) < 0) { say("mss-boot: Q6 XO branch did not start\n"); return -1; }
    v = mmio_read(QDSP6SS_PWR_CTL) | Q6SS_BHS_ON;
    mmio_write(QDSP6SS_PWR_CTL, v); __asm__ volatile("dsb sy" ::: "memory"); timer_delay_us(1u);
    v |= Q6SS_LDO_BYP; mmio_write(QDSP6SS_PWR_CTL, v);
    /* plain q6v55 (no v56/v61/v62 flags on this DT): turn on memories, L2 banks one at a time */
    v = mmio_read(QDSP6SS_PWR_CTL) | 0xFFF00u; mmio_write(QDSP6SS_PWR_CTL, v);
    for (unsigned i = 0; i <= 7u; i++) { v |= (1u << i); mmio_write(QDSP6SS_PWR_CTL, v); }
    /* remove word line clamp, then IO clamp */
    v = mmio_read(QDSP6SS_PWR_CTL) & ~Q6SS_CLAMP_WL; mmio_write(QDSP6SS_PWR_CTL, v);
    v &= ~Q6SS_CLAMP_IO; mmio_write(QDSP6SS_PWR_CTL, v);
    /* bring core out of reset, turn on core clock */
    v = mmio_read(QDSP6SS_RESET) & ~(Q6SS_CORE_ARES | Q6SS_STOP_CORE); mmio_write(QDSP6SS_RESET, v);
    v = mmio_read(QDSP6SS_GFMUX) | Q6SS_CLK_ENA; mmio_write(QDSP6SS_GFMUX, v);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

/* Is OpenWatchFace flashed in `boot`? (user rule 2026-09-13: the modem may write its EFS
 * partitions only while OWF is the flashed OS -- any OWF build, so a `fastboot boot` test image
 * on an OWF-flashed watch may write too; a RAM boot over STOCK boot must not.)
 * Marker: every OWF boot.img is packed with cmdline "owf_baremetal=1" (tools/mk-bootimg*.sh);
 * Android v0 header: magic "ANDROID!" @0, cmdline @64 (512 B). 1 = OWF flashed, 0 = not,
 * <0 = could not check (treated as not flashed). */

/* v361: SMSM (drivers/soc/qcom/smd.c). Stock apps SMSM state on the C2+ is 0x29 = INIT|SMDINIT|RPCINIT
 * (entry 0 of SMEM item 23, 8 x u32; modem = entry 1, stock 0x08000009). The kernel mirrors the modem:
 * modem INIT -> apps INIT, modem SMDINIT -> apps SMDINIT (smsm_irq_handler), RPCINIT when the IPCRTR
 * channel opens (ipc_router_smd_xprt.c:588). Our firmware never touched SMSM -> the modem's init waits
 * on the apps handshake and dies with dog.c:1522. Doorbell to the modem: 0x0b011008 bit 13 (DT
 * qcom,smsm-modem irq-bitmask 0x2000). Item 63 = per-entry/host interrupt masks (stock 5 hosts). */
#define SMSM_INIT_BIT 0x1u
#define SMSM_SMDINIT_BIT 0x8u
#define SMSM_RPCINIT_BIT 0x20u
static volatile uint32_t *smsm_state(void)
{
    /* v368: real ids from the compiled kernel enum: SMSM_SHARED_STATE = 85 (NOT 23 = an SMD channel
     * slot), SMSM_CPU_INTR_MASK = 333, SMSM_SIZE_INFO = 419, VERSION_INFO = 3. All exist from the SBL. */
    uint32_t sz = 0; volatile uint32_t *st = (volatile uint32_t *)smem_get(85u, &sz);
    return (st && sz >= 32u) ? st : 0;
}
static int smsm_apps_set(uint32_t bits)
{
    volatile uint32_t *st = smsm_state(); if (!st) return -1;
    uint32_t old = st[0]; if ((old | bits) == old) return 0;
    st[0] = old | bits; __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(0x0b011008u, 1u << 13);
    hex("mss-smsm: apps ", old); hex(" -> ", old | bits); hex(" (modem ", st[1]); say(")\n");
    return 1;
}

/* v366: announce every APPS-hosted server the stock C2+ shows (dump_servers, node 1) so the first
 * DATA the modem sends names the service its init waits on. Port = 0x4000 + index. */
static const uint32_t s_stock_srv[][2] = {
    { 14u, 1u },            /* rmt_storage (daemon)                 port 0x4000 */
    { 0x1001u, 0u }, { 0x1001u, 2u }, { 0x1001u, 4u },            /* diag modem cntl/data/dci 0x4001.. */
    { 0x1001u, 0x40u }, { 0x1001u, 0x42u }, { 0x1001u, 0x44u },   /* diag lpass */
    { 0x1001u, 0x80u }, { 0x1001u, 0x82u }, { 0x1001u, 0x84u },   /* diag wcnss */
    { 0x1001u, 0xc0u }, { 0x1001u, 0xc2u }, { 0x1001u, 0xc4u },   /* diag sensors 0x400a..0x400c */
    { 0x34u, 0x101u },      /* memshare (kernel)                    0x400d */
    { 0x10fu, 2u },         /* ?                                    0x400e */
    { 0x35u, 0x1001u },     /* ?                                    0x400f */
    { 0x118u, 0x3202u },    /* ?                                    0x4010 */
    { 0x1cu, 0x101u },      /* ?                                    0x4011 */
};
static int rtr_announce_all(struct smd_chan *c)
{
    int bad = 0;
    for (uint32_t i = 0; i < sizeof s_stock_srv / sizeof s_stock_srv[0]; i++) {
        /* v388: the kernel (ipc_router_send_ctl_msg) addresses every control message except HELLO to
         * xprt_info->remote_node_id, i.e. dst node 0 for the modem link; only HELLO carries the
         * broadcast node 0xffffffff (remote_node_id is still -1 at open). v355..v387 sent NEW_SERVER
         * with the broadcast node and the modem never looked our servers up (no rmtfs OPEN, only
         * SSCTL announced vs ~40 services on stock). Match the kernel exactly. */
        uint32_t nsrv[13] = { 1u, 4u, 1u, 0xfffffffeu, 0u, 20u, 0u, 0xfffffffeu,
                              4u, s_stock_srv[i][0], s_stock_srv[i][1], 1u, 0x4000u + i };
        if (smd_send(c, nsrv, sizeof nsrv) < 0) bad++;
        taskYIELD();
    }
    return bad;
}
static volatile int s_mss_ready, s_mss_loading, s_mss_task_done;
static volatile uint32_t s_mss_release_ms;   /* v472: timer_ms() when the modem was released (0 = not yet) */
uint32_t mss_release_ms(void) { return s_mss_release_ms; }
/* Set once the modem has been told the BG is up (the SSCTL "bg-wear" AFTER_POWERUP event below).
 * From that moment the modem's own BG SPI driver owns the QUP4 pads and the AP must not touch
 * them. This is NOT conditional on MSS_BG_AP_RELEASE: that event is sent on every MSS_BOOT build,
 * so the handover happens whether or not the flag is set, and the flag only controls whether we
 * ALSO hand the pads over early and skip the AP-side codec test. Scoping this variable to the
 * flag is what made gen5-modem-19..24 hang: with it compiled out, crown_poll()'s guard vanished,
 * the app's first frame called bgcom_bus_take() on a bus the modem was driving, and the watch
 * died a second after "app starts (modem ready)" -- taking the modem monitor task, and every
 * further audio WRITE_V2, with it. */
volatile int g_bgcom_ap_released;
int mss_ready(void) { return s_mss_ready; }
int mss_task_done(void) { return s_mss_task_done; }
int mss_loading(void) { return s_mss_loading; }
int wcnss_boot_busy(void);
/* v393: heartbeat for the main loop's LOOP line -- the v392 modem-task log stopped at +8 s while
 * the watch kept running; iteration count + the last step entered tells stuck from silent. */
static volatile uint32_t s_mss_iter; static const char *volatile s_mss_where = "not started";
uint32_t mss_iter(void) { return s_mss_iter; }
const char *mss_where(void) { return s_mss_where; }
#define WHERE(x) (s_mss_where = (x))
static uint8_t s_bootblk[512u];
int owf_is_flashed(void)
{
    uint32_t lba, nblk;
    if (emmc_gpt_find("boot", &lba, &nblk) < 0) { say("flash-check: no `boot` partition\n"); return -1; }
    if (emmc_read(lba, 1u, s_bootblk) < 0) { say("flash-check: boot header read failed\n"); return -2; }
    if (memcmp(s_bootblk, "ANDROID!", 8)) { say("flash-check: boot partition has no Android header -> not OWF\n"); return 0; }
    char cmd[65]; memcpy(cmd, s_bootblk + 64, 64); cmd[64] = 0;
    for (unsigned k = 0; k < 64u && cmd[k]; k++) if (cmd[k] < 32 || cmd[k] > 126) cmd[k] = '.';
    con_puts("flash-check: boot cmdline \""); con_puts(cmd); say("\"\n");
    int owf = strstr((const char *)s_bootblk + 64, "owf_baremetal=1") != 0 && memchr(s_bootblk + 64, 0, 512u - 64u) != 0;
    say(owf ? "flash-check: OpenWatchFace is flashed in boot\n" : "flash-check: boot holds another OS (stock?)\n");
    return owf;
}

int mss_boot(void)
{
    static int s_done;
    if (s_done) return 0;
    s_done = 1;
    say("mss-boot: === modem boot (stock pil-msa self-auth flow) ===\n");
    say("mss-boot: running in task mss-boot\n");
    { int f = owf_is_flashed(); say(f == 1 ? "mss-boot: EFS writes ALLOWED (OWF flashed)\n" : "mss-boot: EFS writes BLOCKED (OWF not flashed / unverified)\n"); }
    { uint32_t sz = 0; const char *r = (const char *)smem_get(421u, &sz);
      dec("mss-boot: smem ssr-reason at boot sz ", sz);
      if (r && sz && r[0]) { char b[96]; uint32_t k; for (k = 0; k < sz && k < 95u && r[k]; k++) b[k] = (r[k] >= 32 && r[k] < 127) ? r[k] : '.'; b[k] = 0; con_puts(" \""); con_puts(b); con_puts("\""); }
      say("\n"); }
    /* v395: never load while the WCNSS loader runs (and give the app 8 s to start WiFi first) */
    { uint32_t w0 = timer_ms(); int waited = 0;
      while (timer_ms() < 8000u || (wcnss_boot_busy() && (uint32_t)(timer_ms() - w0) < 120000u)) { waited = 1; mss_delay_ms(100u); }
      if (waited) { dec("mss-boot: waited for the WCNSS loader / app start, now +", timer_ms()); say(" ms\n"); } }
#if defined(PLAT_HAS_BG_QCC1110) && defined(HAVE_BG_FW) && !defined(PLAT_NO_BG_COPROC)
    /* v470: stock order is BG (pil-bg, bg2ap-status high) -> modem -> everything else. The BG task
     * is started by the loading gate (arduino_main.cpp); wait for its status line, 90 s cap. */
    { uint32_t b0 = timer_ms();
      while (!tlmm_in(PLAT_BG2AP_STATUS_GPIO) && (uint32_t)(timer_ms() - b0) < 90000u) mss_delay_ms(250u);
      dec("mss-boot: bg2ap-status ", (uint32_t)tlmm_in(PLAT_BG2AP_STATUS_GPIO)); dec(" at +", timer_ms());
      say(" ms (BG before the modem, as on stock)\n"); }
#if defined(MSS_BG_AP_RELEASE)
    /* v471: wait for the AP's own link-up + crown handshake, then give the QUP4 pads to the modem */
    { extern volatile int g_bgcom_ap_bringup_done; extern void bg_bus_release_to_modem(void);
      uint32_t b1 = timer_ms();
      while (!g_bgcom_ap_bringup_done && (uint32_t)(timer_ms() - b1) < 60000u) mss_delay_ms(250u);
      dec("mss-boot: AP bgcom bring-up done ", (uint32_t)g_bgcom_ap_bringup_done); dec(" at +", timer_ms()); say(" ms\n");
      bg_bus_release_to_modem(); }
#endif
#endif
    s_mss_loading = 1;
    rpm_master_stats_line("mss-boot before"); say("mss-boot: reading MODEM.MDT ...\n");

    /* 1. firmware files: modem.mdt + mba.mbn from the modem FAT partition */
    if (wcnss_fat_read_file("MODEM   MDT", &s_mdt_len, sink_buf, s_mdt) < 0 || s_mdt_len > sizeof s_mdt) { say("mss-boot: cannot read MODEM.MDT\n"); return -1; }
    if (wcnss_fat_read_file("MBA     MBN", &s_mba_len, sink_buf, s_mba) < 0 || s_mba_len > sizeof s_mba) { say("mss-boot: cannot read MBA.MBN\n"); return -1; }
    if (s_mdt[0] != 0x7Fu || s_mdt[1] != 'E') { say("mss-boot: MDT is not ELF\n"); return -1; }
    uint32_t phoff = *(uint32_t *)(s_mdt + 28), phnum = *(uint16_t *)(s_mdt + 44), entry = *(uint32_t *)(s_mdt + 24);
    const struct elf32_phdr_m *ph = (const struct elf32_phdr_m *)(s_mdt + phoff);
    dec("mss-boot: mdt ", s_mdt_len); dec(" B phnum ", phnum); hex(" entry ", entry); dec(", mba ", s_mba_len); hex(" B at ", (uint32_t)(uintptr_t)s_mba); say("\n");
    /* v344: stock C2+ /firmware/image/mba.mbn = 226176 B crc32 0x005e17fe (pulled over adb) */
    /* per-watch stock MBA crc32 from the modem partition backups (2026-09-15): C2 0x005e17fe,
     * Gen 5 0xccd3d6da, darter 0xaf1f7714 -- the old line compared every board against the C2's */
    { uint32_t c = crc32_buf(s_mba, s_mba_len);
      int known = (c == 0x005e17feu || c == 0xccd3d6dau || c == 0xaf1f7714u);
      hex("mss-boot: mba crc32 ", c); say(known ? " (a known stock MBA)\n" : " (not one of the backed-up stock MBAs)\n"); }
    if (phnum > 64u) { say("mss-boot: phnum out of range\n"); return -1; }
    cache_clean(s_mba, sizeof s_mba); cache_clean(s_mdt, sizeof s_mdt);

    /* 2a. power-up: restart_reg 0 (already 0 at boot on the C2), clocks */
    /* v430: STAGE EVERY SEGMENT FILE IN RAM BEFORE THE Q6 IS RELEASED. The MBA waits on the AP with
     * its own watchdog running and its error path resets the whole SoC (Gen 5 v416..v429: reset
     * ~13.5 s after CMD_LOAD_READY whatever the segment, no fault, no PMIC record, MBA status 3
     * throughout). This eMMC streams at ~600 KB/s, seg 12 alone takes 13 s; the C2 only ever fit
     * inside the window. From RAM the whole image lands in the region in a fraction of a second. */
    {
        uint32_t off = 0, t0 = timer_ms(); char name[12]; memcpy(name, "MODEM   B00", 12);
        for (uint32_t i = 0; i < phnum; i++) {
            const struct elf32_phdr_m *p = &ph[i];
            s_stage_off[i] = 0xFFFFFFFFu;
            if (p->p_type != 1u || (p->p_flags & (7u << 24)) == (2u << 24) || p->p_memsz == 0u || p->p_filesz == 0u) continue;
            if (off + p->p_filesz > sizeof s_stage) { hex("mss-boot: staging buffer too small for seg ", i); say("\n"); return -1; }
            name[9] = (char)('0' + i / 10u); name[10] = (char)('0' + i % 10u);
            uint32_t fsz = 0;
            if (wcnss_fat_read_file(name, &fsz, sink_buf, s_stage + off) < 0 || fsz != p->p_filesz) { dec("mss-boot: staging read failed for seg ", i); say("\n"); return -1; }
            s_stage_off[i] = off; off += (p->p_filesz + 63u) & ~63u;
            dec("mss-boot: staged seg ", i); dec(" (", p->p_filesz); dec(" B) at +", timer_ms()); say(" ms\n"); wdog_pet();
        }
        dec("mss-boot: all segments staged: ", off); dec(" B in ", timer_ms() - t0); say(" ms\n");
    }
    say("mss-boot: restart_reg <- 0, clocks (mss_cfg_ahb, q6_bimc_axi, boot_rom_ahb) ...\n");
    mmio_write(MSS_RESTART_REG, 0u); __asm__ volatile("dsb sy" ::: "memory"); timer_delay_us(2u);
    if (cbcr_on(GCC_MSS_CFG_AHB_CBCR) < 0 || cbcr_on(GCC_MSS_Q6_BIMC_AXI_CBCR) < 0) { say("mss-boot: MSS clocks did not start\n"); return -1; }
    mmio_write(GCC_APCS_BRANCH_ENA_VOTE, mmio_read(GCC_APCS_BRANCH_ENA_VOTE) | (1u << 7));
    hex("mss-boot: cfg_ahb ", mmio_read(GCC_MSS_CFG_AHB_CBCR)); hex(" q6_axi ", mmio_read(GCC_MSS_Q6_BIMC_AXI_CBCR)); hex(" boot_rom ", mmio_read(GCC_BOOT_ROM_AHB_CBCR)); say("\n");

    /* 2b. RMB: first touch of 0x04020000 */
    /* v342: QDSP6SS reads BEFORE mss_cfg_ahb was on = unclocked MSS bus -> reset (v340 died here) */
    q6_dump("clocks on");
    say("mss-boot: reading RMB 0x04020000 ...\n");
    rmb_dump("idle");
    mmio_write(RMB_MBA_IMAGE, (uint32_t)(uintptr_t)s_mba);
    mmio_write(RMB_PMI_START, 0u);
    mmio_write(RMB_PMI_LENGTH, 0u);
    __asm__ volatile("dsb sy" ::: "memory");
    rmb_dump("programmed");

    /* 2c. Q6 out of reset -> PBL -> MBA */
#if defined(MSS_PROXY_VOTES)
    /* v348: stock pil_q6v5_make_proxy_votes before the reset: vdd_cx (8916_s1_corner) corner 7,
     * vdd_mx (8916_l3_corner_ao) corner 3, vdd_pll (l7) is already on (USB). Each vote logged
     * first -- these resets the C2 in the Pronto work, so the last line names the rail. */
    {   /* v359: rpm-smd-regulator.c sends corner - RPM_REGULATOR_CORNER_NONE(1), "corn" max 6.
         * v358 sent the raw DT values (cx 7 = out of range) and the SoC reset. Stock: mx
         * vdd_mx-uV 3 (SVS_KRAIT) -> 2, cx vdd_cx-voltage 7 (SUPER_TURBO) -> 6. */
        uint32_t kv[3] = { 0x6e726f63u /* corn */, 4u, PLAT_MSS_MX_WIRE };
        dec("mss-boot: vote mx (board rail) corn ", PLAT_MSS_MX_WIRE); say(" ...\n");
        int rc = rpm_smd_request(0, PLAT_MSS_MX_TYPE, PLAT_MSS_MX_ID, kv, sizeof kv);
        dec("mss-boot: mx vote rc ", (uint32_t)(rc < 0 ? -rc : rc)); say("\n");
        kv[2] = PLAT_MSS_CX_WIRE;
        dec("mss-boot: vote cx (board rail) corn ", PLAT_MSS_CX_WIRE); say(" ...\n");
        rc = rpm_smd_request(0, PLAT_MSS_CX_TYPE, PLAT_MSS_CX_ID, kv, sizeof kv);
#if defined(PLAT_MSS_PLL_LDO)
        { uint32_t pv[9] = { 0x6e657773u /* swen */, 4u, 1u, 0x00007675u /* uv */, 4u, PLAT_MSS_PLL_UV, 0x0000616du /* ma */, 4u, 10u };
          int rp = rpm_smd_request(0, 0x616f646cu, PLAT_MSS_PLL_LDO, pv, sizeof pv); dec("mss-boot: pll ldo vote rc ", (uint32_t)(rp < 0 ? -rp : rp)); say("\n"); }
#endif
        dec("mss-boot: cx vote rc ", (uint32_t)(rc < 0 ? -rc : rc)); say("\n");
    }
#endif
    /* v356: our half of smp2p (drivers/soc/qcom/smp2p.c): APPS->modem item 428 (SMEM_SMP2P_APPS_BASE 427 + modem pid 1) in the host-1
     * partition, header like the modem's own item 435 mirrored (local 0, remote 1), entries
     * "master-kernel" and "smp2p" = 0 as on stock. Doorbell 0x0b011008 bit 14 (DT smp2p-modem). */
    {
        volatile uint32_t *it = (volatile uint32_t *)smem_alloc_host(1u, 428u, 20u + 16u * 20u);
        if (!it) say("mss-smp2p: item 428 alloc FAILED\n");
        else {
            static const char names[2][16] = { "master-kernel", "smp2p" };
            for (unsigned k = 0; k < 2u; k++) {
                volatile uint8_t *en = (volatile uint8_t *)it + 20u + k * 20u;
                for (unsigned c = 0; c < 16u; c++) en[c] = (uint8_t)names[k][c];
                ((volatile uint32_t *)(void *)(en + 16))[0] = 0u;
            }
            it[1] = 0x00000101u;            /* version 1, features SSR_ACK */
            it[2] = 0x00010000u;            /* local pid 0 (apps), remote pid 1 (modem) */
            it[3] = 0x00020010u;            /* 2 valid of 16 */
            it[4] = 0u;
            it[0] = 0x504D5324u;            /* "$SMP" last: the item is complete */
            mmio_write(0x0b011008u, 1u << 14);
            hex("mss-smp2p: item 428 at ", (uint32_t)(uintptr_t)it); say(" created, doorbell rung\n");
        }
    }
    {   /* v368: SMSM items (85 state / 333 mask / 419 size-info / 3 version table) all pre-exist. Print
         * them, zero the apps entry, set our (host 0) mask for the modem entry to 0x49 like smsm_init. */
        uint32_t sz = 0; const uint32_t *v = (const uint32_t *)smem_get(3u, &sz);
        say("mss-smsm: item 3 versions"); if (v) for (uint32_t k = 0; k < sz / 4u && k < 32u; k++) { if (v[k]) { dec(" [", k); hex("]=", v[k]); } } say("\n");
        const uint32_t *si = (const uint32_t *)smem_get(419u, &sz);
        say("mss-smsm: item 419 size-info"); if (si) for (uint32_t k = 0; k < sz / 4u && k < 4u; k++) hex(" ", si[k]); say("\n");
        volatile uint32_t *st = smsm_state(); uint32_t sz333 = 0; volatile uint32_t *mk = (volatile uint32_t *)smem_get(333u, &sz333);
        say("mss-smsm: item 85 state"); if (st) for (unsigned k = 0; k < 8u; k++) hex(" ", st[k]); say("\n");
        hex("mss-smsm: item 333 sz ", sz333); if (mk) { uint32_t h = sz333 / 32u; hex(" hosts ", h); say(" masks"); for (uint32_t k = 0; k < sz333 / 4u && k < 40u; k++) hex(" ", mk[k]); mk[1u * h + 0u] = 0x49u; }
        say("\n");
        if (st) st[0] = 0u;
    }
    /* v374: the stock kernel holds these ACTIVE-set votes while awake; we never did, and the modem's
     * init-time votes covered for us until its init completes (v372/v373: total SoC stall the first
     * time the modem got further). cx smpa1 / mx ldoa3 corner NORMAL (5 -> wire 4), bus clocks
     * pcnoc/snoc (type "clk1" ids 0/1) 100 MHz and bimc ("clk2" id 0) 400 MHz key "KHz", plus the
     * apps GPLL0 vote bit (GCC_APCS_GPLL_ENA_VOTE 0x45000 bit 0). */
    {
        uint32_t kv[3] = { 0x6e726f63u /* corn */, 4u, 4u };
        int r1 = rpm_smd_request(0, PLAT_MSS_CX_TYPE, PLAT_MSS_CX_ID, kv, sizeof kv);
        int r2 = rpm_smd_request(0, PLAT_MSS_MX_TYPE, PLAT_MSS_MX_ID, kv, sizeof kv);
        uint32_t ck[3] = { 0x007a484bu /* KHz */, 4u, 100000u };
        int r3 = rpm_smd_request(0, 0x316b6c63u /* clk1 */, 0u, ck, sizeof ck);
        int r4 = rpm_smd_request(0, 0x316b6c63u /* clk1 */, 1u, ck, sizeof ck);
        ck[2] = 400000u;
        int r5 = rpm_smd_request(0, 0x326b6c63u /* clk2 */, 0u, ck, sizeof ck);
        dec("mss-boot: active votes cx/mx corn 4 rc ", (uint32_t)((r1 | r2) < 0)); dec(" pcnoc/snoc 100 MHz rc ", (uint32_t)((r3 | r4) < 0)); dec(" bimc 400 MHz rc ", (uint32_t)(r5 < 0)); say("\n");
        uint32_t g = mmio_read(0x01800000u + 0x45000u), b = mmio_read(0x01800000u + 0x45004u);
        hex("mss-boot: apcs gpll vote ", g); hex(" branch vote ", b);
        if (!(g & 1u)) { mmio_write(0x01800000u + 0x45000u, g | 1u); hex(" -> gpll0 voted ", mmio_read(0x01800000u + 0x45000u)); }
        say("\n");
    }
    smem_dump_items("pre-reset");
    say("mss-boot: q6v55 reset sequence ...\n");
    if (q6v55_reset() < 0) return -1;
    /* v343: v342 saw PBL status 0xef709ed0 (not a small error code). Trace every change
     * of PBL/MBA/DEBUG for 5 s, captured in RAM first (no console in the loop). */
    {
        uint32_t tv[24][4], n = 0, lp = 0xFFFFFFFFu, lm = 0xFFFFFFFFu, ld = 0xFFFFFFFFu;
        for (uint32_t t = 0; t < 1000u && n < 24u; t++) {
            uint32_t pb = mmio_read(RMB_PBL_STATUS), mb = mmio_read(RMB_MBA_STATUS), db = mmio_read(RMB_DEBUG_INFO);
            if (pb != lp || mb != lm || db != ld) { tv[n][0] = t * 5u; tv[n][1] = pb; tv[n][2] = mb; tv[n][3] = db; n++; lp = pb; lm = mb; ld = db; }
            mss_delay_ms(5u);
        }
        for (uint32_t i = 0; i < n; i++) { dec("mss-boot: trace +", tv[i][0]); hex(" ms pbl ", tv[i][1]); hex(" mba ", tv[i][2]); hex(" dbg ", tv[i][3]); say("\n"); }
    }
    q6_dump("released");
    {
        int32_t st = poll_status(RMB_PBL_STATUS, 1, 0, 1000u);
        hex("mss-boot: PBL status ", (uint32_t)st); say(st == ST_PBL_SUCCESS ? " (success)\n" : " (NOT success)\n");
        if (st != ST_PBL_SUCCESS) { rmb_dump("pbl-fail"); return -1; }
        st = poll_status(RMB_MBA_STATUS, 1, 0, 1000u);
        hex("mss-boot: MBA status ", (uint32_t)st); say((st == 1 || st == 2) ? " (XPU unlocked)\n" : " (unexpected)\n");
        if (st != 1 && st != 2) { rmb_dump("mba-fail"); return -1; }
    }
    rmb_dump("mba-up");

    /* 2d. metadata auth */
    say("mss-boot: metadata auth ...\n");
    mmio_write(RMB_PMI_LENGTH, 0u);
    mmio_write(RMB_PMI_META, (uint32_t)(uintptr_t)s_mdt);
    mmio_write(RMB_MBA_COMMAND, CMD_META_DATA_READY);
    __asm__ volatile("dsb sy" ::: "memory");
    {
        int32_t st = poll_status(RMB_MBA_STATUS, 0, ST_META_AUTH_OK, 10000u);
        hex("mss-boot: meta status ", (uint32_t)st); say(st == ST_META_AUTH_OK ? " (auth ok)\n" : " (FAIL)\n");
        if (st != ST_META_AUTH_OK) { rmb_dump("meta-fail"); return -1; }
    }

    /* 3. segments into the modem region (XPU now unlocked by the MBA) */
    {
        char name[12]; memcpy(name, "MODEM   B00", 12);
        uint32_t total = 0, first = 1;
        for (uint32_t i = 0; i < phnum; i++) {
            const struct elf32_phdr_m *p = &ph[i];
            if (p->p_type != 1u || (p->p_flags & (7u << 24)) == (2u << 24) || p->p_memsz == 0u) continue;
            name[9] = (char)('0' + i / 10u); name[10] = (char)('0' + i % 10u);
            dec("mss-boot: seg ", i); hex(" -> ", p->p_paddr); hex(" filesz ", p->p_filesz); hex(" memsz ", p->p_memsz); dec(" at +", timer_ms()); say(" ms ...\n");
            wdog_pet();
            uint32_t seg_t0 = timer_ms();
            s_stream_crc = 0xFFFFFFFFu;
            if (p->p_filesz) {
                if (s_stage_off[i] == 0xFFFFFFFFu) { dec("mss-boot: seg not staged ", i); say("\n"); return -1; }
                const uint8_t *src = s_stage + s_stage_off[i];
                volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)p->p_paddr; const uint32_t *sw = (const uint32_t *)src;
                uint32_t nw = (p->p_filesz + 3u) / 4u;
                for (uint32_t k = 0; k < nw; k++) { d[k] = sw[k]; if (!(k & 0x3FFFFu)) wdog_pet(); }
                s_stream_crc = crc32_update(0xFFFFFFFFu, src, p->p_filesz);
            }
            dec("mss-boot:   seg copied in ", timer_ms() - seg_t0); say(" ms");
            if (p->p_filesz) {
                uint32_t rb = 0xFFFFFFFFu; const uint8_t *d8 = (const uint8_t *)(uintptr_t)p->p_paddr;
                for (uint32_t k = 0; k < p->p_filesz; k += 4096u) { uint32_t c = p->p_filesz - k > 4096u ? 4096u : p->p_filesz - k; rb = crc32_update(rb, d8 + k, c); if (!(k & 0xFFFFFu)) wdog_pet(); }
                hex(", crc32 streamed ", s_stream_crc ^ 0xFFFFFFFFu); hex(" readback ", rb ^ 0xFFFFFFFFu);
                say((s_stream_crc ^ 0xFFFFFFFFu) == (rb ^ 0xFFFFFFFFu) ? " (match)" : " (MISMATCH: the target does not hold what we wrote)");
            }
            say("\n");
            if (p->p_memsz > p->p_filesz) {
                volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)(p->p_paddr + p->p_filesz);
                for (uint32_t k = 0; k < (p->p_memsz - p->p_filesz + 3u) / 4u; k++) d[k] = 0;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            /* verify_blob */
            if (first) { mmio_write(RMB_PMI_START, p->p_paddr); mmio_write(RMB_MBA_COMMAND, CMD_LOAD_READY); first = 0; }
            total += p->p_memsz;
            mmio_write(RMB_PMI_LENGTH, total);
            __asm__ volatile("dsb sy" ::: "memory");
            {   /* v429: the Gen 5 resets within a fraction of a second of this write on seg 12 (too fast
                 * for a hash of 7.9 MB = a request check, not a data check). Watch the MBA's status and
                 * debug words tightly for 500 ms and sync every change to the blackbox so the value the
                 * MBA writes in the instant before the reset survives it. */
                int32_t st = (int32_t)mmio_read(RMB_MBA_STATUS), last = st; uint32_t dbg = mmio_read(RMB_DEBUG_INFO), ldbg = dbg, t0 = timer_ms();
                hex("mss-boot:   after length write: status ", (uint32_t)st); hex(" dbg ", dbg); dec(" at +", t0); say(" ms\n"); con_flush(); blackbox_sync();
                while ((uint32_t)(timer_ms() - t0) < 100u) {
                    st = (int32_t)mmio_read(RMB_MBA_STATUS); dbg = mmio_read(RMB_DEBUG_INFO);
                    if (st != last || dbg != ldbg) { hex("mss-boot:   status now ", (uint32_t)st); hex(" dbg ", dbg); dec(" at +", timer_ms()); say(" ms\n"); con_flush(); blackbox_sync(); last = st; ldbg = dbg; }
                    if (st < 0) break;
                }
                if (st < 0) { hex("mss-boot: MBA error after segment ", (uint32_t)st); say("\n"); rmb_dump("seg-fail"); return -1; } }
        }
        hex("mss-boot: segments loaded, total ", total); say(" B\n");
    }

    /* 4. auth_and_reset: wait for AUTH_COMPLETE -> the modem is running */
    {
        int32_t st = poll_status(RMB_MBA_STATUS, 0, ST_AUTH_COMPLETE, 10000u);
        hex("mss-boot: final MBA status ", (uint32_t)st); say(st == ST_AUTH_COMPLETE ? " (AUTH_COMPLETE, modem running)\n" : " (FAIL)\n");

        s_mss_loading = 0;                                  /* v395: the WCNSS loader may run again */
        rmb_dump("final");
        if (st != ST_AUTH_COMPLETE) return -1;
    }
    /* v347: v346 died 2-3 s after AUTH_COMPLETE with SMEM 421 still "SFR Init: wdog or kernel
     * error suspected." (the modem's placeholder, no ERR_FATAL text) and rebooted to fastboot.
     * 200 ms monitor, light sync (no settle): GIC pending of the MSS wdog-bite SPI 24, the SFR
     * string, and the modem->apps smp2p item (host 1, item 435: err_fatal/err_ready/proxy bits). */
    {
        /* v354: modem SMD edge (DT smd-modem: doorbell bitmask 0x1000 = bit 12, SPI 25). Watch the
         * APPS<->MDMSW channel table (host 1 item 13, else global), open IPCRTR when it appears and
         * dump what the modem sends (IPC router HELLO etc.). */
        static struct smd_chan s_rtr; int rtr_open = 0; uint32_t last_nch = 0xFFFFFFFFu;
        /* v400: the modem creates apr_audio_svc / apr_apps2 itself at +600 ms and on stock the APPS
         * opens its side (kernel apr driver). A modem task blocked in its own open until the peer
         * opens is a textbook "stalled initialization" (dog.c:1522). Open ours and drain them. */
        /* v435: the Gen 5 modem creates "fastrpcsmd-apps-dsp" (kernel adsprpc opens it on stock) and
         * the five DIAG channels itself (kernel diag opens them on stock) and parks its init on them
         * (dog.c:1522 after announcing only SSCTL). Open our side of every channel stock opens;
         * SSM_RTR_MODEM_APPS and apr_apps2 stay closed as on stock. */
        #define N_OPEN 7u
        static struct smd_chan s_apr[N_OPEN];
        /* v438: bisect mask -- bit n opens k_apr[n]. Gen 5 died at +20 s (wdog, slave-kernel 0x7) with all
         * seven open, vs a dog.c stall at +40..70 s with only apr_audio_svc. */
#ifndef MSS_OPEN_MASK
#define MSS_OPEN_MASK 0x7Fu
#endif
        static const char *const k_apr[N_OPEN] = { "apr_audio_svc", (MSS_OPEN_MASK & 2u) ? "fastrpcsmd-apps-dsp" : "",
            (MSS_OPEN_MASK & 4u) ? "DIAG_2_CMD" : "", (MSS_OPEN_MASK & 8u) ? "DIAG_2" : "", (MSS_OPEN_MASK & 16u) ? "DIAG_CNTL" : "",
            (MSS_OPEN_MASK & 32u) ? "DIAG_CMD" : "", (MSS_OPEN_MASK & 64u) ? "DIAG" : "" };
        int apr_open[N_OPEN] = { 0 }; uint32_t apr_rx[N_OPEN] = { 0 }; int fatal_dumped = 0;
        char last_sfr[96] = { 0 }; uint32_t last_sp[16] = { 0 }, last_pend = 0xFFFFFFFFu;
        /* v351: v349 alive at +7 s, smp2p $SMP v1 16 slots 0 entries, MPSS still awake.
         * Watch 10 min: 200 ms for the first 20 s, then every 10 s. */
        s_mss_release_ms = timer_ms() ? timer_ms() : 1u;
        for (unsigned t = 0; t < 100u + 60u; t++) {
            s_mss_iter++; WHERE("mon-smem");
            uint32_t pend = mmio_read(PLAT_GICD_BASE + 0x204u) & (1u << 24);
            uint32_t sz = 0; const char *r = (const char *)smem_get(421u, &sz);
            char b[96] = { 0 }; uint32_t k = 0;
            if (r && sz) for (; k < sz && k < 95u && r[k]; k++) b[k] = (r[k] >= 32 && r[k] < 127) ? r[k] : '.';
            b[k] = 0;
            uint32_t psz = 0; const uint32_t *sp = (const uint32_t *)smem_get_host(1u, 435u, &psz);
            uint32_t cur[16] = { 0 }; if (sp && psz >= 64u) memcpy(cur, sp, 64);
            {   uint32_t tsz = 0, host = 1u; const volatile struct smd_alloc_entry_m { char name[20]; uint32_t cid, flags, ref; } *e =
                    smem_get_host(1u, 13u, &tsz);
                if (!e) { host = 0xFFFFu; e = smem_get(13u, &tsz); }
                uint32_t n = e ? tsz / sizeof *e : 0, used = 0, rcid = 0xFFFFFFFFu;
                for (uint32_t q = 0; q < n; q++) if (e[q].name[0] || e[q].cid || e[q].flags) used++;
                if (used != last_nch) {
                    dec("mss-smd: +", t < 100u ? t * 200u : 20000u + (t - 100u) * 10000u); dec(" ms table host ", host); dec(" channels ", used); con_puts("\n");
                    for (uint32_t q = 0; q < n; q++) {
                        if (!(e[q].name[0] || e[q].cid || e[q].flags)) continue;
                        char nm[21]; for (unsigned k = 0; k < 20u; k++) nm[k] = e[q].name[k]; nm[20] = 0;
                        dec("mss-smd:   cid ", e[q].cid); dec(" edge ", e[q].flags & 0xFFu); con_puts((e[q].flags & 0x200u) ? " pkt \"" : " strm \""); con_puts(nm); con_puts("\"\n");
                    }
                    last_nch = used;
                }
                for (uint32_t q = 0; q < n; q++) if (!strncmp(e[q].name, "IPCRTR", 7) && (e[q].flags & 0xFFu) == 0u) rcid = e[q].cid;
                for (unsigned a = 0; a < N_OPEN; a++) if (!apr_open[a] && k_apr[a][0]) {
                    for (uint32_t q = 0; q < n; q++) if (!strncmp(e[q].name, k_apr[a], 20) && (e[q].flags & 0xFFu) == 0u) {
                        say("mss-smd: opening "); say(k_apr[a]); dec(" cid ", e[q].cid); say(" (our side of the modem's audio channel) ...\n");
                        apr_open[a] = smd_open(&s_apr[a], host, e[q].cid, 12u) == 0 ? 1 : -1;
                        /* v457: the stock kernel answers the modem's DIAG_CNTL open with a feature-mask
                         * control packet (diag_masks.c diag_send_feature_mask_update: pkt_id 8, data_len 8,
                         * mask_len 4, mask bits FEATURE_MASK_SUPPORT LOG_ON_DEMAND REQ_RSP HDLC STM
                         * MASK_CENTRALIZATION SOCKETS DCI_EXT_HEADER DIAGID = 0xEA55). It is the one exchange
                         * in stock's first 13 s that we never did (v438 only opened the channels). */
                        if (apr_open[a] == 1 && !strncmp(k_apr[a], "DIAG_CNTL", 9)) {
                            /* v460: without DIAGID (bit 15) and DCI-ext-header (bit 14): with DIAGID advertised the stock driver
                             * holds every mask until the peripheral completes a diag-id exchange (pkt 33), which this modem
                             * never started with us; masks sent anyway produced no F3 stream (v458/v459). */
                            static const uint8_t fm[16] = { 8,0,0,0, 8,0,0,0, 4,0,0,0, 0x55,0x2A,0,0 };
                            int fr = smd_send(&s_apr[a], fm, sizeof fm);
                            dec("mss-diag: DIAG_CNTL feature mask 0x2A55 sent rc ", (uint32_t)(fr < 0 ? -fr : 0)); say("\n");
                            /* v459: DIAGMODE (pkt 3, v1, 36 B data): real_time 1, sleep_vote 1, the rest 0 --
                             * diag_create_diag_mode_ctrl_pkt. Without it a peripheral may hold its F3 stream
                             * in its buffer (v458: masks accepted, nothing ever arrived on DIAG). */
                            static const uint32_t dm[11] = { 3u, 36u, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 0u };
                            fr = smd_send(&s_apr[a], dm, sizeof dm);
                            dec("mss-diag: DIAGMODE real-time sent rc ", (uint32_t)(fr < 0 ? -fr : 0)); say("\n");
                        }
                        if (!strcmp(k_apr[a], "DIAG")) { dec("mss-diag: DIAG data channel open rc ", (uint32_t)(apr_open[a] == 1 ? 0 : 1)); say("\n"); }
                        say(apr_open[a] == 1 ? "mss-smd: apr channel OPEN\n" : "mss-smd: apr channel open FAILED\n");
                    }
                }
                if (!rtr_open && rcid != 0xFFFFFFFFu) {
                    dec("mss-smd: opening IPCRTR cid ", rcid); say(" on the modem edge (doorbell bit 12) ...\n");
                    rtr_open = smd_open(&s_rtr, host, rcid, 12u) == 0 ? 1 : -1;
                    say(rtr_open == 1 ? "mss-smd: IPCRTR OPEN\n" : "mss-smd: IPCRTR open FAILED\n");
                    if (rtr_open == 1) {
                        /* v355: IPC router v1 (net/ipc_router): HELLO from APPS node 1 (versions = v1 only,
                         * checksum makes the folded one's-complement sum == IPC_ROUTER_HELLO_MAGIC 0xE110),
                         * then NEW_SERVER rmt_storage svc 14 inst 1 at node 1 port 0x4000 (stock: svc 0x0e
                         * inst 1 on node 1). Header: version, type(=cmd), src node, src port, ctrl, size,
                         * dst node, dst port. Control port 0xfffffffe, broadcast node 0xffffffff. */
                        uint32_t hello[13] = { 1u, 2u, 1u, 0xfffffffeu, 0u, 20u, 0xffffffffu, 0xfffffffeu,
                                               2u, 0x1eebu, 0x2u, 0u, 0u };
                        int rc = smd_send(&s_rtr, hello, sizeof hello);
                        dec("mss-rtr: tx HELLO rc ", (uint32_t)(rc < 0 ? 1 : 0)); say("\n");
                        dec("mss-rtr: tx NEW_SERVER x19 (stock apps service list) failures ", (uint32_t)rtr_announce_all(&s_rtr)); say("\n");
                    }
                }
                for (unsigned a = 0; a < N_OPEN; a++) if (apr_open[a] == 1) {
                    static uint8_t ab[2048]; uint32_t g; unsigned np = 0;
                    /* v456: the fastrpc channel belongs to the listener (mss_fastrpc.c), not the hex dump */
                    if (a == 1u && (MSS_OPEN_MASK & 2u)) { WHERE("fastrpc"); mss_fastrpc_poll(&s_apr[a]); continue; }
                    /* 2026-09-17: apr_audio_svc belongs to the APR client (mss_apr.c, speaker step 1) */
                    if (a == 0u) { WHERE("apr"); mss_apr_poll(&s_apr[a]); continue; }
                    while (np < 8u && (g = smd_recv(&s_apr[a], ab, sizeof ab, 0u)) != 0u) {
                        np++; apr_rx[a] += g;
                        /* v458: DIAG_CNTL feeds the F3-mask sender, DIAG (data) the F3 decoder */
                        if (!strncmp(k_apr[a], "DIAG_CNTL", 9) && mss_diag_cntl_rx(&s_apr[a], ab, g)) continue;
                        if (!strcmp(k_apr[a], "DIAG")) { mss_diag_data_rx(ab, g); continue; }
                        say("mss-apr: "); say(k_apr[a]); dec(" rx ", g); say(" B:");
                        for (uint32_t k = 0; k < g && k < 32u; k++) { static const char hx[] = "0123456789abcdef"; char c2[3] = { hx[ab[k] >> 4], hx[ab[k] & 15], 0 }; if (!(k % 4u)) say(" "); say(c2); }
                        say("\n");
                    }
                }
                if (rtr_open == 1) {
                    static uint8_t pk[2048]; uint32_t got; unsigned npk = 0;   /* v389: RW_IOVEC requests can exceed 512 B */
                    /* v373: once the modem really runs it can flood IPCRTR; a hex dump per packet without
                     * yielding starves the main loop -> hardware wdog bark (v372 rebooted to fastboot).
                     * Cap packets per iteration and yield between them. */
                    WHERE("rtr-recv");
                    while (npk < 16u && (got = smd_recv(&s_rtr, pk, sizeof pk, 0u)) != 0u) {
                        npk++; vTaskDelay(1);
                        /* v357: stock sends its server list on RECEIVING the remote HELLO; ours went out
                         * before the modem had processed our HELLO and may have been dropped. Re-announce
                         * rmtfs on every HELLO from the modem. Data packets (type 1) get a QMI decode. */
                        if (got >= 36u) {
                            uint32_t w[9]; memcpy(w, pk, sizeof w);
                            if (w[1] == 2u) {
                                dec("mss-rtr: modem HELLO -> re-announce x19 failures ", (uint32_t)rtr_announce_all(&s_rtr)); con_puts("\n");
                            } else if (w[1] == 1u && got >= 39u) {
                                hex("mss-rtr: DATA from node ", w[2]); hex(" port ", w[3]); hex(" to port ", w[7]);
                                dec(" len ", w[5]); hex(" qmi flags ", pk[32]); dec(" txn ", (uint32_t)(pk[33] | pk[34] << 8));
                                hex(" msg ", (uint32_t)(pk[35] | pk[36] << 8)); dec(" tlvlen ", (uint32_t)(pk[37] | pk[38] << 8)); con_puts("\n");
                                /* v389: flow control (confirm_rx -> RESUME_TX) and the rmtfs server on port 0x4000 */
                                if (w[4] & 1u) mss_rtr_resume_tx(&s_rtr, w[2], w[6], w[7]);
                                if (w[6] == 1u && w[7] >= 0x4000u && w[7] <= 0x4012u) { WHERE("rmtfs"); mss_rmtfs_handle(&s_rtr, pk, got); WHERE("rtr-recv"); }   /* rmtfs, RFSA, memshare, stubs */
                            } else { dec("mss-rtr: ctrl type ", w[1]); dec(" cmd ", w[8]); con_puts("\n"); }
                        }
                        if (t < 100u) { dec("mss-rtr: rx ", got); con_puts(" B:");
                        for (uint32_t k = 0; k < got && k < 160u; k++) { if (!(k % 4u)) con_puts(" "); static const char hx[] = "0123456789abcdef"; char c2[3] = { hx[pk[k] >> 4], hx[pk[k] & 15], 0 }; con_puts(c2); }
                        con_puts("\n"); }
                    }
                }
            }
            WHERE("mon-smsm");
            {   /* v361: SMSM handshake mirror (smsm_irq_handler) + RPCINIT once IPCRTR is open. */
                static uint32_t last_modm = 0xFFFFFFFFu; volatile uint32_t *st = smsm_state();
                if (st) {
                    uint32_t modm = st[1], set = 0;
                    if (modm != last_modm) { hex("mss-smsm: modem ", modm); hex(" apps ", st[0]); say(" all"); for (unsigned k = 0; k < 8u; k++) hex(" ", st[k]); say("\n"); last_modm = modm; }
                    if (modm & SMSM_INIT_BIT) { set |= SMSM_INIT_BIT; if (modm & SMSM_SMDINIT_BIT) set |= SMSM_SMDINIT_BIT; }
                    if (rtr_open == 1) set |= SMSM_RPCINIT_BIT;
                    if (set) smsm_apps_set(set);
                }
            }
            if (rtr_open == 1 && t < 100u && (t % 5u) == 0) {
                /* v365: is the modem writing IPCRTR data we never read? Dump both info blocks + pending. */
                dec("mss-rtr: rx-pend ", smd_rx_pending(&s_rtr)); dec(" remote-state ", smd_remote_state(&s_rtr));
                say(" theirs"); for (unsigned k = 0; k < 8u; k++) hex(" ", s_rtr.rx[k]);
                say(" ours"); for (unsigned k = 0; k < 8u; k++) hex(" ", s_rtr.tx[k]); say("\n");
            }
            if (rtr_open == 1 && t == 25u) {
                /* v387: liveness probe above the transport: QMI SSCTL (svc 0x2b inst 0x1202, node 0
                 * port 1, announced by the modem) GET_FAILURE_REASON_REQ (msg 0x0022, no TLVs).
                 * IPC router v1 DATA: type 1, src node 1 port 0x4000, dst node 0 port 1, size = 7. */
                uint32_t hdr[8] = { 1u, 1u, 1u, 0x4000u, 0u, 7u, 0u, 1u };
                uint8_t pkt[32 + 8]; memcpy(pkt, hdr, 32);
                pkt[32] = 0u; pkt[33] = 1u; pkt[34] = 0u; pkt[35] = 0x22u; pkt[36] = 0u; pkt[37] = 0u; pkt[38] = 0u; pkt[39] = 0u;
                int rc = smd_send(&s_rtr, pkt, 32u + 8u);
                dec("mss-rtr: tx SSCTL get-failure-reason (QMI 0x22) to node 0 port 1 rc ", (uint32_t)(rc < 0 ? 1 : 0)); say("\n");
            }
#if defined(PLAT_HAS_BG_QCC1110) && !defined(MSS_NO_BG_EVENT)
            {   /* v464: stock subsystem_restart.c send_sysmon_notif(): a subsystem coming up is told the state
                 * of every other online subsystem. QMI SSCTL (svc 0x2b, node 0 port 1) SUBSYS_EVENT_REQ 0x23:
                 * TLV1 {u8 len, "bg-wear"}, TLV2 u32 event AFTER_POWERUP=1, TLV0x10 u32 evt_driven FORCED=0.
                 * Our src port 0x4020 keeps the reply out of the rmtfs handler (0x4000..0x4012). */
                static int bg_evt_sent;
                extern volatile int g_bgcom_codec_done;   /* never while bgcom.c still drives the BG bus */
                if (rtr_open == 1 && !bg_evt_sent && t >= 10u && tlmm_in(PLAT_BG2AP_STATUS_GPIO) && g_bgcom_codec_done) {
                    static const uint8_t qmi[32] = { 0x00, 0x02, 0x00, 0x23, 0x00, 25, 0x00,
                        0x01, 8, 0, 7, 'b', 'g', '-', 'w', 'e', 'a', 'r',
                        0x02, 4, 0, 1, 0, 0, 0,
                        0x10, 4, 0, 0, 0, 0, 0 };
                    uint32_t hdr[8] = { 1u, 1u, 1u, 0x4020u, 0u, sizeof qmi, 0u, 1u };
                    uint8_t pkt[32 + sizeof qmi]; memcpy(pkt, hdr, 32); memcpy(pkt + 32, qmi, sizeof qmi);
                    int rc = smd_send(&s_rtr, pkt, sizeof pkt);
                    bg_evt_sent = 1;
                    g_bgcom_ap_released = 1;   /* from here the modem drives the BG SPI, not us */
                    dec("mss-rtr: tx SSCTL subsys-event \"bg-wear\" AFTER_POWERUP (QMI 0x23) at +", timer_ms());
                    dec(" ms rc ", (uint32_t)(rc < 0 ? 1 : 0)); say(" (reply = DATA to port 0x4020 msg 0x23)\n");
                    q6_audio_probe(500u, "P1 after the bg-wear hand-off");
                }
            }
#endif
            if (t < 100u && (t % 5u) == 0) {
                /* v369: rpm_requests channel info blocks (SMD_BASE_ID 14 + cid: 18 = apps<->rpm cid 4,
                 * 19 = modem<->rpm cid 5, 20..22 = wcnss/tz/adsp). 11 words per half: does the RPM serve the modem? */
                /* v371: decoded modem<->RPM channel (item 19): per half state fDSR fCTS fCD fRI fHEAD fTAIL
                 * fSTATE fBLOCKREADINTR tail head. Unread RPM replies (rpm head != tail) = the modem is not
                 * getting the RPM's interrupt. */
                { uint32_t sz = 0; const volatile uint32_t *ci = (const volatile uint32_t *)smem_get(19u, &sz);
                  if (ci && sz >= 88u) {
                      say("mss-rpmch: modem-half st "); con_putdec(ci[0]); say(" flags"); for (unsigned k = 1; k < 9u; k++) { say(" "); con_putdec(ci[k]); }
                      hex(" tail ", ci[9]); hex(" head ", ci[10]);
                      say(" | rpm-half st "); con_putdec(ci[11]); say(" flags"); for (unsigned k = 12; k < 20u; k++) { say(" "); con_putdec(ci[k]); }
                      hex(" tail ", ci[20]); hex(" head ", ci[21]); say("\n");
                  } }
            }
            WHERE("mon-print");

            if (t == 0u || t == 10u || t == 50u) smem_dump_items(t == 0u ? "+0s" : t == 10u ? "+2s" : t == 50u ? "+10s" : "+20s");
            /* v436: QUIET after +20 s -- the console and the 3 KB blackbox drowned in per-second dumps.
             * From then on: one compact status line every 10 s plus change-driven lines. */
            if (t >= 100u) {
                uint32_t b2 = 0x60150u + 4096u; uint8_t en3 = 0xFF; (void)spmi_read8(1u, 0x1A46u, &en3);
                dec("mss: +", 20u + (t - 100u) * 10u); hex(" s slave-kernel ", cur[14]); hex(" wdog-pend ", pend);
                dec(" MPSS shutdowns ", mmio_read(b2 + 4u)); dec(" xo ", mmio_read(b2 + 52u)); hex(" cores ", mmio_read(b2));
                dec(" vmin ", mmio_read(0x0029dba0u + 48u + 4u)); con_puts(en3 & 0x80u ? " s3 ON" : " s3 off");
                if (strcmp(b, last_sfr) || (cur[14] & 1u)) { con_puts(" sfr \""); con_puts(b); con_puts("\""); strncpy(last_sfr, b, sizeof last_sfr - 1u); }
                con_puts("\n");
                if (memcmp(cur, last_sp, sizeof cur)) mmio_write(0x0b011008u, 1u << 14);   /* smp2p doorbell ack */
                if (sp && psz >= 340u) memcpy(last_sp, cur, sizeof cur);
                last_pend = pend;
                mss_mon_sleep(10000u, &s_apr[0], apr_open[0] == 1);
                continue;
            }
            int chg = (pend != last_pend) || strcmp(b, last_sfr) || memcmp(cur, last_sp, sizeof cur) || (t % 5u) == 0;
            if (chg) {
                dec("mss-mon: +", t < 100u ? t * 200u : 20000u + (t - 100u) * 10000u); hex(" ms wdog-spi24-pend ", pend); dec(" smp2p sz ", psz);
                for (unsigned w = 0; w < 16u; w++) hex(" ", cur[w]);
                if (strcmp(b, last_sfr)) { con_puts(" sfr \""); con_puts(b); con_puts("\""); }
                if (sp && psz >= 340u && memcmp(cur, last_sp, sizeof cur)) {
                    uint32_t nv = sp[3] >> 16;
                    for (uint32_t k = 0; k < nv && k < 16u; k++) {
                        const uint8_t *en = (const uint8_t *)sp + 20u + k * 20u; char nm[17]; memcpy(nm, en, 16); nm[16] = 0;
                        uint32_t v; memcpy(&v, en + 16, 4);
                        con_puts("\nmss-smp2p: modem entry \""); con_puts(nm); hex("\" = ", v);
                        if (!strncmp(nm, "slave-kernel", 16) && (v & 2u)) s_mss_ready = 1;
                        if (!strncmp(nm, "slave-kernel", 16) && (v & 1u) && !fatal_dumped) {
                            /* v400: first err_fatal -> SMEM crash evidence: SSR reason (421) in full and the
                             * modem's ERR crash log (SMEM_ERR_CRASH_LOG = 408, 13 enum entries before 421),
                             * which names the ERR_FATAL site and usually the task. */
                            fatal_dumped = 1;
                            say("\n"); smem_dump_items("err-fatal");
                            {   /* v401: SMEM event log (item 79: 2000 x {identifier, timetick, data1..3}, idx in
                                 * item 78). The modem's err handler logs ERR_ERROR_FATAL (0x60001) and
                                 * ERR_ERROR_FATAL_TASK (0x60002, data = task name) -- see smem_log.h. Print every
                                 * error-base event and the last 32 entries raw. */
                                uint32_t lsz = 0, isz2 = 0; const uint32_t *ev = (const uint32_t *)smem_get(79u, &lsz);
                                const uint32_t *ix = (const uint32_t *)smem_get(78u, &isz2);
                                uint32_t ne = ev ? lsz / 20u : 0, idx = ix ? ix[0] : 0;
                                dec("mss-smemlog: entries ", ne); dec(" idx ", idx); say("\n");
                                for (uint32_t k = 0; k < ne; k++) {
                                    const uint32_t *e5 = ev + k * 5u;
                                    if ((e5[0] & 0x0FFF0000u) != 0x00060000u) continue;
                                    hex("mss-smemlog: ERR event #", k); hex(" id ", e5[0]); hex(" t ", e5[1]); hex(" d ", e5[2]); hex(" ", e5[3]); hex(" ", e5[4]); say(" \"");
                                    for (unsigned b = 0; b < 12u; b++) { char ch = (char)((const uint8_t *)(e5 + 2))[b]; char c2[2] = { (ch >= 32 && ch < 127) ? ch : '.', 0 }; say(c2); }
                                    say("\"\n");
                                }
                                for (uint32_t k = 0; k < 32u && ne; k++) {
                                    uint32_t j = (idx + ne - 32u + k) % ne; const uint32_t *e5 = ev + j * 5u;
                                    if (!e5[0] && !e5[1]) continue;
                                    hex("mss-smemlog: #", j); hex(" id ", e5[0]); hex(" t ", e5[1]); hex(" d ", e5[2]); hex(" ", e5[3]); hex(" ", e5[4]); say(" \"");
                                    for (unsigned b = 0; b < 12u; b++) { char ch = (char)((const uint8_t *)(e5 + 2))[b]; char c2[2] = { (ch >= 32 && ch < 127) ? ch : '.', 0 }; say(c2); }
                                    say("\"\n");
                                }
                            }
                            for (unsigned it = 0; it < 2u; it++) {
                                uint32_t id = it ? 408u : 421u, csz = 0; const char *cl = (const char *)smem_get(id, &csz);
                                dec("mss-crash: item ", id); dec(" size ", csz); say(": \"");
                                if (cl) for (uint32_t k = 0; k < csz && k < 1024u; k++) { char ch = cl[k]; if (ch == 0 && k && cl[k - 1] == 0) break; if (ch == 0) ch = '|'; if (ch == '\n') ch = '/'; if (ch < 32 || ch > 126) ch = '.'; char c2[2] = { ch, 0 }; say(c2); }
                                say("\"\n");
                            }
                        }
#if defined(MSS_PROXY_VOTES)
                        /* v360: stock pil_q6v5_remove_proxy_votes on the proxy-unvote bit (slave-kernel
                         * bit 2): corner back to NONE (wire 0) on cx and mx, once. Holding cx at
                         * SUPER_TURBO would block vmin. */
                        { static int unvoted;
                          if (!unvoted && !strncmp(nm, "slave-kernel", 16) && (v & 4u)) {
                              /* v431: RELEASE EVERYTHING THE BOOT VOTED. v374 held cx/mx at NORMAL (wire 4) and the
                               * pcnoc/snoc/bimc bus clocks at 100/400 MHz in the APPS ACTIVE set and never dropped
                               * them; the sleep set does not name them, so the RPM kept them through every collapse:
                               * C2 15.5 mA / S2 ~20 mA asleep on the coulomb counter (v415). Before the modem work
                               * the APPS voted none of these; the modem votes its own once it runs. Back to 0. */
                              uint32_t kv0[3] = { 0x6e726f63u, 4u, 0u };
                              int r1 = rpm_smd_request(0, PLAT_MSS_CX_TYPE, PLAT_MSS_CX_ID, kv0, sizeof kv0);
                              int r2 = rpm_smd_request(0, PLAT_MSS_MX_TYPE, PLAT_MSS_MX_ID, kv0, sizeof kv0);
                              uint32_t ck0[3] = { 0x007a484bu /* KHz */, 4u, 0u };
                              int r3 = rpm_smd_request(0, 0x316b6c63u /* clk1 */, 0u, ck0, sizeof ck0);   /* pcnoc */
                              int r4 = rpm_smd_request(0, 0x316b6c63u, 1u, ck0, sizeof ck0);               /* snoc  */
                              int r5 = rpm_smd_request(0, 0x326b6c63u /* clk2 */, 0u, ck0, sizeof ck0);   /* bimc  */
                              dec("\nmss-boot: proxy-unvote seen -> cx/mx corner -> 0 rc ", (uint32_t)((r1 | r2) < 0 ? 1 : 0));
                              dec(", pcnoc/snoc/bimc active votes -> 0 rc ", (uint32_t)((r3 | r4 | r5) < 0 ? 1 : 0));
                              unvoted = 1;
                          } }
#endif
                    }
                }
                if (memcmp(cur, last_sp, sizeof cur)) mmio_write(0x0b011008u, 1u << 14);
                con_puts("\n");
                if ((t % 5u) == 0 || t >= 100u) rpm_master_stats_line("mss-mon");
                last_pend = pend; memcpy(last_sfr, b, sizeof b); memcpy(last_sp, cur, sizeof cur);
            }
            WHERE("mon-delay"); mss_mon_sleep(t < 100u ? 200u : 10000u, &s_apr[0], apr_open[0] == 1);
        }
    }
    return 0;
}
static void mss_boot_task(void *arg)
{
    (void)arg;
    (void)mss_boot();
    s_mss_loading = 0; s_mss_task_done = 1;
    con_puts("mss-boot: task done\n");
    vTaskDelete(0);
}
void mss_boot_start(void)
{
    static int started;
    if (started) return;
    started = 1;
    if (xTaskCreate(mss_boot_task, "mss-boot", 4096, 0, 1, 0) != pdPASS) con_puts("mss-boot: task create failed\n");
}
#endif /* PLAT_SOC_MSM8909 && MSS_BOOT */
