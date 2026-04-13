#ifndef OG_DS_H
#define OG_DS_H

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

/****************************  Array ******************************************/
/**************************************************************************** */

typedef struct 
{
    uint32_t count;
    uint32_t capacity;
}array_header_t;

#define GROW_CAPACITY(old_capacity) ((old_capacity) < 8 ? 8 : (old_capacity) * 2)

#define array_header(arr)\
    ((array_header_t*)((char*)(arr) - sizeof(array_header_t)))

#define array_count(arr) \
    ((arr) ? array_header((arr))->count : 0)

#define array_capacity(arr) \
    ((arr) ? array_header((arr))->capacity : 0)

#define array_full(arr) \
    array_count((arr)) == array_capacity((arr))

#define array_push(arr,val) \
do { \
    if (array_full((arr))) { \
        (arr) = array_grow((arr), sizeof(*(arr)), array_count((arr)) + 1); \
    } \
    (arr)[array_header((arr))->count++] = val; \
} while(0)

#define array_resize(arr, new_count) \
do { \
    if ((new_count) > array_capacity((arr))) { \
        (arr) = array_grow((arr), sizeof(*(arr)), new_count); \
    } \
    array_header((arr))->count = new_count; \
} while(0)

#define array_delete(arr, i) \
do { \
    if ((i) < array_count((arr))) { \
        (arr)[(i)] = (arr)[array_count((arr)) - 1]; \
        array_header((arr))->count--; \
    } \
} while (0);

#define array_delete_non_swap(arr, i) \
do { \
    if ((i) < array_count((arr))) { \
        for (uint32_t idx = i; idx < array_count((arr)) - 1; idx++) { \
            (arr)[idx] = (arr)[idx + 1]; \
        } \
        array_header((arr))->count--; \
    } \
}while(0)

#define array_push_array(arr, src, count) \
do { \
    uint32_t old_count = array_count((arr)); \
    array_resize((arr), array_count((arr)) + (count)); \
    memmove((&(arr)[old_count]), (src), (count) * sizeof(*(arr))); \
}while(0);

#if defined (OG_DS_IMPLEMENTATION)

void *reallocate(void *ptr, size_t old_size, size_t new_size)
{
    (void)old_size;
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    void *result = realloc(ptr, new_size);
    //TODO: Remove ptr from a list of tracked allocations and add result to it, so we can detect memory leaks and double frees
    assert(result && "Unable to allocate memory!!");
    return result;
}

void *array_grow(void *arr, size_t elem_size, uint32_t min_capacity)
{
    uint32_t old_capacity = array_capacity(arr);
    uint32_t new_capacity = GROW_CAPACITY(old_capacity);
    
    if (new_capacity < min_capacity) {
        new_capacity = min_capacity;
    }

    array_header_t *header = NULL;
    if (old_capacity == 0) {
        header = reallocate(NULL, 0, new_capacity * elem_size + sizeof(array_header_t));
        header->capacity = new_capacity;
        header->count = 0;
    } else {
        array_header_t *old_header = array_header((arr));
        uint32_t new_count = old_header->count;
        if (new_capacity == min_capacity) {
            // the array was resized, so we double up the max capacity to preserve the amortized O(1) time complexity of push
            new_capacity *= 2;
            // Since the array was resized, we need to make sure the new size is correct
            new_count = min_capacity;
        }

        header = reallocate(old_header, old_capacity * elem_size + sizeof(array_header_t), 
                                                 new_capacity * elem_size + sizeof(array_header_t));
        header->capacity = new_capacity;
        header->count = new_count;
    }
    return &header[1];
}

void array_free(void *array) 
{
    if (!array)return;

    (void)reallocate(array_header(array), 0, 0);
}
#endif //OG_DS_IMPLEMENTATION

/**************************************************************************** */

/************************************* hash map ********************************/
/*******************************************************************************/

typedef struct {
    uint64_t key;
    uint64_t value;
} hash_entry_t;

typedef struct {
    hash_entry_t *entries;
    uint32_t capacity;
    uint32_t count;
}hash_map_t;

bool hash_map_lookup(const hash_map_t *map, const void *key, uint32_t key_length, uint64_t *out_value);
bool hash_map_set(hash_map_t *map, const void *key, uint32_t key_length, uint64_t value);
bool hash_map_delete(hash_map_t *map, const void *key, uint32_t key_length);
void hash_map_init(hash_map_t *map);
void hash_map_free(hash_map_t *map);

