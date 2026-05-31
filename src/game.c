#include "volk.h"

#include "common.h"
#include "game.h"
#include "platform.h"

#include "memory.h"
#include "log.h"
#include "renderer.h"

LV_EXPORT void HotReload(game_memory_t *gameMemory)
{
    game_state_t *gameState = (game_state_t *)gameMemory->gameState;
    PlatformHotReload(&gameMemory->api);
    StringInterningHotReload(&gameState->stringInterning);
    RendererHotReload(&gameState->renderer);
}

LV_EXPORT void Update(game_memory_t *gameMemory, game_input_t *gameInput)
{
    game_state_t *gameState = gameMemory->gameState;
    if (!gameState || !gameState->isInitialized) {
        PlatformInit(&gameMemory->api);
        gameState = PushStruct(PermanentArena(), game_state_t);
        StringInterningInit(&gameState->stringInterning);
        RendererInit(&gameState->renderer);

        gameState->isInitialized = true;
        gameMemory->gameState = gameState;
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
    game_state_t *gameState = (game_state_t *)gameMemory->gameState;
    RendererRender();
}

LV_EXPORT void Shutdown(game_memory_t *gameMemory)
{
    game_state_t *gameState = (game_state_t *)gameMemory->gameState;
    RendererShutdown();
    StringInterningDeinit();
    PlatformShutdown();
}
