#include "terrain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---- small helpers ----

typedef struct {
	const uint8_t* p;
	const uint8_t* end;
} Buf;

static int g1(Buf* b) { return b->p < b->end ? *b->p++ : 0; }
static int g1s(Buf* b) { return (int8_t)g1(b); }
static int g2(Buf* b) { int a = g1(b); return a << 8 | g1(b); }
static int g3(Buf* b) { int a = g2(b); return a << 8 | g1(b); }

static int cosTable[2048];
static int cosReady = 0;

static void ensure_cos(void) {
	if (cosReady) return;
	for (int i = 0; i < 2048; i++) cosTable[i] = (int)(cos(i * 0.0030679615) * 65536.0);
	cosReady = 1;
}

// ---- colour ----

static int rgb_to_hsl(int rgb) {
	double r = (rgb >> 16 & 0xFF) / 256.0, g = (rgb >> 8 & 0xFF) / 256.0, b = (rgb & 0xFF) / 256.0;
	double hue = 0, saturation = 0, min = r, max = r;
	if (r > g) min = g;
	if (b < min) min = b;
	if (g > r) max = g;
	if (b > max) max = b;
	double lightness = (min + max) / 2.0;
	if (min != max) {
		if (lightness < 0.5) saturation = (max - min) / (min + max);
		if (lightness >= 0.5) saturation = (max - min) / (2.0 - max - min);
		if (max == r) hue = (g - b) / (max - min);
		else if (g == max) hue = (b - r) / (max - min) + 2.0;
		else if (max == b) hue = (r - g) / (max - min) + 4.0;
	}
	int sat = (int)(saturation * 256.0), light = (int)(lightness * 256.0);
	hue /= 6.0;
	if (light < 0) light = 0;
	else if (light > 255) light = 255;
	int h = (int)(hue * 256.0);
	if (sat < 0) sat = 0;
	else if (sat > 255) sat = 255;
	if (light > 243) sat >>= 4;
	else if (light > 217) sat >>= 3;
	else if (light > 192) sat >>= 2;
	else if (light > 179) sat >>= 1;
	return (h >> 2 << 10) + (sat >> 5 << 7) + (light >> 1);
}

static int pack_hsl(int lightness, int saturation, int hue) {
	if (lightness > 243) saturation >>= 4;
	else if (lightness > 217) saturation >>= 3;
	else if (lightness > 192) saturation >>= 2;
	else if (lightness > 179) saturation >>= 1;
	return (lightness >> 1) + (saturation >> 5 << 7) + (hue >> 2 << 10);
}

static int clamp_light(int v) {
	return v < 2 ? 2 : v > 126 ? 126 : v;
}

static int multiply_safe(int multiplier, int hsl) {
	if (hsl == -1) return TILE_NO_COLOR;
	return clamp_light(multiplier * (hsl & 0x7F) >> 7) + (hsl & 0xFF80);
}

static int multiply_gray(int hsl, int lightness) {
	if (hsl == -2) return TILE_NO_COLOR;
	if (hsl == -1) return clamp_light(lightness);
	return (hsl & 0xFF80) + clamp_light((hsl & 0x7F) * lightness >> 7);
}

void terrain_build_palette(uint32_t* palette, double brightness) {
	int offset = 0;
	for (int y = 0; y < 512; y++) {
		double hue = (y >> 3) / 64.0 + 0.0078125, saturation = (y & 7) / 8.0 + 0.0625;
		for (int x = 0; x < 128; x++) {
			double lightness = x / 128.0, r = lightness, g = lightness, b = lightness;
			double q = lightness < 0.5 ? lightness * (saturation + 1.0) : lightness + saturation - lightness * saturation;
			double p = lightness * 2.0 - q, t = hue + 0.3333333333333333, u = hue - 0.3333333333333333;
			if (t > 1.0) t--;
			if (u < 0.0) u++;
			if (t * 6.0 < 1.0) r = p + (q - p) * 6.0 * t;
			else if (t * 2.0 < 1.0) r = q;
			else if (t * 3.0 < 2.0) r = p + (q - p) * (0.6666666666666666 - t) * 6.0;
			else r = p;
			if (hue * 6.0 < 1.0) g = p + (q - p) * 6.0 * hue;
			else if (hue * 2.0 < 1.0) g = q;
			else if (hue * 3.0 < 2.0) g = p + (q - p) * (0.6666666666666666 - hue) * 6.0;
			else g = p;
			if (u * 6.0 < 1.0) b = p + (q - p) * 6.0 * u;
			else if (u * 2.0 < 1.0) b = q;
			else if (u * 3.0 < 2.0) b = p + (q - p) * (0.6666666666666666 - u) * 6.0;
			else b = p;
			int ir = (int)(pow(r, brightness) * 256.0), ig = (int)(pow(g, brightness) * 256.0), ib = (int)(pow(b, brightness) * 256.0);
			int rgb = (ir << 16) + (ig << 8) + ib;
			palette[offset++] = rgb == 0 ? 1 : (uint32_t)rgb;
		}
	}
}

