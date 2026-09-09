/* nimble_npl_os_log.h — overrides the FreeRTOS NPL one (nimble_port is earlier
 * on the include path): the upstream version prints every BLE_HS_LOG level,
 * DEBUG included, which floods the console with per-packet dumps. DEBUG is
 * compiled to nothing here; INFO and up go to printf (the console). */
#pragma once
#include <stdarg.h>
#include <stdio.h>
#define _OWF_NPL_LOG_FN(name) \
    static inline void name(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
#define _OWF_NPL_LOG_NOP(name) \
    static inline void name(const char *fmt, ...) { (void)fmt; }
#define _OWF_NPL_IMPL_DEBUG(n)    _OWF_NPL_LOG_NOP(n)
#define _OWF_NPL_IMPL_INFO(n)     _OWF_NPL_LOG_FN(n)
#define _OWF_NPL_IMPL_WARN(n)     _OWF_NPL_LOG_FN(n)
#define _OWF_NPL_IMPL_ERROR(n)    _OWF_NPL_LOG_FN(n)
#define _OWF_NPL_IMPL_CRITICAL(n) _OWF_NPL_LOG_FN(n)
#define BLE_NPL_LOG_IMPL(lvl) \
    _OWF_NPL_IMPL_##lvl(_BLE_NPL_LOG_CAT(BLE_NPL_LOG_MODULE, _BLE_NPL_LOG_CAT(_, lvl)))
