#include "src/common.h"
#include "src/limits.h"

#include "src/vulkan/volk.h"
#include "src/log.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    RETCODE_SUCCESS = 0,
    RETCODE_NOMEM,
    RETCODE_SDL_INIT,
    RETCODE_THREADING_INIT,
    RETCODE_FILE_IO_FAILED,
    RETCODE_VULKAN_INIT,
} platform_return_code_t;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    u32 queueFamily;
} vulkan_presentation_query_t;

typedef struct {
    SDL_Window *window;
    VkSurfaceKHR surface;
    i32 w,h;
} vulkan_window_t;

#define GAME_MEMORY_SIZE GIGABYTES(4)

typedef struct {
    void (*jobFunc)(void *data, memory_arena_t *scratchArena);
    void *data;
} job_t;

static void (*GameUpdate)(game_memory_t *, game_input_t *) = NULL;
static void (*GameRender)(game_memory_t *) = NULL;
static void (*GameShutdown)(game_memory_t *) = NULL;
static void (*GameHotReload)(game_memory_t *) = NULL;

static char gameLibPath[MAX_PATH];
static time_t lastModTime;
static SDL_SharedObject *gameLib;

static SDL_Thread *workerThreads[MAX_THREADS];
static u32 threadIndices[MAX_THREADS];
static u32 threadCount;

static SDL_Mutex *mutex;
static SDL_Condition *cond;
static job_t queue[MAX_JOBS];
static u32 jobCount;
static u32 activeJobCount;
static b8 shutdownFlag;

static memory_arena_t permanentArena;
static memory_arena_t stringArena;
static memory_arena_t scratchArenas[MAX_THREADS]; //threadCount + 1;
static u32 arenaCount;

static u32 reloadIndex = 0;

static _Thread_local u32 threadIndex = 0;

static void ArenaFreeToMarker(memory_arena_t *arena, u64 marker)
{
    if (marker > arena->used) {
        LOGE("Marker is larger than arena used. %zu vs %zu", marker, arena->used);
    }
    arena->used = marker;
}

static u64 ArenaGetMarker(memory_arena_t *arena)
{
    return arena->used;
}

static void *ArenaPushSize(memory_arena_t *arena, u64 size)
{
    LV_ASSERT(arena->used + size <= arena->size);
    void *base = (u8*)arena->base + arena->used;
    arena->used += size;
    return base;
}

static const char *ArenaPrintf(memory_arena_t *arena, const char *fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);
    i32 n = vsnprintf(NULL, 0, fmt, args2);
    va_end(args2);

    char *buffer = ArenaPushSize(arena, n + 1);
    vsnprintf(buffer, n + 1, fmt, args);
    return buffer;
}

static memory_arena_t *PermanentArena(void)
{
    return &permanentArena;
}

static memory_arena_t *ScratchArena(void)
{
    return &scratchArenas[threadIndex];
}

static memory_arena_t *StringArena(void)
{
    return &stringArena;
}

static void ArenaInit(void *base, memory_arena_t *arena, u64 memSize)
{
    arena->base = base;
    arena->size = memSize;
    arena->used = 0;
}

static void ArenaDeinit(memory_arena_t *arena)
{
    arena->base = NULL;
    arena->size = 0;
    arena->used = 0;
}

static void KillWorkerThreads(void)
{
    SDL_LockMutex(mutex);
    while (jobCount > 0) {
        SDL_WaitCondition(cond, mutex);
    }

    shutdownFlag = true;
    SDL_BroadcastCondition(cond);
    SDL_UnlockMutex(mutex);

    for (u32 i = 0; i < threadCount; i++) {
        i32 status;
        SDL_WaitThread(workerThreads[i], &status);
        LOGI("Worker thread %u exited with status: %d", i, status);
        workerThreads[i] = NULL;
    }
}

static i32 WorkerThreadFunc(void *data)
{
    threadIndex = *(u32*)data;
    LOGI("Worker thread %u started", threadIndex);

    //worker thread main loop
    while (true) {
        SDL_LockMutex(mutex);

        while (jobCount == 0 && !shutdownFlag) {
            SDL_WaitCondition(cond, mutex);
        }

        if (shutdownFlag) {
            SDL_BroadcastCondition(cond);
            SDL_UnlockMutex(mutex);
            break;
        }

        job_t job = queue[0];
        //shift the remaining jobs in the queue forward
        for (u32 i = 1; i < jobCount; i++) {
            queue[i - 1] = queue[i];
        }

        jobCount--;
        activeJobCount++;

        SDL_BroadcastCondition(cond); 
        SDL_UnlockMutex(mutex);

        job.jobFunc(job.data, &scratchArenas[threadIndex]);

        SDL_LockMutex(mutex);
        activeJobCount--;
        SDL_BroadcastCondition(cond);
        SDL_UnlockMutex(mutex);
    }

    LOGI("Worker thread %u exiting", threadIndex);

    return 0;
}

