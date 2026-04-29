#include "common.h"
#include "shader.h"
#include "og_ds.h"
#if defined __APPLE__
#include <spirv_cross/spirv.h>
#elif defined __linux__
#include <spirv/unified1/spirv.h>
#elif VK_HEADER_VERSION >= 135
#include <spirv-headers/spirv.h>
#else
#include <vulkan/spirv.h>
#endif

#include <stdio.h>
#include <vulkan/vulkan.h>
#include <volk/volk.h>
#include <SDL3/SDL_filesystem.h>

#include "log.h"

typedef struct {
    u32 opCode;
    u32 typeId;
    u32 storageClass;
    u32 binding;
    u32 set;
    u32 imageSampled;
    u32 constant;
    const char *name;
}Id;

static VkShaderStageFlagBits GetShaderStage(SpvExecutionModel model)
{
    switch (model) {
        case SpvExecutionModelVertex: return VK_SHADER_STAGE_VERTEX_BIT;
        case SpvExecutionModelGeometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case SpvExecutionModelFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case SpvExecutionModelGLCompute: return VK_SHADER_STAGE_COMPUTE_BIT;
        default: LV_ASSERT(false && "Unsupported shader execution model"); return (VkShaderStageFlagBits)0;
    }
}

static VkDescriptorType GetDescriptorType(SpvOp op, u32 imageSampled)
{
    switch (op) {
        case SpvOpTypeStruct:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case SpvOpTypeImage:
            return (imageSampled == 1) ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case SpvOpTypeSampler:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case SpvOpTypeSampledImage:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        default:
            LV_ASSERT(false && "Unsupported SPIR-V type for descriptor");
            return (VkDescriptorType)0;
    }
}

static void ParseShader(shader_t *shader, const u32*code, u32 codeSize, memory_arena_t *arena)
{
    LV_ASSERT(code[0] == SpvMagicNumber && "Invalid SPIR-V magic number");

    u32 idBound = code[3];

    Id *ids = NULL;
    ArrayInitWithArena(ids, arena, idBound);
    ArrayResize(ids, idBound);
    memset(ids, 0, sizeof(Id) * idBound);

    int localSizeIdX = -1;
    int localSizeIdY = -1;
    int localSizeIdZ = -1;

    const u32*insn = &code[5];
    
    while (insn != &code[codeSize]) {
        u16 opCode = (u16)(insn[0]);
        u16 wordCount = (u16)(insn[0] >> 16);

        switch (opCode) {
            case SpvOpEntryPoint: {
                LV_ASSERT(wordCount >= 2);
                shader->stage = GetShaderStage((SpvExecutionModel)insn[1]);
                break;
            }
            case SpvOpExecutionMode: {
                LV_ASSERT(wordCount >= 3);
                u32 mode = insn[2];
                switch (mode) {
                    case SpvExecutionModeLocalSize: {
                        LV_ASSERT(wordCount == 6);
                        shader->localSizeX = insn[3];
                        shader->localSizeY = insn[4];
                        shader->localSizeZ = insn[5];
                        break;
                    }
                }
                break;
            }
            case SpvOpExecutionModeId: {
                LV_ASSERT(wordCount == 6);
                u32 mode = insn[2];
                switch (mode) {
                    case SpvExecutionModeLocalSizeId: {
                        localSizeIdX = insn[3];
                        localSizeIdY = insn[4];
                        localSizeIdZ = insn[5];
                        break;
                    }
                }
                break;
            }

            case SpvOpVariable: {
                LV_ASSERT(wordCount >= 4);

                u32 id = insn[2];
                LV_ASSERT(id < idBound);
                LV_ASSERT(ids[id].opCode == 0 && "ID already defined");

                ids[id].opCode = opCode;
                ids[id].typeId = insn[1];
                ids[id].storageClass = insn[3];
                break;
            }

            case SpvOpName: {
                LV_ASSERT(wordCount >= 3);

                u32 id = insn[1];
                LV_ASSERT(id < idBound);

                ids[id].name = (const char *)&insn[2];
                break;
            }
        }

        LV_ASSERT(insn + wordCount <= code + codeSize && "Instruction exceeds code bounds");
        insn += wordCount;
    }


    for (u32 i = 0; i < idBound; i++) {
        Id id = ids[i];

        if (id.opCode == SpvOpVariable && id.storageClass == SpvStorageClassPushConstant) {
            shader->usesPushConstants = true;
        }

        if  (id.opCode == SpvOpVariable && id.storageClass == SpvStorageClassUniformConstant) {
            LV_ASSERT(id.set == 0 && "Only set 0 is supported for now");
        }

        if (shader->stage == VK_SHADER_STAGE_COMPUTE_BIT) {
            if (localSizeIdX >= 0) {
                LV_ASSERT(ids[localSizeIdX].opCode == SpvOpConstant && "Local size X must be a constant");
                shader->localSizeX = ids[localSizeIdX].constant;
            }
            if (localSizeIdY >= 0) {
                LV_ASSERT(ids[localSizeIdY].opCode == SpvOpConstant && "Local size Y must be a constant");
                shader->localSizeY = ids[localSizeIdY].constant;
            }
            if (localSizeIdZ >= 0) {
                LV_ASSERT(ids[localSizeIdZ].opCode == SpvOpConstant && "Local size Z must be a constant");
                shader->localSizeZ = ids[localSizeIdZ].constant;
            }

            LV_ASSERT(shader->localSizeX && shader->localSizeY && shader->localSizeZ && "Compute shader must specify local size");
        }
    }
}

