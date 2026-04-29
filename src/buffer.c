#include "common.h"
#include "buffer.h"
#include <volk/volk.h>

#include <string.h>

void UploadBuffer(buffer_t *buffer, const void *data, VkDeviceSize size, VkDeviceSize offset)
{
    memcpy((char*)buffer->allocInfo.pMappedData + offset, data, size);
}

void CreateBuffer(buffer_t *buffer, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags, VmaAllocator allocator)
{
    buffer->size = size;

    VkBufferCreateInfo bufferCI = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
    };

    VmaAllocationCreateInfo bufferAllocCI = {
        .flags = allocFlags,
        .usage = memoryUsage,
    };

    VK_CHECK(vmaCreateBuffer(allocator, &bufferCI, &bufferAllocCI, &buffer->buffer, &buffer->allocation, &buffer->allocInfo));

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo bufferAddressInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer->buffer
        };
        buffer->deviceAddress = vkGetBufferDeviceAddress(device, &bufferAddressInfo);
    }
}

void DestroyBuffer(buffer_t *buffer, VmaAllocator allocator)
{
    vmaDestroyBuffer(allocator, buffer->buffer, buffer->allocation);
}
