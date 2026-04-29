#include "src/common.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>

typedef enum {
    RETCODE_SUCCESS = 0,
    RETCODE_NOMEM,
    RETCODE_SDL_INIT,
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

void (*GameInit)(game_memory_t *) = NULL;
void (*GameUpdate)(game_input_t *) = NULL;
void (*GameRender)(void) = NULL;
void (*GameShutdown)(void) = NULL;

b8 VulkanLoadLibrary(void)
{
    return SDL_Vulkan_LoadLibrary(NULL);
}

b8 VulkanGetPresentationSupport(vulkan_instance_t instance, vulkan_physical_device_t physicalDevice, u32 queue)
{
    return SDL_Vulkan_GetPresentationSupport(instance, physicalDevice, queue);
}

b8 CreateWindow(void *data, vulkan_instance_t instance, const char *title, i32 w, i32 h, u64 flags)
{
    vulkan_window_t *window = (vulkan_window_t *)data;
    window->w = w;
    window->h = h;

    window->window = SDL_CreateWindow(title, w, h, flags);
    if (!window->window) {
        fprintf(stderr, "Unable to create SDL window. SDL Error: %s\n", SDL_GetError());
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window->window, instance, NULL, &window->surface)) {
        fprintf(stderr, "Unable to create vulkan surface. SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window->window);
        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Unable to init SDL. Error: %s\n", SDL_GetError());
        return -RETCODE_SDL_INIT;
    }
    
    game_memory_t mem = {0};
    mem.threadCount = SDL_GetNumLogicalCPUCores() - 1;
    mem.memorySize = PERMANENT_ARENA_CAPACITY + mem.threadCount * SCRATCH_ARENA_CAPACITY + STRING_ARENA_CAPACITY;
    mem.memoryBase = SDL_malloc(mem.memorySize);
    if (!mem.memoryBase) {
        fprintf(stderr, "We're absolutely cooked. No memory!\n");
        return -RETCODE_NOMEM;
    }
    
    //Load Vulkan loader library
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        fprintf(stderr, "Unable to initialize vulkan loader library! SDL_Error: %s\n", SDL_GetError());
        return -RETCODE_VULKAN_INIT;
    }

    u32 instanceExtensionCount;
    mem.vulkanInstanceExtensions = SDL_Vulkan_GetInstanceExtensions(&instanceExtensionCount);
    mem.vulkanInstanceExtensionCount = instanceExtensionCount;
    mem.api.VulkanGetPresentationSupport = VulkanGetPresentationSupport;
    mem.api.CreateWindow = CreateWindow;
    
    const char *platformStr = SDL_GetPlatform();
    const char *libName = NULL;
    if (strncmp(platformStr, "Windows", 7) == 0) {
        libName = "libGame.dll";
    } else {
        libName = "libGame.so";
    }

    char *directory = SDL_GetCurrentDirectory();
    char buf[MAX_PATH];
    int written = snprintf(buf, MAX_PATH, "%s/%s", directory, libName);
    if (written <= 0 || written >= sizeof(buf)) {
        fprintf(stderr, "Unable to write game library path");
        return -RETCODE_NOMEM;
    }

    SDL_SharedObject *gameLib = SDL_LoadObject(buf);
    if (!gameLib) {
        fprintf(stderr, "Failed to load %s, Error: %s\n", libName, SDL_GetError());
        return -RETCODE_SDL_INIT;
    }

    GameInit = (void (*)(game_memory_t*))SDL_LoadFunction(gameLib, "Init");
    GameUpdate = (void (*)(game_input_t*))SDL_LoadFunction(gameLib, "Update");
    GameRender = SDL_LoadFunction(gameLib, "Render");
    GameShutdown = SDL_LoadFunction(gameLib, "Shutdown");

    game_input_t gameInput = {0};

    u64 freq = SDL_GetPerformanceFrequency();
    u64 prev = SDL_GetPerformanceCounter();
    f64 accumulator = 0;

    GameInit(&mem);

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
            SDL_GetMouseState(&gameInput.mouseX, &gameInput.mouseY);

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
                        fprintf(stdout, "Gamepad button %d pressed\n", event.gbutton.button);
                        break;
                    case SDL_EVENT_GAMEPAD_BUTTON_UP:
                        fprintf(stdout, "Gamepad button %d released\n", event.gbutton.button);
                    default:
                        break;
                }
            }

            GameUpdate(&gameInput);
            accumulator -= deltaTime;
            if (accumulator < 0) accumulator = 0;
        }

        GameRender();
    }
    
    GameShutdown();
    
    SDL_free(mem.memoryBase);
    SDL_UnloadObject(gameLib);
    SDL_Quit();

    fprintf(stdout, "All good!\n");

    return RETCODE_SUCCESS;
}