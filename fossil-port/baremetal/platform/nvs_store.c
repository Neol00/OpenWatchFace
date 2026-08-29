/* nvs_store.c — persistent key-value store backing the Arduino Preferences
 * API on the Gen 6 (the "extended NVS": 8 MiB region vs an ESP32's ~few
 * hundred KB, though the in-RAM working set is capped far below that).
 *
 * Design: the WHOLE store lives in RAM (records arena + index) and is
 * serialized to eMMC on every nvs_commit(). Two slots ping-pong inside the
 * NVS region; a slot is payload-first then header-with-CRC-last, and load
 * picks the valid slot with the highest sequence number — a torn commit
 * therefore falls back to the previous good snapshot. eMMC does its own
 * wear-leveling; at watch-settings commit rates this is decades of margin.
 *
 * Record wire format (packed, little-endian):
 *   u8 ns_len | u8 key_len | u8 type | u8 pad | u32 data_len
 *   ns bytes | key bytes | data bytes            (no terminators)
 */
#include "platform.h"
#if defined(PLAT_BOARD_FOSSIL_GEN6)

#include <string.h>

#define NVS_ARENA_CAP   (192u * 1024u)
#define NVS_MAX_RECS    512u
#define SLOT_BLOCKS     8192u            /* 4 MiB each, 2 slots in 8 MiB */
#define HDR_MAGIC       0x53564E4Fu      /* "ONVS" */

struct nvs_hdr { uint32_t magic, seq, len, crc; };

struct rec { uint32_t off; };            /* offset of record header in arena */

static uint8_t  s_arena[NVS_ARENA_CAP];
static uint32_t s_arena_len;
static struct rec s_recs[NVS_MAX_RECS];
static uint32_t s_nrecs;
static uint32_t s_seq;
static int      s_loaded, s_dirty;

