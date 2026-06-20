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

void VulkanLoadTexture(resource_t *textureResource, const char *path)
{
    if (!CreateTexture(&textureResource->texture, &gCtx->scratchBuffer, gCtx->device, gCtx->allocator, gCtx->commandPool, gCtx->queue, path)) {
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
    DestroyComputePipeline(&gCtx->computePipeline, gCtx->device);

    vkDestroyDescriptorSetLayout(gCtx->device, gCtx->texLayout, NULL);
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

    vkDestroySwapchainKHR(gCtx->device, gCtx->swapchain, NULL);
    SDL_Vulkan_DestroySurface(gCtx->instance, gCtx->window.surface, NULL);
    SDL_DestroyWindow(gCtx->window.window);

    vmaDestroyAllocator(gCtx->allocator);

    vkDestroyDevice(gCtx->device, NULL);
    vkDestroyInstance(gCtx->instance, NULL);

    volkFinalize();
}

static void VulkanLoadResources(vulkan_context_t *ctx)
{
    ctx->resourcesLoaded = true;


    //draws
    SDL_srand(188);

    geometry_t geometry = ResourceSystemGetGeometry();

    VkDeviceSize vBufSize = sizeof(vertex_t) * ArrayCount(geometry.vertices);
    CreateBuffer(&ctx->vertexBuffer,
        ctx->device,
        vBufSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);
    UploadBuffer(&ctx->vertexBuffer, geometry.vertices, vBufSize, 0);

    VkDeviceSize iBufSize = sizeof(u32) * ArrayCount(geometry.indices);
    CreateBuffer(&ctx->indexBuffer,
        ctx->device,
        iBufSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);
    UploadBuffer(&ctx->indexBuffer, geometry.indices, iBufSize, 0);

    ctx->indexCount = ArrayCount(geometry.indices);

    VkDeviceSize meshBufSize = sizeof(mesh_t) * ArrayCount(geometry.meshes);
    CreateBuffer(&ctx->meshBuffer,
        ctx->device,
        meshBufSize,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);
    UploadBuffer(&ctx->meshBuffer, geometry.meshes, meshBufSize, 0);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(&ctx->shaderGlobalsBuffers[i],
            ctx->device,
            sizeof(globals_t),
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            ctx->allocator);
    }

    gCtx->drawCount = ArrayCount(geometry.draws);

    CreateBuffer(&ctx->drawBuffer,
        ctx->device,
        sizeof(draw_data_t) * gCtx->drawCount,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);

    UploadBuffer(&ctx->drawBuffer, geometry.draws, sizeof(draw_data_t) * gCtx->drawCount, 0);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(&ctx->drawCommandBuffers[i],
            ctx->device,
            gCtx->drawCount * sizeof(draw_command_t),
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            0,
            ctx->allocator);
    }

    CreateBuffer(&ctx->drawCommandCountBuffer,
        ctx->device,
        sizeof(u32),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator
    );

    CreateBuffer(&ctx->materialBuffer,
        ctx->device,
        sizeof(material_t) * ArrayCount(geometry.materials),
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        ctx->shaderData[i].globalsAddress = ctx->shaderGlobalsBuffers[i].deviceAddress;
        ctx->shaderData[i].drawDataAddress = ctx->drawBuffer.deviceAddress;
        ctx->shaderData[i].meshAddress = ctx->meshBuffer.deviceAddress;
        ctx->shaderData[i].drawCommandAddress = ctx->drawCommandBuffers[i].deviceAddress;
        ctx->shaderData[i].drawCommandCountAddress = ctx->drawCommandCountBuffer.deviceAddress;
        ctx->shaderData[i].materialAddress = ctx->materialBuffer.deviceAddress;
    }

    const resource_t **textureResources = ResourceSystemGetTextures(ScratchArena(0));
    u32 texCount = ArrayCount(textureResources);

    VkDescriptorPoolSize poolSize = {
        .type =  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = texCount,
    };

    VkDescriptorPoolCreateInfo descPoolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };

    VK_CHECK(vkCreateDescriptorPool(ctx->device, &descPoolCI, NULL, &ctx->descriptorPool));

    ctx->texLayout = CreateDescriptorSetLayout(ctx->device, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, texCount);

    VkFormat imageFormat = ctx->swapchainImages[0].format;
    VkFormat depthFormat = ctx->depthImage.format;
    CreateGraphicsPipeline(&ctx->pipeline, ctx->device, "shader.spv", imageFormat, depthFormat, sizeof(shader_data_t), ctx->texLayout);
    CreateComputePipeline(&ctx->computePipeline, ctx->device, "compute_shader.spv", sizeof(shader_data_t));

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
    ArrayInitWithArena(textureDescriptors, ScratchArena(0), texCount);

    for (u32 i = 0; i < texCount; i++) {
        VkDescriptorImageInfo info = {0};
        info.imageLayout = textureResources[i]->texture.layout;
        info.imageView   = textureResources[i]->texture.view;
        info.sampler     = textureResources[i]->texture.sampler;
        ArrayPush(textureDescriptors, info);
    }

    VkWriteDescriptorSet writeDescSet = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ctx->descriptorSetTex,
        .dstBinding = 0,
        .descriptorCount = texCount,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = textureDescriptors,
    };

    vkUpdateDescriptorSets(ctx->device, 1, &writeDescSet, 0, NULL);
}

