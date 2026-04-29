#include "common.h"
#include "job.h"
#include "log.h"

#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_mutex.h>

static SDL_Thread *workerThreads[MAX_THREADS];
static u32 threadIndices[MAX_THREADS];
static u32 threadCount;

static SDL_Mutex *jobQueueMutex;
static SDL_Condition *jobQueueCond;
static job_t jobQueue[MAX_JOBS];
static u32 jobCount;
static u32 activeJobCount;
static bool shutdownFlag;

static void KillWorkerThreads(void)
{
    SDL_LockMutex(jobQueueMutex);
    while (jobCount > 0) {
        SDL_WaitCondition(jobQueueCond, jobQueueMutex);
    }
    shutdownFlag = true;
    SDL_BroadcastCondition(jobQueueCond);
    SDL_UnlockMutex(jobQueueMutex);

    for (u32 i = 0; i < threadCount; i++) {
        i32 status;
        SDL_WaitThread(workerThreads[i], &status);
        LOGI("Worker thread %u exited with status: %d", i, status);
        workerThreads[i] = NULL;
    }
}

static i32 WorkerThreadFunc(void *data)
{
    u32 threadIndex = *(u32 *)data;
    LOGI("Worker thread %u started", threadIndex);
    //worker thread main loop
    while (true) {
        SDL_LockMutex(jobQueueMutex);

        while (jobCount == 0 && !shutdownFlag) {
            SDL_WaitCondition(jobQueueCond, jobQueueMutex);
        }

        if (shutdownFlag) {
            SDL_BroadcastCondition(jobQueueCond);
            SDL_UnlockMutex(jobQueueMutex);
            break;
        }

        job_t job = jobQueue[0];
        //shift the remaining jobs in the queue forward
        for (u32 i = 1; i < jobCount; i++) {
            jobQueue[i - 1] = jobQueue[i];
        }

        jobCount--;
        activeJobCount++;

        SDL_BroadcastCondition(jobQueueCond); 
        SDL_UnlockMutex(jobQueueMutex);

        job.jobFunc(job.data, ScratchArena(threadIndex));

        SDL_LockMutex(jobQueueMutex);
        activeJobCount--;
        SDL_BroadcastCondition(jobQueueCond);
        SDL_UnlockMutex(jobQueueMutex);
    }

    LOGI("Worker thread %u exiting", threadIndex);

    return 0;
}

void JobSystemPushJob(void (*jobFunc)(void *data, memory_arena_t *arena), void *data)
{
    SDL_LockMutex(jobQueueMutex);
    while (jobCount >= MAX_JOBS) {
        SDL_WaitCondition(jobQueueCond, jobQueueMutex);
    }

    jobQueue[jobCount].jobFunc = jobFunc;
    jobQueue[jobCount].data = data;
    jobCount++;

    SDL_BroadcastCondition(jobQueueCond); //wake up one worker thread to process the new job
    SDL_UnlockMutex(jobQueueMutex); 
}

void JobSystemWaitForAllJobs(void)
{
    SDL_LockMutex(jobQueueMutex);
    while (activeJobCount > 0  || jobCount > 0) {
        SDL_WaitCondition(jobQueueCond, jobQueueMutex); 
    }
    SDL_BroadcastCondition(jobQueueCond);
    SDL_UnlockMutex(jobQueueMutex);
}

void JobSystemInit(void)
{
    //query the number of logical CPU cores and create that many worker threads. We can use SDL's thread API for this.
    threadCount = SDL_GetNumLogicalCPUCores() - 1;
    if (threadCount > MAX_THREADS) {
        LOGW("Thread count (%u) is larger than MAX_THREADS (%u). Capping thread count to MAX_THREADS.", threadCount, MAX_THREADS);
        threadCount = MAX_THREADS;
    }

    jobQueueMutex = SDL_CreateMutex();
    LV_ASSERT(jobQueueMutex && "Failed to create job queue mutex");
    jobQueueCond = SDL_CreateCondition();
    LV_ASSERT(jobQueueCond && "Failed to create job queue condition variable");

    //start all threads with a dummy job to wait on a condition variable. We can signal the condition variable to wake up the threads when we have jobs to process.
    for (u32 i = 0; i < threadCount; i++) {
        threadIndices[i] = i;
        workerThreads[i] = SDL_CreateThread(WorkerThreadFunc, "WorkerThread", &threadIndices[i]);
        if (!workerThreads[i]) {
            LOGE("Failed to create worker thread %u: %s", i, SDL_GetError());
            //handle thread creation failure (e.g., clean up already created threads)
        }
    }

    memset(jobQueue, 0, sizeof(jobQueue));
    shutdownFlag = false;
    jobCount = 0;
}

void JobSystemDeinit(void)
{
    KillWorkerThreads();

    SDL_DestroyMutex(jobQueueMutex);
    jobQueueMutex = NULL;
    SDL_DestroyCondition(jobQueueCond);
    jobQueueCond = NULL;
}