static uint32_t crc32_step(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static const uint8_t *rec_ns(const uint8_t *r)  { return r + 8; }
static const uint8_t *rec_key(const uint8_t *r) { return r + 8 + r[0]; }
static const uint8_t *rec_data(const uint8_t *r){ return r + 8 + r[0] + r[1]; }
static uint32_t rec_dlen(const uint8_t *r)
{ uint32_t v; memcpy(&v, r + 4, 4); return v; }
static uint32_t rec_size(const uint8_t *r)
{ return 8u + r[0] + r[1] + rec_dlen(r); }

static int rec_match(const uint8_t *r, const char *ns, const char *key)
{
    uint32_t nl = (uint32_t)strlen(ns), kl = (uint32_t)strlen(key);
    return r[0] == nl && r[1] == kl &&
           memcmp(rec_ns(r), ns, nl) == 0 && memcmp(rec_key(r), key, kl) == 0;
}

static int find(const char *ns, const char *key)
{
    for (uint32_t i = 0; i < s_nrecs; i++)
        if (rec_match(s_arena + s_recs[i].off, ns, key)) return (int)i;
    return -1;
}

/* Rebuild the arena without record i (compaction on overwrite/remove). */
static void drop(uint32_t i)
{
    uint32_t off = s_recs[i].off;
    uint32_t sz  = rec_size(s_arena + off);
    memmove(s_arena + off, s_arena + off + sz, s_arena_len - off - sz);
    s_arena_len -= sz;
    for (uint32_t j = 0; j < s_nrecs; j++)
        if (s_recs[j].off > off) s_recs[j].off -= sz;
    s_recs[i] = s_recs[--s_nrecs];
}

static void load_slot(uint32_t slot_lba, struct nvs_hdr *out_hdr, int *out_ok)
{
    uint8_t blk[512];
    struct nvs_hdr h;
    *out_ok = 0;
    if (emmc_read_block(slot_lba, blk) < 0) return;
    memcpy(&h, blk, sizeof(h));
    if (h.magic != HDR_MAGIC || h.len > NVS_ARENA_CAP) return;
    /* payload starts at the next block */
    uint32_t nblk = (h.len + 511u) / 512u;
    for (uint32_t i = 0; i < nblk; i++) {
        if (emmc_read_block(slot_lba + 1u + i, blk) < 0) return;
        uint32_t take = h.len - i * 512u; if (take > 512u) take = 512u;
        memcpy(s_arena + i * 512u, blk, take);
    }
    if (crc32_step(0, s_arena, h.len) != h.crc) return;
    *out_hdr = h; *out_ok = 1;
}

static void index_rebuild(uint32_t len)
{
    s_nrecs = 0; s_arena_len = 0;
    uint32_t off = 0;
    while (off + 8u <= len && s_nrecs < NVS_MAX_RECS) {
        uint32_t sz = rec_size(s_arena + off);
        if (sz < 8u || off + sz > len) break;
        s_recs[s_nrecs++].off = off;
        off += sz;
    }
    s_arena_len = off;
}

int nvs_load(void)
{
    if (s_loaded) return 0;
    if (!storage_ok() && storage_init() < 0) return -1;
    uint32_t base = storage_region_lba(1);
    struct nvs_hdr ha, hb; int oka, okb;
    uint32_t besthdr_seq = 0; int have = 0;

    load_slot(base, &ha, &oka);
    if (oka) { besthdr_seq = ha.seq; index_rebuild(ha.len); have = 1; }
    load_slot(base + SLOT_BLOCKS, &hb, &okb);
    if (okb && (!have || hb.seq > besthdr_seq)) {
        besthdr_seq = hb.seq; index_rebuild(hb.len); have = 1;
    } else if (have && oka && !(okb && hb.seq > besthdr_seq)) {
        /* slot A was the winner but slot B's load overwrote the arena: re-load */
        load_slot(base, &ha, &oka);
        if (oka) index_rebuild(ha.len);
    }
    if (!have) { s_arena_len = 0; s_nrecs = 0; }
    s_seq = besthdr_seq;
    s_loaded = 1;
    con_puts("nvs: loaded "); con_putdec(s_nrecs);
    con_puts(" records, "); con_putdec(s_arena_len); con_puts(" bytes\n");
    return 0;
}

int nvs_commit(void)
{
    if (!s_loaded || !storage_ok() || !s_dirty) return 0;
    uint32_t base = storage_region_lba(1);
    uint32_t slot = base + ((s_seq & 1u) ? 0u : SLOT_BLOCKS);  /* alternate */
    uint32_t nblk = (s_arena_len + 511u) / 512u;
    uint8_t blk[512];

    for (uint32_t i = 0; i < nblk; i++) {
        memset(blk, 0, sizeof(blk));
        uint32_t take = s_arena_len - i * 512u; if (take > 512u) take = 512u;
        memcpy(blk, s_arena + i * 512u, take);
        if (emmc_write_block(slot + 1u + i, blk) < 0) return -1;
    }
    memset(blk, 0, sizeof(blk));
    struct nvs_hdr h = { HDR_MAGIC, s_seq + 1u, s_arena_len,
                         crc32_step(0, s_arena, s_arena_len) };
    memcpy(blk, &h, sizeof(h));
    if (emmc_write_block(slot, blk) < 0) return -1;
    s_seq++;
    s_dirty = 0;
    return 0;
}

int nvs_put(const char *ns, const char *key, uint8_t type,
            const void *data, uint32_t len)
{
    if (!s_loaded && nvs_load() < 0) return -1;
    uint32_t nl = (uint32_t)strlen(ns), kl = (uint32_t)strlen(key);
    if (nl == 0 || nl > 15u || kl == 0 || kl > 15u) return -1;

    int i = find(ns, key);
    if (i >= 0) {
        const uint8_t *r = s_arena + s_recs[i].off;
        if (rec_dlen(r) == len && r[2] == type &&
            memcmp(rec_data(r), data, len) == 0) return (int)len;  /* no-op */
        drop((uint32_t)i);
    }
    uint32_t sz = 8u + nl + kl + len;
    if (s_arena_len + sz > NVS_ARENA_CAP || s_nrecs >= NVS_MAX_RECS) return -1;

    uint8_t *r = s_arena + s_arena_len;
    r[0] = (uint8_t)nl; r[1] = (uint8_t)kl; r[2] = type; r[3] = 0;
    memcpy(r + 4, &len, 4);
    memcpy(r + 8, ns, nl);
    memcpy(r + 8 + nl, key, kl);
    memcpy(r + 8 + nl + kl, data, len);
    s_recs[s_nrecs++].off = s_arena_len;
    s_arena_len += sz;
    s_dirty = 1;
    return (int)len;
}

int nvs_get(const char *ns, const char *key, void *out, uint32_t cap)
{
    if (!s_loaded && nvs_load() < 0) return -1;
    int i = find(ns, key);
    if (i < 0) return -1;
    const uint8_t *r = s_arena + s_recs[i].off;
    uint32_t dl = rec_dlen(r);
    if (out && cap) {
        uint32_t take = dl < cap ? dl : cap;
        memcpy(out, rec_data(r), take);
    }
    return (int)dl;
}

int nvs_erase(const char *ns, const char *key)
{
    if (!s_loaded && nvs_load() < 0) return -1;
    int i = find(ns, key);
    if (i < 0) return -1;
    drop((uint32_t)i);
    s_dirty = 1;
    return 0;
}

int nvs_erase_ns(const char *ns)
{
    if (!s_loaded && nvs_load() < 0) return -1;
    uint32_t nl = (uint32_t)strlen(ns);
    for (uint32_t i = 0; i < s_nrecs; ) {
        const uint8_t *r = s_arena + s_recs[i].off;
        if (r[0] == nl && memcmp(rec_ns(r), ns, nl) == 0) { drop(i); s_dirty = 1; }
        else i++;
    }
    return 0;
}

int nvs_haskey(const char *ns, const char *key)
{
    if (!s_loaded && nvs_load() < 0) return 0;
    return find(ns, key) >= 0;
}

#endif /* PLAT_BOARD_FOSSIL_GEN6 */
