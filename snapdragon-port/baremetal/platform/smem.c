/* smem.c — Qualcomm shared memory (SMEM) reader: the foundation of WiFi.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * Every one of these three watches puts its WiFi on a SEPARATE PROCESSOR.
 * The A7/A53 we run on has no radio of its own: the 802.11 MAC and PHY live
 * on the Pronto/WCNSS co-processor, and the ONLY way to talk to it is a
 * message-passing protocol carried in a slab of DDR that both processors can
 * see. That slab is SMEM, and everything WiFi needs is stacked on top of it:
 *
 *     wcn36xx HAL        (scan / join / set-key / data)
 *       SMD channel      (byte streams to WCNSS, "WCNSS_CTRL" etc.)
 *         SMEM           <-- THIS FILE: the allocator the channels live in
 *
 * The RPM (which owns every PMIC rail WCNSS needs) is reached the same way:
 * an SMD channel ("rpm_requests") in SMEM. So this file gates both power AND
 * the radio, and nothing above it can be debugged until it is right.
 *
 * It is still READ-ONLY: it maps nothing, enables no clock, powers no rail
 * and writes no register.
 *
 * THE LAYOUT (drivers/soc/qcom/smem.c in mainline; identical in the 3.18 and
 * 4.14 vendor kernels these watches shipped with)
 * ------------------------------------------------------------------------
 *   struct smem_header at region base:
 *     0x000  proc_comm[4]     4 x 16 B legacy mailboxes (64 B)   <-- FIRST
 *     0x040  version[32]      per-subsystem versions; [7] is the SBL's
 *     0x0C0  initialized      1 once the SBL has built the heap
 *     0x0C4  free_offset      bump pointer into the global heap
 *     0x0C8  available        bytes left in the global heap
 *     0x0CC  reserved         must be 0
 *     0x0D0  toc[512]         global heap items, 16 B each
 *
 * The first version of this file put `initialized` at offset 0 and
 * `version` at 0x50. Offset 0 is proc_comm[0].command, which is legitimately
 * zero on a running system, so that image reported "SMEM not initialized,
 * every field zero" on the Gen 6 and a full flash cycle went into asking
 * whether TrustZone was feeding us zeros. It was not. The header was simply
 * being read at the wrong offsets. Both the offsets above and the SBL index
 * (7, not 11) are now taken from the mainline driver, which is the reference
 * for every number in this file.
 *
 * TWO GENERATIONS OF SMEM:
 *   version 11 — "global heap". Items live in the TOC above; a partition
 *     table at the LAST 4 KB of the region ("$TOC" magic) adds private
 *     apps<->subsystem partitions (SMD channel descriptors live there).
 *     MEASURED on the Gen 6 (2026-08-30): sbl_ver=0x000b0000, 7 partitions
 *     incl. apps<->wcnss, RPM channels in the aux region 0x60000. The 8909w
 *     boards are expected to match.
 *   version 12 — "global partition". The TOC is unused; items live in a
 *     partition whose host0 == host1 == 0xFFFE. Supported but not seen on
 *     any of these watches so far.
 *
 * ADDRESSES (from the hardware dumps, not from a header guess):
 *   Gen 6 (sda429w): /soc/smem_region@86300000 in the tree dumped from the
 *                    running watch: reg = <0x86300000 0x00100000>, no-map
 *   TicWatch C2 / Gen 4 (msm8909w): skipjack.dts:3831
 *                    qcom,smem@87d00000 reg = <0x87d00000 0x100000 ...>
 *                    plus smem_targ_info_reg @0x193d000, which the vendor
 *                    driver trusts OVER `reg` -- read back in the late probe.
 *
 * MEMORY TYPE: SMEM must NOT be cached. Another processor writes it behind
 * our back, so a cache line we hold is a stale line we will believe. Both
 * boards already map this range as Device-XN in mmu.c (the Gen 6 as part of
 * the 0x85B-0x867 other_ext carve-out, which covers 0x863) -- uncached and
 * strongly ordered, which is exactly what shared memory wants. This file
 * therefore adds no mapping of its own ON PURPOSE.
 */
