/* bringup_stubs.c — the two symbols a NO-APP bring-up image is missing.
 *
 * The sign-of-life build (build-bringup-gen5.sh) links the platform drivers
 * WITHOUT the C++ compat layer and without the storage/blackbox stack, because
 * the whole point of that image is to answer "does our code run on this watch"
 * with the fewest moving parts. Two symbols are defined outside platform/ and
 * are referenced from inside it, so they have to come from somewhere:
 *
 *   g_pwr_sleep_ms  — normally compat/arduino_glue.cpp's accounting of time
 *                     spent in delay(). gen4_stubs.c reads it to report an idle
 *                     duty cycle. With no app there is no delay() accounting,
 *                     so it stays 0 and the duty report reads as "never slept",
 *                     which is exactly true for this build.
 *   blackbox_sync   — normally storage_gen6.c, flushing the crash blackbox to
 *                     eMMC. This image has no filesystem mounted, and a
 *                     bring-up image that formatted storage on its first boot
 *                     would be a terrible surprise, so it is a no-op here.
 *                     The ramlog still holds everything con_puts() emitted.
 *
 * Both are DELIBERATELY inert rather than clever: this file exists to make the
 * link succeed without dragging in stacks whose failure would be indis-
 * tinguishable from the failure we are actually testing for. It must never be
 * linked into a real firmware image — the real definitions live in the files
 * named above, and the linker would reject the duplicate.
 */
#include <stdint.h>

uint32_t g_pwr_sleep_ms = 0;

void blackbox_sync(void);
void blackbox_sync(void) { /* no storage in the bring-up image */ }