#if defined (OG_DS_IMPLEMENTATION)

#define NIL_KEY (0ULL)
#define NIL_VALUE (0ULL)
#define MAP_MAX_LOAD 0.5

uint64_t hash(const uint8_t* key, uint32_t length) {
  uint64_t hash = 14695981039346656037ULL;
  for (int i = 0; i < length; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 1099511628211ULL;
  }
  assert(hash != NIL_KEY && "Oops hash collided with NIL key");
  return hash;
}

static hash_entry_t *find_entry(hash_entry_t *entries, uint32_t capacity, uint64_t key)
{
    uint32_t index = key & (capacity - 1);
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

static void adjust_map_capacity(hash_map_t *map, uint32_t new_capacity)
{
    //create a new map with the new capacity and rehash all the old entries into it
    hash_entry_t *entries = reallocate(NULL, 0, new_capacity * sizeof(hash_entry_t));
    for (uint32_t i = 0; i <  new_capacity; i++) {
        entries[i].key = NIL_KEY;
        entries[i].value = NIL_VALUE;
    }

    map->count = 0;

    for (uint32_t i = 0; i < map->capacity; i++) {
        hash_entry_t *entry = &map->entries[i];
        
        //we don't copy empty entries or tombstones
        if (entry->key == NIL_KEY) continue;

        hash_entry_t *dest = find_entry(entries, new_capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;

        map->count++;
    }

    free(map->entries);

    map->entries = entries;
    map->capacity = new_capacity;
}

bool hash_map_lookup(const hash_map_t *map, const void *key, uint32_t key_length, uint64_t *out_value)
{
    bool success = false;
    if (map->count > 0) {
        uint64_t hashed_key = hash(key, key_length);
        hash_entry_t *entry = find_entry(map->entries, map->capacity, hashed_key);

        if (entry->key != NIL_KEY) {
            *out_value = entry->value;
            success = true;
        }
    }
    return success;
}

bool hash_map_lookup_string(const hash_map_t *map, uint64_t string_id, uint64_t *out_value)
{
    bool success = false;

    if (map->count > 0) {
        hash_entry_t *entry = find_entry(map->entries, map->capacity, string_id);
        if (entry->key != NIL_KEY) {
            *out_value = entry->value;
            success = true;
        }
    }

    return success;
}

static bool __hash_map_set(hash_map_t *map, uint64_t hashed_key, uint64_t value)
{
   if (map->count == map->capacity * MAP_MAX_LOAD) {
        uint32_t capacity = GROW_CAPACITY(map->capacity);
        adjust_map_capacity(map, capacity);
    }

    hash_entry_t *entry = find_entry(map->entries, map->capacity, hashed_key);
    bool is_new_key = (entry->key == NIL_KEY);

    if (is_new_key && entry->value == NIL_VALUE) map->count++; // we only increemnt the count if the new entry goes to a previously empty bucket

    entry->key = hashed_key;
    entry->value = value;

    return is_new_key;
}

bool hash_map_set_string(hash_map_t *map, uint64_t string_id, uint64_t value)
{
    return __hash_map_set(map, string_id, value);
}

bool hash_map_set(hash_map_t *map, const void *key, uint32_t key_length, uint64_t value)
{
    return __hash_map_set(map, hash(key, key_length), value);
}

bool hash_map_delete(hash_map_t *map, const void *key, uint32_t key_length)
{
    if (map->count == 0) {
        return false;
    }

    uint64_t hashed_key = hash(key, key_length);
    hash_entry_t *entry = find_entry(map->entries, map->capacity, hashed_key);
    if (entry->key == NIL_KEY) {
        return false;
    }

    //place a tombstone
    entry->key = NIL_KEY;
    entry->value = 1; //some value that isn't NIL_VAL
    
    return true;
}

void hash_map_init(hash_map_t *map)
{
    map->entries = NULL;
    map->capacity = 0;
    map->count = 0;
}

void hash_map_free(hash_map_t *map)
{
    array_free(map->entries);
    hash_map_init(map);
}

#endif //OG_DS_IMPLEMENTATION
#endif //OG_DS_H