#include "platform.h"
#if defined(PLAT_SMEM_BASE)

#include <stddef.h>

/* ---- header (struct smem_header) ---------------------------------------- */
#define SMEM_HDR_VERSION_OFF   0x040u
#define SMEM_HDR_INIT_OFF      0x0C0u
#define SMEM_HDR_FREE_OFF      0x0C4u
#define SMEM_HDR_AVAIL_OFF     0x0C8u
#define SMEM_HDR_RESERVED_OFF  0x0CCu
#define SMEM_HDR_TOC_OFF       0x0D0u

#define SMEM_MASTER_SBL_VERSION_INDEX  7u
#define SMEM_GLOBAL_HEAP_VERSION       11u
#define SMEM_GLOBAL_PART_VERSION       12u

#define SMEM_ITEM_COUNT     512u
#define SMEM_HOST_APPS      0u
#define SMEM_GLOBAL_HOST    0xFFFEu
#define SMEM_HOST_COUNT     20u

struct smem_global_entry {          /* one item in the global heap TOC */
    uint32_t allocated;
    uint32_t offset;
    uint32_t size;
    uint32_t aux_base;              /* 0 = this region; low 2 bits are flags */
};
#define SMEM_AUX_BASE_MASK  0xFFFFFFFCu

/* ---- partition table: last 4 KB of the region ---------------------------- */
#define SMEM_PTABLE_OFF     (PLAT_SMEM_SIZE - 0x1000u)
#define SMEM_PTABLE_MAGIC   0x434F5424u   /* "$TOC" little-endian */
#define SMEM_PART_MAGIC     0x54525024u   /* "$PRT" */
#define SMEM_PRIVATE_CANARY 0xA5A5u

struct smem_ptable_entry {
    uint32_t offset;                /* within the main region */
    uint32_t size;
    uint32_t flags;
    uint16_t host0;
    uint16_t host1;
    uint32_t cacheline;
    uint32_t reserved[7];
};

struct smem_ptable {
    uint32_t magic;
    uint32_t version;
    uint32_t num_entries;
    uint32_t reserved[5];
    struct smem_ptable_entry entry[];
};

struct smem_partition_header {
    uint32_t magic;
    uint16_t host0;
    uint16_t host1;
    uint32_t size;
    uint32_t offset_free_uncached;
    uint32_t offset_free_cached;
    uint32_t reserved[3];
};

struct smem_private_entry {         /* header of each item in a partition */
    uint16_t canary;
    uint16_t item;
    uint32_t size;                  /* includes padding bytes */
    uint16_t padding_data;
    uint16_t padding_hdr;
    uint32_t reserved;
};

/* Items we actually care about, named so the dump is readable. */
#define SMEM_ID_CHANNEL_ALLOC_TBL   13u   /* SMD channel directory (per edge) */
#define SMEM_ID_HW_SW_BUILD_ID     137u   /* ASCII build string: our proof    */

static volatile uint8_t *const s_smem = (volatile uint8_t *)PLAT_SMEM_BASE;
static int      s_smem_ok;
static uint32_t s_version;              /* SBL SMEM version (11 or 12)       */
static volatile struct smem_ptable *s_ptable;                /* NULL if none */
static volatile struct smem_partition_header *s_global;      /* v12 only     */

static uint32_t smem_rd32(uint32_t off)
{
    return *(volatile uint32_t *)(s_smem + off);
}

/* Bounds-check EVERY offset against the region before handing back a
 * pointer. The tables are written by other processors; treating them as
 * trustworthy is how a plausible-looking offset turns into a wild pointer
 * into TrustZone DDR and an instant, unexplained reset. */
static int smem_range_ok(uint32_t off, uint32_t size)
{
    if (size == 0u || size > PLAT_SMEM_SIZE) return 0;
    if (off  >= PLAT_SMEM_SIZE)              return 0;
    if (off + size > PLAT_SMEM_SIZE)         return 0;
    return 1;
}

