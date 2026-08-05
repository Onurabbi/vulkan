#include "vulkan.h"
#include "shader.h"
#include "buffer.h"
#include "texture.h"
#include "vma.h"
#include "volk.h"
#include "vulkan/vulkan_core.h"
#include "vulkan_types.h"

#include "../platform.h"
#include "../common.h"
#include "../log.h"
#include "../limits.h"
#include "../resource.h"
#include "../renderer.h"

#include <stdio.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#define OG_DS_IMPLEMENTATION
#include "../og_ds.h"

static vulkan_context_t *gCtx;

#define gravity 9.81f

static inline VkCommandBuffer CommandBuffer(vulkan_context_t *ctx)
{
    return ctx->commandBuffers[ctx->frameIndex];
}

static inline VkFence Fence(vulkan_context_t *ctx)
{
    return ctx->fences[ctx->frameIndex];
}

static inline f32 JonswapAlpha(f32 fetch, f32 windSpeed)
{
    return 0.076f * powf(gravity * fetch / windSpeed / windSpeed, -0.22f);
}

static inline f32 JonswapPeakFrequency(f32 fetch, f32 windSpeed)
{
    return 22 * powf(windSpeed * fetch / gravity / gravity, -0.33f);
}

static void FillSpectrumParameters(jonswap_data_t *jonswap, f32 scale, f32 windSpeed, f32 windDirection, f32 fetch, f32 spreadBlend, f32 swell, f32 peakEnhancement, f32 shortWavesFade)
{
    jonswap->scale = scale;
    jonswap->angle = windDirection * HMM_PI / 180.0f;
    jonswap->spreadBlend = spreadBlend;
    jonswap->swell = HMM_Clamp(0.01f, swell, 1.0f);
    jonswap->alpha = JonswapAlpha(fetch, windSpeed);
    jonswap->peakOmega = JonswapPeakFrequency(fetch, windSpeed);
    jonswap->gamma = peakEnhancement;
    jonswap->shortWavesFade = shortWavesFade;
}

// Multisampled color target the scene is rendered into. Its contents are resolved into
// the swapchain image at the end of the render, so they never need to be stored.
static void CreateMSAAColorImage(vulkan_context_t *ctx)
{
    VmaAllocation allocation;
    ctx->msaaColorImage.image = CreateImage(ctx->device, ctx->allocator, ctx->msaaColorImage.format,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
        ctx->window.w, ctx->window.h, 1, 1, ctx->sampleCount, &allocation);
    ctx->msaaColorImage.view = CreateImageView(ctx->device, ctx->msaaColorImage.image, ctx->msaaColorImage.format,
        VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    ctx->msaaColorImage.allocation = allocation;
}

static void DestroyMSAAColorImage(vulkan_context_t *ctx)
{
    vkDestroyImageView(ctx->device, ctx->msaaColorImage.view, NULL);
    vmaDestroyImage(ctx->allocator, ctx->msaaColorImage.image, ctx->msaaColorImage.allocation);
}

static inline texture_desc_t MakeBRDFLUTDescription(vulkan_context_t *ctx, void *data)
{
    return (texture_desc_t) {
        .cb = CommandBuffer(ctx),
        .pipeline = ctx->genBRDFLUTPipeline.pipeline,
        .pipelineLayout = ctx->genBRDFLUTPipeline.pipelineLayout,
        .descriptorSet = ctx->descriptorSetTex,
        .width = 512,
        .height = 512,
        .sampler = ctx->lutSampler,
        .format = VK_FORMAT_R16G16_SFLOAT,
        .layerCount = 1,
        .pushConstantData = data,
        .usesMips = false
    };
}

static inline texture_desc_t MakeDiffuseIrradianceDesc(vulkan_context_t *ctx, void *data)
{
    return (texture_desc_t) {
        .cb = CommandBuffer(ctx),
        .pipeline = ctx->diffuseIrradianceMapPipeline.pipeline,
        .pipelineLayout = ctx->diffuseIrradianceMapPipeline.pipelineLayout,
        .descriptorSet = ctx->descriptorSetTex,
        .width = 64,
        .height = 64,
        .sampler = ctx->cubemapSampler,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .layerCount = 6,
        .pushConstantData = data,
        .usesMips = true
    };
}

static inline texture_desc_t MakePrefilteredEnvDesc(vulkan_context_t *ctx, void *data)
{
    return (texture_desc_t) {
        .cb = CommandBuffer(ctx),
        .pipeline = ctx->prefilteredEnvMapPipeline.pipeline,
        .pipelineLayout = ctx->prefilteredEnvMapPipeline.pipelineLayout,
        .descriptorSet = ctx->descriptorSetTex,
        .width = 512,
        .height = 512,
        .sampler = ctx->cubemapSampler,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .layerCount = 6,
        .pushConstantData = data,
        .usesMips = true
    };
}

static inline texture_desc_t MakeTextureDesc(vulkan_context_t *ctx, u32 layerCount)
{
    return (texture_desc_t) {
        .cb = CommandBuffer(ctx),
        .descriptorSet = ctx->descriptorSetTex,
        .sampler = (layerCount == 1) ? ctx->texSampler : ctx->cubemapSampler,
        .layerCount = layerCount
    };
}

static inline u32 GetMipCount(i32 width, i32 height)
{
    return (u32)(floor(log2(MAX(width, height)))) + 1;
}

VkImageMemoryBarrier2 ImageBarrier(VkImage image, VkPipelineStageFlags2 srcStageMask, VkAccessFlags2 srcAccessMask, VkImageLayout oldLayout, VkPipelineStageFlags2 dstStageMask, VkAccessFlags2 dstAccessMask, VkImageLayout newLayout, VkImageAspectFlags aspectMask, u32 baseMipLevel, u32 levelCount)
{
    return (VkImageMemoryBarrier2) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStageMask,
        .srcAccessMask = srcAccessMask,
        .dstStageMask = dstStageMask,
        .dstAccessMask = dstAccessMask,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspectMask,
            .baseMipLevel = baseMipLevel,
            .levelCount = levelCount,
            .layerCount = VK_REMAINING_ARRAY_LAYERS
        },
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED
    };
}

VkBufferMemoryBarrier2 BufferBarrier(VkBuffer buffer, VkPipelineStageFlags2 srcStageMask, VkAccessFlags2 srcAccessMask, VkPipelineStageFlags2 dstStageMask, VkAccessFlags2 dstAccessMask)
{
    return (VkBufferMemoryBarrier2) {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = srcStageMask,
        .srcAccessMask = srcAccessMask,
        .dstStageMask = dstStageMask,
        .dstAccessMask = dstAccessMask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE
    };
}

void PipelineBarrier(VkCommandBuffer commandBuffer, VkDependencyFlags dependencyFlags, size_t bufferBarrierCount, const VkBufferMemoryBarrier2* bufferBarriers, size_t imageBarrierCount, const VkImageMemoryBarrier2* imageBarriers)
{
    VkDependencyInfo dependencyInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .dependencyFlags = dependencyFlags,
        .bufferMemoryBarrierCount = bufferBarrierCount,
        .pBufferMemoryBarriers = bufferBarriers,
        .imageMemoryBarrierCount = imageBarrierCount,
        .pImageMemoryBarriers = imageBarriers,
    };

    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
}

void StageBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 srcStageMask, VkAccessFlags2 srcAccessMask, VkPipelineStageFlags2 dstStageMask, VkAccessFlags2 dstAccessMask)
{
    VkMemoryBarrier2 memoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = srcStageMask,
        .srcAccessMask = srcAccessMask,
        .dstStageMask = dstStageMask,
        .dstAccessMask = dstAccessMask,
    };

    VkDependencyInfo dependencyInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memoryBarrier
    };

    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
}