static bool LoadShader(shader_t *shader, const char *path, memory_arena_t *arena)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        printf("Failed to open shader file: %s\n", path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    ArrayInitWithArena(shader->spirv, arena, fileSize);
    ArrayResize(shader->spirv, fileSize);

    size_t rc = fread(shader->spirv, 1, fileSize, file);
    LV_ASSERT(rc == fileSize && "Failed to read entire shader file");

    fclose(file);

    LV_ASSERT(fileSize % sizeof(uint32_t) == 0 && "SPIR-V file size must be a multiple of 4");

    ParseShader(shader, (const uint32_t*)shader->spirv, fileSize / sizeof(uint32_t), arena);

    return true;
}

void LoadShaders(void *data, memory_arena_t *arena)
{
    shader_t *result = NULL;
    const char *basePath = SDL_GetBasePath();

    const char *fullPath = ArenaPrintf(arena, "%s%s", basePath, "spirv/");

    i32 spvCount = 0;
    char **glob = SDL_GlobDirectory(fullPath, "*.spv", 0, &spvCount);
    if (!glob) {
        LOGE("Failed to enumerate shader directory: %s\n", fullPath);
    }

    ArrayInitWithArena(result, arena, spvCount);

    for (u32 i = 0; i < spvCount; i++) {
        shader_t shader = {0};
        shader.name = StringIntern(glob[i]);

        //basePath is guaranteed to end with a slash, so we don't need to add an extra one here.
        const char *shaderPath = ArenaPrintf(arena, "%s%s", fullPath, glob[i]);
        if (!LoadShader(&shader, shaderPath, arena)) {
            LOGE("Failed to load shader: %s\n", shaderPath);
            continue;
        }

        ArrayPush(result, shader);
    }

    SDL_free(glob);

    *(shader_t**)data = result;
}

