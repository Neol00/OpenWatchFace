/* uart_msm.c — Qualcomm UARTDM v1.4 ("qcom,msm-lsuart-v14"), polled TX.
 *
 * Register map per the firefish kernel's msm_serial_hs_lite.c v1.4 table
 * (SR=0xA4, TF=0x100) + the standard UARTDM v1.4 layout used by LK's aboot
 * uart_dm driver (CR=0xA8, NCF_TX=0x40, single-character transmit protocol).
 *
 * PRECONDITION: aboot must have left the UART clocks + GPIO mux configured
 * (true when the boot console was active). The skeleton does not own the GCC
 * clock controller yet, so if the production aboot ships with the console
 * disabled, TX writes are harmless no-ops into a clock-gated block and the
 * ramlog remains the console. VERIFY on hardware: HARDWARE.md open question #3.
 */
#include "platform.h"
#if PLAT_UART_TYPE_MSM

#define UARTDM_NCF_TX  (PLAT_UART_BASE + 0x040)
#define UARTDM_SR      (PLAT_UART_BASE + 0x0A4)
#define UARTDM_CR      (PLAT_UART_BASE + 0x0A8)
#define UARTDM_TF      (PLAT_UART_BASE + 0x100)

#define SR_TXRDY   (1u << 2)
#define SR_TXEMT   (1u << 3)

#define CR_TX_ENABLE   (1u << 2)

/* Bounded waits: never hang the boot on a dead/gated UART. */
#define UART_SPIN_MAX  100000u

void uart_init(void)
{
    mmio_write(UARTDM_CR, CR_TX_ENABLE);
}

void uart_putc(char c)
{
    uint32_t spin = 0;
    while (!(mmio_read(UARTDM_SR) & SR_TXEMT)) {
        if (++spin > UART_SPIN_MAX) return;     /* clock-gated or absent */
    }
    mmio_write(UARTDM_NCF_TX, 1);               /* 1 char follows */
    mmio_write(UARTDM_TF, (uint32_t)(uint8_t)c);
}

#endif /* PLAT_UART_TYPE_MSM */