void VulkanLoadShader(resource_t *shaderResource, const char *path, memory_arena_t *scratchArena, memory_arena_t *permanentArena)
{
    u8 *spirv = NULL;
    FILE *file = fopen(path, "rb");
    fseek(file, 0, SEEK_END);
    u32 fileSize = (u32)ftell(file);
    fseek(file, 0, SEEK_SET);

    LV_ASSERT((fileSize % sizeof(u32) == 0) && "SPIR-V file size must be a multiple of 4");

    ArrayInitWithArena(spirv, scratchArena, fileSize);
    ArrayResize(spirv, fileSize);

    LV_ASSERT((fread(spirv, 1, fileSize, file) == fileSize) && ("Failed to read entire shader file"));

    fclose(file);

    ParseShader(shaderResource, spirv, permanentArena);
}

void VulkanUnloadShader(resource_t *shaderResource)
{
    //do nothing
}

void VulkanLoadTexture(resource_t *textureResource, const char *path, u32 layerCount)
{
    texture_desc_t desc = MakeTextureDesc(gCtx, layerCount);
    if (!CreateTexture(gCtx, &textureResource->texture, desc, textureResource->path, CommandBuffer(gCtx), Fence(gCtx))) {
        LOGE("Unable to load texture %s", path);
    }
}

void VulkanUnloadTexture(resource_t *textureResource)
{
    // TODO: Is this fast after the first run?
    VK_CHECK(vkDeviceWaitIdle(gCtx->device));
    DestroyTexture(&textureResource->texture, gCtx->allocator, gCtx->device);
}

void VulkanShutdown(void)
{
    VK_CHECK(vkDeviceWaitIdle(gCtx->device));

    DestroyGraphicsPipeline(&gCtx->pipeline, gCtx->device);
    DestroyGraphicsPipeline(&gCtx->skyboxPipeline, gCtx->device);
    DestroyGraphicsPipeline(&gCtx->genBRDFLUTPipeline, gCtx->device);
    DestroyGraphicsPipeline(&gCtx->diffuseIrradianceMapPipeline, gCtx->device);
    DestroyGraphicsPipeline(&gCtx->prefilteredEnvMapPipeline, gCtx->device);
    DestroyComputePipeline(&gCtx->computePipeline, gCtx->device);
    DestroyComputePipeline(&gCtx->initialOceanSpectrumPipeline, gCtx->device);

    vkDestroyDescriptorSetLayout(gCtx->device, gCtx->texLayout, NULL);
    vkDestroyDescriptorSetLayout(gCtx->device, gCtx->dummyLayout, NULL);
    vkDestroyDescriptorSetLayout(gCtx->device, gCtx->storageImageLayout, NULL);
    
    vkDestroyDescriptorPool(gCtx->device, gCtx->descriptorPool, NULL);
    vkDestroyCommandPool(gCtx->device, gCtx->commandPool, NULL);

    DestroyBuffer(&gCtx->scratchBuffer, gCtx->allocator);
    DestroyBuffer(&gCtx->drawCommandCountBuffer, gCtx->allocator);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        DestroyBuffer(&gCtx->shaderGlobalsBuffers[i], gCtx->allocator);
        DestroyBuffer(&gCtx->drawCommandBuffers[i], gCtx->allocator);
    }

    DestroyBuffer(&gCtx->vertexBuffer, gCtx->allocator);
    DestroyBuffer(&gCtx->indexBuffer, gCtx->allocator);
    DestroyBuffer(&gCtx->drawBuffer, gCtx->allocator);
    DestroyBuffer(&gCtx->meshBuffer, gCtx->allocator);
    DestroyBuffer(&gCtx->materialBuffer, gCtx->allocator);
    DestroyBuffer(&gCtx->jonswapBuffer, gCtx->allocator);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyFence(gCtx->device, gCtx->fences[i], NULL);
        vkDestroySemaphore(gCtx->device, gCtx->presentSemaphores[i], NULL);
    }

    for (u32 i = 0; i < MAX_SWAPCHAIN_IMAGES; i++) {
        vkDestroyImageView(gCtx->device, gCtx->swapchainImages[i].view, NULL);
        vkDestroySemaphore(gCtx->device, gCtx->renderSemaphores[i], NULL);
    }

    vkDestroyImageView(gCtx->device, gCtx->depthImage.view, NULL);
    vmaDestroyImage(gCtx->allocator, gCtx->depthImage.image, gCtx->depthImage.allocation);

    if (gCtx->sampleCount != VK_SAMPLE_COUNT_1_BIT) {
        DestroyMSAAColorImage(gCtx);
    }

    vkDestroySwapchainKHR(gCtx->device, gCtx->swapchain, NULL);
    SDL_Vulkan_DestroySurface(gCtx->instance, gCtx->window.surface, NULL);
    SDL_DestroyWindow(gCtx->window.window);

    vkDestroySampler(gCtx->device, gCtx->texSampler, NULL);
    vkDestroySampler(gCtx->device, gCtx->cubemapSampler, NULL);
    vkDestroySampler(gCtx->device, gCtx->lutSampler, NULL);

    vmaDestroyAllocator(gCtx->allocator);

    vkDestroyDevice(gCtx->device, NULL);
    vkDestroyInstance(gCtx->instance, NULL);

    volkFinalize();
}

static void RenderBRDFLUT(vulkan_context_t *context, void *data)
{
    texture_desc_t *desc = (texture_desc_t *)data;
    vkCmdBindPipeline(desc->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, desc->pipeline);
    vkCmdDraw(desc->cb, 3, 1, 0, 0);
}

static void RenderCubemap(vulkan_context_t *context, void *data)
{
    texture_desc_t *desc  = (texture_desc_t *)data;
    skybox_data_t *skyboxData = (skybox_data_t *)desc->pushConstantData;
    VkCommandBuffer cb        = desc->cb;
    VkPipeline pipeline       = desc->pipeline;
    VkPipelineLayout layout   = desc->pipelineLayout;
    VkDescriptorSet descSet   = desc->descriptorSet;

    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(skybox_data_t), skyboxData);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descSet, 0, NULL);
    vkCmdBindIndexBuffer(cb, context->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, skyboxData->indexCount, 1, skyboxData->indexOffset, skyboxData->vertexOffset, 0);
}

