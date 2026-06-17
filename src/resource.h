#ifndef OG_RESOURCE_H
#define OG_RESOURCE_H

#include "common.h"
#include "og_ds.h"

#include "vulkan/vulkan_types.h"
#include <SDL3/SDL_mutex.h>

typedef struct {
    u32 firstMeshIndex; //index into the meshes array that belongs to the resources system.
    u32 meshCount;
} mesh_resource_t;

typedef struct {
    const u8 *spirv; //dynamic array
    VkShaderStageFlagBits stage;

    u32 localSizeX;
    u32 localSizeY;
    u32 localSizeZ;

    b8 usesPushConstants;
} shader_resource_t;

typedef struct {
    VmaAllocation allocation;
    VkImage image;
    VkImageView view;
    VkSampler sampler;
    VkImageLayout layout;
} texture_resource_t;

typedef enum {
    RESOURCE_TYPE_INVALID = 0,
    RESOURCE_TYPE_MESH = 1,
    RESOURCE_TYPE_SHADER = 2,
    RESOURCE_TYPE_TEXTURE = 3,
}resource_type_t;

typedef struct {
    const char *path;
    resource_type_t type;
    union {
        mesh_resource_t    mesh;
        shader_resource_t  shader;
        texture_resource_t texture;
    };
} resource_t;

typedef struct {
    SDL_Mutex *mutex;
    const char *assetDir;
    hash_map_t map; 

    resource_t *resources;
    
    // geometry storage. this could be temporary?
    vertex_t *vertices; 
    u32      *indices;
    mesh_t   *meshes;

    u32 sceneCount;
    u32 meshCount;
    u32 shaderCount;
    u32 textureCount;
} resource_system_t;

void ResourceSystemInit(resource_system_t *resourceSystem, u32 resourceCapacity);
void ResourceSystemShutdown(void);
void ResourceSystemHotReload(resource_system_t *resourceSystem);

const resource_t *ResourceSystemGetResource(const char *fileName);
const resource_t **ResourceSystemGetTextures(memory_arena_t *arena);
const geometry_t ResourceSystemGetGeometry(void);
#endif
