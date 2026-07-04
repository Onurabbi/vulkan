#ifndef OG_VULKAN_H
#define OG_VULKAN_H

#include "vulkan_types.h"
#include "../resource.h"

void VulkanInit(vulkan_context_t *context);
void VulkanHotReload(vulkan_context_t *context);
void VulkanRender(void);
void VulkanShutdown(void);

void VulkanLoadTexture(resource_t *textureResource, const char *path, u32 layerCount);
void VulkanUnloadTexture(resource_t *textureResource);
void VulkanLoadShader(resource_t *shaderResource, const char *path, memory_arena_t *scratchArena, memory_arena_t *permanentArena);
void VulkanUnloadShader(resource_t *shaderResource);

#endif
