#define VOLK_IMPLEMENTATION
#include <vulkan/vulkan.h>
#include <volk/volk.h>

#define FAST_OBJ_IMPLEMENTATION
#include "fast_obj.h"

#define OG_DS_IMPLEMENTATION
#include "og_ds.h"

#include "HandmadeMath.h"

#include <stdio.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vma/vk_mem_alloc.h>

#include <ktx.h>
#include <ktxvulkan.h>

#include <assert.h>
#include <dlfcn.h>

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ARRAY_COUNT(arr) sizeof(arr)/(sizeof((arr)[0]))
#define VK_CHECK(expr) \
do { \
    if (expr != VK_SUCCESS) { \
        assert(false && #expr" returned a result other than VK_SUCCESS"); \
    } \
} while(0)

typedef HMM_Vec2 Vec2;
typedef HMM_Vec3 Vec3;
typedef HMM_Vec4 Vec4;
typedef HMM_Mat4 Mat4;
typedef HMM_Quat Quat;

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
    Vec3 p;
    Vec3 n;
    Vec2 t;
} Vertex;

typedef struct {
    Vertex *vertices;
    uint32_t *indices;
    Mesh *meshes;
}Geometry;

typedef struct {
    Mat4 projection;
    Mat4 view;
    Mat4 model[3];
    Vec4 lightPos;
    uint32_t selected;
}ShaderData;

typedef struct {
    VmaAllocation allocation;
    VmaAllocationInfo allocInfo;
    VkBuffer buffer;
    VkDeviceAddress deviceAddress;
}ShaderDataBuffer;

typedef struct {
    VmaAllocation allocation;
    VkImage image;
    VkImageView view;
    VkSampler sampler;
}Texture;

typedef struct {
    VmaAllocation allocation;
    VmaAllocationInfo allocInfo;
    VkBuffer buffer;
    VkDeviceAddress deviceAddress;
    VkDeviceSize size;
} Buffer;

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

uint32_t queueFamily;
SDL_Window* window;
VkSurfaceKHR surface;
VmaAllocator allocator;

const uint32_t maxFramesInFlight = 2;
VkCommandBuffer commandBuffers[2];
Buffer shaderDataBuffers[2];

Texture textures[3];//swapchain image count
VkDescriptorImageInfo textureDescriptors[3];
VkDescriptorSetLayout descSetLayoutTex;
VkDescriptorPool descriptorPool;
VkDescriptorSet descriptorSetTex;
VkShaderModule shaderModule;
VkPipelineLayout pipelineLayout;

VkFence fences[2];
VkSemaphore presentSemaphores[2];//max frames in flight
VkSemaphore renderSemaphores[3]; //swapchain image count
VkCommandPool commandPool;
VkCommandBuffer commandBuffers[2];//max frames in flight

int windowWidth,windowHeight;

Geometry geometry;
ShaderData shaderData;

float cameraZ = 6.0f;

VkPipeline pipeline;

void UploadBuffer(Buffer *buffer, const void *data, VkDeviceSize size, VkDeviceSize offset)
{
    memcpy((char*)buffer->allocInfo.pMappedData + offset, data, size);
}

void CreateBuffer(Buffer *buffer, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags)
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

void DestroyBuffer(Buffer *buffer)
{
    vmaDestroyBuffer(allocator, buffer->buffer, buffer->allocation);
}

void appendMesh(Geometry *geom, Vertex *vertices, uint32_t *indices)
{
    Mesh mesh = {0};
    mesh.vertexOffset = array_count(geom->vertices);
    mesh.vertexCount = array_count(vertices);

    array_push_array(geom->vertices, vertices, array_count(vertices));
    array_push_array(geom->indices, indices, array_count(indices));

    Vec3 center =  {0};
    for (uint32_t i = 0; i < array_count(vertices); i++) {
        HMM_Add(center, vertices[i].p);
    }
    HMM_Div(center, (float)array_count(vertices));

    float radius = 0.0f;
    for (uint32_t i = 0; i < array_count(vertices); i++) {
        radius = MAX(radius, HMM_Len(HMM_Sub(vertices[i].p, center)));
    }
    mesh.center = center;
    mesh.radius = radius;

    array_push(geom->meshes, mesh);
}

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkPipelineStageFlags2 srcStageMask, VkAccessFlags2 srcAccessMask, VkImageLayout oldLayout, VkPipelineStageFlags2 dstStageMask, VkAccessFlags2 dstAccessMask, VkImageLayout newLayout, VkImageAspectFlags aspectMask, uint32_t baseMipLevel, uint32_t levelCount)
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

