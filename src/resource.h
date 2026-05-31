#ifndef OG_RESOURCE_H
#define OG_RESOURCE_H

#include "common.h"
#include "og_ds.h"

typedef struct {
    hash_map_t map;
} resource_system_t;

void ResourceSystemInit(resource_system_t *resourceSystem, u32 resourceCapacity);
void ResourceSystemShutdown(void);
void ResourceSystemHotReload(resource_system_t *resourceSystem);

#endif
