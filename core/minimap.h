// Draws the map image the client shows in its minimap.
#ifndef MINIMAP_H
#define MINIMAP_H

#include "scene.h"

#define MINIMAP_SIZE 512
#define MINIMAP_WHITE 0xEEEEEEu
#define MINIMAP_RED 0xEE0000u

// pixels must hold MINIMAP_SIZE * MINIMAP_SIZE values.
void minimap_render(const Scene* s, int plane, uint32_t* pixels);

#endif
