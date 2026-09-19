/* mss_apr.c -- APR link to the audio DSP inside the Wear 3100 modem (2026-09-17).
 *
 * WHY: the speaker's PCM rides primary MI2S (darter/gen5 DT: sound card
 * qcom,msm-bg-audio-codec, cpu DAI msm-dai-q6-mi2s.0), and that DAI belongs to
 * the audio DSP running in the modem. The AP drives it only with APR packets
 * (AFE port, ASM stream, ADM routing) over the SMD channel "apr_audio_svc".
 * This file is step 1: prove the link by asking the DSP core whether it is up.
 *
 * Wire format = kernel 3.18 drivers/soc/qcom/qdsp6v2/apr.c + include/linux/qdsp6v2/apr.h:
 *   struct apr_hdr { u16 hdr_field; u16 pkt_size; u8 src_svc, src_domain; u16 src_port;
 *                    u8 dest_svc, dest_domain; u16 dest_port; u32 token; u32 opcode; }
 *   hdr_field = APR_HDR_FIELD(msg_type, hdr_len/4, ver 0) = (type << 8) | (5 << 4); the header is 20 B
 *   (step 1 first sent 8 << 4 and the DSP still answered, going by pkt_size)
 * q6core registers apr_register("ADSP", "CORE"): dest_domain APR_DOMAIN_ADSP (4),
 * svc APR_SVC_ADSP_CORE (3); apr_send_pkt sets src_domain APPS (5), src_svc = dest_svc.
 * q6core_is_adsp_ready(): SEQ_CMD AVCS_CMD_ADSP_EVENT_GET_STATE, no payload; the DSP
 * answers AVCS_CMDRSP_ADSP_EVENT_GET_STATE with payload[0] = 1 when ready.
 * AVCS_GET_VERSIONS: 3.18 q6core expects a u32 id (Q6 2.6/2.7/2.8); this DSP answers 80 B (see rx_packet). */
#include "platform.h"
#if defined(AUDIO_PUMP_PRIO)
#include "FreeRTOS.h"
#include "task.h"
/* chime61: only mss-boot (the one caller of mss_apr_poll) is raised; the app loop also runs
 * q6_audio_service and must never be raised -- it never blocks (chime60 starved everything). */
static TaskHandle_t s_pump_task;
#endif
#include <string.h>

#if defined(MSS_BOOT)

#define APR_MSG_TYPE_SEQ_CMD   2u
#define APR_HDR_FIELD_SEQ      (uint16_t)((APR_MSG_TYPE_SEQ_CMD << 8) | (5u << 4))   /* hdr_len = 20 B / 4 */
#define APR_DOMAIN_ADSP        4u
#define APR_DOMAIN_APPS        5u
#define APR_SVC_ADSP_CORE      3u
#define APR_BASIC_RSP_RESULT   0x000110E8u
#define AVCS_CMD_ADSP_EVENT_GET_STATE     0x0001290Cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE  0x0001290Du
#define AVCS_GET_VERSIONS                 0x00012905u
#define AVCS_GET_VERSIONS_RSP             0x00012906u

/* ---- step 2: primary TDM RX port start (the speaker's PCM path) --------------
 * The BG sound card (kernel 3.18 sound/soc/msm/msm_bg.c, DT qcom,msm-bg-audio-codec with
 * qcom,tdm-audio-intf) plays through PRIMARY TDM, not MI2S. Sequence = msm_tdm_startup +
 * msm_dai_q6_tdm_hw_params/prepare (msm-dai-q6-v2.c) + q6afe.c for one 48 kHz 16-bit
 * playback with the stock defaults (msm_pri_tdm_rx_0_ch 1; slots 4, width 16; offsets
 * {0,2,4,6} -> group slot mask 0xF). Darter DT: group 0x9100 ports 9000/9002/9004/9006,
 * clk-rate 0 + afe-ebit-unsupported (no AFE clock: the BG drives BCLK), sync mode short,
 * sync src external, data-out 0, invert 0, delay 0, sec-port-enable (the TX group 0x9101
 * and port 0x9001 are started too: "8909 HW needs a Tx port in the same group"). */
#define APR_SVC_AFE                  4u
#define AFE_SVC_CMD_SET_PARAM        0x000100F3u
#define AFE_PORT_CMD_SET_PARAM_V2    0x000100EFu
#define AFE_PORT_CMD_DEVICE_START    0x000100E5u
#define AFE_MODULE_GROUP_DEVICE      0x00010254u
#define AFE_PARAM_ID_GROUP_DEVICE_ENABLE      0x00010256u
#define AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG  0x0001029Eu
#define AFE_MODULE_AUDIO_DEV_INTERFACE        0x0001020Cu
#define AFE_PARAM_ID_TDM_CONFIG               0x0001029Du
#define AFE_MODULE_TDM                        0x0001028Au
#define AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG 0x00010297u
#define TDM_PRI_RX        0x9000u
/* The speaker's backend is PRI_TDM_RX_1, not RX_0: stock's mixer_paths_bg.xml routes every
 * playback and voice path to "PRI_TDM_RX_1 Audio Mixer ...", and msm_bg.c's LPASS_BE_PRI_TDM_RX_1
 * is cpu dai msm-dai-q6-tdm.36866 (port 0x9002) with codec dai bg_cdc_rx2. Its slot offset is
 * 2 bytes (tdm_slot_offset[PRIMARY_TDM_RX_1] = {2, invalid}) -> slot 1, mask 0x2, and the channel
 * count is msm_pri_tdm_rx_0_ch = One. Ports 0x9000/slot 0 (what we used until now, gen5-modem-8..14)
 * are simply not what the BG listens to. */
#define TDM_PRI_RX_1      0x9002u
#define TDM_SPK_PORT      TDM_PRI_RX_1
/* Port slot_mask = 0x2 (this port's slot), SLOT_MAPPING offset = 2, GROUP slot_mask = 0xF.
 * audio_machine.ko msm_tdm_snd_hw_params ORs 1 << (offset >> 1) over all four tdm_slot_offset
 * rows {0,2,4,6} = 0xF and calls snd_soc_dai_set_tdm_slot(dai, 0, 0xF, 4, 16) -- but that 0xF
 * is the GROUP mask, which grp_cfg() already sends. The per-PORT AFE_PARAM_ID_TDM_CONFIG mask
 * is this port's own slot. Proven the hard way: chime12 put 0xF on the port and the first
 * WRITE_V2 never returned WRITE_DONE (gen5-modem-19), the same stall as sync_src = 0. */
#define TDM_SPK_SLOT_MASK 0x2u
#define TDM_SPK_SLOT_OFF  2u
#define TDM_PRI_TX        0x9001u
#define TDM_GRP_PRI_RX    0x9100u
#define TDM_GRP_PRI_TX    0x9101u
#define AFE_PORT_INVALID  0xFFFFu
#define CSR_GP_IO_MUX_MIC_CTL          0x07702000u

/* ---- step 3: a 1 kHz tone through ASM -> ADM -> AFE port 0x9000 --------------
 * kernel 3.18 q6asm.c / q6adm.c: shared memory map (ASM svc, ports 0), ASM open write v3
 * (session 1 stream 1 -> ports 0x0101, legacy mode, NULL POPP), PCM media format v2,
 * ADM device open v5 (port 0x9000 RX COPP, NULL COPP topology, 48 kHz/16/mono), matrix map
 * (AUDIO_RX, session 1 -> copp), session run, then ASM_DATA_CMD_WRITE_V2 buffers from a
 * mapped block of our RAM (identity mapped: phys == virt). DSP versions from GET_VERSIONS:
 * ASM 0x00070002, ADM 0x00070000 (Q6 2.8). */
#define APR_SVC_ASM                        7u
#define APR_SVC_ADM                        8u
#define ASM_CMD_SHARED_MEM_MAP_REGIONS     0x00010D92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS  0x00010D93u
#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL     3u
#define ASM_STREAM_CMD_OPEN_WRITE_V3       0x00010DB3u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 0x00010DA5u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2   0x00010D98u
#define ASM_SESSION_CMD_RUN_V2             0x00010DAAu
#define ASM_DATA_CMD_WRITE_V2              0x00010DABu
#define ASM_DATA_EVENT_WRITE_DONE_V2       0x00010D99u
#define NULL_POPP_TOPOLOGY                 0x00010C68u
#define ADM_CMD_DEVICE_OPEN_V5             0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5          0x00010329u
#define ASM_STREAM_CMD_CLOSE               0x00010BCDu   /* chime46 teardown (q6asm_close) */
#define ADM_CMD_DEVICE_CLOSE_V5            0x00010327u   /* chime46 teardown (adm_close) */
#define AFE_PORT_CMD_DEVICE_STOP           0x000100E6u   /* chime46 teardown (afe_close) */
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5     0x00010325u
/* ADM_CMD_MATRIX_RAMP_GAINS_V5 (0x00010327, unity 0x2000) is deliberately NOT sent. Diffing the
 * "apr: ->" lines of gen5-modem-16 (chime8, the one and only log in which the DSP ever traced a
 * WRITE_V2 arriving -- "line 86 ... args 0x00010dab,0x00000407,0x00000101" -- and answered with
 * WRITE_DONE) against gen5-modem-24 shows exactly ONE difference in the whole APR sequence: this
 * command, inserted between MATRIX_MAP_ROUTINGS_V5 and SESSION_RUN_V2. It answers BASIC_RSP
 * status 0 and, per gen5-audio-tested.md, never made anything audible. Removing it puts the
 * stream back on chime8's exact command sequence. */
#define NULL_COPP_TOPOLOGY                 0x00010312u
#define PCM_CHANNEL_FC                     3u
#define ASM_PORT                           0x0101u     /* (session 1 << 8) | stream 1 */
#define TONE_CHUNK_FRAMES                  2400u       /* 50 ms at 48 kHz */
/* asm_data_cmd_write_v2.flags bit 31 = "this buffer carries no valid timestamp, render it as it
 * arrives". q6asm_write() in kernel 3.18 always sends 0x80000000 for ordinary playback; we sent 0,
 * which declares timestamp 0 VALID -- an absolute render time far in the past. The DSP still
 * consumed every buffer and returned WRITE_DONE (which is why the link looked healthy in
 * gen5-modem-8..23) but never rendered the samples. This is why full-scale tone was inaudible. */
#define ASM_WRITE_NO_TIMESTAMP             0x80000000u
/* Buffers kept in flight. One 50 ms buffer was not enough: the write is only re-armed from
 * mss_apr_poll(), and bgcom.c's codec restart busy-waits on the BG bus for seconds at a time
 * (gen5-modem-23 logged "no WRITE_DONE in 3 s"), so the port ran dry between chunks. Three
 * buffers = 150 ms of slack and they still fit in the 16 KB mapped region. */
