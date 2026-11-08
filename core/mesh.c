#include "mesh.h"

#include <stdlib.h>
#include <string.h>

#define MAX_VERTICES 60000
#define MAX_INDICES 900000
#define HASH_SIZE 131072

typedef struct {
	Mesh* mesh;
	const Scene* scene;
	const uint32_t* palette;
	uint8_t* vertices;
	uint16_t* indices[2];
	uint32_t* hash; // generation << 16 | vertex index
	uint32_t generation;
	uint32_t vertexCount, indexCount[2];
	MeshChunk* chunk;
	int sector, cell, stream0Only, failed;
	uint8_t* direct; // when set, triangles go straight here
	size_t directCount, directMax;
	uint32_t objectChunk, objectStart[2];
} Builder;

static int16_t clamp16(int v) {
	return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static int finish_chunk(Builder* b) {
	Mesh* m = b->mesh;
	MeshChunk* c = b->chunk;
	if (!c) return 0;
	b->chunk = NULL;
	if (b->vertexCount == 0) {
		m->chunkCount--;
		return 0;
	}
	size_t vertexBytes = (size_t)b->vertexCount * MESH_VERTEX_SIZE;
	size_t a = (vertexBytes + 15) & ~(size_t)15;
	size_t i0 = ((size_t)b->indexCount[0] * 2 + 15) & ~(size_t)15;
	size_t i1 = ((size_t)b->indexCount[1] * 2 + 15) & ~(size_t)15;
	if (m->poolUsed + a + i0 + i1 > m->poolSize) return -1;
	c->vertices = m->pool + m->poolUsed;
	memcpy(c->vertices, b->vertices, vertexBytes);
	m->poolUsed += a;
	c->indices[0] = (uint16_t*)(m->pool + m->poolUsed);
	memcpy(c->indices[0], b->indices[0], (size_t)b->indexCount[0] * 2);
	m->poolUsed += i0;
	c->indices[1] = (uint16_t*)(m->pool + m->poolUsed);
	memcpy(c->indices[1], b->indices[1], (size_t)b->indexCount[1] * 2);
	m->poolUsed += i1;
	c->vertexCount = b->vertexCount;
	c->indexCount[0] = b->indexCount[0];
	c->indexCount[1] = b->indexCount[1];
	m->vertices += b->vertexCount;
	return 0;
}

static int start_chunk(Builder* b) {
	Mesh* m = b->mesh;
	if (m->chunkCount >= MESH_MAX_CHUNKS) return -1;
	MeshChunk* c = &m->chunks[m->chunkCount++];
	memset(c, 0, sizeof(*c));
	c->sector = (uint32_t)b->sector;
	b->chunk = c;
	b->vertexCount = 0;
	b->indexCount[0] = b->indexCount[1] = 0;
	b->generation++;
	if (b->generation == 0xFFFF) {
		memset(b->hash, 0, sizeof(uint32_t) * HASH_SIZE);
		b->generation = 1;
	}
	return 0;
}

static void open_cell(Builder* b) {
	MeshCell* c = &b->chunk->cells[b->cell];
	if (c->used) return;
	c->used = 1;
	c->start[0] = b->indexCount[0];
	c->start[1] = b->indexCount[1];
	c->minX = c->minY = c->minZ = 32767;
	c->maxX = c->maxY = c->maxZ = -32768;
}

static int add_vertex(Builder* b, int16_t x, int16_t y, int16_t z, uint32_t rgb, int alpha) {
	uint8_t v[MESH_VERTEX_SIZE];
	int16_t p[4] = {x, y, z, 0};
	memcpy(v, p, 8);
	v[8] = (uint8_t)(rgb >> 16);
	v[9] = (uint8_t)(rgb >> 8);
	v[10] = (uint8_t)rgb;
	v[11] = (uint8_t)alpha;
	uint32_t h = ((uint32_t)(uint16_t)x * 73856093u) ^ ((uint32_t)(uint16_t)y * 19349663u) ^ ((uint32_t)(uint16_t)z * 83492791u) ^ (rgb * 2654435761u) ^ (uint32_t)alpha;
	for (uint32_t slot = h & (HASH_SIZE - 1);; slot = (slot + 1) & (HASH_SIZE - 1)) {
		uint32_t e = b->hash[slot];
		if (e >> 16 != b->generation) {
			uint32_t index = b->vertexCount++;
			memcpy(b->vertices + (size_t)index * MESH_VERTEX_SIZE, v, MESH_VERTEX_SIZE);
			b->hash[slot] = b->generation << 16 | index;
			return (int)index;
		}
		uint32_t index = e & 0xFFFF;
		if (memcmp(b->vertices + (size_t)index * MESH_VERTEX_SIZE, v, MESH_VERTEX_SIZE) == 0) return (int)index;
	}
}

static void add_triangle(Builder* b, int x0, int y0, int z0, uint32_t c0, int x1, int y1, int z1, uint32_t c1, int x2, int y2, int z2, uint32_t c2, int alpha) {
	if (b->failed) return;
	if (b->direct) {
		if (b->directCount + 3 > b->directMax) return;
		int px[3] = {x0, x1, x2}, py[3] = {y0, y1, y2}, pz[3] = {z0, z1, z2};
		uint32_t pc[3] = {c0, c1, c2};
		for (int k = 0; k < 3; k++) {
			uint8_t* v = b->direct + b->directCount++ * MESH_VERTEX_SIZE;
			int16_t p[4] = {clamp16(px[k]), clamp16(py[k]), clamp16(pz[k]), 0};
			memcpy(v, p, 8);
			v[8] = (uint8_t)(pc[k] >> 16);
			v[9] = (uint8_t)(pc[k] >> 8);
			v[10] = (uint8_t)pc[k];
			v[11] = (uint8_t)alpha;
		}
		return;
	}
	// A triangle with no area can never draw.
	long ux = x1 - x0, uy = y1 - y0, uz = z1 - z0, vx = x2 - x0, vy = y2 - y0, vz = z2 - z0;
	if (uy * vz - uz * vy == 0 && uz * vx - ux * vz == 0 && ux * vy - uy * vx == 0) return;
	if (b->vertexCount + 3 > MAX_VERTICES || b->indexCount[0] + 3 > MAX_INDICES) {
		if (finish_chunk(b) != 0 || start_chunk(b) != 0) {
			b->failed = 1;
			return;
		}
	}
	open_cell(b);
	MeshCell* c = &b->chunk->cells[b->cell];
	int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2}, zs[3] = {z0, z1, z2};
	uint32_t cs[3] = {c0, c1, c2};
	for (int k = 0; k < 3; k++) {
		int16_t x = clamp16(xs[k]), y = clamp16(ys[k]), z = clamp16(zs[k]);
		if (x < c->minX) c->minX = x;
		if (x > c->maxX) c->maxX = x;
		if (y < c->minY) c->minY = y;
		if (y > c->maxY) c->maxY = y;
		if (z < c->minZ) c->minZ = z;
		if (z > c->maxZ) c->maxZ = z;
		uint16_t index = (uint16_t)add_vertex(b, x, y, z, cs[k], alpha);
		b->indices[0][b->indexCount[0]++] = index;
		if (!b->stream0Only) b->indices[1][b->indexCount[1]++] = index;
	}
	c->count[0] += 3;
	if (!b->stream0Only) c->count[1] += 3;
	b->mesh->triangles++;
}

