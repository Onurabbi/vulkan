#ifndef OG_VULKAN_H
#define OG_VULKAN_H

#include "shader.h"
#include "buffer.h"
#include "texture.h"

#include "volk.h"
#include "vma.h"

#include <SDL3/SDL_video.h>

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
    u32 textureIndex;
} mesh_t;

typedef struct {
    f32 rx,ry,rz,rw; //because quat_t forces 16 byte alignment
    vec3_t position;
    f32 scale;
    u32 meshIndex;
} draw_data_t;

typedef struct {
    u32 drawId;
    VkDrawIndexedIndirectCommand command;
}draw_command_t;

typedef struct {
    Vertex *vertices;
    u32 *indices;
    mesh_t *meshes;
    u32 indexCount;//required for drawing
}Geometry;

typedef struct {
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    vulkan_window_t  window;
    VkSwapchainKHR swapchain;
    Image swapchainImages[MAX_SWAPCHAIN_IMAGES];
    Image depthImage;
    u32 swapchainImageCount;
    u32 queueFamily;
    VkQueue queue;
    buffer_t vertexBuffer;
    buffer_t indexBuffer;
    buffer_t shaderGlobalsBuffers[MAX_FRAMES_IN_FLIGHT];
    buffer_t drawBuffer;
    buffer_t meshBuffer;
    buffer_t drawCommandBuffers[MAX_FRAMES_IN_FLIGHT];
    buffer_t drawCommandCountBuffer;
    buffer_t scratchBuffer;
    VmaAllocator allocator;
    Texture textures[NUM_TEXTURES];
    VkDescriptorImageInfo textureDescriptors[NUM_TEXTURES];
    VkDescriptorSetLayout texLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSetTex;
    VkFence fences[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore presentSemaphores[MAX_FRAMES_IN_FLIGHT];//max frames in flight
    VkSemaphore renderSemaphores[MAX_SWAPCHAIN_IMAGES]; //swapchain image count
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];//max frames in flight
    shader_data_t shaderData[3];
    Geometry geometry;
    //static compute_data_t gComputeData;
    pipeline_t pipeline;
    pipeline_t computePipeline;
    f32 cameraZ;
    u32 frameIndex;
    b8 windowResized;
    u32 drawCount;
} vulkan_context_t;

void VulkanInit(vulkan_context_t *context, platform_api_t *api, string_interning_system_t *stringInterning);
void VulkanHotReload(vulkan_context_t *context);
void VulkanRender(vulkan_context_t *context);
void VulkanShutdown(vulkan_context_t *context);

#endif
