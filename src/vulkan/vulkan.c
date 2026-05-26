#include "vulkan.h"
#include "shader.h"
#include "buffer.h"
#include "texture.h"
#include "vma.h"
#include "../platform.h"
#include "../common.h"
#include "../log.h"

#include <stdio.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <ktx.h>
#include <ktxvulkan.h>

#define OG_DS_IMPLEMENTATION
#include "../og_ds.h"

#define FAST_OBJ_IMPLEMENTATION
#include "fast_obj.h"


void AppendMesh(Geometry *geom, Vertex *vertices, u32 *indices, u32 textureIndex)
{
    mesh_t mesh = {0};
    mesh.vertexOffset = ArrayCount(geom->vertices);
    mesh.vertexCount = ArrayCount(vertices);

    ArrayPushArray(geom->vertices, vertices, ArrayCount(vertices));
    ArrayPushArray(geom->indices, indices, ArrayCount(indices));

    vec3_t center =  {0};
    for (u32 i = 0; i < ArrayCount(vertices); i++) {
        HMM_Add(center, vertices[i].p);
    }
    HMM_Div(center, (f32)ArrayCount(vertices));

    f32 radius = 0.0f;
    for (u32 i = 0; i < ArrayCount(vertices); i++) {
        radius = MAX(radius, HMM_Len(HMM_Sub(vertices[i].p, center)));
    }
    mesh.center = center;
    mesh.radius = radius;
    mesh.textureIndex = textureIndex;

    ArrayPush(geom->meshes, mesh);
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

b8 LoadObj(Vertex **ppVertices, const char *path, memory_arena_t *arena) 
{
    Vertex *vertices = NULL;
    fastObjMesh *obj = fast_obj_read("../assets/suzanne.obj");
    if (obj) {
        u32 index_count = 0;
        for (u32 i = 0; i < obj->face_count; i++) {
            index_count += 3 * (obj->face_vertices[i] - 2);
        }

        ArrayInitWithArena(vertices, arena, index_count);
        ArrayResize(vertices, index_count);

        u32 vertex_offset = 0;
        u32 index_offset = 0;

        for (u32 i = 0; i < obj->face_count; i++) {
            for (u32 j = 0; j < obj->face_vertices[i]; j++) {
                fastObjIndex gi = obj->indices[index_offset + j];
                if (j >= 3) { 
                    vertices[vertex_offset + 0] = vertices[vertex_offset - 3];
                    vertices[vertex_offset + 1] = vertices[vertex_offset - 1];
                    vertex_offset += 2;
                }

                Vertex *v = &vertices[vertex_offset++];
                v->p.X = obj->positions[3 * gi.p + 0];
                v->p.Y = obj->positions[3 * gi.p + 1];
                v->p.Z = obj->positions[3 * gi.p + 2];
                v->t.X = obj->texcoords[2 * gi.t + 0];
                v->t.Y = 1.0f - obj->texcoords[2 * gi.t + 1];
                v->n.X = obj->normals[3 * gi.n + 0];
                v->n.Y = obj->normals[3 * gi.n + 1];
                v->n.Z = obj->normals[3 * gi.n + 2];
            }
            index_offset += obj->face_vertices[i];
        }
        LV_ASSERT(vertex_offset == index_count);
    }

    *ppVertices = vertices;

    fast_obj_destroy(obj);
    return true;
}

b8 LoadMesh(Geometry *geom, const char *path, memory_arena_t *arena, u32 textureIndex)
{
    Vertex *vertices = NULL;
    if (!LoadObj(&vertices, path, arena)) {
        printf("Unable to load mesh from path: %s\n", path);
        return false;
    }

    u32 *indices = NULL;
    ArrayInitWithArena(indices, arena, ArrayCount(vertices));
    ArrayResize(indices, ArrayCount(vertices));

    for (u32 i = 0; i < ArrayCount(indices); i++) {
        indices[i] = i;
    }

    AppendMesh(geom, vertices, indices, textureIndex);

    return true;
}

static void GeomLoadFunc(void *data, memory_arena_t *arena)
{
    //we only need this temporarily to load the mesh data into buffers
    Geometry *geometry = (Geometry *)data;
    
    ArrayInitWithArena(geometry->vertices, arena, MAX_VERTICES);
    ArrayInitWithArena(geometry->indices, arena, MAX_INDICES);
    ArrayInitWithArena(geometry->meshes, arena, MAX_MESHES);

    if (!LoadMesh(geometry, "../assets/suzanne.obj", arena, 0)) {
        LOGF("Unable to load mesh");
    }
}

void VulkanShutdown(vulkan_context_t *ctx)
{
    VK_CHECK(vkDeviceWaitIdle(ctx->device));

    for (u32 i = 0; i < ARRAY_SIZE(ctx->textures); i++) {
        DestroyTexture(&ctx->textures[i], ctx->allocator, ctx->device);
    }

    DestroyGraphicsPipeline(&ctx->pipeline, ctx->device);
    DestroyComputePipeline(&ctx->computePipeline, ctx->device);

    vkDestroyDescriptorSetLayout(ctx->device, ctx->texLayout, NULL);
    vkDestroyDescriptorPool(ctx->device, ctx->descriptorPool, NULL);
    vkDestroyCommandPool(ctx->device, ctx->commandPool, NULL);

    DestroyBuffer(&ctx->scratchBuffer, ctx->allocator);
    DestroyBuffer(&ctx->drawCommandCountBuffer, ctx->allocator);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        DestroyBuffer(&ctx->shaderGlobalsBuffers[i], ctx->allocator);
        DestroyBuffer(&ctx->drawCommandBuffers[i], ctx->allocator);
    }

    DestroyBuffer(&ctx->vertexBuffer, ctx->allocator);
    DestroyBuffer(&ctx->indexBuffer, ctx->allocator);
    DestroyBuffer(&ctx->drawBuffer, ctx->allocator);
    DestroyBuffer(&ctx->meshBuffer, ctx->allocator);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyFence(ctx->device, ctx->fences[i], NULL);
        vkDestroySemaphore(ctx->device, ctx->presentSemaphores[i], NULL);
    }

    for (u32 i = 0; i < MAX_SWAPCHAIN_IMAGES; i++) {
        vkDestroyImageView(ctx->device, ctx->swapchainImages[i].view, NULL);
        vkDestroySemaphore(ctx->device, ctx->renderSemaphores[i], NULL);
    }

    vkDestroyImageView(ctx->device, ctx->depthImage.view, NULL);
    vmaDestroyImage(ctx->allocator, ctx->depthImage.image, ctx->depthImage.allocation);

    vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
    SDL_Vulkan_DestroySurface(ctx->instance, ctx->window.surface, NULL);
    SDL_DestroyWindow(ctx->window.window);

    vmaDestroyAllocator(ctx->allocator);

    vkDestroyDevice(ctx->device, NULL);
    vkDestroyInstance(ctx->instance, NULL);

    volkFinalize();
}

