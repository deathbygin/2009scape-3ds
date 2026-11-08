// Terrain decoding and tile building, ported from the revision 530 client's software path.
#ifndef TERRAIN_H
#define TERRAIN_H

#include <stdint.h>
#include "js5.h"

#define SCENE_SIZE 104
#define SCENE_LEVELS 4
#define TILE_NO_COLOR 12345678

typedef struct {
	int saturation, weightedHue, lightness, chroma;
} FluDef;

typedef struct {
	int baseColor, secondaryColor, texture;
	uint8_t occludeUnderlay;
} FloDef;

typedef struct {
	FluDef* flu;
	FloDef* flo;
	uint32_t fluCount, floCount;
	uint16_t* textureAverage;
	uint8_t* textureRepeating;
	uint32_t textureCount;
} TerrainTypes;

typedef struct {
	int colorSW, colorSE, colorNE, colorNW, flatColor, texture;
	uint8_t flat;
} PlainTile;

typedef struct {
	int texture, flatColor, underlayRgb;
	int vx[6], vy[6], vz[6];
	int ca[6], cb[6], cc[6], tex[6];
	uint8_t ta[6], tb[6], tc[6];
	uint8_t shape, rotation, flat, vertexCount, triangleCount;
} ShapedTile;

typedef struct {
	int32_t heights[SCENE_LEVELS][105][105];
	uint8_t underlay[SCENE_LEVELS][SCENE_SIZE][SCENE_SIZE];
	uint8_t overlay[SCENE_LEVELS][SCENE_SIZE][SCENE_SIZE];
	uint8_t shape[SCENE_LEVELS][SCENE_SIZE][SCENE_SIZE];
	uint8_t angle[SCENE_LEVELS][SCENE_SIZE][SCENE_SIZE];
	uint8_t flags[SCENE_LEVELS][SCENE_SIZE][SCENE_SIZE];
	int8_t shadow[SCENE_LEVELS][105][105];
	int32_t plainIndex[SCENE_LEVELS][SCENE_SIZE][SCENE_SIZE];
	int32_t shapedIndex[SCENE_LEVELS][SCENE_SIZE][SCENE_SIZE];
	PlainTile* plain;
	ShapedTile* shaped;
	uint32_t plainCount, plainCapacity, shapedCount, shapedCapacity;
	int originX, originY; // world tile at scene position 0,0
} TerrainScene;

int terrain_load_types(Js5Cache* cache, TerrainTypes* types);
void terrain_free_types(TerrainTypes* types);

// brightness is the display gamma exponent; the client uses 0.6 to 0.9.
void terrain_build_palette(uint32_t* palette, double brightness);

void terrain_scene_init(TerrainScene* scene, int originX, int originY);
void terrain_scene_free(TerrainScene* scene);

// Reads one 64x64 map file placed at local position localX, localY. Returns 0 on success.
int terrain_read_region(TerrainScene* scene, const uint8_t* data, size_t size, int localX, int localY);

// Fills a region that has no map file, as the client does.
void terrain_clear_region(TerrainScene* scene, int level, int localX, int localY);

// Builds tiles. Shadows must already be in scene->shadow.
int terrain_build(TerrainScene* scene, const TerrainTypes* types, const uint32_t* palette,
	int hueJitter, int lightnessJitter, int allLevelsVisible, int centralPlane);

#endif
