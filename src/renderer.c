#include "renderer.h"
#include "log.h"

void RendererInit(renderer_t *renderer, platform_api_t *api, string_interning_system_t *stringInterning)
{
    switch (renderer->type) {
        case RENDERER_VULKAN:
            VulkanInit(&renderer->vulkan, api, stringInterning);
            break;
        default:
            LOGF("Unsupported renderer type");
    }
}

void RendererHotReload(renderer_t *renderer)
{
    switch (renderer->type) {
        case RENDERER_VULKAN:
            VulkanHotReload(&renderer->vulkan);
            break;
        default:
            LOGF("Unsupported renderer type");
    }
}

void RendererShutdown(renderer_t *renderer)
{
    switch (renderer->type) {
        case RENDERER_VULKAN:
            VulkanShutdown(&renderer->vulkan);
            break;
        default:
            LOGF("Unsupported renderer type");
    }
}
void RendererRender(renderer_t *renderer)
{
    switch (renderer->type) {
        case RENDERER_VULKAN:
            VulkanRender(&renderer->vulkan);
            break;
        default:
            LOGF("Unsupported renderer type");
    }
}