void VulkanRender(vulkan_context_t *ctx)
{
    float cameraZ = 6.0f;

    VK_CHECK(vkWaitForFences(ctx->device, 1, &ctx->fences[ctx->frameIndex], true, UINT64_MAX));
    VK_CHECK(vkResetFences(ctx->device, 1, &ctx->fences[ctx->frameIndex]));

    u32 imageIndex;
    VK_CHECK(vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX, ctx->presentSemaphores[ctx->frameIndex], VK_NULL_HANDLE, &imageIndex));
    
    f32 znear = 0.1f;
    f32 zfar = 256.0f;

    globals_t globals = {0};
    globals.projection = HMM_Perspective_LH_ZO(HMM_AngleDeg(60.0f), (f32)ctx->window.w / (f32)ctx->window.h, znear, zfar);
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
    globals.drawCount = ctx->drawCount;

    UploadBuffer(&ctx->shaderGlobalsBuffers[ctx->frameIndex], &globals, sizeof(globals_t), 0);

    VkCommandBuffer cb = ctx->commandBuffers[ctx->frameIndex];

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

    vkCmdFillBuffer(cb, ctx->drawCommandCountBuffer.buffer, 0, sizeof(u32), 0);

    StageBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_2_MEMORY_WRITE_BIT, 
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
    );

    //color and depth image need barriers for layout transitions
    VkImageMemoryBarrier2 colorBarrier = ImageBarrier(ctx->swapchainImages[imageIndex].image,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, 1);
    
    VkImageMemoryBarrier2 depthBarrier = ImageBarrier(ctx->depthImage.image,
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
        .imageView = ctx->swapchainImages[imageIndex].view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {1.0f, 1.0f, 0.0f, 1.0f}},
    };

    VkRenderingAttachmentInfo depthAttachmentInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = ctx->depthImage.view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {.depthStencil = {1.0f, 0.0f}},
    };

    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea.extent.width = ctx->window.w,
        .renderArea.extent.height = ctx->window.h,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentInfo,
        .pDepthAttachment = &depthAttachmentInfo,
    };

    //dispatch compute shader here
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->computePipeline.pipeline);
    vkCmdPushConstants(cb, ctx->computePipeline.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shader_data_t), &ctx->shaderData[ctx->frameIndex]);
    u32 numWorkgroups = (ctx->drawCount + 63) / 64;
    vkCmdDispatch(cb, numWorkgroups, 1, 1);
    
    // Synchronize compute shader write with draw indirect read
    StageBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    
    vkCmdBeginRendering(cb, &renderingInfo);

    VkViewport vp = {
        .width = (f32)ctx->window.w,
        .height = (f32)ctx->window.h,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor = {
        .extent.width =  ctx->window.w,
        .extent.height = ctx->window.h,
    };

    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline.pipelineLayout, 0, 1, &ctx->descriptorSetTex, 0, NULL);

    VkDeviceSize vOffset = 0;        
    vkCmdBindVertexBuffers(cb, 0, 1, &ctx->vertexBuffer.buffer, &vOffset);
    vkCmdBindIndexBuffer(cb, ctx->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cb, ctx->pipeline.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(shader_data_t), &ctx->shaderData[ctx->frameIndex]);

    vkCmdDrawIndexedIndirectCount(cb, ctx->drawCommandBuffers[ctx->frameIndex].buffer, offsetof(draw_command_t, command), ctx->drawCommandCountBuffer.buffer, 0, ctx->drawCount, sizeof(draw_command_t));
    vkCmdEndRendering(cb);

    VkImageMemoryBarrier2 barrierPresent = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .image = ctx->swapchainImages[imageIndex].image,
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
        .pWaitSemaphores = &ctx->presentSemaphores[ctx->frameIndex],
        .pWaitDstStageMask = &waitStages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &ctx->renderSemaphores[imageIndex],
    };

    VK_CHECK(vkQueueSubmit(ctx->queue, 1, &submitInfo, ctx->fences[ctx->frameIndex]));

    ctx->frameIndex = (ctx->frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ctx->renderSemaphores[imageIndex],
        .swapchainCount = 1,
        .pSwapchains = &ctx->swapchain,
        .pImageIndices = &imageIndex,
    };

    VK_CHECK(vkQueuePresentKHR(ctx->queue, &presentInfo));

    if (ctx->windowResized) {
        if (SDL_GetWindowSize(ctx->window.window, &ctx->window.w, &ctx->window.h)) {
            printf("Window resized: %ux%u\n", ctx->window.w, ctx->window.h);
        }
        ctx->windowResized = false;

        VK_CHECK(vkDeviceWaitIdle(ctx->device));
    
        VkSurfaceCapabilitiesKHR surfaceCaps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physicalDevice, ctx->window.surface, &surfaceCaps));

        VkSwapchainCreateInfoKHR swapchainCI = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = ctx->window.surface,
            .minImageCount = surfaceCaps.minImageCount,
            .imageFormat = ctx->swapchainImages[0].format,
            .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
            .imageExtent.width = ctx->window.w,
            .imageExtent.height = ctx->window.h,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .oldSwapchain = ctx->swapchain,
        };

        VK_CHECK(vkCreateSwapchainKHR(ctx->device, &swapchainCI, NULL, &ctx->swapchain));

        for (u32 i = 0; i < ctx->swapchainImageCount; i++) {
            vkDestroyImageView(ctx->device, ctx->swapchainImages[i].view, NULL);
        }

        VkImage images[MAX_SWAPCHAIN_IMAGES];
        VK_CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &ctx->swapchainImageCount, images));
        LV_ASSERT(ctx->swapchainImageCount <= MAX_SWAPCHAIN_IMAGES);

        VkFormat format = ctx->swapchainImages[0].format;
        for (u32 i = 0; i < ctx->swapchainImageCount; i++) {
            ctx->swapchainImages[i].image = images[i];
            ctx->swapchainImages[i].view = CreateImageView(ctx->device, images[i], format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }

        vkDestroySwapchainKHR(ctx->device, swapchainCI.oldSwapchain, NULL);
        vmaDestroyImage(ctx->allocator, ctx->depthImage.image, ctx->depthImage.allocation);
        vkDestroyImageView(ctx->device, ctx->depthImage.view, NULL);

        ctx->depthImage.image = CreateImage(ctx->device, ctx->allocator, ctx->depthImage.format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, ctx->window.w, ctx->window.h, 1, &ctx->depthImage.allocation);
        ctx->depthImage.view = CreateImageView(ctx->device, ctx->depthImage.image, ctx->depthImage.format, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    }
}