static void GenerateCubemap(vulkan_context_t *ctx, texture_resource_t *output, void (*render)(vulkan_context_t *, void*), texture_desc_t desc)
{
    VkCommandBuffer cb = desc.cb;
    VkFormat format = desc.format;
    i32 width = desc.width;
    i32 height = desc.height;
    u32 mipCount = desc.usesMips ? GetMipCount(width, height) : 1;
    VmaAllocator allocator = ctx->allocator;
    VkDevice device = ctx->device;

    VmaAllocation outputAllocation;
    output->image = CreateImage(device, allocator, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        width, height, mipCount, 6, VK_SAMPLE_COUNT_1_BIT, &outputAllocation);
    output->view = CreateImageView(device, output->image, format, VK_IMAGE_ASPECT_COLOR_BIT, mipCount, 6);
    output->allocation = outputAllocation;
    output->format = format;

    // Create offscreen image
    VmaAllocation offscreenImageAllocation;
    VkImage offscreenImage = CreateImage(device, allocator, output->format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 
        width, height, 1, 1, VK_SAMPLE_COUNT_1_BIT, &offscreenImageAllocation);
    VkImageView offscreenView = CreateImageView(ctx->device, offscreenImage, output->format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);

    VK_CHECK(vkResetCommandBuffer(cb, 0));

    VkCommandBufferBeginInfo cbBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VK_CHECK(vkBeginCommandBuffer(cb, &cbBeginInfo));

    VkImageMemoryBarrier2 colorBarrier = ImageBarrier(offscreenImage,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
    );

    PipelineBarrier(cb, 0, 0, NULL, 1, &colorBarrier);

    colorBarrier = ImageBarrier(output->image,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount);

    PipelineBarrier(cb, 0, 0, NULL, 1, &colorBarrier);

    // render all 6 cube faces
    VkRenderingAttachmentInfo colorAttachmentInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = offscreenView,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = {1.0f, 1.0f, 0.0f, 1.0f}},
    };

    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea.extent = {
            .width = width,
            .height = height
        },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentInfo,
        .pDepthAttachment = VK_NULL_HANDLE
    };

    VkViewport viewport = {
        .width = (float)width,
        .height = (float)height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };

    VkRect2D scissor = {
        .extent = {
            .width = width,
            .height = height
        }
    };
    // Face order matches your glm list (0..5)
    HMM_Mat4 faceMatrices[6] = {
        // 1: rotate(Y, +90) then rotate(X, +180)
        { .Columns = {
            HMM_V4( 0.0f,  0.0f, -1.0f, 0.0f),
            HMM_V4( 0.0f, -1.0f,  0.0f, 0.0f),
            HMM_V4(-1.0f,  0.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f,  0.0f, 1.0f)
        }},
        // 2: rotate(Y, -90) then rotate(X, +180)
        { .Columns = {
            HMM_V4( 0.0f,  0.0f,  1.0f, 0.0f),
            HMM_V4( 0.0f, -1.0f,  0.0f, 0.0f),
            HMM_V4( 1.0f,  0.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f,  0.0f, 1.0f)
        }},
        // 3: rotate(X, -90)
        { .Columns = {
            HMM_V4( 1.0f,  0.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f, -1.0f, 0.0f),
            HMM_V4( 0.0f,  1.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f,  0.0f, 1.0f)
        }},
        // 4: rotate(X, +90)
        { .Columns = {
            HMM_V4( 1.0f,  0.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f,  1.0f, 0.0f),
            HMM_V4( 0.0f, -1.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f,  0.0f, 1.0f)
        }},
        // 5: rotate(X, 180)
        { .Columns = {
            HMM_V4( 1.0f,  0.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f, -1.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f, -1.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f,  0.0f, 1.0f)
        }},
        // 6: rotate(Z, 180)
        { .Columns = {
            HMM_V4(-1.0f,  0.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f, -1.0f,  0.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f,  1.0f, 0.0f),
            HMM_V4( 0.0f,  0.0f,  0.0f, 1.0f)
        }}
    };

    skybox_data_t *skyboxData = (skybox_data_t *)desc.pushConstantData;

    // Render all cubemap faces
    for (u32 level = 0; level < mipCount; level++) {
        for (u32 face = 0; face < 6; face++) {

            viewport.width  = (float)(width * powf(0.5f, level));
            viewport.height = (float)(height * powf(0.5f, level));

            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);

            //draw skybox
            vkCmdBeginRendering(cb, &renderingInfo);
            HMM_Mat4 view = {0};
            for (u32 i = 0; i < 4; i++) {
                view.Elements[i][i] = 1.0f;
            }

            skyboxData->model = faceMatrices[face];
            // near must stay well inside the cube: its faces are at distance 1.0, so a
            // near of 1.0 puts the captured face exactly on the near plane and clips it
            skyboxData->projection = HMM_Perspective_RH_ZO(HMM_PI / 2.0f, 1.0f, 0.1f, 512.0f);
            skyboxData->view = view;
            skyboxData->roughness = (float)level / (float)(mipCount - 1);
    
            RenderCubemap(ctx, &desc);

            vkCmdEndRendering(cb);

            //barrier from attachment optimal to transfer src optimal
            VkImageMemoryBarrier2 offscreenImageBarrier = ImageBarrier(offscreenImage,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
            );

            PipelineBarrier(cb, 0, 0, NULL, 1, &offscreenImageBarrier);

            // copy from offscreen image to cubemap image
            VkImageCopy copyRegion = {
                .srcSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseArrayLayer = 0,
                    .mipLevel = 0,
                    .layerCount = 1
                },
                .srcOffset = { 0, 0, 0},
                .dstSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseArrayLayer = face,
                    .mipLevel = level,
                    .layerCount = 1,
                },
                .dstOffset = { 0, 0, 0},
                .extent = {
                    .width  = (u32)viewport.width,
                    .height = (u32)viewport.height,
                    .depth  = 1
                }
            };

            vkCmdCopyImage(cb, offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

            offscreenImageBarrier = ImageBarrier(offscreenImage,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);

            PipelineBarrier(cb, 0, 0, NULL, 1, &offscreenImageBarrier);
        }
    }

    VkImageMemoryBarrier2 outputBarrier = ImageBarrier(
        output->image,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        mipCount);

    PipelineBarrier(cb, 0, 0, NULL, 1, &outputBarrier);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo submitInfo2 = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb
    };

    VkFence fence   = Fence(ctx);
    VkQueue queue   = ctx->queue;

    VK_CHECK(vkResetFences(device, 1, &fence));
    VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo2, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyImageView(device, offscreenView, NULL);
    vmaDestroyImage(allocator, offscreenImage, offscreenImageAllocation);

    // Update descriptor set
    VkDescriptorImageInfo imageInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        .imageView   = output->view,
        .sampler     = desc.sampler
    };

    VkWriteDescriptorSet writeDescSet = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = desc.descriptorSet,
        .dstBinding = 0,
        .dstArrayElement = output->textureIndex,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &imageInfo,
    };

    vkUpdateDescriptorSets(device, 1, &writeDescSet, 0, NULL);
}

static void GenerateTexture(vulkan_context_t *ctx, texture_resource_t *output, void (*render)(vulkan_context_t *, void *),  texture_desc_t desc)
{
    VkCommandBuffer cb = desc.cb;
    VkFormat format = desc.format;
    i32 width = desc.width;
    i32 height = desc.height;
    u32 mipCount = desc.usesMips ? GetMipCount(width, height) : 1;
    u32 layerCount = desc.layerCount;

    VmaAllocation allocation;
    output->image = CreateImage(ctx->device, ctx->allocator, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 
        width, height, mipCount, layerCount, VK_SAMPLE_COUNT_1_BIT, &allocation);
    output->view  = CreateImageView(ctx->device, output->image, format, VK_IMAGE_ASPECT_COLOR_BIT, mipCount, layerCount);
    output->allocation = allocation;
    output->format = format;

    VK_CHECK(vkResetCommandBuffer(cb, 0));
    
    VkCommandBufferBeginInfo cbBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VK_CHECK(vkBeginCommandBuffer(cb, &cbBeginInfo));

    // Layout transition for output image
    VkImageMemoryBarrier2 colorBarrier = ImageBarrier(output->image, 
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount
    );

    PipelineBarrier(cb, 0, 0, NULL, 1, &colorBarrier);

    VkRenderingAttachmentInfo colorAttachmentInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = output->view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {1.0f, 1.0f, 0.0f, 1.0f}},
    };

    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea.extent = {
            .width  = width,
            .height = height
        },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentInfo,
        .pDepthAttachment = VK_NULL_HANDLE
    };

    vkCmdBeginRendering(cb, &renderingInfo);

    VkViewport vp = {
        .x        = 0,
        .y        = 0,
        .width    = (f32)width,
        .height   = (f32)height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };

    VkRect2D scissor = {
        .offset = { 0, 0},
        .extent = { width, height }
    };

    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    render(ctx, &desc);

    vkCmdEndRendering(cb);

    // Transition image layout to shader read only optimal
    VkImageMemoryBarrier2 readBarrier = ImageBarrier(output->image, 
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount
    );

    PipelineBarrier(cb, 0, 0, NULL, 1, &readBarrier);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb
    };
    
    VK_CHECK(vkResetFences(ctx->device, 1, &ctx->fences[ctx->frameIndex]));
    VK_CHECK(vkQueueSubmit(ctx->queue, 1, &submitInfo, ctx->fences[ctx->frameIndex]));
    VK_CHECK(vkWaitForFences(ctx->device, 1, &ctx->fences[ctx->frameIndex], VK_TRUE, UINT64_MAX));

    // Update descriptor set
    VkDescriptorImageInfo brdflutInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        .imageView   = output->view,
        .sampler     = desc.sampler
    };

    VkWriteDescriptorSet writeDescSet = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ctx->descriptorSetTex,
        .dstBinding = 0,
        .dstArrayElement = output->textureIndex, 
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &brdflutInfo
    };

    vkUpdateDescriptorSets(ctx->device, 1, &writeDescSet, 0, NULL);
}