#define WRITE_BUFS                         3u
#define TONE_CHUNK_BYTES                   (TONE_CHUNK_FRAMES * TONE_CHANNELS * 2u)
#define TONE_CHANNELS                      1u          /* the speaker BE is mono (PRI_TDM_RX_0 Channels = One) */
#define SINE_STEPS                         64u
#define CSR_GP_IO_LPAIF_PRI_PCM_MUXSEL 0x07702008u
/* -DAUDIO_TDM_MASTER (diagnostic): the DSP drives BCLK/FSYNC itself -- AFE_MODULE_CLOCK_SET on
 * Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT at 48000 * 4 slots * 16 bits, sync_src internal, mic ctl in
 * msm_bg.c's "master mode" value. Stock (DT clk-internal 0, sync-src 0) is slave: the BG clocks.
 * gen5-modem-8/9.txt: slave mode never returned the first WRITE_DONE; this splits DSP from BG. */
#define AFE_MODULE_CLOCK_SET               0x0001028Fu
#define AFE_PARAM_ID_CLOCK_SET             0x00010290u
#define Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT    0x200u

struct apr_hdr {
    uint16_t hdr_field, pkt_size;
    uint8_t  src_svc, src_domain; uint16_t src_port;
    uint8_t  dest_svc, dest_domain; uint16_t dest_port;
    uint32_t token, opcode;
} __attribute__((packed));

struct pdata_v2 { uint32_t module_id, param_id; uint16_t param_size, reserved; } __attribute__((packed));
struct grp_tdm_cfg {                      /* afe_param_id_group_device_tdm_cfg, 44 B (= the union size) */
    uint32_t minor; uint16_t group_id, reserved; uint16_t port_id[8];
    uint32_t num_channels, sample_rate, bit_width; uint16_t nslots_per_frame, slot_width; uint32_t slot_mask;
} __attribute__((packed));
struct tdm_cfg {                          /* afe_param_id_tdm_cfg, 36 B */
    uint32_t minor, num_channels, sample_rate, bit_width;
    uint16_t data_format, sync_mode, sync_src, nslots_per_frame, ctrl_data_out_enable, ctrl_invert_sync_pulse,
             ctrl_sync_data_delay, slot_width;
    uint32_t slot_mask;
} __attribute__((packed));
struct slot_map_cfg {                     /* afe_param_id_slot_mapping_cfg, 28 B */
    uint32_t minor; uint16_t num_channel, bitwidth; uint32_t data_align_type; uint16_t offset[8];
} __attribute__((packed));

_Static_assert(sizeof(struct apr_hdr) == 20, "apr_hdr");
_Static_assert(sizeof(struct pdata_v2) == 12, "afe_port_param_data_v2");
_Static_assert(sizeof(struct grp_tdm_cfg) == 44, "afe_param_id_group_device_tdm_cfg");
_Static_assert(sizeof(struct tdm_cfg) == 36, "afe_param_id_tdm_cfg");
_Static_assert(sizeof(struct slot_map_cfg) == 28, "afe_param_id_slot_mapping_cfg");

_Static_assert(sizeof(struct apr_hdr) + 8u + 12u == 40u, "mem map regions");

static void say(const char *s) { con_puts(s); }
static void hex(const char *s, uint32_t v) { con_puts(s); con_puthex(v); }
static void dec(const char *s, uint32_t v) { con_puts(s); con_putdec(v); }
#if defined(AUDIO_L11_READBACK)
/* pm660_l11 = vdd-spkr (triggerfish-stock.dts: bg_codec vdd-spkr-supply = <0x8b> = pm660_l11,
 * hpm-min-load 10 mA). PM660 LDO11 peripheral: SID 1, base 0x4A00. Read what the PMIC is actually
 * doing while a sound plays: 0x04/05 type/subtype, 0x08 STATUS (bit7 VREG_OK), 0x40/41 voltage,
 * 0x45 MODE_CTL (bit7 HPM/NPM), 0x46 EN_CTL (bit7 enable). */
static void l11_report(const char *when)
{
    uint8_t id[2] = {0}, st = 0, v[2] = {0}, mode = 0, en = 0;
    int rc = spmi_read(1u, 0x4A04u, id, 2);
    rc |= spmi_read8(1u, 0x4A08u, &st);
    rc |= spmi_read(1u, 0x4A40u, v, 2);
    rc |= spmi_read8(1u, 0x4A45u, &mode);
    rc |= spmi_read8(1u, 0x4A46u, &en);
    say("l11 (vdd-spkr) "); say(when);
    hex(": type ", id[0]); hex(" sub ", id[1]); hex(" status ", st); hex(" volt ", (uint32_t)v[0] | ((uint32_t)v[1] << 8));
    hex(" mode ", mode); hex(" en ", en); dec(" rc ", (uint32_t)(rc < 0));
    /* COMMON2 LDO (qpnp-regulator.c): mode 7 HPM, 6 AUTO, 5 LPM, 4 RET, 3 BYP */
    say(!(en & 0x80u) ? " -> OFF\n" : (mode & 7u) == 7u ? " -> ON, HPM\n" : (mode & 7u) == 6u ? " -> ON, AUTO\n" : (mode & 7u) == 5u ? " -> ON, LPM\n" : " -> ON, RET/BYP\n");
}
#endif

static int s_ready = -1;            /* -1 not asked yet, 0 asked/not ready, 1 ready */
static int s_ver_asked;
static uint32_t s_polls, s_asks, s_token;
static int s_step2 = -1;                  /* -1 not started, 0..9 command in flight, 99 done, 98 failed */

static int core_cmd(struct smd_chan *ch, uint32_t opcode, const char *what)
{
    struct apr_hdr h;
    memset(&h, 0, sizeof h);
    h.hdr_field = APR_HDR_FIELD_SEQ;
    h.pkt_size = sizeof h;
    h.src_svc = APR_SVC_ADSP_CORE; h.src_domain = APR_DOMAIN_APPS;
    h.dest_svc = APR_SVC_ADSP_CORE; h.dest_domain = APR_DOMAIN_ADSP;
    h.token = ++s_token; h.opcode = opcode;
    int rc = smd_send(ch, &h, sizeof h);
    say("apr: -> "); say(what); hex(" opcode ", opcode); dec(" token ", h.token);
    dec(" rc ", (uint32_t)(rc < 0 ? -rc : 0)); say(rc < 0 ? " (send FAILED)\n" : "\n");
    return rc;
}

static struct smd_chan *s_ch;
static void step2_begin(struct smd_chan *ch);
#if defined(AUDIO_DSP_AFTER_MODEM)
/* chime45: step 2/3 (TDM port, clock, session) wait until the modem has finished starting, i.e. the
 * loading screen has handed over (arduino_main.cpp: modem ready + 8 s -> "mss-gate: app starts"). */
extern int mss_gate_done(void);
static int s_step2_armed;
static uint32_t s_step2_armed_ms;
#endif
#if defined(AUDIO_DSP_PER_SOUND)
#if !defined(AUDIO_DSP_AFTER_MODEM)
#error "AUDIO_DSP_PER_SOUND needs AUDIO_DSP_AFTER_MODEM (the loading-screen gate)"
#endif
#if defined(AUDIO_CODEC_WITH_DSP) && !defined(AUDIO_PER_SOUND)
#error "AUDIO_CODEC_WITH_DSP needs AUDIO_PER_SOUND (bgcom.c's speaker on/off and bus lock)"
#endif
/* chime46: the DSP side (TDM port + clock, ASM/ADM session) is brought up for every sound and torn
 * down AUDIO_IDLE_MS after the last real sample, as stock does per stream (soc-pcm open/close).
 * gen5-modem-48: the stream started at the loading-screen hand-over played the boot tone; the same
 * stream left running was silent for the alarm a minute later. */
#ifndef AUDIO_IDLE_MS
#define AUDIO_IDLE_MS 2500u            /* chime49 (user): the path stays up 2-3 s after a sound has PLAYED */
#endif
/* chime49: the DSP returns WRITE_DONE far faster than real time (gen5-modem-51: 3 s of tone "done"
 * in ~0.5 s), so "last buffer accepted" is not "last sample played". The session runs from
 * s_run_ms at 48 kHz, so the last real sample written plays out at s_run_ms + s_real_end_frames / 48. */
