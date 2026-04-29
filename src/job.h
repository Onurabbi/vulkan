#ifndef OG_JOB_H
#define OG_JOB_H

#include "memory.h"

typedef struct {
    void (*jobFunc)(void *data, memory_arena_t *arena);
    void *data;
}job_t;

void JobSystemInit(void);
void JobSystemPushJob(void (*jobFunc)(void *data, memory_arena_t *arena), void *data);
void JobSystemWaitForAllJobs(void);
void JobSystemDeinit(void);

#endif // OG_JOB_H
