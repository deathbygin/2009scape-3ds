#include "world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void load_keys(World* w, const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* text = malloc((size_t)n + 1);
	if (!text) {
		fclose(f);
		return;
	}
	if (fread(text, 1, (size_t)n, f) != (size_t)n) n = 0;
	text[n] = 0;
	fclose(f);
	for (char* p = strstr(text, "\"regionId\""); p; p = strstr(p + 1, "\"regionId\"")) {
		char* id = strchr(p + 10, '"');
		char* k = strstr(p, "\"keys\"");
		if (!id || !k || !(k = strchr(k + 6, '"'))) break;
		int region = atoi(id + 1);
		if (region >= 0 && region < 65536 && sscanf(k + 1, "%d,%d,%d,%d", &w->keys[region][0], &w->keys[region][1], &w->keys[region][2], &w->keys[region][3]) == 4)
			w->hasKey[region] = 1;
	}
	free(text);
}

int world_open(World* w, const char* cacheDir, const char* keysPath, double brightness) {
	memset(w, 0, sizeof(*w));
	int rc = js5_open(&w->cache, cacheDir);
	if (rc != 0) return rc;
	if ((rc = js5_load_index(&w->cache, 5)) != 0) return rc;
	if ((rc = terrain_load_types(&w->cache, &w->types)) != 0) return rc;
	w->keys = calloc(65536, sizeof(*w->keys));
	w->hasKey = calloc(65536, 1);
	if (!w->keys || !w->hasKey) return -30;
	load_keys(w, keysPath);
	terrain_build_palette(w->palette, brightness);
	return 0;
}

void world_close(World* w) {
	terrain_free_types(&w->types);
	free(w->keys);
	free(w->hasKey);
	js5_close(&w->cache);
}

static void place(void* context, const LocPlacement* p) {
	scene_add_loc(context, p);
}

static long now(WorldClock clock) {
	return clock ? clock() : 0;
}

int world_build(World* w, Scene* scene, int tileX, int tileY, WorldClock clock, WorldTiming* timing) {
	return world_build_zone(w, scene, tileX >> 3, tileY >> 3, NULL, 0, clock, timing);
}

// The client leaves a few regions around the tutorial and fishing areas empty.
static int region_blacklisted(int zoneX, int zoneY, int rx, int ry) {
	int listed = (zoneX / 8 == 48 || zoneX / 8 == 49) && zoneY / 8 == 48;
	if (zoneX / 8 == 48 && zoneY / 8 == 148) listed = 1;
	return listed && (ry == 49 || ry == 149 || ry == 147 || rx == 50 || (rx == 49 && ry == 47));
}

int world_build_zone(World* w, Scene* scene, int zoneX, int zoneY, const int32_t (*keys)[4], int keyCount, WorldClock clock, WorldTiming* timing) {
	int originX = (zoneX - 6) * 8, originY = (zoneY - 6) * 8;
	int minRX = (zoneX - 6) / 8, maxRX = (zoneX + 6) / 8, minRY = (zoneY - 6) / 8, maxRY = (zoneY + 6) / 8;
	long t0 = now(clock);
	scene_init(scene, &w->cache, &w->types, originX, originY);
	for (int rx = minRX; rx <= maxRX; rx++) {
		for (int ry = minRY; ry <= maxRY; ry++) {
			char name[32];
			snprintf(name, sizeof(name), "m%d_%d", rx, ry);
			int group = region_blacklisted(zoneX, zoneY, rx, ry) ? -1 : js5_find_group(&w->cache.index[5], name);
			Js5Buffer map;
			if (group >= 0 && js5_read_group(&w->cache, 5, (uint32_t)group, NULL, &map) == 0) {
				terrain_read_region(&scene->terrain, map.data, map.size, rx * 64 - originX, ry * 64 - originY);
				free(map.data);
			} else if (zoneY < 800) {
				for (int level = 0; level < SCENE_LEVELS; level++) terrain_clear_region(&scene->terrain, level, rx * 64 - originX, ry * 64 - originY);
			}
		}
	}
	long t1 = now(clock);
	int index = 0;
	for (int rx = minRX; rx <= maxRX; rx++) {
		for (int ry = minRY; ry <= maxRY; ry++, index++) {
			int region = rx << 8 | ry;
			char name[32];
			snprintf(name, sizeof(name), "l%d_%d", rx, ry);
			int group = region_blacklisted(zoneX, zoneY, rx, ry) ? -1 : js5_find_group(&w->cache.index[5], name);
			const int32_t* key = keys && index < keyCount ? keys[index] : w->hasKey[region] ? w->keys[region] : NULL;
			Js5Buffer file;
			if (group < 0 || js5_read_group(&w->cache, 5, (uint32_t)group, key, &file) != 0) continue;
			loc_read_placements(file.data, file.size, rx * 64 - originX, ry * 64 - originY, place, scene);
			free(file.data);
		}
	}
	long t2 = now(clock);
	int rc = terrain_build(&scene->terrain, &w->types, w->palette, 0, 0, 1, 0);
	long t3 = now(clock);
	scene_merge(scene);
	scene_resolve_dynamic(scene);
	long t4 = now(clock);
	if (timing) {
		timing->readMs = t1 - t0;
		timing->locsMs = t2 - t1;
		timing->tilesMs = t3 - t2;
		timing->mergeMs = t4 - t3;
		timing->objects = scene->entityCount;
	}
	return rc;
}