static uint32_t s_run_ms, s_frames_total, s_real_end_frames;
#ifndef AUDIO_LEAD_MS
#define AUDIO_LEAD_MS 150u                 /* chime50: how far ahead of real time we let the DSP get */
#endif
static volatile int s_want_up;             /* a sound is waiting for the DSP side to come up */
static volatile int s_app_active;          /* chime51: the app says a sound is live (q6_audio_active) */
static int s_closing;                      /* idle: no new buffers, waiting for the in-flight ones */
static uint32_t s_closing_ms, s_last_audio_ms;
static int s_step4 = -1;                   /* teardown: -1 idle, 0.. command in flight */
static void step4_rsp(struct smd_chan *ch, uint32_t opcode, uint32_t status);
static int dsp_can_play(void);
#endif
static void step3_rsp(struct smd_chan *ch, uint32_t opcode, uint32_t status, uint32_t value);
static void step3_write_done(struct smd_chan *ch, uint32_t status);
void q6_audio_tone(uint32_t hz, uint32_t ms);
static void step2_rsp(struct smd_chan *ch, uint32_t opcode, uint32_t status);
static void step3_begin(struct smd_chan *ch);
static void rx_packet(const uint8_t *p, uint32_t n)
{
    struct apr_hdr h;
    if (n < sizeof h) { dec("apr: <- short packet ", n); say("\n"); return; }
    memcpy(&h, p, sizeof h);
    uint32_t hlen = ((h.hdr_field >> 4) & 0xFu) * 4u;
    const uint8_t *pl = p + hlen;
    uint32_t plen = n > hlen ? n - hlen : 0u, w0 = 0, w1 = 0;
    if (plen >= 4u) memcpy(&w0, pl, 4u);
    if (plen >= 8u) memcpy(&w1, pl + 4u, 4u);
    if (h.opcode == ASM_DATA_EVENT_WRITE_DONE_V2) {   /* quiet: handled below without a line */
        uint32_t st = 0; if (plen >= 16u) memcpy(&st, pl + 12u, 4u);
        if (st) { hex("apr: <- WRITE_DONE status ", st); say("\n"); }
        step3_write_done(s_ch, st);
        return;
    }
    say("apr: <- "); hex("opcode ", h.opcode); dec(" from svc ", h.src_svc); dec(" domain ", h.src_domain);
    dec(" token ", h.token); dec(" size ", h.pkt_size); dec(" payload ", plen);
    if (h.opcode == AVCS_CMDRSP_ADSP_EVENT_GET_STATE) {
        s_ready = w0 ? 1 : 0;
        say(w0 ? " = ADSP_EVENT_GET_STATE: audio DSP READY" : " = ADSP_EVENT_GET_STATE: audio DSP not ready yet");
    } else if (h.opcode == AVCS_GET_VERSIONS_RSP && plen == 4u) {
        hex(" = GET_VERSIONS: id ", w0);
        say(w0 == 0x00040000u ? " (Q6 2.6)" : w0 == 0x00040001u ? " (Q6 2.7)" : w0 == 0x00040002u ? " (Q6 2.8)" : " (unknown)");
    } else if (h.opcode == AVCS_GET_VERSIONS_RSP && plen >= 8u) {
        /* gen5-modem-6.txt: 80 B payload, not the 3.18 single id -- the newer layout
         * { u32 build_id; u32 num_services; { u32 service_id; u32 version } [num] } (8 + 9 * 8) */
        hex(" = GET_VERSIONS: build ", w0); dec(" services ", w1); say("\n");
        for (uint32_t i = 0; i < w1 && 8u + (i + 1u) * 8u <= plen; i++) {
            uint32_t id, ver; memcpy(&id, pl + 8u + i * 8u, 4u); memcpy(&ver, pl + 12u + i * 8u, 4u);
            hex("apr:    service ", id); hex(" version ", ver); say("\n");
        }
        say("apr: step 1 done -- audio DSP link works\n");
#if defined(AUDIO_DSP_PER_SOUND)
        say("apr: DSP ready -- the port and session come up per sound, after the loading screen\n");
#elif defined(AUDIO_DSP_AFTER_MODEM)
        if (s_step2 < 0 && !s_step2_armed) {
            s_step2_armed = 1; s_step2_armed_ms = timer_ms();
            say("apr: DSP port/session start WAITS for the loading screen to hand over (modem finished starting)\n");
        }
#else
        if (s_step2 < 0) step2_begin(s_ch);
#endif
        return;
    } else if (h.opcode == ASM_CMDRSP_SHARED_MEM_MAP_REGIONS) {
        hex(" = SHARED_MEM_MAP_REGIONS: handle ", w0); say("\n");
        step3_rsp(s_ch, ASM_CMD_SHARED_MEM_MAP_REGIONS, w0 ? 0u : 1u, w0); return;
    } else if (h.opcode == ADM_CMDRSP_DEVICE_OPEN_V5) {
        hex(" = ADM DEVICE_OPEN_V5: status ", w0); hex(" copp_id ", w1 & 0xFFFFu); say("\n");
        step3_rsp(s_ch, ADM_CMD_DEVICE_OPEN_V5, w0, w1 & 0xFFFFu); return;
    } else if (h.opcode == ASM_DATA_EVENT_WRITE_DONE_V2) {
        uint32_t st = 0; if (plen >= 16u) memcpy(&st, pl + 12u, 4u);
        /* one of these per 50 ms buffer forever: only a failure is worth a line */
        step3_write_done(s_ch, st); if (!st) return;
    } else if (h.opcode == APR_BASIC_RSP_RESULT) {
        hex(" = BASIC_RSP for opcode ", w0); hex(" status ", w1);
#if defined(AUDIO_DSP_PER_SOUND)
        if (s_step4 >= 0) { say("\n"); step4_rsp(s_ch, w0, w1); return; }
#endif
        if (h.src_svc == APR_SVC_AFE) { say("\n"); step2_rsp(s_ch, w0, w1); return; }
        if (h.src_svc == APR_SVC_ASM || h.src_svc == APR_SVC_ADM) { say("\n"); step3_rsp(s_ch, w0, w1, 0u); return; }
    } else {
        say(" data:");
        for (uint32_t k = 0; k < plen && k < 24u; k += 4u) { uint32_t v = 0; memcpy(&v, pl + k, plen - k >= 4u ? 4u : plen - k); hex(" ", v); }
    }
    say("\n");
}

static uint8_t s_pkt[256];
static uint32_t s_wait_opcode;            /* the opcode whose BASIC_RSP we wait for; 0 = none */
static uint32_t s_sent_ms;                /* when the command in flight went out (step 2/3 timeout) */

static struct apr_hdr *afe_hdr(uint32_t opcode, uint32_t total)
{
    struct apr_hdr *h = (struct apr_hdr *)s_pkt;
    memset(s_pkt, 0, total);
    h->hdr_field = APR_HDR_FIELD_SEQ; h->pkt_size = (uint16_t)total;
    h->src_svc = APR_SVC_AFE; h->src_domain = APR_DOMAIN_APPS;
    h->dest_svc = APR_SVC_AFE; h->dest_domain = APR_DOMAIN_ADSP;
    h->token = ++s_token; h->opcode = opcode;
    return h;
}
static int afe_send(struct smd_chan *ch, uint32_t total, const char *what)
{
    struct apr_hdr *h = (struct apr_hdr *)s_pkt;
    s_wait_opcode = h->opcode;
    int rc = smd_send(ch, s_pkt, total);
    say("apr: -> AFE "); say(what); hex(" opcode ", h->opcode); dec(" token ", h->token); dec(" size ", total);
    dec(" rc ", (uint32_t)(rc < 0 ? -rc : 0)); say(rc < 0 ? " (send FAILED)\n" : "\n");
    return rc;
}
/* AFE_SVC_CMD_SET_PARAM: hdr (20) + {payload_size, addr_lsw, addr_msw, mem_map_handle} + pdata + data */
static int afe_svc_param(struct smd_chan *ch, uint32_t module, uint32_t param, const void *data, uint16_t dlen, const char *what)
{
    uint32_t total = 20u + 16u + 12u + dlen;
    afe_hdr(AFE_SVC_CMD_SET_PARAM, total);
    uint32_t sp[4] = { 12u + dlen, 0, 0, 0 };
    struct pdata_v2 pd = { module, param, dlen, 0 };
    memcpy(s_pkt + 20u, sp, 16u); memcpy(s_pkt + 36u, &pd, 12u); memcpy(s_pkt + 48u, data, dlen);
    return afe_send(ch, total, what);
}
/* AFE_PORT_CMD_SET_PARAM_V2: hdr (20) + {u16 port_id, u16 payload_size, addr_lsw, addr_msw, handle} + pdata + data */
static int afe_port_param(struct smd_chan *ch, uint16_t port, uint32_t module, uint32_t param, const void *data, uint16_t dlen, const char *what)
{
    uint32_t total = 20u + 16u + 12u + dlen;
    afe_hdr(AFE_PORT_CMD_SET_PARAM_V2, total);
    uint16_t pp[2] = { port, (uint16_t)(12u + dlen) }; uint32_t pz[3] = { 0, 0, 0 };
    struct pdata_v2 pd = { module, param, dlen, 0 };
    memcpy(s_pkt + 20u, pp, 4u); memcpy(s_pkt + 24u, pz, 12u); memcpy(s_pkt + 36u, &pd, 12u); memcpy(s_pkt + 48u, data, dlen);
    return afe_send(ch, total, what);
}
static int afe_port_start(struct smd_chan *ch, uint16_t port, const char *what)
{
    afe_hdr(AFE_PORT_CMD_DEVICE_START, 24u);
    uint16_t pr[2] = { port, 0 }; memcpy(s_pkt + 20u, pr, 4u);
    return afe_send(ch, 24u, what);
}
static void grp_cfg(struct grp_tdm_cfg *g, uint16_t group, uint16_t first_port)
{
    memset(g, 0, sizeof *g);
    g->minor = 1u; g->group_id = group;
    for (unsigned i = 0; i < 8u; i++) g->port_id[i] = i < 4u ? (uint16_t)(first_port + 2u * i) : AFE_PORT_INVALID;
    g->num_channels = 4u; g->sample_rate = 48000u; g->bit_width = 16u;   /* hw_params: = nslots / slot_width */
    g->nslots_per_frame = 4u; g->slot_width = 16u; g->slot_mask = 0xFu;
}
static void port_cfg(struct tdm_cfg *t, uint32_t channels, uint32_t mask)
{
    memset(t, 0, sizeof *t);
    t->minor = 1u; t->num_channels = channels; t->sample_rate = 48000u; t->bit_width = 16u;
    t->nslots_per_frame = 4u; t->slot_width = 16u; t->slot_mask = mask;   /* format/sync/oe/invert/delay = 0 (DT) */
    /* sync_src = 1 (AFE_PORT_TDM_SYNC_SRC_INTERNAL). The DT says 0, but 0 does not work:
     * gen5-modem-17 (no clock, sync_src 0) and gen5-modem-18 (PRI_TDM_IBIT clock on,
     * sync_src 0) BOTH stall -- the first WRITE_V2 never returns WRITE_DONE. Only
     * sync_src 1 transports (gen5-modem-16). So the DSP must drive the frame sync here
     * whatever the DT claims, and the silence is downstream of the TDM link.
     * chime48 (gen5-modem-50): with the codec STARTed after the port (stock order) the alarm came
     * out as a garbled buzz -- consistent with the DSP AND the BG both driving BCLK/FSYNC. Without
     * -DAUDIO_TDM_MASTER the port is configured as stock: DSP slave, sync from the BG (0). */
#if defined(AUDIO_TDM_MASTER)
    t->sync_src = 1u;
#else
    t->sync_src = 0u;
#endif
}

