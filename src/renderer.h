#ifndef OG_RENDERER_H
#define OG_RENDERER_H

#include "common.h"
#include "vulkan/vulkan.h"

typedef struct {
    renderer_type_t type;
    union {
        vulkan_context_t vulkan;
    };
}renderer_t;

void RendererInit(renderer_t *renderer);
void RendererHotReload(renderer_t *renderer);
void RendererShutdown(void);
void RendererRender(void);

#endif
