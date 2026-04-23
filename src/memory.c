#include "memory.h"
#include "log.h"

#if defined (__linux__)
#include <sys/mman.h>
#endif

#include <stdarg.h>
#include <stdio.h>

#include <SDL3/SDL_cpuinfo.h>

//for allocations that are frequently created and destroyed
static memory_arena_t scratchArena[MAX_THREADS] = {0}; 
// for allocations that are created once and used throughout the entire program 
static memory_arena_t permanentArena = {0};
static u32 threadCount;

memory_arena_t *ScratchArena(u32 threadIndex) 
{
    LV_ASSERT(threadIndex < threadCount && "Thread index is larger than MAX_THREADS");
    return &scratchArena[threadIndex];
}

memory_arena_t *PermanentArena(void) 
{
    return &permanentArena;
}

static void *MapMemory(u64 size)
{
    void *base = NULL;
#if defined(__linux__)
    base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#endif
    LV_ASSERT(base && "Failed to map memory");
    return base;
}

u64 ArenaGetMarker(const memory_arena_t *arena)
{
    return arena->used;
}

void ArenaFreeToMarker(memory_arena_t *arena, u64 marker)
{
    LV_ASSERT(marker <= arena->used);
    arena->used = marker;
}

void *ArenaPushSize(memory_arena_t *arena, u64 size)
{
    LV_ASSERT(arena->used + size <= arena->size);
    void *base = (u8*)arena->base + arena->used;
    arena->used += size;
    return base;
}

void ArenaInit(memory_arena_t *arena, u64 memSize)
{
    void *base = MapMemory(memSize);
    arena->base = base;
    arena->size = memSize;
    arena->used = 0;
}

void ArenaDeinit(memory_arena_t *arena)
{
    munmap(arena->base, arena->size);

    arena->base = NULL;
    arena->size = 0;
    arena->used = 0;
}

const char *ArenaPrintf(memory_arena_t *arena, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    
    va_list args2;
    va_copy(args2, args);
    i32 n = vsnprintf(NULL, 0, fmt, args2);
    va_end(args2);

    char *buffer = PushArray(arena, n + 1, char);
    vsnprintf(buffer, n + 1, fmt, args);
    va_end(args);
    return buffer;
}

void MemoryInit(void)
{
    threadCount = SDL_GetNumLogicalCPUCores() - 1;
    if (threadCount > MAX_THREADS) {
        LOGW("Thread count (%u) is larger than MAX_THREADS (%u). Capping thread count to MAX_THREADS.\n", threadCount, MAX_THREADS);
        threadCount = MAX_THREADS;
    }

    for (u32 i = 0; i < threadCount; i++) {
        ArenaInit(&scratchArena[i], SCRATCH_ARENA_CAPACITY);
    }
    ArenaInit(&permanentArena, PERMANENT_ARENA_CAPACITY);
}

void MemoryDeinit(void)
{
    for (u32 i = 0; i < threadCount; i++) {
        ArenaDeinit(&scratchArena[i]);
    }
    ArenaDeinit(&permanentArena);
}

void MemoryReset(void)
{
    for (u32 i = 0; i < threadCount; i++) {
        ArenaFreeToMarker(&scratchArena[i], 0);
    }
    ArenaFreeToMarker(&permanentArena, 0);
}