/* Send command n of step 2. Returns 1 sent, 0 no such command (done), -1 send failed. */
static int step2_cmd(struct smd_chan *ch, int n)
{
    struct grp_tdm_cfg g, en; struct tdm_cfg t; struct slot_map_cfg m;
    memset(&en, 0, sizeof en);                                          /* group enable {u16 id, u16 enable}, union-sized */
    memset(&m, 0, sizeof m); m.minor = 1u; m.num_channel = (uint16_t)TONE_CHANNELS; m.bitwidth = 16u;
    for (unsigned i = 0; i < 8u; i++) m.offset[i] = i < TONE_CHANNELS ? (uint16_t)TDM_SPK_SLOT_OFF : 0xFFFFu;
    int rc;
#if defined(AUDIO_TDM_MASTER)
    if (n == 0) {   /* afe_clk_set {minor 1, clk_id, freq, u16 attri COUPLE_NO, u16 root 0, enable 1} */
        uint32_t c[5] = { 1u, Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT, 48000u * 4u * 16u, 1u, 1u };
        rc = afe_svc_param(ch, AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, c, sizeof c, "CLOCK_SET PRI_TDM_IBIT 3.072 MHz (MASTER test)");
        return rc < 0 ? -1 : 1;
    }
    n--;
#endif
#if defined(AUDIO_STOCK_ORDER)
    /* Stock playback (msm-dai-q6-v2.c msm_dai_q6_tdm_prepare): only the RX group and the speaker
     * port; the TX port 0x9001 is started only for capture. */
    { static const int k_rx_only[] = { 0, 1, 4, 5, 6 };
      if (n >= (int)(sizeof k_rx_only / sizeof k_rx_only[0])) return 0;
      n = k_rx_only[n]; }
#endif
    switch (n) {
    case 0: grp_cfg(&g, TDM_GRP_PRI_RX, TDM_PRI_RX);
            rc = afe_svc_param(ch, AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG, &g, sizeof g, "group 0x9100 TDM_CONFIG"); break;
    case 1: { uint16_t e[2] = { TDM_GRP_PRI_RX, 1u }; memcpy(&en, e, 4u); }
            rc = afe_svc_param(ch, AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_ENABLE, &en, sizeof en, "group 0x9100 ENABLE"); break;
    case 2: grp_cfg(&g, TDM_GRP_PRI_TX, TDM_PRI_TX);
            rc = afe_svc_param(ch, AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG, &g, sizeof g, "group 0x9101 TDM_CONFIG"); break;
    case 3: { uint16_t e[2] = { TDM_GRP_PRI_TX, 1u }; memcpy(&en, e, 4u); }
            rc = afe_svc_param(ch, AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_ENABLE, &en, sizeof en, "group 0x9101 ENABLE"); break;
    case 4: port_cfg(&t, TONE_CHANNELS, TDM_SPK_SLOT_MASK);
            /* Print what we actually configure, so a log identifies its own build. Three
             * separate boots (chime9/10/12) were spent re-testing values the log could not
             * distinguish. */
            hex("apr: TDM port cfg: port ", TDM_SPK_PORT); hex(" slot_mask ", t.slot_mask);
            dec(" nslots ", t.nslots_per_frame); dec(" slot_w ", t.slot_width);
            dec(" ch ", t.num_channels); dec(" sync_src ", t.sync_src);
            hex(" grp_mask ", 0xFu); dec(" offset ", TDM_SPK_SLOT_OFF); say("\n");
            rc = afe_port_param(ch, TDM_SPK_PORT, AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_TDM_CONFIG, &t, sizeof t, "port 0x9002 TDM_CONFIG (speaker BE)"); break;
    case 5: rc = afe_port_param(ch, TDM_SPK_PORT, AFE_MODULE_TDM, AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG, &m, sizeof m, "port 0x9002 SLOT_MAPPING (slot 1)"); break;
    case 6: rc = afe_port_start(ch, TDM_SPK_PORT, "port 0x9002 DEVICE_START"); break;
    case 7: port_cfg(&t, 1u, 0x1u);                                   /* prepare: sec port num_channels 1, slot_mask 1 */
            rc = afe_port_param(ch, TDM_PRI_TX, AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_TDM_CONFIG, &t, sizeof t, "port 0x9001 TDM_CONFIG"); break;
    case 8: rc = afe_port_param(ch, TDM_PRI_TX, AFE_MODULE_TDM, AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG, &m, sizeof m, "port 0x9001 SLOT_MAPPING"); break;
    case 9: rc = afe_port_start(ch, TDM_PRI_TX, "port 0x9001 DEVICE_START"); break;
    default: return 0;
    }
    return rc < 0 ? -1 : 1;
}

static void step2_begin(struct smd_chan *ch)
{
    say("apr: --- step 2: primary TDM RX port start (speaker PCM path) ---\n");
    /* msm_tdm_startup: pri PCM mux -> TDM, mic ctl slave-mode bits */
    uint32_t a = mmio_read(CSR_GP_IO_LPAIF_PRI_PCM_MUXSEL), b = mmio_read(CSR_GP_IO_MUX_MIC_CTL);
    mmio_write(CSR_GP_IO_LPAIF_PRI_PCM_MUXSEL, a | 0x00000001u);
#if defined(AUDIO_TDM_MASTER)
    mmio_write(CSR_GP_IO_MUX_MIC_CTL, b | 0x02020002u);                 /* msm_bg.c: "Use this value for master mode" */
#else
    mmio_write(CSR_GP_IO_MUX_MIC_CTL, b | 0x01808000u);
#endif
    hex("apr: csr pri_pcm_muxsel ", a); hex(" -> ", mmio_read(CSR_GP_IO_LPAIF_PRI_PCM_MUXSEL));
    hex(", mux_mic_ctl ", b); hex(" -> ", mmio_read(CSR_GP_IO_MUX_MIC_CTL)); say("\n");
    /* The TDM lines are gpio0-3 on function sec_mi2s (2). Gen 5 / darter DT: the sound card's
     * qcom,pri-mi2s-gpios -> cdc_dmic_pinctrl "aud_active" = quat_mi2s_active (gpio0/1, sec_mi2s,
     * 8 mA, bias-disable) + quat_mi2s_din_active (gpio2/3, sec_mi2s, 8 mA, bias-disable, output-high).
     * gen5-modem-8.txt: every ASM/ADM command accepted but the first 100 ms buffer never came back
     * (no WRITE_DONE in 3 s) -- the AFE port never clocked; our firmware never muxed these pins. */
    hex("apr: tlmm gpio0..3 cfg before ", mmio_read(0x01000000u)); hex(" ", mmio_read(0x01001000u));
    hex(" ", mmio_read(0x01002000u)); hex(" ", mmio_read(0x01003000u)); say("\n");
    for (uint32_t pin = 0; pin < 4u; pin++) {
        tlmm_cfg(pin, 2u, 0u, 8u, pin >= 2u);
        if (pin >= 2u) tlmm_out(pin, 1);
    }
    hex("apr: tlmm gpio0..3 cfg after  ", mmio_read(0x01000000u)); hex(" ", mmio_read(0x01001000u));
    hex(" ", mmio_read(0x01002000u)); hex(" ", mmio_read(0x01003000u)); say(" (sec_mi2s = TDM lines)\n");
#if defined(AUDIO_STOCK_ORDER)
    /* Stock ASoC order (soc-pcm.c): BE hw_params -> bg_cdc_hw_params (SET_PARAMS, cal first time);
     * BE prepare -> codec dai prepare (bg_cdc_prepare: START) BEFORE cpu dai prepare
     * (msm_dai_q6_tdm_prepare: group enable, port TDM config, DEVICE_START). The BG is the TDM
     * clock master, so its clock runs before the DSP's slave port starts. The pin/mux setup above
     * is msm_tdm_startup, which runs at open, before both. */
    say("apr: stock order -- codec SET_PARAMS + START before the AFE group/port start\n");
    { extern int bgcom_speaker_on(const char *why); (void)bgcom_speaker_on("stock order: before the AFE port"); }
#endif
    s_step2 = 0;
    if (step2_cmd(ch, 0) < 0) s_step2 = 98;
    s_sent_ms = timer_ms();
}
static void step2_rsp(struct smd_chan *ch, uint32_t opcode, uint32_t status)
{
    if (s_step2 < 0 || s_step2 >= 98 || opcode != s_wait_opcode) return;
    s_wait_opcode = 0;
    if (status) { dec("apr: step 2 FAILED at command ", (uint32_t)s_step2); hex(" (DSP status ", status); say(") -- stopping\n"); s_step2 = 98; return; }
    s_step2++;
    int r = step2_cmd(ch, s_step2);
    if (r == 0) {
#if defined(AUDIO_STOCK_ORDER)
        say("apr: step 2 done -- speaker port 0x9002 (PRI_TDM_RX_1) STARTED, no TX port (stock playback)\n"); s_step2 = 99;
#else
        say("apr: step 2 done -- speaker port 0x9002 (PRI_TDM_RX_1) and TX 0x9001 STARTED\n"); s_step2 = 99;
#endif
#if defined(AUDIO_CODEC_WITH_DSP) && !defined(AUDIO_STOCK_ORDER)
        /* chime47: the port is clocking -> codec SET_PARAMS + START, then the session (stock order) */
        { extern int bgcom_speaker_on(const char *why); (void)bgcom_speaker_on("the DSP port is up"); }
#endif
        step3_begin(ch); return;
    }
    else if (r < 0) s_step2 = 98;
    s_sent_ms = timer_ms();
}

/* ---- step 3 ---------------------------------------------------------------- */
volatile int g_audio_tone_state;          /* 0 idle, 1 setting up / playing, 2 done, 3 failed (bgcom.c holds the amp) */
/* -1 not started, 0..6 a setup command in flight (6 = SESSION_RUN_V2 itself), STEP3_RUN = the
 * stream is up and we keep one WRITE_V2 in flight, 98 failed. gen5-modem-21: the assignment here
 * said 6 while q6_audio_ready()/q6_audio_space() tested 7, so audio_alarm.h always took its
 * "[audio] Q6 stream not up" path and every buffer we wrote was silence. gen5-modem-22: making
 * the running state 6 instead is NOT the fix -- 6 is a live command index, so step3_rsp() then
 * drops RUN_V2's own reply and the stream never comes up at all. The running state must be 7. */
#define STEP3_RUN 7
static int s_step3 = -1;
static uint32_t s_mmap_handle, s_copp_id, s_seq, s_write_misses;
static uint32_t s_wr_buf, s_inflight, s_last_done_ms, s_dones;
/* How long the DSP holds each buffer (send -> WRITE_DONE). The DSP is TDM slave: clocked by the BG
 * it must hold a buffer until it has played (~lead time, 100+ ms); with no BG clock it returns
 * them at once. Logged with the per-20 WRITE_DONE line, boot tone vs app sound. */
static uint32_t s_sent_at[32], s_sent_n, s_lat_sum, s_lat_max, s_lat_min, s_gap_prev, s_gap_max, s_gap_late;   /* next region slot, buffers the DSP owns */
static uint8_t s_tone[16384] __attribute__((aligned(4096)));   /* the mapped region: 4 KB pages */
_Static_assert(WRITE_BUFS * TONE_CHUNK_BYTES <= sizeof(uint8_t[16384]), "write buffers fit the mapped region");
/* one period of a sine, 64 steps, peak 8000 of 32767 (about -12 dBFS), scaled by TONE_GAIN.
 * -DAUDIO_TONE_GAIN=n overrides: 1 = -12 dBFS, 4 = near full scale (32000 peak, ~-0.2 dBFS). */
#ifndef AUDIO_TONE_GAIN
#define AUDIO_TONE_GAIN 4
#endif
/* chime44: -DAUDIO_QUIET divides the tone by 10 (-20 dB, peak 3200) to test for TFA9897 speaker
 * protection muting the amp after a loud sustained tone. */
