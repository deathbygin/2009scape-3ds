// Builds a complete scene from the cache: terrain plus lit, placed objects.
// Ported from the revision 530 client's software path.
#ifndef SCENE_H
#define SCENE_H

#include "model.h"
#include "terrain.h"

typedef struct { int x, y, z, magnitude; } VertexNormal;
typedef struct { int x, y, z; } TriangleNormal;

// One model after loc transforms. Shared by every placement of the same loc, rotation and shape.
typedef struct SceneBase {
	int key0, key1;
	struct SceneBase* next;
	int vertexCount, triangleCount, texturedCount;
	int32_t *vx, *vy, *vz, *ta, *tb, *tc;
	int16_t* colors;
	int16_t* textures;
	int8_t *info, *alpha, *textureIndex;
	uint8_t* textureTypes;
	VertexNormal* normals;
	TriangleNormal* triangleNormals;
	int ambient, contrast;
	uint8_t shareLight;
	int32_t *fa, *fb, *fc; // lit colours, when not shareLight
	int16_t* litTextures;
	int32_t* vertexBones; // NPC models only, NULL if the model has none
} SceneBase;

enum { ENTITY_LIT = 1, ENTITY_RAW = 2, ENTITY_DYNAMIC = 3 };

typedef struct {
	SceneBase* base;
	int32_t *vx, *vy, *vz;
	int8_t* info;
	VertexNormal* merged;
	uint8_t* mergedSet;
	int32_t *fa, *fb, *fc;
	int16_t* textures;
	uint8_t kind, ownsXZ, ownsY, ownsInfo, ownsLit;
	int locId, shape, rotation, level, tileX, tileY;
} SceneEntity;

typedef struct {
	SceneEntity* entity;
	int xFine, yFine, zFine, xMin, yMin, xMax, yMax, locId;
	uint8_t shape, orientation, interactable, hasMapscene;
} SceneScenery;

typedef struct {
	SceneEntity *primary, *secondary;
	int xFine, yFine, zFine, locId;
	uint8_t shape, orientation, interactable, hasMapscene;
} SceneWall;

typedef struct {
	SceneEntity *primary, *secondary;
	int xFine, yFine, height, xOffset, yOffset, locId;
} SceneDecor;

typedef struct {
	SceneEntity* entity;
	int xFine, yFine, zFine, locId;
} SceneGround;

typedef struct {
	SceneWall* wall;
	SceneDecor* decor;
	SceneGround* ground;
	SceneScenery* scenery[5];
	uint8_t sceneryCount;
} SceneTile;

#define SCENE_BASE_BUCKETS 4096
#define SCENE_MODEL_BUCKETS 2048

typedef struct SceneModel {
	int id;
	RawModel model;
	struct SceneModel* next;
} SceneModel;

typedef struct {
	TerrainScene terrain;
	SceneTile tiles[SCENE_LEVELS][SCENE_SIZE][SCENE_SIZE];
	Js5Cache* cache;
	const TerrainTypes* types;
	SceneBase* bases[SCENE_BASE_BUCKETS];
	SceneModel* models[SCENE_MODEL_BUCKETS];
	long entityCount, baseCount;
	uint8_t placeShape, placeOrientation, placeInteractable, placeMapscene;
	int placeLocId;
} Scene;

void scene_init(Scene* s, Js5Cache* cache, const TerrainTypes* types, int originX, int originY);
void scene_free(Scene* s);

// Adds one placement, as SceneGraph.addLoc does. Terrain must already be read.
void scene_add_loc(Scene* s, const LocPlacement* p);

// Blends lighting between neighbouring objects and finishes them. Run after terrain_build.
void scene_merge(Scene* s);

// Gives animated and state-changing objects their default, unanimated model.
// Their real appearance depends on the server and on animation time.
void scene_resolve_dynamic(Scene* s);

// NPC definitions are cached for the life of the program. Returns NULL for an unknown id.
const NpcDef* scene_get_npc(Scene* s, int id);

// The definition actually shown for an NPC, or NULL if it is invisible. Variable states are not tracked yet, so state 0 is used.
const NpcDef* scene_shown_npc(Scene* s, const NpcDef* d);

// Builds a lit NPC model in its default pose, before resizing. Free with scene_free_base. Returns NULL if it has no model.
SceneBase* scene_build_npc(Scene* s, const NpcDef* d);

// Builds the head shown beside dialogue. Free with scene_free_base. NULL if there is none.
SceneBase* scene_build_npc_head(Scene* s, const NpcDef* d);

// Builds a model by its own number, for a picture on an interface. Free with scene_free_base.
SceneBase* scene_build_raw(Scene* s, int model);

// A player's look. identikit holds 0, or 0x80000000 | body part, or 0x40000000 | item.
typedef struct {
	int32_t identikit[12];
	uint8_t colours[5], female;
	int basId;
} PlayerLook;

// Builds a lit player model in its default pose. Free with scene_free_base. Returns NULL if it has no parts.
SceneBase* scene_build_player(Scene* s, const PlayerLook* look);
SceneBase* scene_build_player_head(Scene* s, const PlayerLook* look);

// Item definitions are cached for the life of the program. Returns NULL for an unknown id.
const ObjDef* scene_get_obj(Scene* s, int id);

// Builds a lit item model for an icon, as ObjType.getInvModel does. Free with scene_free_base.
SceneBase* scene_build_item(Scene* s, const ObjDef* d);

// Lists every wearable item in id order. The server names worn items by position in this list.
// Slow, since it reads every item. Returns the count, or -1. Free *out with free().
int scene_wear_table(Scene* s, int32_t** out);

// Applies an NPC's size scaling, which the client does last, after any animation.
void scene_resize_npc(int32_t* vx, int32_t* vy, int32_t* vz, int count, const NpcDef* d);
void scene_free_base(SceneBase* b);

#endif
