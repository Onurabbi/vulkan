#ifndef VK_SHADER_H
#define VK_SHADER_H

#include "vulkan_types.h"

void LoadShaders(void *data, memory_arena_t *arena);

void CreateGraphicsPipeline(pipeline_t *pipeline, const shader_t *shaders, VkDevice device, const char *name, VkFormat colorFormat, VkFormat depthFormat, u32 pushConstantSize, VkDescriptorSetLayout setLayout);
void DestroyGraphicsPipeline(pipeline_t *pipeline, VkDevice device);

void CreateComputePipeline(pipeline_t *pipeline, shader_t *shaders, VkDevice device, const char *name, u32 pushConstantSize);
void DestroyComputePipeline(pipeline_t *pipeline, VkDevice device);
VkDescriptorSetLayout CreateDescriptorSetLayout(VkDevice device, VkDescriptorType type, VkShaderStageFlags shaderStage, u32 descriptorCount);
#endif // VK_SHADER_H
