/* ============================================================================
 *  audio_alarm.h — alarm chime through the ES8311 codec + NS4150B speaker amp.
 *
 *  Hardware (per the Waveshare schematic / 08_ES8311 demo):
 *    GPIO41 I2S_SCLK (BCLK)   GPIO45 I2S_LRCK (WS)   GPIO40 I2S_DSDIN (data
 *    to codec)   GPIO16 I2S_MCLK   GPIO46 "Codec_CE" — despite the net name
 *    this only enables the NS4150B AMP (10K pulldown; proof: the ES8311 answers
 *    at 0x18, the CE-pin-LOW address, while GPIO46 is HIGH). The ES8311 itself
 *    sits on the always-on 3.3V rail and is NEVER unpowered — not by GPIO46 and
 *    not by the deep-sleep rail cuts — so it MUST be put back into register-level
 *    power-down after every ring (es8311_power_down), or its analog/DAC blocks
 *    keep drawing a few mA forever, deep sleep included.
 *    The ES8311 sits on the shared touch/PMU I2C bus (Wire, addr 0x18).
 *
 *  No audio file: the melody is synthesized live, chiptune style — sustained
 *  band-limited square waves (Game Boy pulse channel) gated by real MIDI note
 *  durations. The note table is GENERATED from a .mid by tools/midi_to_chime.py
 *  into chime_melody.h. It starts at ~30% volume and ramps to full over 25 s,
 *  so it wakes you gently but won't let you sleep through it.
 *
 *  Threading: audio_alarm_start() does ALL I2C from the caller (the loop task,
 *  where every other Wire user lives), then spawns a small task on core 0 that
 *  only renders samples and blocks on the I2S DMA. audio_alarm_stop() flags it,
 *  waits for it to exit, then cuts Codec_CE and frees the I2S channel — between
 *  rings the whole audio path is unpowered and costs nothing.
 * ========================================================================== */
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
/* AUDIO_PIN_* come from the board header; the BOARD_HAS_AUDIO_* backend flags
 * are defaulted centrally in board.h (at most one =1, enforced there). */

#if !BOARD_HAS_AUDIO_ES8311 && !BOARD_HAS_AUDIO_PWM && !BOARD_HAS_AUDIO_TUYA
/* No codec/speaker on this board — same API, all no-ops (the timer/alarm paths
 * still vibrate via haptics where available). */
static void audio_alarm_init(void) {}
static void audio_alarm_warmup(void) {}
static void audio_alarm_quiesce_codec(void) {}
static void audio_alarm_prepare_sleep(void) {}
static void audio_alarm_start(void) {}
static void audio_alarm_stop(void) {}
static void audio_notify_ding(void) {}
static void audio_alarm_tick(void) {}

#elif BOARD_HAS_AUDIO_PWM
/* ============================================================================
 *  PWM (LEDC square-wave tone) backend — for a board with NO codec/amp, just a
 *  bare speaker (or a small transistor/piezo) on ONE GPIO (AUDIO_PWM_PIN).
 *
 *  PWM can only make a SQUARE WAVE at one frequency at a time (monophonic), so:
 *    - it plays the chime melody (chime_melody.h) at its real pitches/durations,
 *      flattened to the single loudest sounding note at each instant, and
 *    - the notification ding as a short blip.
 *  It can't do timbre/chords/volume-ramp like the ES8311 path — it's a beeper.
 *
 *  A small render task walks the melody timeline and re-tunes the LEDC channel;
 *  the loop's audio_alarm_tick() tears the channel down once a one-shot finishes.
 *  Between sounds the pin is driven LOW (0% duty) so the speaker is silent and
 *  draws no current — including while awake, and latched LOW through deep sleep.
 * ========================================================================== */
#include "driver/gpio.h"            // gpio_hold_* (keep the pin quiet through deep sleep)

/* Duty resolution: LOWER is better for tone accuracy here. The LEDC tone freq is
 * src_clk / (2^res * divider); at high resolution (10-bit) the divider needed for a
 * ~900 Hz note can exceed the timer's range, so the driver falls back to a wrong
 * clock/divider and the pitch comes out garbage. 8-bit leaves plenty of headroom to
 * land low audio frequencies exactly (max tone = src_clk/256, still way above us). */
#define AUDIO_PWM_DUTY_RES  8       // LEDC 8-bit duty (0..255)
#define AUDIO_PWM_STEP_MS   5       // melody scheduler granularity

static volatile TaskHandle_t s_audio_task = nullptr;  // render task (core 0)
static volatile bool s_audio_stop  = false;           // ask it to wind down
static bool          s_audio_live  = false;           // alarm ringing
static bool          s_ding_live   = false;           // ding playing
static bool          s_audio_down_pending = false;    // join timed out; tick() frees later

/* Silence: 0% duty, pin parked LOW. */
static inline void audio_pwm_silence(void) {
  ledcWrite(AUDIO_PWM_PIN, 0);
}

static uint32_t s_audio_cur_hz = 0;   // last frequency set (skip redundant re-tunes)

/* Set the speaker to a tone at `hz` (square wave, ~50% duty). hz<=0 = silence.
 * Only re-tunes when the note actually changes — re-running ledcChangeFrequency
 * every 5 ms re-divides the timer and glitches the output. */
static inline void audio_pwm_tone(float hz) {
  uint32_t f = (hz <= 1.0f) ? 0 : (uint32_t)(hz + 0.5f);
  if (f == s_audio_cur_hz) return;
  s_audio_cur_hz = f;
  if (f == 0) { audio_pwm_silence(); return; }
  uint32_t got = ledcChangeFrequency(AUDIO_PWM_PIN, f, AUDIO_PWM_DUTY_RES);
  // `got` is the frequency the driver ACTUALLY set. If it's 0 or far from `f`, the
  // resolution/clock can't represent this note — the log shows exactly what played.
  if (got == 0 || (got > f ? got - f : f - got) > f / 20)
    Serial.printf("[audio] tone req %u Hz -> got %u Hz\n", f, got);
  ledcWrite(AUDIO_PWM_PIN, 1 << (AUDIO_PWM_DUTY_RES - 1));   // 50% duty = loudest square
}

