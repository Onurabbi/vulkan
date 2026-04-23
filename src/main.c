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

typedef struct {
    Vec3 center;
    float radius;
    
    uint32_t vertexOffset;
    uint32_t vertexCount;
} Mesh;

typedef struct {
    Vec3 position;
    float scale;
    Quat orientation;
    uint32_t meshIndex;
} DrawData;

typedef struct {
    Vertex *vertices;
    uint32_t *indices;
    Mesh *meshes;
    u32 indexCount;//required for drawing
}Geometry;

VkDevice device;
VkInstance instance;
VkPhysicalDevice physicalDevice;
 
VkSwapchainKHR swapchain;
VkFormat swapchainImageFormat = VK_FORMAT_B8G8R8A8_SRGB;
VkImage swapchainImages[3];
VkImageView swapchainImageViews[3];
uint32_t swapchainImageCount;

VkImage depthImage;
VkImageView depthImageView;
VkFormat depthImageFormat = VK_FORMAT_UNDEFINED;
VmaAllocation depthImageAllocation;

Buffer vBuffer;
Buffer iBuffer;
Buffer shaderGlobalsBuffers[2];
Buffer drawCommandBuffers[2];
Buffer drawCommandCountBuffer;
Buffer scratch;

uint32_t queueFamily;
SDL_Window* window;
VkSurfaceKHR surface;
VmaAllocator allocator;

const uint32_t maxFramesInFlight = 2;
VkCommandBuffer commandBuffers[2];

Texture textures[3];//swapchain image count
VkDescriptorImageInfo textureDescriptors[3];
VkDescriptorSetLayout texLayout;
VkDescriptorPool descriptorPool;
VkDescriptorSet descriptorSetTex;

VkFence fences[2];
VkSemaphore presentSemaphores[2];//max frames in flight
VkSemaphore renderSemaphores[3]; //swapchain image count
VkCommandPool commandPool;
VkCommandBuffer commandBuffers[2];//max frames in flight

int windowWidth,windowHeight;

ShaderData shaderData[3];
ShaderData computeData;

float cameraZ = 6.0f;

void AppendMesh(Geometry *geom, Vertex *vertices, uint32_t *indices)
{
    Mesh mesh = {0};
    mesh.vertexOffset = ArrayCount(geom->vertices);
    mesh.vertexCount = ArrayCount(vertices);

    ArrayPushArray(geom->vertices, vertices, ArrayCount(vertices));
    ArrayPushArray(geom->indices, indices, ArrayCount(indices));

    Vec3 center =  {0};
    for (uint32_t i = 0; i < ArrayCount(vertices); i++) {
        HMM_Add(center, vertices[i].p);
    }
    HMM_Div(center, (float)ArrayCount(vertices));

    float radius = 0.0f;
    for (uint32_t i = 0; i < ArrayCount(vertices); i++) {
        radius = MAX(radius, HMM_Len(HMM_Sub(vertices[i].p, center)));
    }
    mesh.center = center;
    mesh.radius = radius;

    ArrayPush(geom->meshes, mesh);
}

