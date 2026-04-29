#include "memory.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include <SDL3/SDL_cpuinfo.h>

//for allocations that are frequently created and destroyed
static memory_arena_t *scratchArenas; 
static u32 threadCount;
// for allocations that are created once and used throughout the entire program 
static memory_arena_t permanentArena = {0};
static memory_arena_t stringArena = {0};

memory_arena_t *ScratchArena(u32 threadIndex) 
{
    LV_ASSERT(threadIndex < threadCount && "Thread index is larger than MAX_THREADS");
    return &scratchArenas[threadIndex];
}

memory_arena_t *PermanentArena(void) 
{
    return &permanentArena;
}

memory_arena_t *StringArena(void)
{
    return &stringArena;
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

void ArenaInit(void *base, memory_arena_t *arena, u64 memSize)
{
    arena->base = base;
    arena->size = memSize;
    arena->used = 0;
}

void ArenaDeinit(memory_arena_t *arena)
{
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

void MemoryInit(game_memory_t *memory)
{
    u8 *base = (u8*)memory->memoryBase;
    ArenaInit(base, &permanentArena, PERMANENT_ARENA_CAPACITY);
    base += PERMANENT_ARENA_CAPACITY;

    threadCount = memory->threadCount;
    scratchArenas = PushArray(&permanentArena, threadCount, memory_arena_t);
    for (u32 i = 0; i < threadCount; i++) {
        ArenaInit(base, &scratchArenas[i], SCRATCH_ARENA_CAPACITY);
        base += SCRATCH_ARENA_CAPACITY;
    }

    ArenaInit(base, &stringArena, STRING_ARENA_CAPACITY);
    base += STRING_ARENA_CAPACITY;
    
    LV_ASSERT(base - (u8*)memory->memoryBase == memory->memorySize);
}

void MemoryDeinit(void)
{
    for (u32 i = 0; i < threadCount; i++) {
        ArenaDeinit(&scratchArenas[i]);
    }
    ArenaDeinit(&permanentArena);
}

void MemoryReset(void)
{
    for (u32 i = 0; i < threadCount; i++) {
        ArenaFreeToMarker(&scratchArenas[i], 0);
    }
}