#if defined(AUDIO_QUIET)
#define AUDIO_TONE_DIV 10
#else
#define AUDIO_TONE_DIV 1
#endif
static const int16_t k_sine[SINE_STEPS] = {
    0,784,1560,2321,3061,3770,4444,5075,5657,6185,6653,7057,7393,7657,7848,7962,8000,7962,7848,7657,7393,7057,6653,6185,
    5657,5075,4444,3770,3061,2321,1560,784,0,-784,-1560,-2321,-3061,-3770,-4444,-5075,-5657,-6185,-6653,-7057,-7393,-7657,
    -7848,-7962,-8000,-7962,-7848,-7657,-7393,-7057,-6653,-6185,-5657,-5075,-4444,-3770,-3061,-2321,-1560,-784 };
/* what the app asked for (q6_audio_tone): phase in 16.16, step per sample, chunks left to write */
static uint32_t s_tone_hz, s_tone_left_ms, s_phase, s_phase_step;
/* PCM the app renders itself (q6_audio_push): a ring of stereo frames the write loop drains.
 * 24000 frames = 500 ms at 48 kHz, so the app can refill from its main loop without gaps. */
#define RING_FRAMES 144000u   /* chime51: 3 s -- the app's loop blocks for seconds on WiFi/NTP/weather */
static int16_t s_ring[RING_FRAMES * 2u];
static int s_app_pcm_seen; static int16_t s_app_pcm_peak;   /* per-sound console report (step3_write) */
static volatile int s_sleeping;   /* 2026-09-18: no buffers in flight across a system power collapse */
static volatile uint32_t s_ring_rd, s_ring_wr;   /* frame indices, wr - rd = frames queued */

