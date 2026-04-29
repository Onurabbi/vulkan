#ifndef VK_SHADER_H
#define VK_SHADER_H

#include "common.h"
#include "og_ds.h"
#include "HandmadeMath.h"

#include <vulkan/vulkan.h>

#include <stdbool.h>

typedef HMM_Vec2 vec2_t;
typedef HMM_Vec3 vec3_t;
typedef HMM_Vec4 vec4_t;
typedef HMM_Mat4 mat4_t;
typedef HMM_Quat quat_t;

typedef struct {
    vec3_t p;
    vec3_t n;
    vec2_t t;
} Vertex;

typedef struct {
    const char *name;
    char *spirv; //dynamic array
    VkShaderStageFlagBits stage;

    u32 localSizeX;
    u32 localSizeY;
    u32 localSizeZ;

    bool usesPushConstants;
} shader_t;

typedef struct {
    mat4_t projection;
    mat4_t view;
    mat4_t model[3];
    vec4_t lightPos;
    u32 *selected;
} globals_t;

typedef struct {
    VkDeviceAddress globalsAddress;
} shader_data_t;

typedef struct {
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    VkShaderModule shaderModule;
} pipeline_t;

void LoadShaders(void *data, memory_arena_t *arena);

void CreateGraphicsPipeline(pipeline_t *pipeline, const shader_t *shaders, VkDevice device, const char *name, VkFormat colorFormat, VkFormat depthFormat, u32 pushConstantSize, VkDescriptorSetLayout setLayout);
void DestroyGraphicsPipeline(pipeline_t *pipeline, VkDevice device);

void CreateComputePipeline(pipeline_t *pipeline, const shader_t *shaders, VkDevice device, const char *name, u32 pushConstantSize);    
void DestroyComputePipeline(pipeline_t *pipeline, VkDevice device);
VkDescriptorSetLayout CreateDescriptorSetLayout(VkDevice device, VkDescriptorType type, VkShaderStageFlags shaderStage, u32 descriptorCount);
#endif // VK_SHADER_H
