#ifndef OG_RENDERER_H
#define OG_RENDERER_H

#include "common.h"
#include "vulkan/vulkan_types.h"
#include "camera.h"

#define ASPECT_RATIO (16.0 / 9.0f)

typedef struct {
    renderer_type_t type;
    camera_t camera;
    union 
    {
        vulkan_context_t vulkan;
    };
}renderer_t;

void RendererInit(renderer_t *renderer);
void RendererHotReload(renderer_t *renderer);
void RendererShutdown(void);
void RendererRender(void);
mat4_t RendererGetProjectionMatrix(void);
mat4_t RendererGetViewMatrix(void);
vec3_t RendererGetCameraPosition(void);
f32 RendererGetCameraNear(void);
f32 RendererGetCameraFar(void);

#endif
