#ifndef VK_TEXTURE_H
#define VK_TEXTURE_H

#include "../common.h"
#include "vulkan_types.h"

b8 CreateTexture(texture_t *texture, VkDescriptorImageInfo *imageInfo, buffer_t *scratch, VkDevice device, VmaAllocator allocator, VkCommandPool pool, VkQueue queue, const char *path);
void DestroyTexture(texture_t *texture, VmaAllocator allocator, VkDevice device);
VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, u32 mipLevels);
VkImage CreateImage(VkDevice device, VmaAllocator allocator, VkFormat format,  VkImageUsageFlags usage, u32 width, u32 height, u32 mipLevels, VmaAllocation *allocation);
#endif