static volatile struct smem_partition_header *
smem_partition_hdr(volatile struct smem_ptable_entry *e)
{
    volatile struct smem_partition_header *p;

    if (!smem_range_ok(e->offset, e->size)) return NULL;
    if (e->size < sizeof *p)                 return NULL;
    p = (volatile struct smem_partition_header *)(s_smem + e->offset);
    if (p->magic != SMEM_PART_MAGIC)         return NULL;
    if (p->host0 != e->host0 || p->host1 != e->host1) return NULL;
    if (p->size != e->size)                  return NULL;
    if (p->offset_free_uncached > p->size)   return NULL;
    return p;
}

/* Find the private partition shared by the apps core and `host`, or the
 * global partition when host == SMEM_GLOBAL_HOST. */
static volatile struct smem_partition_header *smem_find_partition(uint32_t host)
{
    uint32_t i;

    if (!s_ptable) return NULL;
    for (i = 0; i < s_ptable->num_entries; i++) {
        volatile struct smem_ptable_entry *e = &s_ptable->entry[i];
        if (!e->offset || !e->size) continue;
        if (host == SMEM_GLOBAL_HOST) {
            if (e->host0 == SMEM_GLOBAL_HOST && e->host1 == SMEM_GLOBAL_HOST)
                return smem_partition_hdr(e);
        } else {
            if ((e->host0 == SMEM_HOST_APPS && e->host1 == host) ||
                (e->host1 == SMEM_HOST_APPS && e->host0 == host))
                return smem_partition_hdr(e);
        }
    }
    return NULL;
}

/* Walk a partition's uncached entries (they grow upward from the header)
 * looking for `id`. Stops at the partition's free pointer, on a bad canary,
 * or on an entry that would run past the partition -- any of which means the
 * remote side and we disagree about the layout and nothing here is safe. */
static void *smem_partition_get(volatile struct smem_partition_header *p,
                                uint32_t id, uint32_t *size_out)
{
    uint32_t base = (uint32_t)((volatile uint8_t *)p - s_smem);
    uint32_t off  = sizeof *p;
    uint32_t end  = p->offset_free_uncached;

    while (off + sizeof(struct smem_private_entry) <= end) {
        volatile struct smem_private_entry *e =
            (volatile struct smem_private_entry *)(s_smem + base + off);
        uint32_t data_off, next;

        if (e->canary != SMEM_PRIVATE_CANARY) return NULL;
        data_off = off + sizeof *e + e->padding_hdr;
        next     = data_off + e->size;
        if (next > end || next < off) return NULL;   /* overflow / overrun */

        if (e->item == id) {
            if (e->size < e->padding_data) return NULL;
            if (size_out) *size_out = e->size - e->padding_data;
            return (void *)(s_smem + base + data_off);
        }
        off = next;
    }
    return NULL;
}

int smem_init(void)
{
    uint32_t init, reserved, ver;

    if (s_smem_ok) return 0;

    init     = smem_rd32(SMEM_HDR_INIT_OFF);
    reserved = smem_rd32(SMEM_HDR_RESERVED_OFF);
    if (init != 1u || reserved != 0u) {
        con_dbg("smem: header not initialized (init=");
        con_dbg_hex(init);
        con_dbg(" reserved=");
        con_dbg_hex(reserved);
        con_puts(") - shared memory unusable\n");
        return -1;
    }

    ver = smem_rd32(SMEM_HDR_VERSION_OFF + 4u * SMEM_MASTER_SBL_VERSION_INDEX);
    s_version = ver >> 16;

    /* The partition table is optional on v11 and mandatory on v12. Validate
     * it once here so every later lookup can trust s_ptable. */
    s_ptable = NULL;
    if (smem_rd32(SMEM_PTABLE_OFF) == SMEM_PTABLE_MAGIC) {
        volatile struct smem_ptable *pt =
            (volatile struct smem_ptable *)(s_smem + SMEM_PTABLE_OFF);
        uint32_t n = pt->num_entries;
        if (n <= (0x1000u - sizeof *pt) / sizeof(struct smem_ptable_entry))
            s_ptable = pt;
        else {
            con_dbg("smem: partition table claims ");
            con_dbg_dec(n);
            con_puts(" entries - refusing it\n");
        }
    }

    s_global = NULL;
    if (s_version == SMEM_GLOBAL_PART_VERSION) {
        s_global = smem_find_partition(SMEM_GLOBAL_HOST);
        if (!s_global) {
            con_dbg("smem: v12 layout but no valid global partition\n");
            return -1;
        }
    } else if (s_version != SMEM_GLOBAL_HEAP_VERSION) {
        con_puts("smem: unexpected SBL version ");
        con_dbg_dec(s_version);
        con_puts(" - refusing to guess the layout\n");
        return -1;
    }

    s_smem_ok = 1;
    return 0;
}

