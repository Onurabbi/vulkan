#include "../common.h"
#include "../resource.h"
#include "../log.h"

#include "texture.h"
#include "volk.h"
#include "vma.h"
#include "vulkan/vulkan_core.h"

#include <stdio.h>

typedef struct
{
	u32 dwSize;
	u32 dwFlags;
	u32 dwFourCC;
	u32 dwRGBBitCount;
	u32 dwRBitMask;
	u32 dwGBitMask;
	u32 dwBBitMask;
	u32 dwABitMask;
} DDS_PIXELFORMAT;

typedef struct
{
	u32 dwSize;
	u32 dwFlags;
	u32 dwHeight;
	u32 dwWidth;
	u32 dwPitchOrLinearSize;
	u32 dwDepth;
	u32 dwMipMapCount;
	u32 dwReserved1[11];
	DDS_PIXELFORMAT ddspf;
	u32 dwCaps;
	u32 dwCaps2;
	u32 dwCaps3;
	u32 dwCaps4;
	u32 dwReserved2;
} DDS_HEADER;

typedef struct
{
	u32 dxgiFormat;
	u32 resourceDimension;
	u32 miscFlag;
	u32 arraySize;
	u32 miscFlags2;
} DDS_HEADER_DXT10;

const u32 DDSCAPS2_CUBEMAP = 0x200;
const u32 DDSCAPS2_VOLUME = 0x200000;

const u32 DDS_DIMENSION_TEXTURE2D = 3;

typedef enum
{
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
	DXGI_FORMAT_BC1_UNORM = 71,
	DXGI_FORMAT_BC1_UNORM_SRGB = 72,
	DXGI_FORMAT_BC2_UNORM = 74,
	DXGI_FORMAT_BC2_UNORM_SRGB = 75,
	DXGI_FORMAT_BC3_UNORM = 77,
	DXGI_FORMAT_BC3_UNORM_SRGB = 78,
	DXGI_FORMAT_BC4_UNORM = 80,
	DXGI_FORMAT_BC4_SNORM = 81,
	DXGI_FORMAT_BC5_UNORM = 83,
	DXGI_FORMAT_BC5_SNORM = 84,
	DXGI_FORMAT_BC6H_UF16 = 95,
	DXGI_FORMAT_BC6H_SF16 = 96,
	DXGI_FORMAT_BC7_UNORM = 98,
	DXGI_FORMAT_BC7_UNORM_SRGB = 99,
} DXGI_FORMAT;

static u32 fourCC(const char *str)
{
    return (u32)str[0] | ((u32)str[1] << 8) | ((u32)str[2] << 16) | ((u32)str[3] << 24);
}

static VkFormat getFormat(const DDS_HEADER *header, const DDS_HEADER_DXT10 *header10)
{
	if (header->ddspf.dwFourCC == fourCC("DXT1"))
		return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	if (header->ddspf.dwFourCC == fourCC("DXT3"))
		return VK_FORMAT_BC2_UNORM_BLOCK;
	if (header->ddspf.dwFourCC == fourCC("DXT5"))
		return VK_FORMAT_BC3_UNORM_BLOCK;
	if (header->ddspf.dwFourCC == fourCC("ATI1"))
		return VK_FORMAT_BC4_UNORM_BLOCK;
	if (header->ddspf.dwFourCC == fourCC("ATI2"))
		return VK_FORMAT_BC5_UNORM_BLOCK;

	if (header->ddspf.dwFourCC == fourCC("DX10"))
	{
		switch (header10->dxgiFormat)
		{
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case DXGI_FORMAT_BC1_UNORM:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case DXGI_FORMAT_BC2_UNORM:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            return VK_FORMAT_BC2_SRGB_BLOCK;
        case DXGI_FORMAT_BC3_UNORM:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return VK_FORMAT_BC3_SRGB_BLOCK;
		case DXGI_FORMAT_BC4_UNORM:
			return VK_FORMAT_BC4_UNORM_BLOCK;
		case DXGI_FORMAT_BC4_SNORM:
			return VK_FORMAT_BC4_SNORM_BLOCK;
		case DXGI_FORMAT_BC5_UNORM:
			return VK_FORMAT_BC5_UNORM_BLOCK;
		case DXGI_FORMAT_BC5_SNORM:
			return VK_FORMAT_BC5_SNORM_BLOCK;
        case DXGI_FORMAT_BC6H_UF16:
            return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case DXGI_FORMAT_BC6H_SF16:
            return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case DXGI_FORMAT_BC7_UNORM:
            return VK_FORMAT_BC7_UNORM_BLOCK;
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return VK_FORMAT_BC7_SRGB_BLOCK;
		}
	}

	return VK_FORMAT_UNDEFINED;
}

