#include "vulkan/volk.h"

#include "common.h"
#include "game.h"
#include "platform.h"

#include "log.h"
#include "renderer.h"
#include "camera.h"

#include <SDL3/SDL_keyboard.h>

LV_EXPORT void HotReload(game_memory_t *gameMemory)
{
    game_state_t *gameState = (game_state_t *)gameMemory->gameState;
    PlatformHotReload(&gameMemory->api);
    StringUtilsHotReload(&gameState->stringInterning);
    ResourceSystemHotReload(&gameState->resourceSystem);
    RendererHotReload(&gameState->renderer);
}

LV_EXPORT void Update(game_memory_t *gameMemory, game_input_t *gameInput)
{
    game_state_t *gameState = gameMemory->gameState;
    if (!gameState || !gameState->isInitialized) {
        PlatformInit(&gameMemory->api);
        gameState = PushStruct(PermanentArena(), game_state_t);
        StringUtilsInit(&gameState->stringInterning);
        RendererInit(&gameState->renderer);
        ResourceSystemInit(&gameState->resourceSystem, MAX_RESOURCES);
        gameState->isInitialized = true;
        gameMemory->gameState = gameState;
        gameMemory->updateMarker = ArenaGetMarker(PermanentArena());
        gameMemory->renderMarker = 0;
    }

    // Handle key events
    for (u32 i = 0; i < SCANCODE_COUNT; i++) {
        if (gameInput->keyEvents[i].event) {
            if (gameInput->keyEvents[i].down) {
                if (i == SDL_SCANCODE_ESCAPE) {
                    gameInput->quit = true;
                }
            } else {
                LOGI("Button %u was released", i);
            }

            if (gameInput->keyEvents[i].repeat) {
            }
        }
    }

    // x direction is left-right, y direction is forward backward
    vec3_t cameraMovement = {0};
    // Handle key states
    if (gameInput->keyboardState[SDL_SCANCODE_W]) {
        cameraMovement.Y = 1.0f;
    } else if (gameInput->keyboardState[SDL_SCANCODE_S]) {
        cameraMovement.Y = -1.0f;
    }

    if (gameInput->keyboardState[SDL_SCANCODE_D]) {
        cameraMovement.X = 1.0f;
    } else if (gameInput->keyboardState[SDL_SCANCODE_A]) {
        cameraMovement.X = -1.0f;
    }
    
    if (gameInput->keyboardState[SDL_SCANCODE_Q]) {
        cameraMovement.Z = 1.0f;
    } else if (gameInput->keyboardState[SDL_SCANCODE_E]) {
        cameraMovement.Z = -1.0f;
    }

    CameraMove(&gameState->renderer.camera, cameraMovement, gameInput->mouseX, gameInput->mouseY);

    gameState->windowResized = gameInput->windowResized;
}

LV_EXPORT void Render(game_memory_t *gameMemory)
{
    ArenaFreeToMarker(PermanentArena(), gameMemory->renderMarker);
    RendererRender();
}

LV_EXPORT void Shutdown(game_memory_t *gameMemory)
{
    game_state_t *gameState = (game_state_t *)gameMemory->gameState;
    ResourceSystemShutdown();
    RendererShutdown();
    StringUtilsShutdown();
    PlatformShutdown();
}