void VulkanHotReload(vulkan_context_t *ctx)
{
    //reinitialize volk
    if (volkInitialize() != VK_SUCCESS) {
        LOGF("Unable to initialize volk");
        return;
    }
    volkLoadInstance(ctx->instance);
    volkLoadDevice(ctx->device);
}

void VulkanInit(vulkan_context_t *ctx, platform_api_t *api, string_interning_system_t *stringInterning)
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
        .enabledExtensionCount = api->vulkanInstanceExtensionCount,
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = &validationLayerName,
        .ppEnabledExtensionNames = api->vulkanInstanceExtensions,
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

    if (!api->VulkanGetPresentationSupport(ctx->instance, ctx->physicalDevice, ctx->queueFamily)) {
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

    if (!api->CreateWindow(&ctx->window, ctx->instance, "HowToVulkan", 1280U, 720U, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE)) {
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

    //launch job to load geometry
    PushJob(GeomLoadFunc, &ctx->geometry);

    shader_t *shaders = NULL;
    resource_loader_t loader = {};
    loader.stringInterning = stringInterning;
    loader.loadedData = &shaders;
    PushJob(LoadShaders, &loader);
    WaitForAllJobs();

    VkDeviceSize vBufSize = sizeof(Vertex) * ArrayCount(ctx->geometry.vertices);
    CreateBuffer(&ctx->vertexBuffer, 
        ctx->device,
        vBufSize, 
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);
    UploadBuffer(&ctx->vertexBuffer, ctx->geometry.vertices, vBufSize, 0);

    VkDeviceSize iBufSize = sizeof(u32) * ArrayCount(ctx->geometry.indices);
    CreateBuffer(&ctx->indexBuffer, 
        ctx->device,
        iBufSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);
    UploadBuffer(&ctx->indexBuffer, ctx->geometry.indices, iBufSize, 0);

    ctx->geometry.indexCount = ArrayCount(ctx->geometry.indices);

    VkDeviceSize meshBufSize = sizeof(mesh_t) * ArrayCount(ctx->geometry.meshes);
    CreateBuffer(&ctx->meshBuffer,
        ctx->device,
        meshBufSize,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);
    UploadBuffer(&ctx->meshBuffer, ctx->geometry.meshes, meshBufSize, 0);

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

    //draws
    SDL_srand(42);
    ctx->drawCount = 25000;
    f32 sceneRadius = 300.0f;

    memory_arena_t *arena = PermanentArena();
    draw_data_t *draws = NULL;
    
    ArrayInitWithArena(draws, arena, ctx->drawCount);
    ArrayResize(draws, ctx->drawCount);

    for (u32 i = 0; i < ctx->drawCount; i++) {
        draw_data_t *draw = &draws[i];
        draw->position.X = (f32)(SDL_randf() * sceneRadius * 2 - sceneRadius);
        draw->position.Y = (f32)(SDL_randf() * sceneRadius * 2 - sceneRadius);
        draw->position.Z = (f32)(SDL_randf() * sceneRadius * 2 - sceneRadius);
        draw->scale = (f32)(SDL_randf()) + 1; 
        draw->scale *= 2.0f;
        draw->meshIndex = 0;

        f32 angle = SDL_randf() * SDL_PI_F / 2.0f;
        vec3_t axis = (vec3_t){
            (f32)(SDL_randf() * 2.0f - 1.0f),
            (f32)(SDL_randf() * 2.0f - 1.0f),
            (f32)(SDL_randf() * 2.0f - 1.0f)
        };

        axis = HMM_NormV3(axis);

        quat_t orientation = HMM_QFromAxisAngle_LH(axis, angle);
        draw->rx = orientation.X;
        draw->ry = orientation.Y;
        draw->rz = orientation.Z;
        draw->rw = orientation.W;
    }

    CreateBuffer(&ctx->drawBuffer, 
        ctx->device,
        sizeof(draw_data_t) * ctx->drawCount,
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);
 
    UploadBuffer(&ctx->drawBuffer, draws, sizeof(draw_data_t) * ctx->drawCount, 0);
    LOGI("Size of draw data: %zu", sizeof(draw_data_t));
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(&ctx->drawCommandBuffers[i], 
            ctx->device,
            ctx->drawCount * sizeof(draw_command_t), 
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

    CreateBuffer(&ctx->scratchBuffer, 
        ctx->device,
        128 * 1024 * 1024, 
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        ctx->allocator);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        ctx->shaderData[i].globalsAddress = ctx->shaderGlobalsBuffers[i].deviceAddress;
        ctx->shaderData[i].drawDataAddress = ctx->drawBuffer.deviceAddress;
        ctx->shaderData[i].meshAddress = ctx->meshBuffer.deviceAddress;
        ctx->shaderData[i].drawCommandAddress = ctx->drawCommandBuffers[i].deviceAddress;
        ctx->shaderData[i].drawCommandCountAddress = ctx->drawCommandCountBuffer.deviceAddress;
    }

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

    for (u32 i = 0; i < ARRAY_SIZE(ctx->textures); i++) {
        const char * texPath = ArenaPrintf(ScratchArena(0), "../assets/suzanne%u.ktx", i);
        CreateTexture(&ctx->textures[i], &ctx->textureDescriptors[i], &ctx->scratchBuffer, ctx->device, ctx->allocator, ctx->commandPool, ctx->queue, texPath);
    }

    VkDescriptorPoolSize poolSize = {
        .type =  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = ARRAY_SIZE(ctx->textures),
    };

    VkDescriptorPoolCreateInfo descPoolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };

    VK_CHECK(vkCreateDescriptorPool(ctx->device, &descPoolCI, NULL, &ctx->descriptorPool));

    ctx->texLayout = CreateDescriptorSetLayout(ctx->device, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, ARRAY_SIZE(ctx->textures));

    CreateGraphicsPipeline(&ctx->pipeline, stringInterning, shaders, ctx->device, "shader.spv", imageFormat, depthFormat, sizeof(shader_data_t), ctx->texLayout);
    CreateComputePipeline(&ctx->computePipeline, stringInterning, shaders, ctx->device, "compute_shader.spv", sizeof(shader_data_t));

    u32 variableDescCount = ARRAY_SIZE(ctx->textures);
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &variableDescCount,
    };

    VkDescriptorSetAllocateInfo texDescSetAlloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variableDescCountAI,
        .descriptorPool = ctx->descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &ctx->texLayout,
    };

    VK_CHECK(vkAllocateDescriptorSets(ctx->device, &texDescSetAlloc, &ctx->descriptorSetTex));

    VkWriteDescriptorSet writeDescSet = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ctx->descriptorSetTex,
        .dstBinding = 0,
        .descriptorCount = ARRAY_SIZE(ctx->textures),
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = ctx->textureDescriptors,
    };

    vkUpdateDescriptorSets(ctx->device, 1, &writeDescSet, 0, NULL);
}