// ---- types ----

static void decode_flu(FluDef* d, Buf* b) {
	for (;;) {
		int op = g1(b);
		if (op == 0) return;
		if (op == 1) {
			int rgb = g3(b);
			double red = (rgb >> 16 & 0xFF) / 256.0, green = (rgb >> 8 & 0xFF) / 256.0, blue = (rgb & 0xFF) / 256.0;
			double min = red, max = red, hue = 0, sat = 0;
			if (green < red) min = green;
			if (blue < min) min = blue;
			if (green > red) max = green;
			if (blue > max) max = blue;
			double lum = (max + min) / 2.0;
			if (min != max) {
				if (lum < 0.5) sat = (max - min) / (max + min);
				if (max == red) hue = (green - blue) / (max - min);
				else if (green == max) hue = (blue - red) / (max - min) + 2.0;
				else if (blue == max) hue = (red - green) / (max - min) + 4.0;
				if (lum >= 0.5) sat = (max - min) / (2.0 - max - min);
			}
			d->chroma = lum > 0.5 ? (int)(sat * (1.0 - lum) * 512.0) : (int)(sat * lum * 512.0);
			if (d->chroma < 1) d->chroma = 1;
			d->saturation = (int)(sat * 256.0);
			d->lightness = (int)(lum * 256.0);
			if (d->lightness < 0) d->lightness = 0;
			else if (d->lightness > 255) d->lightness = 255;
			hue /= 6.0;
			d->weightedHue = (int)(d->chroma * hue);
			if (d->saturation < 0) d->saturation = 0;
			else if (d->saturation > 255) d->saturation = 255;
		} else if (op == 2 || op == 3) {
			g2(b);
		}
	}
}

static int flo_color(int rgb) {
	return rgb == 16711935 ? -1 : rgb_to_hsl(rgb);
}

static void decode_flo(FloDef* d, Buf* b) {
	for (;;) {
		int op = g1(b);
		if (op == 0) return;
		switch (op) {
		case 1: d->baseColor = flo_color(g3(b)); break;
		case 2: d->texture = g1(b); break;
		case 3:
			d->texture = g2(b);
			if (d->texture == 65535) d->texture = -1;
			break;
		case 5: d->occludeUnderlay = 0; break;
		case 7: d->secondaryColor = flo_color(g3(b)); break;
		case 9: g2(b); break;
		case 11: g1(b); break;
		case 13: g3(b); break;
		case 14: g1(b); break;
		default: break; // 8, 10 and 12 have no payload
		}
	}
}

static uint32_t group_capacity(const Js5Index* index, uint32_t group) {
	const Js5Group* g = &index->groups[group];
	if (!g->fileIds) return g->size;
	uint32_t max = 0;
	for (uint32_t i = 0; i < g->size; i++) if (g->fileIds[i] > max) max = g->fileIds[i];
	return max + 1;
}

