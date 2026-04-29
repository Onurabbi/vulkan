#define VOLK_IMPLEMENTATION
#include <vulkan/vulkan.h>
#include <volk/volk.h>

#define FAST_OBJ_IMPLEMENTATION
#include "fast_obj.h"

#define OG_DS_IMPLEMENTATION
#include "og_ds.h"

#include "common.h"
#include "memory.h"
#include "job.h"
#include "buffer.h"
#include "texture.h"
#include "shader.h"
#include "log.h"

#include <stdio.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_cpuinfo.h>

#include <vma/vk_mem_alloc.h>

#include <ktx.h>
#include <ktxvulkan.h>

#include <dlfcn.h>

#define MAX_FRAMES_IN_FLIGHT 2
#define MAX_SWAPCHAIN_IMAGES 3
#define NUM_TEXTURES 3

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    VmaAllocation allocation;
} Image;

typedef struct {
    SDL_Window *window;
    VkSurfaceKHR surface;
    i32 w,h;
}vulkan_window_t;

typedef struct {
    vec3_t center;
    f32 radius;
    
    u32 vertexOffset;
    u32 vertexCount;
} mesh_t;

typedef struct {
    vec3_t position;
    f32 scale;
    quat_t orientation;
    u32 meshIndex;
} draw_data_t;

typedef struct {
    Vertex *vertices;
    u32 *indices;
    mesh_t *meshes;
    u32 indexCount;//required for drawing
}Geometry;

static VkInstance gInstance;
static VkDevice gDevice;
static VkPhysicalDevice gPhysicalDevice;
static vulkan_window_t  gWindow;
static VkSwapchainKHR gSwapchain;
static Image gSwapchainImages[MAX_SWAPCHAIN_IMAGES];
static Image gDepthImage;
static u32 gSwapchainImageCount;
static u32 gQueueFamily;
static VkQueue gQueue;
static buffer_t gVertexBuffer;
static buffer_t gIndexBuffer;
static buffer_t gShaderGlobalsBuffers[MAX_FRAMES_IN_FLIGHT];
static buffer_t gDrawCommandBuffers[MAX_FRAMES_IN_FLIGHT];
static buffer_t gDrawCommandCountBuffer;
static buffer_t gScratchBuffer;
static VmaAllocator gAllocator;
static VkCommandBuffer gCommandBuffers[MAX_FRAMES_IN_FLIGHT];
static Texture gTextures[NUM_TEXTURES];
static VkDescriptorImageInfo gTextureDescriptors[NUM_TEXTURES];
static VkDescriptorSetLayout gTexLayout;
static VkDescriptorPool gDescriptorPool;
static VkDescriptorSet gDescriptorSetTex;
static VkFence gFences[MAX_FRAMES_IN_FLIGHT];
static VkSemaphore gPresentSemaphores[MAX_FRAMES_IN_FLIGHT];//max frames in flight
static VkSemaphore gRenderSemaphores[MAX_SWAPCHAIN_IMAGES]; //swapchain image count
static VkCommandPool gCommandPool;
static VkCommandBuffer gCommandBuffers[MAX_FRAMES_IN_FLIGHT];//max frames in flight
static shader_data_t gShaderData[3];
//static compute_data_t gComputeData;
static pipeline_t gPipeline;
static pipeline_t gComputePipeline;
static f32 gCameraZ = 6.0f;
static u32 gFrameIndex;
static b8 gWindowResized;
static Geometry geometry;

void AppendMesh(Geometry *geom, Vertex *vertices, u32 *indices)
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

bool LoadObj(Vertex **ppVertices, const char *path, memory_arena_t *arena) 
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

b8 LoadMesh(Geometry *geom, const char *path, memory_arena_t *arena)
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

    AppendMesh(geom, vertices, indices);

    return true;
}

static void GeomLoadFunc(void *data, memory_arena_t *arena)
{
    //we only need this temporarily to load the mesh data into buffers
    Geometry *geometry = (Geometry *)data;
    
    ArrayInitWithArena(geometry->vertices, arena, MAX_VERTICES);
    ArrayInitWithArena(geometry->indices, arena, MAX_INDICES);
    ArrayInitWithArena(geometry->meshes, arena, 16);

    if (!LoadMesh(geometry, "../assets/suzanne.obj", arena)) {
        LOGF("Unable to load mesh");
    }
}

