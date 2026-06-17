#include "renderer.h"
#include "platform.h"
#include "log.h"

#include "vulkan/vulkan.h"

static renderer_t *gRenderer;

void RendererInit(renderer_t *renderer)
{
    renderer->type = GetRendererType();
    switch (renderer->type) {
        case RENDERER_TYPE_VULKAN:
            VulkanInit(&renderer->vulkan);
            break;
        default:
            LOGF("Unsupported renderer type");
    }
    gRenderer = renderer;
}

void RendererHotReload(renderer_t *renderer)
{
    renderer->type = GetRendererType();
    switch (renderer->type) {
        case RENDERER_TYPE_VULKAN:
            VulkanHotReload(&renderer->vulkan);
            break;
        default:
            LOGF("Unsupported renderer type");
    }
    gRenderer = renderer;
}

void RendererShutdown(void)
{
    switch (gRenderer->type) {
        case RENDERER_TYPE_VULKAN:
            VulkanShutdown();
            break;
        default:
            LOGF("Unsupported renderer type");
    }
}

void RendererRender(void)
{
    switch (gRenderer->type) {
        case RENDERER_TYPE_VULKAN:
            VulkanRender();
            break;
        default:
            LOGF("Unsupported renderer type");
    }
}