VkDescriptorSetLayout CreateDescriptorSetLayout(VkDevice device, VkDescriptorType type, VkShaderStageFlags shaderStage, u32 descriptorCount)
{
    VkDescriptorSetLayout result;
    //descriptor set layout for indexing. 
    VkDescriptorBindingFlags descVariableFlag = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo descBindingFlags = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 1,
        .pBindingFlags = &descVariableFlag,
    };

    VkDescriptorSetLayoutBinding descLayoutBindingTex = {
        .descriptorType = type,
        .descriptorCount = descriptorCount,
        .stageFlags = shaderStage,
    };

    VkDescriptorSetLayoutCreateInfo descLayoutTexCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &descBindingFlags,
        .bindingCount = 1,
        .pBindings = &descLayoutBindingTex
    };
    VK_CHECK(vkCreateDescriptorSetLayout(device, &descLayoutTexCI, NULL, &result));
    return result;
}

void DestroyGraphicsPipeline(pipeline_t *pipeline, VkDevice device)
{
    vkDestroyPipeline(device, pipeline->pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline->pipelineLayout, NULL);
    vkDestroyShaderModule(device, pipeline->shaderModule, NULL);
}

void DestroyComputePipeline(pipeline_t *pipeline, VkDevice device)
{
    DestroyGraphicsPipeline(pipeline, device);
}

static const shader_t *FindShaderByName(const shader_t *shaders, const char *name)
{
    const shader_t *result = NULL;
    const char *shaderName = StringIntern(name);
    for (u32 i = 0; i < ArrayCount(shaders); i++) {
        if (shaders[i].name == shaderName) {
            result = &shaders[i];
            break;
        }
    }
    return result;
}

void CreateComputePipeline(pipeline_t *pipeline, const shader_t *shaders, VkDevice device, const char *name, u32 pushConstantSize) 
{
    const shader_t *shader = FindShaderByName(shaders, name);
    LV_ASSERT(shader && "Shader not found for pipeline creation");
    LV_ASSERT(shader->stage == VK_SHADER_STAGE_COMPUTE_BIT);
    
    VkShaderModuleCreateInfo shaderModuleCI = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = ArrayCount(shader->spirv),
        .pCode = (const u32*)shader->spirv,
    };
    VK_CHECK(vkCreateShaderModule(device, &shaderModuleCI, NULL, &pipeline->shaderModule));
    
    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = pushConstantSize,
    };

    VkPipelineLayoutCreateInfo pipelineLayoutCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0,
        .pSetLayouts = NULL,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange
    };

    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutCI, NULL, &pipeline->pipelineLayout));

    VkPipelineShaderStageCreateInfo shaderStageCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = pipeline->shaderModule,
        .pName = "main",
    };
    
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = shaderStageCI,
        .layout = pipeline->pipelineLayout,
    };

    VK_CHECK(vkCreateComputePipelines(device, NULL, 1, &ci, 0, &pipeline->pipeline));
}

void CreateGraphicsPipeline(pipeline_t *pipeline, const shader_t *shaders, VkDevice device, const char *name, VkFormat colorFormat, VkFormat depthFormat, u32 pushConstantSize, VkDescriptorSetLayout setLayout)
{
    const shader_t *shader = FindShaderByName(shaders, name);
    LV_ASSERT(shader && "Shader not found for pipeline creation");

    VkShaderModuleCreateInfo shaderModuleCI = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = ArrayCount(shader->spirv),
        .pCode = (const uint32_t*)shader->spirv,
    };
    VK_CHECK(vkCreateShaderModule(device, &shaderModuleCI, NULL, &pipeline->shaderModule));

    //Pipeline
    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .size = pushConstantSize,
    };

    VkPipelineLayoutCreateInfo pipelineLayoutCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange
    };

    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutCI, NULL, &pipeline->pipelineLayout));

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
        .vertexAttributeDescriptionCount = ARRAY_SIZE(vertexAttributes),
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
            .module = pipeline->shaderModule,
            .pName = "main"
        },

        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = pipeline->shaderModule,
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

    VkFormat colorImageFormats[1] = {colorFormat};
    VkPipelineRenderingCreateInfo renderingCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = colorImageFormats,
        .depthAttachmentFormat = depthFormat
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
        .layout = pipeline->pipelineLayout
    };

    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, NULL, &pipeline->pipeline));
}
