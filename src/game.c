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
        gameState = PushStruct(PermanentArena(), game_state_t);
        StringUtilsInit(&gameState->stringInterning);
        RendererInit(&gameState->renderer);
        ResourceSystemInit(&gameState->resourceSystem, MAX_RESOURCES);
        gameState->isInitialized = true;
        gameMemory->gameState = gameState;
        gameMemory->updateMarker = ArenaGetMarker(PermanentArena());
        gameMemory->renderMarker = 0;
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
