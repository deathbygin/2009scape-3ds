// The game's own fonts from the cache, kept as runs of pixels so they can be drawn without textures.
#ifndef FONT_H
#define FONT_H

#include "js5.h"

typedef struct {
	int8_t x, y;  // from the pen, with y from the top of the line
	uint8_t length;
} FontRun;

typedef struct {
	uint8_t advance[256];
	uint32_t first[257]; // where each glyph's runs start, so first[c + 1] is where they end
	FontRun* runs;
	int lineHeight;
} GameFont;

// Loads a font by name, such as p11_full. Returns 0 on success.
int font_load(Js5Cache* cache, const char* name, GameFont* out);

// The same by group id, which is how interfaces name their fonts.
int font_load_id(Js5Cache* cache, int id, GameFont* out);

// How many lines text takes at a width, following the client's rules for tags, spaces and hyphens.
int font_lines(const GameFont* f, const char* text, int width);

// How wide a line of text is.
int font_width(const GameFont* f, const char* text);

#endif
