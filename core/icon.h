// Draws item icons in software, as Inv.renderObjectSprite does: the item's 3D model into a small picture.
#ifndef ICON_H
#define ICON_H

#include "scene.h"

#define ICON_W 36
#define ICON_H 32
#define ICON_SHADOW 0x302020

// Renders an item as the bag shows it. Pixels are 0xRRGGBB, with 0 for transparent. Returns 0 on success.
// This loads models, so call scene_free afterwards if the scene is not being built.
int icon_render(Scene* s, const uint32_t* palette, int id, int stack, uint32_t* pixels);

// A head for dialogue, split up once so that each frame of an expression only needs new positions.
typedef struct {
	uint16_t* cornerVertex;
	uint16_t* indices;
	uint8_t* colours;
	uint32_t indexCount;
	int centreX, centreY, scale; // fitted to the canvas from the resting pose, scale in 256ths
} HeadMesh;

int head_prepare(Scene* s, const uint32_t* palette, const SceneBase* model, HeadMesh* out);
void head_free(HeadMesh* mesh);

// Turns what head_draw draws, for models that face the viewer. A negative value goes back to the turn heads use.
void head_set_yaw(int yaw);

// Draws into a square canvas of pixels, which the caller clears. Canvases can be up to 96 pixels across.
void head_draw(const HeadMesh* mesh, const int32_t* vx, const int32_t* vy, const int32_t* vz, int vertexCount, uint32_t* pixels, int size);

#endif
