
#ifndef VK_BUFFER_H
#define VK_BUFFER_H

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

typedef struct {
    VmaAllocation allocation;
    VmaAllocationInfo allocInfo;
    VkBuffer buffer;
    VkDeviceAddress deviceAddress;
    VkDeviceSize size;
} buffer_t;

void UploadBuffer(buffer_t *buffer, const void *data, VkDeviceSize size, VkDeviceSize offset);
void CreateBuffer(buffer_t *buffer, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags, VmaAllocator allocator);
void DestroyBuffer(buffer_t *buffer, VmaAllocator allocator);

#endif

