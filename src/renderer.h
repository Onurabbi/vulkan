#ifndef RENDERER_H
#define RENDERER_H

#include "common.h"
#include "vulkan/vulkan.h"

typedef enum {
    RENDERER_VULKAN,
    RENDERER_TYPE_COUNT
}renderer_type_t;

typedef struct {
    renderer_type_t type;
    union {
        vulkan_context_t vulkan;
    };
}renderer_t;

void RendererInit(renderer_t *renderer, platform_api_t *api, string_interning_system_t *stringInterning);
void RendererHotReload(renderer_t *renderer);
void RendererShutdown(renderer_t *renderer);
void RendererRender(renderer_t *renderer);

#endif