int terrain_load_types(Js5Cache* cache, TerrainTypes* types) {
	memset(types, 0, sizeof(*types));
	int rc = js5_load_index(cache, 2);
	if (rc == 0) rc = js5_load_index(cache, 26);
	if (rc != 0) return rc;
	const Js5Index* config = &cache->index[2];
	Js5Buffer group;

	if ((rc = js5_read_group(cache, 2, 1, NULL, &group)) != 0) return rc;
	types->fluCount = group_capacity(config, 1);
	types->flu = calloc(types->fluCount, sizeof(FluDef));
	for (uint32_t i = 0; types->flu && i < types->fluCount; i++) {
		Js5Buffer file;
		if (js5_group_file(config, 1, &group, i, &file) != 0) continue;
		Buf b = {file.data, file.data + file.size};
		decode_flu(&types->flu[i], &b);
		free(file.data);
	}
	free(group.data);

	if ((rc = js5_read_group(cache, 2, 4, NULL, &group)) != 0) return rc;
	types->floCount = group_capacity(config, 4);
	types->flo = calloc(types->floCount, sizeof(FloDef));
	for (uint32_t i = 0; types->flo && i < types->floCount; i++) {
		FloDef* d = &types->flo[i];
		d->secondaryColor = -1;
		d->texture = -1;
		d->occludeUnderlay = 1;
		Js5Buffer file;
		if (js5_group_file(config, 4, &group, i, &file) != 0) continue;
		Buf b = {file.data, file.data + file.size};
		decode_flo(d, &b);
		free(file.data);
	}
	free(group.data);

	if ((rc = js5_read_group(cache, 26, 0, NULL, &group)) != 0) return rc;
	Buf b = {group.data, group.data + group.size};
	uint32_t n = (uint32_t)g2(&b);
	types->textureCount = n;
	types->textureAverage = calloc(n ? n : 1, sizeof(uint16_t));
	types->textureRepeating = calloc(n ? n : 1, 1);
	uint8_t* defined = calloc(n ? n : 1, 1);
	if (!types->flu || !types->flo || !types->textureAverage || !types->textureRepeating || !defined) return -20;
	for (uint32_t i = 0; i < n; i++) defined[i] = g1(&b) == 1;
	for (uint32_t i = 0; i < n; i++) if (defined[i]) types->textureRepeating[i] = g1(&b) == 1;
	// opaque, low detail, flipped, scroll, brightness, material and animation: seven byte tables not used here
	for (int table = 0; table < 7; table++)
		for (uint32_t i = 0; i < n; i++) if (defined[i]) g1(&b);
	for (uint32_t i = 0; i < n; i++) if (defined[i]) types->textureAverage[i] = (uint16_t)g2(&b);
	free(defined);
	free(group.data);
	return 0;
}

void terrain_free_types(TerrainTypes* types) {
	free(types->flu);
	free(types->flo);
	free(types->textureAverage);
	free(types->textureRepeating);
	memset(types, 0, sizeof(*types));
}

// ---- noise for tiles with no stored height ----

static int noise(int x, int y) {
	uint32_t n = (uint32_t)y * 57u + (uint32_t)x;
	uint32_t hash = n ^ n << 13;
	uint32_t result = 0x7FFFFFFFu & ((hash * hash * 15731u + 789221u) * hash + 1376312589u);
	return (int)(result >> 19 & 0xFF);
}

static int smooth_noise(int x, int y) {
	int corners = noise(x - 1, y - 1) + noise(x + 1, y - 1) + noise(x - 1, y + 1) + noise(x + 1, y + 1);
	int sides = noise(x - 1, y) + noise(x + 1, y) + noise(x, y - 1) + noise(x, y + 1);
	return sides / 8 + corners / 16 + noise(x, y) / 4;
}

static int interpolate(int a, int b, int t, int scale) {
	int weight = (65536 - cosTable[t * 1024 / scale]) >> 1;
	return (a * (65536 - weight) >> 16) + (b * weight >> 16);
}

static int interpolated_noise(int frequency, int x, int y) {
	int gridX = x / frequency, gridY = y / frequency;
	int fracY = y & (frequency - 1), fracX = x & (frequency - 1);
	int a = interpolate(smooth_noise(gridX, gridY), smooth_noise(gridX + 1, gridY), fracX, frequency);
	int b = interpolate(smooth_noise(gridX, gridY + 1), smooth_noise(gridX + 1, gridY + 1), fracX, frequency);
	return interpolate(a, b, fracY, frequency);
}

