/* sys_arch.c — NO_SYS lwIP needs only a millisecond clock. */
#include "lwip/sys.h"
uint32_t timer_ms(void);
u32_t sys_now(void) { return timer_ms(); }