/* Bring the LEDC tone channel up on the speaker pin (idle/silent). */
static bool audio_pwm_up(const char *who) {
  if (s_audio_task || s_audio_down_pending) {
    Serial.printf("[audio] previous tone still winding down - no %s sound\n", who);
    return false;
  }
  gpio_hold_dis((gpio_num_t)AUDIO_PWM_PIN);
  s_audio_cur_hz = 0;   // fresh channel: force the first note to actually re-tune
  // Attach at a placeholder freq; the render task re-tunes it per note. 1000 Hz is
  // arbitrary — duty stays 0 until a note plays, so nothing is audible yet.
  if (!ledcAttach(AUDIO_PWM_PIN, 1000, AUDIO_PWM_DUTY_RES)) {
    Serial.printf("[audio] ledcAttach failed - no %s sound\n", who);
    return false;
  }
  audio_pwm_silence();
  return true;
}

/* Tear the tone channel down and leave the pin parked LOW (silent, no draw). */
static void audio_pwm_down(void) {
  audio_pwm_silence();
  ledcDetach(AUDIO_PWM_PIN);
  pinMode(AUDIO_PWM_PIN, OUTPUT);
  digitalWrite(AUDIO_PWM_PIN, LOW);
}

/* Wait for the render task to exit, then drop the channel. Mirrors the ES8311
 * path's deferred-teardown safety so we never detach LEDC under a live task. */
static void audio_pwm_join_down(void) {
  s_audio_stop = true;
  for (int i = 0; i < 200 && s_audio_task; i++) delay(5);
  if (s_audio_task) {
    Serial.println("[audio] tone task did not exit in time - deferring teardown");
    audio_pwm_silence();
    s_audio_down_pending = true;
    return;
  }
  audio_pwm_down();
}

#include "chime_melody.h"
#ifndef CHIME_INTRO_MS
#define CHIME_INTRO_MS 0
#endif

/* How long each voice holds the single channel before we switch to the next, when
 * more than one note sounds at once. */
#define AUDIO_PWM_VOICE_SLICE_MS  80
#define AUDIO_PWM_MAX_VOICES      6     // distinct simultaneous notes we'll round-robin

/* Alarm render task: loop the melody, stepping every AUDIO_PWM_STEP_MS. PWM is
 * monophonic but the tune is polyphonic (a main melody plus a faster arpeggio), so
 * at each step we collect ALL distinct notes currently sounding and TIME-MULTIPLEX
 * them — handing the channel to the next voice every AUDIO_PWM_VOICE_SLICE_MS so
 * both lines are heard at once instead of one masking the other. */
