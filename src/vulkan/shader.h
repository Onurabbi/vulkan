#ifndef VK_SHADER_H
#define VK_SHADER_H

#include "../common.h"
#include "../og_ds.h"
#include "../string_intern.h"
#include "volk.h"

#include <HandmadeMath.h>

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

    b8 usesPushConstants;
} shader_t;

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
    string_interning_system_t *stringInterning;
    void *loadedData;
}resource_loader_t;

void LoadShaders(void *data, memory_arena_t *arena);

void CreateGraphicsPipeline(pipeline_t *pipeline, string_interning_system_t *stringInterning, const shader_t *shaders, VkDevice device, const char *name, VkFormat colorFormat, VkFormat depthFormat, u32 pushConstantSize, VkDescriptorSetLayout setLayout);
void DestroyGraphicsPipeline(pipeline_t *pipeline, VkDevice device);

void CreateComputePipeline(pipeline_t *pipeline, string_interning_system_t* stringInterning, shader_t *shaders, VkDevice device, const char *name, u32 pushConstantSize);
void DestroyComputePipeline(pipeline_t *pipeline, VkDevice device);
VkDescriptorSetLayout CreateDescriptorSetLayout(VkDevice device, VkDescriptorType type, VkShaderStageFlags shaderStage, u32 descriptorCount);
#endif // VK_SHADER_H
