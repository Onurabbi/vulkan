#ifndef VK_TEXTURE_H
#define VK_TEXTURE_H

#include "vulkan_types.h"
#include "../resource.h"

b8 CreateTexture(texture_resource_t *texture, buffer_t *scratch, VkDevice device, VmaAllocator allocator, VkCommandPool pool, VkQueue queue, VkSampler sampler, const char *path, u32 layerCount);
void DestroyTexture(texture_resource_t *texture, VmaAllocator allocator, VkDevice device);
VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, u32 mipLevels, u32 layerCount);
VkImage CreateImage(VkDevice device, VmaAllocator allocator, VkFormat format,  VkImageUsageFlags usage, u32 width, u32 height, u32 mipLevels, u32 layerCount, VmaAllocation *allocation);
VkSampler CreateTextureSampler(VkDevice device);
VkSampler CreateCubemapSampler(VkDevice device);
#endif