/* Return a pointer to global SMEM item `id`, or NULL. `*size_out` gets its
 * length. TOC items with a non-zero aux_base live in a DIFFERENT memory
 * region (the RPM message RAM, typically) which we do not map, so those are
 * refused rather than dereferenced. */
void *smem_get(uint32_t id, uint32_t *size_out)
{
    volatile struct smem_global_entry *e;
    uint32_t off, size;

    if (!s_smem_ok || id >= SMEM_ITEM_COUNT) return NULL;

    if (s_global)
        return smem_partition_get(s_global, id, size_out);

    e = (volatile struct smem_global_entry *)(s_smem + SMEM_HDR_TOC_OFF) + id;
    if (e->allocated != 1u) return NULL;

    off  = e->offset;
    size = e->size;

#if defined(PLAT_SMEM_AUX_BASE)
    /* Items in the aux region (RPM message RAM): offset is relative to THAT
     * region's base. This is where the RPM's SMD channels live. */
    if ((e->aux_base & SMEM_AUX_BASE_MASK) == PLAT_SMEM_AUX_BASE) {
        if (size == 0u || off >= PLAT_SMEM_AUX_SIZE ||
            size > PLAT_SMEM_AUX_SIZE - off) return NULL;
        if (size_out) *size_out = size;
        return (void *)(uintptr_t)(PLAT_SMEM_AUX_BASE + off);
    }
#endif
    if ((e->aux_base & SMEM_AUX_BASE_MASK) != 0u) return NULL;
    if (!smem_range_ok(off, size)) return NULL;

    if (size_out) *size_out = size;
    return (void *)(s_smem + off);
}

/* Item `id` in the private partition shared between us and `host`
 * (SMEM_HOST_WCNSS etc.). This is where SMD keeps its per-edge channel
 * descriptors and FIFOs. NULL if there is no such partition or item. */
void *smem_get_host(uint32_t host, uint32_t id, uint32_t *size_out)
{
    volatile struct smem_partition_header *p;

    if (!s_smem_ok || id >= SMEM_ITEM_COUNT) return NULL;
    p = smem_find_partition(host);
    if (!p) return NULL;
    return smem_partition_get(p, id, size_out);
}

int      smem_ok(void)      { return s_smem_ok; }
uint32_t smem_version(void) { return s_version; }

int smem_host_partition_present(uint32_t host)
{
    return s_smem_ok && smem_find_partition(host) != NULL;
}

/* ---- diagnostics ---------------------------------------------------------
 * Gated behind -DSMEM_DIAG on its own, NOT behind BOOT_DIAG as well: a
 * diagnostic that needs two independent switches thrown to say anything is a
 * diagnostic that reports nothing on the day you need it. */

static void smem_put_ascii(const char *s, uint32_t n)
{
    uint32_t i;
    /* Bounded by the item size, stopped at the first NUL, non-printable bytes
     * shown as '.' so a wrong guess about an item cannot spray control
     * characters at the USB console. */
    for (i = 0; i < n && s[i]; i++)
        con_dbg_c((s[i] >= 0x20 && s[i] < 0x7F) ? s[i] : '.');
}

static const char *smem_host_name(uint32_t h)
{
    switch (h) {
    case 0:  return "apps";
    case 1:  return "modem";
    case 2:  return "adsp";
    case 3:  return "dsps";
    case 4:  return "wcnss";
    case 5:  return "cdsp";
    case 6:  return "rpm";
    case SMEM_GLOBAL_HOST: return "global";
    default: return "?";
    }
}

