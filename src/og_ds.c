#include "og_ds.h"
#include "platform.h"

u64 hash(const u8* key, u32 length) {
  u64 hash = 14695981039346656037ULL;
  for (u32 i = 0; i < length; i++) {
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

b8 HashMapLookup(const hash_map_t *map, const void *key, u32 keyLength, u64 *outValue)
{
    b8 success = false;
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

b8 HashMapLookupString(const hash_map_t *map, u64 stringHash, const char **outValue)
{
    b8 success = false;

    if (map->count > 0) {
        hash_entry_t *entry = FindEntry(map->entries, map->capacity, stringHash);
        if (entry->key != NIL_KEY) {
            *(u64*)outValue = entry->value;
            success = true;
        }
    }

    return success;
}

static b8 HashMapSetInternal(hash_map_t *map, u64 hashedKey, u64 value)
{
   if (map->count == map->capacity * MAP_MAX_LOAD) {
        u32 capacity = GROW_CAPACITY(map->capacity);
        AdjustMapCapacity(map, capacity);
    }

    hash_entry_t *entry = FindEntry(map->entries, map->capacity, hashedKey);
    b8 isNewKey = (entry->key == NIL_KEY);

    if (isNewKey && entry->value == NIL_VALUE) map->count++; // we only increemnt the count if the new entry goes to a previously empty bucket

    entry->key = hashedKey;
    entry->value = value;

    return isNewKey;
}

b8 HashMapSetString(hash_map_t *map, u64 stringId, const char *value)
{
    return HashMapSetInternal(map, stringId, (u64)value);
}

b8 HashMapSet(hash_map_t *map, const void *key, u32 keyLength, u64 value)
{
    return HashMapSetInternal(map, hash(key, keyLength), value);
}

b8 HashMapDelete(hash_map_t *map, const void *key, u32 keyLength)
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
    } else {
        free(map->entries);
    }
    HashMapInit(map);
}

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
