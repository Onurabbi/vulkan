#ifndef OG_STRING_INTERN_H
#define OG_STRING_INTERN_H

#include "og_ds.h"

typedef struct {
    memory_arena_t *arena;
    hash_map_t      map;
}string_interning_system_t;

const char *StringIntern(const char *str);
void StringUtilsInit(string_interning_system_t *system);
void StringUtilsHotReload(string_interning_system_t *system);
void StringUtilsShutdown(void);
const char *StringUtilsGetExtensionFromPath(const char *path);
#endif
