#ifndef OG_HEXGRID_H
#define OG_HEXGRID_H

#include "common.h"
#include <HandmadeMath.h>

typedef struct {
    vec3_t position;
} hexcell_t;

typedef struct hexgrid_t {
    hexcell_t *cells;
    u32 width;
    u32 height;
} hexgrid_t;

void InitHexgrid(hexgrid_t *grid, u32 width, u32 height);

#endif
