#ifndef VK_TEXTURE_H
#define VK_TEXTURE_H

#include "vulkan_types.h"
#include "../resource.h"

b8 CreateTexture(vulkan_context_t *ctx, texture_resource_t *texture, texture_desc_t desc, const char *path, VkCommandBuffer cb, VkFence fence);
void DestroyTexture(texture_resource_t *texture, VmaAllocator allocator, VkDevice device);
VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, u32 mipLevels, u32 layerCount);
VkImage CreateImage(VkDevice device, VmaAllocator allocator, VkFormat format,  VkImageUsageFlags usage, u32 width, u32 height, u32 mipLevels, u32 layerCount, VkSampleCountFlagBits samples, VmaAllocation *allocation);
VkSampler CreateTextureSampler(VkDevice device, u32 numLevels, f32 maxAnisotropy);
VkSampler CreateCubemapSampler(VkDevice device, u32 numLevels, f32 maxAnisotropy);
VkSampler CreateLUTSampler(VkDevice device, u32 numLevels, f32 maxAnisotropy);
#endif
