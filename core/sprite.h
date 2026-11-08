// Interface artwork from the cache, ported from the client's SpriteLoader.
#ifndef SPRITE_H
#define SPRITE_H

#include "js5.h"

typedef struct {
	int width, height;   // the sprite's full size
	int xOffset, yOffset; // where the stored pixels sit within it
	int innerWidth, innerHeight;
	uint32_t* pixels;    // 0xAARRGGBB, innerWidth * innerHeight
} Sprite;

// Loads the first frame of a sprite. Returns 0 on success.
int sprite_load(Js5Cache* cache, int id, Sprite* out);

// Loads one frame of a sprite that has several. Fails if there is no such frame.
int sprite_load_frame(Js5Cache* cache, int id, int frame, Sprite* out);
void sprite_free(Sprite* s);

#endif