static int noise_height(int x, int y) {
	int height = interpolated_noise(4, y + 45365, x + 91923) + ((interpolated_noise(2, y + 10294, x + 37821) - 128) >> 1) +
		((interpolated_noise(1, y, x) - 128) >> 2) - 128;
	height = (int)(height * 0.3) + 35;
	return height < 10 ? 10 : height > 60 ? 60 : height;
}

// ---- reading ----

void terrain_scene_init(TerrainScene* scene, int originX, int originY) {
	memset(scene, 0, sizeof(*scene));
	memset(scene->plainIndex, 0xFF, sizeof(scene->plainIndex));
	memset(scene->shapedIndex, 0xFF, sizeof(scene->shapedIndex));
	scene->originX = originX;
	scene->originY = originY;
	ensure_cos();
}

void terrain_scene_free(TerrainScene* scene) {
	free(scene->plain);
	free(scene->shaped);
	scene->plain = NULL;
	scene->shaped = NULL;
}

static void read_tile(TerrainScene* s, Buf* b, int level, int x, int y) {
	if (x < 0 || x >= SCENE_SIZE || y < 0 || y >= SCENE_SIZE) {
		for (;;) {
			int op = g1(b);
			if (op == 0) break;
			if (op == 1) {
				g1(b);
				break;
			}
			if (op <= 49) g1(b);
		}
		return;
	}
	s->flags[level][x][y] = 0;
	for (;;) {
		int op = g1(b);
		if (op == 0) {
			if (level == 0) s->heights[0][x][y] = -noise_height(y + s->originY + 556238, s->originX + x + 932731) * 8;
			else s->heights[level][x][y] = s->heights[level - 1][x][y] - 240;
			return;
		}
		if (op == 1) {
			int height = g1(b);
			if (height == 1) height = 0;
			if (level == 0) s->heights[0][x][y] = -height * 8;
			else s->heights[level][x][y] = s->heights[level - 1][x][y] - height * 8;
			return;
		}
		if (op <= 49) {
			s->overlay[level][x][y] = (uint8_t)g1s(b);
			s->shape[level][x][y] = (uint8_t)((op - 2) / 4);
			s->angle[level][x][y] = (uint8_t)((op - 2) & 3);
		} else if (op > 81) {
			s->underlay[level][x][y] = (uint8_t)(op - 81);
		} else {
			s->flags[level][x][y] = (uint8_t)(op - 49);
		}
	}
}

int terrain_read_region(TerrainScene* scene, const uint8_t* data, size_t size, int localX, int localY) {
	Buf b = {data, data + size};
	for (int level = 0; level < SCENE_LEVELS; level++)
		for (int x = 0; x < 64; x++)
			for (int y = 0; y < 64; y++)
				read_tile(scene, &b, level, localX + x, localY + y);
	return 0;
}

void terrain_clear_region(TerrainScene* s, int level, int x, int y) {
	const int n = 64;
	int32_t (*h)[105] = s->heights[level];
	int32_t (*below)[105] = level > 0 ? s->heights[level - 1] : NULL;
	for (int i = x; i <= x + n; i++)
		for (int j = y; j <= y + n; j++)
			if (i >= 0 && i < SCENE_SIZE && j >= 0 && j < SCENE_SIZE) s->shadow[level][i][j] = 127;
	for (int i = x; i < x + n; i++)
		for (int j = y; j < y + n; j++)
			if (i >= 0 && i < SCENE_SIZE && j >= 0 && j < SCENE_SIZE) h[i][j] = level <= 0 ? 0 : below[i][j];
	if (x > 0 && x < SCENE_SIZE)
		for (int j = y + 1; j < y + n; j++)
			if (j >= 0 && j < SCENE_SIZE) h[x][j] = h[x - 1][j];
	if (y > 0 && y < SCENE_SIZE)
		for (int i = x + 1; i < x + n; i++)
			if (i >= 0 && i < SCENE_SIZE) h[i][y] = h[i][y - 1];
	if (x < 0 || y < 0 || x >= SCENE_SIZE || y >= SCENE_SIZE) return;
	if (level == 0) {
		if (x > 0 && h[x - 1][y] != 0) h[x][y] = h[x - 1][y];
		else if (y > 0 && h[x][y - 1] != 0) h[x][y] = h[x][y - 1];
		else if (x > 0 && y > 0 && h[x - 1][y - 1] != 0) h[x][y] = h[x - 1][y - 1];
	} else if (x > 0 && below[x - 1][y] != h[x - 1][y]) {
		h[x][y] = h[x - 1][y];
	} else if (y > 0 && h[x][y - 1] != below[x][y - 1]) {
		h[x][y] = h[x][y - 1];
	} else if (x > 0 && y > 0 && h[x - 1][y - 1] != below[x - 1][y - 1]) {
		h[x][y] = h[x - 1][y - 1];
	}
}

