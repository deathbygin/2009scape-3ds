// A compact list of the objects a player can interact with, kept after the scene is freed.
#ifndef PICK_H
#define PICK_H

#include "scene.h"

typedef struct {
	const LocDef* def; // the definition shown to the player, after any multi-state lookup
	int id;            // the id sent to the server
	int16_t minX, maxX, minY, maxY, minZ, maxZ; // bounds in game units
	int16_t tileX, tileY;
	uint8_t plane, level, layer;
	uint8_t hidden, dynamic; // removed by the server, or added by it
} PickObject;

// Fills out with interactive objects. Returns how many were written.
int pick_build(Scene* s, PickObject* out, int max);

#endif
