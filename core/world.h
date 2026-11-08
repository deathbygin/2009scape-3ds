// Loads the map around a world tile from the cache, as the client does when the server sends a new region.
#ifndef WORLD_H
#define WORLD_H

#include "mesh.h"

typedef struct {
	Js5Cache cache;
	TerrainTypes types;
	uint32_t palette[65536];
	int32_t (*keys)[4];
	uint8_t* hasKey;
} World;

typedef struct {
	long readMs, locsMs, tilesMs, mergeMs;
	long objects;
} WorldTiming;

typedef long (*WorldClock)(void);

// keysPath is a JSON file of region keys. On the live server these will arrive over the network instead.
int world_open(World* w, const char* cacheDir, const char* keysPath, double brightness);
void world_close(World* w);

// Builds the scene centred on a world tile. originX and originY give the world tile at scene position 0,0.
int world_build(World* w, Scene* scene, int tileX, int tileY, WorldClock clock, WorldTiming* timing);

// Builds the scene for a zone, with region keys from the server. With keys NULL, the key file is used.
int world_build_zone(World* w, Scene* scene, int zoneX, int zoneY, const int32_t (*keys)[4], int keyCount, WorldClock clock, WorldTiming* timing);

#endif