/* List every item in one partition: ids and sizes only. */
static void smem_dump_partition(volatile struct smem_partition_header *p)
{
    uint32_t base = (uint32_t)((volatile uint8_t *)p - s_smem);
    uint32_t off  = sizeof *p, end = p->offset_free_uncached, n = 0;

    con_dbg("    part@");  con_dbg_hex(PLAT_SMEM_BASE + base);
    con_dbg(" size=");     con_dbg_hex(p->size);
    con_dbg(" free_unc="); con_dbg_hex(p->offset_free_uncached);
    con_dbg(" free_c=");   con_dbg_hex(p->offset_free_cached);
    con_dbg("\n");

    while (off + sizeof(struct smem_private_entry) <= end) {
        volatile struct smem_private_entry *e =
            (volatile struct smem_private_entry *)(s_smem + base + off);
        uint32_t next;
        if (e->canary != SMEM_PRIVATE_CANARY) {
            con_dbg("    bad canary at +"); con_dbg_hex(off); con_dbg("\n");
            break;
        }
        next = off + sizeof *e + e->padding_hdr + e->size;
        if (next > end || next < off) { con_dbg("    entry overruns\n"); break; }
        con_dbg("      item "); con_dbg_dec(e->item);
        con_dbg(" size=");      con_dbg_hex(e->size - e->padding_data);
        if (e->item == SMEM_ID_CHANNEL_ALLOC_TBL) con_dbg("  <- SMD channel table");
        con_dbg("\n");
        n++;
        off = next;
    }
    con_dbg("    "); con_dbg_dec(n); con_dbg(" items\n");
}

/* SMD channel directory (item 13): 64 entries of { name[20]; cid; flags;
 * ref_count }. flags[7:0] is the edge (15 = RPM, 6 = WCNSS), bit 9 = packet
 * mode. This is the list of channels the SBL/RPM pre-created; "rpm_requests"
 * on edge 15 is the one step 2 opens first. Also decodes the same table
 * when it appears inside a private partition (the WCNSS edge, once booted). */
struct smd_alloc_entry { char name[20]; uint32_t cid, flags, ref_count; };

static void smem_dump_smd_table(const char *where, const void *tbl, uint32_t size)
{
    const volatile struct smd_alloc_entry *e = tbl;
    uint32_t n = size / sizeof *e, i, used = 0;

    con_dbg("smem: SMD channel table ("); con_dbg(where); con_dbg("):\n");
    for (i = 0; i < n; i++) {
        uint32_t cid = e[i].cid, fl = e[i].flags;
        if (e[i].name[0] == 0 && cid == 0 && fl == 0) continue;
        used++;
        con_dbg("    cid "); con_dbg_dec(cid);
        con_dbg(" edge ");   con_dbg_dec(fl & 0xFFu);
        con_dbg((fl & 0x200u) ? " pkt " : " strm");
        con_dbg(" ref=");    con_dbg_dec(e[i].ref_count);
        con_dbg(" \"");     smem_put_ascii((const char *)e[i].name, 20u);
        con_dbg("\"\n");
    }
    con_dbg("    "); con_dbg_dec(used); con_dbg(" channels\n");
}

