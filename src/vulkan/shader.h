#ifndef VK_SHADER_H
#define VK_SHADER_H

#include "vulkan_types.h"
#include "../resource.h"

void ParseShader(resource_t *shaderResource, const u8 *spirv, memory_arena_t *arena);

void CreateGraphicsPipeline(pipeline_t *pipeline, VkDevice device, const char *name, VkFormat colorFormat, VkFormat depthFormat, u32 pushConstantSize, VkDescriptorSetLayout setLayout);
void DestroyGraphicsPipeline(pipeline_t *pipeline, VkDevice device);

void CreateComputePipeline(pipeline_t *pipeline, VkDevice device, const char *name, u32 pushConstantSize);
void DestroyComputePipeline(pipeline_t *pipeline, VkDevice device);
VkDescriptorSetLayout CreateDescriptorSetLayout(VkDevice device, VkDescriptorType type, VkShaderStageFlags shaderStage, u32 descriptorCount, b8 descriptorIndexing);
#endif // VK_SHADER_H
