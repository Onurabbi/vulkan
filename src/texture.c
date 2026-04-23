#include "common.h"
#include "texture.h"

#include <volk/volk.h>


#include <string.h>
#include <ktx.h>
#include <ktxvulkan.h>

bool CreateTexture(Texture *texture, VkDescriptorImageInfo *imageInfo, Buffer *scratch, VkDevice device, VmaAllocator allocator, VkCommandPool pool, VkQueue queue, const char *path)
{
    ktxTexture *ktxTex = NULL;
    KTX_error_code err = ktxTexture_CreateFromNamedFile(path, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
    if (err != KTX_SUCCESS) {
        fprintf(stderr, "Failed to load texture from file %s: %d\n", path, err);
        return false;
    }

    UploadBuffer(scratch, ktxTex->pData, ktxTex->dataSize, 0);
    VkFormat format = ktxTexture_GetVkFormat(ktxTex);
    texture->image = CreateImage(device, allocator, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 
        ktxTex->baseWidth, ktxTex->baseHeight, ktxTex->numLevels, &texture->allocation);

    texture->view = CreateImageView(device, texture->image, format, VK_IMAGE_ASPECT_COLOR_BIT, ktxTex->numLevels);
    VkFenceCreateInfo fenceOneTimeCI = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };

    VkFence fenceOneTime;
    VK_CHECK(vkCreateFence(device, &fenceOneTimeCI, NULL, &fenceOneTime));
    VkCommandBufferAllocateInfo cbOneTimeAI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .commandBufferCount = 1,
    };

    VkCommandBuffer cbOneTime;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbOneTimeAI, &cbOneTime));

    VkCommandBufferBeginInfo cbOneTimeBI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VK_CHECK(vkBeginCommandBuffer(cbOneTime, &cbOneTimeBI));

    VkImageMemoryBarrier2 barrierTexImage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = texture->image,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = ktxTex->numLevels,
        .subresourceRange.layerCount = 1,
    };

    VkDependencyInfo barrierTexInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrierTexImage,
    };

    vkCmdPipelineBarrier2(cbOneTime, &barrierTexInfo);

    VkBufferImageCopy copyRegions[32] = {0};
    for (uint32_t j = 0; j < ktxTex->numLevels; j++) {
        ktx_size_t mipOffset = 0;
        KTX_error_code ret = ktxTexture_GetImageOffset(ktxTex, j, 0, 0, &mipOffset);
        copyRegions[j].bufferOffset = mipOffset;
        copyRegions[j].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegions[j].imageSubresource.mipLevel = j;
        copyRegions[j].imageSubresource.layerCount = 1;
        uint32_t mipWidth = ktxTex->baseWidth >> j;
        uint32_t mipHeight = ktxTex->baseHeight >> j;
        if (mipWidth == 0) mipWidth = 1;
        if (mipHeight == 0) mipHeight = 1;
        copyRegions[j].imageExtent.width = mipWidth;
        copyRegions[j].imageExtent.height = mipHeight;
        copyRegions[j].imageExtent.depth = 1;
        copyRegions[j].imageOffset.x = 0;
        copyRegions[j].imageOffset.y = 0;
        copyRegions[j].imageOffset.z = 0;
        copyRegions[j].bufferRowLength = 0;
        copyRegions[j].bufferImageHeight = 0;
    }

    vkCmdCopyBufferToImage(cbOneTime, scratch->buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ktxTex->numLevels, copyRegions);

    VkImageMemoryBarrier2 barrierTexRead = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        .image = texture->image,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = ktxTex->numLevels,
        .subresourceRange.layerCount = 1,
    };

    barrierTexInfo.pImageMemoryBarriers = &barrierTexRead;
    
    vkCmdPipelineBarrier2(cbOneTime, &barrierTexInfo);

    VK_CHECK(vkEndCommandBuffer(cbOneTime));

    VkSubmitInfo oneTimeSI = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cbOneTime
    };

    VK_CHECK(vkQueueSubmit(queue, 1, &oneTimeSI, fenceOneTime));
    VK_CHECK(vkWaitForFences(device, 1, &fenceOneTime, VK_TRUE, UINT64_MAX));

    vkFreeCommandBuffers(device, pool, 1, &cbOneTime);
    vkDestroyFence(device, fenceOneTime, NULL);

    VkSamplerCreateInfo samplerCI = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = 8.0f,
        .maxLod = (float)ktxTex->numLevels,
    };

    VK_CHECK(vkCreateSampler(device, &samplerCI, NULL, &texture->sampler));

    imageInfo->sampler = texture->sampler;
    imageInfo->imageView = texture->view;
    imageInfo->imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

    ktxTexture_Destroy(ktxTex);
    return true;
}

void DestroyTexture(Texture *texture, VmaAllocator allocator, VkDevice device)
{
    vkDestroySampler(device, texture->sampler, NULL);
    vkDestroyImageView(device, texture->view, NULL);
    vmaDestroyImage(allocator, texture->image, texture->allocation);
}

VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels)
{
    VkImageViewCreateInfo viewCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange.aspectMask = aspect,
        .subresourceRange.levelCount = mipLevels,
        .subresourceRange.layerCount = 1,
    };

    VkImageView view;
    VK_CHECK(vkCreateImageView(device, &viewCI, NULL, &view));
    return view;
}

VkImage CreateImage(VkDevice device, VmaAllocator allocator, VkFormat format,  VkImageUsageFlags usage, uint32_t width, uint32_t height, uint32_t mipLevels, VmaAllocation *allocation)
{
    VkImageCreateInfo imgCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {.width = width, .height = height, .depth = 1},
        .mipLevels = mipLevels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VmaAllocationCreateInfo imgAllocCI = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
    };

    VkImage image;
    VK_CHECK(vmaCreateImage(allocator,  &imgCI, &imgAllocCI, &image, allocation, NULL));
    
    return image;
}