void VulkanGenerateTexture(resource_t *textureResource)
{
    if (textureResource->path == gCtx->brdfLutPath) {
        texture_desc_t desc = MakeBRDFLUTDescription(gCtx, NULL);
        GenerateTexture(gCtx, &textureResource->texture, RenderBRDFLUT, desc);
    } else if (textureResource->path == gCtx->diffuseIrradianceMapPath) {
        texture_desc_t desc = MakeDiffuseIrradianceDesc(gCtx, &gCtx->skyboxData[gCtx->frameIndex]);
        GenerateCubemap(gCtx, &textureResource->texture, RenderCubemap, desc);
    } else if (textureResource->path == gCtx->prefilteredEnvMapPath) {
        texture_desc_t desc = MakePrefilteredEnvDesc(gCtx, &gCtx->skyboxData[gCtx->frameIndex]);
        GenerateCubemap(gCtx, &textureResource->texture, RenderCubemap, desc);
    } else {
        LOGE("Unknown generated texture path %s", textureResource->path);
        LV_ASSERT(false);
    }
}

// Only create the image and write descriptors. Do not render image yet
static void GenerateInitialOceanSpectrum(vulkan_context_t *ctx, texture_resource_t *textureResource)
{
    VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    i32 width  = 1024;
    i32 height = 1024;

    VmaAllocation alloc;
    VkImage image = CreateImage(ctx->device, ctx->allocator, format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        width, height, 1, 1, VK_SAMPLE_COUNT_1_BIT, &alloc);
    VkImageView view = CreateImageView(ctx->device, image, format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);

    textureResource->image = image;
    textureResource->view = view;
    textureResource->allocation = alloc;

    //transition layout
    VkCommandBuffer cb = CommandBuffer(ctx);
    VkCommandBufferBeginInfo cbOneTimeBI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VK_CHECK(vkBeginCommandBuffer(cb, &cbOneTimeBI));
    
    VkImageMemoryBarrier2 barrier = ImageBarrier(image,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 
            0, 
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL, 
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
    );

    PipelineBarrier(cb, 0, 0, NULL, 1, &barrier);
    vkEndCommandBuffer(cb);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb
    };
    
    VkFence fence = Fence(ctx);
    VK_CHECK(vkResetFences(ctx->device, 1, &fence));
    VK_CHECK(vkQueueSubmit(ctx->queue, 1, &submitInfo, fence));
    VK_CHECK(vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX));

    VkDescriptorImageInfo storageInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageView = view,
        .sampler = VK_NULL_HANDLE
    };

    VkDescriptorImageInfo sampledInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageView = view,
        .sampler = ctx->texSampler
    };

    VkWriteDescriptorSet writes[2] = {
        { 
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 
            .dstSet = ctx->descriptorSetTex, 
            .dstBinding = 0, 
            .dstArrayElement = textureResource->textureIndex,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &sampledInfo
        },
        { 
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 
            .dstSet = ctx->descriptorSetStorageImage, 
            .dstBinding = 0, 
            .dstArrayElement = textureResource->storageIndex,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &storageInfo
        }
    };

    vkUpdateDescriptorSets(ctx->device, 2, writes, 0, NULL);
}

void VulkanGenerateStorageImage(resource_t *textureResource)
{
    if (textureResource->path == gCtx->initialOceanSpectrumPath0 ||
        textureResource->path == gCtx->initialOceanSpectrumPath1 ||
        textureResource->path == gCtx->initialOceanSpectrumPath2 || 
        textureResource->path == gCtx->initialOceanSpectrumPath3) {
        GenerateInitialOceanSpectrum(gCtx, &textureResource->texture);
    } else {
        LOGE("Unknown generated texture path %s", textureResource->path);
        LV_ASSERT(false);
    }
}

static texture_resource_t *LoadGeneratedTexture(vulkan_context_t *ctx, const char *uri)
{
    texture_resource_t *result = NULL;

    resource_t *res = ResourceSystemLoadResource(uri);
    if (res) {
        result = &res->texture;
    }
    return result;
}