static void PushJob(void (*jobFunc)(void *data, memory_arena_t *scratchArena), void *data)
{
    SDL_LockMutex(mutex);
    while (jobCount >= MAX_JOBS) {
        SDL_WaitCondition(cond, mutex);
    }

    queue[jobCount].jobFunc = jobFunc;
    queue[jobCount].data = data;
    jobCount++;

    SDL_BroadcastCondition(cond); //wake up one worker thread to process the new job
    SDL_UnlockMutex(mutex); 
}

static void WaitForAllJobs(void)
{
    SDL_LockMutex(mutex);
    while (activeJobCount > 0  || jobCount > 0) {
        SDL_WaitCondition(cond, mutex); 
    }
    SDL_BroadcastCondition(cond);
    SDL_UnlockMutex(mutex);
}

static b8 VulkanGetPresentationSupport(vulkan_instance_t instance, vulkan_physical_device_t physicalDevice, u32 queue)
{
    return SDL_Vulkan_GetPresentationSupport(instance, physicalDevice, queue);
}

static b8 CreateWindow(void *data, vulkan_instance_t instance, const char *title, i32 w, i32 h, u64 flags)
{
    vulkan_window_t *window = (vulkan_window_t *)data;
    window->w = w;
    window->h = h;

    window->window = SDL_CreateWindow(title, w, h, flags);
    if (!window->window) {
        LOGE("Unable to create SDL window. SDL Error: %s", SDL_GetError());
        return false;
    }

    if (!SDL_SetWindowRelativeMouseMode(window->window, true)) {
        LOGE("Unable to set relative mouse mode. SDL Error: %s", SDL_GetError());
    }

    if (!SDL_Vulkan_CreateSurface(window->window, instance, NULL, &window->surface)) {
        LOGE("Unable to create vulkan surface. SDL_Error: %s", SDL_GetError());
        SDL_DestroyWindow(window->window);
        return false;
    }

    return true;
}

static b8 LoadLibrary(const char *path)
{
    SDL_SharedObject *handle = SDL_LoadObject(path);
    if (!path) {
        LOGE("Failed to load %s: %s", path, SDL_GetError());
        return false;
    }

    SDL_FunctionPointer update = SDL_LoadFunction(handle, "Update");
    SDL_FunctionPointer render = SDL_LoadFunction(handle, "Render");
    SDL_FunctionPointer shutdown = SDL_LoadFunction(handle, "Shutdown");
    SDL_FunctionPointer hotReload = SDL_LoadFunction(handle, "HotReload");

    if (!update || !render || !shutdown) {
        LOGE("Hot reload failed: Missing symbols in %s", path);
        SDL_UnloadObject(handle);
        return false;
    }

    gameLib = handle;

    GameUpdate = (void (*)(game_memory_t *, game_input_t*))update;
    GameRender = (void(*)(game_memory_t *))render;
    GameShutdown = (void(*)(game_memory_t *))shutdown;
    GameHotReload = (void(*)(game_memory_t *))hotReload;

    struct stat sb;
    if (stat(path, &sb) == 0) {
        lastModTime = sb.st_mtime;
    }
    return true;
}