static uint32_t pal(const Builder* b, int hsl) {
	return b->palette[hsl & 0xFFFF];
}

static int multiply(int average, int lightness) {
	int v = lightness * (average & 0x7F) >> 7;
	v = v < 2 ? 2 : v > 126 ? 126 : v;
	return (average & 0xFF80) + v;
}

static uint32_t tile_color(const Builder* b, int texture, int color) {
	if (texture < 0) return pal(b, color);
	return pal(b, multiply(b->scene->types->textureAverage[texture], color));
}

static void add_terrain(Builder* b, int level, int x, int y) {
	const TerrainScene* t = &b->scene->terrain;
	int plain = t->plainIndex[level][x][y], shaped = t->shapedIndex[level][x][y];
	if (plain >= 0) {
		const PlainTile* p = &t->plain[plain];
		int x0 = x * 128, x1 = x0 + 128, z0 = y * 128, z1 = z0 + 128, tex = p->texture;
		int hSW = t->heights[level][x][y], hSE = t->heights[level][x + 1][y], hNE = t->heights[level][x + 1][y + 1], hNW = t->heights[level][x][y + 1];
		if (p->colorNE != TILE_NO_COLOR)
			add_triangle(b, x1, hNE, z1, tile_color(b, tex, p->colorNE), x0, hNW, z1, tile_color(b, tex, p->colorNW), x1, hSE, z0, tile_color(b, tex, p->colorSE), 255);
		if (p->colorSW != TILE_NO_COLOR)
			add_triangle(b, x0, hSW, z0, tile_color(b, tex, p->colorSW), x1, hSE, z0, tile_color(b, tex, p->colorSE), x0, hNW, z1, tile_color(b, tex, p->colorNW), 255);
	}
	if (shaped >= 0) {
		const ShapedTile* s = &t->shaped[shaped];
		for (int i = 0; i < s->triangleCount; i++) {
			if (s->ca[i] == TILE_NO_COLOR) continue;
			int a = s->ta[i], v = s->tb[i], c = s->tc[i], tex = s->tex[i];
			add_triangle(b, s->vx[a], s->vy[a], s->vz[a], tile_color(b, tex, s->ca[i]),
				s->vx[v], s->vy[v], s->vz[v], tile_color(b, tex, s->cb[i]),
				s->vx[c], s->vy[c], s->vz[c], tile_color(b, tex, s->cc[i]), 255);
		}
	}
}

