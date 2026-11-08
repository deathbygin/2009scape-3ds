// Raw model and loc (object) definition decoding, ported from the revision 530 client.
#ifndef MODEL_H
#define MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "js5.h"

typedef struct {
	int vertexCount, triangleCount, texturedCount;
	int priority;
	int32_t *vx, *vy, *vz;
	int32_t *ta, *tb, *tc;
	int16_t* colors;
	int8_t* info;       // NULL if absent
	int8_t* alpha;      // NULL if absent
	int8_t* priorities; // NULL if absent
	int16_t* textures;  // NULL if absent
	int8_t* textureIndex; // NULL if absent
	int16_t *faceP, *faceM, *faceN; // NULL if no textured faces
	uint8_t* textureTypes;          // NULL if no textured faces
	int32_t* vertexBones;   // NULL if absent
	int32_t* triangleBones; // NULL if absent
} RawModel;

// Returns 0 on success. Free with model_free.
int model_decode(RawModel* m, const uint8_t* src, size_t size);
void model_free(RawModel* m);

typedef struct {
	int id;
	int width, length, blockwalk, interactable, walloff, anim;
	int ambient, contrast, hillskewType, hillskewAmount;
	int resizeX, resizeY, resizeZ, xoff, yoff, zoff;
	int multiLocVarbit, multiLocVarp, supportitems, mapscene;
	uint8_t blockrange, active, occlude, mirror, computeVertexColors, dynamic, castshadow, forcedecor, render, clipped, breakroutefinding;
	char* name;
	char* ops[5]; // NULL when the option is absent or hidden
	int modelCount, recolCount, recolPaletteCount, retexCount, multiLocCount;
	int32_t* models;
	int32_t* shapes; // NULL when the loc has one model set for shape 10
	int16_t *recolS, *recolD, *retexS, *retexD;
	int8_t* recolPalette;
	int32_t* multiLocs;
} LocDef;

// Decodes a definition exactly as LocTypeList.get does. Free with loc_free.
void loc_decode(LocDef* d, int id, const uint8_t* src, size_t size);
void loc_free(LocDef* d);

typedef struct {
	int id, size, combatLevel, rotationSpeed, basId;
	int ambient, contrast, resizeX, resizeY;
	int multiVarbit, multiVarp, multiCount, modelCount, recolCount, recolPaletteCount, retexCount;
	int32_t* models;  // -1 marks an empty slot
	int32_t* offsets; // three per model, NULL if none
	uint8_t* hasOffset;
	int32_t* multiNpcs;
	int32_t* headModels; // for dialogue, NULL if none
	int headModelCount;
	int16_t *recolS, *recolD, *retexS, *retexD;
	int8_t* recolPalette;
	char* name;
	char* ops[5];
	uint8_t interactive;
} NpcDef;

void npc_decode(NpcDef* d, int id, const uint8_t* src, size_t size);

// A body part a player can choose.
typedef struct {
	int modelCount, recolCount, retexCount;
	int32_t* models; // NULL if the part has no body model
	int32_t heads[5]; // for dialogue, -1 where unused
	int16_t *recolS, *recolD, *retexS, *retexD;
} IdkDef;

void idk_decode(IdkDef* d, const uint8_t* src, size_t size);

// An item. Only what is needed to wear and name it.
typedef struct {
	int id;
	int manwear, manwear2, manwear3, womanwear, womanwear2, womanwear3;
	int8_t manOffset[3], womanOffset[3];
	int recolCount, retexCount;
	int16_t *recolS, *recolD, *retexS, *retexD;
	int certTemplate, certLink, lentLink, lentTemplate;
	char* name;
	char* iops[5]; // bag options, NULL when absent
	char* ops[5];  // ground options, NULL when absent
	int cost;
	int model, zoom2d, xan2d, yan2d, zan2d, xof2d, yof2d;
	int resizeX, resizeY, resizeZ, ambient, contrast;
	uint8_t stackable, hasCounts, members;
	int manhead, womanhead, manhead2, womanhead2; // worn on a dialogue head, -1 if not
	int countObj[10], countCo[10]; // a different look for bigger stacks
} ObjDef;

void obj_decode(ObjDef* d, int id, const uint8_t* src, size_t size);
void obj_free(ObjDef* d);


typedef struct {
	int id, shape, orientation, plane, x, y; // x and y are local scene tiles
} LocPlacement;

// Reads a location map placed at local position baseX, baseY. Calls emit for each loc inside the scene.
typedef void (*LocEmit)(void* context, const LocPlacement* placement);
void loc_read_placements(const uint8_t* src, size_t size, int baseX, int baseY, LocEmit emit, void* context);

#endif