static b8 LoadGameLibrary(const char *path, SDL_SharedObject *oldHandle)
{
    struct stat sb;
    // Ensure file exists and isn't empty (compiler might still be writing)
    if (stat(path, &sb) != 0 || sb.st_size == 0) {
        return false;
    }

    if (!oldHandle) {
        return LoadLibrary(path);       
    }

    SDL_UnloadObject(oldHandle);
    
    gameLib = NULL;
    GameUpdate = NULL;
    GameRender = NULL;
    GameShutdown = NULL;
    GameHotReload = NULL;

    size_t size;
    void *data = SDL_LoadFile(path, &size);
    if (!data) {
        LOGE("Failed to read game library %s: %s", path, SDL_GetError());
        return false;
    }

    reloadIndex++;
    char loadPath[MAX_PATH];
    snprintf(loadPath, sizeof(loadPath), "%s%u", path, reloadIndex);

    SDL_IOStream *dest = SDL_IOFromFile(loadPath, "wb");
    if (!dest) {
        LOGE("Failed to create load copy %s: %s", loadPath, SDL_GetError());
        SDL_free(data);
        return false;
    }
    SDL_WriteIO(dest, data, size);
    SDL_CloseIO(dest);
    SDL_free(data);

    if (!LoadLibrary(loadPath)) {
        LOGE("Failed to load game library %s", loadPath);
        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    platform_return_code_t retCode = RETCODE_SUCCESS;
    
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGE("Unable to init SDL. Error: %s", SDL_GetError());
        retCode = -RETCODE_SDL_INIT;
        goto exit;
    }

    threadCount = SDL_GetNumLogicalCPUCores() - 1;
    u64 memorySize = GAME_MEMORY_SIZE;
    void *memoryBlock = SDL_malloc(memorySize);
    if (!memoryBlock) {
        LOGE("We're absolutely cooked. No memory!");
        retCode = -RETCODE_NOMEM;
    }
    
    //clear the entire block to zero. ZII style
    memset(memoryBlock, 0, memorySize);
    game_memory_t *gameMemory = (game_memory_t *)memoryBlock;
    u8* ptr = (u8*)(memoryBlock + sizeof(*gameMemory));

    threadIndex = 0;
    arenaCount = threadCount + 1;

    ArenaInit(ptr, &permanentArena, MAIN_PERMANENT_ARENA_CAPACITY);
    ptr += MAIN_PERMANENT_ARENA_CAPACITY;

    ArenaInit(ptr, &scratchArenas[0], MAIN_SCRATCH_ARENA_CAPACITY);
    ptr += MAIN_SCRATCH_ARENA_CAPACITY;

    for (u32 i = 1; i < arenaCount; i++) {
        ArenaInit(ptr, &scratchArenas[i], WORKER_SCRATCH_ARENA_CAPACITY);
        ptr += WORKER_SCRATCH_ARENA_CAPACITY;
    }

    ArenaInit(ptr, &stringArena, STRING_ARENA_CAPACITY);
    ptr += STRING_ARENA_CAPACITY;

    LOGI("Total memory used by the game: %zu", (u64)ptr - (u64)memoryBlock);

    gameMemory->api.vulkanInstanceExtensions = SDL_Vulkan_GetInstanceExtensions(&gameMemory->api.vulkanInstanceExtensionCount);
    gameMemory->api.VulkanGetPresentationSupport = VulkanGetPresentationSupport;
    gameMemory->api.CreateWindow = CreateWindow;
    gameMemory->api.PushJob = PushJob;
    gameMemory->api.WaitForAllJobs = WaitForAllJobs;
    gameMemory->api.ArenaPushSize = ArenaPushSize;
    gameMemory->api.ArenaFreeToMarker = ArenaFreeToMarker;
    gameMemory->api.ArenaGetMarker = ArenaGetMarker;
    gameMemory->api.ArenaPrintf = ArenaPrintf;
    gameMemory->api.PermanentArena = PermanentArena;
    gameMemory->api.ScratchArena = ScratchArena;
    gameMemory->api.StringArena = StringArena;
#if defined (RENDERER_VULKAN)
    gameMemory->api.rendererType = RENDERER_TYPE_VULKAN;
#endif
    gameMemory->threadCount = threadCount;
    gameMemory->renderMarker = 0;
    gameMemory->updateMarker = 0;

    //Init threads
    mutex = SDL_CreateMutex();
    if (!mutex) {
        LOGE("Failed to create job queue mutex");
        retCode = -RETCODE_THREADING_INIT;
        goto exit;
    }

    cond = SDL_CreateCondition();
    if (!cond) {
        LOGE("Failed to create job queue condition");
        retCode = -RETCODE_THREADING_INIT;
        goto exit;
    }

    for (u32 i = 1; i <= threadCount; i++) {
        threadIndices[i - 1] = i;
        workerThreads[i - 1] = SDL_CreateThread(WorkerThreadFunc, "WorkerThread", &threadIndices[i - 1]);
        if (!workerThreads[i - 1]) {
            LOGE("Failed to create thread: %u: %s", i, SDL_GetError());
            retCode = -RETCODE_THREADING_INIT;
            goto exit;
        }
    }

    //Load Vulkan loader library
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        LOGE("Unable to initialize vulkan loader library! SDL_Error: %s", SDL_GetError());
        retCode = -RETCODE_VULKAN_INIT;
        goto exit;
    }