void VulkanInit(platform_api_t *api, char const* const* instanceExtensions, u32 instanceExtensionCount, memory_arena_t *scratchArena)
{
    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Levent",
        .apiVersion = VK_API_VERSION_1_3
    };
    
    const char *validationLayerName = "VK_LAYER_KHRONOS_validation";

    VkInstanceCreateInfo instanceCI = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = instanceExtensionCount,
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = &validationLayerName,
        .ppEnabledExtensionNames = instanceExtensions,
        .pApplicationInfo = &appInfo
    };

    VK_CHECK(vkCreateInstance(&instanceCI, NULL, &gInstance));

    volkLoadInstance(gInstance);

    u32 deviceCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(gInstance, &deviceCount, NULL));

    VkPhysicalDevice physicalDevices[8];
    VK_CHECK(vkEnumeratePhysicalDevices(gInstance, &deviceCount, physicalDevices));
    gPhysicalDevice = physicalDevices[0];

    VkPhysicalDeviceProperties2 deviceProperties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    };

    vkGetPhysicalDeviceProperties2(gPhysicalDevice, &deviceProperties);

    u32 queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gPhysicalDevice, &queueFamilyCount, NULL);

    VkQueueFamilyProperties queueFamilyProperties[8];
    vkGetPhysicalDeviceQueueFamilyProperties(gPhysicalDevice, &queueFamilyCount, queueFamilyProperties);

    for  (u32 i = 0; i < queueFamilyCount; i++) {
        if (queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){
            printf("Selecting queue family %u\n", i);
            gQueueFamily = i;
            break;
        }  
    }

    LV_ASSERT(api->VulkanGetPresentationSupport(gInstance, gPhysicalDevice, gQueueFamily) && "Unable to query vulkan presentation support");

    const f32 priorities = 1.0f;

    VkDeviceQueueCreateInfo queueCI = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gQueueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priorities
    };

    const char *deviceExtensions[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features features12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .descriptorIndexing = true,
        .shaderSampledImageArrayNonUniformIndexing = true,
        .descriptorBindingVariableDescriptorCount = true,
        .runtimeDescriptorArray = true,
        .bufferDeviceAddress = true,
        .drawIndirectCount = true,
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

    VK_CHECK(vkCreateDevice(gPhysicalDevice, &deviceCI, NULL, &gDevice));

    volkLoadDevice(gDevice);
    vkGetDeviceQueue(gDevice, gQueueFamily, 0, &gQueue);

    VmaVulkanFunctions vkFunctions = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        .vkCreateImage = vkCreateImage
    };

    VmaAllocatorCreateInfo allocatorCI = {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = gPhysicalDevice,
        .device = gDevice,
        .pVulkanFunctions = &vkFunctions,
        .instance = gInstance
    };

    VK_CHECK(vmaCreateAllocator(&allocatorCI, &gAllocator));

    LV_ASSERT(api->CreateWindow(&gWindow, gInstance, "HowToVulkan", 1280U, 720U, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE));

    VkSurfaceCapabilitiesKHR surfaceCaps = {0};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gPhysicalDevice, gWindow.surface, &surfaceCaps));

    VkExtent2D swapchainExtent = surfaceCaps.currentExtent;
    if (swapchainExtent.width == 0xFFFFFFFF) {
        swapchainExtent.width = gWindow.w;
        swapchainExtent.height = gWindow.h;
    }

    VkFormat imageFormat =  VK_FORMAT_B8G8R8A8_SRGB;
    VkSwapchainCreateInfoKHR swapchainCI = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = gWindow.surface,
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

    VK_CHECK(vkCreateSwapchainKHR(gDevice, &swapchainCI, NULL, &gSwapchain));

    VkImage swapchainImages[32];
    u32 swapchainImageCount;
    VK_CHECK(vkGetSwapchainImagesKHR(gDevice, gSwapchain, &swapchainImageCount, NULL));
    LV_ASSERT(swapchainImageCount <= MAX_SWAPCHAIN_IMAGES);
    VK_CHECK(vkGetSwapchainImagesKHR(gDevice, gSwapchain, &swapchainImageCount, swapchainImages));

    for (u32 i = 0; i < swapchainImageCount; i++) {
        gSwapchainImages[i].image = swapchainImages[i];
        gSwapchainImages[i].view = CreateImageView(gDevice, gSwapchainImages[i].image, imageFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        gSwapchainImages[i].allocation = NULL;
        gSwapchainImages[i].format = imageFormat;
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

        vkGetPhysicalDeviceFormatProperties2(gPhysicalDevice, depthFormatList[i], &formatProperties);
        if (formatProperties.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){
            depthFormat = depthFormatList[i];
            break;
        }
    }

    LV_ASSERT(depthFormat != VK_FORMAT_UNDEFINED);
    
    VmaAllocation depthAllocation;
    gDepthImage.image = CreateImage(gDevice, gAllocator, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, gWindow.w, gWindow.h, 1, &depthAllocation);
    gDepthImage.view = CreateImageView(gDevice, gDepthImage.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    gDepthImage.format = depthFormat;
    gDepthImage.allocation = depthAllocation;

    //launch job to load geometry
    JobSystemPushJob(GeomLoadFunc, &geometry);
    shader_t *shaders = NULL;
    JobSystemPushJob(LoadShaders, &shaders);
    JobSystemWaitForAllJobs();

    VkDeviceSize vBufSize = sizeof(Vertex) * ArrayCount(geometry.vertices);
    CreateBuffer(&gVertexBuffer, 
        gDevice,
        vBufSize, 
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        gAllocator);
    UploadBuffer(&gVertexBuffer, geometry.vertices, vBufSize, 0);

    VkDeviceSize iBufSize = sizeof(u32) * ArrayCount(geometry.indices);
    CreateBuffer(&gIndexBuffer, 
        gDevice,
        iBufSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        gAllocator);
    UploadBuffer(&gIndexBuffer, geometry.indices, iBufSize, 0);

    geometry.indexCount = ArrayCount(geometry.indices);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(&gShaderGlobalsBuffers[i], 
            gDevice,
            sizeof(globals_t), 
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 
            VMA_MEMORY_USAGE_AUTO, 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            gAllocator);
        gShaderData[i].globalsAddress = gShaderGlobalsBuffers[i].deviceAddress;
    }

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        CreateBuffer(&gDrawCommandBuffers[i], 
            gDevice,
            3 * sizeof(VkDrawIndexedIndirectCommand), 
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, 
            VMA_MEMORY_USAGE_AUTO, 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            gAllocator);
    }

    CreateBuffer(&gDrawCommandCountBuffer, 
        gDevice,
        sizeof(u32), 
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, 
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        gAllocator
    );

    CreateBuffer(&gScratchBuffer, 
        gDevice,
        128 * 1024 * 1024, 
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        gAllocator);

    VkSemaphoreCreateInfo semaphoreCI = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    VkFenceCreateInfo fenceCI = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkCreateFence(gDevice, &fenceCI, NULL, &gFences[i]));
        VK_CHECK(vkCreateSemaphore(gDevice, &semaphoreCI, NULL, &gPresentSemaphores[i]));
    }

    for (u32 i = 0; i < swapchainImageCount; i++) {
        VK_CHECK(vkCreateSemaphore(gDevice, &semaphoreCI, NULL, &gRenderSemaphores[i]));
    }

    VkCommandPoolCreateInfo commandPoolCI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gQueueFamily
    };

    VK_CHECK(vkCreateCommandPool(gDevice, &commandPoolCI, NULL, &gCommandPool));
 
    VkCommandBufferAllocateInfo cbAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = gCommandPool,
        .commandBufferCount = MAX_FRAMES_IN_FLIGHT
    };

    VK_CHECK(vkAllocateCommandBuffers(gDevice, &cbAllocInfo, gCommandBuffers));

    for (u32 i = 0; i < ARRAY_SIZE(gTextures); i++) {
        const char * texPath = ArenaPrintf(scratchArena, "../assets/suzanne%u.ktx", i);
        CreateTexture(&gTextures[i], &gTextureDescriptors[i], &gScratchBuffer, gDevice, gAllocator, gCommandPool, gQueue, texPath);
    }

    VkDescriptorPoolSize poolSize = {
        .type =  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = ARRAY_SIZE(gTextures),
    };

    VkDescriptorPoolCreateInfo descPoolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };

    VK_CHECK(vkCreateDescriptorPool(gDevice, &descPoolCI, NULL, &gDescriptorPool));

    int rc = system("ninja compile_shaders");
    if (rc != 0) {
        printf("Unable to compile shaders\n");
    }

    gTexLayout = CreateDescriptorSetLayout(gDevice, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, ARRAY_SIZE(gTextures));

    CreateGraphicsPipeline(&gPipeline, shaders, gDevice, "shader.spv", imageFormat, depthFormat, sizeof(shader_data_t), gTexLayout);
    CreateComputePipeline(&gComputePipeline, shaders, gDevice, "compute_shader.spv", sizeof(shader_data_t));

    u32 variableDescCount = ARRAY_SIZE(gTextures);
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &variableDescCount,
    };

    VkDescriptorSetAllocateInfo texDescSetAlloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variableDescCountAI,
        .descriptorPool = gDescriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &gTexLayout,
    };

    VK_CHECK(vkAllocateDescriptorSets(gDevice, &texDescSetAlloc, &gDescriptorSetTex));

    VkWriteDescriptorSet writeDescSet = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = gDescriptorSetTex,
        .dstBinding = 0,
        .descriptorCount = ARRAY_SIZE(gTextures),
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = gTextureDescriptors,
    };

    vkUpdateDescriptorSets(gDevice, 1, &writeDescSet, 0, NULL);
}