void smem_diag_dump(void)
{
    uint32_t size = 0, n = 0, i;
    const char *build;

    con_dbg("smem: base=");   con_dbg_hex(PLAT_SMEM_BASE);
    con_dbg(" init=");        con_dbg_hex(smem_rd32(SMEM_HDR_INIT_OFF));
    con_dbg(" free_off=");    con_dbg_hex(smem_rd32(SMEM_HDR_FREE_OFF));
    con_dbg(" avail=");       con_dbg_hex(smem_rd32(SMEM_HDR_AVAIL_OFF));
    con_dbg(" sbl_ver=");     con_dbg_hex(smem_rd32(SMEM_HDR_VERSION_OFF +
                                          4u * SMEM_MASTER_SBL_VERSION_INDEX));
    con_dbg(" ptable_magic="); con_dbg_hex(smem_rd32(SMEM_PTABLE_OFF));
    con_dbg("\n");

    if (!s_smem_ok) {
        con_puts("smem: not usable - see the line above\n");
        return;
    }

    con_dbg("smem: layout v"); con_dbg_dec(s_version);
    con_dbg(s_global ? " (global partition)\n" : " (global heap)\n");

    /* THE PROOF: an ASCII string the SBL wrote, printed back by us. */
    build = (const char *)smem_get(SMEM_ID_HW_SW_BUILD_ID, &size);
    if (build && size) {
        con_dbg("smem: build_id=\"");
        smem_put_ascii(build, size);
        con_dbg("\" hex:");
        for (i = 0; i < 4u && i * 4u < size; i++) {
            con_dbg(" "); con_dbg_hex(*(const volatile uint32_t *)(build + 4u * i));
        }
        con_dbg("\n");
    } else {
        con_dbg("smem: no build-id item (137) - layout guess may be wrong\n");
    }

    /* Partition table: which hosts the SBL gave us a private channel with.
     * WCNSS (4) and, for the rails, RPM (6) are the ones WiFi needs. */
    if (s_ptable) {
        con_dbg("smem: ptable v"); con_dbg_dec(s_ptable->version);
        con_dbg(", ");            con_dbg_dec(s_ptable->num_entries);
        con_dbg(" entries\n");
        for (i = 0; i < s_ptable->num_entries; i++) {
            volatile struct smem_ptable_entry *e = &s_ptable->entry[i];
            volatile struct smem_partition_header *p;
            if (!e->offset || !e->size) continue;
            con_dbg("  ["); con_dbg_dec(i); con_dbg("] ");
            con_dbg(smem_host_name(e->host0)); con_dbg("<->");
            con_dbg(smem_host_name(e->host1));
            con_dbg(" off=");  con_dbg_hex(e->offset);
            con_dbg(" size="); con_dbg_hex(e->size);
            con_dbg(" cl=");   con_dbg_dec(e->cacheline);
            p = smem_partition_hdr(e);
            con_dbg(p ? " ok\n" : " BAD HEADER\n");
            if (p && (e->host0 == SMEM_GLOBAL_HOST ||
                      e->host0 == SMEM_HOST_APPS || e->host1 == SMEM_HOST_APPS))
                smem_dump_partition(p);
        }
    } else {
        con_dbg("smem: no partition table (pure global heap)\n");
    }

    /* Global heap TOC (v11). On v12 the SBL still fills a few fixed items
     * here, so print it either way -- the count alone is diagnostic. */
    for (i = 0; i < SMEM_ITEM_COUNT; i++) {
        volatile struct smem_global_entry *e =
            (volatile struct smem_global_entry *)(s_smem + SMEM_HDR_TOC_OFF) + i;
        if (e->allocated != 1u) continue;
        n++;
        con_dbg("  toc item "); con_dbg_dec(i);
        con_dbg(" off=");       con_dbg_hex(e->offset);
        con_dbg(" size=");      con_dbg_hex(e->size);
        if ((e->aux_base & SMEM_AUX_BASE_MASK) != 0u) {
            con_dbg(" aux="); con_dbg_hex(e->aux_base & SMEM_AUX_BASE_MASK);
            con_dbg(" (other region)");
        }
        if (i == SMEM_ID_CHANNEL_ALLOC_TBL) con_dbg("  <- SMD channel table");
        con_dbg("\n");
    }
    con_dbg("smem: "); con_dbg_dec(n); con_dbg(" global TOC items\n");

    {
        const void *tbl = smem_get(SMEM_ID_CHANNEL_ALLOC_TBL, &size);
        if (tbl) smem_dump_smd_table("global heap", tbl, size);
        tbl = smem_get_host(SMEM_HOST_WCNSS, SMEM_ID_CHANNEL_ALLOC_TBL, &size);
        if (tbl) smem_dump_smd_table("apps<->wcnss", tbl, size);
        else con_dbg("smem: no SMD table in apps<->wcnss yet (WCNSS not booted)\n");
    }
}