static void audio_pwm_alarm_task(void *arg) {
  (void)arg;
  uint32_t pos_ms = 0;
  while (!s_audio_stop) {
    // Collect the distinct frequencies sounding right now (highest first so a single
    // voice still defaults to the lead). Dedup so unisons don't waste a slice.
    float freqs[AUDIO_PWM_MAX_VOICES];
    int   nv = 0;
    for (size_t i = 0; i < sizeof(chime_notes) / sizeof(chime_notes[0]); i++) {
      uint32_t a = chime_notes[i].at_ms;
      uint32_t b = a + chime_notes[i].dur_ms;
      if (pos_ms < a || pos_ms >= b) continue;
      float f = chime_notes[i].freq;
      bool dup = false;
      for (int k = 0; k < nv; k++) if (freqs[k] == f) { dup = true; break; }
      if (dup) continue;
      int p = nv < AUDIO_PWM_MAX_VOICES ? nv : AUDIO_PWM_MAX_VOICES - 1;   // insertion-sort desc
      while (p > 0 && freqs[p - 1] < f) { freqs[p] = freqs[p - 1]; p--; }
      freqs[p] = f;
      if (nv < AUDIO_PWM_MAX_VOICES) nv++;
    }

    if (nv == 0) {
      audio_pwm_tone(0.0f);   // rest
    } else {
      // PWM is monophonic but the tune is polyphonic: time-multiplex the voices,
      // handing the channel to the next one every AUDIO_PWM_VOICE_SLICE_MS so both
      // lines are heard rather than one masking the other.
      int slot = (pos_ms / AUDIO_PWM_VOICE_SLICE_MS) % nv;
      audio_pwm_tone(freqs[slot]);
    }

    delay(AUDIO_PWM_STEP_MS);
    pos_ms += AUDIO_PWM_STEP_MS;
    if (pos_ms >= CHIME_PERIOD_MS) pos_ms = CHIME_INTRO_MS;   // loop (skip the pickup)
  }
  audio_pwm_silence();
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

/* Ding render task: one short E6 blip (~150 ms), then exit. */
static void audio_pwm_ding_task(void *arg) {
  (void)arg;
  audio_pwm_tone(1318.51f);            // E6
  for (int i = 0; i < 30 && !s_audio_stop; i++) delay(5);   // ~150 ms
  audio_pwm_silence();
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

/* ------------------------------- public API ------------------------------- */

static void audio_alarm_init(void) {
  // Park the speaker pin LOW (silent) and release any deep-sleep hold.
  gpio_hold_dis((gpio_num_t)AUDIO_PWM_PIN);
  pinMode(AUDIO_PWM_PIN, OUTPUT);
  digitalWrite(AUDIO_PWM_PIN, LOW);
}

static void audio_alarm_warmup(void) {}          // nothing to pre-register (LEDC is per-ring)

static void audio_alarm_quiesce_codec(void) {}   // no codec on this board

static void audio_alarm_prepare_sleep(void) {
  // Latch the speaker pin LOW through deep sleep so it can't buzz/float.
  audio_pwm_silence();
  digitalWrite(AUDIO_PWM_PIN, LOW);
  gpio_hold_en((gpio_num_t)AUDIO_PWM_PIN);
}

static void audio_alarm_start(void) {
  if (s_audio_live) return;
  if (s_ding_live) { audio_pwm_join_down(); s_ding_live = false; }
  if (!audio_pwm_up("alarm")) return;
  s_audio_stop = false;
  if (xTaskCreatePinnedToCore(audio_pwm_alarm_task, "alarm_pwm", 2560, nullptr,
                              3, (TaskHandle_t *)&s_audio_task, 0) != pdPASS) {
    Serial.println("[audio] tone task spawn failed");
    audio_pwm_down();
    return;
  }
  s_audio_live = true;
}

static void audio_alarm_stop(void) {
  if (!s_audio_live) return;
  audio_pwm_join_down();
  s_audio_live = false;
}

static void audio_notify_ding(void) {
  if (settings_get_mute()) return;
  if (s_audio_live || s_ding_live) return;
  if (!audio_pwm_up("notification")) return;
  s_audio_stop = false;
  if (xTaskCreatePinnedToCore(audio_pwm_ding_task, "notif_pwm", 2048, nullptr,
                              3, (TaskHandle_t *)&s_audio_task, 0) != pdPASS) {
    audio_pwm_down();
    return;
  }
  s_ding_live = true;
}

static void audio_alarm_tick(void) {
  if (s_audio_down_pending && s_audio_task == nullptr) {
    audio_pwm_down();
    s_audio_down_pending = false;
  }
  if (s_ding_live && s_audio_task == nullptr && !s_audio_down_pending) {
    audio_pwm_down();
    s_ding_live = false;
  }
}

#elif BOARD_HAS_AUDIO_ES8311
#include "ESP_I2S.h"
#include "driver/gpio.h"            // gpio_hold_* (keep the amp OFF through deep sleep)

#define AUDIO_SAMPLE_RATE 16000     // MCLK = 256*fs = 4.096 MHz (in the ES8311 coeff table)
#define AUDIO_ES8311_ADDR 0x18      // ES8311_ADDRRES_0 (CE pin strapped low... addr select)

/* ---- speaker loudness profile ----
 * Define AUDIO_SMALL_SPEAKER 1 (in the .ino, before this include) on a watch
 * modified with a tiny/inefficient speaker (in-ear driver): full drive — codec
 * at +12.5 dB digital gain plus full software gain.
 * Default 0 = the ORIGINAL Waveshare speaker, which that drive overwhelms badly
 * (the +12.5 dB boost alone eats the DAC's headroom, so it clips even during
 * the "quiet" ramp-in and would blow the driver at full volume): codec at 0 dB
 * and ~1/4 the software gain, about -24 dB total vs. the small-speaker build. */
#ifndef AUDIO_SMALL_SPEAKER
#define AUDIO_SMALL_SPEAKER 0
#endif
#if AUDIO_SMALL_SPEAKER
#define AUDIO_CODEC_VOL    0xD8     // ES8311 reg32: +12.5 dB (0xBF = 0 dB, 0.5 dB/step)
#define AUDIO_MASTER_GAIN  6500.0f  // ~4 sounding squares before the clamp bites
#else
#define AUDIO_CODEC_VOL    0xBF     // 0 dB — no digital boost, DAC keeps its headroom
#define AUDIO_MASTER_GAIN  1600.0f
#endif

static I2SClass     s_audio_i2s;
static volatile TaskHandle_t s_audio_task = nullptr;  // written by the render task (core 0)
static volatile bool s_audio_stop  = false;   // ask the render task to wind down
static bool          s_audio_live  = false;   // alarm started; stop() has work to do
static bool          s_ding_live   = false;   // notification ding playing; the loop's
                                              // audio_alarm_tick() tears down after it
static bool          s_audio_down_pending = false;    // join timed out with the render task
                                              // still alive: tick() downs the path once it exits

/* ----------------------------- ES8311 over Wire --------------------------- */

static bool es8311_wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AUDIO_ES8311_ADDR);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t es8311_rd(uint8_t reg) {
  Wire.beginTransmission(AUDIO_ES8311_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint8_t)AUDIO_ES8311_ADDR, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

/* Bring the codec from reset to "DAC playing I2S slave, 16-bit @ 16 kHz, MCLK
 * from the MCLK pin". This is the espressif es8311 driver's init path inlined
 * with the coefficients for (MCLK 4.096 MHz, fs 16 kHz): pre_div 1, pre_multi
 * 1x, adc/dac_div 1, single speed, lrck 0x00FF, bclk_div 4, osr 0x10. */
static bool es8311_codec_init(void) {
  if (!es8311_wr(0x00, 0x1F)) return false;       // reset
  es8311_wr(0x00, 0x00);
  es8311_wr(0x00, 0x80);                          // power on, slave mode

  es8311_wr(0x01, 0x3F);                          // all clocks on, MCLK from MCLK pin
  es8311_wr(0x02, es8311_rd(0x02) & 0x07);        // pre_div 1, pre_multi 1x
  es8311_wr(0x03, 0x10);                          // single speed, adc_osr 0x10
  es8311_wr(0x04, 0x10);                          // dac_osr 0x10
  es8311_wr(0x05, 0x00);                          // adc_div 1, dac_div 1
  es8311_wr(0x06, (es8311_rd(0x06) & 0xC0) | 0x03); // sclk not inverted, bclk_div 4
  es8311_wr(0x07, es8311_rd(0x07) & 0xC0);        // lrck_h 0x00
  es8311_wr(0x08, 0xFF);                          // lrck_l 0xFF

  es8311_wr(0x09, 0x0C);                          // SDP-in  16-bit I2S
  es8311_wr(0x0A, 0x0C);                          // SDP-out 16-bit I2S
  es8311_wr(0x0D, 0x01);                          // power up analog circuitry
  es8311_wr(0x0E, 0x02);                          // PGA/ADC modulator on
  es8311_wr(0x12, 0x00);                          // power up DAC
  es8311_wr(0x13, 0x10);                          // enable output to HP drive
  es8311_wr(0x1C, 0x6A);                          // ADC EQ bypass, DC offset cancel
  es8311_wr(0x37, 0x08);                          // bypass DAC EQ

  es8311_wr(0x32, AUDIO_CODEC_VOL);               // DAC volume per the speaker profile above
  es8311_wr(0x31, 0x00);                          // unmute
  return true;
}

/* Put the codec back into its low-power standby. The chip is on the always-on
 * 3.3V rail, so this register script is the ONLY thing standing between "ring
 * once" and "+mA forever". Sequence is esp-adf's es8311_suspend() plus clearing
 * the power-on bit and re-gating the internal clocks. */
static void es8311_power_down(void) {
  es8311_wr(0x31, 0x60);   // mute DAC first (kills the power-down pop)
  es8311_wr(0x32, 0x00);   // DAC volume 0
  es8311_wr(0x0E, 0xFF);   // power down ADC/PGA/modulator
  es8311_wr(0x12, 0x02);   // power down DAC
  es8311_wr(0x14, 0x00);   // PGA off
  es8311_wr(0x0D, 0xFA);   // power down analog circuitry
  es8311_wr(0x15, 0x00);
  es8311_wr(0x37, 0x08);
  es8311_wr(0x45, 0x01);
  es8311_wr(0x00, 0x00);   // clear the power-on bit -> standby
  es8311_wr(0x01, 0x30);   // gate internal clocks (register default)
}

/* ------------------------------- the chime -------------------------------- */
/* Bell voices: fundamental + 2nd/3rd harmonics, each dying exponentially. The
 * harmonics decay faster than the fundamental, which is what reads as "bell"
 * instead of "buzzer". */

#define CHIME_RAMP_S     25.0f      // seconds from gentle (30%) to full volume
#define CHIME_VOICES     10         // organ peaks at ~4 sounding notes; headroom for
                                    // release tails overlapping retriggers

/* The melody (chime_notes[] + CHIME_PERIOD_MS) is generated from a MIDI file by
 * tools/midi_to_chime.py — see the header's comment for the regenerate command. */
#include "chime_melody.h"

#ifndef CHIME_INTRO_MS              // older generated headers: no pickup, loop from 0
#define CHIME_INTRO_MS 0
#endif

/* Chiptune voices: the source tune is an old Game Boy track, so each note is a
 * SUSTAINED band-limited square wave (odd harmonics, like the GB pulse channel)
 * gated by the note's real MIDI duration — not a struck bell. Attack/release
 * are just long enough to kill the on/off clicks. */
#define CHIME_ATTACK_SAMP  (AUDIO_SAMPLE_RATE / 333)  // ~3 ms fade-in
#define CHIME_RELEASE_SAMP (AUDIO_SAMPLE_RATE / 50)   // ~20 ms fade-out

typedef struct {
  float ph, w;                      // phase + per-sample phase increment (rad)
  float amp;
  uint32_t sustain;                 // samples until note-off (from MIDI duration)
  uint16_t attack;                  // samples left of fade-in
  uint16_t release;                 // samples left of fade-out after note-off
  uint8_t  nharm;                   // odd harmonics that stay under the alias guard
  bool live;
} chime_voice_t;

static void audio_render_task(void *arg) {
  (void)arg;
  static int16_t buf[256 * 2];                    // 256 frames stereo = 16 ms
  chime_voice_t v[CHIME_VOICES] = {};
  uint32_t frames_total = 0;                      // ring time, in samples
  uint32_t period_pos   = 0;                      // position inside the melody loop
  size_t   next_note    = 0;

  while (!s_audio_stop) {
    for (int i = 0; i < 256; i++) {
      /* note scheduler: start voices as their time comes up */
      uint32_t pos_ms = period_pos / (AUDIO_SAMPLE_RATE / 1000);
      while (next_note < sizeof(chime_notes) / sizeof(chime_notes[0]) &&
             pos_ms >= chime_notes[next_note].at_ms) {
        float freq = chime_notes[next_note].freq;
        uint8_t nh = 0;                           // odd harmonics under the 7 kHz
        for (int n = 1; n <= 7; n += 2)           // alias guard (Nyquist is 8 kHz)
          if (freq * n < 7000.0f) nh++;
        for (int k = 0; k < CHIME_VOICES; k++) {
          if (v[k].live) continue;
          v[k] = { 0.0f, 2.0f * (float)M_PI * freq / AUDIO_SAMPLE_RATE,
                   chime_notes[next_note].amp,
                   (uint32_t)chime_notes[next_note].dur_ms * (AUDIO_SAMPLE_RATE / 1000),
                   CHIME_ATTACK_SAMP, CHIME_RELEASE_SAMP, nh, true };
          break;
        }
        next_note++;
      }
      if (++period_pos >= (uint32_t)(CHIME_PERIOD_MS * (AUDIO_SAMPLE_RATE / 1000))) {
        /* repeats skip the pickup: restart at the intro mark, with next_note
         * past every note that starts before it */
        period_pos = (uint32_t)(CHIME_INTRO_MS * (AUDIO_SAMPLE_RATE / 1000));
        next_note  = 0;
        while (next_note < sizeof(chime_notes) / sizeof(chime_notes[0]) &&
               chime_notes[next_note].at_ms < CHIME_INTRO_MS) next_note++;
      }

      float s = 0.0f;
      for (int k = 0; k < CHIME_VOICES; k++) {
        if (!v[k].live) continue;
        /* band-limited square: sin(ph) + sin(3ph)/3 + sin(5ph)/5 + sin(7ph)/7 */
        float sq = 0.0f, n = 1.0f;
        for (uint8_t h = 0; h < v[k].nharm; h++, n += 2.0f)
          sq += sinf(n * v[k].ph) / n;
        /* gate: attack -> sustain (MIDI duration) -> release -> free */
        float env;
        if      (v[k].attack)  { env = 1.0f - (float)v[k].attack / CHIME_ATTACK_SAMP; v[k].attack--; }
        else if (v[k].sustain) { env = 1.0f; v[k].sustain--; }
        else if (v[k].release) { env = (float)v[k].release / CHIME_RELEASE_SAMP; v[k].release--; }
        else                   { v[k].live = false; continue; }
        s += v[k].amp * env * sq;
        v[k].ph += v[k].w;
        if (v[k].ph > 2.0f * (float)M_PI) v[k].ph -= 2.0f * (float)M_PI;
      }

      /* gentle-start master ramp: 30% -> 100% over CHIME_RAMP_S */
      float t   = (float)frames_total / AUDIO_SAMPLE_RATE;
      float vol = 0.30f + 0.70f * (t >= CHIME_RAMP_S ? 1.0f : t / CHIME_RAMP_S);
      int32_t q = (int32_t)(s * vol * AUDIO_MASTER_GAIN);
      if (q >  32000) q =  32000;
      if (q < -32000) q = -32000;
      buf[2 * i] = buf[2 * i + 1] = (int16_t)q;
      frames_total++;
    }
    s_audio_i2s.write((uint8_t *)buf, sizeof(buf));       // blocks on DMA = self-pacing
  }

  memset(buf, 0, sizeof(buf));                            // flush silence so the amp
  s_audio_i2s.write((uint8_t *)buf, sizeof(buf));         // isn't cut mid-waveform
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

/* ---------------------------- notification ding --------------------------- */
/* One short E6 square-wave blip (same chiptune voice as the alarm): ~3 ms
 * attack, 90 ms sustain, 60 ms release, a bit softer than the melody. The task
 * only renders; the loop's audio_alarm_tick() does the I2C teardown after it. */

static void audio_ding_task(void *arg) {
  (void)arg;
  static int16_t buf[256 * 2];
  const float w = 2.0f * (float)M_PI * 1318.51f / AUDIO_SAMPLE_RATE;  // E6
  const uint32_t attack  = CHIME_ATTACK_SAMP;                         // 5th harmonic
  const uint32_t sustain = (AUDIO_SAMPLE_RATE * 90) / 1000;           // 6.6 kHz — still
  const uint32_t release = (AUDIO_SAMPLE_RATE * 60) / 1000;           // under the guard
  const uint32_t total   = attack + sustain + release;
  uint32_t n = 0;
  float ph = 0.0f;

  while (n < total && !s_audio_stop) {
    for (int i = 0; i < 256; i++, n++) {
      float env = 0.0f;
      if      (n >= total)           env = 0.0f;
      else if (n < attack)           env = (float)n / attack;
      else if (n < attack + sustain) env = 1.0f;
      else                           env = (float)(total - n) / release;
      float sq = sinf(ph) + sinf(3.0f * ph) / 3.0f + sinf(5.0f * ph) / 5.0f;
      int32_t q = (int32_t)(0.7f * env * sq * AUDIO_MASTER_GAIN);
      if (q >  32000) q =  32000;
      if (q < -32000) q = -32000;
      buf[2 * i] = buf[2 * i + 1] = (int16_t)q;
      ph += w;
      if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
    }
    s_audio_i2s.write((uint8_t *)buf, sizeof(buf));
  }
  memset(buf, 0, sizeof(buf));
  s_audio_i2s.write((uint8_t *)buf, sizeof(buf));
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

/* ------------------------------- public API ------------------------------- */

/* Call once in setup(): park the amp rail OFF (and release any deep-sleep hold). */
static void audio_alarm_init(void) {
  gpio_hold_dis((gpio_num_t)AUDIO_PIN_CE);
  pinMode(AUDIO_PIN_CE, OUTPUT);
  digitalWrite(AUDIO_PIN_CE, LOW);
}

static void audio_alarm_warmup(void) {}          // I2S/codec come up per ring; nothing to pre-init

/* Call once after Wire.begin(): force the codec into standby no matter what
 * state a crash, watchdog reset, or older firmware left it in (it survives
 * everything short of battery removal — reboots don't reset it). */
static void audio_alarm_quiesce_codec(void) {
  es8311_power_down();
}

/* Latch the amp enable LOW through deep sleep (GPIO46 is not an RTC pin; the
 * digital hold rides on gpio_deep_sleep_hold_en(), which haptics already set).
 * Also re-run the codec standby script: it's ~20 I2C writes of insurance against
 * sleeping with the codec's analog blocks still powered. */
static void audio_alarm_prepare_sleep(void) {
  digitalWrite(AUDIO_PIN_CE, LOW);
  gpio_hold_en((gpio_num_t)AUDIO_PIN_CE);
  es8311_power_down();
}

/* Amp on + I2S up + codec to playback. All I2C happens on the CALLER's task (the
 * loop task, where every other Wire user lives) — render tasks never touch Wire. */
static bool audio_path_up(const char *who) {
  // A previous render task that overstayed its join window may still be winding
  // down: starting a new I2S session under it would tear its channel away mid-write.
  // Refuse; audio_alarm_tick() finishes the deferred teardown once it exits.
  if (s_audio_task || s_audio_down_pending) {
    Serial.printf("[audio] previous render task still winding down - no %s sound\n", who);
    return false;
  }
  digitalWrite(AUDIO_PIN_CE, HIGH);               // enable the NS4150B amp
  delay(10);                                      // let it settle before I2C

  s_audio_i2s.setPins(AUDIO_PIN_BCLK, AUDIO_PIN_LRCK, AUDIO_PIN_DOUT, -1, AUDIO_PIN_MCLK);
  if (!s_audio_i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE,
                         I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.printf("[audio] I2S begin failed - no %s sound\n", who);
    digitalWrite(AUDIO_PIN_CE, LOW);
    return false;
  }
  if (!es8311_codec_init()) {
    Serial.printf("[audio] ES8311 not responding - no %s sound\n", who);
    s_audio_i2s.end();
    digitalWrite(AUDIO_PIN_CE, LOW);
    return false;
  }
  return true;
}

static void audio_path_down(void) {
  digitalWrite(AUDIO_PIN_CE, LOW);                // amp off first: kills any teardown pop
  es8311_power_down();                            // codec back to standby — GPIO46 does NOT
                                                  // cut its power, only the amp's
  s_audio_i2s.end();
}

/* Wait for the current render task (alarm or ding) to exit, then drop the path.
 * NEVER end() the I2S driver while the task is still alive: it may be blocked
 * inside s_audio_i2s.write(), and deleting the channel under it makes the driver
 * teardown fail half-way — ESP_I2S then leaks the channel, and after two of those
 * every begin() fails with "i2s_new_channel: no available channel found" until
 * reboot (the no-speaker bug). Normally the task exits within one ~16 ms chunk;
 * the 1 s ceiling covers it being starved by BLE traffic on core 0. If it STILL
 * hasn't exited, defer the teardown to audio_alarm_tick() instead of forcing it. */
static void audio_path_join_down(void) {
  s_audio_stop = true;
  for (int i = 0; i < 200 && s_audio_task; i++) delay(5);
  if (s_audio_task) {
    Serial.println("[audio] render task did not exit in time - deferring I2S teardown");
    digitalWrite(AUDIO_PIN_CE, LOW);   // silence the amp + codec now, free I2S later
    es8311_power_down();
    s_audio_down_pending = true;
    return;
  }
  audio_path_down();
}

static void audio_alarm_start(void) {
  if (s_audio_live) return;
  if (s_ding_live) {                              // a ding is mid-flight: abort it and
    audio_path_join_down();                       // recycle the path for the alarm
    s_ding_live = false;
  }
  if (!audio_path_up("alarm")) return;            // vibration still works without sound

  s_audio_stop = false;
  if (xTaskCreatePinnedToCore(audio_render_task, "alarm_audio", 4096, nullptr,
                              3, (TaskHandle_t *)&s_audio_task, 0) != pdPASS) {
    Serial.println("[audio] render task spawn failed");
    audio_path_down();
    return;
  }
  s_audio_live = true;
}

static void audio_alarm_stop(void) {
  if (!s_audio_live) return;
  audio_path_join_down();
  s_audio_live = false;
}

/* Short notification blip. Call from the LOOP task only (does I2C). No-op while
 * the alarm rings, while a previous ding is still sounding, or when muted. */
static void audio_notify_ding(void) {
  if (settings_get_mute()) return;
  if (s_audio_live || s_ding_live) return;
  if (!audio_path_up("notification")) return;

  s_audio_stop = false;
  if (xTaskCreatePinnedToCore(audio_ding_task, "notif_ding", 3072, nullptr,
                              3, (TaskHandle_t *)&s_audio_task, 0) != pdPASS) {
    audio_path_down();
    return;
  }
  s_ding_live = true;
}

/* Call every loop (like haptics_tick): once the ding task has finished rendering,
 * finish its teardown here on the loop task — codec standby + amp off + I2S free. */
static void audio_alarm_tick(void) {
  if (s_audio_down_pending && s_audio_task == nullptr) {   // overdue join: finish it now
    audio_path_down();
    s_audio_down_pending = false;
  }
  if (s_ding_live && s_audio_task == nullptr && !s_audio_down_pending) {
    audio_path_down();
    s_ding_live = false;
  }
}

#elif BOARD_HAS_AUDIO_TUYA
/* ============================================================================
 *  TuyaOpen (T5-E1) backend — internal codec + NS4150B Class-D amp.
 *
 *  Hardware (per the Waveshare T5-E1 schematic): the T5 chip's OWN codec drives
 *  the analog line-out AUDLP/AUDLN, which feeds the NS4150B mono amp (U6); the
 *  amp's enable (CTRL pin) is GPIO28 (net PA_CTRL, via R36=0R). A 10K pulldown
 *  (R37) holds the amp in SHUTDOWN by default, so IO28 must be driven HIGH to
 *  un-mute it. There is NO ES8311 and NO external I2S — playback goes through the
 *  TuyaOpen low-level audio HAL, NOT Arduino ESP_I2S/Wire.
 *
 *  We use the SAME proven path as the stock Arduino Audio library: register the
 *  codec (tdd_audio_register) with spk_pin=IO28 so the SDK enables the NS4150B in
 *  sync with playback, then tdl_audio_find -> tdl_audio_open, and push PCM with
 *  tdl_audio_play(). (The stock lib omits spk_pin, which is why it wouldn't drive
 *  the amp; we set it.) We also drive IO28 ourselves on teardown/sleep as a hard
 *  off. The whole path is closed (tdl_audio_close) after each ring/ding.
 *
 *  Synthesis is identical to the ES8311 path (chiptune band-limited squares from
 *  chime_melody.h) so the watch sounds the same across boards — only the OUTPUT
 *  stage differs (PCM frames to tdl_audio instead of Arduino I2S).
 *
 *  Threading mirrors the other backends: the public start/stop/ding/init/tick
 *  entry points run on the loop task; a render task on core 0 only synthesizes
 *  samples and blocks in tdl_audio_play() (self-pacing on the SDK buffer).
 *
 *  Requires the SDK media stack (ENABLE_MEDIA=1) — tdd_audio.h is gated on it.
 * ========================================================================== */
extern "C" {
#include "tkl_gpio.h"
#include "tkl_audio.h"
#include "tdd_audio.h"              // tdd_audio_register + TDD_AUDIO_T5AI_T
#include "tdl_audio_manage.h"       // tdl_audio_find/open/play/close (the proven path)
}

#define AUDIO_SAMPLE_RATE 16000     // matches the official Waveshare board (16 kHz mono)
#define AUDIO_CODEC_NAME_STR "owf_audio"  // name we register the codec under

/* ---- speaker loudness profile (honors AUDIO_SMALL_SPEAKER from the .ino) ----
 * Two knobs on the T5 path: AUDIO_T5_VOLUME = the codec/DAC volume (tdl_audio_volume_set,
 * 0-100), and AUDIO_MASTER_GAIN = the software synth gain before the int16 clamp.
 *   AUDIO_SMALL_SPEAKER 1 — watch modified with a tiny/inefficient in-ear driver: full drive.
 *   AUDIO_SMALL_SPEAKER 0 (default) — the ORIGINAL Waveshare speaker, which full drive would
 *     clip/overdrive: lower codec volume + ~1/3 the software gain. */
#ifndef AUDIO_SMALL_SPEAKER
#define AUDIO_SMALL_SPEAKER 0
#endif
#if AUDIO_SMALL_SPEAKER
#define AUDIO_T5_VOLUME   100
#define AUDIO_MASTER_GAIN 6000.0f   // headroom for ~4 simultaneous squares before the clamp
#else
#define AUDIO_T5_VOLUME   70
#define AUDIO_MASTER_GAIN 2200.0f
#endif

static volatile TaskHandle_t s_audio_task = nullptr;  // render task (core 0)
static volatile bool s_audio_stop  = false;           // ask it to wind down
static bool          s_audio_live  = false;           // alarm ringing
static bool          s_ding_live   = false;           // ding playing
static bool          s_audio_down_pending = false;    // join timed out; tick() frees later
static bool          s_audio_registered = false;      // tdd_audio_register done (once)
static TDL_AUDIO_HANDLE_T s_audio_hdl = nullptr;      // tdl handle; non-null == path open

/* ---- amp enable (IO28) ---- */
static inline void audio_amp_set(bool on) {
  tkl_gpio_write((TUYA_GPIO_NUM_E)AUDIO_PIN_CE,
                 on ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW);
}

static void audio_amp_init_pin(void) {
  TUYA_GPIO_BASE_CFG_T cfg;
  cfg.mode   = TUYA_GPIO_PUSH_PULL;
  cfg.direct = TUYA_GPIO_OUTPUT;
  cfg.level  = TUYA_GPIO_LEVEL_LOW;     // default OFF (amp in shutdown)
  tkl_gpio_init((TUYA_GPIO_NUM_E)AUDIO_PIN_CE, &cfg);
}

/* Register the codec driver ONCE with spk_pin=IO28 so the SDK enables the NS4150B
 * amp in sync with playback. (The stock Arduino Audio lib omits spk_pin, which is
 * why it wouldn't drive the amp — we set it here.) */
static bool audio_register_once(void) {
  if (s_audio_registered) return true;
  TDD_AUDIO_T5AI_T cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.aec_enable      = 0;                      // playback only
  cfg.ai_chn          = TKL_AI_0;
  cfg.sample_rate     = TKL_AUDIO_SAMPLE_16K;
  cfg.data_bits       = TKL_AUDIO_DATABITS_16;
  cfg.channel         = TKL_AUDIO_CHANNEL_MONO;
  cfg.spk_sample_rate = TKL_AUDIO_SAMPLE_16K;
  cfg.spk_pin          = AUDIO_PIN_CE;          // NS4150B enable (IO28)
  cfg.spk_pin_polarity = TUYA_GPIO_LEVEL_LOW;   // 0 == HIGH-enable (see board header)
  if (tdd_audio_register((char *)AUDIO_CODEC_NAME_STR, cfg) != OPRT_OK) {
    Serial.println("[audio] tdd_audio_register failed");
    return false;
  }
  s_audio_registered = true;
  return true;
}

/* Bring up the codec + output path via the proven tdl_audio layer (same sequence
 * the stock Arduino Audio library uses: register -> find -> open). Runs on the
 * CALLER (loop) task. We also force IO28 high here as belt-and-suspenders. */
static bool audio_path_up(const char *who) {
  if (s_audio_task || s_audio_down_pending) {
    Serial.print("[audio] previous render task still winding down - no ");
    Serial.print(who); Serial.println(" sound");
    return false;
  }
  if (!audio_register_once()) return false;

  audio_amp_set(true);                 // enable NS4150B before the codec drives the line
  delay(10);                           // let the amp settle (avoids the turn-on pop)

  if (s_audio_hdl == nullptr) {
    if (tdl_audio_find((char *)AUDIO_CODEC_NAME_STR, &s_audio_hdl) != OPRT_OK || s_audio_hdl == nullptr) {
      Serial.print("[audio] tdl_audio_find failed - no "); Serial.print(who); Serial.println(" sound");
      audio_amp_set(false);
      s_audio_hdl = nullptr;
      return false;
    }
    if (tdl_audio_open(s_audio_hdl, nullptr) != OPRT_OK) {
      Serial.print("[audio] tdl_audio_open failed - no "); Serial.print(who); Serial.println(" sound");
      audio_amp_set(false);
      s_audio_hdl = nullptr;
      return false;
    }
    tdl_audio_volume_set(s_audio_hdl, AUDIO_T5_VOLUME);
  }
  return true;
}

/* Push one mono 16-bit PCM chunk to the speaker. Blocks inside the SDK. */
static inline void audio_t5_write(int16_t *buf, uint32_t frames) {
  if (s_audio_hdl == nullptr) return;
  tdl_audio_play(s_audio_hdl, (uint8_t *)buf, frames * sizeof(int16_t));
}

/* Tear the path down: amp OFF first (kills the pop), then close the codec so it
 * isn't left drawing current between sounds. */
static void audio_path_down(void) {
  audio_amp_set(false);
  if (s_audio_hdl != nullptr) {
    tdl_audio_close(s_audio_hdl);
    s_audio_hdl = nullptr;
  }
}

static void audio_path_join_down(void) {
  s_audio_stop = true;
  for (int i = 0; i < 200 && s_audio_task; i++) delay(5);
  if (s_audio_task) {
    Serial.println("[audio] render task did not exit in time - deferring teardown");
    audio_amp_set(false);              // silence now, free the codec later in tick()
    s_audio_down_pending = true;
    return;
  }
  audio_path_down();
}

/* ------------------------------- the chime -------------------------------- */
/* Same chiptune voice model as the ES8311 path: sustained band-limited squares
 * (odd harmonics under the alias guard) gated by each note's MIDI duration. */
#define CHIME_RAMP_S       25.0f
#define CHIME_VOICES       10
#define CHIME_ATTACK_SAMP  (AUDIO_SAMPLE_RATE / 333)   // ~3 ms fade-in
#define CHIME_RELEASE_SAMP (AUDIO_SAMPLE_RATE / 50)    // ~20 ms fade-out

#include "chime_melody.h"
#ifndef CHIME_INTRO_MS
#define CHIME_INTRO_MS 0
#endif

typedef struct {
  float ph, w;
  float amp;
  uint32_t sustain;
  uint16_t attack;
  uint16_t release;
  uint8_t  nharm;
  bool live;
} chime_voice_t;

static void audio_render_task(void *arg) {
  (void)arg;
  static int16_t buf[256];                        // 256 mono frames = 16 ms
  chime_voice_t v[CHIME_VOICES] = {};
  uint32_t frames_total = 0;
  uint32_t period_pos   = 0;
  size_t   next_note    = 0;

  while (!s_audio_stop) {
    for (int i = 0; i < 256; i++) {
      uint32_t pos_ms = period_pos / (AUDIO_SAMPLE_RATE / 1000);
      while (next_note < sizeof(chime_notes) / sizeof(chime_notes[0]) &&
             pos_ms >= chime_notes[next_note].at_ms) {
        float freq = chime_notes[next_note].freq;
        uint8_t nh = 0;
        for (int n = 1; n <= 7; n += 2)
          if (freq * n < 7000.0f) nh++;             // alias guard (Nyquist 8 kHz)
        for (int k = 0; k < CHIME_VOICES; k++) {
          if (v[k].live) continue;
          v[k] = { 0.0f, 2.0f * (float)M_PI * freq / AUDIO_SAMPLE_RATE,
                   chime_notes[next_note].amp,
                   (uint32_t)chime_notes[next_note].dur_ms * (AUDIO_SAMPLE_RATE / 1000),
                   CHIME_ATTACK_SAMP, CHIME_RELEASE_SAMP, nh, true };
          break;
        }
        next_note++;
      }
      if (++period_pos >= (uint32_t)(CHIME_PERIOD_MS * (AUDIO_SAMPLE_RATE / 1000))) {
        period_pos = (uint32_t)(CHIME_INTRO_MS * (AUDIO_SAMPLE_RATE / 1000));
        next_note  = 0;
        while (next_note < sizeof(chime_notes) / sizeof(chime_notes[0]) &&
               chime_notes[next_note].at_ms < CHIME_INTRO_MS) next_note++;
      }

      float s = 0.0f;
      for (int k = 0; k < CHIME_VOICES; k++) {
        if (!v[k].live) continue;
        float sq = 0.0f, n = 1.0f;
        for (uint8_t h = 0; h < v[k].nharm; h++, n += 2.0f)
          sq += sinf(n * v[k].ph) / n;
        float env;
        if      (v[k].attack)  { env = 1.0f - (float)v[k].attack / CHIME_ATTACK_SAMP; v[k].attack--; }
        else if (v[k].sustain) { env = 1.0f; v[k].sustain--; }
        else if (v[k].release) { env = (float)v[k].release / CHIME_RELEASE_SAMP; v[k].release--; }
        else                   { v[k].live = false; continue; }
        s += v[k].amp * env * sq;
        v[k].ph += v[k].w;
        if (v[k].ph > 2.0f * (float)M_PI) v[k].ph -= 2.0f * (float)M_PI;
      }

      float t   = (float)frames_total / AUDIO_SAMPLE_RATE;
      float vol = 0.30f + 0.70f * (t >= CHIME_RAMP_S ? 1.0f : t / CHIME_RAMP_S);
      int32_t q = (int32_t)(s * vol * AUDIO_MASTER_GAIN);
      if (q >  32000) q =  32000;
      if (q < -32000) q = -32000;
      buf[i] = (int16_t)q;
      frames_total++;
    }
    audio_t5_write(buf, 256);                       // blocks on DMA = self-pacing
  }

  memset(buf, 0, sizeof(buf));                      // flush silence before the amp cut
  audio_t5_write(buf, 256);
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

/* Notification ding: one short E6 square blip (~150 ms). */
static void audio_ding_task(void *arg) {
  (void)arg;
  static int16_t buf[256];
  const float w = 2.0f * (float)M_PI * 1318.51f / AUDIO_SAMPLE_RATE;  // E6
  const uint32_t attack  = CHIME_ATTACK_SAMP;
  const uint32_t sustain = (AUDIO_SAMPLE_RATE * 90) / 1000;
  const uint32_t release = (AUDIO_SAMPLE_RATE * 60) / 1000;
  const uint32_t total   = attack + sustain + release;
  uint32_t n = 0;
  float ph = 0.0f;

  while (n < total && !s_audio_stop) {
    for (int i = 0; i < 256; i++, n++) {
      float env;
      if      (n >= total)           env = 0.0f;
      else if (n < attack)           env = (float)n / attack;
      else if (n < attack + sustain) env = 1.0f;
      else                           env = (float)(total - n) / release;
      float sq = sinf(ph) + sinf(3.0f * ph) / 3.0f + sinf(5.0f * ph) / 5.0f;
      int32_t q = (int32_t)(0.7f * env * sq * AUDIO_MASTER_GAIN);
      if (q >  32000) q =  32000;
      if (q < -32000) q = -32000;
      buf[i] = (int16_t)q;
      ph += w;
      if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
    }
    audio_t5_write(buf, 256);
  }
  memset(buf, 0, sizeof(buf));
  audio_t5_write(buf, 256);
  s_audio_task = nullptr;
  vTaskDelete(nullptr);
}

/* ------------------------------- public API ------------------------------- */

static void audio_alarm_init(void) {
  audio_amp_init_pin();                // park IO28 LOW (amp in shutdown)
}

/* Pre-register the codec driver ONCE at boot so the first ring doesn't pay the
 * tdd_audio_register() cost inside alarm_fire() on the loop thread. tdd_audio_register
 * only installs the driver (it does NOT enable the amp or open the path / start the SDK
 * audio task), so it's safe and silent to run early — find/open/close still happen lazily
 * per ring in audio_path_up()/audio_path_down(). Cheap no-op once registered. Without this,
 * a deep-sleep TIMER/alarm wake brings the whole media stack up for the first time from the
 * first loop's alarm_fire(), which is the slow/blocking path that froze the half-drawn ring
 * screen. (The overlay is also flushed before audio starts now — see alarm_fire_ex.) */
static void audio_alarm_warmup(void) {
  audio_register_once();
}

static void audio_alarm_quiesce_codec(void) {}   // codec owned by the SDK; nothing to script

static void audio_alarm_prepare_sleep(void) {
  audio_amp_set(false);                // amp off going into sleep
}

static void audio_alarm_start(void) {
  if (s_audio_live) return;
  if (s_ding_live) { audio_path_join_down(); s_ding_live = false; }
  if (!audio_path_up("alarm")) return;
  s_audio_stop = false;
  if (xTaskCreatePinnedToCore(audio_render_task, "alarm_audio", 4096, nullptr,
                              3, (TaskHandle_t *)&s_audio_task, 0) != pdPASS) {
    Serial.println("[audio] render task spawn failed");
    audio_path_down();
    return;
  }
  s_audio_live = true;
}

static void audio_alarm_stop(void) {
  if (!s_audio_live) return;
  audio_path_join_down();
  s_audio_live = false;
}

static void audio_notify_ding(void) {
  if (settings_get_mute()) return;
  if (s_audio_live || s_ding_live) return;
  if (!audio_path_up("notification")) return;
  s_audio_stop = false;
  if (xTaskCreatePinnedToCore(audio_ding_task, "notif_ding", 3072, nullptr,
                              3, (TaskHandle_t *)&s_audio_task, 0) != pdPASS) {
    audio_path_down();
    return;
  }
  s_ding_live = true;
}

static void audio_alarm_tick(void) {
  if (s_audio_down_pending && s_audio_task == nullptr) {
    audio_path_down();
    s_audio_down_pending = false;
  }
  if (s_ding_live && s_audio_task == nullptr && !s_audio_down_pending) {
    audio_path_down();
    s_ding_live = false;
  }
}

#endif  /* audio backend select: stubs / PWM / ES8311 / TUYA */
