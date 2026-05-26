#ifndef OG_PLATFORM_H
#define OG_PLATFORM_H

#include "common.h"

b8 VulkanGetPresentationSupport(vulkan_instance_t instance, vulkan_physical_device_t physicalDevice, u32 queue);
b8 CreateWindow(void* window, void *arg, const char *title, i32 w, i32 h, u64 flags);
b8 HotReloaded(void);
void SetHotReloaded(b8 reloaded);
void PushJob(void(*jobFunc)(void*,memory_arena_t*), void*);
void WaitForAllJobs(void);
memory_arena_t *PermanentArena(void);
memory_arena_t *ScratchArena(u32 threadIndex);
memory_arena_t *StringArena(void);
void *ArenaPushSize(memory_arena_t *arena, u64 size);
u64 ArenaGetMarker(memory_arena_t *arena);
void ArenaFreeToMarker(memory_arena_t *arena, u64 marker);
const char *ArenaPrintf(memory_arena_t *arena, const char *fmt, ...);
void InitPlatformApi(platform_api_t *api);

#endif
