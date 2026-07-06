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
    vec3_t translation;
    f32 scale;
    quat_t rotation;
} keyframe_t;

typedef struct {
    u32 drawIndex;
    f32 startTime;
    f32 period;
    keyframe_t *keyframes;
} animation_t;

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
    u32 metalRoughness;
    u32 normal;
    u32 emissive;
    u32 ao;
    f32 diffuseFactor[4];
    f32 emissiveFactor[3];
    f32 metallicFactor;
    f32 roughnessFactor;
    f32 aoStrength;
    f32 normalScale;
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
    u32 lodCount;
    mesh_lod_t meshLods[8];
} mesh_t;

typedef struct {
    u16 vx, vy, vz, vw;
    u8  nx, ny, nz, nw;
    u8  tx, ty, tz, tw;
    u16 u, v;
} vertex_t;

typedef struct {
    f32 rx,ry,rz,rw; //because quat_t forces 16 byte alignment
    vec3_t position;
    f32 scale;
    u32 meshIndex;
    u32 materialIndex;
    u32 postPass;
} draw_data_t;

typedef struct {
    u32 drawId;
    VkDrawIndexedIndirectCommand command;
}draw_command_t;

typedef struct {
    vertex_t    *vertices;
    u32         *indices;
    mesh_t      *meshes;
    draw_data_t *draws;
    material_t  *materials;
}geometry_t;

typedef struct {
    f32 P00, P11, near, far;
    f32 frustum[4];
    mat4_t projection;
    mat4_t view;
    vec3_t lightDir; //sunlight
    vec3_t camPos;
    u32 selected;
    u32 drawCount;
    f32 lodTarget;
} globals_t;

typedef struct {
    mat4_t projection;
    mat4_t model;
    VkDeviceAddress vertexAddress;
    u32 vertexOffset;
    // required by cpu code, but not by shader code. I still put it here for convenience
    u32 indexOffset;
    u32 indexCount; //this is always 36 for a cube
} skybox_data_t;

typedef struct {
    VkDeviceAddress globalsAddress;
    VkDeviceAddress meshAddress;
    VkDeviceAddress drawCommandAddress;
    VkDeviceAddress drawCommandCountAddress;
    VkDeviceAddress drawDataAddress;
    VkDeviceAddress materialAddress;
    VkDeviceAddress vertexAddress;
} pbr_data_t;

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
    buffer_t materialBuffer;
    buffer_t drawCommandBuffers[MAX_FRAMES_IN_FLIGHT];
    buffer_t drawCommandCountBuffer;
    buffer_t scratchBuffer;
    VmaAllocator allocator;
    VkDescriptorSetLayout texLayout;
    VkDescriptorSetLayout skyboxLayout;
    VkSampler texSampler;
    VkSampler skyboxSampler;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSetTex;
    VkDescriptorSet descriptorSetSkybox;
    VkFence fences[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore presentSemaphores[MAX_FRAMES_IN_FLIGHT];//max frames in flight
    VkSemaphore renderSemaphores[MAX_SWAPCHAIN_IMAGES]; //swapchain image count
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT];//max frames in flight
    pbr_data_t pbrData[MAX_FRAMES_IN_FLIGHT];
    skybox_data_t skyboxData[MAX_FRAMES_IN_FLIGHT];
    pipeline_t pipeline;
    pipeline_t skyboxPipeline;
    pipeline_t computePipeline;
    f32 cameraZ;
    u32 frameIndex;
    u32 drawCount;
    u32 indexCount;
    b8 windowResized;
    b8 resourcesLoaded;
} vulkan_context_t;

#endif