static size_t GetImagePitch(u32 width, u32 height, VkFormat format, u32 level)
{
    width >>= level;
    height >>= level;

    switch (format) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            return ((width  + 3) / 4) * ((height + 3) / 4) * 8;
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return ((width + 3) / 4) * ((height + 3) / 4) * 16;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return width * height * 16;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return width * height * 8;
        default:
            return 0;
	}
}

static size_t GetImageSize(u32 width, u32 height, u32 levels, u32 layers, VkFormat format)
{
	size_t result = 0;

	for (u32 i = 0; i < levels; ++i)
	{
		result += GetImagePitch(width, height, format, i);
	}

	return result * layers;
}

b8 CreateTexture(vulkan_context_t *ctx, texture_resource_t *texture, texture_desc_t desc, const char *path, VkCommandBuffer cb, VkFence fence)
{
    b8 success = false;
    FILE *file = fopen(path, "rb");
    if (!file) {
        LOGE("Failed to open texture file %s", path);
        goto exit;
    }

    u32 magic = 0;
    if (fread(&magic, sizeof(magic), 1, file) != 1 || magic != fourCC("DDS ")) {
        LV_ASSERT(false);
        goto exit;
    }

    DDS_HEADER header = {0};
    if (fread(&header, sizeof(header), 1, file) != 1) {
        LV_ASSERT(false);
        goto exit;
    }

    DDS_HEADER_DXT10 header10 = {0};
    if (header.ddspf.dwFourCC == fourCC("DX10")) {
        if (fread(&header10, sizeof(header10), 1, file) != 1) {
            LV_ASSERT(false);
            goto exit;
        }
    }

   	if (header.dwSize != sizeof(header) || header.ddspf.dwSize != sizeof(header.ddspf)) {
        LV_ASSERT(false);
        goto exit;
    }

	if (header.dwCaps2 & (DDSCAPS2_VOLUME)){
	    LV_ASSERT(false);
		goto exit;
	}

	if (header.ddspf.dwFourCC == fourCC("DX10") && header10.resourceDimension != DDS_DIMENSION_TEXTURE2D) {
	    LV_ASSERT(false);
	    goto exit;
	}

    VkFormat format = getFormat(&header, &header10);
	if (format == VK_FORMAT_UNDEFINED) {
	    LV_ASSERT(false);
	    goto exit;
	}

    u32 mipLevels = header.dwMipMapCount ? header.dwMipMapCount : 1;

    size_t imageSize = GetImageSize(header.dwWidth, header.dwHeight, mipLevels, desc.layerCount, format);

	if (ctx->scratchBuffer.size < imageSize) {
	    LV_ASSERT(false);
	    goto exit;
	}

	texture->image = CreateImage(ctx->device, ctx->allocator, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        header.dwWidth, header.dwHeight, mipLevels, desc.layerCount, VK_SAMPLE_COUNT_1_BIT, &texture->allocation);

	size_t readSize = fread(ctx->scratchBuffer.allocInfo.pMappedData, 1, imageSize, file);
	if (readSize != imageSize) {
	    LV_ASSERT(false);
	    goto exit;
	}

	if (fgetc(file) != -1) {
	    LV_ASSERT(false);
	    goto exit;
	}

    texture->view = CreateImageView(ctx->device, texture->image, format, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels, desc.layerCount);

    VkCommandBufferBeginInfo cbOneTimeBI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VK_CHECK(vkBeginCommandBuffer(cb, &cbOneTimeBI));

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
        .subresourceRange.levelCount = mipLevels,
        .subresourceRange.layerCount = desc.layerCount,
    };

    VkDependencyInfo barrierTexInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrierTexImage,
    };

    vkCmdPipelineBarrier2(cb, &barrierTexInfo);

    // Allocate enough space for all copy regions (e.g. up to 13 mips * 6 faces = 78 regions)
    VkBufferImageCopy copyRegions[256];
    u32 regionIndex = 0;

    // DDS stores cubemap data face-first. Each face contains all of its mip
    // levels before the data for the next face begins.
    size_t faceSize = GetImageSize(header.dwWidth, header.dwHeight, mipLevels, 1, format);
    size_t mipStartOffset = 0;

    for (u32 level = 0; level < mipLevels; level++) {
        size_t pitch = GetImagePitch(header.dwWidth, header.dwHeight, format, level);

        u32 mipWidth = header.dwWidth >> level;
        u32 mipHeight = header.dwHeight >> level;

        if (mipWidth == 0) mipWidth = 1;
        if (mipHeight == 0) mipHeight = 1;

        for (u32 layer = 0; layer < desc.layerCount; layer++) {
            VkBufferImageCopy region = {0};

            region.bufferOffset = layer * faceSize + mipStartOffset;

            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageSubresource.layerCount = 1;

            region.imageExtent.width = mipWidth;
            region.imageExtent.height = mipHeight;
            region.imageExtent.depth = 1;

            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;

            copyRegions[regionIndex++] = region;
        }

        mipStartOffset += pitch;
    }
    u32 copyCount = mipLevels * desc.layerCount;
    vkCmdCopyBufferToImage(cb, ctx->scratchBuffer.buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copyCount, copyRegions);

    LV_ASSERT(faceSize * desc.layerCount == imageSize);

    VkImageMemoryBarrier2 barrierTexRead = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = texture->image,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = mipLevels,
        .subresourceRange.layerCount = desc.layerCount,
    };

    barrierTexInfo.pImageMemoryBarriers = &barrierTexRead;

    vkCmdPipelineBarrier2(cb, &barrierTexInfo);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo oneTimeSI = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb
    };

    VK_CHECK(vkResetFences(ctx->device, 1, &fence));
    VK_CHECK(vkQueueSubmit(ctx->queue, 1, &oneTimeSI, fence));
    VK_CHECK(vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX));

    success = true;

    texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture->layerCount = desc.layerCount;
    texture->format = format;
