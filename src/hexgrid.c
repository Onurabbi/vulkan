#include "hexgrid.h"
#include "og_ds.h"
#include "platform.h"

static const float outerRadius = 10.0f;
static const float innerRadius = outerRadius * 0.866025404f;

void InitHexgrid(hexgrid_t *grid, u32 width, u32 height)
{
    ArrayInitWithArena(grid->cells, PermanentArena(), width * height);
    grid->width = width;
    grid->height = height;

    for (u32 row = 0; row < height; row++) {
        for (u32 col = 0; col < width; col++) {
            hexcell_t *cell = &grid->cells[row * width + col];
            cell->position.X = col * 10.0f;
            cell->position.Y = 0;
            cell->position.Z = row * 10.0f;
        }
    }
}

