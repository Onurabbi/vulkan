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
        }
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
    if (!CreateTexture(&textureResource->texture, &gCtx->scratchBuffer, gCtx->device, gCtx->allocator, gCtx->commandPool, gCtx->queue, gCtx->texSampler, path, layerCount)) {
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

    vkDestroyDescriptorSetLayout(gCtx->device, gCtx->texLayout, NULL);
    vkDestroyDescriptorSetLayout(gCtx->device, gCtx->dummyLayout, NULL);

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

    vkDestroyImageView(gCtx->device, gCtx->brdfLut.view, NULL);
    vmaDestroyImage(gCtx->allocator, gCtx->brdfLut.image, gCtx->brdfLut.allocation);

    vkDestroyImageView(gCtx->device, gCtx->diffuseIrradianceMap.view, NULL);
    vmaDestroyImage(gCtx->allocator, gCtx->diffuseIrradianceMap.image, gCtx->diffuseIrradianceMap.allocation);

    vkDestroyImageView(gCtx->device, gCtx->prefilteredEnv.view, NULL);
    vmaDestroyImage(gCtx->allocator, gCtx->prefilteredEnv.image, gCtx->prefilteredEnv.allocation);

    vkDestroySwapchainKHR(gCtx->device, gCtx->swapchain, NULL);
    SDL_Vulkan_DestroySurface(gCtx->instance, gCtx->window.surface, NULL);
    SDL_DestroyWindow(gCtx->window.window);

    vkDestroySampler(gCtx->device, gCtx->texSampler, NULL);
    vkDestroySampler(gCtx->device, gCtx->cubemapSampler, NULL);

    vmaDestroyAllocator(gCtx->allocator);

    vkDestroyDevice(gCtx->device, NULL);
    vkDestroyInstance(gCtx->instance, NULL);

    volkFinalize();
}

static void GenerateIBLCubemap(image_t *output, int32_t dim, VkFormat format, skybox_data_t *skyboxData, VkBuffer indexBuffer, VkDevice device, VkCommandBuffer cb, pipeline_t *pipeline, VkDescriptorSet descriptorSet, VkQueue queue, VkFence fence, VmaAllocator allocator)
{
    const uint32_t mipCount = (uint32_t)(floor(log2(dim))) + 1;

    VmaAllocation outputAllocation;
    output->image = CreateImage(device, allocator, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        dim, dim, mipCount, 6, &outputAllocation);
    output->view = CreateImageView(device, output->image, format, VK_IMAGE_ASPECT_COLOR_BIT, mipCount, 6);
    output->allocation = outputAllocation;
    output->format = format;

    // Create offscreen image
    VmaAllocation offscreenImageAllocation;
    VkImage offscreenImage = CreateImage(device, allocator, output->format,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, dim, dim, 1, 1, &offscreenImageAllocation);
    VkImageView offscreenView = CreateImageView(device, offscreenImage, output->format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);

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

    vkEndCommandBuffer(cb);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb
    };

    VK_CHECK(vkResetFences(device, 1, &fence));
    VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkQueueWaitIdle(queue);

    // layout transition for cubemap faces undefined -> transfer dst
    VK_CHECK(vkResetCommandBuffer(cb, 0));
    VK_CHECK(vkBeginCommandBuffer(cb, &cbBeginInfo));

    colorBarrier = ImageBarrier(output->image,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount);
    PipelineBarrier(cb, 0, 0, NULL, 1, &colorBarrier);

    vkEndCommandBuffer(cb);

    VK_CHECK(vkResetFences(device, 1, &fence));
    VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkQueueWaitIdle(queue);

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
            .width = dim,
            .height = dim
        },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentInfo,
        .pDepthAttachment = VK_NULL_HANDLE
    };

    VkViewport viewport = {
        .width = (float)dim,
        .height = (float)dim,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };

    VkRect2D scissor = {
        .extent = {
            .width = dim,
            .height = dim
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

    // Render all cubemap faces
    for (u32 level = 0; level < mipCount; level++) {
        for (u32 face = 0; face < 6; face++) {
            // layout transition for all cubemap faces
            VK_CHECK(vkResetCommandBuffer(cb, 0));

            VkCommandBufferBeginInfo cbBeginInfo = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
            };

            VK_CHECK(vkBeginCommandBuffer(cb, &cbBeginInfo));

            viewport.width  = (float)(dim * powf(0.5f, level));
            viewport.height = (float)(dim * powf(0.5f, level));

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
            
            vkCmdPushConstants(cb, pipeline->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(skybox_data_t), skyboxData);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipelineLayout, 0, 1, &descriptorSet, 0, NULL);
            vkCmdBindIndexBuffer(cb, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, skyboxData->indexCount, 1, skyboxData->indexOffset, skyboxData->vertexOffset, 0);

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

            vkEndCommandBuffer(cb);

            VkSubmitInfo submitInfo = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers    = &cb
            };

            VK_CHECK(vkResetFences(device, 1, &fence));
            VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
            VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
            vkQueueWaitIdle(queue);
        }
    }

    // Transition the generated cubemap from transfer dst to shader read-only
    VK_CHECK(vkResetCommandBuffer(cb, 0));
    VK_CHECK(vkBeginCommandBuffer(cb, &cbBeginInfo));

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

    VK_CHECK(vkResetFences(device, 1, &fence));
    VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo2, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkQueueWaitIdle(queue);

    vkDestroyImageView(device, offscreenView, NULL);
    vmaDestroyImage(allocator, offscreenImage, offscreenImageAllocation);
}