// ---- tile building ----

static const uint8_t VERTEX_INDICES[13][6] = {
	{1,3,5,7}, {1,3,5,7}, {1,3,5,7}, {1,3,5,7,6}, {1,3,5,7,6}, {1,3,5,7,6}, {1,3,5,7,6},
	{1,3,5,7,2,6}, {1,3,5,7,2,8}, {1,3,5,7,2,8}, {1,3,5,7,11,12}, {1,3,5,7,11,12}, {1,3,5,7,13,14}};
static const uint8_t VERTEX_COUNTS[13] = {4,4,4,5,5,5,5,6,6,6,6,6,6};
static const uint8_t TRIANGLE_INDICES[13][24] = {
	{0,1,2,3, 0,0,1,3}, {1,1,2,3, 1,0,1,3}, {0,1,2,3, 1,0,1,3},
	{0,0,1,2, 0,0,2,4, 1,0,4,3}, {0,0,1,4, 0,0,4,3, 1,1,2,4}, {0,0,4,3, 1,0,1,2, 1,0,2,4}, {0,1,2,4, 1,0,1,4, 1,0,4,3},
	{0,4,1,2, 0,4,2,5, 1,0,4,5, 1,0,5,3}, {0,4,1,2, 0,4,2,3, 0,4,3,5, 1,0,4,5}, {0,0,4,5, 1,4,1,2, 1,4,2,3, 1,4,3,5},
	{0,0,1,5, 0,1,4,5, 0,1,2,4, 1,0,5,3, 1,5,4,3, 1,4,2,3}, {1,0,1,5, 1,1,4,5, 1,1,2,4, 0,0,5,3, 0,5,4,3, 0,4,2,3},
	{1,0,5,4, 1,0,1,5, 0,0,4,3, 0,4,5,3, 0,5,2,3, 0,1,2,5}};
static const uint8_t TRIANGLE_COUNTS[13] = {2,2,2,3,3,3,3,4,4,4,6,6,6};

