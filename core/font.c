#include "font.h"

#include <stdlib.h>
#include <string.h>

#define ARCHIVE_SPRITES 8
#define ARCHIVE_METRICS 13
#define GLYPHS 256

static int be16(const uint8_t* p) {
	return p[0] << 8 | p[1];
}

static int read_file(Js5Cache* cache, int archive, int group, Js5Buffer* file) {
	if (js5_load_index(cache, archive) != 0 || group < 0) return -1;
	Js5Buffer data = {0, 0};
	if (js5_read_group(cache, archive, (uint32_t)group, NULL, &data) != 0) return -3;
	int rc = js5_group_file(&cache->index[archive], (uint32_t)group, &data, 0, file);
	free(data.data);
	return rc;
}

// Every frame of the sprite is one letter, one after the other, each with its own flags.
static int read_glyphs(GameFont* f, const uint8_t* d, long size) {
	int frames = be16(d + size - 2);
	long at = size - (long)frames * 8 - 7;
	if (frames < GLYPHS || at < 0) return -1;
	const uint8_t* table = d + at + 5;
	size_t capacity = 8192, count = 0;
	f->runs = malloc(sizeof(FontRun) * capacity);
	if (!f->runs) return -2;
	const uint8_t* src = d;
	for (int glyph = 0; glyph < GLYPHS; glyph++) {
		int xOffset = be16(table + glyph * 2), yOffset = be16(table + frames * 2 + glyph * 2);
		int w = be16(table + frames * 4 + glyph * 2), h = be16(table + frames * 6 + glyph * 2);
		int flags = *src++, area = w * h;
		f->first[glyph] = (uint32_t)count;
		for (int y = 0; y < h; y++) {
			int x = 0;
			while (x < w) {
				int index = flags & 1 ? x * h + y : y * w + x;
				if (src[index] == 0) {
					x++;
					continue;
				}
				int start = x;
				while (x < w && src[flags & 1 ? x * h + y : y * w + x] != 0) x++;
				if (count == capacity) {
					capacity *= 2;
					FontRun* grown = realloc(f->runs, sizeof(FontRun) * capacity);
					if (!grown) return -3;
					f->runs = grown;
				}
				f->runs[count++] = (FontRun){(int8_t)(xOffset + start), (int8_t)(yOffset + y), (uint8_t)(x - start)};
			}
		}
		src += area;
		if (flags & 2) src += area;
	}
	f->first[GLYPHS] = (uint32_t)count;
	return 0;
}

int font_load_id(Js5Cache* cache, int id, GameFont* out) {
	memset(out, 0, sizeof(*out));
	Js5Buffer sprite = {0, 0}, metrics = {0, 0};
	if (read_file(cache, ARCHIVE_METRICS, id, &metrics) != 0 || metrics.size < GLYPHS) return -1;
	memcpy(out->advance, metrics.data, GLYPHS);
	// A short table ends with the line height. A longer one has spacing pairs, which small text can do without.
	out->lineHeight = metrics.size == GLYPHS + 1 ? metrics.data[GLYPHS] : metrics.data[metrics.size - 1];
	free(metrics.data);
	if (read_file(cache, ARCHIVE_SPRITES, id, &sprite) != 0) return -2;
	int rc = read_glyphs(out, sprite.data, (long)sprite.size);
	free(sprite.data);
	return rc;
}

int font_load(Js5Cache* cache, const char* name, GameFont* out) {
	if (js5_load_index(cache, ARCHIVE_METRICS) != 0) return -1;
	return font_load_id(cache, js5_find_group(&cache->index[ARCHIVE_METRICS], name), out);
}

int font_lines(const GameFont* f, const char* text, int width) {
	int lines = 0, lineWidth = 0, length = 0, lineStart = 0, breakPos = -1, breakWidth = 0;
	while (*text) {
		if (*text == '<') {
			const char* end = strchr(text, '>');
			if (end) {
				if (strncmp(text, "<br>", 4) == 0) {
					lines++;
					lineStart = length;
					lineWidth = 0;
					breakPos = -1;
				}
				length += (int)(end - text) + 1; // tags stay in the client's buffer
				text = end + 1;
				continue;
			}
		}
		int letter = (uint8_t)*text++;
		length++;
		lineWidth += f->advance[letter];
		if (letter == ' ') {
			breakPos = length;
			breakWidth = lineWidth;
		}
		if (lineWidth > width && breakPos >= 0) {
			lines++;
			lineStart = breakPos;
			breakPos = -1;
			lineWidth -= breakWidth;
		}
		if (letter == '-') {
			breakPos = length;
			breakWidth = lineWidth;
		}
	}
	return length > lineStart ? lines + 1 : lines;
}

int font_width(const GameFont* f, const char* text) {
	int width = 0;
	for (; *text; text++) width += f->advance[(uint8_t)*text];
	return width;
}
