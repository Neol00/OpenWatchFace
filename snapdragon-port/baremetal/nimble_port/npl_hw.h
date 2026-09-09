/* npl_hw.h — pulled in by npl_os_freertos.c (NIMBLE_NPL_OS_EXTRA_INCLUDE).
 * Its in_isr() reads the Cortex-M SCB; this is a Cortex-A7 and nothing here
 * calls NimBLE from interrupt context (the HCI transport is a task), so a
 * fake SCB whose ICSR is always 0 answers "not in an ISR". */
#pragma once
struct owf_fake_scb { volatile unsigned int ICSR; };
extern struct owf_fake_scb owf_fake_scb;
#define SCB (&owf_fake_scb)
#define SCB_ICSR_VECTACTIVE_Msk 0x1FFu
