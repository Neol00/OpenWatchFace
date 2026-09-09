/* ============================================================================
 *  hr_sensor.h — board-neutral optical heart-rate (PPG) sensor layer.
 *
 *  The Heart app (app_heart.h) and the beat detector (hr_algo.h) only ever talk
 *  to this API; the chip behind it is selected by ONE board flag:
 *    BOARD_HAS_HR_PAH8011  PixArt PAH8011 over I2C (Fossil Gen 4 / TicWatch C2,
 *                          driver in snapdragon-port/baremetal/platform/hr_pah8011.c)
 *  A board with no flag gets stubs (hr_sensor_available() == false) and the
 *  Heart tile is omitted from the menu (BOARD_HAS_HR in board.h).
 *
 *  Adding a sensor: implement the five functions below for it in a new #elif
 *  branch and add its BOARD_HAS_HR_* flag to BOARD_HAS_HR in board.h.
 *
 *  API:
 *    hr_sensor_begin()      probe + park the chip (boot). true when present.
 *    hr_sensor_available()  probed OK?
 *    hr_sensor_start()      LEDs on, streaming at hr_sensor_rate_hz().
 *    hr_sensor_stop()       LEDs off, chip parked (call when the measurement ends).
 *    hr_sensor_poll(cb)     drain whatever the chip buffered; cb(value, touch)
 *                           is called once per sample in order. Returns samples.
 *    hr_sensor_rate_hz()    sample rate of the stream.
 * ========================================================================== */
#pragma once
#include <stdint.h>

typedef void (*hr_sample_cb)(int32_t ppg, bool touch);

#if BOARD_HAS_HR_PAH8011
extern "C" {
int  pah8011_init(void);
int  pah8011_present(void);
int  pah8011_start(int hz200, int samples_per_burst);
void pah8011_stop(void);
int  pah8011_poll(int32_t *ch1, int32_t *ch2, int max, int *touch);
void pah8011_diag(void);
int  pah8011_set_led_dac(uint8_t v);
uint8_t pah8011_get_led_dac(void);
}
#define hr_sensor_set_gain(v)  pah8011_set_led_dac((uint8_t)(v))
#define hr_sensor_get_gain()   ((int)pah8011_get_led_dac())
#define HR_GAIN_MAX            0x3F
#define hr_sensor_diag() pah8011_diag()
#define HR_PAH_RATE_HZ   200
#define HR_PAH_BURST     20        /* samples per FIFO interrupt = 100 ms at 200 Hz */
static bool s_hr_ok = false, s_hr_running = false;

static inline bool hr_sensor_begin(void) {
  if (s_hr_ok) return true;
  s_hr_ok = pah8011_init() != 0;
  return s_hr_ok;
}
static inline bool hr_sensor_available(void) { return s_hr_ok; }
static inline int  hr_sensor_rate_hz(void)   { return HR_PAH_RATE_HZ; }
static inline bool hr_sensor_start(void) {
  if (!s_hr_ok) return false;
  s_hr_running = pah8011_start(1, HR_PAH_BURST) != 0;
  return s_hr_running;
}
static inline void hr_sensor_stop(void) {
  if (!s_hr_ok || !s_hr_running) return;
  pah8011_stop();
  s_hr_running = false;
}
static inline int hr_sensor_poll(hr_sample_cb cb) {
  static int32_t ch1[HR_PAH_BURST], ch2[HR_PAH_BURST];
  int touch = 0, total = 0;
  if (!s_hr_running) return 0;
  for (int rounds = 0; rounds < 4; rounds++) {            // drain a backlog, bounded
    int n = pah8011_poll(ch1, ch2, HR_PAH_BURST, &touch);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) cb(ch1[i], touch != 0);   // ch1 = green LED channel
    total += n;
  }
  return total;
}

#else  /* no heart-rate sensor on this board */
static inline bool hr_sensor_begin(void)          { return false; }
static inline bool hr_sensor_available(void)      { return false; }
static inline int  hr_sensor_rate_hz(void)        { return 0; }
static inline bool hr_sensor_start(void)          { return false; }
static inline void hr_sensor_stop(void)           {}
static inline int  hr_sensor_poll(hr_sample_cb)   { return 0; }
#define hr_sensor_diag() ((void)0)
#define hr_sensor_set_gain(v) ((void)0)
#define hr_sensor_get_gain()  0
#define HR_GAIN_MAX           0
#endif