LV_EXPORT void Init(game_memory_t *gameMemory)
{
    MemoryInit(gameMemory);
    JobSystemInit();
    StringInterningInit(); 

    volkInitialize();
    VulkanInit(&gameMemory->api, gameMemory->vulkanInstanceExtensions, gameMemory->vulkanInstanceExtensionCount, ScratchArena(0));
}

LV_EXPORT void Update(game_input_t *gameInput)
{
    for (u32 i = 0; i < SCANCODE_COUNT; i++) {
        if (gameInput->keyEvents[i].event) {
            if (gameInput->keyEvents[i].down) {
                LOGI("Button %u was pressed", i);
            } else {
                LOGI("Button %u was released", i);
            }

            if (gameInput->keyEvents[i].repeat) {
                LOGI("Button %u was repeated", i);
            }
        }
    }
    gWindowResized = gameInput->windowResized;
}

LV_EXPORT void Render(void)
{
    //reset all arenas
    MemoryReset();

    VK_CHECK(vkWaitForFences(gDevice, 1, &gFences[gFrameIndex], true, UINT64_MAX));
    VK_CHECK(vkResetFences(gDevice, 1, &gFences[gFrameIndex]));

    u32 imageIndex;
    VK_CHECK(vkAcquireNextImageKHR(gDevice, gSwapchain, UINT64_MAX, gPresentSemaphores[gFrameIndex], VK_NULL_HANDLE, &imageIndex));
    
    globals_t globals = {0};
    globals.projection = HMM_Perspective_LH_ZO(HMM_AngleDeg(60.0f), (f32)gWindow.w / (f32)gWindow.h, 0.1f, 32.0f);
    globals.view = HMM_LookAt_LH((vec3_t){0.0f, 0.0f, gCameraZ}, (vec3_t){0.0f, 0.0f, 0.0f}, (vec3_t){0.0f, -1.0f, 0.0f});
    for (u32 i = 0; i < 3; i++) {
        vec3_t instancePos = (vec3_t){((f32)(i) - 1.0f) * 3.0f, 0.0f, 0.0f};
        globals.model[i] = HMM_Translate(instancePos);
    }

    UploadBuffer(&gShaderGlobalsBuffers[gFrameIndex], &globals, sizeof(globals_t), 0);

    VkDrawIndexedIndirectCommand indirectDrawCommand = {
        .indexCount = geometry.indexCount,
        .instanceCount = 3,
        .firstIndex = 0,
        .vertexOffset = 0,
        .firstInstance = 0
    };

    UploadBuffer(&gDrawCommandBuffers[gFrameIndex], &indirectDrawCommand, sizeof(VkDrawIndexedIndirectCommand), 0);
    
    VkCommandBuffer cb = gCommandBuffers[gFrameIndex];

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

    vkCmdFillBuffer(cb, gDrawCommandCountBuffer.buffer, 0, sizeof(u32), 0);

    StageBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_2_MEMORY_WRITE_BIT, 
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
    );

    //color and depth image need barriers for layout transitions
    VkImageMemoryBarrier2 colorBarrier = ImageBarrier(gSwapchainImages[gFrameIndex].image,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, 1);
    
    VkImageMemoryBarrier2 depthBarrier = ImageBarrier(gDepthImage.image,
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
        .imageView = gSwapchainImages[imageIndex].view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {0.0f, 0.0f, 0.2f, 1.0f}},
    };

    VkRenderingAttachmentInfo depthAttachmentInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = gDepthImage.view,
        .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {.depthStencil = {1.0f, 0.0f}},
    };

    VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea.extent.width = gWindow.w,
        .renderArea.extent.height = gWindow.h,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentInfo,
        .pDepthAttachment = &depthAttachmentInfo,
    };

    //dispatch compute shader here
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gComputePipeline.pipeline);
    vkCmdPushConstants(cb, gComputePipeline.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shader_data_t), &gDrawCommandCountBuffer.deviceAddress);
    vkCmdDispatch(cb, 1,1,1);
    
    // Synchronize compute shader write with draw indirect read
    StageBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
        VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 
        VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    
    vkCmdBeginRendering(cb, &renderingInfo);

    VkViewport vp = {
        .width = (f32)gWindow.w,
        .height = (f32)gWindow.h,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor = {
        .extent.width =  gWindow.w,
        .extent.height = gWindow.h,
    };

    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gPipeline.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gPipeline.pipelineLayout, 0, 1, &gDescriptorSetTex, 0, NULL);

    VkDeviceSize vOffset = 0;        
    vkCmdBindVertexBuffers(cb, 0, 1, &gVertexBuffer.buffer, &vOffset);
    vkCmdBindIndexBuffer(cb, gIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cb, gPipeline.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(shader_data_t), &gShaderData[gFrameIndex]);

    vkCmdDrawIndexedIndirectCount(cb, gDrawCommandBuffers[gFrameIndex].buffer, 0, gDrawCommandCountBuffer.buffer, 0, 3, sizeof(VkDrawIndexedIndirectCommand));
    vkCmdEndRendering(cb);

    VkImageMemoryBarrier2 barrierPresent = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .image = gSwapchainImages[imageIndex].image,
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
        .pWaitSemaphores = &gPresentSemaphores[gFrameIndex],
        .pWaitDstStageMask = &waitStages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &gRenderSemaphores[imageIndex],
    };

    VK_CHECK(vkQueueSubmit(gQueue, 1, &submitInfo, gFences[gFrameIndex]));

    gFrameIndex = (gFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &gRenderSemaphores[imageIndex],
        .swapchainCount = 1,
        .pSwapchains = &gSwapchain,
        .pImageIndices = &imageIndex,
    };

    VK_CHECK(vkQueuePresentKHR(gQueue, &presentInfo));

    if (gWindowResized) {
        if (SDL_GetWindowSize(gWindow.window, &gWindow.w, &gWindow.h)) {
            printf("Window resized: %ux%u\n", gWindow.w, gWindow.h);
        }
        gWindowResized = false;

        VK_CHECK(vkDeviceWaitIdle(gDevice));
    
        VkSurfaceCapabilitiesKHR surfaceCaps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gPhysicalDevice, gWindow.surface, &surfaceCaps));

        VkSwapchainCreateInfoKHR swapchainCI = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = gWindow.surface,
            .minImageCount = surfaceCaps.minImageCount,
            .imageFormat = gSwapchainImages[0].format,
            .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
            .imageExtent.width = gWindow.w,
            .imageExtent.height = gWindow.h,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .oldSwapchain = gSwapchain,
        };

        VK_CHECK(vkCreateSwapchainKHR(gDevice, &swapchainCI, NULL, &gSwapchain));

        for (u32 i = 0; i < gSwapchainImageCount; i++) {
            vkDestroyImageView(gDevice, gSwapchainImages[i].view, NULL);
        }

        VkImage images[MAX_SWAPCHAIN_IMAGES];
        VK_CHECK(vkGetSwapchainImagesKHR(gDevice, gSwapchain, &gSwapchainImageCount, images));
        LV_ASSERT(gSwapchainImageCount <= MAX_SWAPCHAIN_IMAGES);

        VkFormat format = gSwapchainImages[0].format;
        for (u32 i = 0; i < gSwapchainImageCount; i++) {
            gSwapchainImages[i].image = images[i];
            gSwapchainImages[i].view = CreateImageView(gDevice, images[i], format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        }

        vkDestroySwapchainKHR(gDevice, swapchainCI.oldSwapchain, NULL);
        vmaDestroyImage(gAllocator, gDepthImage.image, gDepthImage.allocation);
        vkDestroyImageView(gDevice, gDepthImage.view, NULL);

        gDepthImage.image = CreateImage(gDevice, gAllocator, gDepthImage.format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, gWindow.w, gWindow.h, 1, &gDepthImage.allocation);
        gDepthImage.view = CreateImageView(gDevice, gDepthImage.image, gDepthImage.format, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    }
}

