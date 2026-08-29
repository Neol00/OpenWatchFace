#include "Wire.h"
#include "../platform/platform.h"

/* Default bus for the plain `Wire` object. Defined out-of-line so Wire.h does
 * not need platform.h (which would drag SoC macros into every firmware TU). */
const uintptr_t TwoWire::WIRE_DEFAULT_BASE =
#if defined(PLAT_I2C_DEFAULT_BASE)
    PLAT_I2C_DEFAULT_BASE;
#elif defined(PLAT_I2C_TOUCH_BASE)
    PLAT_I2C_TOUCH_BASE;
#else
    0;   /* begin() will fail cleanly */
#endif

TwoWire Wire;