VkBufferMemoryBarrier2 bufferBarrier(VkBuffer buffer, VkPipelineStageFlags2 srcStageMask, VkAccessFlags2 srcAccessMask, VkPipelineStageFlags2 dstStageMask, VkAccessFlags2 dstAccessMask)
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

void pipelineBarrier(VkCommandBuffer commandBuffer, VkDependencyFlags dependencyFlags, size_t bufferBarrierCount, const VkBufferMemoryBarrier2* bufferBarriers, size_t imageBarrierCount, const VkImageMemoryBarrier2* imageBarriers)
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

void stageBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 srcStageMask, VkAccessFlags2 srcAccessMask, VkPipelineStageFlags2 dstStageMask, VkAccessFlags2 dstAccessMask)
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
}

bool loadObj(Vertex **ppVertices, const char *path) 
{
    Vertex *vertices = NULL;
    fastObjMesh *obj = fast_obj_read("../assets/suzanne.obj");
    if (obj) {
        uint32_t index_count = 0;
        for (uint32_t i = 0; i < obj->face_count; i++) {
            index_count += 3 * (obj->face_vertices[i] - 2);
        }

        array_resize(vertices, index_count);

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
        assert(vertex_offset == index_count);
    }

    *ppVertices = vertices;

    fast_obj_destroy(obj);
    return true;
}

bool loadMesh(Geometry *geom, const char *path)
{
    Vertex *vertices = NULL;
    if (!loadObj(&vertices, path)) {
        printf("Unable to load mesh from path: %s\n", path);
        return false;
    }

    uint32_t *indices = NULL;
    array_resize(indices, array_count(vertices));
    for (uint32_t i = 0; i < array_count(indices); i++) {
        indices[i] = i;
    }

    appendMesh(geom, vertices, indices);

    array_free(vertices);
    array_free(indices);

    return true;
}

