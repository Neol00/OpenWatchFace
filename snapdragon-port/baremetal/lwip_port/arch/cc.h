/* arch/cc.h — lwIP compiler/arch glue for arm-none-eabi-gcc */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define BYTE_ORDER              LITTLE_ENDIAN
#define LWIP_NO_STDINT_H        0
#define LWIP_NO_INTTYPES_H      1
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "lu"
#define S32_F "ld"
#define X32_F "lx"
#define SZT_F "u"
#define PACK_STRUCT_FIELD(x)    x
#define PACK_STRUCT_STRUCT      __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { } while (0)
#define LWIP_TIMEVAL_PRIVATE    0
#include <sys/time.h>
