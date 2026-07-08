#include "renderer.h"
#include "platform.h"
#include "log.h"

#include "vulkan/vulkan.h"

static renderer_t *gRenderer;

void RendererInit(renderer_t *renderer)
{
    CameraInit(&renderer->camera, (vec3_t){ -3.0f, -3.0f, 0.0f }, (vec3_t){ 0.0f, 0.0f, -1.0f }, ASPECT_RATIO);
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

mat4_t RendererGetProjectionMatrix(void)
{
    return CameraGetProjectionMatrix(&gRenderer->camera);
}

mat4_t RendererGetViewMatrix(void)
{
    return CameraGetViewMatrix(&gRenderer->camera);
}

vec3_t RendererGetCameraPosition(void)
{
    return gRenderer->camera.position;
}

f32 RendererGetCameraNear(void)
{
    return gRenderer->camera.near;
}

f32 RendererGetCameraFar(void)
{
    return gRenderer->camera.far;
}
