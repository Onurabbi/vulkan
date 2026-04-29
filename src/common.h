#ifndef OG_COMMON_H
#define OG_COMMON_H

#define ARRAY_SIZE(arr) (uint32_t)(sizeof(arr)/(sizeof((arr)[0])))

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef signed long long   i64;

typedef float  f32;
typedef double f64;

typedef int   b32;
typedef _Bool b8;

#define true  1
#define false 0

#if defined(UINT32_MAX)
#undef UINT32_MAX
#define UINT32_MAX ((u32)-1)
#endif

#if defined(UINT64_MAX)
#undef UINT64_MAX
#define UINT64_MAX ((u64)-1)
#endif

#define NULL ((void *)0)
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#define MIN(a,b) (((a) < (b)) ? (a) : (b))

#define KILOBYTES(a) (1024UL * (a))
#define MEGABYTES(a) (1024UL * KILOBYTES((a)))
#define GIGABYTES(a) (1024UL * MEGABYTES((a)))

#ifdef _MSC_VER
#define LV_EXPORT __dsclspec(dllexport)
#else
#define LV_EXPORT __attribute__((visibility ("default")))
#endif

#define SIZE_ALIGN(n,align) (u64)((~((align)-1)) & ((n) + ((align)-1)))

#define MAX_PATH 511
#define MAX_NAME 255

#define LEVO_PI 3.14159265358979323846

#define DEG_TO_RAD(x) (0.0174532925 * (x))

#define RETURN_FALSE_IF_FALSE(x)          \
do {                                      \
   if (!(x)) {                            \
      LOGI("Condition: " #x " is false"); \
      return false;                       \
   }                                      \
} while(0)

#if defined(__clang__) || defined(__GNUC__)
#define STATIC_ASSERT _Static_assert
#else
#define STATIC_ASSERT static_assert
#endif

STATIC_ASSERT(sizeof(u8) == 1, "Expected u8 to be 1 byte.");
STATIC_ASSERT(sizeof(u16) == 2, "Expected u16 to be 2 bytes.");
STATIC_ASSERT(sizeof(u32) == 4, "Expected u32 to be 4 bytes.");
STATIC_ASSERT(sizeof(u64) == 8, "Expected u64 to be 8 bytes.");
STATIC_ASSERT(sizeof(i8) == 1, "Expected i8 to be 1 byte.");
STATIC_ASSERT(sizeof(i16) == 2, "Expected i16 to be 2 bytes.");
STATIC_ASSERT(sizeof(i32) == 4, "Expected i32 to be 4 bytes.");
STATIC_ASSERT(sizeof(i64) == 8, "Expected i64 to be 8 bytes.");
STATIC_ASSERT(sizeof(f32) == 4, "Expected f32 to be 4 bytes.");
STATIC_ASSERT(sizeof(f64) == 8, "Expected f64 to be 8 bytes.");

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
#define LV_PLATFORM_WINDOWS 1
#elif defined(__linux__) || defined(__gnu_linux__)
#define LV_PLATFORM_LINUX 1
#endif

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

#define BIT_SET(num, bit) ((num) |= (1 << (bit)))
#define BIT_CLEAR(num, bit) ((num) &= ~(1 << (bit)))
#define BIT_CHECK(num, bit) ((num) & (1 << (bit)))
#define BIT_FLIP(num, bit) ((num) ^= (1 << (bit)))

#define VK_CHECK(expr) \
do { \
    if (expr != VK_SUCCESS) { \
        LV_ASSERT(false && #expr" returned a result other than VK_SUCCESS"); \
    } \
} while(0)

//Limits
#define SCANCODE_COUNT 512
#define MAX_THREADS 32
#define MAX_JOBS 256
#define STRING_ARENA_CAPACITY MEGABYTES(1)
#define MAX_STRING_COUNT 32768U // we can intern up to 32768 unique strings
#define SCRATCH_ARENA_CAPACITY MEGABYTES(128)
#define PERMANENT_ARENA_CAPACITY MEGABYTES(128)
#define MAX_VERTICES (1U * 1024U * 1024U)
#define MAX_INDICES (1U * 1024U * 1024U)
#define MAX_MESHES (1U * 1024U) 

typedef void *vulkan_instance_t;
typedef void *vulkan_physical_device_t;

typedef struct {
   b8 (*VulkanGetPresentationSupport)(vulkan_instance_t instance, vulkan_physical_device_t physical_device, u32 queue);
   b8 (*CreateWindow)(void* window, void *arg, const char *title, i32 w, i32 h, u64 flags);
}platform_api_t;

typedef struct {
   void *memoryBase;
   u64   memorySize;
   u32   threadCount;
   char const* const* vulkanInstanceExtensions;
   u32 vulkanInstanceExtensionCount;
   platform_api_t api;
} game_memory_t;

typedef struct {
   b32 event;
   b32 down;
   b32 repeat;
}key_event_t;

typedef struct {
   const b8* keyboardState;
   key_event_t keyEvents[SCANCODE_COUNT];
   f32 mouseX, mouseY;
   f32 mouseXRel, mouseYRel;
   b8 quit;
   b8 windowResized;
} game_input_t;

#endif // OG_COMMON_H
