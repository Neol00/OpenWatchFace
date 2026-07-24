/* uart_pl011.c — PL011 for QEMU -M virt (-nographic maps it to stdio).
 * QEMU's PL011 needs no baud setup for the emulated console. */
#include "platform.h"
#if PLAT_UART_TYPE_PL011

#define UARTDR   (PLAT_UART_BASE + 0x00)
#define UARTFR   (PLAT_UART_BASE + 0x18)
#define UARTLCRH (PLAT_UART_BASE + 0x2c)
#define UARTCR   (PLAT_UART_BASE + 0x30)
#define FR_TXFF  (1u << 5)

void uart_init(void)
{
    /* Newer QEMU PL011 models honor the enable bits — program them instead of
     * relying on legacy always-transmit behavior. Baud divisors are ignored
     * by QEMU; real PL011 hardware would need IBRD/FBRD here. */
    mmio_write(UARTLCRH, 0x70);              /* 8n1, FIFOs on */
    mmio_write(UARTCR, (1u << 0) | (1u << 8) | (1u << 9)); /* UARTEN|TXE|RXE */
}

void uart_putc(char c)
{
    while (mmio_read(UARTFR) & FR_TXFF) { }
    mmio_write(UARTDR, (uint32_t)(uint8_t)c);
}

#endif /* PLAT_UART_TYPE_PL011 */
