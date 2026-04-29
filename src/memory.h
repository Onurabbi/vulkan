#ifndef OG_MEMORY_H
#define OG_MEMORY_H

#include "common.h"

typedef struct {
   void *base;
   u64  size;
   u64  used;
}memory_arena_t;

u64 ArenaGetMarker(const memory_arena_t *arena);
void ArenaFreeToMarker(memory_arena_t *arena, u64 marker);
void *ArenaPushSize(memory_arena_t *arena, u64 size);
void ArenaInit(void *base, memory_arena_t* arena, u64 memorySize);
void ArenaDeinit(memory_arena_t* arena);
const char *ArenaPrintf(memory_arena_t *arena, const char *, ...);

void MemoryInit(game_memory_t *memory);
void MemoryDeinit(void);
void MemoryReset(void);

memory_arena_t *ScratchArena(u32 threadIndex);
memory_arena_t *PermanentArena(void);
memory_arena_t *StringArena(void);

#define PushArray(arena, count, type) (type*)ArenaPushSize((arena), (count) * sizeof(type))
#define PushStruct(arena, type) PushArray(arena, 1, type)


#endif // OG_MEMORY_H
