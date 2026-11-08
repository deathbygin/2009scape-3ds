// Turns a built scene into GPU-ready indexed geometry, one 8x8-tile chunk at a time.
#ifndef MESH_H
#define MESH_H

#include "scene.h"

#define MESH_MAX_CHUNKS 320
#define MESH_VERTEX_SIZE 12
#define MESH_SECTOR_GRID 13

typedef struct {
	uint32_t start[2], count[2]; // index ranges: 0 = with ground decor, 1 = without
	int16_t minX, maxX, minY, maxY, minZ, maxZ; // true bounds in game units
	uint8_t used;
} MeshCell;

typedef struct {
	uint32_t sector, vertexCount, indexCount[2];
	uint8_t* vertices; // x, y, z, pad as int16, then r, g, b, a
	uint16_t* indices[2];
	MeshCell cells[16]; // 2x2-tile cells in Morton order
} MeshChunk;

// Where one placed object lives in the index data, so it can be hidden later.
typedef struct {
	uint32_t start[2], count[2];
	uint16_t chunk;
	uint8_t level, x, y, layer;
} MeshObject;

#define MESH_MAX_OBJECTS 60000

typedef struct {
	MeshChunk chunks[MESH_MAX_CHUNKS];
	uint32_t chunkCount;
	uint8_t* pool;
	size_t poolSize, poolUsed;
	long triangles, vertices;
	MeshObject* objects;
	uint32_t objectCount;
} Mesh;

// Fills mesh using pool, which must be GPU-accessible on the 3DS. Returns 0 on success.
int mesh_build(Mesh* mesh, const Scene* scene, const uint32_t* palette, uint8_t* pool, size_t poolSize);

// Writes the objects on one tile as a plain triangle list. Returns the number of vertices.
size_t mesh_emit_tile(const Scene* scene, const uint32_t* palette, int level, int x, int y, uint8_t* out, size_t maxVertices);

// Splits a lit model into corners, each a vertex with one colour, so that animation only changes positions.
// Each output needs room for three entries per triangle. Returns the number of corners, or -1 on failure.
int mesh_index_model(const Scene* scene, const uint32_t* palette, const SceneBase* model, uint16_t* cornerVertex, uint8_t* colours, uint16_t* indices, uint32_t* indexCount);

#endif
