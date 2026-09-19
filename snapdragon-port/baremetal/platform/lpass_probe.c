/* lpass_probe.c — READ-ONLY survey of the unmodelled GCC window at 0x181c000.
 *
 * Context. The Gen 5 speaker is a TFA9897 on the BG; the BG takes only control
 * commands (CODEC_CHANNEL, working since v287). PCM rides primary MI2S and the
 * AP owns those pads -- from the watch's own DTB, gpio59 pri_mi2s_mclk_a,
 * gpio60 sck_a, gpio61 ws_a, gpio63 data1_a, all "output-high" when active, so
 * the AP side is the I2S master. Something on the AP has to clock that.
 *
 * On apq8016 the clocks would be gcc-msm8916.c's ULTAUDIO block at GCC offsets
 * 0x1c000..0x1c0b0. On this chip clock-gcc-8909.c models NOTHING in the whole
 * 0x19000..0x20000 range, so what lives at 0x1c000 here is simply unknown. The
 * v289 read shows the window is populated -- sixteen registers, mostly
 * 0x80000000 but 0x1c004 = 0x80008000 and 0x1c024 = 0x80004ff0, and an
 * unmapped window cannot return differing values at differing offsets.
 *
 * v289/v290 went further and set bit 0 in two of them on the assumption they
 * were the 8916 CBCRs. That assumption was unfounded and it hard-reset the
 * watch, twice. So this file no longer writes ANYTHING, and no longer touches
 * LPASS (0x7702000 CSR, 0x7708000 LPAIF) at all: reading an unclocked AHB
 * region hangs the fabric, and we have no evidence the region is clocked.
 *
 * Reads of GCC are safe -- GCC is clocked for every other peripheral we drive,
 * and the read stage completed cleanly on two separate boots. That is the
 * entire scope of this file now: print the window and get out.
 *
 * Identifying the registers needs a source of truth, not another guess. The
 * cheap one is the stock ROM: boot stock Wear OS, play a sound, and dump this
 * same window over adb. If the values move while audio plays, these ARE the
 * audio clocks and the dump hands us the exact programmed values to copy. */
#include "platform.h"
#if defined(PLAT_HAS_BG_QCC1110)

#define GCC_BASE  0x01800000u
#define ULT_BASE  (GCC_BASE + 0x1C000u)

/* Offsets are the 8916 ULTAUDIO layout. On this chip they are ONLY offsets --
 * the names are what the register WOULD be on apq8016, not what it is here. */
struct ult_reg { uint16_t off; const char *name_on_8916; };

static const struct ult_reg s_ult[] = {
    { 0x000u, "pcnoc_mport"   }, { 0x004u, "pcnoc_sway"    },
    { 0x010u, "ahbfabric"     }, { 0x024u, "ixfabric_lpm"  },
    { 0x028u, "ixfabric"      }, { 0x034u, "xo"            },
    { 0x04Cu, "avsync_xo"     }, { 0x050u, "stc_xo"        },
    { 0x054u, "lpaif_pri_i2s" }, { 0x068u, "lpaif_pri_cbcr"},
    { 0x06Cu, "lpaif_sec_i2s" }, { 0x080u, "lpaif_sec_cbcr"},
    { 0x084u, "lpaif_aux_i2s" }, { 0x098u, "lpaif_aux_cbcr"},
    { 0x09Cu, "digcodec"      }, { 0x0B0u, "digcodec_cbcr" },
};
#define ULT_N (sizeof s_ult / sizeof s_ult[0])

void lpass_probe_report(void)
{
    unsigned i, differing = 0;
    uint32_t first;

    con_puts("lpass: --- GCC 0x181c000 window, READ ONLY (v291) ---\n");

    first = mmio_read(ULT_BASE + s_ult[0].off);
    for (i = 0; i < ULT_N; i++) {
        uint32_t a = ULT_BASE + s_ult[i].off;
        uint32_t v = mmio_read(a);
        if (v != first) differing++;
        con_puts("lpass:   "); con_puthex(a);
        con_puts(" = "); con_puthex(v);
        con_puts("  (8916: "); con_puts(s_ult[i].name_on_8916); con_puts(")\n");
    }

    if (differing == 0u) {
        /* Every offset identical, including all-zero or all-ones, is what an
         * unmapped window looks like on this fabric. */
        con_puts("lpass: window is uniform -- looks unmapped, not a clock block\n");
    } else {
        con_puts("lpass: window is populated ("); con_putdec(differing);
        con_puts(" of "); con_putdec((uint32_t)ULT_N);
        con_puts(" differ) -- real registers, identity still UNKNOWN\n");
        con_puts("lpass: not writing to them: v289/v290 did and reset the watch\n");
    }
    con_puts("lpass: --- done (nothing written, LPASS untouched) ---\n");
}

#endif /* PLAT_HAS_BG_QCC1110 */
