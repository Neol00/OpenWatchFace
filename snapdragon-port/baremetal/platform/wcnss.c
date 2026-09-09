/* wcnss.c — boot the Pronto/WCNSS WiFi co-processor (steps 3+4 of
 * WIFI-BRINGUP.md).
 *
 * WHAT HAS TO HAPPEN, in the order mainline's qcom_wcnss.c + mdt_loader.c do
 * it (the vendor tree does the same things spread over three drivers):
 *
 *   1. crypto-engine clocks on        TZ hashes the image with the CE; the
 *                                     PIL driver proxies gcc_crypto_* for it
 *   2. wcnss.mdt -> DDR, cache-clean  pas_init_image(6, mdt_phys): TZ checks
 *                                     the signature and remembers the hashes
 *   3. pas_mem_setup(6, base, size)   where the segments will be
 *   4. copy wcnss.bNN segments        relocated: dest = base + (paddr - min)
 *   5. rails + clocks via the RPM     vddmx/vddcx/vddpx for Pronto, vddxo/
 *                                     vddrfa/vdddig for Iris, rf_clk, cxo
 *   6. TLMM 76..80 -> wcss_wlan       the 5-wire Iris interface
 *   7. Iris XO config in the PMU      NV-download flag, XO enable, reset,
 *                                     XO_CFG, settle
 *   8. pas_auth_and_reset(6)          TZ verifies every segment hash and
 *                                     releases the Pronto core from reset
 *   9. wait for SMD channels          WCNSS firmware creates them in the
 *                                     apps<->wcnss SMEM partition
 *
 * THE FIRMWARE FILES live in the `modem` eMMC partition, a FAT16 volume with
 * an IMAGE/ directory (WCNSS.MDT + WCNSS.B00..B12). A private read-only
 * FAT16 walker is used rather than a second FatFs volume: it is 100 lines,
 * needs no config change to the userdata storage, and reads through the
 * existing emmc_read().
 *
 * EVERY STEP PRINTS BEFORE IT ACTS. Steps 7 and 8 are the first in this
 * project that can end in a hang or a TZ-initiated reset, and a line that
 * reaches the host before the risky access is the only evidence that
 * survives one. The dead-man timer still reboots to fastboot if it hangs.
 */
#include "platform.h"
#if defined(PLAT_WCNSS_FW_BASE) && defined(PLAT_SMEM_BASE)

#include <string.h>

/* ---- console helpers ---------------------------------------------------- */
/* Every line is also forced onto the eMMC blackbox before we go on: a TZ or
 * XPU reset leaves no other trace, and the watch may be powered off (DDR
 * lost) before the log can be read. blackbox_print_previous() replays it. */
#if defined(LOG_VERBOSE)
#define vsay(s)        say(s)
#define vsay_hex(s, v) say_hex(s, v)
#define vsay_dec(s, v) say_dec(s, v)
#else
#define vsay(s)        ((void)(s))
#define vsay_hex(s, v) ((void)(s), (void)(v))
#define vsay_dec(s, v) ((void)(s), (void)(v))
#endif
static void say(const char *s) { con_puts(s); con_flush(); usb_poll(); blackbox_sync(); }
static void say_hex(const char *s, uint32_t v) { con_puts(s); con_puthex(v); con_flush(); usb_poll(); }
static void say_rc(const char *s, int rc)
{
    if (rc >= 0) { con_dbg(s); con_dbg(" ok"); if (rc) { con_dbg(" ("); con_dbg_dec((uint32_t)rc); con_dbg(")"); } con_dbg("\n"); }
    else { con_puts(s); con_puts(" FAILED rc=-"); con_putdec((uint32_t)-rc); con_puts("\n"); }
    con_flush(); usb_poll(); timer_delay_ms(30);
}

/* ---- FAT16 reader over the modem partition ------------------------------ */
#define SECT 512u
static uint32_t s_part_lba, s_fat_lba, s_root_lba, s_data_lba, s_spc, s_spf;
static uint8_t  s_sec[SECT] __attribute__((aligned(64)));
static uint8_t  s_cluster[32u * SECT] __attribute__((aligned(64)));   /* 16 KB */
static int      s_fat_ok;

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int fat_init(void)
{
    uint32_t nblk, rsv, nfats, rootent;

    if (s_fat_ok) return 0;
    if (emmc_gpt_find("modem", &s_part_lba, &nblk) < 0) { say("wcnss: no `modem` partition in GPT\n"); return -1; }
    if (emmc_read(s_part_lba, 1, s_sec) < 0) return -1;
    if (rd16(s_sec + 510) != 0xAA55u || rd16(s_sec + 11) != SECT) { say("wcnss: modem partition is not FAT\n"); return -1; }
    s_spc   = s_sec[13];
    rsv     = rd16(s_sec + 14);
    nfats   = s_sec[16];
    rootent = rd16(s_sec + 17);
    s_spf   = rd16(s_sec + 22);
    if (!s_spc || s_spc > 32u || !s_spf || !rootent) { say("wcnss: FAT geometry not FAT16\n"); return -1; }
    s_fat_lba  = s_part_lba + rsv;
    s_root_lba = s_fat_lba + nfats * s_spf;
    s_data_lba = s_root_lba + (rootent * 32u) / SECT;
    s_fat_ok = 1;
    return 0;
}

static uint32_t fat_next(uint32_t c)
{
    uint32_t byte = c * 2u;
    if (emmc_read(s_fat_lba + byte / SECT, 1, s_sec) < 0) return 0xFFFFu;
    return rd16(s_sec + (byte % SECT));
}

/* Find an 8.3 entry (name padded to 11, e.g. "WCNSS   B06") in a directory
 * that spans `nsect` sectors starting at `lba`. Returns cluster, size. */
static int fat_find(uint32_t lba, uint32_t nsect, const char *name11, uint32_t *clus, uint32_t *size)
{
    uint32_t s, i;
    for (s = 0; s < nsect; s++) {
        if (emmc_read(lba + s, 1, s_sec) < 0) return -1;
        for (i = 0; i < SECT; i += 32u) {
            const uint8_t *e = s_sec + i;
            if (e[0] == 0) return -1;
            if (e[0] == 0xE5u || e[11] == 0x0Fu) continue;
            if (!memcmp(e, name11, 11)) { *clus = rd16(e + 26); *size = rd32(e + 28); return 0; }
        }
    }
    return -1;
}

/* Read a whole file by cluster chain, delivering exact bytes to `sink`. */
static int fat_read_file(const char *name11, uint32_t *size_out,
                         void (*sink)(void *ctx, const uint8_t *p, uint32_t off, uint32_t n), void *ctx)
{
    static uint32_t s_img_clus, s_img_size;
    uint32_t c, size, off = 0, cbytes = s_spc * SECT;

    if (!s_img_clus && fat_find(s_root_lba, (s_data_lba - s_root_lba), "IMAGE      ", &s_img_clus, &s_img_size) < 0) {
        say("wcnss: no IMAGE directory\n"); return -1;
    }
    if (fat_find(s_data_lba + (s_img_clus - 2u) * s_spc, s_spc, name11, &c, &size) < 0) return -1;
    *size_out = size;
    while (off < size && c >= 2u && c < 0xFFF8u) {
        uint32_t n = size - off; if (n > cbytes) n = cbytes;
        { int tries = 0;
          while (emmc_read(s_data_lba + (c - 2u) * s_spc, (n + SECT - 1u) / SECT, s_cluster) < 0) {
              if (++tries >= 3) return -1;
              con_dbg("wcnss: eMMC read retry\n"); timer_delay_ms(5);
          } }
        sink(ctx, s_cluster, off, n);
        off += n;
        c = fat_next(c);
    }
    return off == size ? 0 : -1;
}

/* ---- ELF / MDT -------------------------------------------------------- */
struct elf32_phdr { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; };
#define PT_LOAD            1u
#define MDT_TYPE_MASK      (7u << 24)
#define MDT_TYPE_HASH      (2u << 24)
#define MDT_RELOCATABLE    (1u << 27)

/* Metadata buffer -- WHERE it lives is the point of seq v12.
 *
 * pas_init_image returns -13 (EACCES) on both watches. Every earlier attempt
 * placed this buffer INSIDE wcnss_fw_region (0x8A600000, a `no-map` reserved
 * carveout) to get an uncached Device mapping. But that region is exactly the
 * kind of address TZ's init_image rejects: it is not part of the HLOS-owned
 * DDR that TZ validates the metadata pointer against, and the XPU guards it.
 * The vendor allocates the metadata from ordinary kernel DMA memory (normal
 * HLOS lowmem) -- and cacheable-vs-uncached made no difference to our -13, so
 * the untested variable is the LOCATION, not the attribute.
 *
 * So the metadata now lives in a plain static buffer in our own .bss, which
 * links around 0x80xxxxxx -- normal cacheable HLOS DDR, in the memory map TZ
 * knows HLOS owns, exactly where the vendor's dma_alloc_coherent lands. We
 * cache-clean it (DCCMVAC) right before the SCM so TZ reads current bytes.
 * DDR is identity-mapped (VA==PA), so its address is the phys we hand TZ. */
static uint8_t s_mdt_buf[0x2000] __attribute__((aligned(64)));
#define WCNSS_MDT_SCRATCH  ((uint32_t)(uintptr_t)s_mdt_buf)
static uint8_t *const s_mdt = s_mdt_buf;
static uint32_t s_mdt_len;
static struct elf32_phdr s_phbuf[32];

static void sink_mdt(void *ctx, const uint8_t *p, uint32_t off, uint32_t n)
{
    (void)ctx;
    if (off + n <= 0x2000u)
        memcpy(s_mdt + off, p, n);
}

