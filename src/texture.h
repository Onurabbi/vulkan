#ifndef VK_TEXTURE_H
#define VK_TEXTURE_H

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>
#include <stdbool.h>

#include "buffer.h"

typedef struct {
    VmaAllocation allocation;
    VkImage image;
    VkImageView view;
    VkSampler sampler;
}Texture;

bool CreateTexture(Texture *texture, VkDescriptorImageInfo *imageInfo, Buffer *scratch, VkDevice device, VmaAllocator allocator, VkCommandPool pool, VkQueue queue, const char *path);
void DestroyTexture(Texture *texture, VmaAllocator allocator, VkDevice device);
VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels);
VkImage CreateImage(VkDevice device, VmaAllocator allocator, VkFormat format,  VkImageUsageFlags usage, uint32_t width, uint32_t height, uint32_t mipLevels, VmaAllocation *allocation);
#endif