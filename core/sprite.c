#include "sprite.h"

#include <stdlib.h>
#include <string.h>

#define ARCHIVE_SPRITES 8

static int be16(const uint8_t* p) {
	return p[0] << 8 | p[1];
}

int sprite_load(Js5Cache* cache, int id, Sprite* out) {
	return sprite_load_frame(cache, id, 0, out);
}

int sprite_load_frame(Js5Cache* cache, int id, int frame, Sprite* out) {
	memset(out, 0, sizeof(*out));
	if (js5_load_index(cache, ARCHIVE_SPRITES) != 0) return -1;
	Js5Buffer group = {0, 0}, file = {0, 0};
	if (js5_read_group(cache, ARCHIVE_SPRITES, (uint32_t)id, NULL, &group) != 0) return -2;
	int rc = js5_group_file(&cache->index[ARCHIVE_SPRITES], (uint32_t)id, &group, 0, &file);
	free(group.data);
	if (rc != 0) return -3;
	const uint8_t* d = file.data;
	long size = (long)file.size;
	int frames = be16(d + size - 2);
	long at = size - (long)frames * 8 - 7;
	if (frames < 1 || frame >= frames || at < 0) {
		free(file.data);
		return -4;
	}
	out->width = be16(d + at);
	out->height = be16(d + at + 2);
	int paletteSize = d[at + 4] + 1;
	const uint8_t* table = d + at + 5;
	out->xOffset = be16(table + frame * 2);
	out->yOffset = be16(table + frames * 2 + frame * 2);
	out->innerWidth = be16(table + frames * 4 + frame * 2);
	out->innerHeight = be16(table + frames * 6 + frame * 2);
	const uint8_t* colours = d + size + 3 - (long)frames * 8 - (long)paletteSize * 3 - 7;
	uint32_t palette[256];
	palette[0] = 0;
	for (int i = 1; i < paletteSize; i++) {
		uint32_t c = (uint32_t)colours[(i - 1) * 3] << 16 | (uint32_t)colours[(i - 1) * 3 + 1] << 8 | colours[(i - 1) * 3 + 2];
		palette[i] = c ? c : 1;
	}
	int w = out->innerWidth, h = out->innerHeight, count = w * h;
	out->pixels = malloc(sizeof(uint32_t) * (size_t)(count + 1));
	if (!out->pixels) {
		free(file.data);
		return -5;
	}
	// Frames follow one another, each with its own flags.
	const uint8_t* src = d;
	for (int i = 0; i < frame; i++) {
		int area = be16(table + frames * 4 + i * 2) * be16(table + frames * 6 + i * 2);
		int skipFlags = *src++;
		src += area;
		if (skipFlags & 2) src += area;
	}
	int flags = *src++;
	for (int i = 0; i < count; i++) {
		int at2 = flags & 1 ? (i % h) * w + i / h : i;
		int index = src[i];
		out->pixels[at2] = index ? 0xFF000000u | palette[index] : 0;
	}
	if (flags & 2) {
		const uint8_t* alpha = src + count;
		for (int i = 0; i < count; i++) {
			int at2 = flags & 1 ? (i % h) * w + i / h : i;
			if (out->pixels[at2]) out->pixels[at2] = (out->pixels[at2] & 0xFFFFFF) | (uint32_t)alpha[i] << 24;
		}
	}
	free(file.data);
	return 0;
}

void sprite_free(Sprite* s) {
	free(s->pixels);
	s->pixels = NULL;
}
