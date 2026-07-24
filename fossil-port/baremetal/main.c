/* main.c — Phase 2 proof: FreeRTOS multitasking on the fossil-port runtime.
 *
 * Two tasks demonstrate the full contract the firmware will rely on:
 *  - heartbeat: vTaskDelay timing (tick IRQ, preemption) checked against the
 *    free-running arch timer, plus an FPU computation (per-task VFP context).
 *  - counter: busy-ish worker at lower priority proving preemptive scheduling.
 * Next step stacks LVGL + the ramfb framebuffer on top of this.
 */
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"

extern char __image_start[], __image_end[], __ramlog_start[];

static volatile uint32_t s_counter;

static void counter_task(void *arg)
{
    (void)arg;
    for (;;) s_counter++;              /* lowest prio: soaks idle time */
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t beat = 0;
    float drift = 0.25f;               /* exercise per-task FPU context */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        drift *= 1.5f;
        con_puts("beat "); con_putdec(++beat);
        con_puts("  rtos-ticks="); con_putdec((uint32_t)xTaskGetTickCount());
        con_puts("  wall-ms="); con_putdec(timer_ms());
        con_puts("  counter="); con_putdec(s_counter);
        con_puts("  fpu="); con_putdec((uint32_t)drift);
        con_puts("\n");
    }
}

void main(void)
{
    mmu_enable_flat();
    uart_init();
    ramlog_init();
    gic_init();

    con_puts("\nOpenWatchFace bare-metal runtime [" PLAT_NAME "] + FreeRTOS " tskKERNEL_VERSION_NUMBER "\n");
    con_puts("image:  ");  con_puthex((uint32_t)(uintptr_t)__image_start);
    con_puts("..");        con_puthex((uint32_t)(uintptr_t)__image_end);
    con_puts("\nramlog: "); con_puthex((uint32_t)(uintptr_t)__ramlog_start);
    con_puts(ramlog_had_previous() ? " (previous boot log preserved)\n" : " (fresh)\n");
    con_puts("timer:  ");  con_putdec(timer_freq_hz()); con_puts(" Hz\n");

    extern void ui_task(void *);
    xTaskCreate(ui_task,        "ui",        8192, NULL, 3, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 1024, NULL, 2, NULL);
    xTaskCreate(counter_task,   "counter",   256,  NULL, 1, NULL);

    con_puts("starting scheduler\n");
    vTaskStartScheduler();

    con_puts("!! scheduler returned\n");
    for (;;) { }
}