static void VulkanLoadResources(vulkan_context_t *ctx)
{
    ctx->resourcesLoaded = true;
    //draws
    SDL_srand(188);

    geometry_t geometry = ResourceSystemGetGeometry();

    VkDeviceSize vBufSize = sizeof(vertex_t) * ArrayCount(geometry.vertices);
    CreateBuffer(&ctx->vertexBuffer, ctx->device, vBufSize, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);
    UploadBuffer(&ctx->vertexBuffer, geometry.vertices, vBufSize, 0);

    VkDeviceSize iBufSize = sizeof(u32) * ArrayCount(geometry.indices);
    CreateBuffer(&ctx->indexBuffer, ctx->device, iBufSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);
    UploadBuffer(&ctx->indexBuffer, geometry.indices, iBufSize, 0);

    ctx->indexCount = ArrayCount(geometry.indices);

    VkDeviceSize meshBufSize = sizeof(mesh_t) * ArrayCount(geometry.meshes);
    CreateBuffer(&ctx->meshBuffer, ctx->device, meshBufSize, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);
    UploadBuffer(&ctx->meshBuffer, geometry.meshes, meshBufSize, 0);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(&ctx->shaderGlobalsBuffers[i], ctx->device, sizeof(globals_t), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);
    }

    gCtx->drawCount = ArrayCount(geometry.draws);

    CreateBuffer(&ctx->drawBuffer, ctx->device, sizeof(draw_data_t) * gCtx->drawCount, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);
    UploadBuffer(&ctx->drawBuffer, geometry.draws, sizeof(draw_data_t) * gCtx->drawCount, 0);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(&ctx->drawCommandBuffers[i], ctx->device, gCtx->drawCount * sizeof(draw_command_t), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 0, ctx->allocator);
    }

    CreateBuffer(&ctx->drawCommandCountBuffer, ctx->device, sizeof(u32),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);

    u32 materialCount = ArrayCount(geometry.materials);
    CreateBuffer(&ctx->materialBuffer, ctx->device, sizeof(material_t) * ArrayCount(geometry.materials), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);
    UploadBuffer(&ctx->materialBuffer, geometry.materials, sizeof(material_t) * materialCount, 0);

    CreateBuffer(&ctx->jonswapBuffer, ctx->device, sizeof(jonswap_data_t) * 8, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);

    // 2 for each cascade
    jonswap_data_t jonswapParameters[8];
    FillSpectrumParameters(&jonswapParameters[0], 0.1, 2, 22, 100000, 0.642, 1, 1, 0.025);
    FillSpectrumParameters(&jonswapParameters[1], 0.07, 2, 59, 1000, 0, 1, 1, 0.01);
    FillSpectrumParameters(&jonswapParameters[2], 0.25, 20, 97, 100000000, 0.14, 1, 1, 0.5);
    FillSpectrumParameters(&jonswapParameters[3], 0.25, 20, 67, 1000000, 0.47, 1, 1, 0.5);
    FillSpectrumParameters(&jonswapParameters[4], 0.15, 5, 105, 1000000, 0.2, 1, 1, 0.5);
    FillSpectrumParameters(&jonswapParameters[5], 0.1, 1, 19, 10000, 0.298, 0.695, 1, 0.5);
    FillSpectrumParameters(&jonswapParameters[6], 1, 1, 209, 200000, 0.56, 1, 1, 0.0001);
    FillSpectrumParameters(&jonswapParameters[7], 0.23, 1, 0, 1000, 0, 0, 1, 0.0001);
    
    UploadBuffer(&ctx->jonswapBuffer, jonswapParameters, sizeof(jonswapParameters), 0);
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        ctx->spectrumData[i].jonswapData = ctx->jonswapBuffer.deviceAddress;
        ctx->spectrumData[i].seed = 1234;
        ctx->spectrumData[i].lowCutoff = 0.0001f;
        ctx->spectrumData[i].highCutoff = 9000.0f;
        ctx->spectrumData[i].depth = 20.0f;
        ctx->spectrumData[i].lengthScale[0] = 94;
        ctx->spectrumData[i].lengthScale[1] = 128;
        ctx->spectrumData[i].lengthScale[2] = 64;
        ctx->spectrumData[i].lengthScale[3] = 32;
    }

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        ctx->pbrData[i].globalsAddress = ctx->shaderGlobalsBuffers[i].deviceAddress;
        ctx->pbrData[i].drawDataAddress = ctx->drawBuffer.deviceAddress;
        ctx->pbrData[i].meshAddress = ctx->meshBuffer.deviceAddress;
        ctx->pbrData[i].drawCommandAddress = ctx->drawCommandBuffers[i].deviceAddress;
        ctx->pbrData[i].drawCommandCountAddress = ctx->drawCommandCountBuffer.deviceAddress;
        ctx->pbrData[i].materialAddress = ctx->materialBuffer.deviceAddress;
        ctx->pbrData[i].vertexAddress   = ctx->vertexBuffer.deviceAddress;
    }

    // setup skybox push constant data vertex offset
    resource_t *skyboxResource = ResourceSystemGetResource("Cube.gltf");
    if (skyboxResource) {
        u32 meshIndex = skyboxResource->mesh.firstMeshIndex;
        u32 meshCount = skyboxResource->mesh.meshCount;
        LV_ASSERT(meshIndex < ArrayCount(geometry.meshes));
        LV_ASSERT(meshCount == 1);
        u32 vertexOffset = geometry.meshes[meshIndex].vertexOffset;
        u32 indexOffset  = geometry.meshes[meshIndex].meshLods[0].indexOffset;
        u32 indexCount   = geometry.meshes[meshIndex].meshLods[0].indexCount;

        uint32_t skyboxTextureIndex = 0;
        resource_t *skyboxTexture = ResourceSystemGetResource("cubemap_cobblestone_parish_road.dds");
        if (skyboxTexture) {
            skyboxTextureIndex = skyboxTexture->texture.textureIndex;
        }

        LV_ASSERT(indexCount == 36);
        
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            ctx->skyboxData[i].vertexOffset  = vertexOffset;
            ctx->skyboxData[i].indexOffset   = indexOffset;
            ctx->skyboxData[i].indexCount    = indexCount;
            ctx->skyboxData[i].vertexAddress = ctx->vertexBuffer.deviceAddress;
            ctx->skyboxData[i].textureIndex  = skyboxTextureIndex;
        }
    }

    ctx->texSampler     = CreateTextureSampler(ctx->device, 16, 16.0f);
    ctx->cubemapSampler = CreateCubemapSampler(ctx->device, 16, 1.0f);
    ctx->lutSampler     = CreateLUTSampler(ctx->device, 1, 1.0f);

    // brdf lut, diffuse irradiance, prefiltered env, plus the four ocean spectrum images.
    // Storage images take a slot in the sampled array as well, since the water shader reads them.
    const u32 genTextureCount = 3 + 4;
    const resource_t **textureResources = ResourceSystemGetTextures(ScratchArena());
    u32 texCount = ArrayCount(textureResources) + genTextureCount;
    const u32 storageImageCount = 128; // TODO: Completely arbitrary

    ctx->texLayout          = CreateDescriptorSetLayout(ctx->device, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, texCount, true);
    ctx->dummyLayout        = CreateDescriptorSetLayout(ctx->device, 0, 0, 0, false);
    ctx->storageImageLayout = CreateDescriptorSetLayout(ctx->device, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, storageImageCount, true);
    
    VkDescriptorPoolSize poolSizes[2] = { 
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount = texCount }, 
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = storageImageCount }
    };

    VkDescriptorPoolCreateInfo descPoolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = ARRAY_SIZE(poolSizes),
        .poolSizeCount = ARRAY_SIZE(poolSizes),
        .pPoolSizes = poolSizes
    };

    VK_CHECK(vkCreateDescriptorPool(ctx->device, &descPoolCI, NULL, &ctx->descriptorPool));

    // Allocate descriptor sets
    u32 descriptorSetCounts[2] = { texCount, storageImageCount };
    VkDescriptorSetLayout setLayouts[2] = { ctx->texLayout, ctx->storageImageLayout };

    VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT,
        .descriptorSetCount = 2,
        .pDescriptorCounts = descriptorSetCounts,
    };

    VkDescriptorSetAllocateInfo texDescSetAlloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variableDescCountAI,
        .descriptorPool = ctx->descriptorPool,
        .descriptorSetCount = 2,
        .pSetLayouts = setLayouts,
    };

    VkDescriptorSet sets[2];
    VK_CHECK(vkAllocateDescriptorSets(ctx->device, &texDescSetAlloc, sets));

    ctx->descriptorSetTex = sets[0];
    ctx->descriptorSetStorageImage = sets[1];

    // write texture and cubemap descriptor set
    VkDescriptorImageInfo *textureDescriptors = NULL;
    // Allocating extra memory for BRDF Lut, irradiance cube map and pre-filtered environment cube map
    ArrayInitWithArena(textureDescriptors, ScratchArena(), texCount);

    for (u32 i = 0; i < texCount - genTextureCount; i++) {
        VkDescriptorImageInfo info = {0};
        info.imageLayout = textureResources[i]->texture.layout;
        info.imageView   = textureResources[i]->texture.view;
        // Use cubemap sampler for cubemaps, regular sampler for 2D textures
        info.sampler     = textureResources[i]->texture.layerCount == 6 ? ctx->cubemapSampler : ctx->texSampler;
        ArrayPush(textureDescriptors, info);
    }

    VkWriteDescriptorSet writeDescSet = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ctx->descriptorSetTex,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = ArrayCount(textureDescriptors), // we are going to write to last three later
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = textureDescriptors,
    };

    vkUpdateDescriptorSets(ctx->device, 1, &writeDescSet, 0, NULL);

    VkFormat imageFormat = ctx->swapchainImages[0].format;
    VkFormat depthFormat = ctx->depthImage.format;
    // Main scene pipelines render into the multisampled targets, the offscreen bake
    // pipelines render into single sampled images
    CreateGraphicsPipeline(&ctx->pipeline, "shader.spv", ctx->device, imageFormat, depthFormat, sizeof(pbr_data_t), setLayouts, 2, VK_TRUE, VK_CULL_MODE_BACK_BIT, ctx->sampleCount);
    CreateGraphicsPipeline(&ctx->skyboxPipeline, "skybox.spv", ctx->device, imageFormat, depthFormat, sizeof(skybox_data_t), setLayouts, 2, VK_FALSE, VK_CULL_MODE_NONE, ctx->sampleCount);
    CreateGraphicsPipeline(&ctx->genBRDFLUTPipeline, "genbrdflut.spv", ctx->device, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED, 0, setLayouts, 2, VK_FALSE, VK_CULL_MODE_NONE, VK_SAMPLE_COUNT_1_BIT);
    CreateGraphicsPipeline(&ctx->diffuseIrradianceMapPipeline, "diffuse_irradiance.spv", ctx->device, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED, sizeof(skybox_data_t), setLayouts, 2, VK_FALSE, VK_CULL_MODE_NONE, VK_SAMPLE_COUNT_1_BIT);
    CreateGraphicsPipeline(&ctx->prefilteredEnvMapPipeline, "prefiltered_env_map.spv", ctx->device, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED, sizeof(skybox_data_t), setLayouts, 2, VK_FALSE, VK_CULL_MODE_NONE, VK_SAMPLE_COUNT_1_BIT);
    
    CreateComputePipeline(&ctx->computePipeline, "compute_shader.spv", ctx->device, NULL, 0, sizeof(pbr_data_t));
    CreateComputePipeline(&ctx->initialOceanSpectrumPipeline, "initial_spectrum.spv", ctx->device, &ctx->storageImageLayout, 1, sizeof(initial_ocean_spectrum_data_t));

    ctx->brdfLutPath = ResourceSystemMakeFullPath("brdfLut.img", RESOURCE_TYPE_IMAGE);
    ctx->diffuseIrradianceMapPath = ResourceSystemMakeFullPath("diffuseIrradianceMap.img", RESOURCE_TYPE_IMAGE);
    ctx->prefilteredEnvMapPath = ResourceSystemMakeFullPath("prefilteredEnvMap.img", RESOURCE_TYPE_IMAGE);
    ctx->initialOceanSpectrumPath0 = ResourceSystemMakeFullPath("initialOceanSpectrum0.simg", RESOURCE_TYPE_STORAGE_IMAGE);
    ctx->initialOceanSpectrumPath1 = ResourceSystemMakeFullPath("initialOceanSpectrum1.simg", RESOURCE_TYPE_STORAGE_IMAGE);
    ctx->initialOceanSpectrumPath2 = ResourceSystemMakeFullPath("initialOceanSpectrum2.simg", RESOURCE_TYPE_STORAGE_IMAGE);
    ctx->initialOceanSpectrumPath3 = ResourceSystemMakeFullPath("initialOceanSpectrum3.simg", RESOURCE_TYPE_STORAGE_IMAGE);

    ctx->brdfLut = LoadGeneratedTexture(ctx, "brdfLut.img");
    ctx->diffuseIrradianceMap = LoadGeneratedTexture(ctx, "diffuseIrradianceMap.img");
    ctx->prefilteredEnvMap = LoadGeneratedTexture(ctx, "prefilteredEnvMap.img");

    // Storage images need to be created first
    ctx->initialOceanSpectrum0 = LoadGeneratedTexture(ctx, "initialOceanSpectrum0.simg");
    ctx->initialOceanSpectrum1 = LoadGeneratedTexture(ctx, "initialOceanSpectrum1.simg");
    ctx->initialOceanSpectrum2 = LoadGeneratedTexture(ctx, "initialOceanSpectrum2.simg");
    ctx->initialOceanSpectrum3 = LoadGeneratedTexture(ctx, "initialOceanSpectrum3.simg");

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        ctx->spectrumData[i].textureIndices[0] = ctx->initialOceanSpectrum0->storageIndex;
        ctx->spectrumData[i].textureIndices[1] = ctx->initialOceanSpectrum1->storageIndex;
        ctx->spectrumData[i].textureIndices[2] = ctx->initialOceanSpectrum2->storageIndex;
        ctx->spectrumData[i].textureIndices[3] = ctx->initialOceanSpectrum3->storageIndex;
    }

    VkCommandBuffer cb = CommandBuffer(ctx);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo cbBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VK_CHECK(vkBeginCommandBuffer(cb, &cbBeginInfo));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gCtx->initialOceanSpectrumPipeline.pipeline);
    vkCmdPushConstants(cb, ctx->initialOceanSpectrumPipeline.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ctx->spectrumData[0]), &ctx->spectrumData[ctx->frameIndex]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->initialOceanSpectrumPipeline.pipelineLayout, 0, 1, &ctx->descriptorSetStorageImage, 0, NULL);
    // initial_spectrum.slang is [numthreads(8,8,1)] over the full 1024x1024 texture
    u32 numWorkgroups = 1024 / 8;
    vkCmdDispatch(cb, numWorkgroups, numWorkgroups, 1);

    StageBarrier(cb,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT);
    
    vkEndCommandBuffer(cb);
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb
    };
    
    VkFence fence = Fence(ctx);
    VK_CHECK(vkResetFences(ctx->device, 1, &fence));
    VK_CHECK(vkQueueSubmit(ctx->queue, 1, &submitInfo, fence));
    VK_CHECK(vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX));
}