static void add_entity(Builder* b, const SceneEntity* e, int wx, int wy, int wz) {
	if (!e || e->kind != ENTITY_LIT) return; // a model left unlit is never drawn by the client
	const SceneBase* m = e->base;
	for (int i = 0; i < m->triangleCount; i++) {
		if (e->fc[i] == -2) continue;
		int a = m->ta[i], v = m->tb[i], c = m->tc[i];
		int alpha = m->alpha ? 255 - (m->alpha[i] & 0xFF) : 255, flat = e->fc[i] == -1;
		uint32_t c0, c1, c2;
		if (e->textures && e->textures[i] != -1) {
			int average = b->scene->types->textureAverage[(uint16_t)e->textures[i]];
			c0 = pal(b, multiply(average, e->fa[i] & 0xFFFF));
			c1 = flat ? c0 : pal(b, multiply(average, e->fb[i] & 0xFFFF));
			c2 = flat ? c0 : pal(b, multiply(average, e->fc[i] & 0xFFFF));
		} else {
			c0 = pal(b, e->fa[i]);
			c1 = flat ? c0 : pal(b, e->fb[i]);
			c2 = flat ? c0 : pal(b, e->fc[i]);
		}
		add_triangle(b, e->vx[a] + wx, e->vy[a] + wy, e->vz[a] + wz, c0, e->vx[v] + wx, e->vy[v] + wy, e->vz[v] + wz, c1, e->vx[c] + wx, e->vy[c] + wy, e->vz[c] + wz, c2, alpha);
	}
}

static void begin_object(Builder* b) {
	b->objectChunk = b->mesh ? b->mesh->chunkCount : 0;
	b->objectStart[0] = b->indexCount[0];
	b->objectStart[1] = b->indexCount[1];
}

