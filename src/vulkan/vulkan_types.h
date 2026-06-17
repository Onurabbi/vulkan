#ifndef OG_VULKAN_TYPES_H
#define OG_VULKAN_TYPES_H

#include "../common.h"
#include "../limits.h"

#ifndef VULKAN_H_
#include "volk.h"
#endif

#ifndef HANDMADE_MATH_H
#include <HandmadeMath.h>
#endif

#ifndef AMD_VULKAN_MEMORY_ALLOCATOR_H
#include "vma.h"
#endif

typedef struct SDL_Window SDL_Window;

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    VmaAllocation allocation;
} image_t;

typedef struct {
    VmaAllocation allocation;
    VmaAllocationInfo allocInfo;
    VkBuffer buffer;
    VkDeviceAddress deviceAddress;
    VkDeviceSize size;
} buffer_t;

typedef struct {
    SDL_Window *window;
    VkSurfaceKHR surface;
    i32 w,h;
} vulkan_window_t;

typedef struct {
    u32 albedo;
    u32 normal;
    u32 specular;
    u32 emissive;
    vec4_t diffuseFactor;
    vec4_t specularFactor;
    vec3_t emissiveFactor;
    
    u32 padding;
} material_t;

typedef struct {
    u32 indexOffset;
    u32 indexCount;
    f32 error;
} mesh_lod_t;

typedef struct {
    vec3_t center;
    f32 radius;
    u32 vertexOffset;
    u32 vertexCount;
    u32 textureIndex;
    u32 lodCount;
    mesh_lod_t meshLods[8];
} mesh_t;

typedef struct {
    vec3_t p;
    vec3_t n;
    vec2_t t;
} vertex_t;

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
    vertex_t *vertices;
    u32 *indices;
    mesh_t *meshes;
}geometry_t;

typedef struct {
    f32 P00, P11, near, far;
    f32 frustum[4];
    mat4_t projection;
    mat4_t view;
    vec4_t lightPos;
    u32 selected;
    u32 drawCount;
} globals_t;

typedef struct {
    VkDeviceAddress globalsAddress;
    VkDeviceAddress meshAddress;
    VkDeviceAddress drawCommandAddress;
    VkDeviceAddress drawCommandCountAddress;
    VkDeviceAddress drawDataAddress;
} shader_data_t;

typedef struct {
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    VkShaderModule shaderModule;
} pipeline_t;

typedef struct {
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    vulkan_window_t  window;
    VkSwapchainKHR swapchain;
    image_t swapchainImages[MAX_SWAPCHAIN_IMAGES];
    image_t depthImage;
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
    VkDescriptorSetLayout texLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSetTex;
    VkFence fences[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore presentSemaphores[MAX_FRAMES_IN_FLIGHT];//max frames in flight
    VkSemaphore renderSemaphores[MAX_SWAPCHAIN_IMAGES]; //swapchain image count
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];//max frames in flight
    shader_data_t shaderData[3];
    //static compute_data_t gComputeData;
    pipeline_t pipeline;
    pipeline_t computePipeline;
    f32 cameraZ;
    u32 frameIndex;
    u32 drawCount;
    u32 indexCount;
    b8 windowResized;
    b8 resourcesLoaded;
} vulkan_context_t;

#endif
