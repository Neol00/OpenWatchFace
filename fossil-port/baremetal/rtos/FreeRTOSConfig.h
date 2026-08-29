/* FreeRTOSConfig.h — fossil-port bare-metal (FreeRTOS V11.2, GCC/ARM_CA9 port,
 * which is the ARMv7-A + GICv2 port and exactly matches the Cortex-A7). */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* GIC location differs per board; keep this header self-contained (it is also
 * included from assembly) — mirror the boards/*.h values via the same defines. */
#if defined(PLAT_BOARD_QEMU_VIRT)
  #define configINTERRUPT_CONTROLLER_BASE_ADDRESS          0x08000000UL
  #define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET  0x00010000UL
  /* QEMU's GICv2 implements all 8 priority bits -> 256 levels (the port
   * verifies this against the hardware at boot). */
  #define configUNIQUE_INTERRUPT_PRIORITIES        256
  #define configMAX_API_CALL_INTERRUPT_PRIORITY    144
#elif defined(PLAT_BOARD_FOSSIL_GEN4) || defined(PLAT_BOARD_FOSSIL_GEN6) || \
      defined(PLAT_BOARD_TICWATCH_C2)
  /* Every MSM watch here uses qcom,msm-qgic2 at the SAME addresses — the Gen 6
   * DTB's interrupt-controller@b000000 reg is byte-identical to the Gen 4's,
   * and the TicWatch C2 is the Gen 4's SoC outright. This block cannot use
   * PLAT_SOC_MSM: the file is included from assembly and must stay
   * self-contained, so it never sees platform.h's derived tiers. */
  #define configINTERRUPT_CONTROLLER_BASE_ADDRESS          0x0B000000UL
  #define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET  0x00002000UL
  /* GIC priority bits: MEASURED ON HARDWARE 2026-08-03, twice, both ways.
   * The line-number-encoding vAssertCalled read "8 s" = port.c:365 = the
   * read-back assert with 32 configured -> the register reads back 0xF0 ->
   * 15 -> NON-SECURE 4-bit view, 16 levels. (An earlier "3 s" reading was
   * misattributed to this assert firing with 16 configured; it was actually
   * a post-scheduler-start event — with 16 the GIC checks all PASS.)
   * 16 is the measured truth; do not "correct" it back to 32.
   *
   * The C2 inherits this MEASURED-on-Fossil value untested. Same GIC-400 in
   * the same non-secure view, so 16 is very likely right — but if the C2
   * asserts early in port.c, this is the first line to suspect. */
  #define configUNIQUE_INTERRUPT_PRIORITIES        16
  #define configMAX_API_CALL_INTERRUPT_PRIORITY    12
#else
  #error "FreeRTOSConfig.h: unknown PLAT_BOARD_*"
#endif

#define configSETUP_TICK_INTERRUPT()  vConfigureTickInterrupt()
#define configCLEAR_TICK_INTERRUPT()  vClearTickInterrupt()
#ifndef __ASSEMBLER__
void vConfigureTickInterrupt( void );
void vClearTickInterrupt( void );
void vAssertCalled( const char * pcFile, unsigned long ulLine );
#endif

#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       ( 800000000UL ) /* informational */
#define configTICK_RATE_HZ                       ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES                     ( 8 )
#define configMINIMAL_STACK_SIZE                 ( ( unsigned short ) 256 )
#define configTOTAL_HEAP_SIZE                    ( ( size_t ) ( 16 * 1024 * 1024 ) )
#define configMAX_TASK_NAME_LEN                  ( 16 )
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_TASK_NOTIFICATIONS             1
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_QUEUE_SETS                     0
#define configUSE_TIME_SLICING                   1
#define configUSE_NEWLIB_REENTRANT               0
#define configENABLE_BACKWARD_COMPATIBILITY      0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  2

/* Cortex-A FPU: save/restore VFP-NEON context for every task. */
#define configUSE_TASK_FPU_SUPPORT               2

#define configSUPPORT_STATIC_ALLOCATION          0
#define configSUPPORT_DYNAMIC_ALLOCATION         1

#define configUSE_IDLE_HOOK                      1  /* WFI idle — see irq.c (the 100%-duty bug) */
#define configUSE_TICK_HOOK                      0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_DAEMON_TASK_STARTUP_HOOK       0

#define configGENERATE_RUN_TIME_STATS            0
#define configUSE_TRACE_FACILITY                 0
#define configUSE_STATS_FORMATTING_FUNCTIONS     0

#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             ( 1024 )

#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_xTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetIdleTaskHandle           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTimerPendFunctionCall           1
#define INCLUDE_xTaskGetSchedulerState           1

#define configASSERT( x ) \
    if( ( x ) == 0 ) vAssertCalled( __FILE__, __LINE__ )

#endif /* FREERTOS_CONFIG_H */