/* ---- late probe -----------------------------------------------------------
 * The one read here that has NOT been proved safe by a booting image is the
 * smem_targ_info register on the msm8909w boards. It sits in TCSR, an
 * always-on block, so it should not hang the bus the way a clock-gated
 * peripheral does -- but "should not" is exactly what this contract is for:
 *
 *   - call it only after usb_is_configured(), from the main loop, and
 *   - print the address, flush, poll USB, and wait BEFORE the read, so a
 *     read that never returns still leaves its address in the host's tty.
 *
 * (The earlier version of this probe swept the other_ext/QSEE carve-out and
 * bootlooped twice. That sweep is gone: it was looking for SMEM in the wrong
 * place because the header was being parsed at the wrong offsets.)
 */
static uint32_t smem_probe(const char *what, uint32_t addr)
{
    uint32_t v;

    con_dbg("  probe ");  con_dbg(what);
    con_dbg(" @");        con_dbg_hex(addr);
    con_dbg(" ... ");
    con_flush();                 /* into the ramlog */
    usb_poll();                  /* and onto the cable */
    timer_delay_ms(50);          /* give the transfer time to land */

    v = *(volatile uint32_t *)addr;   /* <-- the read that may not return */

    con_dbg_hex(v);
    con_dbg("\n");
    con_flush();
    usb_poll();
    timer_delay_ms(20);
    return v;
}

void smem_scan_report(void)
{
    wdog_extend(60u);
    deadman_kick();

    con_dbg("smem-scan: bounded probe, host is listening\n");
    con_flush();
    usb_poll();

    /* CONTROL: our own text. MUST print non-zero, or the fault is in this
     * diagnostic rather than in the memory it is asking about. */
    smem_probe("control/own-text", 0x80008000u);

    /* The header at its REAL offsets, and the table at the end of the region.
     * If init != 1 here the region base is wrong; if the ptable magic is not
     * "$TOC" on the Gen 6 the v12 assumption is wrong. */
    smem_probe("hdr.initialized", PLAT_SMEM_BASE + SMEM_HDR_INIT_OFF);
    smem_probe("hdr.free_offset", PLAT_SMEM_BASE + SMEM_HDR_FREE_OFF);
    smem_probe("hdr.sbl_version", PLAT_SMEM_BASE + SMEM_HDR_VERSION_OFF +
                                  4u * SMEM_MASTER_SBL_VERSION_INDEX);
    smem_probe("ptable.magic",    PLAT_SMEM_BASE + SMEM_PTABLE_OFF);

#if defined(PLAT_SMEM_TARG_INFO_REG)
    /* msm8909w: the vendor smem driver ignores `reg` when this register is
     * present and takes base/size from here instead:
     *   struct smem_targ_info { u32 identifier "SIII"; u32 size; u64 base; }
     * If it disagrees with PLAT_SMEM_BASE, the board header is wrong. */
    {
        uint32_t id   = smem_probe("targ_info.id",   PLAT_SMEM_TARG_INFO_REG);
        uint32_t size = smem_probe("targ_info.size", PLAT_SMEM_TARG_INFO_REG + 4u);
        uint32_t base = smem_probe("targ_info.base", PLAT_SMEM_TARG_INFO_REG + 8u);
        if (id == 0x49494953u) {
            con_dbg("smem-scan: targ_info says base=");  con_dbg_hex(base);
            con_dbg(" size=");                            con_dbg_hex(size);
            con_dbg((base == PLAT_SMEM_BASE && size == PLAT_SMEM_SIZE)
                     ? " - matches the board header\n"
                     : " - DOES NOT MATCH the board header, fix PLAT_SMEM_*\n");
        } else {
            con_dbg("smem-scan: no \"SIII\" magic - targ_info absent, `reg` stands\n");
        }
    }
#endif

    con_dbg("smem-scan: done -- no probed read was fatal\n");
    con_flush();
    usb_poll();
}

#endif /* PLAT_SMEM_BASE */
