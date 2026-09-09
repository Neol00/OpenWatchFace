/* rng_msm.c — entropy for mbedTLS on the msm8909w: the SoC's PRNG block
 * (qrng@22000, "qcom,msm-rng") with a timer-jitter fallback.
 *
 * The block is the Linux msm_rng driver's: PRNG_DATA_OUT at +0, PRNG_STATUS
 * at +4 (bit 0 = a word is ready), PRNG_LFSR_CFG at +0x100 and PRNG_CONFIG at
 * +0x104 (bit 1 = HW enable). Its AHB clock is GCC_PRNG_AHB_CBCR (+0x13004),
 * a plain branch clock. On these watches TrustZone has usually configured the
 * PRNG already; we only enable the branch and the HW bit if needed.
 *
 * If the block never signals a word (clock gated by TZ, wrong offset on some
 * unit) the poll falls back to timer_us32() jitter mixed across reads. That
 * is weak entropy; it is flagged in the log once so it is not mistaken for
 * the real thing. The consumer is the TLS client's CTR-DRBG seed. */
#include "platform.h"
#include <stddef.h>

/* The PRNG lives at a different address per SoC: msm8909w qrng@22000,
 * sdm429w qrng@e3000 (both watches' own device trees). Board headers may
 * override; the 8909 value stays the default. */
#ifndef PLAT_PRNG_BASE
#define PLAT_PRNG_BASE  0x00022000u
#endif
#define PRNG_BASE       PLAT_PRNG_BASE
#define PRNG_DATA_OUT   (PRNG_BASE + 0x000u)
#define PRNG_STATUS     (PRNG_BASE + 0x004u)
#define PRNG_LFSR_CFG   (PRNG_BASE + 0x100u)
#define PRNG_CONFIG     (PRNG_BASE + 0x104u)
#define PRNG_HW_ENABLE  0x2u
#define GCC_PRNG_AHB_CBCR (PLAT_GCC_BASE + 0x13004u)

static int s_state;   /* 0 unknown, 1 hw works, -1 hw dead (jitter only) */

/* v201: the stock msm_rng driver registers a bus-scaling client for
 * "msm-rng-noc" and votes bandwidth on the APPS -> slv-prng path (C2 DTS:
 * slv-prng cell 0x26a, qcom,slv-rpm-id 0x2c, 800 KBps) before every access.
 * We never did, and the first access (the OTA check's TLS seed) reset the
 * watch at once with no CPU fault: an unvoted NoC slave raises NOCERR, which
 * TrustZone owns and answers with PS_HOLD. Vote it, and if the RPM refuses,
 * never touch the block. */
#define RPM_BUS_SLAVE_REQ 0x766c7362u   /* "bslv" */
#define RPM_KEY_BW        0x00007762u   /* "bw", bytes/s */
#define PRNG_SLV_RPM_ID   0x2Cu
static int prng_bus_vote(void)
{
    static int voted;
    if (voted) return voted > 0 ? 0 : -1;
    if (rpm_smd_init() < 0) { voted = -1; con_puts("rng: no RPM channel, prng not used\n"); return -1; }
    uint32_t kv[3] = { RPM_KEY_BW, 4, 800u * 1024u };
    int rc = rpm_smd_request(0, RPM_BUS_SLAVE_REQ, PRNG_SLV_RPM_ID, kv, sizeof kv);
    con_puts("rng: prng bus vote (bslv 0x2c) rc "); con_putdec((uint32_t)(rc < 0 ? -rc : rc)); con_puts(rc ? " -> prng NOT used\n" : "\n");
    voted = rc == 0 ? 1 : -1;
    return rc == 0 ? 0 : -1;
}
static void prng_enable(void)
{
    uint32_t v;
    mmio_write(GCC_PRNG_AHB_CBCR, mmio_read(GCC_PRNG_AHB_CBCR) | 1u);
    timer_delay_us(50);
    v = mmio_read(PRNG_CONFIG);
    if (!(v & PRNG_HW_ENABLE)) {
        uint32_t l = mmio_read(PRNG_LFSR_CFG) & 0xFFFFu;
        mmio_write(PRNG_LFSR_CFG, l | 0xDDDDu);
        mmio_write(PRNG_CONFIG, v | PRNG_HW_ENABLE);
    }
}

static int prng_word(uint32_t *out)
{
    uint32_t t0 = timer_us32();
    while (!(mmio_read(PRNG_STATUS) & 1u)) {
        if ((uint32_t)(timer_us32() - t0) > 2000u) return -1;
    }
    *out = mmio_read(PRNG_DATA_OUT);
    return 0;
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    size_t i = 0;
    static uint32_t mix = 0x9E3779B9u;
    (void)data;

    if (s_state == 0) {
        uint32_t w;
        if (prng_bus_vote() < 0) s_state = -1;
        else {
            prng_enable();
            s_state = (prng_word(&w) == 0 && w != 0u && w != 0xFFFFFFFFu) ? 1 : -1;
        }
        con_dbg(s_state > 0 ? "rng: msm prng ok\n"
                             : "rng: msm prng silent, timer-jitter fallback (weak)\n");
    }
    while (i < len) {
        uint32_t w = 0;
        if (s_state > 0 && prng_word(&w) < 0) { s_state = -1; con_puts("rng: prng stalled, jitter fallback\n"); }
        if (s_state < 0) {
            /* xorshift over the timer's low bits sampled across a busy wait */
            uint32_t t = timer_us32();
            uint32_t j; for (j = 0; j < 64u; j++) { mix ^= timer_us32() + (mix << 13); mix ^= mix >> 17; mix ^= mix << 5; }
            w = mix ^ t;
        }
        {
            size_t n = len - i < 4u ? len - i : 4u, k;
            for (k = 0; k < n; k++) output[i + k] = (unsigned char)(w >> (8u * k));
            i += n;
        }
    }
    *olen = len;
    return 0;
}