VkImageMemoryBarrier2 ImageBarrier(VkImage image, VkPipelineStageFlags2 srcStageMask, VkAccessFlags2 srcAccessMask, VkImageLayout oldLayout, VkPipelineStageFlags2 dstStageMask, VkAccessFlags2 dstAccessMask, VkImageLayout newLayout, VkImageAspectFlags aspectMask, uint32_t baseMipLevel, uint32_t levelCount)
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
        uint32_t index_count = 0;
        for (uint32_t i = 0; i < obj->face_count; i++) {
            index_count += 3 * (obj->face_vertices[i] - 2);
        }

        ArrayInitWithArena(vertices, arena, index_count);
        ArrayResize(vertices, index_count);

        uint32_t vertex_offset = 0;
        uint32_t index_offset = 0;

        for (uint32_t i = 0; i < obj->face_count; i++) {
            for (uint32_t j = 0; j < obj->face_vertices[i]; j++) {
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

bool LoadMesh(Geometry *geom, const char *path, memory_arena_t *arena)
{
    Vertex *vertices = NULL;
    if (!LoadObj(&vertices, path, arena)) {
        printf("Unable to load mesh from path: %s\n", path);
        return false;
    }

    uint32_t *indices = NULL;
    ArrayInitWithArena(indices, arena, ArrayCount(vertices));
    ArrayResize(indices, ArrayCount(vertices));

    for (uint32_t i = 0; i < ArrayCount(indices); i++) {
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

int main(int argc, char *argv[])
{
    MemoryInit();
    JobSystemInit();
    StringInterningInit(STRING_ARENA_CAPACITY, MAX_STRING_COUNT); 

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("Unable to init SDL. SDL Error: %s\n", SDL_GetError());
        return -1;
    }

    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        printf("Unable to load vulkan loader library.\n");
        return -1;
    }

    volkInitialize();

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Levent",
        .apiVersion = VK_API_VERSION_1_3
    };

    uint32_t instanceExtensionCount = 0;
    char const* const* instanceExtensions = SDL_Vulkan_GetInstanceExtensions(&instanceExtensionCount);
    const char *validationLayerName = "VK_LAYER_KHRONOS_validation";

    VkInstanceCreateInfo instanceCI = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = instanceExtensionCount,
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = &validationLayerName,
        .ppEnabledExtensionNames = instanceExtensions,
        .pApplicationInfo = &appInfo
    };

    VK_CHECK(vkCreateInstance(&instanceCI, NULL, &instance));

    volkLoadInstance(instance);

    //launch job to load geometry
    Geometry geometry = {0};
    JobSystemPushJob(GeomLoadFunc, &geometry);
    Shader *shaders = NULL;
    JobSystemPushJob(LoadShaders, &shaders);
    JobSystemWaitForAllJobs();

    uint32_t deviceCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &deviceCount, NULL));

    VkPhysicalDevice physicalDevices[8];
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices));

    physicalDevice = physicalDevices[0];

    VkPhysicalDeviceProperties2 deviceProperties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    };

    vkGetPhysicalDeviceProperties2(physicalDevice, &deviceProperties);
    printf("selected device: %s\n", deviceProperties.properties.deviceName);

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, NULL);

    VkQueueFamilyProperties queueFamilyProperties[8];
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyProperties);

    queueFamily = 0;

    for  (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){
            printf("Selecting queue family %u\n", i);
            queueFamily = i;
            break;
        }  
    }

    if(!SDL_Vulkan_GetPresentationSupport(instance, physicalDevice, queueFamily)) {
        printf("Need to select another queue\n");
        return -1;
    }

    const float priorities = 1.0f;

    VkDeviceQueueCreateInfo queueCI = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily,
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

    VK_CHECK(vkCreateDevice(physicalDevice, &deviceCI, NULL, &device));

    volkLoadDevice(device);
    
    VkQueue queue;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VmaVulkanFunctions vkFunctions = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        .vkCreateImage = vkCreateImage
    };

    VmaAllocatorCreateInfo allocatorCI = {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = physicalDevice,
        .device = device,
        .pVulkanFunctions = &vkFunctions,
        .instance = instance
    };

    VK_CHECK(vmaCreateAllocator(&allocatorCI, &allocator));

    window = SDL_CreateWindow("How to Vulkan", 1280U, 720U, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    VkSurfaceKHR surface;

    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)) {
        printf("Unable to create vulkan surface. SDL Error: %s\n", SDL_GetError());
        return -1;
    }

    VkSurfaceCapabilitiesKHR surfaceCaps = {0};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &surfaceCaps));

    SDL_GetWindowSize(window, &windowWidth,  &windowHeight);

    VkExtent2D swapchainExtent = surfaceCaps.currentExtent;
    if (swapchainExtent.width == 0xFFFFFFFF) {
        swapchainExtent.width = windowWidth;
        swapchainExtent.height = windowHeight;
    }

    VkFormat imageFormat =  VK_FORMAT_B8G8R8A8_SRGB;
    VkSwapchainCreateInfoKHR swapchainCI = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = surfaceCaps.minImageCount,
        .imageFormat = swapchainImageFormat,
        .imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
        .imageExtent.width = swapchainExtent.width,
        .imageExtent.height = swapchainExtent.height,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
    };

    VkSwapchainKHR swapchain;
    VK_CHECK(vkCreateSwapchainKHR(device, &swapchainCI, NULL, &swapchain));

    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, NULL));
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages));

    for (uint32_t i = 0; i < swapchainImageCount; i++) {
        swapchainImageViews[i] = CreateImageView(device, swapchainImages[i], swapchainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    }
 
    VkFormat depthFormatList[2] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };

    for (uint32_t i = 0; i < ARRAY_SIZE(depthFormatList); i++) {
        VkFormatProperties2 formatProperties = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        };

        vkGetPhysicalDeviceFormatProperties2(physicalDevice, depthFormatList[i], &formatProperties);
        if (formatProperties.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){
            depthImageFormat = depthFormatList[i];
            break;
        }
    }

    LV_ASSERT(depthImageFormat != VK_FORMAT_UNDEFINED);

    depthImage = CreateImage(device, allocator, depthImageFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, windowWidth, windowHeight, 1, &depthImageAllocation);
    depthImageView = CreateImageView(device, depthImage, depthImageFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);

    VkDeviceSize vBufSize = sizeof(Vertex) * ArrayCount(geometry.vertices);
    CreateBuffer(&vBuffer, 
        device,
        vBufSize, 
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        allocator);
    UploadBuffer(&vBuffer, geometry.vertices, vBufSize, 0);

    VkDeviceSize iBufSize = sizeof(uint32_t) * ArrayCount(geometry.indices);
    CreateBuffer(&iBuffer, 
        device,
        iBufSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        allocator);
    UploadBuffer(&iBuffer, geometry.indices, iBufSize, 0);

    geometry.indexCount = ArrayCount(geometry.indices);

    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        CreateBuffer(&shaderGlobalsBuffers[i], 
            device,
            sizeof(Globals), 
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 
            VMA_MEMORY_USAGE_AUTO, 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            allocator);
        shaderData[i].globalsAddress = shaderGlobalsBuffers[i].deviceAddress;
    }

    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        CreateBuffer(&drawCommandBuffers[i], 
            device,
            3 * sizeof(VkDrawIndexedIndirectCommand), 
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, 
            VMA_MEMORY_USAGE_AUTO, 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            allocator);
    }

    CreateBuffer(&drawCommandCountBuffer, 
        device,
        sizeof(uint32_t), 
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, 
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
        VMA_ALLOCATION_CREATE_MAPPED_BIT,
        allocator
    );

    CreateBuffer(&scratch, 
        device,
        128 * 1024 * 1024, //massive textures
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        allocator);

    VkSemaphoreCreateInfo semaphoreCI = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    VkFenceCreateInfo fenceCI = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        VK_CHECK(vkCreateFence(device, &fenceCI, NULL, &fences[i]));
        VK_CHECK(vkCreateSemaphore(device, &semaphoreCI, NULL, &presentSemaphores[i]));
    }

    for (uint32_t i = 0; i < swapchainImageCount; i++) {
        VK_CHECK(vkCreateSemaphore(device, &semaphoreCI, NULL, &renderSemaphores[i]));
    }

    VkCommandPoolCreateInfo commandPoolCI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily
    };

    VK_CHECK(vkCreateCommandPool(device, &commandPoolCI, NULL, &commandPool));
 
    VkCommandBufferAllocateInfo cbAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .commandBufferCount = maxFramesInFlight
    };

    VK_CHECK(vkAllocateCommandBuffers(device, &cbAllocInfo, commandBuffers));

    for (uint32_t i = 0; i < ARRAY_SIZE(textures); i++) {
        char buf[256];
        sprintf(buf, "../assets/suzanne%u.ktx", i);
        CreateTexture(&textures[i], &textureDescriptors[i], &scratch,device, allocator, commandPool, queue, buf);
    }

    VkDescriptorPoolSize poolSize = {
        .type =  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = ARRAY_SIZE(textures),
    };

    VkDescriptorPoolCreateInfo descPoolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };

    VK_CHECK(vkCreateDescriptorPool(device, &descPoolCI, NULL, &descriptorPool));
    int rc = system("ninja compile_shaders");
    if (rc != 0) {
        printf("Unable to compile shaders\n");
    }

    texLayout = CreateDescriptorSetLayout(device, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, ARRAY_SIZE(textures));

    Pipeline pipeline = {0};
    CreateGraphicsPipeline(&pipeline, shaders, device, "shader.spv", swapchainImageFormat, depthImageFormat, sizeof(ShaderData), texLayout);
    Pipeline computePipeline = {0};
    CreateComputePipeline(&computePipeline, shaders, device, "compute_shader.spv", sizeof(ShaderData));

    uint32_t variableDescCount = ARRAY_SIZE(textures);
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &variableDescCount,
    };

    VkDescriptorSetAllocateInfo texDescSetAlloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variableDescCountAI,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &texLayout,
    };

    VK_CHECK(vkAllocateDescriptorSets(device, &texDescSetAlloc, &descriptorSetTex));

    VkWriteDescriptorSet writeDescSet = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptorSetTex,
        .dstBinding = 0,
        .descriptorCount = ARRAY_SIZE(textures),
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = textureDescriptors,
    };

    vkUpdateDescriptorSets(device, 1, &writeDescSet, 0, NULL);

    uint64_t lastTime = SDL_GetTicks();
    uint32_t frameIndex = 0;
    bool quit = false;

    while(!quit) {
        //reset all arenas
        MemoryReset();

        VK_CHECK(vkWaitForFences(device, 1, &fences[frameIndex], true, UINT64_MAX));
        VK_CHECK(vkResetFences(device, 1, &fences[frameIndex]));

        uint32_t imageIndex;
        VK_CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, presentSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex));
        
        Globals globals = {0};

        globals.projection = HMM_Perspective_LH_ZO(HMM_AngleDeg(60.0f), (float)windowWidth / (float)windowHeight, 0.1f, 32.0f);
        globals.view = HMM_LookAt_LH((Vec3){0.0f, 0.0f, cameraZ}, (Vec3){0.0f, 0.0f, 0.0f}, (Vec3){0.0f, -1.0f, 0.0f});
        for (uint32_t i = 0; i < 3; i++) {
            Vec3 instancePos = (Vec3){((float)(i) - 1.0f) * 3.0f, 0.0f, 0.0f};
            globals.model[i] = HMM_Translate(instancePos);
        }

        UploadBuffer(&shaderGlobalsBuffers[frameIndex], &globals, sizeof(Globals), 0);

        VkDrawIndexedIndirectCommand indirectDrawCommand = {
            .indexCount = geometry.indexCount,
            .instanceCount = 3,
            .firstIndex = 0,
            .vertexOffset = 0,
            .firstInstance = 0
        };

        UploadBuffer(&drawCommandBuffers[frameIndex], &indirectDrawCommand, sizeof(VkDrawIndexedIndirectCommand), 0);
        
        VkCommandBuffer cb = commandBuffers[frameIndex];
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

        vkCmdFillBuffer(cb, drawCommandCountBuffer.buffer, 0, sizeof(uint32_t), 0);

        StageBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT, 
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
        );

        //color and depth image need barriers for layout transitions
        VkImageMemoryBarrier2 colorBarrier = ImageBarrier(swapchainImages[imageIndex],
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT,
            0, 1);
        
        VkImageMemoryBarrier2 depthBarrier = ImageBarrier(depthImage,
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
            .imageView = swapchainImageViews[imageIndex],
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {0.0f, 0.0f, 0.2f, 1.0f}},
        };

        VkRenderingAttachmentInfo depthAttachmentInfo = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depthImageView,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = {.depthStencil = {1.0f, 0.0f}},
        };

        VkRenderingInfo renderingInfo = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea.extent.width = windowWidth,
            .renderArea.extent.height = windowHeight,
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachmentInfo,
            .pDepthAttachment = &depthAttachmentInfo,
        };

        //dispatch compute shader here
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline.pipeline);
        vkCmdPushConstants(cb, computePipeline.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ShaderData), &drawCommandCountBuffer.deviceAddress);
        vkCmdDispatch(cb, 1,1,1);
        
        // Synchronize compute shader write with draw indirect read
        StageBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 
            VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        
        vkCmdBeginRendering(cb, &renderingInfo);

        VkViewport vp = {
            .width = (float)windowWidth,
            .height = (float)windowHeight,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        vkCmdSetViewport(cb, 0, 1, &vp);
        VkRect2D scissor = {
            .extent.width =  windowWidth,
            .extent.height = windowHeight,
        };

        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

        VkDeviceSize vOffset = 0;        
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipelineLayout, 0, 1, &descriptorSetTex, 0, NULL);
        vkCmdBindVertexBuffers(cb, 0, 1, &vBuffer.buffer, &vOffset);
        vkCmdBindIndexBuffer(cb, iBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdPushConstants(cb, pipeline.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShaderData), &shaderData[frameIndex]);

        vkCmdDrawIndexedIndirectCount(cb, drawCommandBuffers[frameIndex].buffer, 0, drawCommandCountBuffer.buffer, 0, 3,sizeof(VkDrawIndexedIndirectCommand));
        vkCmdEndRendering(cb);

        VkImageMemoryBarrier2 barrierPresent = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .image = swapchainImages[imageIndex],
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
            .pWaitSemaphores = &presentSemaphores[frameIndex],
            .pWaitDstStageMask = &waitStages,
            .commandBufferCount = 1,
            .pCommandBuffers = &cb,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &renderSemaphores[imageIndex],
        };

        VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, fences[frameIndex]));

        frameIndex = (frameIndex + 1) % maxFramesInFlight;

        VkPresentInfoKHR presentInfo = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &renderSemaphores[imageIndex],
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIndex,
        };

        VK_CHECK(vkQueuePresentKHR(queue, &presentInfo));

        float elapsedTime = (SDL_GetTicks() - lastTime) / 1000.0f;
        lastTime = SDL_GetTicks();

        bool windowResized = false;
        for (SDL_Event event; SDL_PollEvent(&event);) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = true;
            }
            if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                windowResized = true;
            }
        }
        
        if (windowResized) {
            if (SDL_GetWindowSize(window, &windowWidth, &windowHeight)) {
                printf("Window resized: %ux%u\n", windowWidth, windowHeight);
            }
            windowResized = false;

            VK_CHECK(vkDeviceWaitIdle(device));
            VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &surfaceCaps));
            swapchainCI.oldSwapchain = swapchain;
            swapchainCI.imageExtent.width = windowWidth;
            swapchainCI.imageExtent.height = windowHeight;
            VK_CHECK(vkCreateSwapchainKHR(device, &swapchainCI, NULL, &swapchain));

            for (uint32_t i = 0; i < swapchainImageCount; i++) {
                vkDestroyImageView(device, swapchainImageViews[i], NULL);
            }

            VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages));
            for (uint32_t i = 0; i < swapchainImageCount; i++) {
                swapchainImageViews[i] = CreateImageView(device, swapchainImages[i], imageFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            }   
            vkDestroySwapchainKHR(device, swapchainCI.oldSwapchain, NULL);
            vmaDestroyImage(allocator, depthImage, depthImageAllocation);
            vkDestroyImageView(device, depthImageView, NULL);

            depthImage = CreateImage(device, allocator, depthImageFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, windowWidth, windowHeight, 1, &depthImageAllocation);
            depthImageView = CreateImageView(device, depthImage, depthImageFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
        }
    }

    VK_CHECK(vkDeviceWaitIdle(device));

    for (uint32_t i = 0; i < ARRAY_SIZE(textures); i++) {
        DestroyTexture(&textures[i], allocator, device);
    }

    DestroyGraphicsPipeline(&pipeline, device);
    DestroyComputePipeline(&computePipeline, device);

    vkDestroyDescriptorSetLayout(device, texLayout, NULL);
    vkDestroyDescriptorPool(device, descriptorPool, NULL);
    vkDestroyCommandPool(device, commandPool, NULL);

    DestroyBuffer(&scratch, allocator);
    DestroyBuffer(&drawCommandCountBuffer, allocator);

    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        DestroyBuffer(&shaderGlobalsBuffers[i], allocator);
        DestroyBuffer(&drawCommandBuffers[i],allocator);
    }

    DestroyBuffer(&vBuffer, allocator);
    DestroyBuffer(&iBuffer, allocator);

    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        vkDestroyFence(device, fences[i], NULL);
        vkDestroySemaphore(device, presentSemaphores[i], NULL);
    }

    for (uint32_t i = 0; i < swapchainImageCount; i++) {
        vkDestroyImageView(device, swapchainImageViews[i], NULL);
        vkDestroySemaphore(device, renderSemaphores[i], NULL);
    }

    vkDestroyImageView(device, depthImageView, NULL);
    vmaDestroyImage(allocator, depthImage, depthImageAllocation);

    vkDestroySwapchainKHR(device, swapchain, NULL);
    SDL_Vulkan_DestroySurface(instance, surface, NULL);
    SDL_DestroyWindow(window);

    vmaDestroyAllocator(allocator);

    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    volkFinalize();

    JobSystemWaitForAllJobs();
    JobSystemDeinit();
    StringInterningDeinit();
    MemoryDeinit();

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();
    printf("All is  well\n");

    return 0;
}
