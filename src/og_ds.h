#ifndef OG_DS_H
#define OG_DS_H

#include "common.h"
#include "memory.h"

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

#if defined (OG_DS_IMPLEMENTATION)

void *reallocate(void *ptr, size_t oldSize, size_t newSize)
{
    (void)oldSize;
    if (newSize == 0) {
        free(ptr);
        return NULL;
    }
    void *result = realloc(ptr, newSize);
    //TODO: Remove ptr from a list of tracked allocations and add result to it, so we can detect memory leaks and double frees
    LV_ASSERT(result && "Unable to allocate memory!!");
    return result;
}

void *ArrayGrow(void *arr, size_t elemSize, u32 minCapacity)
{
    u32 oldCapacity = ArrayCapacity(arr);
    u32 newCapacity = GROW_CAPACITY(oldCapacity);
    
    if (newCapacity < minCapacity) {
        newCapacity = minCapacity;
    }

    array_header_t *header = NULL;
    if (oldCapacity == 0) {
        header = reallocate(NULL, 0, newCapacity * elemSize + sizeof(array_header_t));
        header->capacity = newCapacity;
        header->count = 0;
        header->arena = NULL;
    } else {
        array_header_t *oldHeader = ArrayHeader((arr));
        u32 new_count = oldHeader->count;
        if (newCapacity == minCapacity) {
            // the array was resized, so we double up the max capacity to preserve the amortized O(1) time complexity of push
            newCapacity *= 2;
            // Since the array was resized, we need to make sure the new size is correct
            new_count = minCapacity;
        }

        header = reallocate(oldHeader, oldCapacity * elemSize + sizeof(array_header_t), 
                                                 newCapacity * elemSize + sizeof(array_header_t));
        header->capacity = newCapacity;
        header->count = new_count;
        header->arena = NULL;
    }
    return &header[1];
}

void ArrayFree(void *array) 
{
    if (!array) return;

    if (ArrayArena(array)) {
        LV_ASSERT(false && "We don't support freeing arrays that use memory arenas since they can't be reallocated"); 
        return;
    }
    (void)reallocate(ArrayHeader(array), 0, 0);
}

#endif //OG_DS_IMPLEMENTATION

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

bool HashMapLookup(const hash_map_t *map, const void *key, u32 keyLength, u64 *outValue);
bool HashMapLookupString(const hash_map_t *map, u64 stringId, const char **outValue);
bool HashMapSet(hash_map_t *map, const void *key, u32 keyLength, u64 value);
bool HashMapSetString(hash_map_t *map, u64 stringId, const char *value);
bool HashMapDelete(hash_map_t *map, const void *key, u32 keyLength);
void HashMapInit(hash_map_t *map);
void HashMapInitWithArena(hash_map_t *map, memory_arena_t *arena, u32 capacity);
void HashMapFree(hash_map_t *map);
u64 hash(const u8* key, u32 length);

/**************************************************************************** */

/******************************* string interning ******************************/
/*******************************************************************************/

const char *StringIntern(const char *str);
void StringInterningInit(u32 capacity, u32 maxStringCount);
void StringInterningDeinit(void);

#if defined (OG_DS_IMPLEMENTATION)

static hash_map_t stringMap;
static memory_arena_t stringArena;
static memory_arena_t stringMapArena;

static const char *PushString(const char *str)
{
    size_t strSize = strlen(str) + 1;
    char *result = PushArray(&stringArena, strSize, char);
    strncpy(result, str, strSize);
    return result;
}

const char *StringIntern(const char *str)
{
    u64 id = hash((const u8*)str, strlen(str));
    //lookup the string in the map, if it exists return the id, otherwise insert it and return the new id
    const char *result = NULL;
    if (!HashMapLookupString(&stringMap, id, &result)) {
        result = PushString(str);
        HashMapSetString(&stringMap, id, result);
    }
    return result;
}

void StringInterningInit(u32 capacity, u32 maxStringCount)
{
    maxStringCount *= 2; // max load factor of 0.5 for the hash map
    ArenaInit(&stringMapArena, maxStringCount*sizeof(hash_entry_t));
    HashMapInitWithArena(&stringMap, &stringMapArena, maxStringCount);

    ArenaInit(&stringArena, capacity);
    
    char *dst = PushStruct(&stringArena, char);
    *dst = '\0'; // we want the first byte of the arena to be a null terminator so that we can return it for empty strings
}

void StringInterningDeinit(void)
{
    HashMapFree(&stringMap);
    ArenaDeinit(&stringArena);
}

#endif
#if defined (OG_DS_IMPLEMENTATION)

#define NIL_KEY (0ULL)
#define NIL_VALUE (0ULL)
#define MAP_MAX_LOAD 0.5

u64 hash(const u8* key, u32 length) {
  u64 hash = 14695981039346656037ULL;
  for (int32_t i = 0; i < length; i++) {
    hash ^= (u8)key[i];
    hash *= 1099511628211ULL;
  }
  LV_ASSERT(hash != NIL_KEY && "Oops hash collided with NIL key");
  return hash;
}