static void build_shaped(ShapedTile* t, int shape, int rotation, int overlayTexture, int tileX, int tileY,
	int hSW, int hSE, int hNE, int hNW, const int* under, const int* over, int underlayRgb, int flatColor) {
	t->flat = hSW == hSE && hSW == hNE && hSW == hNW;
	t->shape = (uint8_t)shape;
	t->rotation = (uint8_t)rotation;
	t->texture = overlayTexture;
	t->flatColor = flatColor;
	t->underlayRgb = underlayRgb; // the client stores this in a field it calls textureId
	const int size = 128, half = 64, quarter = 32, threeQuarter = 96;
	int bx = tileX * size, bz = tileY * size;
	int n = VERTEX_COUNTS[shape], uc[6], oc[6];
	int cSW = under[0], cSE = under[1], cNE = under[2], cNW = under[3];
	int oSW = over[0], oSE = over[1], oNE = over[2], oNW = over[3];
	t->vertexCount = (uint8_t)n;
	for (int v = 0; v < n; v++) {
		int code = VERTEX_INDICES[shape][v];
		if ((code & 1) == 0 && code <= 8) code = ((code - rotation - rotation - 1) & 7) + 1;
		if (code > 8 && code <= 12) code = ((code - rotation - 9) & 3) + 9;
		if (code > 12 && code <= 16) code = ((code - rotation - 13) & 3) + 13;
		int vx, vz, vy, c, o;
		switch (code) {
		case 1: vx = bx; vz = bz; vy = hSW; c = cSW; o = oSW; break;
		case 2: vx = bx + half; vz = bz; vy = (hSW + hSE) >> 1; c = (cSW + cSE) >> 1; o = (oSW + oSE) >> 1; break;
		case 3: vx = bx + size; vz = bz; vy = hSE; c = cSE; o = oSE; break;
		case 4: vx = bx + size; vz = bz + half; vy = (hSE + hNE) >> 1; c = (cSE + cNE) >> 1; o = (oSE + oNE) >> 1; break;
		case 5: vx = bx + size; vz = bz + size; vy = hNE; c = cNE; o = oNE; break;
		case 6: vx = bx + half; vz = bz + size; vy = (hNE + hNW) >> 1; c = (cNE + cNW) >> 1; o = (oNE + oNW) >> 1; break;
		case 7: vx = bx; vz = bz + size; vy = hNW; c = cNW; o = oNW; break;
		case 8: vx = bx; vz = bz + half; vy = (hNW + hSW) >> 1; c = (cNW + cSW) >> 1; o = (oNW + oSW) >> 1; break;
		case 9: vx = bx + half; vz = bz + quarter; vy = (hSW + hSE) >> 1; c = (cSW + cSE) >> 1; o = (oSW + oSE) >> 1; break;
		case 10: vx = bx + threeQuarter; vz = bz + half; vy = (hSE + hNE) >> 1; c = (cSE + cNE) >> 1; o = (oSE + oNE) >> 1; break;
		case 11: vx = bx + half; vz = bz + threeQuarter; vy = (hNE + hNW) >> 1; c = (cNE + cNW) >> 1; o = (oNE + oNW) >> 1; break;
		case 12: vx = bx + quarter; vz = bz + half; vy = (hNW + hSW) >> 1; c = (cNW + cSW) >> 1; o = (oNW + oSW) >> 1; break;
		case 13: vx = bx + quarter; vz = bz + quarter; vy = hSW; c = cSW; o = oSW; break;
		case 14: vx = bx + threeQuarter; vz = bz + quarter; vy = hSE; c = cSE; o = oSE; break;
		case 15: vx = bx + threeQuarter; vz = bz + threeQuarter; vy = hNE; c = cNE; o = oNE; break;
		default: vx = bx + quarter; vz = bz + threeQuarter; vy = hNW; c = cNW; o = oNW; break;
		}
		t->vx[v] = vx;
		t->vy[v] = vy;
		t->vz[v] = vz;
		uc[v] = c;
		oc[v] = o;
	}
	int count = TRIANGLE_COUNTS[shape];
	t->triangleCount = (uint8_t)count;
	for (int i = 0; i < count; i++) {
		const uint8_t* def = &TRIANGLE_INDICES[shape][i * 4];
		int a = def[1], b = def[2], c = def[3];
		if (a < 4) a = (a - rotation) & 3;
		if (b < 4) b = (b - rotation) & 3;
		if (c < 4) c = (c - rotation) & 3;
		t->ta[i] = (uint8_t)a;
		t->tb[i] = (uint8_t)b;
		t->tc[i] = (uint8_t)c;
		const int* colors = def[0] == 0 ? uc : oc;
		t->ca[i] = colors[a];
		t->cb[i] = colors[b];
		t->cc[i] = colors[c];
		t->tex[i] = def[0] == 0 ? -1 : overlayTexture;
	}
}

static PlainTile* add_plain(TerrainScene* s, int level, int x, int y) {
	if (s->plainCount == s->plainCapacity) {
		uint32_t cap = s->plainCapacity ? s->plainCapacity * 2 : 4096;
		PlainTile* p = realloc(s->plain, cap * sizeof(PlainTile));
		if (!p) return NULL;
		s->plain = p;
		s->plainCapacity = cap;
	}
	s->plainIndex[level][x][y] = (int32_t)s->plainCount;
	return &s->plain[s->plainCount++];
}