static void end_object(Builder* b, int level, int x, int y, int layer) {
	Mesh* m = b->mesh;
	if (!m || !m->objects || m->objectCount >= MESH_MAX_OBJECTS) return;
	// If the object crossed into a new chunk, only the part in that chunk can be hidden.
	if (m->chunkCount != b->objectChunk) b->objectStart[0] = b->objectStart[1] = 0;
	MeshObject* o = &m->objects[m->objectCount];
	o->start[0] = b->objectStart[0];
	o->start[1] = b->objectStart[1];
	o->count[0] = b->indexCount[0] - b->objectStart[0];
	o->count[1] = b->indexCount[1] - b->objectStart[1];
	if (o->count[0] == 0) return;
	o->chunk = (uint16_t)(m->chunkCount - 1);
	o->level = (uint8_t)level;
	o->x = (uint8_t)x;
	o->y = (uint8_t)y;
	o->layer = (uint8_t)layer;
	m->objectCount++;
}

static void add_tile_objects(Builder* b, int level, int x, int y) {
	const SceneTile* t = &b->scene->tiles[level][x][y];
	if (t->wall) {
		begin_object(b);
		add_entity(b, t->wall->primary, t->wall->xFine, t->wall->zFine, t->wall->yFine);
		add_entity(b, t->wall->secondary, t->wall->xFine, t->wall->zFine, t->wall->yFine);
		end_object(b, level, x, y, 0);
	}
	if (t->decor) {
		const SceneDecor* d = t->decor;
		begin_object(b);
		add_entity(b, d->primary, d->xFine + d->xOffset, d->height, d->yFine + d->yOffset);
		add_entity(b, d->secondary, d->xFine, d->height, d->yFine);
		end_object(b, level, x, y, 1);
	}
	for (int i = 0; i < t->sceneryCount; i++) {
		const SceneScenery* s = t->scenery[i];
		if (s->xMin != x || s->yMin != y) continue;
		begin_object(b);
		add_entity(b, s->entity, s->xFine, s->zFine, s->yFine);
		end_object(b, level, x, y, 2);
	}
}

static void add_tile_main(Builder* b, int level, int x, int y) {
	add_terrain(b, level, x, y);
	add_tile_objects(b, level, x, y);
}

size_t mesh_emit_tile(const Scene* scene, const uint32_t* palette, int level, int x, int y, uint8_t* out, size_t maxVertices) {
	Builder b;
	memset(&b, 0, sizeof(b));
	b.scene = scene;
	b.palette = palette;
	b.direct = out;
	b.directMax = maxVertices;
	add_tile_objects(&b, level, x, y);
	const SceneGround* g = scene->tiles[level][x][y].ground;
	if (g) add_entity(&b, g->entity, g->xFine, g->zFine, g->yFine);
	return b.directCount;
}

int mesh_index_model(const Scene* scene, const uint32_t* palette, const SceneBase* m, uint16_t* cornerVertex, uint8_t* colours, uint16_t* indices, uint32_t* indexCount) {
	Builder b;
	memset(&b, 0, sizeof(b));
	b.scene = scene;
	b.palette = palette;
	int* head = malloc(sizeof(int) * (size_t)(m->vertexCount + 1));
	int* next = malloc(sizeof(int) * ((size_t)m->triangleCount * 3 + 1));
	if (!head || !next) {
		free(head);
		free(next);
		return -1;
	}
	memset(head, 0xFF, sizeof(int) * (size_t)(m->vertexCount + 1));
	int count = 0;
	uint32_t written = 0;
	for (int i = 0; i < m->triangleCount; i++) {
		if (m->fc[i] == -2) continue;
		int corners[3] = {m->ta[i], m->tb[i], m->tc[i]};
		int alpha = m->alpha ? 255 - (m->alpha[i] & 0xFF) : 255, flat = m->fc[i] == -1;
		uint32_t c[3];
		if (m->litTextures && m->litTextures[i] != -1) {
			int average = scene->types->textureAverage[(uint16_t)m->litTextures[i]];
			c[0] = pal(&b, multiply(average, m->fa[i] & 0xFFFF));
			c[1] = flat ? c[0] : pal(&b, multiply(average, m->fb[i] & 0xFFFF));
			c[2] = flat ? c[0] : pal(&b, multiply(average, m->fc[i] & 0xFFFF));
		} else {
			c[0] = pal(&b, m->fa[i]);
			c[1] = flat ? c[0] : pal(&b, m->fb[i]);
			c[2] = flat ? c[0] : pal(&b, m->fc[i]);
		}
		for (int k = 0; k < 3; k++) {
			uint8_t rgba[4] = {(uint8_t)(c[k] >> 16), (uint8_t)(c[k] >> 8), (uint8_t)c[k], (uint8_t)alpha};
			int at = head[corners[k]];
			while (at != -1 && memcmp(colours + at * 4, rgba, 4) != 0) at = next[at];
			if (at == -1) {
				if (count > 65535) {
					free(head);
					free(next);
					return -1;
				}
				at = count++;
				memcpy(colours + at * 4, rgba, 4);
				cornerVertex[at] = (uint16_t)corners[k];
				next[at] = head[corners[k]];
				head[corners[k]] = at;
			}
			indices[written++] = (uint16_t)at;
		}
	}
	free(head);
	free(next);
	*indexCount = written;
	return count;
}