void Shutdown(void)
{
    VK_CHECK(vkDeviceWaitIdle(gDevice));

    for (u32 i = 0; i < ARRAY_SIZE(gTextures); i++) {
        DestroyTexture(&gTextures[i], gAllocator, gDevice);
    }

    DestroyGraphicsPipeline(&gPipeline, gDevice);
    DestroyComputePipeline(&gComputePipeline, gDevice);

    vkDestroyDescriptorSetLayout(gDevice, gTexLayout, NULL);
    vkDestroyDescriptorPool(gDevice, gDescriptorPool, NULL);
    vkDestroyCommandPool(gDevice, gCommandPool, NULL);

    DestroyBuffer(&gScratchBuffer, gAllocator);
    DestroyBuffer(&gDrawCommandCountBuffer, gAllocator);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        DestroyBuffer(&gShaderGlobalsBuffers[i], gAllocator);
        DestroyBuffer(&gDrawCommandBuffers[i], gAllocator);
    }

    DestroyBuffer(&gVertexBuffer, gAllocator);
    DestroyBuffer(&gVertexBuffer, gAllocator);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroyFence(gDevice, gFences[i], NULL);
        vkDestroySemaphore(gDevice, gPresentSemaphores[i], NULL);
    }

    for (u32 i = 0; i < gSwapchainImageCount; i++) {
        vkDestroyImageView(gDevice, gSwapchainImages[i].view, NULL);
        vkDestroySemaphore(gDevice, gRenderSemaphores[i], NULL);
    }

    vkDestroyImageView(gDevice, gDepthImage.view, NULL);
    vmaDestroyImage(gAllocator, gDepthImage.image, gDepthImage.allocation);

    vkDestroySwapchainKHR(gDevice, gSwapchain, NULL);
    SDL_Vulkan_DestroySurface(gInstance, gWindow.surface, NULL);
    SDL_DestroyWindow(gWindow.window);

    vmaDestroyAllocator(gAllocator);

    vkDestroyDevice(gDevice, NULL);
    vkDestroyInstance(gInstance, NULL);

    volkFinalize();

    JobSystemWaitForAllJobs();
    JobSystemDeinit();
    StringInterningDeinit();
    MemoryDeinit();

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();

    LOGI("All is  well\n");
}

