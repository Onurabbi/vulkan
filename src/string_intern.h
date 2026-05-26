#ifndef STRING_INTERN_H
#define STRING_INTERN_H

#include "memory.h"
#include "og_ds.h"

typedef struct {
    memory_arena_t *arena;
    hash_map_t      map;
}string_interning_system_t;

const char *StringIntern(string_interning_system_t *system, const char *str);
void StringInterningInit(string_interning_system_t *system);
void StringInterningDeinit(string_interning_system_t *system);

#endif