static void cache_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + n;
    for (; a < end; a += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static struct apr_hdr *hdr3(uint8_t svc, uint16_t port, uint32_t opcode, uint32_t total)
{
    struct apr_hdr *h = afe_hdr(opcode, total);
    h->src_svc = svc; h->dest_svc = svc; h->src_port = port; h->dest_port = port;
    return h;
}
static int send3(struct smd_chan *ch, uint32_t total, uint32_t wait, const char *what)
{
    struct apr_hdr *h = (struct apr_hdr *)s_pkt;
    s_wait_opcode = wait;
    int rc = smd_send(ch, s_pkt, total);
    say("apr: -> "); say(h->dest_svc == APR_SVC_ADM ? "ADM " : "ASM "); say(what); hex(" opcode ", h->opcode);
    dec(" token ", h->token); dec(" size ", total); dec(" rc ", (uint32_t)(rc < 0 ? -rc : 0)); say(rc < 0 ? " (send FAILED)\n" : "\n");
    return rc;
}
static void put16(uint32_t off, uint16_t v) { memcpy(s_pkt + off, &v, 2u); }
static void put32(uint32_t off, uint32_t v) { memcpy(s_pkt + off, &v, 4u); }

static int step3_cmd(struct smd_chan *ch, int n)
{
    int rc;
    switch (n) {
    case 0: /* avs_cmd_shared_mem_map_regions {u16 pool, u16 num_regions, u32 property} + {lsw, msw, size} */
        hdr3(APR_SVC_ASM, 0u, ASM_CMD_SHARED_MEM_MAP_REGIONS, 40u);
        put16(20u, ADSP_MEMORY_MAP_SHMEM8_4K_POOL); put16(22u, 1u); put32(24u, 0u);
        put32(28u, (uint32_t)(uintptr_t)s_tone); put32(32u, 0u); put32(36u, sizeof s_tone);
        rc = send3(ch, 40u, ASM_CMD_SHARED_MEM_MAP_REGIONS, "SHARED_MEM_MAP_REGIONS (16 KB tone buffer)"); break;
    case 1: /* asm_stream_cmd_open_write_v3 {u32 mode, u16 sink, u16 bits, u32 popp, u32 fmt} */
        hdr3(APR_SVC_ASM, ASM_PORT, ASM_STREAM_CMD_OPEN_WRITE_V3, 36u);
        put32(20u, 0u); put16(24u, 0u); put16(26u, 16u); put32(28u, NULL_POPP_TOPOLOGY); put32(32u, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2);
        rc = send3(ch, 36u, ASM_STREAM_CMD_OPEN_WRITE_V3, "STREAM_OPEN_WRITE_V3 (PCM 16-bit, session 1)"); break;
    case 2: /* asm_multi_channel_pcm_fmt_blk_v2 {u32 blk_size, u16 ch, u16 bits, u32 rate, u16 signed, u16 rsvd, u8 map[8]} */
        hdr3(APR_SVC_ASM, ASM_PORT, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, 44u);
        put32(20u, 20u); put16(24u, (uint16_t)TONE_CHANNELS); put16(26u, 16u); put32(28u, 48000u); put16(32u, 1u); put16(34u, 0u);
        s_pkt[36] = PCM_CHANNEL_FC;
        rc = send3(ch, 44u, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, "MEDIA_FMT_UPDATE_V2 (48 kHz mono)"); break;
    case 3: /* adm_cmd_device_open_v5 {u16 flags, u16 mode, u16 ep1, u16 ep2, u32 topo, u16 ch, u16 bits, u32 rate, u8 map[8]} */
        hdr3(APR_SVC_ADM, TDM_SPK_PORT, ADM_CMD_DEVICE_OPEN_V5, 48u);
        ((struct apr_hdr *)s_pkt)->token = 0u;                          /* port_idx << 16 | copp_idx */
        put16(20u, 0u); put16(22u, 1u); put16(24u, TDM_SPK_PORT); put16(26u, 0xFFFFu); put32(28u, NULL_COPP_TOPOLOGY);
        put16(32u, (uint16_t)TONE_CHANNELS); put16(34u, 16u); put32(36u, 48000u); s_pkt[40] = PCM_CHANNEL_FC;
        rc = send3(ch, 48u, ADM_CMD_DEVICE_OPEN_V5, "DEVICE_OPEN_V5 (port 0x9002 RX COPP, mono)"); break;
    case 4: /* adm_cmd_matrix_map_routings_v5 {u32 matrix, u32 num_sessions} + {u16 session, u16 num_copps} + copp ids (u32-sized) */
        hdr3(APR_SVC_ADM, 0u, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, 36u);
        ((struct apr_hdr *)s_pkt)->src_svc = 0u; ((struct apr_hdr *)s_pkt)->token = 0u;
        put32(20u, 0u); put32(24u, 1u); put16(28u, 1u); put16(30u, 1u); put16(32u, (uint16_t)s_copp_id);
        rc = send3(ch, 36u, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, "MATRIX_MAP_ROUTINGS_V5 (session 1 -> copp)"); break;
    case 5: /* asm_session_cmd_run_v2 {u32 flags, u32 time_lsw, u32 time_msw} */
        hdr3(APR_SVC_ASM, ASM_PORT, ASM_SESSION_CMD_RUN_V2, 32u);
        rc = send3(ch, 32u, ASM_SESSION_CMD_RUN_V2, "SESSION_RUN_V2 (immediate)"); break;
    default: return 0;
    }
    return rc < 0 ? -1 : 1;
}

static int step3_write(struct smd_chan *ch)
{
    uint32_t off = s_wr_buf * TONE_CHUNK_BYTES;
    int16_t *smp = (int16_t *)(void *)(s_tone + off);
    for (uint32_t i = 0; i < TONE_CHUNK_FRAMES; i++) {     /* mono: the app pushes stereo, take L */
        int16_t v; int real = 1;
        if (s_ring_wr != s_ring_rd) {                      /* app-rendered PCM first (the chime) */
            if (!s_app_pcm_seen) { s_app_pcm_seen = 1; s_app_pcm_peak = 0; }
            v = s_ring[(s_ring_rd % RING_FRAMES) * 2u]; s_ring_rd++;
            { int16_t a = v < 0 ? (int16_t)-v : v; if (a > s_app_pcm_peak) s_app_pcm_peak = a; }
        } else if (s_tone_hz) {                            /* the built-in test tone */
            v = (int16_t)(k_sine[(s_phase >> 16) & (SINE_STEPS - 1u)] * AUDIO_TONE_GAIN / AUDIO_TONE_DIV);
            s_phase += s_phase_step;
        } else { v = 0; real = 0; }
#if defined(AUDIO_DSP_PER_SOUND)
        if (real) { s_last_audio_ms = timer_ms(); s_real_end_frames = s_frames_total + i + 1u; }
#else
        (void)real;
#endif
        smp[i] = v;
    }
    cache_clean(s_tone + off, TONE_CHUNK_BYTES);
#if defined(AUDIO_DSP_PER_SOUND)
    s_frames_total += TONE_CHUNK_FRAMES;
#endif
    /* 2026-09-18: the boot tone plays, the app's chime and ding do not. Say, once per sound, that
     * app PCM actually reached a DSP buffer and how loud it was; and say when the ring ran dry. */
    if (s_app_pcm_seen == 1) { s_app_pcm_seen = 2; dec("apr: app PCM reached the DSP, queued ", s_ring_wr - s_ring_rd); dec(" frames, peak ", (uint32_t)s_app_pcm_peak); say("\n"); }
    if (s_app_pcm_seen == 2 && s_ring_wr == s_ring_rd) { s_app_pcm_seen = 0; dec("apr: app PCM drained, peak ", (uint32_t)s_app_pcm_peak); say("\n"); }
    /* asm_data_cmd_write_v2 {lsw, msw, handle, size, seq, ts_lsw, ts_msw, flags} */
    hdr3(APR_SVC_ASM, ASM_PORT, ASM_DATA_CMD_WRITE_V2, 52u);
    put32(20u, (uint32_t)(uintptr_t)s_tone + off); put32(24u, 0u); put32(28u, s_mmap_handle); put32(32u, TONE_CHUNK_BYTES);
    put32(36u, ++s_seq); put32(40u, 0u); put32(44u, 0u); put32(48u, ASM_WRITE_NO_TIMESTAMP);
    int rc = smd_send(ch, s_pkt, 52u);
    /* The first writes are logged on the AP console on purpose. The DSP's own F3 trace of a
     * WRITE_V2 arriving is NOT reliable evidence here: the modem diag channel is drained by the
     * same task bgcom.c starves, so gen5-modem-21..24 lose the traces for exactly this window.
     * Only these lines can tell "we never sent it" from "the DSP never answered". */
#if defined(AUDIO_DSP_PER_SOUND)
    if (0)
#else
    if (s_seq <= 12u)
#endif
                      { dec("apr: -> WRITE_V2 seq ", s_seq); dec(" off ", off); dec(" inflight ", s_inflight);
                        dec(" rc ", (uint32_t)(rc < 0 ? -rc : 0)); say(rc < 0 ? " (send FAILED)\n" : "\n"); }
    if (rc < 0) { say("apr: ASM DATA_WRITE_V2 send FAILED\n"); return rc; }
    s_sent_at[s_sent_n++ & 31u] = timer_ms();
    s_wr_buf = (s_wr_buf + 1u) % WRITE_BUFS;
    s_inflight++;
    return rc;
}

/* Top the DSP back up to WRITE_BUFS buffers in flight. */
static int step3_pump(struct smd_chan *ch)
{
    if (s_sleeping) return 0;                                  /* let the in-flight buffers drain, queue none */
#if defined(AUDIO_DSP_PER_SOUND)
    if (s_closing || s_step4 >= 0) return 0;                  /* never feed a session being torn down */
#endif
    while (s_inflight < WRITE_BUFS) {
#if defined(AUDIO_DSP_PER_SOUND)
        /* chime50: gen5-modem-52 -- the DSP returns WRITE_DONE ~8x faster than real time (100
         * buffers = 5 s of audio in 615 ms) and evidently discards what the port cannot play. So
         * WE pace: write a buffer only while less than AUDIO_LEAD_MS of audio is ahead of the
         * clock since SESSION RUN. The app's 500 ms ring absorbs the difference. */
        if (s_frames_total / 48u >= (timer_ms() - s_run_ms) + AUDIO_LEAD_MS) break;
#endif
        if (step3_write(ch) < 0) return -1;
    }
    return 0;
}

static void step3_begin(struct smd_chan *ch)
{
    say("apr: --- step 3: opening the playback stream (ASM -> ADM COPP -> TDM 0x9002) ---\n");
    g_audio_tone_state = 1;
    s_step3 = s_mmap_handle ? 1 : 0;                           /* the 16 KB region stays mapped across sounds */
    if (step3_cmd(ch, s_step3) < 0) { s_step3 = 98; g_audio_tone_state = 3; }
    s_sent_ms = timer_ms();
}
static void step3_fail(const char *why, uint32_t v)
{
    say("apr: step 3 FAILED at command "); con_putdec((uint32_t)s_step3); say(": "); say(why); hex(" ", v); say(" -- stopping\n");
    s_step3 = 98; s_wait_opcode = 0; g_audio_tone_state = 3;
}
static void step3_rsp(struct smd_chan *ch, uint32_t opcode, uint32_t status, uint32_t value)
{
    if (s_step3 < 0 || s_step3 >= STEP3_RUN || opcode != s_wait_opcode) return;
    s_wait_opcode = 0;
    if (status) { step3_fail("DSP status", status); return; }
    if (opcode == ASM_CMD_SHARED_MEM_MAP_REGIONS) s_mmap_handle = value;
    if (opcode == ADM_CMD_DEVICE_OPEN_V5) s_copp_id = value;
    s_step3++;
    s_sent_ms = timer_ms();
    int r = step3_cmd(ch, s_step3);
    if (r < 0) { step3_fail("send", 0u); return; }
    if (r == 0) {                                              /* the stream is up and idle */
        say("apr: step 3 done -- playback stream RUNNING (q6_audio_tone plays through it)\n");
#if defined(AUDIO_PUMP_PRIO)
        /* chime60: the pump lives in mss-boot (prio 1); wlan-net (2) and bg-ka (4) starved it --
         * chime59: 1 s of audio took 3 s to hand over. Above them while a stream runs. */
        if (s_pump_task) { vTaskPrioritySet(s_pump_task, AUDIO_PUMP_PRIO); dec("apr: mss-boot (pump) priority -> ", AUDIO_PUMP_PRIO); say(" while the stream runs\n"); }
#endif
        s_step3 = STEP3_RUN; g_audio_tone_state = 2;
        /* AUDIO_BOOT_TONE used to fire here. That tone finished BEFORE bgcom.c's STOP/OPEN/START
         * restart in every log (gen5-modem-26..30), so it only ever tested the amp started minutes
         * before the TDM clock existed -- audible on some boots, silent on most. It now fires from
         * bgcom_codec_report() once the restarted, clock-synchronised path is up.
         * 2026-09-18, gen5-modem-31: WRONG. The restarted path is the SILENT one: the tone after
         * the restart and the app's PCM (peak 32000, reaching the DSP) were both inaudible, while
         * the same tone through the FIRST START (chime23) was heard. The restart is gone and the
         * tone is back here, through the amp as it was first started. */
#if defined(AUDIO_BOOT_TONE) && !defined(AUDIO_DSP_PER_SOUND)
        q6_audio_tone(1000u, 3000u);                           /* diagnostic: 3 s of 1 kHz at boot (chime23 position) */
#endif
#if defined(AUDIO_DSP_PER_SOUND)
        s_last_audio_ms = timer_ms(); s_closing = 0;
        s_run_ms = timer_ms(); s_frames_total = 0; s_real_end_frames = 0;
#endif
        s_inflight = 0; s_wr_buf = 0; s_write_misses = 0; s_dones = 0; s_last_done_ms = timer_ms(); s_sent_n = 0; s_lat_sum = 0; s_lat_max = 0; s_lat_min = 0xFFFFFFFFu;
        if (step3_pump(ch) < 0) step3_fail("write send", 0u);  /* keep WRITE_BUFS buffers in flight */
    }
}
/* Keep exactly one buffer in flight for as long as the stream is up: silence when no tone is
 * playing, so the port keeps its timing and a ding starts without re-opening anything. */
static void step3_write_done(struct smd_chan *ch, uint32_t status)
{
    if (s_step3 != STEP3_RUN) return;
    if (status) { step3_fail("WRITE_DONE status", status); return; }
    { uint32_t now2 = timer_ms(), gap = s_dones ? now2 - s_gap_prev : 0u; s_gap_prev = now2;
      if (gap > s_gap_max) s_gap_max = gap; if (gap > 100u) s_gap_late++; }
    { uint32_t lat = timer_ms() - s_sent_at[s_dones & 31u];
      s_lat_sum += lat; if (lat > s_lat_max) s_lat_max = lat; if (lat < s_lat_min) s_lat_min = lat; }
    if (!s_dones++) {
        say("apr: first WRITE_DONE -- the DSP is consuming buffers\n");
#if defined(AUDIO_L11_READBACK)
        l11_report("at first WRITE_DONE");
#endif
#if defined(AUDIO_MPSS_WATCH)
        { extern void rpm_master_stats_line(const char *tag); rpm_master_stats_line("first note"); }
#endif
    }
#if !defined(AUDIO_DSP_PER_SOUND)
    if (s_dones <= 12u)
#else
    if (0)
#endif
                        { dec("apr: <- WRITE_DONE ", s_dones); dec(" inflight ", s_inflight); say("\n"); }
#if defined(AUDIO_DSP_PER_SOUND)
    if ((s_dones % 20u) == 0u && s_dones <= 1200u) {         /* real consumption rate: ~1000 ms per 20 at 48 kHz */
        dec("apr: WRITE_DONE ", s_dones); dec(" at +", timer_ms() - s_run_ms); dec(" ms since RUN (real time for them: ", s_dones * 50u);
        dec(" ms); last real sample plays at +", s_real_end_frames / 48u); say(" ms\n");
        dec("apr:   DSP held each buffer avg ", s_lat_sum / 20u); dec(" ms, min ", s_lat_min); dec(" max ", s_lat_max);
        dec(" ms; longest gap between WRITE_DONEs ", s_gap_max); dec(" ms, gaps over 100 ms: ", s_gap_late);
        say(s_gap_late ? " (the pump was late -- underrun)\n" : "\n");
#if defined(AUDIO_L11_READBACK)
        l11_report("during playback");
#endif
#if defined(AUDIO_MPSS_WATCH)
        { extern void rpm_master_stats_line(const char *tag); rpm_master_stats_line("playback"); }   /* is the modem (and the DSP in it) sleeping mid-sound? */
#endif
        s_lat_sum = 0; s_lat_max = 0; s_lat_min = 0xFFFFFFFFu; s_gap_max = 0; s_gap_late = 0;
    }
#endif
    if (s_inflight) s_inflight--;
    s_write_misses = 0;
    s_last_done_ms = timer_ms();
    s_sent_ms = s_last_done_ms;
    if (s_tone_hz) {
        uint32_t ms = TONE_CHUNK_FRAMES / 48u;
        if (s_tone_left_ms <= ms) { s_tone_hz = 0; s_tone_left_ms = 0; say("apr: tone done\n"); }
        else s_tone_left_ms -= ms;
    }
    if (step3_pump(ch) < 0) step3_fail("write send", 0u);
}

/* ---- the app's handle on the speaker (audio_alarm.h, BOARD_HAS_AUDIO_Q6) ----
 * hz 0 stops. The write loop runs in the modem monitor task; this only sets the tone. */
void q6_audio_tone(uint32_t hz, uint32_t ms)
{
#if defined(AUDIO_DSP_PER_SOUND)
    if (!dsp_can_play()) { if (hz) say("q6-audio: tone requested but the DSP is not ready\n"); return; }
    if (hz && ms) { s_want_up = 1; s_closing = 0; }
#else
    if (s_step3 != STEP3_RUN) { if (hz) say("q6-audio: tone requested but the stream is not up\n"); return; }
#endif
    if (!hz || !ms) { s_tone_hz = 0; s_tone_left_ms = 0; return; }
    s_tone_hz = hz; s_tone_left_ms = ms;
    s_phase = 0; s_phase_step = (uint32_t)(((uint64_t)hz * SINE_STEPS << 16) / 48000u);
    dec("q6-audio: tone ", hz); dec(" Hz for ", ms); dec(" ms, peak ", 8000u * AUDIO_TONE_GAIN / AUDIO_TONE_DIV); say(" of 32767\n");
}
#if defined(AUDIO_DSP_PER_SOUND)
static int dsp_can_play(void) { return s_ready == 1 && mss_gate_done(); }
int q6_audio_ready(void) { return dsp_can_play(); }
#else
int q6_audio_ready(void) { return s_step3 == STEP3_RUN; }
#endif

/* chime40 bisect probe: a short tone of a distinct pitch at a boot checkpoint, pumped here so it
 * plays NOW, not whenever the next poller runs. Which pitches are heard says where the sound dies. */
void q6_audio_probe(uint32_t hz, const char *where)
{
#if defined(AUDIO_PROBES)
    say("q6-probe: "); say(where);
    if (s_step3 != STEP3_RUN) { say(" -- stream not up, no probe\n"); return; }
    dec(" -- ", hz); say(" Hz for 800 ms\n");
#if defined(AUDIO_PER_SOUND)
    { extern int bgcom_speaker_on(const char *why); (void)bgcom_speaker_on(where); }
#endif
    q6_audio_tone(hz, 800u);
    uint32_t t0 = timer_ms();
    while (s_tone_hz && timer_ms() - t0 < 2500u) { q6_audio_service(); timer_delay_us(5000u); }
    say(s_tone_hz ? "q6-probe: TIMED OUT (no WRITE_DONE for 2.5 s)\n" : "q6-probe: done\n");
#if defined(AUDIO_PER_SOUND)
    t0 = timer_ms();                                  /* let the buffers in flight play out */
    while (timer_ms() - t0 < 300u) { q6_audio_service(); timer_delay_us(5000u); }
    { extern void bgcom_speaker_off(const char *why); bgcom_speaker_off("probe done"); }
#endif
#else
    (void)hz; (void)where;
#endif
}

/* The app renders its own PCM (audio_alarm.h: the chime melody, the ding) at 48 kHz stereo and
 * pushes it here; the modem monitor task drains it into the DSP. Returns frames accepted. */
uint32_t q6_audio_push(const int16_t *frames, uint32_t n)
{
#if defined(AUDIO_DSP_PER_SOUND)
    if (!dsp_can_play() || !frames) return 0;
    if (n) { s_want_up = 1; s_closing = 0; }
#else
    if (s_step3 != STEP3_RUN || !frames) return 0;
#endif
#if defined(AUDIO_ALARM_AS_TONE)
    return n;                                   /* chime55: swallow the app's PCM */
#endif
    uint32_t space = RING_FRAMES - 1u - (s_ring_wr - s_ring_rd);
    if (n > space) n = space;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = ((s_ring_wr + i) % RING_FRAMES) * 2u;
        s_ring[k] = frames[i * 2u]; s_ring[k + 1u] = frames[i * 2u + 1u];
    }
    s_ring_wr += n;
    return n;
}
#if defined(AUDIO_DSP_PER_SOUND)
uint32_t q6_audio_space(void) { return dsp_can_play() ? RING_FRAMES - 1u - (s_ring_wr - s_ring_rd) : 0u; }
#else
uint32_t q6_audio_space(void) { return s_step3 == STEP3_RUN ? RING_FRAMES - 1u - (s_ring_wr - s_ring_rd) : 0u; }
#endif
uint32_t q6_audio_queued(void) { return s_ring_wr - s_ring_rd; }
void q6_audio_flush(void) { s_ring_rd = s_ring_wr; s_tone_hz = 0; s_tone_left_ms = 0; }
/* Deep sleep (2026-09-18). With the stream RUNNING the DSP answers every 50 ms buffer with a
 * WRITE_DONE, i.e. a modem->apps doorbell, and the monitor task polls every 20 ms: a cluster
 * collapse wakes at once, every time, and the suspend loop spins through thousands of attempts
 * (the tzdbg[...] flood seen on chime27). Sleeping = stop queueing buffers, so the DSP goes
 * quiet within 150 ms and the monitor drops back to its 10 s cadence. The session stays in
 * RUN; waking is just queueing again, done lazily by the first sound the app asks for. */