void VulkanRender(void)
{
    if (!gCtx->resourcesLoaded) {
        VulkanLoadResources(gCtx);
    }

    VK_CHECK(vkWaitForFences(gCtx->device, 1, &gCtx->fences[gCtx->frameIndex], true, UINT64_MAX));
    VK_CHECK(vkResetFences(gCtx->device, 1, &gCtx->fences[gCtx->frameIndex]));

    u32 imageIndex;
    VK_CHECK(vkAcquireNextImageKHR(gCtx->device, gCtx->swapchain, UINT64_MAX, gCtx->presentSemaphores[gCtx->frameIndex], VK_NULL_HANDLE, &imageIndex));

    //prepare shader constants
    globals_t globals = {0};
    globals.projection = RendererGetProjectionMatrix();
    globals.view = RendererGetViewMatrix();

    mat4_t projectionT = HMM_TransposeM4(globals.projection);
    vec4_t frustumX = HMM_Norm(HMM_Add(projectionT.Columns[3], projectionT.Columns[0]));
    vec4_t frustumY = HMM_Norm(HMM_Add(projectionT.Columns[3], projectionT.Columns[1]));

    globals.P00 = globals.projection.Elements[0][0];
    globals.P11 = globals.projection.Elements[1][1];
    globals.near = RendererGetCameraNear();
    globals.far  = RendererGetCameraFar();
    globals.frustum[0] = frustumX.X;
    globals.frustum[1] = frustumX.Z;
    globals.frustum[2] = frustumY.Y;
    globals.frustum[3] = frustumY.Z;
    globals.drawCount = gCtx->drawCount;
    globals.lodTarget = (2 / globals.P11) * (1.0f / (f32)gCtx->window.h);
    globals.camPos = RendererGetCameraPosition();
    globals.lightDir = (vec3_t){1.0f, 1.0f, 1.0f};
    globals.lightDir = HMM_Norm(globals.lightDir);
    globals.brdflutIndex = gCtx->brdfLut->textureIndex;
    globals.diffuseIrradianceIndex = gCtx->diffuseIrradianceMap->textureIndex;
    globals.prefilteredEnvIndex = gCtx->prefilteredEnvMap->textureIndex;

    UploadBuffer(&gCtx->shaderGlobalsBuffers[gCtx->frameIndex], &globals, sizeof(globals_t), 0);

    mat4_t skyboxView = globals.view;
    skyboxView.Elements[3][0] = 0.0f;
    skyboxView.Elements[3][1] = 0.0f;
    skyboxView.Elements[3][2] = 0.0f;

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        gCtx->skyboxData[i].projection = globals.projection;
        gCtx->skyboxData[i].model      = skyboxView;
    }

    VkCommandBuffer cb = gCtx->commandBuffers[gCtx->frameIndex];

    VK_CHECK(vkResetCommandBuffer(cb, 0));

    VkCommandBufferBeginInfo cbBI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VK_CHECK(vkBeginCommandBuffer(cb, &cbBI));

    StageBarrier(cb, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);

    vkCmdFillBuffer(cb, gCtx->drawCommandCountBuffer.buffer, 0, sizeof(u32), 0);

    StageBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);

    //color and depth image need barriers for layout transitions
    VkImageMemoryBarrier2 colorBarrier = ImageBarrier(gCtx->swapchainImages[imageIndex].image,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, 1);

    VkImageMemoryBarrier2 depthBarrier = ImageBarrier(gCtx->depthImage.image,
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
        0, 1);

    b8 msaaEnabled = (gCtx->sampleCount != VK_SAMPLE_COUNT_1_BIT);

    VkImageMemoryBarrier2 outputBarriers[3] = {colorBarrier, depthBarrier};
    u32 outputBarrierCount = 2;

    if (msaaEnabled) {
        // The multisampled target is the actual render target, the swapchain image above
        // is only the resolve destination
        outputBarriers[outputBarrierCount++] = ImageBarrier(gCtx->msaaColorImage.image,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT,
            0, 1);
    }

    PipelineBarrier(cb, 0, 0, NULL, outputBarrierCount, outputBarriers);

    // With MSAA the samples are averaged into the swapchain image by the resolve, so the
    // multisampled contents themselves are thrown away
    VkRenderingAttachmentInfo colorAttachmentInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = msaaEnabled ? gCtx->msaaColorImage.view : gCtx->swapchainImages[imageIndex].view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .resolveMode = msaaEnabled ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
        .resolveImageView = msaaEnabled ? gCtx->swapchainImages[imageIndex].view : VK_NULL_HANDLE,
        .resolveImageLayout = msaaEnabled ? VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = msaaEnabled ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {1.0f, 1.0f, 0.0f, 1.0f}},
    };

    VkRenderingAttachmentInfo depthAttachmentInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = gCtx->depthImage.view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {.depthStencil = {1.0f, 0.0f}},
    };

    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea.extent.width = gCtx->window.w,
        .renderArea.extent.height = gCtx->window.h,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentInfo,
        .pDepthAttachment = &depthAttachmentInfo,
    };

    // Dispatch compute shader here
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gCtx->computePipeline.pipeline);
    vkCmdPushConstants(cb, gCtx->computePipeline.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pbr_data_t), &gCtx->pbrData[gCtx->frameIndex]);
    u32 numWorkgroups = (gCtx->drawCount + 63) / 64;
    vkCmdDispatch(cb, numWorkgroups, 1, 1);

    // Synchronize compute shader write with draw indirect read
    StageBarrier(cb, 
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);

    vkCmdBeginRendering(cb, &renderingInfo);

    VkViewport vp = {
        .width = (f32)gCtx->window.w,
        .height = (f32)gCtx->window.h,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor = {
        .extent.width =  gCtx->window.w,
        .extent.height = gCtx->window.h,
    };

    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gCtx->skyboxPipeline.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gCtx->skyboxPipeline.pipelineLayout, 0, 1, &gCtx->descriptorSetTex, 0, NULL);
    vkCmdBindIndexBuffer(cb, gCtx->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cb, gCtx->skyboxPipeline.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(skybox_data_t), &gCtx->skyboxData[gCtx->frameIndex]);
    vkCmdDrawIndexed(cb, gCtx->skyboxData[gCtx->frameIndex].indexCount, 1, gCtx->skyboxData[gCtx->frameIndex].indexOffset, gCtx->skyboxData[gCtx->frameIndex].vertexOffset, 0);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gCtx->pipeline.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gCtx->pipeline.pipelineLayout, 0, 1, &gCtx->descriptorSetTex, 0, NULL);
    vkCmdBindIndexBuffer(cb, gCtx->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cb, gCtx->pipeline.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pbr_data_t), &gCtx->pbrData[gCtx->frameIndex]);

    vkCmdDrawIndexedIndirectCount(cb, gCtx->drawCommandBuffers[gCtx->frameIndex].buffer, offsetof(draw_command_t, command), gCtx->drawCommandCountBuffer.buffer, 0, gCtx->drawCount, sizeof(draw_command_t));
    vkCmdEndRendering(cb);

    VkImageMemoryBarrier2 barrierPresent = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .image = gCtx->swapchainImages[imageIndex].image,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };

    VkDependencyInfo barrierPresentDependencyInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrierPresent
    };

    vkCmdPipelineBarrier2(cb, &barrierPresentDependencyInfo);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &gCtx->presentSemaphores[gCtx->frameIndex],
        .pWaitDstStageMask = &waitStages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &gCtx->renderSemaphores[imageIndex],
    };

    VK_CHECK(vkQueueSubmit(gCtx->queue, 1, &submitInfo, gCtx->fences[gCtx->frameIndex]));

    gCtx->frameIndex = (gCtx->frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &gCtx->renderSemaphores[imageIndex],
        .swapchainCount = 1,
        .pSwapchains = &gCtx->swapchain,
        .pImageIndices = &imageIndex,
    };

    VK_CHECK(vkQueuePresentKHR(gCtx->queue, &presentInfo));

    if (gCtx->windowResized) {
        if (SDL_GetWindowSize(gCtx->window.window, &gCtx->window.w, &gCtx->window.h)) {
            LOGI("Window resized: %ux%u", gCtx->window.w, gCtx->window.h);
        }
        gCtx->windowResized = false;

        VK_CHECK(vkDeviceWaitIdle(gCtx->device));

        VkSurfaceCapabilitiesKHR surfaceCaps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gCtx->physicalDevice, gCtx->window.surface, &surfaceCaps));

        VkSwapchainCreateInfoKHR swapchainCI = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = gCtx->window.surface,
            .minImageCount = surfaceCaps.minImageCount,
            .imageFormat = gCtx->swapchainImages[0].format,
            .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
            .imageExtent.width = gCtx->window.w,
            .imageExtent.height = gCtx->window.h,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .oldSwapchain = gCtx->swapchain,
        };

        VK_CHECK(vkCreateSwapchainKHR(gCtx->device, &swapchainCI, NULL, &gCtx->swapchain));

        for (u32 i = 0; i < gCtx->swapchainImageCount; i++) {
            vkDestroyImageView(gCtx->device, gCtx->swapchainImages[i].view, NULL);
        }

        VkImage images[MAX_SWAPCHAIN_IMAGES];
        VK_CHECK(vkGetSwapchainImagesKHR(gCtx->device, gCtx->swapchain, &gCtx->swapchainImageCount, images));
        LV_ASSERT(gCtx->swapchainImageCount <= MAX_SWAPCHAIN_IMAGES);

        VkFormat format = gCtx->swapchainImages[0].format;
        for (u32 i = 0; i < gCtx->swapchainImageCount; i++) {
            gCtx->swapchainImages[i].image = images[i];
            gCtx->swapchainImages[i].view = CreateImageView(gCtx->device, images[i], format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        }

        vkDestroySwapchainKHR(gCtx->device, swapchainCI.oldSwapchain, NULL);
        vmaDestroyImage(gCtx->allocator, gCtx->depthImage.image, gCtx->depthImage.allocation);
        vkDestroyImageView(gCtx->device, gCtx->depthImage.view, NULL);

        gCtx->depthImage.image = CreateImage(gCtx->device, gCtx->allocator, gCtx->depthImage.format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, gCtx->window.w, gCtx->window.h, 1, 1, gCtx->sampleCount, &gCtx->depthImage.allocation);
        gCtx->depthImage.view = CreateImageView(gCtx->device, gCtx->depthImage.image, gCtx->depthImage.format, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1);

        if (gCtx->sampleCount != VK_SAMPLE_COUNT_1_BIT) {
            DestroyMSAAColorImage(gCtx);
            CreateMSAAColorImage(gCtx);
        }
    }
}

