#ifndef OG_RESOURCE_H
#define OG_RESOURCE_H

#include "common.h"
#include "og_ds.h"

#include "vulkan/vulkan_types.h"
#include <SDL3/SDL_mutex.h>

typedef struct {
    const char *path;
    union {
        vertex_t *vertices; //meshes;
        u8 *spirv; //shaders
    };
}resource_t;

typedef struct {
    const char *assetDir;
    hash_map_t map; 
    resource_t *shaderResources;
    resource_t *meshResources;
    SDL_Mutex *mutex;
} resource_system_t;

void ResourceSystemInit(resource_system_t *resourceSystem, u32 resourceCapacity);
void ResourceSystemShutdown(void);
void ResourceSystemHotReload(resource_system_t *resourceSystem);

const resource_t *ResourceSystemGetResource(const char *fileName);
#endif