void VulkanRender(void)
{
    if (!gCtx->resourcesLoaded) {
        VulkanLoadResources(gCtx);
    }

    float cameraZ = 6.0f;

    VK_CHECK(vkWaitForFences(gCtx->device, 1, &gCtx->fences[gCtx->frameIndex], true, UINT64_MAX));
    VK_CHECK(vkResetFences(gCtx->device, 1, &gCtx->fences[gCtx->frameIndex]));

    u32 imageIndex;
    VK_CHECK(vkAcquireNextImageKHR(gCtx->device, gCtx->swapchain, UINT64_MAX, gCtx->presentSemaphores[gCtx->frameIndex], VK_NULL_HANDLE, &imageIndex));

    f32 znear = 0.1f;
    f32 zfar = 256.0f;

    globals_t globals = {0};
    globals.projection = HMM_Perspective_LH_ZO(HMM_AngleDeg(60.0f), (f32)gCtx->window.w / (f32)gCtx->window.h, znear, zfar);
    globals.view = HMM_LookAt_LH((vec3_t){0.0f, 0.0f, cameraZ}, (vec3_t){0.0f, 0.0f, 0.0f}, (vec3_t){0.0f, -1.0f, 0.0f});

    mat4_t projectionT = HMM_TransposeM4(globals.projection);
    vec4_t frustumX = HMM_Norm(HMM_Add(projectionT.Columns[3], projectionT.Columns[0]));
    vec4_t frustumY = HMM_Norm(HMM_Add(projectionT.Columns[3], projectionT.Columns[1]));

    globals.P00 = globals.projection.Elements[0][0];
    globals.P11 = globals.projection.Elements[1][1];
    globals.near = znear;
    globals.far = zfar;
    globals.frustum[0] = frustumX.X;
    globals.frustum[1] = frustumX.Z;
    globals.frustum[2] = frustumY.Y;
    globals.frustum[3] = frustumY.Z;
    globals.drawCount = gCtx->drawCount;
    globals.lodTarget = (2 / globals.P11) * (1.0f / (f32)gCtx->window.h);
    
    UploadBuffer(&gCtx->shaderGlobalsBuffers[gCtx->frameIndex], &globals, sizeof(globals_t), 0);

    VkCommandBuffer cb = gCtx->commandBuffers[gCtx->frameIndex];

    VK_CHECK(vkResetCommandBuffer(cb, 0));

    VkCommandBufferBeginInfo cbBI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    VK_CHECK(vkBeginCommandBuffer(cb, &cbBI));

    StageBarrier(cb, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
    );

    vkCmdFillBuffer(cb, gCtx->drawCommandCountBuffer.buffer, 0, sizeof(u32), 0);

    StageBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
    );

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

    VkDependencyInfo barrierDependencyInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = outputBarriers,
        .pNext = NULL,
    };

    vkCmdPipelineBarrier2(cb, &barrierDependencyInfo);

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

    //dispatch compute shader here
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gCtx->computePipeline.pipeline);
    vkCmdPushConstants(cb, gCtx->computePipeline.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shader_data_t), &gCtx->shaderData[gCtx->frameIndex]);
    u32 numWorkgroups = (gCtx->drawCount + 63) / 64;
    vkCmdDispatch(cb, numWorkgroups, 1, 1);

    // Synchronize compute shader write with draw indirect read
    StageBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

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

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gCtx->pipeline.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gCtx->pipeline.pipelineLayout, 0, 1, &gCtx->descriptorSetTex, 0, NULL);

    VkDeviceSize vOffset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &gCtx->vertexBuffer.buffer, &vOffset);
    vkCmdBindIndexBuffer(cb, gCtx->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cb, gCtx->pipeline.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(shader_data_t), &gCtx->shaderData[gCtx->frameIndex]);

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
            gCtx->swapchainImages[i].view = CreateImageView(gCtx->device, images[i], format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }

        vkDestroySwapchainKHR(gCtx->device, swapchainCI.oldSwapchain, NULL);
        vmaDestroyImage(gCtx->allocator, gCtx->depthImage.image, gCtx->depthImage.allocation);
        vkDestroyImageView(gCtx->device, gCtx->depthImage.view, NULL);

        gCtx->depthImage.image = CreateImage(gCtx->device, gCtx->allocator, gCtx->depthImage.format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, gCtx->window.w, gCtx->window.h, 1, &gCtx->depthImage.allocation);
        gCtx->depthImage.view = CreateImageView(gCtx->device, gCtx->depthImage.image, gCtx->depthImage.format, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    }
}

void VulkanHotReload(vulkan_context_t *ctx)
{
    gCtx = ctx;
    //reinitialize volk
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
        .shaderDrawParameters = true,
    };

    VkPhysicalDeviceVulkan12Features features12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &features11,
        .descriptorIndexing = true,
        .shaderSampledImageArrayNonUniformIndexing = true,
        .descriptorBindingVariableDescriptorCount = true,
        .runtimeDescriptorArray = true,
        .bufferDeviceAddress = true,
        .drawIndirectCount = true,
        .scalarBlockLayout = true,
        .storageBuffer8BitAccess = true,
        .uniformAndStorageBuffer8BitAccess = true,
    };

    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &features12,
        .synchronization2 = true,
        .dynamicRendering = true,
    };

    VkPhysicalDeviceFeatures features = {
        .samplerAnisotropy = true,
    };

    VkDeviceCreateInfo deviceCI = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCI,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = deviceExtensions,
        .pEnabledFeatures = &features,
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
        ctx->swapchainImages[i].view = CreateImageView(ctx->device, ctx->swapchainImages[i].image, imageFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
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
    ctx->depthImage.image = CreateImage(ctx->device, ctx->allocator, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, ctx->window.w, ctx->window.h, 1, &depthAllocation);
    ctx->depthImage.view = CreateImageView(ctx->device, ctx->depthImage.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    ctx->depthImage.format = depthFormat;
    ctx->depthImage.allocation = depthAllocation;

    CreateBuffer(&ctx->scratchBuffer,
        ctx->device,
        128 * 1024 * 1024,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);

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