void VulkanHotReload(vulkan_context_t *ctx)
{
    gCtx = ctx;
    // Reinitialize volk
    if (volkInitialize() != VK_SUCCESS) {
        LOGF("Unable to initialize volk");
        return;
    }
    volkLoadInstance(ctx->instance);
    volkLoadDevice(ctx->device);
}

void VulkanInit(vulkan_context_t *ctx)
{
    VkResult volkResult = volkInitialize();
    if (volkResult != VK_SUCCESS) {
        LOGF("Failed to initialize Volk: %d", volkResult);
        return;
    }

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Levent",
        .apiVersion = VK_API_VERSION_1_3
    };

    const char *validationLayerName = "VK_LAYER_KHRONOS_validation";

    VkInstanceCreateInfo instanceCI = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = PlatformGetVulkanInstanceExtensionCount(),
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = &validationLayerName,
        .ppEnabledExtensionNames = PlatformGetVulkanInstanceExtensions(),
        .pApplicationInfo = &appInfo
    };

    VkInstance instance;
    VK_CHECK(vkCreateInstance(&instanceCI, NULL, &instance));

    ctx->instance = instance;

    volkLoadInstance(ctx->instance);

    u32 deviceCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, NULL));

    VkPhysicalDevice physicalDevices[8];
    VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, physicalDevices));
    ctx->physicalDevice = physicalDevices[0];

    VkPhysicalDeviceProperties2 deviceProperties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    };

    vkGetPhysicalDeviceProperties2(ctx->physicalDevice, &deviceProperties);

    // Color and depth attachments in one render pass must agree on the sample count,
    // so only take counts both support
    VkSampleCountFlags sampleCounts = deviceProperties.properties.limits.framebufferColorSampleCounts &
                                      deviceProperties.properties.limits.framebufferDepthSampleCounts;
    ctx->sampleCount = (sampleCounts & VK_SAMPLE_COUNT_8_BIT) ? VK_SAMPLE_COUNT_8_BIT : VK_SAMPLE_COUNT_1_BIT;
    LOGI("MSAA sample count: %u", (u32)ctx->sampleCount);

    u32 queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physicalDevice, &queueFamilyCount, NULL);

    VkQueueFamilyProperties queueFamilyProperties[8];
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physicalDevice, &queueFamilyCount, queueFamilyProperties);

    for  (u32 i = 0; i < queueFamilyCount; i++) {
        if (queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){
            printf("Selecting queue family %u\n", i);
            ctx->queueFamily = i;
            break;
        }
    }

    if (!PlatformGetVulkanPresentationSupport(ctx->instance, ctx->physicalDevice, ctx->queueFamily)) {
        LOGF("Selected queue family does not support presentation");
        return;
    }

    const f32 priorities = 1.0f;

    VkDeviceQueueCreateInfo queueCI = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priorities
    };

    const char *deviceExtensions[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkPhysicalDeviceVulkan11Features features11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .storageBuffer16BitAccess = true,
        .storagePushConstant16 = true,
        .shaderDrawParameters = true,
    };

    VkPhysicalDeviceVulkan12Features features12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &features11,
        .shaderFloat16 = true,
        .shaderInt8 = true,
        .descriptorIndexing = true,
        .shaderSampledImageArrayNonUniformIndexing = true,
        .descriptorBindingVariableDescriptorCount = true,
        .runtimeDescriptorArray = true,
        .bufferDeviceAddress = true,
        .drawIndirectCount = true,
        .scalarBlockLayout = true,
        .storageBuffer8BitAccess = true,
        .uniformAndStorageBuffer8BitAccess = true,
        .storagePushConstant8 = true,
        .samplerFilterMinmax = true,
        .descriptorBindingSampledImageUpdateAfterBind = true,
        .descriptorBindingPartiallyBound = true,
        .runtimeDescriptorArray = true,
    };

    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &features12,
        .synchronization2 = true,
        .dynamicRendering = true,
        .maintenance4 = true,
    };

    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .features.samplerAnisotropy = true,
        .features.multiDrawIndirect = true,
        .features.pipelineStatisticsQuery = true,
        .features.shaderInt16 = true,
        .features.shaderInt64 = true,
        .features.samplerAnisotropy = true,
        .pNext = &features13,
    };

    VkDeviceCreateInfo deviceCI = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCI,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = deviceExtensions,

    };

    VkDevice device;
    VK_CHECK(vkCreateDevice(ctx->physicalDevice, &deviceCI, NULL, &device));
    ctx->device = device;

    volkLoadDevice(ctx->device);

    VkQueue queue;
    vkGetDeviceQueue(ctx->device, ctx->queueFamily, 0, &queue);
    ctx->queue = queue;

    VmaVulkanFunctions vkFunctions = {0};
    VmaAllocatorCreateInfo allocatorCI = {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = ctx->physicalDevice,
        .device = ctx->device,
        .pVulkanFunctions = &vkFunctions,
        .instance = ctx->instance,
    };

    VK_CHECK(vmaImportVulkanFunctionsFromVolk(&allocatorCI, &vkFunctions));
    VK_CHECK(vmaCreateAllocator(&allocatorCI, &ctx->allocator));

    if (!PlatformCreateWindow(&ctx->window, ctx->instance, "HowToVulkan", 1280U, 720U, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE)) {
        LOGF("Unable to create window");
        return;
    }

    VkSurfaceCapabilitiesKHR surfaceCaps = {0};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice, ctx->window.surface, &surfaceCaps));

    VkExtent2D swapchainExtent = surfaceCaps.currentExtent;
    if (swapchainExtent.width == 0xFFFFFFFF) {
        swapchainExtent.width = ctx->window.w;
        swapchainExtent.height = ctx->window.h;
    }

    VkFormat imageFormat =  VK_FORMAT_B8G8R8A8_SRGB;
    VkSwapchainCreateInfoKHR swapchainCI = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = ctx->window.surface,
        .minImageCount = surfaceCaps.minImageCount,
        .imageFormat = imageFormat,
        .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
        .imageExtent.width = swapchainExtent.width,
        .imageExtent.height = swapchainExtent.height,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
    };

    VK_CHECK(vkCreateSwapchainKHR(ctx->device, &swapchainCI, NULL, &ctx->swapchain));

    VkImage swapchainImages[32];
    u32 swapchainImageCount;
    VK_CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &swapchainImageCount, NULL));
    LV_ASSERT(swapchainImageCount <= MAX_SWAPCHAIN_IMAGES);
    VK_CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &swapchainImageCount, swapchainImages));

    for (u32 i = 0; i < swapchainImageCount; i++) {
        ctx->swapchainImages[i].image = swapchainImages[i];
        ctx->swapchainImages[i].view = CreateImageView(ctx->device, ctx->swapchainImages[i].image, imageFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        ctx->swapchainImages[i].allocation = NULL;
        ctx->swapchainImages[i].format = imageFormat;
    }

    VkFormat depthFormatList[2] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };

    VkFormat depthFormat;
    for (u32 i = 0; i < ARRAY_SIZE(depthFormatList); i++) {
        VkFormatProperties2 formatProperties = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        };

        vkGetPhysicalDeviceFormatProperties2(ctx->physicalDevice, depthFormatList[i], &formatProperties);
        if (formatProperties.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){
            depthFormat = depthFormatList[i];
            break;
        }
    }

    LV_ASSERT(depthFormat != VK_FORMAT_UNDEFINED);

    VmaAllocation depthAllocation;
    ctx->depthImage.image = CreateImage(ctx->device, ctx->allocator, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        ctx->window.w, ctx->window.h, 1, 1, ctx->sampleCount, &depthAllocation);
    ctx->depthImage.view = CreateImageView(ctx->device, ctx->depthImage.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1);
    ctx->depthImage.format = depthFormat;
    ctx->depthImage.allocation = depthAllocation;

    ctx->msaaColorImage.format = imageFormat;
    if (ctx->sampleCount != VK_SAMPLE_COUNT_1_BIT) {
        CreateMSAAColorImage(ctx);
    }

    CreateBuffer(&ctx->scratchBuffer, ctx->device, 128 * 1024 * 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, ctx->allocator);

    VkSemaphoreCreateInfo semaphoreCI = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    VkFenceCreateInfo fenceCI = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkCreateFence(ctx->device, &fenceCI, NULL, &ctx->fences[i]));
        VK_CHECK(vkCreateSemaphore(ctx->device, &semaphoreCI, NULL, &ctx->presentSemaphores[i]));
    }

    for (u32 i = 0; i < swapchainImageCount; i++) {
        VK_CHECK(vkCreateSemaphore(ctx->device, &semaphoreCI, NULL, &ctx->renderSemaphores[i]));
    }

    VkCommandPoolCreateInfo commandPoolCI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->queueFamily
    };

    VK_CHECK(vkCreateCommandPool(ctx->device, &commandPoolCI, NULL, &ctx->commandPool));

    VkCommandBufferAllocateInfo cbAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->commandPool,
        .commandBufferCount = MAX_FRAMES_IN_FLIGHT
    };

    VK_CHECK(vkAllocateCommandBuffers(ctx->device, &cbAllocInfo, ctx->commandBuffers));

    gCtx = ctx;
}
