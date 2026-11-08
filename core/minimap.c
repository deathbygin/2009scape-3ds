#include "minimap.h"

#include <string.h>

// Ported from the client's MiniMap. The image is 512x512 with the scene at offset 48, 4 pixels per tile.
// Map scene icons are not drawn yet: they need the sprite system.

static const uint8_t SHAPE_MASKS[13][16] = {
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}, {1,0,0,0,1,1,0,0,1,1,1,0,1,1,1,1},
	{1,1,0,0,1,1,0,0,1,0,0,0,1,0,0,0}, {0,0,1,1,0,0,1,1,0,0,0,1,0,0,0,1}, {0,1,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
	{1,1,1,0,1,1,1,0,1,1,1,1,1,1,1,1}, {1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0}, {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0},
	{1,1,1,1,1,1,1,1,0,1,1,1,0,0,1,1}, {1,1,1,1,1,1,0,0,1,0,0,0,1,0,0,0}, {0,0,0,0,0,0,1,1,0,1,1,1,0,1,1,1},
	{0,0,0,0,0,0,0,0,0,1,1,0,1,1,1,1}};
static const uint8_t SHAPE_ROTATIONS[4][16] = {
	{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, {12,8,4,0,13,9,5,1,14,10,6,2,15,11,7,3},
	{15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0}, {3,7,11,15,2,6,10,14,1,5,9,13,0,4,8,12}};

// A bridge moves each tile on the bridged square down one level.
static int tile_level(const Scene* s, int level, int x, int y) {
	if (s->terrain.flags[1][x][y] & 2) return level + 1 < SCENE_LEVELS ? level + 1 : -1;
	return level;
}

static void render_tile(const Scene* s, uint32_t* pixels, int index, int level, int x, int y) {
	const TerrainScene* t = &s->terrain;
	int actual = tile_level(s, level, x, y);
	if (actual < 0) return;
	int plain = t->plainIndex[actual][x][y];
	if (plain >= 0) {
		uint32_t color = (uint32_t)t->plain[plain].flatColor;
		if (color == 0) return;
		for (int row = 0; row < 4; row++, index += MINIMAP_SIZE)
			for (int i = 0; i < 4; i++) pixels[index + i] = color;
		return;
	}
	int shaped = t->shapedIndex[actual][x][y];
	if (shaped < 0) return;
	const ShapedTile* tile = &t->shaped[shaped];
	const uint8_t* mask = SHAPE_MASKS[tile->shape];
	const uint8_t* rotation = SHAPE_ROTATIONS[tile->rotation];
	uint32_t under = (uint32_t)tile->underlayRgb, over = (uint32_t)tile->flatColor;
	int m = 0;
	for (int row = 0; row < 4; row++, index += MINIMAP_SIZE) {
		for (int i = 0; i < 4; i++) {
			int solid = mask[rotation[m++]];
			if (under != 0) pixels[index + i] = solid == 0 ? under : over;
			else if (solid != 0) pixels[index + i] = over;
		}
	}
}

static void line(uint32_t* p, int start, int step, uint32_t color) {
	for (int i = 0; i < 4; i++) p[start + i * step] = color;
}

static void render_features(const Scene* s, uint32_t* pixels, int level, int x, int y) {
	int actual = tile_level(s, level, x, y);
	if (actual < 0) return;
	const SceneTile* tile = &s->tiles[actual][x][y];
	int offset = (103 - y) * 4 * MINIMAP_SIZE + x * 4 + 48 * MINIMAP_SIZE + 48;
	const SceneWall* w = tile->wall;
	if (w && !w->hasMapscene) {
		uint32_t color = w->interactable ? MINIMAP_RED : MINIMAP_WHITE;
		int shape = w->shape, r = w->orientation;
		const int row = MINIMAP_SIZE;
		if (shape == 0 || shape == 2) {
			if (r == 0) line(pixels, offset, row, color);
			else if (r == 1) line(pixels, offset, 1, color);
			else if (r == 2) line(pixels, offset + 3, row, color);
			else line(pixels, offset + 3 * row, 1, color);
		}
		if (shape == 3) {
			if (r == 0) pixels[offset] = color;
			else if (r == 1) pixels[offset + 3] = color;
			else if (r == 2) pixels[offset + 3 + 3 * row] = color;
			else pixels[offset + 3 * row] = color;
		}
		if (shape == 2) {
			if (r == 3) line(pixels, offset, row, color);
			else if (r == 0) line(pixels, offset, 1, color);
			else if (r == 1) line(pixels, offset + 3, row, color);
			else line(pixels, offset + 3 * row, 1, color);
		}
	}
	for (int i = 0; i < tile->sceneryCount; i++) {
		const SceneScenery* c = tile->scenery[i];
		if (c->shape != 9 || c->hasMapscene || c->xMin != x || c->yMin != y) continue;
		uint32_t color = c->interactable ? MINIMAP_RED : MINIMAP_WHITE;
		const int row = MINIMAP_SIZE;
		if (c->orientation == 0 || c->orientation == 2) {
			pixels[offset + 3 * row] = color;
			pixels[offset + 2 * row + 1] = color;
			pixels[offset + row + 2] = color;
			pixels[offset + 3] = color;
		} else {
			pixels[offset] = color;
			pixels[offset + row + 1] = color;
			pixels[offset + 2 * row + 2] = color;
			pixels[offset + 3 * row + 3] = color;
		}
		break;
	}
}

void minimap_render(const Scene* s, int plane, uint32_t* pixels) {
	const TerrainScene* t = &s->terrain;
	for (int i = 0; i < MINIMAP_SIZE * MINIMAP_SIZE; i++) pixels[i] = 1;
	for (int y = 1; y < 103; y++) {
		int offset = 4 * MINIMAP_SIZE * (103 - y) + 24628;
		for (int x = 1; x < 103; x++, offset += 4) {
			if ((t->flags[plane][x][y] & 0x18) == 0) render_tile(s, pixels, offset, plane, x, y);
			if (plane < 3 && (t->flags[plane + 1][x][y] & 8) != 0) render_tile(s, pixels, offset, plane + 1, x, y);
		}
	}
	for (int y = 1; y < 103; y++) {
		for (int x = 1; x < 103; x++) {
			if ((t->flags[plane][x][y] & 0x18) == 0) render_features(s, pixels, plane, x, y);
			if (plane < 3 && (t->flags[plane + 1][x][y] & 8) != 0) render_features(s, pixels, plane + 1, x, y);
		}
	}
}
