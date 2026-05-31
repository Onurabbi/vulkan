#ifndef OG_VULKAN_H
#define OG_VULKAN_H

#include "vulkan_types.h"

void VulkanInit(vulkan_context_t *context);
void VulkanHotReload(vulkan_context_t *context);
void VulkanRender(void);
void VulkanShutdown(void);

#endif
