#ifndef VK_SHADER_H
#define VK_SHADER_H

#include "common.h"
#include "og_ds.h"
#include "HandmadeMath.h"

#include <vulkan/vulkan.h>

#include <stdbool.h>

typedef HMM_Vec2 Vec2;
typedef HMM_Vec3 Vec3;
typedef HMM_Vec4 Vec4;
typedef HMM_Mat4 Mat4;
typedef HMM_Quat Quat;

typedef struct {
    Vec3 p;
    Vec3 n;
    Vec2 t;
} Vertex;

typedef struct {
    const char *name;
    char *spirv; //dynamic array
    VkShaderStageFlagBits stage;

    uint32_t localSizeX;
    uint32_t localSizeY;
    uint32_t localSizeZ;

    bool usesPushConstants;
} Shader;

typedef struct {
    Mat4 projection;
    Mat4 view;
    Mat4 model[3];
    Vec4 lightPos;
    uint32_t selected;
} Globals;

typedef struct {
    VkDeviceAddress globalsAddress;
} ShaderData;

typedef struct {
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    VkShaderModule shaderModule;
} Pipeline;

void LoadShaders(void *data, memory_arena_t *arena);

void CreateGraphicsPipeline(Pipeline *pipeline, const Shader *shaders, VkDevice device, const char *name, VkFormat colorFormat, VkFormat depthFormat, uint32_t pushConstantSize, VkDescriptorSetLayout setLayout);
void DestroyGraphicsPipeline(Pipeline *pipeline, VkDevice device);

void CreateComputePipeline(Pipeline *pipeline, const Shader *shaders, VkDevice device, const char *name, uint32_t pushConstantSize);    
void DestroyComputePipeline(Pipeline *pipeline, VkDevice device);
VkDescriptorSetLayout CreateDescriptorSetLayout(VkDevice device, VkDescriptorType type, VkShaderStageFlags shaderStage, uint32_t descriptorCount);
#endif // VK_SHADER_H