#if defined LV_PLATFORM_LINUX
    const char *libName = "./libGame.so";
#elif defined LV_PLATFORM_WINDOWS
    const char *libName = ".\\libGame.dll";
#else
#endif

    if (!LoadGameLibrary(libName, NULL)) {
        retCode = -RETCODE_FILE_IO_FAILED;
        goto exit;
    }

    game_input_t gameInput = {0};
    
    u64 freq = SDL_GetPerformanceFrequency();
    u64 prev = SDL_GetPerformanceCounter();
    f64 accumulator = 1 / 59.0; // Hack to be able to update on the first frame before rendering
    f64 libraryLoadTimer = 0;

    // Initialize the update and render markers for the first time
    gameMemory->updateMarker = 0;
    gameMemory->renderMarker = 0;

    while (!gameInput.quit) {
        u64 now = SDL_GetPerformanceCounter();
        u64 elapsed = now - prev;
        prev = now;

        f64 sec = 1.0 / ((f64)freq / (f64)elapsed);
        accumulator += sec;

        while (accumulator > 1.0 / 61.0) {
            f64 deltaTime = 1.0 / 59.0;

            memset(&gameInput, 0, sizeof(gameInput));
            gameInput.keyboardState = SDL_GetKeyboardState(NULL);
            SDL_GetRelativeMouseState(&gameInput.mouseX, &gameInput.mouseY);

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch(event.type) {
                    case SDL_EVENT_QUIT:
                        gameInput.quit = true;
                        break;
                    case SDL_EVENT_WINDOW_RESIZED:
                        gameInput.windowResized = true;
                        break;
                    case SDL_EVENT_KEY_UP:
                    case SDL_EVENT_KEY_DOWN:
                        gameInput.keyEvents[event.key.scancode].event = true;
                        gameInput.keyEvents[event.key.scancode].down = event.key.down;
                        gameInput.keyEvents[event.key.scancode].repeat = event.key.repeat;
                        break;
                    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                        LOGI("Gamepad button %d pressed", event.gbutton.button);
                        break;
                    case SDL_EVENT_GAMEPAD_BUTTON_UP:
                        LOGI("Gamepad button %d released", event.gbutton.button);
                        break;
                    default:
                        break;
                }
            }

            ArenaFreeToMarker(&permanentArena, gameMemory->updateMarker);

            if (GameUpdate) {
                GameUpdate(gameMemory, &gameInput);
            }

            gameMemory->renderMarker = ArenaGetMarker(&permanentArena);

            accumulator -= deltaTime;
            if (accumulator < 0) accumulator = 0;

            //check if the game library has been modified
            struct stat sb;
            if (stat(libName, &sb) == 0) {
                time_t modTime = sb.st_mtime;
                if (modTime > lastModTime) {
                    WaitForAllJobs();
                    if (LoadGameLibrary(libName, gameLib)) {
                        GameHotReload(gameMemory);
                    }
                }
            }
        }

        if (GameRender) {
            GameRender(gameMemory);
        }

        // We can release all scratch memory at this point since we don't except any scratch
        // memory to carry over to next frame
        for (u32 i = 0; i < gameMemory->threadCount; i++) {
            ArenaFreeToMarker(&scratchArenas[i], 0);
        }
    }

    if (GameShutdown) {
        GameShutdown(gameMemory);
    }

exit:
    WaitForAllJobs();
    KillWorkerThreads();
    
    ArenaDeinit(&permanentArena);

    for (u32 i = 0; i < arenaCount; i++) {
        ArenaDeinit(&scratchArenas[i]);
    }

    ArenaDeinit(&stringArena);

    if (mutex) SDL_DestroyMutex(mutex);
    if (cond) SDL_DestroyCondition(cond);

    //delete library files
    for (uint32_t i = 1; i <= reloadIndex; i++) {
        char loadPath[MAX_PATH];
        snprintf(loadPath, sizeof(loadPath), "%s%u", libName, i);
        if (!SDL_RemovePath(loadPath)) {
            LOGE("Unable to load library at path %s, Error: %s", loadPath, SDL_GetError());
        }
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_free(memoryBlock);
    SDL_UnloadObject(gameLib);
    SDL_Quit();

    LOGI("All good!");

    return retCode;
}
