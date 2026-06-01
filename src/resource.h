#ifndef OG_RESOURCE_H
#define OG_RESOURCE_H

#include "common.h"
#include "og_ds.h"

#include "vulkan/vulkan_types.h"
#include <SDL3/SDL_mutex.h>

typedef struct {
    vertex_t *vertices;
    const char *path;
} mesh_resource_t;

typedef struct {
    hash_map_t map; 
    mesh_resource_t *meshResources;
    SDL_Mutex *mutex;
} resource_system_t;

void ResourceSystemInit(resource_system_t *resourceSystem, u32 resourceCapacity);
void ResourceSystemLoadResource(const char *path);
mesh_resource_t *ResourceSystemGetMeshResource(const char *path);
void ResourceSystemShutdown(void);
void ResourceSystemHotReload(resource_system_t *resourceSystem);

#endif