int main()
{
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
        .bufferDeviceAddress = true
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
        VkImageViewCreateInfo viewCI = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchainImages[i],
            .format = swapchainImageFormat,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = 1,
            .subresourceRange.layerCount = 1,
        };
        VK_CHECK(vkCreateImageView(device, &viewCI, NULL, &swapchainImageViews[i]));
    }
 
    VkFormat depthFormatList[2] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };

    for (uint32_t i = 0; i < ARRAY_COUNT(depthFormatList); i++) {
        VkFormatProperties2 formatProperties = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        };

        vkGetPhysicalDeviceFormatProperties2(physicalDevice, depthFormatList[i], &formatProperties);
        if (formatProperties.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){
            depthImageFormat = depthFormatList[i];
            break;
        }
    }

    assert(depthImageFormat != VK_FORMAT_UNDEFINED);

    VkImageCreateInfo depthImageCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = depthImageFormat,
        .extent.width = windowWidth,
        .extent.height = windowHeight,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VmaAllocationCreateInfo allocCI = {
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO
    };

    VK_CHECK(vmaCreateImage(allocator, &depthImageCI, &allocCI, &depthImage, &depthImageAllocation, NULL));

    VkImageViewCreateInfo depthViewCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = depthImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = depthImageFormat,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };

    VK_CHECK(vkCreateImageView(device, &depthViewCI, NULL, &depthImageView));

    if (!loadMesh(&geometry, "../assets/suzanne.obj")) {
        printf("Unable to load mesh\n");
        return -1;
    }

    VkDeviceSize vBufSize = sizeof(Vertex) * array_count(geometry.vertices);
    CreateBuffer(&vBuffer, vBufSize, 
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
        VMA_ALLOCATION_CREATE_MAPPED_BIT);

    UploadBuffer(&vBuffer, geometry.vertices, vBufSize, 0);

    VkDeviceSize iBufSize = sizeof(uint32_t) * array_count(geometry.indices);
    CreateBuffer(&iBuffer, iBufSize,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT);

    UploadBuffer(&iBuffer, geometry.indices, iBufSize, 0);

    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        CreateBuffer(&shaderDataBuffers[i], sizeof(ShaderData), 
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 
            VMA_MEMORY_USAGE_AUTO, 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | 
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

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

    for (uint32_t i = 0; i < ARRAY_COUNT(textures); i++) {
        ktxTexture *ktxTex = NULL;
        char buf[256];
        sprintf(buf, "../assets/suzanne%u.ktx", i);
        ktxTexture_CreateFromNamedFile(buf, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTex);
        assert(ktxTex);

        VkImageCreateInfo texImgCI = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = ktxTexture_GetVkFormat(ktxTex),
            .extent = {.width = ktxTex->baseWidth, .height = ktxTex->baseHeight, .depth = 1},
            .mipLevels = ktxTex->numLevels,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
        };

        VmaAllocationCreateInfo texImgAllocCI = {
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        VK_CHECK(vmaCreateImage(allocator,  &texImgCI, &texImgAllocCI, &textures[i].image, &textures[i].allocation, NULL));

        VkImageViewCreateInfo texViewCI = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = textures[i].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = texImgCI.format,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = ktxTex->numLevels,
            .subresourceRange.layerCount = ktxTex->numLayers,
        };

        VK_CHECK(vkCreateImageView(device, &texViewCI, NULL, &textures[i].view));

        Buffer imgSrcBuffer;
        CreateBuffer(&imgSrcBuffer, ktxTex->dataSize, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 
            VMA_MEMORY_USAGE_AUTO, 
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

        UploadBuffer(&imgSrcBuffer, ktxTex->pData, ktxTex->dataSize, 0);

        VkFenceCreateInfo fenceOneTimeCI = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };

        VkFence fenceOneTime;
        VK_CHECK(vkCreateFence(device, &fenceOneTimeCI, NULL, &fenceOneTime));
        VkCommandBufferAllocateInfo cbOneTimeAI = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commandPool,
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
            .image = textures[i].image,
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

        vkCmdCopyBufferToImage(cbOneTime, imgSrcBuffer.buffer, textures[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ktxTex->numLevels, copyRegions);

        VkImageMemoryBarrier2 barrierTexRead = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
            .image = textures[i].image,
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

        vkFreeCommandBuffers(device, commandPool, 1, &cbOneTime);
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

        VK_CHECK(vkCreateSampler(device, &samplerCI, NULL, &textures[i].sampler));

        textureDescriptors[i].sampler = textures[i].sampler;
        textureDescriptors[i].imageView = textures[i].view;
        textureDescriptors[i].imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;

        DestroyBuffer(&imgSrcBuffer);
        ktxTexture_Destroy(ktxTex);
    }

    VkDescriptorBindingFlags descVariableFlag = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo descBindingFlags = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 1,
        .pBindingFlags = &descVariableFlag,
    };

    VkDescriptorSetLayoutBinding descLayoutBindingTex = {
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = ARRAY_COUNT(textures),
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    };

    VkDescriptorSetLayoutCreateInfo descLayoutTexCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &descBindingFlags,
        .bindingCount = 1,
        .pBindings = &descLayoutBindingTex
    };

    VK_CHECK(vkCreateDescriptorSetLayout(device, &descLayoutTexCI, NULL, &descSetLayoutTex));

    VkDescriptorPoolSize poolSize = {
        .type =  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = ARRAY_COUNT(textures),
    };

    VkDescriptorPoolCreateInfo descPoolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };

    VK_CHECK(vkCreateDescriptorPool(device, &descPoolCI, NULL, &descriptorPool));

    uint32_t variableDescCount = ARRAY_COUNT(textures);
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
        .pSetLayouts = &descSetLayoutTex,
    };

    VK_CHECK(vkAllocateDescriptorSets(device, &texDescSetAlloc, &descriptorSetTex));

    VkWriteDescriptorSet writeDescSet = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptorSetTex,
        .dstBinding = 0,
        .descriptorCount = ARRAY_COUNT(textures),
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = textureDescriptors,
    };

    vkUpdateDescriptorSets(device, 1, &writeDescSet, 0, NULL);

    int rc = system("ninja compile_shaders");
    if (rc != 0) {
        printf("Unable to compile shaders\n");
    }

    {

        //read shader file
        FILE *file = fopen("./spirv/shader.spv", "rb");
        assert(file);

        fseek(file, 0, SEEK_END);
        size_t fileSize = ftell(file);
        uint8_t *fileBuf = malloc(fileSize);
        fseek(file, 0, SEEK_SET);
        fread(fileBuf, 1, fileSize, file);
        fclose(file);

        VkShaderModuleCreateInfo shaderModuleCI = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = fileSize,
            .pCode = (uint32_t*)fileBuf,
        };

        VK_CHECK(vkCreateShaderModule(device, &shaderModuleCI, NULL, &shaderModule));
        free(fileBuf);
    }

    //Pipeline
    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .size = sizeof(VkDeviceAddress),
    };

    VkPipelineLayoutCreateInfo pipelineLayoutCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descSetLayoutTex,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange
    };

    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutCI, NULL, &pipelineLayout));

    VkVertexInputBindingDescription vertexBinding = {
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };


    VkVertexInputAttributeDescription vertexAttributes[3] = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex,n)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex,t)}
    };

    VkPipelineVertexInputStateCreateInfo vertexInputState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertexBinding,
        .vertexAttributeDescriptionCount = ARRAY_COUNT(vertexAttributes),
        .pVertexAttributeDescriptions = vertexAttributes,
    };

    VkPipelineInputAssemblyStateCreateInfo vertexInputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    VkPipelineShaderStageCreateInfo shaderStages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = shaderModule,
            .pName = "main"
        },

        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = shaderModule,
            .pName = "main"
        }
    };

    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates
    };


    VkPipelineDepthStencilStateCreateInfo depthStencilState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
    };

    VkPipelineRenderingCreateInfo renderingCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &imageFormat,
        .depthAttachmentFormat = depthImageFormat
    };

    VkPipelineColorBlendAttachmentState blendAttachment = {
        .colorWriteMask = 0xF
    };

    VkPipelineColorBlendStateCreateInfo colorBlending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };

    VkPipelineRasterizationStateCreateInfo rasterizationState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisampleState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };

    VkGraphicsPipelineCreateInfo pipelineCI = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingCI,
        .stageCount = 2,
        .pStages = shaderStages,
        .pVertexInputState = &vertexInputState,
        .pInputAssemblyState = &vertexInputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizationState,
        .pMultisampleState = &multisampleState,
        .pDepthStencilState = &depthStencilState,
        .pColorBlendState = &colorBlending,
        .pDynamicState = &dynamicState,
        .layout = pipelineLayout
    };

    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, NULL, &pipeline));

    uint64_t lastTime = SDL_GetTicks();
    uint32_t frameIndex = 0;
    bool quit = false;

    while(!quit) {

        VK_CHECK(vkWaitForFences(device, 1, &fences[frameIndex], true, UINT64_MAX));
        VK_CHECK(vkResetFences(device, 1, &fences[frameIndex]));

        uint32_t imageIndex;
        VK_CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, presentSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex));
        shaderData.projection = HMM_Perspective_LH_ZO(HMM_AngleDeg(60.0f), (float)windowWidth / (float)windowHeight, 0.1f, 32.0f);
        shaderData.view = HMM_LookAt_LH((Vec3){0.0f, 0.0f, cameraZ}, (Vec3){0.0f, 0.0f, 0.0f}, (Vec3){0.0f, -1.0f, 0.0f});
        for (uint32_t i = 0; i < 3; i++) {
            Vec3 instancePos = (Vec3){((float)(i) - 1.0f) * 3.0f, 0.0f, 0.0f};
            shaderData.model[i] = HMM_Translate(instancePos);
        }

        memcpy(shaderDataBuffers[frameIndex].allocInfo.pMappedData, &shaderData, sizeof(shaderData));

        VkCommandBuffer cb = commandBuffers[frameIndex];
        VK_CHECK(vkResetCommandBuffer(cb, 0));

        VkCommandBufferBeginInfo cbBI = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        VK_CHECK(vkBeginCommandBuffer(cb, &cbBI));

        //cull 
        {
            //barrier
        
        }
        
        //color and depth image need barriers for layout transitions
        VkImageMemoryBarrier2 colorBarrier = imageBarrier(swapchainImages[imageIndex],
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT,
            0, 1);
        
        VkImageMemoryBarrier2 depthBarrier = imageBarrier(depthImage,
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

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        VkDeviceSize vOffset = 0;        
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSetTex, 0, NULL);
        vkCmdBindVertexBuffers(cb, 0, 1, &vBuffer.buffer, &vOffset);
        vkCmdBindIndexBuffer(cb, iBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VkDeviceAddress), &shaderDataBuffers[frameIndex].deviceAddress);

        vkCmdDrawIndexed(cb, array_count(geometry.indices), 3, 0, 0, 0);
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
                VkImageViewCreateInfo swapchainViewCI = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = swapchainImages[i],
                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                    .format = imageFormat,
                    .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .subresourceRange.levelCount = 1,
                    .subresourceRange.layerCount = 1,
                };

                VK_CHECK(vkCreateImageView(device, &swapchainViewCI, NULL, &swapchainImageViews[i]));
            }   
            vkDestroySwapchainKHR(device, swapchainCI.oldSwapchain, NULL);
            vmaDestroyImage(allocator, depthImage, depthImageAllocation);
            vkDestroyImageView(device, depthImageView, NULL);
            depthImageCI.extent.width = windowWidth;
            depthImageCI.extent.height = windowHeight;
            depthImageCI.format = depthImageFormat;
            VmaAllocationCreateInfo depthImageAllocCI = {0};
            depthImageAllocCI.usage = VMA_MEMORY_USAGE_AUTO;
            depthImageAllocCI.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            VK_CHECK(vmaCreateImage(allocator, &depthImageCI, &depthImageAllocCI, &depthImage, &depthImageAllocation, NULL));
            VkImageViewCreateInfo viewCI = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = depthImage,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = depthImageFormat,
                .subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                .subresourceRange.levelCount = 1,
                .subresourceRange.layerCount = 1,
            };
            VK_CHECK(vkCreateImageView(device, &viewCI, NULL, &depthImageView));
        }
    }

    VK_CHECK(vkDeviceWaitIdle(device));

    for (uint32_t i = 0; i < ARRAY_COUNT(textures); i++) {
        vkDestroySampler(device, textures[i].sampler, NULL);
        vkDestroyImageView(device, textures[i].view, NULL);
        vmaDestroyImage(allocator, textures[i].image, textures[i].allocation);
    }

    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipelineLayout, NULL);
    vkDestroyShaderModule(device, shaderModule, NULL);
    vkDestroyDescriptorSetLayout(device, descSetLayoutTex, NULL);
    vkDestroyDescriptorPool(device, descriptorPool, NULL);

    vkDestroyCommandPool(device, commandPool, NULL);

    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        DestroyBuffer(&shaderDataBuffers[i]);
    }

    for (uint32_t i = 0; i < maxFramesInFlight; i++) {
        vkDestroyFence(device, fences[i], NULL);
        vkDestroySemaphore(device, presentSemaphores[i], NULL);
    }

    for (uint32_t i = 0; i < swapchainImageCount; i++) {
        vkDestroyImageView(device, swapchainImageViews[i], NULL);
        vkDestroySemaphore(device, renderSemaphores[i], NULL);
    }

    DestroyBuffer(&vBuffer);
    DestroyBuffer(&iBuffer);

    array_free(geometry.vertices);
    array_free(geometry.indices);

    vkDestroyImageView(device, depthImageView, NULL);
    vmaDestroyImage(allocator, depthImage, depthImageAllocation);

    vkDestroySwapchainKHR(device, swapchain, NULL);
    SDL_Vulkan_DestroySurface(instance, surface, NULL);
    SDL_DestroyWindow(window);

    vmaDestroyAllocator(allocator);

    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    volkFinalize();

    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    SDL_Quit();

    printf("All is  well\n");

    return 0;

}
