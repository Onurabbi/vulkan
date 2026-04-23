
#ifndef ASSERT_H
#define ASSERT_H

#include "common.h"

#if !defined(NDEBUG)
#define LV_ASSERTIONS_ENABLED
#endif

#if defined(_MSC_VER)
extern void __cdecl __debugbreak(void);
#define lv_debug_break() __debugbreak()
#elif ((!defined(__NACL__)) && ((defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))))
#define lv_debug_break() __asm__ __volatile__ ( "int $3\n\t")
#elif defined(__386__) && defined(__WATCOMC__)
#define lv_debug_break() { _asm { int 0x03 } }
#elif defined (HAVE_SIGNAL_H) && !defined(__WATCOMC__)
#include <signal.h>
#define lv_debug_break() raise(SIGTRAP)
#else
#define lv_debug_break() ((void*)0=0)
#endif

#ifdef LV_ASSERTIONS_ENABLED

LV_EXPORT void ReportAssertionFailure(const char *expr);

#define LV_ASSERT(expr)                       \
do                                            \
{                                             \
if (!(expr)) {                                \
ReportAssertionFailure(#expr);              \
lv_debug_break();                             \
}                                             \
} while(0)

#else
#define LV_ASSERT(expr)
#endif

#endif //ASSERT_H