static ShapedTile* add_shaped(TerrainScene* s, int level, int x, int y) {
	if (s->shapedCount == s->shapedCapacity) {
		uint32_t cap = s->shapedCapacity ? s->shapedCapacity * 2 : 1024;
		ShapedTile* p = realloc(s->shaped, cap * sizeof(ShapedTile));
		if (!p) return NULL;
		s->shaped = p;
		s->shapedCapacity = cap;
	}
	s->shapedIndex[level][x][y] = (int32_t)s->shapedCount;
	return &s->shaped[s->shapedCount++];
}

static int render_level(const TerrainScene* s, int x, int y, int plane) {
	if ((s->flags[plane][x][y] & 8) != 0) return 0;
	return plane <= 0 || (s->flags[1][x][y] & 2) == 0 ? plane : plane - 1;
}

static int jitter_color(int base, int hue, int light) {
	int adjusted = hue + (base & 0x7F);
	if (adjusted < 0) adjusted = 0;
	else if (adjusted > 127) adjusted = 127;
	return (base & 0x380) + ((base + light) & 0xFC00) + adjusted;
}

int terrain_build(TerrainScene* s, const TerrainTypes* types, const uint32_t* palette,
	int hueJitter, int lightnessJitter, int allLevelsVisible, int centralPlane) {
	static int colors[SCENE_SIZE][SCENE_SIZE], light[SCENE_SIZE][SCENE_SIZE];
	static int rowHue[SCENE_SIZE], rowSat[SCENE_SIZE], rowLight[SCENE_SIZE], rowChroma[SCENE_SIZE], rowCount[SCENE_SIZE];
	int lightShift = lightnessJitter >> 2 << 10, hueShift = hueJitter >> 1;
	memset(colors, 0, sizeof(colors));
	memset(light, 0, sizeof(light));
	for (int level = 0; level < SCENE_LEVELS; level++) {
		int8_t (*sh)[105] = s->shadow[level];
		int32_t (*h)[105] = s->heights[level];
		int root = (int)sqrt(5100.0), scale = root * 768 >> 8;
		for (int y = 1; y < 103; y++) {
			for (int x = 1; x < 103; x++) {
				int dy = h[x][y + 1] - h[x][y - 1], dx = h[x + 1][y] - h[x - 1][y];
				int len = (int)sqrt((double)(dx * dx + dy * dy + 65536));
				int ny = (dy << 8) / len, nz = -65536 / len, nx = (dx << 8) / len;
				int shade = (sh[x][y] >> 1) + (sh[x][y - 1] >> 2) + (sh[x + 1][y] >> 3) + (sh[x - 1][y] >> 2) + (sh[x][y + 1] >> 3);
				light[x][y] = (ny * -50 + nx * -50 + nz * -10) / scale + 74 - shade;
			}
		}
		memset(rowHue, 0, sizeof(rowHue));
		memset(rowSat, 0, sizeof(rowSat));
		memset(rowLight, 0, sizeof(rowLight));
		memset(rowChroma, 0, sizeof(rowChroma));
		memset(rowCount, 0, sizeof(rowCount));
		for (int i = -5; i < SCENE_SIZE; i++) {
			for (int j = 0; j < SCENE_SIZE; j++) {
				int add = i + 5;
				if (add < SCENE_SIZE) {
					int id = s->underlay[level][add][j];
					if (id > 0 && (uint32_t)(id - 1) < types->fluCount) {
						const FluDef* t = &types->flu[id - 1];
						rowHue[j] += t->weightedHue;
						rowSat[j] += t->saturation;
						rowLight[j] += t->lightness;
						rowChroma[j] += t->chroma;
						rowCount[j]++;
					}
				}
				int sub = i - 5;
				if (sub >= 0) {
					int id = s->underlay[level][sub][j];
					if (id > 0 && (uint32_t)(id - 1) < types->fluCount) {
						const FluDef* t = &types->flu[id - 1];
						rowHue[j] -= t->weightedHue;
						rowSat[j] -= t->saturation;
						rowLight[j] -= t->lightness;
						rowChroma[j] -= t->chroma;
						rowCount[j]--;
					}
				}
			}
			if (i < 0) continue;
			int hue = 0, lum = 0, sat = 0, chroma = 0, count = 0;
			for (int p = -5; p < SCENE_SIZE; p++) {
				int add = p + 5;
				if (add < SCENE_SIZE) {
					sat += rowSat[add];
					count += rowCount[add];
					hue += rowHue[add];
					chroma += rowChroma[add];
					lum += rowLight[add];
				}
				int sub = p - 5;
				if (sub >= 0) {
					sat -= rowSat[sub];
					chroma -= rowChroma[sub];
					hue -= rowHue[sub];
					count -= rowCount[sub];
					lum -= rowLight[sub];
				}
				if (p >= 0 && count > 0 && chroma != 0) colors[i][p] = pack_hsl(lum / count, sat / count, hue * 256 / chroma);
			}
		}
		for (int i = 1; i < 103; i++) {
			for (int j = 1; j < 103; j++) {
				if (!(allLevelsVisible || (s->flags[0][i][j] & 2) != 0 ||
					((s->flags[level][i][j] & 0x10) == 0 && render_level(s, i, j, level) == centralPlane))) continue;
				int under = s->underlay[level][i][j], over = s->overlay[level][i][j];
				if (under <= 0 && over <= 0) continue;
				int hSW = h[i][j], hSE = h[i + 1][j], hNE = h[i + 1][j + 1], hNW = h[i][j + 1];
				int q = -1, underRgb = 0;
				if (under > 0) {
					q = colors[i][j];
					int adjusted = (q & 0x7F) + hueShift;
					if (adjusted < 0) adjusted = 0;
					else if (adjusted > 127) adjusted = 127;
					int t = (q & 0x380) + ((q + lightShift) & 0xFC00) + adjusted;
					underRgb = (int)palette[multiply_safe(96, t) & 0xFFFF];
				}
				int lSW = light[i][j], lNW = light[i][j + 1], lSE = light[i + 1][j], lNE = light[i + 1][j + 1];
				int uc[4] = {multiply_safe(lSW, q), multiply_safe(lSE, q), multiply_safe(lNE, q), multiply_safe(lNW, q)};
				if (over == 0) {
					PlainTile* t = add_plain(s, level, i, j);
					if (!t) return -1;
					t->colorSW = uc[0]; t->colorSE = uc[1]; t->colorNE = uc[2]; t->colorNW = uc[3];
					t->texture = -1;
					t->flatColor = underRgb;
					t->flat = 0;
					continue;
				}
				int shape = s->shape[level][i][j] + 1, angle = s->angle[level][i][j];
				const FloDef* flo = &types->flo[over - 1];
				int texture = flo->texture, base, overRgb;
				if (texture >= 0 && (!((uint32_t)texture < types->textureCount) || !types->textureRepeating[texture])) texture = -1;
				if (texture >= 0) {
					base = -1;
					overRgb = (int)palette[multiply_gray(types->textureAverage[texture], 96) & 0xFFFF];
				} else if (flo->baseColor == -1) {
					base = -2;
					overRgb = 0;
				} else {
					base = flo->baseColor;
					overRgb = (int)palette[multiply_gray(jitter_color(base, hueShift, lightShift), 96) & 0xFFFF];
				}
				if (flo->secondaryColor >= 0) overRgb = (int)palette[multiply_gray(jitter_color(flo->secondaryColor, hueShift, lightShift), 96) & 0xFFFF];
				int oc[4] = {multiply_gray(base, lSW), multiply_gray(base, lSE), multiply_gray(base, lNE), multiply_gray(base, lNW)};
				if (shape == 1) {
					PlainTile* t = add_plain(s, level, i, j);
					if (!t) return -1;
					t->colorSW = oc[0]; t->colorSE = oc[1]; t->colorNE = oc[2]; t->colorNW = oc[3];
					t->texture = texture;
					t->flatColor = overRgb;
					t->flat = hSW == hSE && hSW == hNE && hSW == hNW;
				} else {
					ShapedTile* t = add_shaped(s, level, i, j);
					if (!t) return -1;
					build_shaped(t, shape, angle, texture, i, j, hSW, hSE, hNE, hNW, uc, oc, underRgb, overRgb);
				}
			}
		}
	}
	return 0;
}
