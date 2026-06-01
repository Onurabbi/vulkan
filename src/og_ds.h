#ifndef OG_DS_H
#define OG_DS_H

#include "common.h"
#include "limits.h"

#include <stdlib.h>
#include <string.h>

/****************************  Array ******************************************/
/**************************************************************************** */

typedef struct 
{
    u32 count;
    u32 capacity;
    memory_arena_t *arena;
}array_header_t;

#define GROW_CAPACITY(oldCapacity) ((oldCapacity) < 8 ? 8 : (oldCapacity) * 2)

#define ArrayHeader(arr)\
    ((array_header_t*)((char*)(arr) - sizeof(array_header_t)))

#define ArrayCount(arr) \
    ((arr) ? ArrayHeader((arr))->count : 0)

#define ArrayCapacity(arr) \
    ((arr) ? ArrayHeader((arr))->capacity : 0)

#define ArrayFull(arr) \
    ArrayCount((arr)) == ArrayCapacity((arr))

#define ArrayArena(arr) \
    ((arr) ? ArrayHeader((arr))->arena : NULL)

#define ArrayPush(arr,val) \
do { \
    if (ArrayFull((arr))) { \
        if (ArrayArena((arr))) { \
            /*fail, we don't support growing arrays that use memory arenas since they can't be reallocated*/ \
            LV_ASSERT(false && "Array is full and uses a memory arena, we don't support growing arrays"); \
        } else { \
            (arr) = ArrayGrow((arr), sizeof(*(arr)), ArrayCount((arr)) + 1); \
        } \
    } \
    (arr)[ArrayHeader((arr))->count++] = val; \
} while(0)

#define ArrayResize(arr, newCount) \
do { \
    if ((newCount) > ArrayCapacity((arr))) { \
        if (ArrayArena((arr))) { \
            /*fail, we don't support growing arrays that use memory arenas since they can't be reallocated*/ \
            LV_ASSERT(false && "Array is full and uses a memory arena, we don't support growing arrays"); \
        } else { \
            (arr) = ArrayGrow((arr), sizeof(*(arr)), newCount); \
        } \
    } \
    ArrayHeader((arr))->count = newCount; \
} while(0)

#define ArrayDelete(arr, i) \
do { \
    if ((i) < ArrayCount((arr))) { \
        (arr)[(i)] = (arr)[ArrayCount((arr)) - 1]; \
        ArrayHeader((arr))->count--; \
    } \
} while (0);

#define ArrayDeleteNonSwap(arr, i) \
do { \
    if ((i) < ArrayCount((arr))) { \
        for (u32 idx = i; idx < ArrayCount((arr)) - 1; idx++) { \
            (arr)[idx] = (arr)[idx + 1]; \
        } \
        ArrayHeader((arr))->count--; \
    } \
}while(0)

#define ArrayPushArray(arr, src, count) \
do { \
    u32 oldCount = ArrayCount((arr)); \
    ArrayResize((arr), ArrayCount((arr)) + (count)); \
    memmove((&(arr)[oldCount]), (src), (count) * sizeof(*(arr))); \
}while(0);

#define ArrayClear(arr) ((arr) ? ArrayHeader((arr))->count = 0 : (void)0)
#define ArrayBack(arr) ((arr) ? &(arr)[ArrayCount((arr)) - 1] : NULL)

#define ArrayInitWithArena(arr, a, cap) \
do { \
    array_header_t *header = ArenaPushSize((a), sizeof(array_header_t) + (cap) * sizeof(*(arr))); \
    header->count = 0; \
    header->capacity = cap; \
    header->arena = a; \
    (arr) = (void*)&header[1]; \
} while(0);

void ArrayFree(void *array);
void *ArrayGrow(void *arr, size_t elemSize, u32 minCapacity);
void *reallocate(void *ptr, size_t oldSize, size_t newSize);

/**************************************************************************** */

/************************************* hash map ********************************/
/*******************************************************************************/

typedef struct {
    u64 key;
    u64 value;
} hash_entry_t;

typedef struct {
    hash_entry_t *entries;
    u32 capacity;
    u32 count;
    memory_arena_t *arena;
}hash_map_t;

#ifdef __cplusplus
extern "C" {
#endif

b8 HashMapLookup(const hash_map_t *map, const void *key, u32 keyLength, u64 *outValue);
b8 HashMapLookupString(const hash_map_t *map, u64 stringId, const char **outValue);
b8 HashMapSet(hash_map_t *map, const void *key, u32 keyLength, u64 value);
b8 HashMapSetString(hash_map_t *map, u64 stringId, const char *value);
b8 HashMapDelete(hash_map_t *map, const void *key, u32 keyLength);
void HashMapInit(hash_map_t *map);
void HashMapInitWithArena(hash_map_t *map, memory_arena_t *arena, u32 capacity);
void HashMapFree(hash_map_t *map);
u64 hash(const u8* key, u32 length);

#ifdef __cplusplus
}
#endif
/**************************************************************************** */

#define NIL_KEY (0ULL)
#define NIL_VALUE (0ULL)
#define MAP_MAX_LOAD 0.5

#endif //OG_DS_H