static hash_entry_t *FindEntry(hash_entry_t *entries, u32 capacity, u64 key)
{
    u32 index = key & (capacity - 1);
    hash_entry_t *tombstone = NULL;
    for (;;) {
        hash_entry_t *entry = &entries[index];
        if (entry->key == NIL_KEY) {
            if (entry->value == NIL_VALUE) {
                //empty value
                return tombstone != NULL ? tombstone : entry;
            }  else {
                //we found a tombstone
                if (tombstone == NULL) tombstone = entry;
            }
        } else if (entry->key == key) {
            //we found the key
            return entry;
        }

        index = (index + 1) & (capacity - 1);
    }
}

static void AdjustMapCapacity(hash_map_t *map, u32 newCapacity)
{
    if (map->arena) {
        LV_ASSERT(false && "We don't support resizing hash maps that use memory arenas since they can't be reallocated"); 
        return;
    }
    //create a new map with the new capacity and rehash all the old entries into it
    hash_entry_t *entries = reallocate(NULL, 0, newCapacity * sizeof(hash_entry_t));
    for (u32 i = 0; i <  newCapacity; i++) {
        entries[i].key = NIL_KEY;
        entries[i].value = NIL_VALUE;
    }

    map->count = 0;

    for (u32 i = 0; i < map->capacity; i++) {
        hash_entry_t *entry = &map->entries[i];
        
        //we don't copy empty entries or tombstones
        if (entry->key == NIL_KEY) continue;

        hash_entry_t *dest = FindEntry(entries, newCapacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;

        map->count++;
    }

    free(map->entries);

    map->entries = entries;
    map->capacity = newCapacity;
}

bool HashMapLookup(const hash_map_t *map, const void *key, u32 keyLength, u64 *outValue)
{
    bool success = false;
    if (map->count > 0) {
        u64 hashedKey = hash(key, keyLength);
        hash_entry_t *entry = FindEntry(map->entries, map->capacity, hashedKey);

        if (entry->key != NIL_KEY) {
            *outValue = entry->value;
            success = true;
        }
    }
    return success;
}

bool HashMapLookupString(const hash_map_t *map, u64 stringHash, const char **outValue)
{
    bool success = false;

    if (map->count > 0) {
        hash_entry_t *entry = FindEntry(map->entries, map->capacity, stringHash);
        if (entry->key != NIL_KEY) {
            *(uint64_t*)outValue = entry->value;
            success = true;
        }
    }

    return success;
}

static bool HashMapSetInternal(hash_map_t *map, u64 hashedKey, u64 value)
{
   if (map->count == map->capacity * MAP_MAX_LOAD) {
        u32 capacity = GROW_CAPACITY(map->capacity);
        AdjustMapCapacity(map, capacity);
    }

    hash_entry_t *entry = FindEntry(map->entries, map->capacity, hashedKey);
    bool isNewKey = (entry->key == NIL_KEY);

    if (isNewKey && entry->value == NIL_VALUE) map->count++; // we only increemnt the count if the new entry goes to a previously empty bucket

    entry->key = hashedKey;
    entry->value = value;

    return isNewKey;
}

bool HashMapSetString(hash_map_t *map, u64 stringId, const char *value)
{
    return HashMapSetInternal(map, stringId, (u64)value);
}

bool HashMapSet(hash_map_t *map, const void *key, u32 keyLength, u64 value)
{
    return HashMapSetInternal(map, hash(key, keyLength), value);
}

bool HashMapDelete(hash_map_t *map, const void *key, u32 keyLength)
{
    if (map->count == 0) {
        return false;
    }

    u64 hashedKey = hash(key, keyLength);
    hash_entry_t *entry = FindEntry(map->entries, map->capacity, hashedKey);
    if (entry->key == NIL_KEY) {
        return false;
    }

    //place a tombstone
    entry->key = NIL_KEY;
    entry->value = 1; //some value that isn't NIL_VAL
    
    return true;
}

void HashMapInit(hash_map_t *map)
{
    map->entries = NULL;
    map->capacity = 0;
    map->count = 0;
    map->arena = NULL;
}

void HashMapInitWithArena(hash_map_t *map, memory_arena_t *arena, u32 capacity)
{
    map->entries = PushArray(arena, capacity, hash_entry_t);
    for (u32 i = 0; i < capacity; i++) {
        map->entries[i].key = NIL_KEY;
        map->entries[i].value = NIL_VALUE;
    }
    map->capacity = capacity;
    map->count = 0;
    map->arena = arena;
}

void HashMapFree(hash_map_t *map)
{
    if (map->arena) {
        ArenaDeinit(map->arena);
    } else {
        free(map->entries);
    }
    HashMapInit(map);
}

#endif //OG_DS_IMPLEMENTATION

#endif //OG_DS_H
