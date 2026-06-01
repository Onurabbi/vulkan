#ifndef OG_GAME_H
#define OG_GAME_H

#include "common.h"

#include "string_utils.h"
#include "renderer.h"
#include "resource.h"

typedef struct {
    string_interning_system_t stringInterning;
    renderer_t renderer;
    resource_system_t resourceSystem;
    b8 isInitialized;
    b8 windowResized;
}game_state_t;

#endif
