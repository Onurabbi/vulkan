#ifndef OG_PLATFORM_H
#define OG_PLATFORM_H

#include "common.h"

b8 PlatformGetVulkanPresentationSupport(vulkan_instance_t instance, vulkan_physical_device_t physicalDevice, u32 queue);
u32 PlatformGetVulkanInstanceExtensionCount(void);
const char * const *PlatformGetVulkanInstanceExtensions(void);
b8 PlatformCreateWindow(void* window, void *arg, const char *title, i32 w, i32 h, u64 flags);
void PushJob(void(*jobFunc)(void*,memory_arena_t *, memory_arena_t *), void*);
void WaitForAllJobs(void);
memory_arena_t *PermanentArena(u32 threadIndex);
memory_arena_t *ScratchArena(u32 threadIndex);
memory_arena_t *StringArena(void);
void *ArenaPushSize(memory_arena_t *arena, u64 size);
u64 ArenaGetMarker(memory_arena_t *arena);
void ArenaFreeToMarker(memory_arena_t *arena, u64 marker);
const char *ArenaPrintf(memory_arena_t *arena, const char *fmt, ...);
renderer_type_t GetRendererType(void);
void PlatformInit(platform_api_t *api);
void PlatformShutdown(void);
void PlatformHotReload(platform_api_t *api);
#endif
