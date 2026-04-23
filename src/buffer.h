
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
} Buffer;

void UploadBuffer(Buffer *buffer, const void *data, VkDeviceSize size, VkDeviceSize offset);
void CreateBuffer(Buffer *buffer, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags, VmaAllocator allocator);
void DestroyBuffer(Buffer *buffer, VmaAllocator allocator);

#endif