/* Segment sink: word copies into the (Device-mapped) firmware region. */
static void sink_seg(void *ctx, const uint8_t *p, uint32_t off, uint32_t n)
{
    volatile uint32_t *dst = (volatile uint32_t *)((uintptr_t)ctx + off);
    uint32_t i, w = (n + 3u) / 4u;
    for (i = 0; i < w; i++) {
        uint32_t v; memcpy(&v, p + 4u * i, 4);   /* p may be unaligned at the tail */
        dst[i] = v;
    }
}

static void region_zero(uint32_t phys, uint32_t n)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)phys;
    uint32_t i;
    for (i = 0; i < (n + 3u) / 4u; i++) d[i] = 0;
}

static void __attribute__((unused)) cache_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    for (; a < end; a += 32u)
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");   /* DCCMVAC */
    __asm__ volatile("dsb sy" ::: "memory");
}

static int seg_loadable(const struct elf32_phdr *p)
{
    return p->p_type == PT_LOAD && (p->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH && p->p_memsz != 0;
}

/* ---- RPM votes ---------------------------------------------------------- */
#define K_SWEN 0x6e657773u  /* "swen" */
#define K_UV   0x00007675u  /* "uv"   */
#define K_MA   0x0000616du  /* "ma"   */
#define K_VLVL 0x6c766c76u  /* "vlvl" */
#define K_ENAB 0x62616e45u  /* "Enab" */
#define K_KHZ  0x007a484bu  /* "KHz"  */
#define T_LDOA 0x616f646cu  /* "ldoa" */
#define T_SMPA 0x61706d73u  /* "smpa" */
#define T_CLKA 0x616B6C63u  /* "clka" XO buffers */
#define T_CLK0 0x306b6c63u  /* "clk0" misc: cxo */
#define T_CLK1 0x316b6c63u  /* "clk1" bus: snoc */
#define RPM_LEVEL_NOM   256u
#define RPM_LEVEL_TURBO 384u

/* Every vote is announced BEFORE it is sent and the line is forced onto the
 * eMMC blackbox: the Gen 4's first WiFi run reset inside the first rail vote
 * with nothing after "voting rails", so the vote that kills the SoC must be
 * named by a line that precedes it. */
static uint32_t s_t0;   /* wcnss_boot start, for the [+ms] stamps */
static void pre(const char *what)
{
    uint32_t t;
    con_dbg("  [+"); con_dbg_dec(timer_ms() - s_t0); con_dbg(" ms]"); con_dbg(what); con_dbg(" ...");
    con_flush(); blackbox_sync();
    /* Let the host actually drain the line before the request goes out: the
     * v1/v2 Gen 4 logs both ended one line SHORT of the request that reset
     * the SoC, so the announce must sit on the wire, not in our FIFO. */
    for (t = timer_ms(); timer_ms() - t < 20u; ) { usb_poll(); timer_delay_ms(1); }
}
static int vote_uv(const char *what, uint32_t type, uint32_t id, uint32_t uv, uint32_t ma)
{
    uint32_t kv[9] = { K_SWEN, 4, 1, K_UV, 4, uv, K_MA, 4, ma };
    int rc;
    pre(what);
    rc = rpm_smd_request(0, type, id, kv, sizeof kv);
    say_rc("", rc);
    return rc;
}
static int vote_ldo(const char *what, uint32_t id, uint32_t uv, uint32_t ma)
{   return vote_uv(what, T_LDOA, id, uv, ma); }
int rpm_ldo_on(uint32_t id, uint32_t uv, uint32_t ma)
{
    if (rpm_smd_init() < 0) { vsay("rpm: channel not open\n"); return -1; }
    return vote_ldo("  sensor rail ldoa", id, uv, ma);
}
/* Corner-type regulators (msm8909w v138): key "corn", value = corner. */
#define K_CORN 0x6e726f63u  /* "corn" */
/* Exactly what mainline rpmpd.c sends for the msm8916 CX/MX power domains
 * (DEFINE_RPMPD_PAIR(msm8916, vddcx, SMPA, CORNER, 1) / (vddmx, LDOA, CORNER,
 * 3)): the "corn" key alone, no "swen", no "uv". Levels: 1 retention,
 * 3 svs_soc, 4 nominal, 5 turbo, 6 super_turbo. */
static int __attribute__((unused)) vote_corner(const char *what, uint32_t type, uint32_t id, uint32_t corner)
{
    uint32_t kv[3] = { K_CORN, 4, corner };
    int rc;
    pre(what);
    rc = rpm_smd_request(0, type, id, kv, sizeof kv);
    say_rc("", rc);
    return rc;
}
static int __attribute__((unused)) vote_level(const char *what, uint32_t id, uint32_t lvl)
{
    uint32_t kv[3] = { K_VLVL, 4, lvl };
    int rc;
    pre(what);
    rc = rpm_smd_request(0, T_SMPA, id, kv, sizeof kv);
    say_rc("", rc);
    return rc;
}
static int vote_kv(const char *what, uint32_t type, uint32_t id, uint32_t key, uint32_t val)
{
    uint32_t kv[3] = { key, 4, val };
    int rc;
    pre(what);
    rc = rpm_smd_request(0, type, id, kv, sizeof kv);
    say_rc("", rc);
    return rc;
}

/* scm_pas_enable_bw() equivalent: provision the crypto engine's path to DDR
 * so TZ can hash/verify the image at pas_init. This is the ONE thing the
 * vendor pil_init_image_trusted() does before the SCM that our firmware did
 * not -- and both watches (one that runs WiFi under Wear OS) fail pas_init
 * with -13 without it. Values from mainline drivers/interconnect/qcom:
 *   RPM_BUS_MASTER_REQ / RPM_BUS_SLAVE_REQ, key RPM_KEY_BW, value = bytes/sec.
 * Ids from the Gen 6 DT: mas-crypto mas-rpm-id 0x17 (SNOC), slv-ebi 0x00.
 * Bandwidth from the vendor scm_pas_bw_tbl (ab = 492*8*1e5 ~= 393.6 MB/s). */
#define RPM_BUS_MASTER_REQ 0x73616d62u   /* "bmas" */
#define RPM_BUS_SLAVE_REQ  0x766c7362u   /* "bslv" */
#define RPM_KEY_BW         0x00007762u   /* "bw"   */
#define CRYPTO_MAS_RPM_ID  0x17u
#define EBI_SLV_RPM_ID     0x00u
#define PAS_BW_BPS         393600000u

/* Program the crypto master's SNOC QoS. mas-crypto is qcom,ap-owned, so its
 * NoC priority/mode are set by AP register writes (the stock msm_bus driver
 * does this once at probe), NOT by an RPM vote -- which is why the master BW
 * vote returns "resource does not exist". Without it the crypto master may not
 * be admitted onto the SNOC path to DDR, so TZ's hash at pas_init fails.
 * Addresses: SNOC base 0x580000 + base-offset 0x7000 + qport0.
 *   PRIORITY @0x587008 [3:0] = (p1<<2)|p0 = 0x5   (qcom,prio1/prio0 = 1)
 *   MODE     @0x58700C [1:0] = 0 (FIXED)          (qcom,qos-mode = "fixed") */
#define SNOC_QOS_PRIO_CRYPTO 0x00587008u
#define SNOC_QOS_MODE_CRYPTO 0x0058700Cu

static void __attribute__((unused)) crypto_noc_qos(void)
{
    uint32_t p0 = mmio_read(SNOC_QOS_PRIO_CRYPTO), m0 = mmio_read(SNOC_QOS_MODE_CRYPTO);
    mmio_write(SNOC_QOS_PRIO_CRYPTO, (p0 & ~0xFu) | 0x5u);
    mmio_write(SNOC_QOS_MODE_CRYPTO, (m0 & ~0x3u));
    __asm__ volatile("dsb sy" ::: "memory");
    vsay("wcnss: crypto SNOC QoS prio ");
    con_dbg_hex(p0); con_dbg("->"); con_dbg_hex(mmio_read(SNOC_QOS_PRIO_CRYPTO));
    con_dbg(" mode "); con_dbg_hex(m0); con_dbg("->"); con_dbg_hex(mmio_read(SNOC_QOS_MODE_CRYPTO));
    con_dbg("\n"); con_flush(); usb_poll();
}

static void vote_pas_bw(void)
{
    uint32_t kv[3] = { RPM_KEY_BW, 4, PAS_BW_BPS };
    int rs = rpm_smd_request(0, RPM_BUS_SLAVE_REQ,  EBI_SLV_RPM_ID,    kv, sizeof kv);
    int rm = rpm_smd_request(0, RPM_BUS_MASTER_REQ, CRYPTO_MAS_RPM_ID, kv, sizeof kv);
    vsay("wcnss: pas bus-bw vote (crypto->EBI, ~393 MB/s)");
    con_dbg(" slave="); if (rs>=0) con_dbg("ok"); else { con_dbg("rc-"); con_dbg_dec((uint32_t)-rs); }
    con_dbg(" master="); if (rm>=0) con_dbg("ok"); else { con_dbg("rc-"); con_dbg_dec((uint32_t)-rm); }
    con_dbg("\n"); con_flush(); usb_poll();
}

/* ---- crypto engine clocks (TZ hashes the image with the CE) ------------- */
#define GCC_R(off)    mmio_read(PLAT_GCC_BASE + (off))
#define GCC_W(off, v) mmio_write(PLAT_GCC_BASE + (off), (v))
#define GCC_APCS_VOTE      0x45004u
#define CRYPTO_CMD_RCGR    0x16004u
#define CRYPTO_AHB_CBCR    0x16024u
#define CRYPTO_AXI_CBCR    0x16020u
#define CRYPTO_CBCR        0x1601Cu

static void crypto_dump(const char *when)
{
    con_dbg("wcnss: CE "); con_dbg(when);
    con_dbg(" rcg=");   con_dbg_hex(GCC_R(CRYPTO_CMD_RCGR));
    con_dbg(" cfg=");   con_dbg_hex(GCC_R(CRYPTO_CMD_RCGR + 4u));
    con_dbg(" vote=");  con_dbg_hex(GCC_R(GCC_APCS_VOTE) & 7u);
    con_dbg(" cbcr[c/ax/ah]="); con_dbg_hex(GCC_R(CRYPTO_CBCR));
    con_dbg("/"); con_dbg_hex(GCC_R(CRYPTO_AXI_CBCR));
    con_dbg("/"); con_dbg_hex(GCC_R(CRYPTO_AHB_CBCR));
    con_dbg("\n"); con_flush(); usb_poll();
}

static int crypto_clocks_on(void)
{
    uint32_t t0;
    crypto_dump("before");
    /* crypto_clk_src: GPLL0 (parent-map src 1) / 10 = 80 MHz (ftbl_crypto_clk_src
     * in gcc-sdm429w.c). hid_width=5 -> CFG div field = 2*10-1 = 19, CFG=0x113.
     * FORCE it unconditionally now (was: only if root disabled) so a stale
     * aboot rate cannot leave TZ's signature check reading a dead engine. */
    GCC_W(CRYPTO_CMD_RCGR + 4u, (1u << 8) | 19u);                 /* src=GPLL0, div=/10 */
    GCC_W(CRYPTO_CMD_RCGR, GCC_R(CRYPTO_CMD_RCGR) | 3u);          /* ROOT_EN | UPDATE */
    t0 = timer_ms();
    while ((GCC_R(CRYPTO_CMD_RCGR) & 1u) && timer_ms() - t0 < 10u) { }
    GCC_W(GCC_APCS_VOTE, GCC_R(GCC_APCS_VOTE) | 7u);              /* ahb | axi | core */
    t0 = timer_ms();
    while (((GCC_R(CRYPTO_CBCR) | GCC_R(CRYPTO_AXI_CBCR) | GCC_R(CRYPTO_AHB_CBCR)) & (1u << 31)) &&
           timer_ms() - t0 < 10u) { }
    crypto_dump("after ");
    return (GCC_R(CRYPTO_CBCR) & (1u << 31)) ? -1 : 0;
}

/* ---- Iris XO configuration (qcom_wcnss.c: wcnss_configure_iris) --------- */
#define PMU_CFG      (PLAT_PRONTO_PMU_BASE + 0x1004u)
#define PMU_SPARE    (PLAT_PRONTO_PMU_BASE + 0x1088u)
#define IRIS_REG     (PLAT_PRONTO_PMU_BASE + 0x1134u)
#define SPARE_NVBIN_DLND   (1u << 25)
#define PMU_IRIS_XO_CFG    (1u << 3)
#define PMU_IRIS_XO_EN     (1u << 4)
#define PMU_GC_BUS_MUX_TOP (1u << 5)
#define PMU_IRIS_XO_CFG_STS (1u << 6)
#define PMU_IRIS_RESET     (1u << 7)
#define PMU_IRIS_RESET_STS (1u << 8)
#define PMU_IRIS_XO_READ   (1u << 9)
#define PMU_IRIS_XO_READ_STS (1u << 10)
#define PMU_XO_MODE_MASK   (3u << 1)

static int pmu_wait_clear(uint32_t bit, uint32_t ms)
{
    uint32_t t0 = timer_ms();
    while (mmio_read(PMU_CFG) & bit) { if (timer_ms() - t0 > ms) return -1; }
    return 0;
}

static int iris_configure(void)
{
    uint32_t v, id;

    vsay_hex("wcnss: PMU_CFG before=", mmio_read(PMU_CFG)); vsay("\n");
    mmio_write(PMU_SPARE, mmio_read(PMU_SPARE) | SPARE_NVBIN_DLND);   /* "NV bin will be downloaded" */

    mmio_write(PMU_CFG, 0);
    v = PMU_GC_BUS_MUX_TOP | PMU_IRIS_XO_EN;
    mmio_write(PMU_CFG, v);

    /* Vendor extra (qcom,has-autodetect-xo): ask the Iris for its chip id.
     * Purely informational here, but it proves the 5-wire link is alive. */
    mmio_write(IRIS_REG, (mmio_read(IRIS_REG) & 0xFFFFu) | 0x04u);
    mmio_write(PMU_CFG, v | PMU_IRIS_RESET);
    if (pmu_wait_clear(PMU_IRIS_RESET_STS, 50u) < 0) { say("wcnss: iris reset never completed\n"); return -1; }
    mmio_write(PMU_CFG, v);
    mmio_write(PMU_CFG, v | PMU_IRIS_XO_READ);
    if (pmu_wait_clear(PMU_IRIS_XO_READ_STS, 50u) < 0) say("wcnss: iris read timed out\n");
    id = mmio_read(IRIS_REG);
    mmio_write(PMU_CFG, v);
    vsay_hex("wcnss: IRIS reg=", id);
    id >>= 16;
    vsay(id == 0x5111u ? " (WCN3620)\n" : id == 0x5112u ? " (WCN3620A)\n" : id == 0x0400u ? " (WCN3660B)\n"
        : id == 0x8110u ? " (WCN3615)\n" : id == 0x9101u ? " (WCN3610)\n" : " (unknown id)\n");

    /* 19.2 MHz XO (mode 0), reset Iris, run XO configuration, settle. */
    v &= ~PMU_XO_MODE_MASK;
    mmio_write(PMU_CFG, v);
    mmio_write(PMU_CFG, v | PMU_IRIS_RESET);
    if (pmu_wait_clear(PMU_IRIS_RESET_STS, 50u) < 0) { say("wcnss: iris reset (2) never completed\n"); return -1; }
    mmio_write(PMU_CFG, v);
    mmio_write(PMU_CFG, v | PMU_IRIS_XO_CFG);
    if (pmu_wait_clear(PMU_IRIS_XO_CFG_STS, 50u) < 0) { say("wcnss: XO config never completed\n"); return -1; }
    v &= ~(PMU_GC_BUS_MUX_TOP | PMU_IRIS_XO_CFG);
    mmio_write(PMU_CFG, v);
    timer_delay_ms(20);
    vsay_hex("wcnss: PMU_CFG after=", mmio_read(PMU_CFG)); vsay("\n");
    return 0;
}

/* ---- step 5: wcnss_ctrl -- the firmware's control channel ----------------
 * Once the Pronto core runs, its firmware registers SMD channels in the
 * apps<->wcnss partition's alloc table (item 13). The first thing to talk to
 * is "WCNSS_CTRL" (drivers/soc/qcom/wcnss_ctrl.c): a version request answers
 * with the firmware version, and later the NV blob goes down the same
 * channel. Message header {u32 type; u32 len}, types from 0x01000000:
 *   VERSION_REQ 0x01000000  VERSION_RESP 0x01000001
 *   DOWNLOAD_NV_REQ 0x01000002  DOWNLOAD_NV_RESP 0x01000003
 *   UPLOAD_CAL_REQ  0x01000004  UPLOAD_CAL_RESP  0x01000005
 *   DOWNLOAD_CAL_REQ 0x01000006 DOWNLOAD_CAL_RESP 0x01000007
 * Channels appear a few hundred ms after auth_and_reset (the Gen 4 v4 dump,
 * 150 ms after release, saw the table allocated but still empty). */
#define WCNSS_IPC_BIT        17u
#define WCNSS_VERSION_REQ    0x01000000u
#define WCNSS_VERSION_RESP   0x01000001u
struct smd_alloc_entry { char name[20]; uint32_t cid, flags, ref_count; };
static struct smd_chan s_ctrl;
static int s_restart;            /* a previous firmware life was shut down */

int wcnss_channel_lookup(const char *name, uint32_t *cid, uint32_t *flags)
{
    uint32_t tsz = 0, n, i;
    const volatile struct smd_alloc_entry *e = smem_get_host(SMEM_HOST_WCNSS, 13u, &tsz);
    if (!e) return -1;
    n = tsz / sizeof *e;
    for (i = 0; i < n; i++) {
        char nm[21]; uint32_t k;
        if (e[i].name[0] == 0 && e[i].cid == 0 && e[i].flags == 0) continue;
        for (k = 0; k < 20u; k++) nm[k] = e[i].name[k];
        nm[20] = 0;
        if (!strncmp(nm, name, 20u)) { *cid = e[i].cid; *flags = e[i].flags; return 0; }
    }
    return -1;
}

static int wcnss_find_channel(const char *name, uint32_t *cid, uint32_t *edge, uint32_t *count)
{
    uint32_t tsz = 0, n, i, used = 0;
    const volatile struct smd_alloc_entry *e = smem_get_host(SMEM_HOST_WCNSS, 13u, &tsz);
    int found = -1;
    if (!e) { *count = 0; return -1; }
    n = tsz / sizeof *e;
    for (i = 0; i < n; i++) {
        char nm[21]; uint32_t k;
        if (e[i].name[0] == 0 && e[i].cid == 0 && e[i].flags == 0) continue;
        used++;
        for (k = 0; k < 20u; k++) nm[k] = e[i].name[k];
        nm[20] = 0;
        if (found < 0 && !strncmp(nm, name, 20u)) { *cid = e[i].cid; *edge = e[i].flags & 0xFFu; found = 0; }
    }
    *count = used;
    return found;
}

void wcnss_list_channels(void)
{
    uint32_t tsz = 0, n, i;
    const volatile struct smd_alloc_entry *e = smem_get_host(SMEM_HOST_WCNSS, 13u, &tsz);
    if (!e) return;
    n = tsz / sizeof *e;
    for (i = 0; i < n; i++) {
        uint32_t k;
        if (e[i].name[0] == 0 && e[i].cid == 0 && e[i].flags == 0) continue;
        con_dbg("    cid "); con_dbg_dec(e[i].cid); con_dbg(" edge "); con_dbg_dec(e[i].flags & 0xFFu);
        con_dbg((e[i].flags & 0x200u) ? " pkt  \"" : " strm \"");
        for (k = 0; k < 20u && e[i].name[k]; k++) con_dbg_c(e[i].name[k]);
        con_dbg("\"\n");
    }
    con_flush(); usb_poll();
}

/* NV download (vendor wcnss_wlan.c wcnss_nv_download_work / mainline
 * wcnss_ctrl.c wcnss_download_nv). The blob is the per-device
 * /persist/WCNSS_qcom_wlan_nv.bin, embedded at build time by
 * firmware/<board>/wcnss_nv.c (generated, gitignored); the symbols are weak
 * so a board without the file just skips this step. Format on the wire:
 *   { u32 type = DOWNLOAD_NV_REQ; u32 len = 12 + n; u16 seq; u16 last; u8 data[n] }
 * n = 3072 per fragment; the firmware acks the LAST fragment with
 * DOWNLOAD_NV_RESP { hdr; u8 status } (0 = ok). Per the vendor driver the
 * first 4 bytes of the file are a validity bitmap and are NOT sent. */
extern const uint8_t  wcnss_nv_bin[] __attribute__((weak));
extern const uint32_t wcnss_nv_len   __attribute__((weak));
#define WCNSS_DOWNLOAD_NV_REQ  0x01000002u
#define WCNSS_DOWNLOAD_NV_RESP 0x01000003u
#define NV_FRAGMENT_SIZE       3072u

#define WCNSS_CBC_COMPLETE_IND 0x0100000Cu
static int wcnss_nv_download(void)
{
    static uint32_t frag[(16u + NV_FRAGMENT_SIZE) / 4u];
    uint32_t rsp[16], off = 0, seq = 0, total, got, t, i, st = 0xFFu;

    if (!wcnss_nv_bin || !wcnss_nv_len) { say("wcnss: no NV blob embedded for this board -- skipping NV download\n"); return 0; }
    total = wcnss_nv_len;
    vsay_hex("wcnss: NV download, ", total); vsay_hex(" B in fragments of ", NV_FRAGMENT_SIZE);
    vsay_hex(", tx fifo ", s_ctrl.fifo_size); vsay("\n");
    if (s_ctrl.fifo_size <= 16u + NV_FRAGMENT_SIZE + 20u) { say("wcnss: WCNSS_CTRL fifo too small for a 3 KB fragment\n"); return -1; }
    /* mainline wcnss_download_nv(): whole file from byte 0, header
     * { type, len, u16 seq, u16 last, u32 frag_size } = 16 bytes. v7-v9 sent
     * a 12-byte header (no frag_size) and skipped 4 bytes; the firmware
     * still answered 2 = ACK_COLD_BOOTING, i.e. accepted. */
    while (off < total) {
        uint32_t n = total - off, last, wire;
        if (n > NV_FRAGMENT_SIZE) n = NV_FRAGMENT_SIZE;
        last = (off + n >= total);
        frag[0] = WCNSS_DOWNLOAD_NV_REQ;
        frag[1] = 16u + n;
        frag[2] = (seq & 0xFFFFu) | (last << 16);
        frag[3] = n;
        memcpy(&frag[4], wcnss_nv_bin + off, n);
        wire = 16u + n;                       /* exact length: byte-layout channel */
        for (t = timer_ms(); smd_send(&s_ctrl, frag, wire) < 0; ) {
            if (timer_ms() - t > 1000u) { say_hex("wcnss: NV fragment did not fit after 1 s, seq ", seq); say("\n"); return -1; }
            timer_delay_ms(1);
        }
        off += n; seq++;
    }
    vsay_hex("wcnss: sent ", seq); vsay(" fragments, waiting for DOWNLOAD_NV_RESP ... ");
    for (i = 0; i < 8u; i++) {
        got = smd_recv(&s_ctrl, rsp, sizeof rsp, 5000u);
        if (got < 8u) { say("no reply within 5 s\n"); return -1; }
        if (rsp[0] == WCNSS_DOWNLOAD_NV_RESP) { st = got >= 9u ? (rsp[2] & 0xFFu) : 0xFFu; break; }
        vsay_hex("\nwcnss:   other msg type=", rsp[0]); vsay_hex(" len=", rsp[1]); vsay(" ... ");
    }
    vsay_hex("status ", st);
    if (st == 1u) { say(" = ACK_DONE_BOOTING -- *** NV ACCEPTED ***\n"); return 0; }
    if (st != 2u) { say(" -- REJECTED\n"); return -1; }
    say(" = ACK_COLD_BOOTING -- *** NV ACCEPTED ***, waiting for CBC_COMPLETE_IND (cold-boot calibration) ... ");
    for (i = 0; i < 8u; i++) {
        got = smd_recv(&s_ctrl, rsp, sizeof rsp, 10000u);
        if (got < 8u) { vsay("none within 10 s (continuing anyway)\n"); return 0; }
        if (rsp[0] == WCNSS_CBC_COMPLETE_IND) { say("*** calibration COMPLETE ***\n"); return 0; }
        vsay_hex("\nwcnss:   other msg type=", rsp[0]); vsay_hex(" len=", rsp[1]); vsay(" ... ");
    }
    vsay("\n"); return 0;
}

/* ---- step 6: wcn36xx HAL over WLAN_CTRL -----------------------------------
 * The WLAN firmware speaks the "HAL" protocol (drivers/net/wireless/ath/
 * wcn36xx/hal.h + smd.c) on the WLAN_CTRL channel. Header:
 *   { u16 msg_type; u16 msg_version; u32 len (incl. header) }
 * Order in wcn36xx_start(): DOWNLOAD_NV_REQ (55) fragments -- the same NV
 * file, table from byte 4 (struct nv_data { int is_valid; u8 table; }),
 * one DOWNLOAD_NV_RSP (56, u32 status) per fragment -- then START_REQ (0):
 * { hdr; u32 driver_type = 0 production; u32 cfg_len; TLVs { u16 id; u16 len
 * = 4; u16 pad; u16 rsvd; u32 val } x45 (wcn36xx_cfg_vals, generated into
 * wcn36xx_cfg.h) }. START_RSP (1): { hdr; u16 status; u8 stations; u8 bssids;
 * u8 rev, ver, minor, major; char crm[64]; char wlan[64] }. All of this is
 * pure control-channel traffic, before any DXE (DMA) setup. */
#include "wcn36xx_cfg.h"
#define HAL_START_REQ        0u
#define HAL_START_RSP        1u
#define HAL_DOWNLOAD_NV_REQ  55u
#define HAL_DOWNLOAD_NV_RSP  56u
static struct smd_chan s_wlan;
static uint32_t s_hal_buf[1024];      /* 4 KB, WCN36XX_HAL_BUF_SIZE */

/* Send one HAL message and wait for the reply of type `want`; other
 * messages (indications) are printed and skipped. Returns payload bytes. */
static uint32_t hal_send_wait(const void *msg, uint32_t len, uint32_t want, uint32_t *rsp, uint32_t max, uint32_t ms)
{
    uint32_t got, i, t;
    for (t = timer_ms(); smd_send(&s_wlan, msg, len) < 0; ) {      /* exact length: byte-layout channel */
        if (timer_ms() - t > 1000u) { say("wcnss: WLAN_CTRL send did not fit\n"); return 0; }
        timer_delay_ms(1);
    }
    for (i = 0; i < 16u; i++) {
        got = smd_recv(&s_wlan, rsp, max, ms);
        if (got < 8u) return 0;
        if ((rsp[0] & 0xFFFFu) == want) return got;
        vsay_hex("\nwcnss:   hal ind type=", rsp[0] & 0xFFFFu); vsay_hex(" len=", rsp[1]); vsay(" ");
    }
    return 0;
}

static int wlan_hal_load_nv(void)
{
    uint32_t off = 4u, seq = 0, got;
    if (!wcnss_nv_bin || !wcnss_nv_len) { say("wcnss: no NV blob -- skipping HAL NV load\n"); return 0; }
    vsay("wcnss: HAL DOWNLOAD_NV_REQ ... ");
    while (off < wcnss_nv_len) {
        uint32_t n = wcnss_nv_len - off, last;
        if (n > 3072u) n = 3072u;
        last = (off + n >= wcnss_nv_len);
        s_hal_buf[0] = HAL_DOWNLOAD_NV_REQ;          /* type 55, version 0 */
        s_hal_buf[1] = 16u + n;
        s_hal_buf[2] = (seq & 0xFFFFu) | (last << 16);
        s_hal_buf[3] = n;
        memcpy(&s_hal_buf[4], wcnss_nv_bin + off, n);
        got = hal_send_wait(s_hal_buf, 16u + n, HAL_DOWNLOAD_NV_RSP, s_hal_buf, sizeof s_hal_buf, last ? 15000u : 3000u);
        if (got < 12u) {
            vsay_hex("no DOWNLOAD_NV_RSP for fragment ", seq); vsay_hex(" (remote state ", smd_remote_state(&s_wlan)); vsay(")\n");
            return -1;
        }
        if (s_hal_buf[2] != 0u) { say_hex("fragment ", seq); say_hex(" status ", s_hal_buf[2]); say(" -- FAILED\n"); return -1; }
        off += n; seq++;
    }
    vsay_hex("", seq); vsay(" fragments, every one acknowledged -- *** HAL NV LOADED ***\n");
    return 0;
}

static int s_hal_up;            /* MAC started (HAL_START answered) */

/* Load NV + HAL_START + DXE: everything wcn36xx_start() does on ifup. Reused
 * for the light "radio on" after a HAL_STOP, without touching the firmware. */
static int wlan_hal_mac_start(void)
{
    uint32_t got, i, len;
    uint8_t *b = (uint8_t *)s_hal_buf;

    if (wlan_hal_load_nv() < 0) return -1;

    /* START_REQ + the 45 config TLVs */
    s_hal_buf[0] = HAL_START_REQ; s_hal_buf[2] = 0u /* DRIVER_TYPE_PRODUCTION */;
    len = 16u;
    for (i = 0; i < sizeof k_wcn36xx_cfg / sizeof k_wcn36xx_cfg[0]; i++) {
        uint16_t id = k_wcn36xx_cfg[i].id, l4 = 4u, z = 0;
        uint32_t v = k_wcn36xx_cfg[i].val;
        memcpy(b + len, &id, 2); memcpy(b + len + 2, &l4, 2); memcpy(b + len + 4, &z, 2); memcpy(b + len + 6, &z, 2);
        memcpy(b + len + 8, &v, 4);
        len += 12u;
    }
    s_hal_buf[1] = len; s_hal_buf[3] = len - 16u;
    pre(""); vsay_hex("wcnss: HAL_START_REQ (", i); vsay_hex(" cfg TLVs, ", len); vsay(" B) ... ");
    got = hal_send_wait(s_hal_buf, len, HAL_START_RSP, s_hal_buf, sizeof s_hal_buf, 5000u);
    if (got < 8u) { say("no HAL_START_RSP within 5 s\n"); return -1; }
    {
        uint16_t st; memcpy(&st, b + 8, 2);
        vsay_hex("status=", st); vsay_hex(" stations=", b[10]); vsay_hex(" bssids=", b[11]);
        vsay(" api "); con_dbg_dec(b[15]); con_dbg("."); con_dbg_dec(b[14]); con_dbg("."); con_dbg_dec(b[13]); con_dbg("."); con_dbg_dec(b[12]);
        if (got >= 16u + 128u) {
            uint32_t k;
            vsay("\nwcnss:   crm  \""); for (k = 0; k < 64u && b[16 + k]; k++) con_dbg_c((char)b[16 + k]);
            vsay("\"\nwcnss:   wlan \""); for (k = 0; k < 64u && b[80 + k]; k++) con_dbg_c((char)b[80 + k]); vsay("\"");
        }
        say(st == 0 ? "\nwcnss: *** WLAN HAL STARTED ***\n" : "\nwcnss: HAL start refused\n");
        if (st != 0) return -1;
    }
    /* step 6b: the DXE rings, so frames can flow */
    wdog_extend(31u); deadman_kick();
    if (wcn36xx_dxe_init() < 0) return -1;
    s_hal_up = 1;
    return 0;
}

static int wlan_hal_start(void)
{
    uint32_t cid = 0, edge = 0, count = 0;
    if (wcnss_find_channel("WLAN_CTRL", &cid, &edge, &count) < 0) { say("wcnss: no WLAN_CTRL channel\n"); return -1; }
    vsay_hex("wcnss: opening WLAN_CTRL (cid ", cid); vsay(") ... ");
    if (smd_open(&s_wlan, SMEM_HOST_WCNSS, cid, WCNSS_IPC_BIT) < 0) { say("FAILED\n"); return -1; }
    vsay_hex("open, fifo ", s_wlan.fifo_size); vsay("\n");
    return wlan_hal_mac_start();
}

static int wcnss_ctrl_handshake(void)
{
    uint32_t t0 = timer_ms(), cid = 0, edge = 0, count = 0, last = 0xFFFFFFFFu, got, i;
    uint32_t req[2] = { WCNSS_VERSION_REQ, 8u };
    uint32_t rsp[64];

    /* 10. wait for the firmware to register its channels (up to 10 s) */
    /* RESTART (2026-09-07): after pas_shutdown the alloc table and the old
     * firmware's channel state words survive in SMEM, so the lookup below
     * returns at once and smd_open finds the remote "already OPENED" -- while
     * the new firmware is still in cold-boot calibration. That produced the
     * alternating "NV fragment did not fit" / "remote never entered opening"
     * failures on the first bring-ups after a sleep. The new firmware resets
     * its side of WCNSS_CTRL when it starts: wait for that edge. */
    if (s_restart && s_ctrl.rx) {
        uint32_t tw = timer_ms(), st0 = smd_remote_state(&s_ctrl), st = st0;
        while (timer_ms() - tw < 8000u) { st = smd_remote_state(&s_ctrl); if (st != 2u) break; timer_delay_ms(2); }
        con_puts("wcnss: restart: WCNSS_CTRL remote state "); con_putdec(st0);
        con_puts(" -> "); con_putdec(st); con_puts(" after "); con_putdec(timer_ms() - tw); con_puts(" ms\n");
        if (st == 2u) con_puts("wcnss: restart: remote never reset its channel (stale SMEM?)\n");
        timer_delay_ms(50);
    }
    vsay("wcnss: waiting for the firmware's SMD channels ...\n");
    while (timer_ms() - t0 < 10000u) {
        int f = wcnss_find_channel("WCNSS_CTRL", &cid, &edge, &count);
        if (count != last) {
            last = count;
            vsay_hex("wcnss: +", timer_ms() - t0); vsay_hex(" ms: ", count); vsay(" channel(s)\n");
            wcnss_list_channels();
        }
        if (f == 0) break;
        timer_delay_ms(20);
    }
    if (last == 0xFFFFFFFFu || wcnss_find_channel("WCNSS_CTRL", &cid, &edge, &count) < 0) {
        say("wcnss: no WCNSS_CTRL channel after 10 s\n"); return -1;
    }

    /* 11. open it and ask for the firmware version */
    vsay_hex("wcnss: opening WCNSS_CTRL (cid ", cid); vsay_hex(", edge ", edge); vsay(") ... ");
    if (smd_open(&s_ctrl, SMEM_HOST_WCNSS, cid, WCNSS_IPC_BIT) < 0) { say("FAILED\n"); return -1; }
    vsay("open\n");
    pre(""); vsay("wcnss: WCNSS_VERSION_REQ ... ");
    if (smd_send(&s_ctrl, req, sizeof req) < 0) { say("send failed\n"); return -1; }
    /* the firmware may send other messages first; print each for 3 s */
    for (i = 0; i < 8u; i++) {
        got = smd_recv(&s_ctrl, rsp, sizeof rsp, 3000u);
        if (got < 8u) { say("no reply within 3 s\n"); return -1; }
        vsay_hex("\nwcnss:   msg type=", rsp[0]); vsay_hex(" len=", rsp[1]); vsay_hex(" got=", got);
        if (rsp[0] == WCNSS_VERSION_RESP && got >= 12u) {
            uint8_t *v = (uint8_t *)&rsp[2];
            say("\nwcnss: *** firmware version "); con_putdec(v[0]); con_puts("."); con_putdec(v[1]);
            con_dbg("."); con_dbg_dec(v[2]); con_dbg("."); con_dbg_dec(v[3]);
            vsay(" -- WCNSS_CTRL handshake COMPLETE ***\n");
            if (wcnss_nv_download() < 0) return -1;
            return wlan_hal_start();
        }
    }
    say("\nwcnss: no VERSION_RESP among the replies\n");
    return -1;
}

/* ---- the boot ----------------------------------------------------------- */
static uint32_t s_seg_dest;   /* sink_seg ctx */

int wcnss_boot(void)
{
    const struct elf32_phdr *ph;
    uint32_t phoff, phnum, i, lo = 0xFFFFFFFFu, hi = 0, total = 0, size, reloc;
    int rc, relocatable = 0;
    char name[12] = "WCNSS   BXX";

    s_t0 = timer_ms();
#if defined(NO_RADIO_BOOT)
    /* Ghost-master test (2026-09-08): never start WCNSS, so no radio-side RPM
     * votes ever exist; the sleep census then shows whether l9/s3 and the
     * active-set floor belong to a PAS-killed Pronto session. */
    say("wcnss: NO_RADIO_BOOT build - radio never started\n");
    return -1;
#endif
    if (!smem_ok() || rpm_smd_init() < 0) { say("wcnss: SMEM/RPM not up\n"); return -1; }

    /* 1. crypto clocks */
    rc = crypto_clocks_on();
    say_rc("wcnss: crypto clocks", rc);
    if (rc < 0) return -1;

    if (fat_init() < 0) return -1;
    if (fat_read_file("WCNSS   MDT", &s_mdt_len, sink_mdt, 0) < 0 || s_mdt_len > 0x2000u) {
        say("wcnss: cannot read IMAGE/WCNSS.MDT\n"); return -1;
    }
    if (s_mdt[0] != 0x7Fu || s_mdt[1] != 'E' || s_mdt[2] != 'L' || s_mdt[3] != 'F') { say("wcnss: MDT is not ELF\n"); return -1; }
    { uint8_t hdr[64]; uint32_t i; for (i = 0; i < 64u; i++) hdr[i] = s_mdt[i];
      phoff = rd32(hdr + 28); phnum = rd16(hdr + 44); }
    if (phnum > 32u || phoff + phnum * sizeof(struct elf32_phdr) > s_mdt_len) { say("wcnss: MDT phdrs out of range\n"); return -1; }
    /* Copy the program-header table out of the uncached buffer to parse it. */
    { uint32_t i; volatile uint8_t *sp = s_mdt + phoff; uint8_t *dp = (uint8_t *)s_phbuf;
      for (i = 0; i < phnum * sizeof(struct elf32_phdr); i++) dp[i] = sp[i]; }
    ph = s_phbuf;
    for (i = 0; i < phnum; i++) {
        if (!seg_loadable(&ph[i])) continue;
        if (ph[i].p_flags & MDT_RELOCATABLE) relocatable = 1;
        if (ph[i].p_paddr < lo) lo = ph[i].p_paddr;
        if (ph[i].p_paddr + ph[i].p_memsz > hi) hi = ph[i].p_paddr + ph[i].p_memsz;
        total += ph[i].p_memsz;
    }
    hi = (hi + 0xFFFu) & ~0xFFFu;
    size = hi - lo;
    reloc = relocatable ? PLAT_WCNSS_FW_BASE - lo : 0;
    vsay_hex("wcnss: mdt ", s_mdt_len); vsay_hex(" B, segs span ", lo); vsay_hex("..", hi);
    vsay_hex(" -> region ", lo + reloc); vsay_hex(" size ", size); vsay(relocatable ? " (relocatable)\n" : " (fixed)\n");
    if (size > PLAT_WCNSS_FW_SIZE || lo + reloc < PLAT_WCNSS_FW_BASE ||
        lo + reloc + size > PLAT_WCNSS_FW_BASE + PLAT_WCNSS_FW_SIZE) {
        say("wcnss: image does not fit the firmware region\n"); return -1;
    }

    /* 5. rails, clocks, pins, Iris -- BEFORE init_image, exactly where the
     * vendor PIL's proxy vote runs (pil_boot: proxy_vote -> init_image).
     * TZ hashes the metadata through the crypto engine's DMA, so the bus
     * paths and the subsystem's rails must be up before it is asked to. */
    vsay("wcnss: voting rails\n");
#if defined(PLAT_WCNSS_RAILS_PM660)
    /* Gen 6 (hoki DT): pronto vddmx s2_level_ao, vddcx s1_level, vddpx l13;
     * iris vddxo l12, vddrfa l5, vdddig l13; clocks xo + rf_clk2 + snoc. */
    vote_level("  vddcx  pm660_s1 level 384", 1u, RPM_LEVEL_TURBO);
    vote_level("  vddmx  pm660_s2 level 384", 2u, RPM_LEVEL_TURBO);
    vote_ldo  ("  vddpx  pm660_l13 1.8V",     13u, 1800000u, 10u);
    vote_ldo  ("  vddxo  pm660_l12 1.8V",     12u, 1800000u, 10u);
    vote_ldo  ("  vddrfa pm660_l5  1.3V",      5u, 1300000u, 100u);
    vote_kv   ("  cxo (clk0 id0 Enab)",        T_CLK0, 0u, K_ENAB, 1u);
    vote_kv   ("  rf_clk1 (clka id4 swen)",    T_CLKA, 4u, K_SWEN, 1u);
#elif defined(PLAT_WCNSS_RAILS_PM8916)
    /* msm8909w + PM8916 (firefish-stock.dts / skipjack.dts, identical):
     *   pronto vddmx 8916_l3_corner_ao   vddcx 8916_s1_corner   vddpx 8916_l7
     *   iris   vddxo 8916_l7 1.8 V       vddrfa 8916_s3 1.3 V   vddpa 8916_l9 3.3 V
     *          vdddig 8916_l5 1.8 V      pil proxy vdd_pronto_pll = l7
     * Corner regulators take the "corn" key (rpm-regulator-smd.c): the value
     * sent is the enum minus RPM_REGULATOR_CORNER_NONE, so 5 = TURBO; the
     * vendor wcnss driver asks for NORMAL..SUPER_TURBO, so any value >= 4 is
     * inside what it would have voted. Currents from the iris-*-current
     * properties (0x2710 = 10 mA, 0x186a0 = 100 mA, 0x7dbb8 = 515 mA). */
    /* Order (v2): the plain LDOs first -- they are the same request shape
     * the Gen 6 has sent hundreds of times -- then the corner domains, mx
     * before cx as the vendor wcnss_vreg.c and mainline both do. v1 reset
     * the SoC inside its first vote (cx corner, with "swen"); the blackbox
     * replay at the next boot names the exact line. */
    vote_ldo   ("  vddpx/vddxo/pll 8916_l7 1.8V",  7u, 1800000u, 10u);
    vote_ldo   ("  vdddig 8916_l5 1.8V",            5u, 1800000u, 10u);
    vote_ldo   ("  vddpa  8916_l9 3.3V",            9u, 3300000u, 515u);
    vote_uv    ("  vddrfa 8916_s3 1.3V (smpa3)",    T_SMPA, 3u, 1300000u, 100u);
#if defined(WCNSS_CORNER_VOTES)
    vote_corner("  vddmx  8916_l3 corner 5 (ldoa3 corn only)", T_LDOA, 3u, 5u);
    vote_corner("  vddcx  8916_s1 corner 5 (smpa1 corn only)", T_SMPA, 1u, 5u);
#else
    /* v3: SKIPPED. v1 reset at the first corner vote (cx, corn+swen) and v2
     * at the first corner vote again (mx, corn only) after four LDO votes
     * succeeded. The RPM already keeps cx/mx at the level the running CPU
     * needs, which is enough for a first PAS attempt; re-enable with
     * -DWCNSS_CORNER_VOTES once the reset is understood. */
    vsay("  vddmx/vddcx corner votes: skipped in this build (-DWCNSS_CORNER_VOTES enables)\n");
#endif
    vote_kv    ("  cxo (clk0 id0 Enab)",            T_CLK0, 0u, K_ENAB, 1u);
    /* The iris clock is rf_clk2 on msm8916 (RPM_SMD_RF_CLK2 = clka id 5);
     * rf_clk1 is voted too -- an extra XO buffer on is harmless, a missing
     * one is a dead radio, and the 8909 rpm clock hash cannot be resolved
     * from the DT alone. */
    vote_kv    ("  rf_clk2 (clka id5 swen)",        T_CLKA, 5u, K_SWEN, 1u);
    vote_kv    ("  rf_clk1 (clka id4 swen)",        T_CLKA, 4u, K_SWEN, 1u);
#else
#error "no PLAT_WCNSS_RAILS_* table for this board"
#endif
    vote_kv   ("  snoc 200 MHz (clk1 id1 KHz)", T_CLK1, 1u, K_KHZ, 200000u);
    vote_kv   ("  pcnoc 100 MHz (clk1 id0 KHz)", T_CLK1, 0u, K_KHZ, 100000u);
    timer_delay_ms(5);

    /* 6. 5-wire pins */
    for (i = 0; i < 5u; i++) tlmm_cfg(PLAT_WCNSS_GPIO_FIRST + i, PLAT_WCNSS_GPIO_FUNC, 3u /* pull-up */, 6u, 0);
    vsay_hex("wcnss: TLMM ", PLAT_WCNSS_GPIO_FIRST); vsay_hex("..", PLAT_WCNSS_GPIO_FIRST + 4u); vsay(" -> wcss_wlan\n");

    /* 7. Iris. First touch of the Pronto PMU block. */
    pre(""); vsay("wcnss: configuring Iris XO (first PMU access) ...\n"); timer_delay_ms(50);
    if (iris_configure() < 0) return -1;


    pre(""); vsay("wcnss: pas_is_supported ... "); timer_delay_ms(30);
    say_rc("", scm_pas_is_supported(PLAT_WCNSS_PAS_ID));

    /* The garbage-pointer probe (seq v4) proved TZ reads and PARSES our
     * metadata (valid ELF -> -13, non-ELF -> -2), and seq v5 proved the
     * crypto engine is clocked. Since this same TZ authenticates this same
     * signed image for stock Wear OS, -13 on a valid image is most likely
     * "pas-id already initialized": the last stock session PAS-loaded WCNSS
     * and a warm `fastboot reboot` did not clear TZ's subsystem state. Tear
     * down any existing WCNSS ownership first; on a clean boot this simply
     * returns an error we ignore. */
    pre(""); vsay("wcnss: pas_shutdown(6) first (clears any stale TZ ownership) ... ");
    { int sd = scm_pas_shutdown(PLAT_WCNSS_PAS_ID); say_rc("", sd); }
    timer_delay_ms(50);

    /* cached HLOS buffer: flush our writes to DDR so TZ reads current bytes */
    cache_clean(s_mdt, s_mdt_len);
    __asm__ volatile("dsb sy" ::: "memory");
    vsay_hex("wcnss: metadata @", WCNSS_MDT_SCRATCH);
    vsay_hex(" magic=", (uint32_t)s_mdt[0] | ((uint32_t)s_mdt[1]<<8) | ((uint32_t)s_mdt[2]<<16) | ((uint32_t)s_mdt[3]<<24));
    vsay_hex(" len=", s_mdt_len); vsay("\n");
    /* NOTE: crypto_noc_qos() removed -- writing SNOC config space (0x580000)
     * from bare-metal faults and reboots the watch (the NoC bus_clk the stock
     * msm_bus driver enables at probe is not up). It never helped the -13. */
    vote_pas_bw();      /* scm_pas_enable_bw() equivalent -- crypto path to DDR */
    timer_delay_ms(5);
#if !defined(PLAT_WCNSS_INIT_VARIANTS)
    /* Plain loader path (msm8909w): one init_image, TZ's own log on failure. */
    vsay("wcnss: pas_init_image ("); vsay(scm_convention_name()); vsay_hex(") metadata @", WCNSS_MDT_SCRATCH); vsay(" ... ");
    timer_delay_ms(30);
    rc = scm_pas_init_image(PLAT_WCNSS_PAS_ID, WCNSS_MDT_SCRATCH);
    say_rc("", rc);
    tz_log_tail(240u);
    if (rc < 0) {
        say("wcnss: init_image rejected -- full TZ diag log:\n");
        tz_log_dump();
        return -1;
    }
#else
    /* ---- seq v17: four discriminating init_image experiments -------------
     * The TZ diag log (seq v15) showed our call logs only its entry
     * "(3000065 6)" and then fails: no metadata mapping, no crypto-engine
     * entries. So TZ rejects something it reads from the ELF HEADER, before
     * hashing. Same-address non-ELF content gives -2 (so the address itself
     * is accepted). Candidates, each isolated below, TZ log tail after each:
     *   E1  the buffer as-is                (baseline, expect -13)
     *   E2  same bytes at 0xA0000000        NS DDR far from our image and
     *                                       every carve-out: is it placement?
     *   E4  p_paddr of every loadable phdr  is -13 a segment-range check
     *       rebased into the wcnss region   against the pas-6 region? (this
     *                                       breaks the signature, so a NEW
     *                                       error code here is the tell)
     *   E7  ELF header + phdrs only, hash   is -13 raised before the hash
     *       segment zeroed                  segment is even looked at?     */
    {
        static uint8_t s_alt[0x2000] __attribute__((aligned(64)));
        uint32_t k, hdrsz = phoff + phnum * (uint32_t)sizeof(struct elf32_phdr);
        volatile uint8_t *far = (volatile uint8_t *)0xA0000000u;
        int e;

        for (k = 0; k < s_mdt_len; k++) far[k] = s_mdt[k];
        cache_clean((const void *)(uintptr_t)0xA0000000u, s_mdt_len);

        /* seq v18: ONE variant per boot. v17 showed that after the first
         * failed init_image even pas_shutdown(6) returns -13 (it returned 0
         * before), so every later attempt in the same boot is measured in a
         * poisoned TZ state and proves nothing. The variant rotates on the
         * superblock boot counter: reboot four times, get four clean results. */
        e = (int)(storage_boot_count() % 4u);
        vsay_hex("wcnss: boot #", storage_boot_count()); vsay_hex(" -> variant index ", (uint32_t)e); vsay("\n");
        for (; e < 4; e++) {
            uint32_t phys;
            const char *what;
            if (e == 0)      { what = "E1 buffer as-is (.bss)";            phys = WCNSS_MDT_SCRATCH; }
            else if (e == 1) { what = "E2 same bytes @0xA0000000";         phys = 0xA0000000u; }
            else if (e == 2) {
                what = "E4 phdr p_paddr rebased into wcnss region";
                memcpy(s_alt, s_mdt, s_mdt_len);
                for (k = 0; k < phnum; k++) {
                    struct elf32_phdr *p = (struct elf32_phdr *)(s_alt + phoff) + k;
                    if (seg_loadable(p)) p->p_paddr += reloc;
                }
                cache_clean(s_alt, s_mdt_len);
                phys = (uint32_t)(uintptr_t)s_alt;
            } else {
                what = "E7 ELF header+phdrs only, hash segment zeroed";
                memcpy(s_alt, s_mdt, hdrsz);
                memset(s_alt + hdrsz, 0, s_mdt_len - hdrsz);
                cache_clean(s_alt, s_mdt_len);
                phys = (uint32_t)(uintptr_t)s_alt;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            scm_pas_shutdown(PLAT_WCNSS_PAS_ID);          /* clean state each time */
            timer_delay_ms(20);
            vsay("wcnss: "); vsay(what); vsay_hex(" @", phys); vsay(" ... "); timer_delay_ms(30);
            rc = scm_pas_init_image(PLAT_WCNSS_PAS_ID, phys);
            say_rc("", rc);
            tz_log_tail(240u);
            if (rc == 0) { vsay("wcnss: *** init_image ACCEPTED with this variant ***\n"); break; }
            /* State probe after the failure: which PAS calls still work? */
            vsay("wcnss:   after failure: is_supported ");
            say_rc("", scm_pas_is_supported(PLAT_WCNSS_PAS_ID));
            vsay("wcnss:   after failure: shutdown ");
            say_rc("", scm_pas_shutdown(PLAT_WCNSS_PAS_ID));
            vsay("wcnss:   after failure: shutdown again ");
            say_rc("", scm_pas_shutdown(PLAT_WCNSS_PAS_ID));
            break;   /* one clean attempt per boot -- see the note above */
        }
        if (rc < 0) {
            say("wcnss: all init_image variants rejected -- full TZ diag log:\n");
            tz_log_dump();
            return -1;
        }
    }
#endif /* PLAT_WCNSS_INIT_VARIANTS */

    /* 3. tell TZ where the image goes */
    pre(""); vsay("wcnss: pas_mem_setup ... "); timer_delay_ms(30);
    rc = scm_pas_mem_setup(PLAT_WCNSS_PAS_ID, lo + reloc, size);
    say_rc("", rc);
    if (rc < 0) return -1;

    /* 4. segments */
    for (i = 0; i < phnum; i++) {
        uint32_t fsz;
        if (!seg_loadable(&ph[i])) continue;
        s_seg_dest = ph[i].p_paddr + reloc;
        if (ph[i].p_filesz) {
            name[9] = (char)('0' + i / 10u); name[10] = (char)('0' + i % 10u);
            if (fat_read_file(name, &fsz, sink_seg, (void *)(uintptr_t)s_seg_dest) < 0) {
                say("wcnss: cannot read "); say(name); say("\n"); return -1;
            }
            if (fsz != ph[i].p_filesz) { say("wcnss: segment size mismatch "); say(name); say("\n"); return -1; }
        }
        if (ph[i].p_memsz > ph[i].p_filesz)
            region_zero(s_seg_dest + ph[i].p_filesz, ph[i].p_memsz - ph[i].p_filesz);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    vsay_hex("wcnss: segments loaded, ", total); vsay(" B\n");

    /* 8. GO */
    pre(""); vsay("wcnss: pas_auth_and_reset ... "); timer_delay_ms(50);
    rc = scm_pas_auth_and_reset(PLAT_WCNSS_PAS_ID);
    say_rc("", rc);
    if (rc < 0) return -1;

    /* 9. proof of life: the firmware allocates its SMD channel table */
    {
        uint32_t t0 = timer_ms(), tsz;
        const void *tbl = 0;
        while (timer_ms() - t0 < 5000u) {
            tbl = smem_get_host(SMEM_HOST_WCNSS, 13u, &tsz);
            if (tbl) break;
            timer_delay_ms(10);
        }
        vsay_hex("wcnss: waited ", timer_ms() - t0); vsay(" ms for the SMD table: ");
        if (!tbl) { say("NOT there - WCNSS did not come up\n"); return -1; }
        vsay("PRESENT - WCNSS is running\n");
    }
    return wcnss_ctrl_handshake();
}

uint32_t wlan_hal_xfer(const void *msg, uint32_t len, uint32_t want, uint32_t *rsp, uint32_t max, uint32_t ms)
{   return hal_send_wait(msg, len, want, rsp, max, ms); }

extern const uint8_t wcnss_mac[6] __attribute__((weak));
const uint8_t *wlan_mac(void)
{
    static const uint8_t fallback[6] = { 0x02, 0x00, 0x5e, 0x00, 0x00, 0x01 };   /* locally administered */
    return wcnss_mac ? wcnss_mac : fallback;
}

/* ---- the radio as the app sees it ------------------------------------- */
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
static SemaphoreHandle_t s_wlan_lock;
void wlan_lock(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return;
    if (!s_wlan_lock) s_wlan_lock = xSemaphoreCreateRecursiveMutex();
    if (s_wlan_lock) xSemaphoreTakeRecursive(s_wlan_lock, portMAX_DELAY);
}
void wlan_unlock(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return;
    if (s_wlan_lock) xSemaphoreGiveRecursive(s_wlan_lock);
}
static int s_wlan_up;
#define HAL_STOP_REQ 2u
#define HAL_STOP_RSP 3u

int wlan_is_up(void) { return s_wlan_up; }
int wcnss_fw_resident(void) { return s_wlan_up; }

/* wlan_up/down are the app's "radio on/off". They are LIGHT: on = load the
 * firmware once (PAS) and start the MAC; off = HAL_STOP only, firmware stays
 * resident. The app turns WiFi off after every scan and on every sleep entry,
 * so a full PAS reload each time (6 s, and a path never exercised) is neither
 * affordable nor proven. wlan_power_off() is the real shutdown. */
int wlan_up(void)
{
    int rc;
    if (s_wlan_up && s_hal_up) return 0;
    if (s_wlan_up) {                          /* firmware resident, MAC stopped */
        vsay("wlan: restarting the MAC\n");
        wdog_extend(31u); deadman_kick();
        rc = wlan_hal_mac_start();
        say(rc == 0 ? "wlan: radio UP (MAC restarted)\n" : "wlan: MAC restart FAILED\n");
        return rc;
    }
    if (!smem_ok() && smem_init() != 0) { vsay("wlan: SMEM not available\n"); return -1; }
    say("wlan: bringing the radio up\n");
    wdog_extend(31u); deadman_kick();
    rc = wcnss_boot();
    wdog_extend(31u); deadman_kick();
    say(rc == 0 ? "wlan: radio UP\n" : "wlan: bring-up FAILED\n");
    s_wlan_up = (rc == 0);
    if (rc != 0) {
        /* Leave nothing half-started: a firmware that was released but never
         * answered, or rails left up, poisoned the next attempt (Iris reset
         * hang, then the watchdog). pas_shutdown is harmless if PAS never
         * got to auth_and_reset. */
        say("wlan: tearing the failed bring-up down\n");
        wlan_sta_reset(); s_hal_up = 0;
        smd_close(&s_wlan); smd_close(&s_ctrl); bt_hci_drop();
        s_restart = 1;
        vsay("wlan: pas_shutdown ... "); say_rc("", scm_pas_shutdown(PLAT_WCNSS_PAS_ID));
#if defined(PLAT_WCNSS_RAILS_PM8916)
        vote_kv("  rf_clk2 off", T_CLKA, 5u, K_SWEN, 0u);
        vote_kv("  rf_clk1 off", T_CLKA, 4u, K_SWEN, 0u);
        vote_kv("  vddpa  8916_l9 off", T_LDOA, 9u, K_SWEN, 0u);
        vote_kv("  vddrfa 8916_s3 off", T_SMPA, 3u, K_SWEN, 0u);
        vote_kv("  vdddig 8916_l5 off", T_LDOA, 5u, K_SWEN, 0u);
#endif
        mmio_write(PMU_CFG, 0);                   /* Iris/PMU back to the reset state */
    }
    return rc;
}

int wlan_down(void)
{
    uint32_t got;
    if (!s_wlan_up || !s_hal_up) return 0;
    net_down();
    wlan_sta_disconnect();
    vsay("wlan: stopping the MAC (firmware stays loaded)\n");
    /* HAL_STOP_REQ { hdr; u32 reason = RF_KILL (2) } */
    s_hal_buf[0] = HAL_STOP_REQ; s_hal_buf[1] = 12u; s_hal_buf[2] = 2u;
    got = hal_send_wait(s_hal_buf, 12u, HAL_STOP_RSP, s_hal_buf, sizeof s_hal_buf, 3000u);
    vsay_hex("wlan: HAL_STOP ", got >= 12u ? s_hal_buf[2] : 0xFFFFFFFFu); vsay("\n");
    s_hal_up = 0;
    wlan_sta_reset();
    return 0;
}

/* wlan_idle(): the radio the way stock leaves it for sleep (2026-09-08).
 * PROVEN on the Gen 4 (v166/v167): after wlan_power_off()'s PAS shutdown the
 * dead Pronto's RPM votes stay aggregated forever -- l9 reads ON and no vote
 * of ours can clear it; with the radio never started l9 is off. The stock
 * stack never unloads Pronto for idle: the firmware stays resident, the AP
 * stops the MAC and releases ITS OWN iris/bus votes, and Pronto puts itself
 * into XO shutdown (rooted C2+: PRONTO xo_count 0x34f) which drops its votes.
 * So: HAL_STOP, keep the firmware and the BT channels, release only what the
 * AP voted for a running WLAN (the PA rail and the bus rates). */
int wlan_idle(void)
{
    if (!s_wlan_up) return 0;
    wlan_down();
#if defined(PLAT_WCNSS_RAILS_PM8916)
    vote_kv("  vddpa 8916_l9 off (AP vote; Pronto votes it when it needs the PA)", T_LDOA, 9u, K_SWEN, 0u);
#endif
    vote_kv("  snoc 0 KHz",  T_CLK1, 1u, K_KHZ, 0u);
    vote_kv("  pcnoc 0 KHz", T_CLK1, 0u, K_KHZ, 0u);
    { uint32_t kv[3] = { RPM_KEY_BW, 4u, 0u }; (void)rpm_smd_request(0, RPM_BUS_SLAVE_REQ, EBI_SLV_RPM_ID, kv, sizeof kv); }
    say("wlan: radio IDLE (firmware resident, MAC stopped, AP votes released)\n");
    return 0;
}

int wlan_power_off(void)
{
    if (!s_wlan_up) return 0;
    wlan_down();
    smd_close(&s_wlan); smd_close(&s_ctrl); bt_hci_drop();   /* our halves CLOSED before the firmware dies */
    vsay("wlan: pas_shutdown ... "); say_rc("", scm_pas_shutdown(PLAT_WCNSS_PAS_ID));
    /* release the Iris/Pronto rails and XO buffers we voted (swen 0) */
#if defined(PLAT_WCNSS_RAILS_PM8916)
    vote_kv("  rf_clk2 off", T_CLKA, 5u, K_SWEN, 0u);
    vote_kv("  rf_clk1 off", T_CLKA, 4u, K_SWEN, 0u);
    vote_kv("  vddpa  8916_l9 off", T_LDOA, 9u, K_SWEN, 0u);
    vote_kv("  vddrfa 8916_s3 off", T_SMPA, 3u, K_SWEN, 0u);
    vote_kv("  vdddig 8916_l5 off", T_LDOA, 5u, K_SWEN, 0u);
    vote_kv("  vddpx/pll 8916_l7 off", T_LDOA, 7u, K_SWEN, 0u);
#endif
    /* (2026-09-07) the boot also pinned the crystal and both NoCs; release
     * them too or the SoC never drops below the WiFi-active bus rates. */
    vote_kv("  cxo Enab off",  T_CLK0, 0u, K_ENAB, 0u);
    vote_kv("  snoc 0 KHz",    T_CLK1, 1u, K_KHZ, 0u);
    vote_kv("  pcnoc 0 KHz",   T_CLK1, 0u, K_KHZ, 0u);
    bt_hci_drop();                      /* channels die with the firmware */
    mmio_write(PMU_CFG, 0);             /* Iris/PMU to the reset state, like the failure teardown */
    s_restart = 1;
    s_wlan_up = 0; s_hal_up = 0;
    say("wlan: radio powered DOWN\n");
    return 0;
}

int wlan_scan(struct wlan_scan_net *out, uint32_t max)
{
    int n;
    if (!s_wlan_up || !s_hal_up) return -1;
    wdog_extend(31u); deadman_kick();
    n = wcn36xx_scan(&s_wlan, 150u, out, max);
    wdog_extend(31u); deadman_kick();
    return n;
}

int wlan_connect(const char *ssid, const char *pass)
{
    static struct wlan_scan_net nets[16];
    int n, i, best = -1;
    if (wlan_up() != 0) return -1;
    if (wlan_sta_connected()) return 0;
    vsay("wlan: connect: scanning for \""); vsay(ssid); vsay("\"\n");
    n = wlan_scan(nets, 16u);
    for (i = 0; i < n; i++)
        if (!strcmp(nets[i].ssid, ssid) && (best < 0 || nets[i].rssi > nets[best].rssi)) best = i;
    if (best < 0) { say("wlan: connect: network not seen\n"); return -1; }
    wdog_extend(31u); deadman_kick();
    return wlan_sta_connect(ssid, pass, &nets[best]);   /* link only; the caller brings IP up WITHOUT holding the lock */
}
int wlan_connected(void) { return s_wlan_up && s_hal_up && wlan_sta_connected(); }
int wlan_disconnect(void) { net_down(); return wlan_sta_disconnect(); }

void wcnss_boot_diag(void)
{
    int rc;
#if defined(PLAT_WCNSS_INIT_VARIANTS)
    vsay("wcnss: build " __DATE__ " " __TIME__ " (seq v18: ONE init_image variant per boot (boot# mod 4: E1/E2/E4/E7) + post-failure state probes)\n");
#else
    say("wcnss: build " __DATE__ " " __TIME__ " (8909w v138: revert stuck USB-present bit)\n");
#endif
    wdog_extend(90u);
    deadman_kick();
    wlan_lock();                 /* the app (WiFi or BLE) may be bringing the radio up too */
    rc = wlan_up();
    say(rc == 0 ? "wcnss: BOOT OK\n" : "wcnss: boot failed\n");
    if (rc == 0) { static struct wlan_scan_net nets[16]; wlan_scan(nets, 16u); }
    wlan_unlock();
#if defined(BT_DIAG)
    if (rc == 0) bt_hci_diag();
#endif
#if defined(SMEM_DIAG)
    if (rc == 0) smem_diag_dump();     /* now shows the WCNSS channels */
#endif
}

#else
int  wcnss_boot(void) { return -1; }
void wcnss_boot_diag(void) { con_dbg("wcnss: not configured for this board\n"); }
int  wlan_up(void) { return -1; }
int  wlan_down(void) { return 0; }
int  wlan_power_off(void) { return 0; }
int  wlan_idle(void) { return 0; }
int  wlan_is_up(void) { return 0; }
int  wcnss_fw_resident(void) { return 0; }
int  wcnss_channel_lookup(const char *n, uint32_t *c, uint32_t *f) { (void)n; (void)c; (void)f; return -1; }
void wcnss_list_channels(void) {}
int  wlan_scan(struct wlan_scan_net *out, uint32_t max) { (void)out; (void)max; return -1; }
int  wlan_connect(const char *s, const char *p) { (void)s; (void)p; return -1; }
void wlan_lock(void) {}
void wlan_unlock(void) {}
int  wlan_connected(void) { return 0; }
int  wlan_disconnect(void) { return 0; }
uint32_t wlan_hal_xfer(const void *m, uint32_t l, uint32_t w, uint32_t *r, uint32_t x, uint32_t t) { (void)m;(void)l;(void)w;(void)r;(void)x;(void)t; return 0; }
const uint8_t *wlan_mac(void) { static const uint8_t z[6]; return z; }
#endif