static void GenerateBRDFLUT(image_t *brdfLut, VkDevice device, VkCommandBuffer cb, pipeline_t *pipeline, VkQueue queue, VkFence fence, VmaAllocator allocator)
{
    VmaAllocation brdfLutAllocation;
    const int32_t brdfLutDim = 512;
    VkFormat brdfLutFormat = VK_FORMAT_R16G16_SFLOAT;
    brdfLut->image = CreateImage(device, allocator, brdfLutFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 
        brdfLutDim, brdfLutDim, 1, 1, &brdfLutAllocation);
    brdfLut->view  = CreateImageView(device, brdfLut->image, brdfLutFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    brdfLut->allocation = brdfLutAllocation;
    brdfLut->format = brdfLutFormat;

    VK_CHECK(vkResetCommandBuffer(cb, 0));
    
    VkCommandBufferBeginInfo cbBeginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VK_CHECK(vkBeginCommandBuffer(cb, &cbBeginInfo));

    // Layout transition for bfrd lut image
    VkImageMemoryBarrier2 colorBarrier = ImageBarrier(brdfLut->image, 
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
    );

    PipelineBarrier(cb, 0, 0, NULL, 1, &colorBarrier);

    VkRenderingAttachmentInfo colorAttachmentInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = brdfLut->view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {1.0f, 1.0f, 0.0f, 1.0f}},
    };

    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea.extent = {
            .width  = 512,
            .height = 512
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
        .width    = (f32)512,
        .height   = (f32)512,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };

    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D scissor = {
        .offset = { 0, 0},
        .extent = { 512, 512 }
    };

    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);

    // Transition BRDF LUT image layout to shader read-only optimal for use as a lookup table in PBR shader
    VkImageMemoryBarrier2 lutBarrier = ImageBarrier(brdfLut->image, 
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
    );

    PipelineBarrier(cb, 0, 0, NULL, 1, &lutBarrier);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb
    };
    
    VK_CHECK(vkResetFences(device, 1, &fence));
    VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkQueueWaitIdle(queue);
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

    const resource_t **textureResources = ResourceSystemGetTextures(ScratchArena());
    u32 texCount = ArrayCount(textureResources) + 3; // plus 3 for genbrdflut, diffuse irradiance and prefiltered env map

    ctx->texLayout    = CreateDescriptorSetLayout(ctx->device, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, texCount, true);
    ctx->dummyLayout  = CreateDescriptorSetLayout(ctx->device, 0, 0, 0, false);

    VkDescriptorPoolSize poolSizes[1] = { 
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount = texCount }, 
    };

    VkDescriptorPoolCreateInfo descPoolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = poolSizes
    };

    VK_CHECK(vkCreateDescriptorPool(ctx->device, &descPoolCI, NULL, &ctx->descriptorPool));

    // Finally we can write descriptors
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &texCount,
    };

    VkDescriptorSetAllocateInfo texDescSetAlloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variableDescCountAI,
        .descriptorPool = ctx->descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &ctx->texLayout,
    };

    VK_CHECK(vkAllocateDescriptorSets(ctx->device, &texDescSetAlloc, &ctx->descriptorSetTex));

    VkDescriptorImageInfo *textureDescriptors = NULL;
    // Allocating extra memory for BRDF Lut, irradiance cube map and pre-filtered environment cube map
    ArrayInitWithArena(textureDescriptors, ScratchArena(), texCount);

    for (u32 i = 0; i < texCount - 3; i++) {
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
    CreateGraphicsPipeline(&ctx->pipeline, "shader.spv", ctx->device, imageFormat, depthFormat, sizeof(pbr_data_t), ctx->texLayout, VK_TRUE, VK_CULL_MODE_BACK_BIT);
    CreateGraphicsPipeline(&ctx->skyboxPipeline, "skybox.spv", ctx->device, imageFormat, depthFormat, sizeof(skybox_data_t), ctx->texLayout, VK_FALSE, VK_CULL_MODE_NONE);
    CreateGraphicsPipeline(&ctx->genBRDFLUTPipeline, "genbrdflut.spv", ctx->device, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED, 0, ctx->dummyLayout, VK_FALSE, VK_CULL_MODE_NONE);
    CreateGraphicsPipeline(&ctx->diffuseIrradianceMapPipeline, "diffuse_irradiance.spv", ctx->device, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_UNDEFINED, sizeof(skybox_data_t), ctx->texLayout, VK_FALSE, VK_CULL_MODE_NONE);
    CreateGraphicsPipeline(&ctx->prefilteredEnvMapPipeline, "prefiltered_env_map.spv", ctx->device, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED, sizeof(skybox_data_t), ctx->texLayout, VK_FALSE, VK_CULL_MODE_NONE);
    CreateComputePipeline(&ctx->computePipeline, "compute_shader.spv", ctx->device, sizeof(pbr_data_t));

    // Generate source images for pbr
    VkCommandBuffer cb = ctx->commandBuffers[ctx->frameIndex];
    GenerateBRDFLUT(&ctx->brdfLut, ctx->device, cb, &ctx->genBRDFLUTPipeline, ctx->queue, ctx->fences[ctx->frameIndex], ctx->allocator);
    ctx->brdfLut.index = ArrayCount(textureDescriptors);

    GenerateIBLCubemap(&ctx->diffuseIrradianceMap, 64, VK_FORMAT_R32G32B32A32_SFLOAT, &ctx->skyboxData[ctx->frameIndex], ctx->indexBuffer.buffer, ctx->device, cb,
        &ctx->diffuseIrradianceMapPipeline, ctx->descriptorSetTex, ctx->queue, ctx->fences[ctx->frameIndex], ctx->allocator);
    ctx->diffuseIrradianceMap.index = ArrayCount(textureDescriptors) + 1;

    GenerateIBLCubemap(&ctx->prefilteredEnv, 512, VK_FORMAT_R16G16B16A16_SFLOAT, &ctx->skyboxData[ctx->frameIndex], ctx->indexBuffer.buffer, ctx->device, cb,
        &ctx->prefilteredEnvMapPipeline, ctx->descriptorSetTex, ctx->queue, ctx->fences[ctx->frameIndex], ctx->allocator);
    ctx->prefilteredEnv.index = ArrayCount(textureDescriptors) + 2;

    // We can finally push the generated images
    VkDescriptorImageInfo brdflutInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        .imageView   = ctx->brdfLut.view,
        .sampler     = ctx->cubemapSampler
    };

    VkDescriptorImageInfo diffuseIrradianceInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        .imageView   = ctx->diffuseIrradianceMap.view,
        .sampler     = ctx->cubemapSampler
    };

    VkDescriptorImageInfo prefilteredEnvInfo = {
        .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
        .imageView   = ctx->prefilteredEnv.view,
        .sampler     = ctx->cubemapSampler
    };

    VkDescriptorImageInfo genImageInfo[3] = { brdflutInfo, diffuseIrradianceInfo, prefilteredEnvInfo };

    VkWriteDescriptorSet writeDescSet2 = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ctx->descriptorSetTex,
        .dstBinding = 0,
        .dstArrayElement = ArrayCount(textureDescriptors),
        .descriptorCount = 3,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = genImageInfo,
    };

    vkUpdateDescriptorSets(ctx->device, 1, &writeDescSet2, 0, NULL);
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
    globals.brdflutIndex = gCtx->brdfLut.index;
    globals.diffuseIrradianceIndex = gCtx->diffuseIrradianceMap.index;
    globals.prefilteredEnvIndex = gCtx->prefilteredEnv.index;

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

    VkImageMemoryBarrier2 outputBarriers[] = {colorBarrier, depthBarrier};

    PipelineBarrier(cb, 0, 0, NULL, ARRAY_SIZE(outputBarriers), outputBarriers);

    VkRenderingAttachmentInfo colorAttachmentInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = gCtx->swapchainImages[imageIndex].view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
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
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
            printf("Window resized: %ux%u\n", gCtx->window.w, gCtx->window.h);
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

        gCtx->depthImage.image = CreateImage(gCtx->device, gCtx->allocator, gCtx->depthImage.format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, gCtx->window.w, gCtx->window.h, 1, 1, &gCtx->depthImage.allocation);
        gCtx->depthImage.view = CreateImageView(gCtx->device, gCtx->depthImage.image, gCtx->depthImage.format, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1);
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
    int rc = system("ninja compile_shaders");
    if (rc != 0) {
        printf("Unable to compile shaders\n");
    }

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
        ctx->window.w, ctx->window.h, 1, 1, &depthAllocation);
    ctx->depthImage.view = CreateImageView(ctx->device, ctx->depthImage.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1);
    ctx->depthImage.format = depthFormat;
    ctx->depthImage.allocation = depthAllocation;

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