int mesh_build(Mesh* mesh, const Scene* scene, const uint32_t* palette, uint8_t* pool, size_t poolSize) {
	MeshObject* objects = mesh->objects;
	memset(mesh, 0, sizeof(*mesh));
	mesh->objects = objects ? objects : malloc(sizeof(MeshObject) * MESH_MAX_OBJECTS);
	mesh->pool = pool;
	mesh->poolSize = poolSize;
	Builder b;
	memset(&b, 0, sizeof(b));
	b.mesh = mesh;
	b.scene = scene;
	b.palette = palette;
	b.vertices = malloc((size_t)(MAX_VERTICES + 3) * MESH_VERTEX_SIZE);
	b.indices[0] = malloc(sizeof(uint16_t) * (MAX_INDICES + 3));
	b.indices[1] = malloc(sizeof(uint16_t) * (MAX_INDICES + 3));
	b.hash = calloc(HASH_SIZE, sizeof(uint32_t));
	int rc = -2;
	if (!b.vertices || !b.indices[0] || !b.indices[1] || !b.hash) goto done;
	rc = -1;
	for (int sector = 0; sector < MESH_SECTOR_GRID * MESH_SECTOR_GRID; sector++) {
		b.sector = sector;
		if (start_chunk(&b) != 0) goto done;
		int baseX = (sector % MESH_SECTOR_GRID) * 8, baseY = (sector / MESH_SECTOR_GRID) * 8;
		for (int cell = 0; cell < 16; cell++) {
			b.cell = cell;
			int cx = (cell & 1) | (cell >> 2 & 1) << 1, cy = (cell >> 1 & 1) | (cell >> 3 & 1) << 1;
			// Main geometry first, ground decor last, so the second index stream can leave decor out.
			for (int pass = 0; pass < 2; pass++) {
				b.stream0Only = pass;
				for (int level = 0; level < SCENE_LEVELS; level++)
					for (int dx = 0; dx < 2; dx++)
						for (int dy = 0; dy < 2; dy++) {
							int x = baseX + cx * 2 + dx, y = baseY + cy * 2 + dy;
							if (x >= SCENE_SIZE || y >= SCENE_SIZE) continue;
							if (pass == 0) add_tile_main(&b, level, x, y);
							else if (scene->tiles[level][x][y].ground) {
								const SceneGround* g = scene->tiles[level][x][y].ground;
								begin_object(&b);
								add_entity(&b, g->entity, g->xFine, g->zFine, g->yFine);
								end_object(&b, level, x, y, 3);
							}
						}
			}
			if (b.failed) goto done;
		}
		if (finish_chunk(&b) != 0) goto done;
	}
	rc = 0;
done:
	free(b.vertices);
	free(b.indices[0]);
	free(b.indices[1]);
	free(b.hash);
	return rc;
}