static int apr_lock(void); static void apr_unlock(void);
void q6_audio_sleep(void) { s_sleeping = 1; s_ring_rd = s_ring_wr; s_tone_hz = 0; s_tone_left_ms = 0; }
void q6_audio_wake(void)  { if (!s_sleeping) return; s_sleeping = 0; if (s_ch && s_step3 == STEP3_RUN && apr_lock()) { if (step3_pump(s_ch) < 0) step3_fail("write send", 0u); apr_unlock(); } }
#if defined(AUDIO_DSP_PER_SOUND)
int q6_audio_playing(void) { return s_tone_hz != 0u || s_ring_wr != s_ring_rd; }
/* chime51: the app marks a sound live (alarm start, ding) and finished (alarm stop, ding end, sleep).
 * While live the DSP side is never torn down, even if the app's loop stalls and the ring runs dry. */
void q6_audio_active(int on)
{
    if (on && !s_app_active) {
        dec("apr: app sound START at +", timer_ms()); say(" ms\n");
        /* log 54: the boot tone is heard, the same running stream is silent 17 s later (after the
         * WiFi bring-up). Re-read what the speaker path depends on, to compare with the boot values:
         * gpio0..3 cfg c8 c8 2c8 2c8, mux 1 / 0x01808000, gpio72 cfg 2c0 out-high. */
        hex("apr: recheck tlmm gpio0..3 cfg ", mmio_read(0x01000000u)); hex(" ", mmio_read(0x01001000u));
        hex(" ", mmio_read(0x01002000u)); hex(" ", mmio_read(0x01003000u)); say("\n");
        hex("apr: recheck pri_pcm_muxsel ", mmio_read(CSR_GP_IO_LPAIF_PRI_PCM_MUXSEL));
        hex(" mux_mic_ctl ", mmio_read(CSR_GP_IO_MUX_MIC_CTL)); say("\n");
        hex("apr: recheck spkr_en gpio72 cfg ", mmio_read(0x01048000u)); hex(" in_out ", mmio_read(0x01048004u)); say("\n");
#if defined(AUDIO_L11_READBACK)
        l11_report("at sound START");
#endif
    }
    if (!on && s_app_active) { dec("apr: app sound END at +", timer_ms()); say(" ms (teardown after the play-out + tail)\n"); }
    s_app_active = on ? 1 : 0;
    if (on && dsp_can_play()) { s_want_up = 1; s_closing = 0; }
#if defined(AUDIO_ALARM_AS_TONE)
    /* chime55: the app's alarm is replaced by the boot tone (its PCM is dropped in q6_audio_push).
     * Audible -> the app's samples are the problem; silent -> the speaker is dead by then. */
    if (on) { say("apr: ALARM_AS_TONE -- playing the boot sine instead of the app PCM
"); q6_audio_tone(1000u, 3000u); }
#endif
}
#else
int q6_audio_playing(void) { return s_step3 == STEP3_RUN && (s_tone_hz != 0u || s_ring_wr != s_ring_rd); }
void q6_audio_active(int on) { (void)on; }
#endif

/* bgcom.c's codec bring-up busy-waits on the BG SPI bus for seconds without yielding, so the
 * modem monitor task -- and with it mss_apr_poll() -- does not run. Its wait loops call this so
 * WRITE_DONEs are still collected and the next buffer is queued: without it the DSP runs dry
 * exactly while the amp is being started (gen5-modem-23, "no WRITE_DONE in 3 s"). */
/* bgcom's bring-up runs in its own task, so this and mss_apr_poll() can land on the APR channel
 * at the same time; one owner at a time. A missed tick is harmless -- the other side is polling. */
static volatile uint32_t s_apr_lock;
static int apr_lock(void)  { return __sync_lock_test_and_set(&s_apr_lock, 1u) == 0u; }
static void apr_unlock(void) { __sync_lock_release(&s_apr_lock); }

#if defined(AUDIO_DSP_PER_SOUND)
/* ---- step 4: tear the DSP side down (stock close order: ASM stream, ADM device, AFE ports,
 * TDM groups, bit clock). Best effort: a failed or silent command is logged and skipped. */
static int step4_cmd(struct smd_chan *ch, int n)
{
    int rc;
    switch (n) {
    case 0: hdr3(APR_SVC_ASM, ASM_PORT, ASM_STREAM_CMD_CLOSE, 20u);
            rc = send3(ch, 20u, ASM_STREAM_CMD_CLOSE, "STREAM_CLOSE (session 1)"); break;
    case 1: hdr3(APR_SVC_ADM, TDM_SPK_PORT, ADM_CMD_DEVICE_CLOSE_V5, 20u);
            ((struct apr_hdr *)s_pkt)->dest_port = (uint16_t)s_copp_id; ((struct apr_hdr *)s_pkt)->token = 0u;
            rc = send3(ch, 20u, ADM_CMD_DEVICE_CLOSE_V5, "DEVICE_CLOSE_V5 (port 0x9002 COPP)"); break;
    case 2: case 3: { uint16_t pr[2] = { n == 2 ? TDM_SPK_PORT : TDM_PRI_TX, 0 };
            afe_hdr(AFE_PORT_CMD_DEVICE_STOP, 24u); memcpy(s_pkt + 20u, pr, 4u);
            rc = afe_send(ch, 24u, n == 2 ? "port 0x9002 DEVICE_STOP" : "port 0x9001 DEVICE_STOP"); break; }
    case 4: case 5: { struct grp_tdm_cfg en; uint16_t e[2] = { n == 4 ? TDM_GRP_PRI_RX : TDM_GRP_PRI_TX, 0u };
            memset(&en, 0, sizeof en); memcpy(&en, e, 4u);
            rc = afe_svc_param(ch, AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_ENABLE, &en, sizeof en,
                               n == 4 ? "group 0x9100 DISABLE" : "group 0x9101 DISABLE"); break; }
#if defined(AUDIO_TDM_MASTER)
    case 6: { uint32_t c[5] = { 1u, Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT, 48000u * 4u * 16u, 1u, 0u };
            rc = afe_svc_param(ch, AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, c, sizeof c, "CLOCK_SET PRI_TDM_IBIT OFF"); break; }
#endif
    default: return 0;
    }
    return rc < 0 ? -1 : 1;
}
static void step4_next(struct smd_chan *ch)
{
    for (;;) {
        s_step4++;
        s_sent_ms = timer_ms();
#if defined(AUDIO_STOCK_ORDER)
        if (s_step4 == 3 || s_step4 == 5) continue;           /* TX port / TX group were never started */
#endif
        int r = step4_cmd(ch, s_step4);
        if (r > 0) return;
        if (r == 0) break;                                    /* all sent */
        dec("apr: teardown command ", (uint32_t)s_step4); say(" send failed -- skipping\n");
    }
    s_wait_opcode = 0; s_step4 = -1; s_closing = 0;
    s_step2 = -1; s_step3 = -1; s_inflight = 0; s_wr_buf = 0;
    dec("apr: DSP side DOWN (port, session, clock) at +", timer_ms()); say(" ms\n");
#if defined(AUDIO_PUMP_PRIO)
    if (s_pump_task) { vTaskPrioritySet(s_pump_task, 1); say("apr: mss-boot (pump) priority -> 1\n"); }
#endif
}
static void step4_rsp(struct smd_chan *ch, uint32_t opcode, uint32_t status)
{
    if (opcode != s_wait_opcode) return;
    s_wait_opcode = 0;
    if (status) { dec("apr: teardown command ", (uint32_t)s_step4); hex(" DSP status ", status); say(" -- continuing\n"); }
    step4_next(ch);
}
/* Bring-up on demand, idle detection and teardown. Runs under the APR lock from the modem task
 * and from the app's loop (q6_audio_service), so neither has to wait for the other's cadence. */
static void dsp_housekeeping(struct smd_chan *ch)
{
    uint32_t now = timer_ms();
    if (s_want_up && s_step4 < 0 && dsp_can_play() &&
        (s_step3 == STEP3_RUN || ((s_step2 < 0 || s_step2 == 98) && (s_step3 < 0 || s_step3 == 98)))) {
        s_want_up = 0; s_closing = 0;
        if (s_step3 != STEP3_RUN) {
            dec("apr: sound requested -- bringing the DSP side UP at +", now); say(" ms\n");
            s_step2 = -1; s_step3 = -1; s_wait_opcode = 0;
            step2_begin(ch);
        }
    }
    if (s_step3 == STEP3_RUN && s_step4 < 0 && !s_want_up) {
        uint32_t played_ms = s_run_ms + s_real_end_frames / 48u;   /* when the last real sample has played */
#if defined(AUDIO_DSP_KEEP_UP)
        /* chime54 (user): after the first bring-up NOTHING is torn down -- codec stays STARTed, the
         * session RUNs on silence, the BG keepalive keeps polling. Tests whether the first teardown
         * is what silences every later sound. */
        if (0 &&
#else
        if (!s_closing && !s_app_active && !q6_audio_playing() && now - s_last_audio_ms >= AUDIO_IDLE_MS &&
#endif
            (int32_t)(now - played_ms) >= (int32_t)AUDIO_IDLE_MS) {
            s_closing = 1; s_closing_ms = now;
            dec("apr: sound played out at +", played_ms - s_run_ms); dec(" ms after RUN, idle ", now - played_ms);
            say(" ms since -- draining, then tearing the DSP side down\n");
        }
        if (s_closing && (s_inflight == 0u || now - s_closing_ms >= 500u)) {
#if defined(AUDIO_CODEC_WITH_DSP)
            { extern void bgcom_speaker_off(const char *why); bgcom_speaker_off("before the DSP teardown"); }
#endif
            say("apr: --- step 4: DSP teardown ---\n");
            s_step4 = -1; step4_next(ch);
        }
    }
    if (s_step4 >= 0 && s_wait_opcode && (int32_t)(timer_ms() - s_sent_ms) > 3000) {   /* not `now`: the codec STOP above ran after it */
        dec("apr: teardown command ", (uint32_t)s_step4); hex(" got no reply (opcode ", s_wait_opcode); say(") -- skipping\n");
        s_wait_opcode = 0; step4_next(ch);
    }
}
static int dsp_in_transition(void)
{
    return s_want_up || s_step4 >= 0 || s_closing || (s_step2 >= 0 && s_step2 < 98 && s_step2 != 99) || (s_step3 >= 0 && s_step3 < STEP3_RUN);
}
#endif

void q6_audio_service(void)
{
    static uint8_t buf[512];
#if defined(AUDIO_DSP_PER_SOUND)
    if (!s_ch || (s_step3 != STEP3_RUN && !dsp_in_transition())) return;
#else
    if (!s_ch || s_step3 != STEP3_RUN) return;
#endif
    if (!apr_lock()) return;
    uint32_t g; unsigned np = 0;
    while (np < 8u && (g = smd_recv(s_ch, buf, sizeof buf, 0u)) != 0u) { np++; rx_packet(buf, g); }
#if defined(AUDIO_DSP_PER_SOUND)
    dsp_housekeeping(s_ch);
    if (s_step3 == STEP3_RUN && s_inflight < WRITE_BUFS && step3_pump(s_ch) < 0) step3_fail("write send", 0u);
#else
    if (s_inflight < WRITE_BUFS && step3_pump(s_ch) < 0) step3_fail("write send", 0u);
#endif
    apr_unlock();
}

/* Called from the modem monitor loop (every 200 ms, then every 10 s) once apr_audio_svc is open. */
void mss_apr_poll(struct smd_chan *ch)
{
    static uint8_t buf[512];
    s_ch = ch;
#if defined(AUDIO_PUMP_PRIO)
    if (!s_pump_task) s_pump_task = xTaskGetCurrentTaskHandle();
#endif
    if (!apr_lock()) return;
    uint32_t g; unsigned np = 0;
    while (np < 8u && (g = smd_recv(ch, buf, sizeof buf, 0u)) != 0u) { np++; rx_packet(buf, g); }

    s_polls++;
#if defined(AUDIO_DSP_PER_SOUND)
    { static int s_boot_tone_done;
      if (!s_boot_tone_done && dsp_can_play()) {
          s_boot_tone_done = 1;
          dec("apr: loading screen done at +", timer_ms()); say(" ms -- DSP side comes up per sound from here\n");
#if defined(AUDIO_BOOT_TONE)
          q6_audio_tone(1000u, 3000u);
#endif
      }
#if defined(AUDIO_SECOND_TONE)
      /* chime53: the same tone again 20 s after the boot tone -- after the first teardown and a
       * second bring-up -- to separate "later bring-ups are silent" from "the app's PCM is". */
      { static uint32_t s_t2_at; static int s_t2_done;
        if (s_boot_tone_done && !s_t2_at) s_t2_at = timer_ms() + 20000u;
        if (s_t2_at && !s_t2_done && (int32_t)(timer_ms() - s_t2_at) >= 0 && s_step4 < 0) {
            s_t2_done = 1;
            say("apr: SECOND TEST TONE (1000 Hz, 3 s) -- after a teardown, same path as the boot tone\n");
            q6_audio_tone(1000u, 3000u);
        } }
#endif
    }
    dsp_housekeeping(ch);
#endif
#if defined(AUDIO_DSP_AFTER_MODEM) && !defined(AUDIO_DSP_PER_SOUND)
    if (s_step2 < 0 && s_step2_armed && mss_gate_done()) {
        s_step2_armed = 0;
        dec("apr: loading screen done at +", timer_ms()); dec(" ms (waited ", timer_ms() - s_step2_armed_ms);
        say(" ms) -- starting the TDM port and session\n");
        step2_begin(ch);
    }
#endif
    /* ask once 1 s after the channel opened, then every 2 s until the DSP says ready (10 tries) */
    if (s_ready != 1 && s_polls >= 5u && ((s_polls - 5u) % 10u) == 0u && s_asks < 10u) {
        if (s_asks == 0u) say("apr: --- step 1: is the audio DSP up? (AVCS_CMD_ADSP_EVENT_GET_STATE) ---\n");
        s_asks++;
        if (core_cmd(ch, AVCS_CMD_ADSP_EVENT_GET_STATE, "ADSP_EVENT_GET_STATE") >= 0 && s_ready < 0) s_ready = 0;
        if (s_asks == 10u) say("apr: no READY after 10 asks -- stopping (check the <- lines above)\n");
    }
    /* step 2/3: no reply within 3 s of a command */
    if (s_step2 >= 0 && s_step2 < 98 && s_wait_opcode && timer_ms() - s_sent_ms > 3000u) {
        dec("apr: step 2 TIMEOUT waiting for the reply to command ", (uint32_t)s_step2); hex(" opcode ", s_wait_opcode); say(" -- stopping\n");
        s_step2 = 98; s_wait_opcode = 0;
    }
    /* A setup command that never answers is fatal. A missing WRITE_DONE is NOT: bgcom.c's codec
     * restart busy-waits on the BG bus (timer_delay_us, no vTaskDelay) for well over 3 s, so this
     * poll is simply not running while the buffer is in flight. gen5-modem-19..21 all "failed" at
     * opcode 0x00010dab for exactly that reason, which then tore the working stream down. Re-arm
     * the write instead, and only give up after the DSP has ignored several in a row. */
    if (s_step3 >= 0 && s_step3 < STEP3_RUN && s_wait_opcode && timer_ms() - s_sent_ms > 3000u) {
        say("apr: step 3 TIMEOUT"); step3_fail("waiting for the reply to opcode", s_wait_opcode);
    }
    if (s_step3 == STEP3_RUN && s_inflight && timer_ms() - s_last_done_ms > 3000u) {
        s_last_done_ms = timer_ms();
        if (++s_write_misses > 10u) { say("apr: no WRITE_DONE after 10 retries"); step3_fail("write stalled, inflight", s_inflight); }
        else { dec("apr: no WRITE_DONE in 3 s -- re-arming the buffers (miss ", s_write_misses); say(")\n");
               s_inflight = 0;
               if (step3_pump(ch) < 0) step3_fail("write send", 0u); }
    }
    if (s_ready == 1 && !s_ver_asked) {
        s_ver_asked = 1;
        (void)core_cmd(ch, AVCS_GET_VERSIONS, "GET_VERSIONS");
    }
    apr_unlock();
}

/* 1 while a step 2/3 command or the tone is in flight: mss_boot.c then polls every 20 ms, not 10 s */
int mss_apr_busy(void)
{
    if (s_sleeping && s_step3 == STEP3_RUN && !s_inflight) return 0;   /* asleep: nothing in flight, 10 s cadence */
#if defined(AUDIO_DSP_AFTER_MODEM)
    if (s_step2_armed) return 1;                                        /* check the loading-screen hand-over every 20 ms */
#endif
#if defined(AUDIO_DSP_PER_SOUND)
    if (dsp_in_transition()) return 1;
    if (s_ready == 1 && !mss_gate_done()) return 1;                     /* catch the hand-over for the boot tone */
#endif
    return (s_step2 >= 0 && s_step2 < 98 && s_step2 != 99) || (s_step3 >= 0 && s_step3 < 98);   /* 6 = stream up: keep servicing writes */
}

#else
void mss_apr_poll(struct smd_chan *ch) { (void)ch; }
int mss_apr_busy(void) { return 0; }
void q6_audio_tone(uint32_t hz, uint32_t ms) { (void)hz; (void)ms; }
int q6_audio_ready(void) { return 0; }
void q6_audio_probe(uint32_t hz, const char *where) { (void)hz; (void)where; }
int q6_audio_playing(void) { return 0; }
uint32_t q6_audio_push(const int16_t *f, uint32_t n) { (void)f; (void)n; return 0; }
uint32_t q6_audio_space(void) { return 0; }
uint32_t q6_audio_queued(void) { return 0; }
void q6_audio_flush(void) {}
void q6_audio_active(int on) { (void)on; }
void q6_audio_sleep(void) {}
void q6_audio_wake(void) {}
void q6_audio_service(void) {}
volatile int g_audio_tone_state;
#endif