exit:
    if (file) {
        fclose(file);
    }

    return success;
}

void DestroyTexture(texture_resource_t *texture, VmaAllocator allocator, VkDevice device)
{
    vkDestroyImageView(device, texture->view, NULL);
    vmaDestroyImage(allocator, texture->image, texture->allocation);
}

VkSampler CreateTextureSampler(VkDevice device, u32 numLevels, f32 maxAnisotropy)
{
    VkSampler result = VK_NULL_HANDLE;
    VkSamplerCreateInfo samplerCI = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = maxAnisotropy, //check this
        .maxLod = (f32)numLevels,
    };

    VK_CHECK(vkCreateSampler(device, &samplerCI, NULL, &result));
    return result;
}

VkSampler CreateCubemapSampler(VkDevice device, u32 numLevels, f32 maxAnisotropy)
{
    VkSampler result = VK_NULL_HANDLE;
    VkSamplerCreateInfo samplerCI = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .anisotropyEnable = VK_TRUE,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = numLevels > 0 ? (f32)(numLevels - 1) : 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        .maxAnisotropy = maxAnisotropy,
    };
    VK_CHECK(vkCreateSampler(device, &samplerCI, NULL, &result));
    return result;
}

VkSampler CreateLUTSampler(VkDevice device, u32 numLevels, f32 maxAnisotropy)
{
    VkSampler result = VK_NULL_HANDLE;
    VkSamplerCreateInfo samplerCI = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .anisotropyEnable = VK_TRUE,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = numLevels > 0 ? (f32)(numLevels - 1) : 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        .maxAnisotropy = maxAnisotropy,
    };
    VK_CHECK(vkCreateSampler(device, &samplerCI, NULL, &result));
    return result;
}

VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, u32 mipLevels, u32 layerCount)
{
    VkImageViewCreateInfo viewCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = layerCount == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_CUBE,
        .format = format,
        .subresourceRange.aspectMask     = aspect,
        .subresourceRange.baseMipLevel   = 0,
        .subresourceRange.levelCount     = mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount     = layerCount,

    };

    VkImageView view;
    VK_CHECK(vkCreateImageView(device, &viewCI, NULL, &view));
    return view;
}

VkImage CreateImage(VkDevice device, VmaAllocator allocator, VkFormat format,  VkImageUsageFlags usage, u32 width, u32 height, u32 mipLevels, u32 layerCount, VkSampleCountFlagBits samples, VmaAllocation *allocation)
{
    // Multisampled images cannot be mipmapped or arrayed
    LV_ASSERT(samples == VK_SAMPLE_COUNT_1_BIT || (mipLevels == 1 && layerCount == 1));

    VkImageCreateInfo imgCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {.width = width, .height = height, .depth = 1},
        .mipLevels = mipLevels,
        .arrayLayers = layerCount,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .flags = (layerCount == 6) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
    };

    VmaAllocationCreateInfo imgAllocCI = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
    };

    VkImage image;
    VK_CHECK(vmaCreateImage(allocator,  &imgCI, &imgAllocCI, &image, allocation, NULL));

    return image;
}
