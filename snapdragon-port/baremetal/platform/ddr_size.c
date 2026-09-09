/* ddr_size.c — how much DDR this watch actually has, read at boot instead of
 * assumed from the board header.
 *
 * The TicWatch C2 and C2+ run the SAME image (same board key, same DTB) and
 * differ only in RAM, 512 MB vs 1 GB. The firmware never needs more than the
 * first 128 MB, so this changes nothing about what runs; it makes the About
 * screen's DDR line true and gives the log one honest line at boot.
 *
 * Two sources, tried in order:
 *   1. The device tree aboot hands over in r2 (kept in boot_r2 by startup.S).
 *      aboot fills the /memory node's reg from the SBL's RAM table before
 *      jumping, so the appended tree's <0 0 0 0> placeholder becomes the real
 *      banks. Root cells are 2/2 on these trees; both are read, not assumed.
 *   2. The SMEM "usable RAM partition table" (item 402), the same table aboot
 *      read: sum of the partitions typed SYS_MEMORY in category SDRAM. v0/v1
 *      carry 32-bit start/size, v2 64-bit.
 * If both fail the board header's PLAT_DDR_SIZE stands and the log says so. */
#include "platform.h"
#include <string.h>

#define FDT_MAGIC       0xD00DFEEDu
#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* Walk the flat tree: remember root #address-cells/#size-cells, and when
 * inside a top-level node whose name is "memory" (or "memory@...") or whose
 * device_type is "memory", sum the sizes in its reg. Returns bytes, 0 if none. */
static uint64_t fdt_memory_bytes(const uint8_t *fdt)
{
    uint32_t total = be32(fdt + 4), off_struct = be32(fdt + 8), off_str = be32(fdt + 12);
    const uint8_t *p = fdt + off_struct, *end = fdt + total;
    uint32_t ac = 2, sc = 2, depth = 0;
    int in_mem = 0, dt_mem = 0;
    uint64_t sum = 0, reg_sum = 0;
    if (total > 0x100000u || off_struct >= total || off_str >= total) return 0;
    while (p + 4 <= end) {
        uint32_t tok = be32(p); p += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)p; size_t n = strlen(name);
            p += (n + 4) & ~3u; depth++;
            if (depth == 2) { in_mem = (strncmp(name, "memory", 6) == 0 && (name[6] == 0 || name[6] == '@')); dt_mem = 0; reg_sum = 0; }
        } else if (tok == FDT_END_NODE) {
            if (depth == 2 && (in_mem || dt_mem)) sum += reg_sum;
            if (depth) depth--;
        } else if (tok == FDT_PROP) {
            uint32_t len = be32(p), nameoff = be32(p + 4); const uint8_t *val = p + 8;
            const char *pn = (const char *)fdt + off_str + nameoff;
            p += 8 + ((len + 3) & ~3u);
            if (depth == 1) {
                if (!strcmp(pn, "#address-cells") && len == 4) ac = be32(val);
                else if (!strcmp(pn, "#size-cells") && len == 4) sc = be32(val);
            } else if (depth == 2) {
                if (!strcmp(pn, "device_type") && len >= 6 && !strncmp((const char *)val, "memory", 6)) dt_mem = 1;
                else if (!strcmp(pn, "reg")) {
                    uint32_t cell = (ac + sc) * 4u, i;
                    reg_sum = 0;
                    if (cell == 0) continue;
                    for (i = 0; i + cell <= len; i += cell) {
                        const uint8_t *s = val + i + ac * 4u;
                        uint64_t sz = 0; uint32_t k;
                        for (k = 0; k < sc; k++) sz = (sz << 32) | be32(s + k * 4u);
                        reg_sum += sz;
                    }
                }
            }
        } else if (tok == FDT_NOP) {
        } else break;   /* FDT_END or garbage */
    }
    return sum;
}

/* SMEM usable-RAM partition table (lk: smem_ram_ptable). */
#define SMEM_USABLE_RAM_PARTITION_TABLE 402u
#define RAM_PTABLE_MAGIC1 0x9DA5E0A8u
#define RAM_PTABLE_MAGIC2 0xAF9EC4E2u
#define RAM_PART_SYS_MEMORY 1u
#define RAM_PART_CAT_SDRAM  0x0Eu

static uint64_t smem_ram_bytes(void)
{
    uint32_t size = 0;
    const uint8_t *t = (const uint8_t *)smem_get(SMEM_USABLE_RAM_PARTITION_TABLE, &size);
    uint32_t magic1, magic2, version, nparts, i;
    uint64_t sum = 0;
    if (!t || size < 16) return 0;
    memcpy(&magic1, t, 4); memcpy(&magic2, t + 4, 4); memcpy(&version, t + 8, 4); memcpy(&nparts, t + 12, 4);
    if (magic1 != RAM_PTABLE_MAGIC1 || magic2 != RAM_PTABLE_MAGIC2 || nparts > 32) return 0;
    for (i = 0; i < nparts; i++) {
        const uint8_t *e; uint32_t attr, cat, dom, type, esz;
        uint64_t start, len;
        if (version >= 2) {                     /* name[16] u64 start u64 size u32 attr cat dom type nparts rsvd[3] */
            esz = 16 + 8 + 8 + 4 * 8; e = t + 16 + i * esz; if (e + esz > t + size) break;
            memcpy(&start, e + 16, 8); memcpy(&len, e + 24, 8);
            memcpy(&attr, e + 32, 4); memcpy(&cat, e + 36, 4); memcpy(&dom, e + 40, 4); memcpy(&type, e + 44, 4);
        } else {                                /* name[16] u32 start u32 size u32 attr cat dom type nparts rsvd[3] */
            uint32_t s32, l32;
            esz = 16 + 4 + 4 + 4 * 8; e = t + 16 + i * esz; if (e + esz > t + size) break;
            memcpy(&s32, e + 16, 4); memcpy(&l32, e + 20, 4); start = s32; len = l32;
            memcpy(&attr, e + 24, 4); memcpy(&cat, e + 28, 4); memcpy(&dom, e + 32, 4); memcpy(&type, e + 36, 4);
        }
        (void)attr; (void)dom; (void)start;
        if (type == RAM_PART_SYS_MEMORY && cat == RAM_PART_CAT_SDRAM) sum += len;
    }
    return sum;
}

static uint32_t s_ddr_bytes;
static const char *s_ddr_src = "board header";

uint32_t ddr_size_detect(void)
{
    uint64_t b = 0;
    if (s_ddr_bytes) return s_ddr_bytes;
    if (boot_r2 >= PLAT_DDR_BASE && boot_r2 < PLAT_DDR_BASE + 0x10000000u && (boot_r2 & 3u) == 0u) {
        const uint8_t *fdt = (const uint8_t *)(uintptr_t)boot_r2;
        if (be32(fdt) == FDT_MAGIC) { b = fdt_memory_bytes(fdt); if (b) s_ddr_src = "device tree from aboot"; }
    }
    if (!b) { b = smem_ram_bytes(); if (b) s_ddr_src = "smem ram table"; }
    if (!b || b > 0xFFFFFFFFull) b = PLAT_DDR_SIZE;
    s_ddr_bytes = (uint32_t)b;
    con_puts("ddr: "); con_putdec(s_ddr_bytes >> 20); con_puts(" MB ("); con_puts(s_ddr_src); con_puts(")\n");
    return s_ddr_bytes;
}

uint32_t plat_ddr_size(void) { return s_ddr_bytes ? s_ddr_bytes : PLAT_DDR_SIZE; }
