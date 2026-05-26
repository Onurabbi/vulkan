#include "platform.h"

#include <stdarg.h>

static platform_api_t *platformApi;

void InitPlatformApi(platform_api_t *api)
{
    platformApi = api;
}

b8 VulkanGetPresentationSupport(vulkan_instance_t instance, vulkan_physical_device_t physicalDevice, u32 queue)
{
    return platformApi->VulkanGetPresentationSupport(instance, physicalDevice, queue);
}

b8 CreateWindow(void* window, void *arg, const char *title, i32 w, i32 h, u64 flags)
{
    return platformApi->CreateWindow(window, arg, title, w, h, flags);
}

void PushJob(void(*jobFunc)(void*, memory_arena_t*), void* data)
{
    platformApi->PushJob(jobFunc, data);
}

void WaitForAllJobs(void)
{
    platformApi->WaitForAllJobs();
}

memory_arena_t *ScratchArena(u32 tid)
{
    return platformApi->ScratchArena(tid);
}

memory_arena_t *StringArena(void)
{
    return platformApi->StringArena();
}

memory_arena_t *PermanentArena(void)
{
    return platformApi->PermanentArena();
}

void *ArenaPushSize(memory_arena_t *arena, u64 size)
{
    return platformApi->ArenaPushSize(arena, size);
}

u64 ArenaGetMarker(memory_arena_t *arena)
{
    return platformApi->ArenaGetMarker(arena);
}

void ArenaFreeToMarker(memory_arena_t *arena, u64 marker)
{
    platformApi->ArenaFreeToMarker(arena, marker);
}

const char *ArenaPrintf(memory_arena_t *arena, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    const char *result = platformApi->ArenaPrintf(arena, fmt, args);
    va_end(args);
    return result;
}
