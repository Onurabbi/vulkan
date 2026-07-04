#include "vulkan/volk.h"

#include "common.h"
#include "game.h"
#include "platform.h"

#include "log.h"
#include "renderer.h"

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
        gameState = PushStruct(PermanentArena(0), game_state_t);
        StringUtilsInit(&gameState->stringInterning);
        RendererInit(&gameState->renderer);
        ResourceSystemInit(&gameState->resourceSystem, MAX_RESOURCES);
        gameState->isInitialized = true;
        gameMemory->gameState = gameState;
        for (u32 i = 0; i < gameMemory->threadCount; i++) {
            gameMemory->updateMarkers[i] = ArenaGetMarker(PermanentArena(i));
            gameMemory->renderMarkers[i] = 0;
        }
    }

    for (u32 i = 0; i < gameMemory->threadCount; i++) {
        ArenaFreeToMarker(PermanentArena(i), gameMemory->updateMarkers[i]);
    }

    for (u32 i = 0; i < SCANCODE_COUNT; i++) {
        if (gameInput->keyEvents[i].event) {
            if (gameInput->keyEvents[i].down) {
                LOGI("Button %u was pressed", i);
            } else {
                LOGI("Button %u was released", i);
            }

            if (gameInput->keyEvents[i].repeat) {
                LOGI("Button %u was repeated", i);
            }
        }
    }
    gameState->windowResized = gameInput->windowResized;
    //do updates e.g.
    for (u32 i = 0; i < gameMemory->threadCount; i++) {
        gameMemory->renderMarkers[i] = ArenaGetMarker(PermanentArena(i));
    }
}

LV_EXPORT void Render(game_memory_t *gameMemory)
{
    for (u32 i = 0; i < gameMemory->threadCount; i++) {
        ArenaFreeToMarker(PermanentArena(i), gameMemory->renderMarkers[i]);
    }

    RendererRender();
    
    // We can release all scratch memory at this point since we don't except any scratch
    // memory to carry over to next frame
    for (u32 i = 0; i < gameMemory->threadCount; i++) {
        ArenaFreeToMarker(ScratchArena(i), 0);
    }
}

LV_EXPORT void Shutdown(game_memory_t *gameMemory)
{
    game_state_t *gameState = (game_state_t *)gameMemory->gameState;
    ResourceSystemShutdown();
    RendererShutdown();
    StringUtilsShutdown();
    PlatformShutdown();
}
