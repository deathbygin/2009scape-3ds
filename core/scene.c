#include "scene.h"
#include "anim.h"
#include "player_colours.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int sinTable[2048], cosTable[2048], tablesReady = 0;

static void ensure_tables(void) {
	if (tablesReady) return;
	for (int i = 0; i < 2048; i++) {
		sinTable[i] = (int)(sin(i * 0.0030679615) * 65536.0);
		cosTable[i] = (int)(cos(i * 0.0030679615) * 65536.0);
	}
	tablesReady = 1;
}

static void* dup_mem(const void* p, size_t size) {
	if (!p) return NULL;
	void* c = malloc(size ? size : 1);
	if (c) memcpy(c, p, size);
	return c;
}

// ---- working model used while applying loc transforms ----

typedef struct {
	int vertexCount, triangleCount, texturedCount;
	int32_t *vx, *vy, *vz, *ta, *tb, *tc;
	int16_t *colors, *textures;
	int8_t *info, *alpha, *textureIndex;
	uint8_t* textureTypes;
	int32_t* vertexBones; // only while building a character
	int16_t *faceP, *faceM, *faceN; // only while building a character
} Work;

static int trackBones = 0;

static void work_from_model(Work* w, const RawModel* m) {
	size_t v = (size_t)m->vertexCount, t = (size_t)m->triangleCount;
	memset(w, 0, sizeof(*w));
	w->vertexCount = m->vertexCount;
	w->triangleCount = m->triangleCount;
	w->texturedCount = m->texturedCount;
	w->vx = dup_mem(m->vx, v * 4);
	w->vy = dup_mem(m->vy, v * 4);
	w->vz = dup_mem(m->vz, v * 4);
	w->ta = dup_mem(m->ta, t * 4);
	w->tb = dup_mem(m->tb, t * 4);
	w->tc = dup_mem(m->tc, t * 4);
	w->colors = dup_mem(m->colors, t * 2);
	w->textures = dup_mem(m->textures, t * 2);
	w->info = dup_mem(m->info, t);
	w->alpha = dup_mem(m->alpha, t);
	w->textureIndex = dup_mem(m->textureIndex, t);
	w->textureTypes = dup_mem(m->textureTypes, (size_t)m->texturedCount);
	if (trackBones && m->vertexBones) w->vertexBones = dup_mem(m->vertexBones, v * 4);
}

static int add_vertex(Work* w, const RawModel* m, int vertex) {
	int x = m->vx[vertex], y = m->vy[vertex], z = m->vz[vertex];
	for (int i = 0; i < w->vertexCount; i++)
		if (x == w->vx[i] && y == w->vy[i] && z == w->vz[i]) return i;
	w->vx[w->vertexCount] = x;
	w->vy[w->vertexCount] = y;
	if (w->vertexBones && m->vertexBones) w->vertexBones[w->vertexCount] = m->vertexBones[vertex];
	w->vz[w->vertexCount] = z;
	return w->vertexCount++;
}

// new RawModel(models, count)
static void work_merge(Work* w, const RawModel* const* models, int count) {
	int vertices = 0, triangles = 0, textured = 0, keepInfo = 0, keepAlpha = 0, keepTextures = 0, keepIndex = 0;
	memset(w, 0, sizeof(*w));
	for (int i = 0; i < count; i++) {
		const RawModel* m = models[i];
		vertices += m->vertexCount;
		triangles += m->triangleCount;
		textured += m->texturedCount;
		keepInfo |= m->info != NULL;
		keepAlpha |= m->alpha != NULL;
		keepTextures |= m->textures != NULL;
		keepIndex |= m->textureIndex != NULL;
	}
	w->vx = calloc((size_t)vertices + 1, 4);
	w->vy = calloc((size_t)vertices + 1, 4);
	w->vz = calloc((size_t)vertices + 1, 4);
	if (trackBones) w->vertexBones = calloc((size_t)vertices + 1, 4);
	w->ta = calloc((size_t)triangles + 1, 4);
	w->tb = calloc((size_t)triangles + 1, 4);
	w->tc = calloc((size_t)triangles + 1, 4);
	w->colors = calloc((size_t)triangles + 1, 2);
	if (keepInfo) w->info = calloc((size_t)triangles + 1, 1);
	if (keepAlpha) w->alpha = calloc((size_t)triangles + 1, 1);
	if (keepTextures) w->textures = calloc((size_t)triangles + 1, 2);
	if (keepIndex) w->textureIndex = calloc((size_t)triangles + 1, 1);
	if (textured > 0) w->textureTypes = calloc((size_t)textured, 1);
	if (trackBones && textured > 0) {
		w->faceP = calloc((size_t)textured, 2);
		w->faceM = calloc((size_t)textured, 2);
		w->faceN = calloc((size_t)textured, 2);
	}
	for (int i = 0; i < count; i++) {
		const RawModel* m = models[i];
		for (int t = 0; t < m->triangleCount; t++) {
			int n = w->triangleCount;
			if (keepInfo && m->info) w->info[n] = m->info[t];
			if (keepAlpha && m->alpha) w->alpha[n] = m->alpha[t];
			if (keepTextures) w->textures[n] = m->textures ? m->textures[t] : -1;
			if (keepIndex) w->textureIndex[n] = !m->textureIndex || m->textureIndex[t] == -1 ? -1 : (int8_t)(m->textureIndex[t] + w->texturedCount);
			w->colors[n] = m->colors[t];
			w->ta[n] = add_vertex(w, m, m->ta[t]);
			w->tb[n] = add_vertex(w, m, m->tb[t]);
			w->tc[n] = add_vertex(w, m, m->tc[t]);
			w->triangleCount++;
		}
		for (int t = 0; t < m->texturedCount; t++) {
			int type = m->textureTypes[t], at = w->texturedCount;
			w->textureTypes[w->texturedCount++] = (uint8_t)type;
			// Texture mapping corners count as vertices even when no triangle uses them.
			if (type == 0) {
				int p = add_vertex(w, m, m->faceP[t]), q = add_vertex(w, m, m->faceM[t]), r = add_vertex(w, m, m->faceN[t]);
				if (w->faceP) {
					w->faceP[at] = (int16_t)p;
					w->faceM[at] = (int16_t)q;
					w->faceN[at] = (int16_t)r;
				}
			}
		}
	}
}

static void negate_z_reverse(Work* w) {
	for (int i = 0; i < w->vertexCount; i++) w->vz[i] = -w->vz[i];
	for (int i = 0; i < w->triangleCount; i++) {
		int t = w->ta[i];
		w->ta[i] = w->tc[i];
		w->tc[i] = t;
	}
}

static void rotate256(int32_t* vx, int32_t* vz, int count) {
	int s = sinTable[256], c = cosTable[256];
	for (int i = 0; i < count; i++) {
		int x = (vz[i] * s + vx[i] * c) >> 16;
		vz[i] = (vz[i] * c - vx[i] * s) >> 16;
		vx[i] = x;
	}
}

static void translate(int32_t* vx, int32_t* vy, int32_t* vz, int count, int x, int y, int z) {
	for (int i = 0; i < count; i++) {
		vx[i] += x;
		vy[i] += y;
		vz[i] += z;
	}
}

static void rotate_x(int32_t* vy, int32_t* vz, int count, int angle) {
	int s = sinTable[angle], c = cosTable[angle];
	for (int i = 0; i < count; i++) {
		int y = (vy[i] * c - vz[i] * s) >> 16;
		vz[i] = (vy[i] * s + vz[i] * c) >> 16;
		vy[i] = y;
	}
}

static void rotate_z(int32_t* vx, int32_t* vy, int count, int angle) {
	int s = sinTable[angle], c = cosTable[angle];
	for (int i = 0; i < count; i++) {
		int x = (vy[i] * s + vx[i] * c) >> 16;
		vy[i] = (vy[i] * c - vx[i] * s) >> 16;
		vx[i] = x;
	}
}

// ---- normals and lighting ----

static void calculate_normals(SceneBase* b) {
	b->normals = calloc((size_t)b->vertexCount + 1, sizeof(VertexNormal));
	for (int i = 0; i < b->triangleCount; i++) {
		int a = b->ta[i], v = b->tb[i], c = b->tc[i];
		int dxAB = b->vx[v] - b->vx[a], dyAB = b->vy[v] - b->vy[a], dzAB = b->vz[v] - b->vz[a];
		int dxAC = b->vx[c] - b->vx[a], dyAC = b->vy[c] - b->vy[a], dzAC = b->vz[c] - b->vz[a];
		int nx = dyAB * dzAC - dyAC * dzAB, ny = dzAB * dxAC - dzAC * dxAB, nz = dxAB * dyAC - dxAC * dyAB;
		while (nx > 8192 || ny > 8192 || nz > 8192 || nx < -8192 || ny < -8192 || nz < -8192) {
			nx >>= 1;
			ny >>= 1;
			nz >>= 1;
		}
		int length = (int)sqrt((double)(nx * nx + ny * ny + nz * nz));
		if (length <= 0) length = 1;
		nx = nx * 256 / length;
		ny = ny * 256 / length;
		nz = nz * 256 / length;
		int info = b->info ? b->info[i] : 0;
		if (info == 0) {
			VertexNormal* n[3] = {&b->normals[a], &b->normals[v], &b->normals[c]};
			for (int k = 0; k < 3; k++) {
				n[k]->x += nx;
				n[k]->y += ny;
				n[k]->z += nz;
				n[k]->magnitude++;
			}
		} else if (info == 1) {
			if (!b->triangleNormals) b->triangleNormals = calloc((size_t)b->triangleCount + 1, sizeof(TriangleNormal));
			b->triangleNormals[i].x = nx;
			b->triangleNormals[i].y = ny;
			b->triangleNormals[i].z = nz;
		}
	}
}

static int clamp_light(int v) {
	return v < 2 ? 2 : v > 126 ? 126 : v;
}

static int multiply2(int a, int b) {
	return (a & 0xFF80) + clamp_light(b * (a & 0x7F) >> 7);
}

// Objects use one light direction and NPCs another.
static int LIGHT_X = -50, LIGHT_Y = -10, LIGHT_Z = -50;

static int shade(const VertexNormal* n, int ambient, int contrastLight) {
	return ambient + (LIGHT_X * n->x + LIGHT_Y * n->y + LIGHT_Z * n->z) / (contrastLight * n->magnitude);
}

// new SoftwareModel(model, ambient, contrast, -50, -10, -50)
static void light_model(const SceneBase* b, const int8_t* info, const VertexNormal* merged, const uint8_t* mergedSet,
	const TerrainTypes* types, int32_t* fa, int32_t* fb, int32_t* fc, int16_t* textures) {
	int magnitude = (int)sqrt((double)(LIGHT_X * LIGHT_X + LIGHT_Y * LIGHT_Y + LIGHT_Z * LIGHT_Z));
	int ambient = b->ambient, contrastLight = b->contrast * magnitude >> 8;
	if (textures) {
		for (int i = 0; i < b->triangleCount; i++) {
			int t = b->textures[i];
			textures[i] = t != -1 && (uint32_t)(uint16_t)t < types->textureCount && types->textureRepeating[(uint16_t)t] ? (int16_t)t : -1;
		}
	}
	if (b->texturedCount > 0 && b->textureIndex && textures) {
		for (int i = 0; i < b->triangleCount; i++) {
			int index = b->textureIndex[i];
			if (index != -1 && b->textureTypes[index & 0xFF] != 0) textures[i] = -1;
		}
	}
	for (int i = 0; i < b->triangleCount; i++) {
		int render = info ? info[i] : 0, alpha = b->alpha ? b->alpha[i] : 0, texture = textures ? textures[i] : -1;
		if (alpha == -2) render = 3;
		if (alpha == -1) render = 2;
		int a = b->ta[i], v = b->tb[i], c = b->tc[i];
		const VertexNormal* na = merged && mergedSet[a] ? &merged[a] : &b->normals[a];
		const VertexNormal* nb = merged && mergedSet[v] ? &merged[v] : &b->normals[v];
		const VertexNormal* nc = merged && mergedSet[c] ? &merged[c] : &b->normals[c];
		if (texture == -1) {
			if (render == 0) {
				int color = b->colors[i] & 0xFFFF;
				int l = shade(na, ambient, contrastLight) << 17;
				fa[i] = l | multiply2(color, l >> 17);
				l = shade(nb, ambient, contrastLight) << 17;
				fb[i] = l | multiply2(color, l >> 17);
				l = shade(nc, ambient, contrastLight) << 17;
				fc[i] = l | multiply2(color, l >> 17);
			} else if (render == 1) {
				const TriangleNormal* n = &b->triangleNormals[i];
				int l = (ambient + (LIGHT_X * n->x + LIGHT_Y * n->y + LIGHT_Z * n->z) / (contrastLight + contrastLight / 2)) << 17;
				fa[i] = l | multiply2(b->colors[i] & 0xFFFF, l >> 17);
				fb[i] = 0;
				fc[i] = -1;
			} else if (render == 3) {
				fa[i] = 128;
				fb[i] = 0;
				fc[i] = -1;
			} else {
				fa[i] = 0;
				fb[i] = 0;
				fc[i] = -2;
			}
		} else if (render == 0) {
			fa[i] = clamp_light(shade(na, ambient, contrastLight));
			fb[i] = clamp_light(shade(nb, ambient, contrastLight));
			fc[i] = clamp_light(shade(nc, ambient, contrastLight));
		} else if (render == 1) {
			const TriangleNormal* n = &b->triangleNormals[i];
			fa[i] = clamp_light(ambient + (LIGHT_X * n->x + LIGHT_Y * n->y + LIGHT_Z * n->z) / (contrastLight + contrastLight / 2));
			fb[i] = 0;
			fc[i] = -1;
		} else {
			fa[i] = 0;
			fb[i] = 0;
			fc[i] = -2;
		}
	}
}

// ---- loc lookup ----

// Definitions never change, so they stay cached for the life of the program.
static LocDef** locDefs = NULL;
static Js5Buffer* locGroups = NULL;
static uint32_t locGroupCount = 0;

static const LocDef* get_loc(Scene* s, int id) {
	if (!locDefs) {
		locGroupCount = s->cache->index[16].capacity;
		locDefs = calloc((size_t)locGroupCount * 256, sizeof(LocDef*));
		locGroups = calloc(locGroupCount, sizeof(Js5Buffer));
		if (!locDefs || !locGroups) return NULL;
	}
	uint32_t group = (uint32_t)id >> 8;
	if (id < 0 || group >= locGroupCount) return NULL;
	if (locDefs[id]) return locDefs[id];
	if (!locGroups[group].data && js5_read_group(s->cache, 16, group, NULL, &locGroups[group]) != 0) {
		locGroups[group].data = NULL;
		return NULL;
	}
	LocDef* d = malloc(sizeof(LocDef));
	if (!d) return NULL;
	Js5Buffer file = {0, 0};
	if (js5_group_file(&s->cache->index[16], group, &locGroups[group], (uint32_t)(id & 255), &file) == 0) {
		loc_decode(d, id, file.data, file.size);
		free(file.data);
	} else {
		loc_decode(d, id, NULL, 0);
	}
	locDefs[id] = d;
	return d;
}

const LocDef* scene_get_loc(Scene* s, int id) {
	return get_loc(s, id);
}

// Decoded models are kept for one scene build, since most are used at several rotations.
static const RawModel* get_model(Scene* s, int id) {
	uint32_t bucket = (uint32_t)id % SCENE_MODEL_BUCKETS;
	for (SceneModel* m = s->models[bucket]; m; m = m->next)
		if (m->id == id) return &m->model;
	Js5Buffer b;
	if (js5_read_group(s->cache, 7, (uint32_t)id, NULL, &b) != 0) return NULL;
	SceneModel* m = malloc(sizeof(SceneModel));
	if (!m) {
		free(b.data);
		return NULL;
	}
	int rc = model_decode(&m->model, b.data, b.size);
	free(b.data);
	if (rc != 0) {
		free(m);
		return NULL;
	}
	m->id = id;
	m->next = s->models[bucket];
	s->models[bucket] = m;
	return &m->model;
}

// LocType.getRawModel. Returns 0 on success, nonzero when the loc has no model for this shape.
static int build_work(Scene* s, const LocDef* d, int rotation, int shape, Work* w) {
	int mirrored = d->mirror;
	if (shape == 2 && rotation > 3) mirrored = !mirrored;
	if (!d->shapes) {
		if (shape != 10 || !d->models) return -1;
		RawModel copies[4];
		const RawModel* list[4];
		int count = d->modelCount < 4 ? d->modelCount : 4;
		for (int i = 0; i < count; i++) {
			if (!(list[i] = get_model(s, d->models[i] & 0xFFFF))) return -2;
		}
		if (count > 1) {
			if (mirrored) {
				// The client mirrors each part before merging.
				for (int i = 0; i < count; i++) {
					copies[i] = *list[i];
					copies[i].vz = dup_mem(list[i]->vz, (size_t)list[i]->vertexCount * 4);
					copies[i].ta = dup_mem(list[i]->tc, (size_t)list[i]->triangleCount * 4);
					copies[i].tc = dup_mem(list[i]->ta, (size_t)list[i]->triangleCount * 4);
					for (int v = 0; v < copies[i].vertexCount; v++) copies[i].vz[v] = -copies[i].vz[v];
					list[i] = &copies[i];
				}
			}
			work_merge(w, list, count);
			if (mirrored) {
				for (int i = 0; i < count; i++) {
					free(copies[i].vz);
					free(copies[i].ta);
					free(copies[i].tc);
				}
			}
		} else {
			work_from_model(w, list[0]);
			if (mirrored) negate_z_reverse(w);
		}
	} else {
		int index = -1;
		for (int i = 0; i < d->modelCount; i++) {
			if (d->shapes[i] == shape) {
				index = i;
				break;
			}
		}
		if (index == -1) return -1;
		const RawModel* m = get_model(s, d->models[index] & 0xFFFF);
		if (!m) return -2;
		work_from_model(w, m);
		if (mirrored) negate_z_reverse(w);
	}
	if (shape == 4 && rotation > 3) {
		rotate256(w->vx, w->vz, w->vertexCount);
		translate(w->vx, w->vy, w->vz, w->vertexCount, 45, 0, -45);
	}
	int quadrant = rotation & 3;
	for (int v = 0; v < w->vertexCount; v++) {
		int x = w->vx[v], z = w->vz[v];
		if (quadrant == 1) {
			w->vx[v] = z;
			w->vz[v] = -x;
		} else if (quadrant == 2) {
			w->vx[v] = -x;
			w->vz[v] = -z;
		} else if (quadrant == 3) {
			w->vz[v] = x;
			w->vx[v] = -z;
		}
	}
	for (int j = 0; j < d->recolCount; j++) {
		int16_t dest = !d->recolPalette || d->recolPaletteCount <= j ? d->recolD[j] : 0;
		for (int i = 0; i < w->triangleCount; i++) if (w->colors[i] == d->recolS[j]) w->colors[i] = dest;
	}
	if (w->textures) {
		for (int j = 0; j < d->retexCount; j++)
			for (int i = 0; i < w->triangleCount; i++) if (w->textures[i] == d->retexS[j]) w->textures[i] = d->retexD[j];
	}
	if (d->resizeX != 128 || d->resizeY != 128 || d->resizeZ != 128) {
		for (int v = 0; v < w->vertexCount; v++) {
			w->vx[v] = w->vx[v] * d->resizeX / 128;
			w->vy[v] = w->vy[v] * d->resizeY / 128;
			w->vz[v] = w->vz[v] * d->resizeZ / 128;
		}
	}
	if (d->xoff || d->yoff || d->zoff) translate(w->vx, w->vy, w->vz, w->vertexCount, d->xoff, d->yoff, d->zoff);
	return 0;
}

static SceneBase* get_base(Scene* s, const LocDef* d, int rotation, int shape, int shareLight) {
	int key0 = d->id, key1 = rotation | (d->shapes ? shape << 3 : 0) | (shareLight ? 1 << 12 : 0);
	uint32_t bucket = ((uint32_t)key0 * 2654435761u ^ (uint32_t)key1 * 40503u) % SCENE_BASE_BUCKETS;
	for (SceneBase* b = s->bases[bucket]; b; b = b->next)
		if (b->key0 == key0 && b->key1 == key1) return b;
	Work w;
	if (build_work(s, d, rotation, shape, &w) != 0) return NULL;
	if (shape == 10 && rotation > 3) rotate256(w.vx, w.vz, w.vertexCount);
	SceneBase* b = calloc(1, sizeof(SceneBase));
	if (!b) return NULL;
	b->key0 = key0;
	b->key1 = key1;
	b->vertexCount = w.vertexCount;
	b->triangleCount = w.triangleCount;
	b->texturedCount = w.texturedCount;
	b->vx = w.vx; b->vy = w.vy; b->vz = w.vz;
	b->ta = w.ta; b->tb = w.tb; b->tc = w.tc;
	b->colors = w.colors;
	b->textures = w.textures;
	b->info = w.info;
	b->alpha = w.alpha;
	b->textureIndex = w.textureIndex;
	b->textureTypes = w.textureTypes;
	b->vertexBones = w.vertexBones;
	b->ambient = (int16_t)(d->ambient + 64);
	b->contrast = (int16_t)(d->contrast * 5 + 768);
	b->shareLight = (uint8_t)shareLight;
	calculate_normals(b);
	if (!shareLight) {
		size_t t = (size_t)b->triangleCount + 1;
		b->fa = malloc(t * 4);
		b->fb = malloc(t * 4);
		b->fc = malloc(t * 4);
		if (b->textures) b->litTextures = malloc(t * 2);
		light_model(b, b->info, NULL, NULL, s->types, b->fa, b->fb, b->fc, b->litTextures);
	}
	b->next = s->bases[bucket];
	s->bases[bucket] = b;
	s->baseCount++;
	return b;
}

// ---- terrain fitting ----

typedef struct {
	int minX, maxX, minY, maxY, minZ, maxZ;
} Bounds;

static Bounds bounds_of(const int32_t* vx, const int32_t* vy, const int32_t* vz, int count) {
	int minX = 32767, minY = 32767, minZ = 32767, maxX = -32768, maxY = -32768, maxZ = -32768;
	for (int v = 0; v < count; v++) {
		if (vx[v] < minX) minX = vx[v];
		if (vx[v] > maxX) maxX = vx[v];
		if (vy[v] < minY) minY = vy[v];
		if (vy[v] > maxY) maxY = vy[v];
		if (vz[v] < minZ) minZ = vz[v];
		if (vz[v] > maxZ) maxZ = vz[v];
	}
	Bounds b = {(int16_t)minX, (int16_t)maxX, (int16_t)minY, (int16_t)maxY, (int16_t)minZ, (int16_t)maxZ};
	return b;
}

typedef int32_t (*HeightMap)[105];

static int height_at(HeightMap h, int tileX, int tileZ, int fracX, int fracZ) {
	int a = (h[tileX][tileZ] * (128 - fracX) + h[tileX + 1][tileZ] * fracX) >> 7;
	int b = (h[tileX][tileZ + 1] * (128 - fracX) + h[tileX + 1][tileZ + 1] * fracX) >> 7;
	return (a * (128 - fracZ) + b * fracZ) >> 7;
}

static int interpolate_height(HeightMap h, int fineX, int fineZ) {
	int tileX = fineX >> 7, tileZ = fineZ >> 7;
	if (tileX < 0 || tileZ < 0 || tileX >= 104 || tileZ >= 104) return 0;
	return height_at(h, tileX, tileZ, fineX & 0x7F, fineZ & 0x7F);
}

static void align_to_terrain(HeightMap h, int32_t* vx, int32_t* vy, int32_t* vz, int count, int xFine, int base, int zFine, int sizeX, int sizeZ) {
	int sw = interpolate_height(h, xFine + -sizeX / 2, zFine + -sizeZ / 2);
	int se = interpolate_height(h, xFine + sizeX / 2, zFine + -sizeZ / 2);
	int nw = interpolate_height(h, xFine + -sizeX / 2, zFine + sizeZ / 2);
	int ne = interpolate_height(h, xFine + sizeX / 2, zFine + sizeZ / 2);
	int minSouth = sw < se ? sw : se, minNorth = nw < ne ? nw : ne, minEast = se < ne ? se : ne, minWest = sw < nw ? sw : nw;
	if (sizeZ != 0) {
		int angle = (int)(atan2((double)(minSouth - minNorth), (double)sizeZ) * 325.95) & 0x7FF;
		if (angle != 0) rotate_x(vy, vz, count, angle);
	}
	if (sizeX != 0) {
		int angle = (int)(atan2((double)(minWest - minEast), (double)sizeX) * 325.95) & 0x7FF;
		if (angle != 0) rotate_z(vx, vy, count, angle);
	}
	int avg = sw + ne;
	if (se + nw < avg) avg = se + nw;
	avg = (avg >> 1) - base;
	if (avg != 0) translate(vx, vy, vz, count, 0, avg, 0);
}

// Shared by SoftwareModel.alignToTerrain and RawModel.placeOnTerrain. Returns 1 if the entity was changed.
static int fit_to_terrain(SceneEntity* e, int type, int amount, HeightMap h, HeightMap above, int xFine, int zFine, int base) {
	SceneBase* b = e->base;
	int count = b->vertexCount;
	Bounds bb = bounds_of(e->vx, e->vy, e->vz, count);
	int west = xFine + bb.minX, east = xFine + bb.maxX, south = zFine + bb.minZ, north = zFine + bb.maxZ;
	if ((type == 1 || type == 2 || type == 3 || type == 5) && (west < 0 || (east + 128) >> 7 >= 105 || south < 0 || (north + 128) >> 7 >= 105)) return 0;
	if (type == 4 || type == 5) {
		if (!above) return 0;
		if (west < 0 || (east + 128) >> 7 >= 105 || south < 0 || (north + 128) >> 7 >= 105) return 0;
	} else {
		west >>= 7;
		east = (east + 127) >> 7;
		south >>= 7;
		north = (north + 127) >> 7;
		if (h[west][south] == base && h[east][south] == base && h[west][north] == base && h[east][north] == base) return 0;
	}
	int32_t* src = e->vy;
	if (type == 3) {
		int32_t* ox = dup_mem(e->vx, (size_t)count * 4 + 4);
		int32_t* oy = dup_mem(e->vy, (size_t)count * 4 + 4);
		int32_t* oz = dup_mem(e->vz, (size_t)count * 4 + 4);
		int sizeX = (amount & 0xFF) * 4, sizeZ = (amount >> 8 & 0xFF) * 4;
		if (e->kind == ENTITY_RAW) {
			// The client rotates the shared model here and returns an unrotated copy, so later placements inherit the change.
			align_to_terrain(h, e->vx, e->vy, e->vz, count, xFine, base, zFine, sizeX, sizeZ);
		} else {
			align_to_terrain(h, ox, oy, oz, count, xFine, base, zFine, sizeX, sizeZ);
		}
		e->vx = ox;
		e->vy = oy;
		e->vz = oz;
		e->ownsXZ = 1;
		e->ownsY = 1;
		return 1;
	}
	int32_t* out = malloc((size_t)count * 4 + 4);
	if (!out) return 0;
	for (int v = 0; v < count; v++) {
		int wx = e->vx[v] + xFine, wz = e->vz[v] + zFine;
		int fx = wx & 0x7F, fz = wz & 0x7F, tx = wx >> 7, tz = wz >> 7;
		if (type == 1) {
			out[v] = src[v] + height_at(h, tx, tz, fx, fz) - base;
		} else if (type == 2) {
			int f = bb.minY ? (src[v] << 16) / bb.minY : 0;
			out[v] = f < amount ? src[v] + (height_at(h, tx, tz, fx, fz) - base) * (amount - f) / amount : src[v];
		} else if (type == 4) {
			out[v] = src[v] + height_at(above, tx, tz, fx, fz) + (bb.maxY - bb.minY) - base;
		} else {
			int ground = height_at(h, tx, tz, fx, fz), upper = height_at(above, tx, tz, fx, fz);
			int span = bb.maxY - bb.minY;
			out[v] = ((span ? (src[v] << 8) / span : 0) * (ground - upper) >> 8) - (base - ground);
		}
	}
	e->vy = out;
	e->ownsY = 1;
	return 1;
}

// LocType.getStaticEntity. Returns NULL when the loc has no model for this shape.
static SceneEntity* make_static(Scene* s, const LocDef* d, int rotation, int shape, HeightMap h, HeightMap above, int base, int xFine, int zFine) {
	int shareLight = d->computeVertexColors;
	SceneBase* b = get_base(s, d, rotation, shape, shareLight);
	if (!b) return NULL;
	SceneEntity* e = calloc(1, sizeof(SceneEntity));
	if (!e) return NULL;
	e->kind = shareLight ? ENTITY_RAW : ENTITY_LIT;
	e->base = b;
	e->vx = b->vx;
	e->vy = b->vy;
	e->vz = b->vz;
	if (shareLight) {
		if (b->info) {
			e->info = dup_mem(b->info, (size_t)b->triangleCount);
			e->ownsInfo = 1;
		}
	} else {
		e->fa = b->fa;
		e->fb = b->fb;
		e->fc = b->fc;
		e->textures = b->litTextures;
	}
	if (d->hillskewType != 0) fit_to_terrain(e, d->hillskewType, d->hillskewAmount, h, above, xFine, zFine, base);
	s->entityCount++;
	return e;
}

static SceneEntity* make_dynamic(Scene* s, const LocDef* d, int shape, int rotation, int level, int x, int y) {
	SceneEntity* e = calloc(1, sizeof(SceneEntity));
	if (!e) return NULL;
	e->kind = ENTITY_DYNAMIC;
	e->locId = d->id;
	e->shape = shape;
	e->rotation = rotation;
	e->level = level;
	e->tileX = x;
	e->tileY = y;
	s->entityCount++;
	return e;
}

static void free_entity(SceneEntity* e) {
	if (!e) return;
	if (e->ownsXZ) {
		free(e->vx);
		free(e->vz);
	}
	if (e->ownsY) free(e->vy);
	if (e->ownsInfo) free(e->info);
	if (e->ownsLit) {
		free(e->fa);
		free(e->fb);
		free(e->fc);
		free(e->textures);
	}
	free(e->merged);
	free(e->mergedSet);
	free(e);
}

// ---- placing entities ----

static int add_scenery(Scene* s, int level, int x, int y, int zFine, int xSize, int ySize, SceneEntity* e) {
	if (!e) return 1;
	for (int tx = x; tx < x + xSize; tx++)
		for (int ty = y; ty < y + ySize; ty++) {
			if (tx < 0 || ty < 0 || tx >= SCENE_SIZE || ty >= SCENE_SIZE) return 0;
			if (s->tiles[level][tx][ty].sceneryCount >= 5) return 0;
		}
	SceneScenery* c = calloc(1, sizeof(SceneScenery));
	if (!c) return 0;
	c->entity = e;
	c->xFine = x * 128 + xSize * 64;
	c->yFine = y * 128 + ySize * 64;
	c->zFine = zFine;
	c->xMin = x;
	c->yMin = y;
	c->xMax = x + xSize - 1;
	c->yMax = y + ySize - 1;
	c->locId = s->placeLocId;
	c->shape = s->placeShape;
	c->orientation = s->placeOrientation;
	c->interactable = s->placeInteractable;
	c->hasMapscene = s->placeMapscene;
	for (int tx = x; tx < x + xSize; tx++)
		for (int ty = y; ty < y + ySize; ty++) {
			SceneTile* t = &s->tiles[level][tx][ty];
			t->scenery[t->sceneryCount++] = c;
		}
	return 1;
}

static void set_wall(Scene* s, int level, int x, int y, int zFine, SceneEntity* primary, SceneEntity* secondary, int locId) {
	if (!primary && !secondary) return;
	SceneWall* w = calloc(1, sizeof(SceneWall));
	if (!w) return;
	w->primary = primary;
	w->secondary = secondary;
	w->xFine = x * 128 + 64;
	w->yFine = y * 128 + 64;
	w->zFine = zFine;
	w->locId = locId;
	w->shape = s->placeShape;
	w->orientation = s->placeOrientation;
	w->interactable = s->placeInteractable;
	w->hasMapscene = s->placeMapscene;
	s->tiles[level][x][y].wall = w;
}

static void set_decor(Scene* s, int level, int x, int y, int height, SceneEntity* primary, SceneEntity* secondary, int xOffset, int yOffset, int locId) {
	if (!primary) return;
	SceneDecor* d = calloc(1, sizeof(SceneDecor));
	if (!d) return;
	d->primary = primary;
	d->secondary = secondary;
	d->xFine = x * 128 + 64;
	d->yFine = y * 128 + 64;
	d->height = height;
	d->xOffset = xOffset;
	d->yOffset = yOffset;
	d->locId = locId;
	s->tiles[level][x][y].decor = d;
}

static void adjust_decor(Scene* s, int level, int x, int y, int walloff) {
	SceneDecor* d = s->tiles[level][x][y].decor;
	if (d) {
		d->xOffset = d->xOffset * walloff / 16;
		d->yOffset = d->yOffset * walloff / 16;
	}
}

static const int DECO_X[4] = {1, 0, -1, 0}, DECO_Y[4] = {0, -1, 0, 1};
static const int OFFSET_X[4] = {1, -1, -1, 1}, OFFSET_Y[4] = {-1, -1, 1, 1};

static int wall_walloff(Scene* s, int level, int x, int y, int fallback, int divisor) {
	SceneWall* w = s->tiles[level][x][y].wall;
	if (!w) return fallback;
	const LocDef* d = get_loc(s, w->locId);
	return d ? d->walloff / divisor : fallback;
}

static void shadow_corner(TerrainScene* t, int plane, int x, int y, int orientation) {
	if (orientation == 0) t->shadow[plane][x][y + 1] = 50;
	else if (orientation == 1) t->shadow[plane][x + 1][y + 1] = 50;
	else if (orientation == 2) t->shadow[plane][x + 1][y] = 50;
	else t->shadow[plane][x][y] = 50;
}

void scene_add_loc(Scene* s, const LocPlacement* p) {
	TerrainScene* t = &s->terrain;
	const LocDef* def = get_loc(s, p->id);
	if (!def) return;
	LocDef d = *def;
	s->placeLocId = p->id;
	s->placeShape = (uint8_t)p->shape;
	s->placeOrientation = (uint8_t)p->orientation;
	s->placeInteractable = d.interactable != 0;
	s->placeMapscene = d.mapscene != -1;
	int plane = p->plane, x = p->x, y = p->y, orientation = p->orientation, shape = p->shape;
	int width = orientation == 1 || orientation == 3 ? d.length : d.width;
	int length = orientation == 1 || orientation == 3 ? d.width : d.length;
	int west, east, south, north;
	if (x + width <= 104) {
		west = x + (width >> 1);
		east = x + ((width + 1) >> 1);
	} else {
		east = x + 1;
		west = x;
	}
	if (length + y > 104) {
		south = y;
		north = y + 1;
	} else {
		south = (length >> 1) + y;
		north = y + ((length + 1) >> 1);
	}
	HeightMap h = t->heights[plane];
	HeightMap above = plane < 3 ? t->heights[plane + 1] : NULL;
	int xFine = (width << 6) + (x << 7), yFine = (length << 6) + (y << 7);
	int average = (h[west][north] + h[east][south] + h[west][south] + h[east][north]) >> 2;
	int dynamic = d.anim != -1 || d.multiLocs != NULL || d.dynamic;
#define ENTITY(rotation, modelShape) (dynamic ? make_dynamic(s, &d, modelShape, rotation, plane, x, y) : make_static(s, &d, rotation, modelShape, h, above, average, xFine, yFine))

	if (shape == 22) {
		if (1) { // showGroundDecorations is on by default
			SceneEntity* e = ENTITY(orientation, 22);
			if (e) {
				SceneGround* g = calloc(1, sizeof(SceneGround));
				if (g) {
					g->entity = e;
					g->xFine = x * 128 + 64;
					g->yFine = y * 128 + 64;
					g->zFine = average;
					g->locId = p->id;
					s->tiles[plane][x][y].ground = g;
				}
			}
		}
	} else if (shape == 10 || shape == 11) {
		SceneEntity* e = ENTITY(shape == 11 ? orientation + 4 : orientation, 10);
		if (e) {
			int added = add_scenery(s, plane, x, y, average, width, length, e);
			if (d.active && added) {
				int size = 15;
				if (e->kind == ENTITY_LIT) {
					long best = 0;
					for (int v = 0; v < e->base->vertexCount; v++) {
						long r = (long)e->vx[v] * e->vx[v] + (long)e->vz[v] * e->vz[v];
						if (r > best) best = r;
					}
					size = (int16_t)(sqrt((double)best) + 0.99) / 4;
					if (size > 30) size = 30;
				}
				for (int sx = 0; sx <= width; sx++)
					for (int sy = 0; sy <= length; sy++)
						if (t->shadow[plane][x + sx][y + sy] < size) t->shadow[plane][x + sx][y + sy] = (int8_t)size;
			}
		}
	} else if (shape >= 12) {
		add_scenery(s, plane, x, y, average, 1, 1, ENTITY(orientation, shape));
	} else if (shape == 0) {
		set_wall(s, plane, x, y, average, ENTITY(orientation, 0), NULL, p->id);
		if (d.active) {
			if (orientation == 0) {
				t->shadow[plane][x][y] = 50;
				t->shadow[plane][x][y + 1] = 50;
			} else if (orientation == 1) {
				t->shadow[plane][x][y + 1] = 50;
				t->shadow[plane][x + 1][y + 1] = 50;
			} else if (orientation == 2) {
				t->shadow[plane][x + 1][y] = 50;
				t->shadow[plane][x + 1][y + 1] = 50;
			} else {
				t->shadow[plane][x][y] = 50;
				t->shadow[plane][x + 1][y] = 50;
			}
		}
		if (d.walloff != 16) adjust_decor(s, plane, x, y, d.walloff);
	} else if (shape == 1 || shape == 3) {
		set_wall(s, plane, x, y, average, ENTITY(orientation, shape), NULL, p->id);
		if (d.active) shadow_corner(t, plane, x, y, orientation);
	} else if (shape == 2) {
		int other = (orientation + 1) & 3;
		SceneEntity* primary = ENTITY(orientation + 4, 2);
		SceneEntity* secondary = ENTITY(other, 2);
		set_wall(s, plane, x, y, average, primary, secondary, p->id);
		if (d.walloff != 16) adjust_decor(s, plane, x, y, d.walloff);
	} else if (shape == 9) {
		add_scenery(s, plane, x, y, average, 1, 1, ENTITY(orientation, 9));
		if (d.walloff != 16) adjust_decor(s, plane, x, y, d.walloff);
	} else if (shape == 4) {
		set_decor(s, plane, x, y, average, ENTITY(orientation, 4), NULL, 0, 0, p->id);
	} else if (shape == 5) {
		int off = wall_walloff(s, plane, x, y, 16, 1);
		set_decor(s, plane, x, y, average, ENTITY(orientation, 4), NULL, off * DECO_X[orientation], DECO_Y[orientation] * off, p->id);
	} else if (shape == 6) {
		int off = wall_walloff(s, plane, x, y, 8, 2);
		set_decor(s, plane, x, y, average, ENTITY(orientation + 4, 4), NULL, off * OFFSET_X[orientation], off * OFFSET_Y[orientation], p->id);
	} else if (shape == 7) {
		int opposite = (orientation + 2) & 3;
		set_decor(s, plane, x, y, average, ENTITY(opposite + 4, 4), NULL, 0, 0, p->id);
	} else if (shape == 8) {
		int off = wall_walloff(s, plane, x, y, 8, 2), opposite = (orientation + 2) & 3;
		SceneEntity* primary = ENTITY(orientation + 4, 4);
		SceneEntity* secondary = ENTITY(opposite + 4, 4);
		set_decor(s, plane, x, y, average, primary, secondary, off * OFFSET_X[orientation], OFFSET_Y[orientation] * off, p->id);
	}
#undef ENTITY
}

// ---- merging normals between neighbours ----

static int tagsA[10000], tagsB[10000], mergeGeneration = 0;

static int can_merge(const SceneEntity* e) {
	return e && e->kind == ENTITY_RAW;
}

static void ensure_merged(SceneEntity* e) {
	if (e->merged) return;
	size_t n = (size_t)e->base->vertexCount + 1;
	e->merged = malloc(n * sizeof(VertexNormal));
	e->mergedSet = calloc(n, 1);
}

static void merge_normals(SceneEntity* a, SceneEntity* m, int offsetX, int offsetY, int offsetZ, int markTriangles) {
	const SceneBase* ab = a->base;
	const SceneBase* mb = m->base;
	Bounds b = bounds_of(m->vx, m->vy, m->vz, mb->vertexCount);
	mergeGeneration++;
	int count = 0;
	for (int i = 0; i < ab->vertexCount; i++) {
		const VertexNormal* normal = &ab->normals[i];
		if (normal->magnitude == 0) continue;
		int y = a->vy[i] - offsetY;
		if (y < b.minY || y > b.maxY) continue;
		int x = a->vx[i] - offsetX;
		if (x < b.minX || x > b.maxX) continue;
		int z = a->vz[i] - offsetZ;
		if (z < b.minZ || z > b.maxZ) continue;
		for (int j = 0; j < mb->vertexCount; j++) {
			const VertexNormal* other = &mb->normals[j];
			if (x != m->vx[j] || z != m->vz[j] || y != m->vy[j] || other->magnitude == 0) continue;
			ensure_merged(a);
			ensure_merged(m);
			if (!a->mergedSet[i]) {
				a->merged[i] = *normal;
				a->mergedSet[i] = 1;
			}
			if (!m->mergedSet[j]) {
				m->merged[j] = *other;
				m->mergedSet[j] = 1;
			}
			a->merged[i].x += other->x;
			a->merged[i].y += other->y;
			a->merged[i].z += other->z;
			a->merged[i].magnitude += other->magnitude;
			m->merged[j].x += normal->x;
			m->merged[j].y += normal->y;
			m->merged[j].z += normal->z;
			m->merged[j].magnitude += normal->magnitude;
			count++;
			tagsA[i] = mergeGeneration;
			tagsB[j] = mergeGeneration;
		}
	}
	if (count < 3 || !markTriangles) return;
	for (int i = 0; i < ab->triangleCount; i++) {
		if (tagsA[ab->ta[i]] == mergeGeneration && tagsA[ab->tb[i]] == mergeGeneration && tagsA[ab->tc[i]] == mergeGeneration) {
			if (!a->info) {
				a->info = calloc((size_t)ab->triangleCount + 1, 1);
				a->ownsInfo = 1;
			}
			a->info[i] = 2;
		}
	}
	for (int i = 0; i < mb->triangleCount; i++) {
		if (tagsB[mb->ta[i]] == mergeGeneration && tagsB[mb->tb[i]] == mergeGeneration && tagsB[mb->tc[i]] == mergeGeneration) {
			if (!m->info) {
				m->info = calloc((size_t)mb->triangleCount + 1, 1);
				m->ownsInfo = 1;
			}
			m->info[i] = 2;
		}
	}
}

static int tile_average(HeightMap h, int x, int y) {
	return (h[x][y] + h[x + 1][y] + h[x][y + 1] + h[x + 1][y + 1]) / 4;
}

static void merge_entity(Scene* s, SceneEntity* e, int level, int x, int y, int xSize, int ySize) {
	int sameLevel = 1, startX = x, endX = x + xSize, startY = y - 1, endY = y + ySize;
	for (int lvl = level; lvl <= level + 1; lvl++) {
		if (lvl == SCENE_LEVELS) continue;
		for (int tx = startX; tx <= endX; tx++) {
			if (tx < 0 || tx >= SCENE_SIZE) continue;
			for (int ty = startY; ty <= endY; ty++) {
				if (ty < 0 || ty >= SCENE_SIZE) continue;
				if (sameLevel && !(tx >= endX || ty >= endY || (ty < y && tx != x))) continue;
				SceneTile* t = &s->tiles[lvl][tx][ty];
				int delta = tile_average(s->terrain.heights[lvl], tx, ty) - tile_average(s->terrain.heights[level], x, y);
				if (t->wall) {
					int ox = (tx - x) * 128 + (1 - xSize) * 64, oz = (ty - y) * 128 + (1 - ySize) * 64;
					if (can_merge(t->wall->primary)) merge_normals(e, t->wall->primary, ox, delta, oz, sameLevel);
					if (can_merge(t->wall->secondary)) merge_normals(e, t->wall->secondary, ox, delta, oz, sameLevel);
				}
				for (int i = 0; i < t->sceneryCount; i++) {
					SceneScenery* c = t->scenery[i];
					if (can_merge(c->entity) && (tx == c->xMin || tx == startX) && (ty == c->yMin || ty == startY)) {
						int w = c->xMax + 1 - c->xMin, l = c->yMax + 1 - c->yMin;
						merge_normals(e, c->entity, (c->xMin - x) * 128 + (w - xSize) * 64, delta, (c->yMin - y) * 128 + (l - ySize) * 64, sameLevel);
					}
				}
			}
		}
		startX--;
		sameLevel = 0;
	}
}

static void merge_ground(Scene* s, SceneEntity* e, int level, int x, int y) {
	static const int DX[4] = {1, 0, 1, 1}, DY[4] = {0, 1, 1, -1};
	static const int OX[4] = {128, 0, 128, 128}, OZ[4] = {0, 128, 128, -128};
	for (int i = 0; i < 4; i++) {
		int nx = x + DX[i], ny = y + DY[i];
		if (nx < 0 || ny < 0 || nx >= SCENE_SIZE || ny >= SCENE_SIZE) continue;
		SceneGround* g = s->tiles[level][nx][ny].ground;
		if (g && can_merge(g->entity)) merge_normals(e, g->entity, OX[i], 0, OZ[i], 1);
	}
}

// RawModel.createModel
static void finish_entity(Scene* s, SceneEntity* e) {
	const SceneBase* b = e->base;
	size_t t = (size_t)b->triangleCount + 1;
	e->fa = malloc(t * 4);
	e->fb = malloc(t * 4);
	e->fc = malloc(t * 4);
	e->textures = b->textures ? malloc(t * 2) : NULL;
	e->ownsLit = 1;
	light_model(b, e->info, e->merged, e->mergedSet, s->types, e->fa, e->fb, e->fc, e->textures);
	e->kind = ENTITY_LIT;
}

void scene_merge(Scene* s) {
	for (int level = 0; level < SCENE_LEVELS; level++) {
		for (int x = 0; x < SCENE_SIZE; x++) {
			for (int y = 0; y < SCENE_SIZE; y++) {
				SceneTile* t = &s->tiles[level][x][y];
				SceneWall* w = t->wall;
				if (w && can_merge(w->primary)) {
					merge_entity(s, w->primary, level, x, y, 1, 1);
					if (can_merge(w->secondary)) {
						merge_entity(s, w->secondary, level, x, y, 1, 1);
						merge_normals(w->primary, w->secondary, 0, 0, 0, 0);
						finish_entity(s, w->secondary);
					}
					finish_entity(s, w->primary);
				}
				for (int i = 0; i < t->sceneryCount; i++) {
					SceneScenery* c = t->scenery[i];
					if (can_merge(c->entity)) {
						merge_entity(s, c->entity, level, x, y, c->xMax + 1 - c->xMin, c->yMax - c->yMin + 1);
						finish_entity(s, c->entity);
					}
				}
				SceneGround* g = t->ground;
				if (g && can_merge(g->entity)) {
					merge_ground(s, g->entity, level, x, y);
					finish_entity(s, g->entity);
				}
			}
		}
	}
}

// ---- lifetime ----

void scene_init(Scene* s, Js5Cache* cache, const TerrainTypes* types, int originX, int originY) {
	anim_init(cache);
	memset(s, 0, sizeof(*s));
	ensure_tables();
	terrain_scene_init(&s->terrain, originX, originY);
	s->cache = cache;
	s->types = types;
	js5_load_index(cache, 7);
	js5_load_index(cache, 16);
}

void scene_free(Scene* s) {
	for (int l = 0; l < SCENE_LEVELS; l++) {
		for (int x = 0; x < SCENE_SIZE; x++) {
			for (int y = 0; y < SCENE_SIZE; y++) {
				SceneTile* t = &s->tiles[l][x][y];
				if (t->wall) {
					free_entity(t->wall->primary);
					free_entity(t->wall->secondary);
					free(t->wall);
				}
				if (t->decor) {
					free_entity(t->decor->primary);
					free_entity(t->decor->secondary);
					free(t->decor);
				}
				if (t->ground) {
					free_entity(t->ground->entity);
					free(t->ground);
				}
				for (int i = 0; i < t->sceneryCount; i++) {
					SceneScenery* c = t->scenery[i];
					if (c->xMin == x && c->yMin == y) {
						free_entity(c->entity);
						free(c);
					}
				}
			}
		}
	}
	for (int i = 0; i < SCENE_BASE_BUCKETS; i++) {
		SceneBase* b = s->bases[i];
		while (b) {
			SceneBase* next = b->next;
			free(b->vx); free(b->vy); free(b->vz); free(b->ta); free(b->tb); free(b->tc);
			free(b->colors); free(b->textures); free(b->info); free(b->alpha); free(b->textureIndex); free(b->textureTypes);
			free(b->normals); free(b->triangleNormals);
			free(b->fa); free(b->fb); free(b->fc); free(b->litTextures); free(b->vertexBones);
			free(b);
			b = next;
		}
		s->bases[i] = NULL;
	}
	for (int i = 0; i < SCENE_MODEL_BUCKETS; i++) {
		SceneModel* m = s->models[i];
		while (m) {
			SceneModel* next = m->next;
			model_free(&m->model);
			free(m);
			m = next;
		}
		s->models[i] = NULL;
	}
	terrain_scene_free(&s->terrain);
	memset(s->tiles, 0, sizeof(s->tiles));
}

// ---- NPCs ----

static NpcDef** npcDefs = NULL;
static Js5Buffer* npcGroups = NULL;
static uint32_t npcGroupCount = 0;

const NpcDef* scene_get_npc(Scene* s, int id) {
	if (!npcDefs) {
		if (js5_load_index(s->cache, 18) != 0) return NULL;
		npcGroupCount = s->cache->index[18].capacity;
		npcDefs = calloc((size_t)npcGroupCount * 128, sizeof(NpcDef*));
		npcGroups = calloc(npcGroupCount, sizeof(Js5Buffer));
		if (!npcDefs || !npcGroups) return NULL;
	}
	uint32_t group = (uint32_t)id >> 7;
	if (id < 0 || group >= npcGroupCount) return NULL;
	if (npcDefs[id]) return npcDefs[id];
	if (!npcGroups[group].data && js5_read_group(s->cache, 18, group, NULL, &npcGroups[group]) != 0) {
		npcGroups[group].data = NULL;
		return NULL;
	}
	NpcDef* d = malloc(sizeof(NpcDef));
	if (!d) return NULL;
	Js5Buffer file = {0, 0};
	if (js5_group_file(&s->cache->index[18], group, &npcGroups[group], (uint32_t)(id & 127), &file) == 0) {
		npc_decode(d, id, file.data, file.size);
		free(file.data);
	} else {
		npc_decode(d, id, NULL, 0);
	}
	npcDefs[id] = d;
	return d;
}

const NpcDef* scene_shown_npc(Scene* s, const NpcDef* d) {
	if (!d || !d->multiNpcs) return d;
	int id = d->multiCount > 1 && d->multiNpcs[0] != -1 ? d->multiNpcs[0] : d->multiNpcs[d->multiCount - 1];
	return id == -1 ? NULL : scene_get_npc(s, id);
}

static void rotate_xyz(int32_t* vx, int32_t* vy, int32_t* vz, int count, int rotX, int rotY, int rotZ) {
	if (rotZ) {
		int s = sinTable[rotZ & 2047], c = cosTable[rotZ & 2047];
		for (int v = 0; v < count; v++) {
			int t = (vy[v] * s + vx[v] * c) >> 16;
			vy[v] = (vy[v] * c - vx[v] * s) >> 16;
			vx[v] = t;
		}
	}
	if (rotX) {
		int s = sinTable[rotX & 2047], c = cosTable[rotX & 2047];
		for (int v = 0; v < count; v++) {
			int t = (vy[v] * c - vz[v] * s) >> 16;
			vz[v] = (vy[v] * s + vz[v] * c) >> 16;
			vy[v] = t;
		}
	}
	if (rotY) {
		int s = sinTable[rotY & 2047], c = cosTable[rotY & 2047];
		for (int v = 0; v < count; v++) {
			int t = (vz[v] * s + vx[v] * c) >> 16;
			vz[v] = (vz[v] * c - vx[v] * s) >> 16;
			vx[v] = t;
		}
	}
}

void scene_free_base(SceneBase* b) {
	if (!b) return;
	free(b->vx); free(b->vy); free(b->vz); free(b->ta); free(b->tb); free(b->tc);
	free(b->colors); free(b->textures); free(b->info); free(b->alpha); free(b->textureIndex); free(b->textureTypes);
	free(b->normals); free(b->triangleNormals);
	free(b->fa); free(b->fb); free(b->fc); free(b->litTextures); free(b->vertexBones);
	free(b);
}

// RawModel.createModel with the light characters use.
static SceneBase* finish_model(Scene* s, Work* w, int ambient, int contrast, int lightX, int lightY, int lightZ) {
	SceneBase* b = calloc(1, sizeof(SceneBase));
	if (!b) return NULL;
	b->vertexCount = w->vertexCount;
	b->triangleCount = w->triangleCount;
	b->texturedCount = w->texturedCount;
	b->vx = w->vx; b->vy = w->vy; b->vz = w->vz;
	b->ta = w->ta; b->tb = w->tb; b->tc = w->tc;
	b->colors = w->colors;
	b->textures = w->textures;
	b->info = w->info;
	b->alpha = w->alpha;
	b->textureIndex = w->textureIndex;
	b->textureTypes = w->textureTypes;
	b->vertexBones = w->vertexBones;
	free(w->faceP);
	free(w->faceM);
	free(w->faceN);
	b->ambient = (int16_t)ambient;
	b->contrast = (int16_t)contrast;
	calculate_normals(b);
	size_t t = (size_t)b->triangleCount + 1;
	b->fa = malloc(t * 4);
	b->fb = malloc(t * 4);
	b->fc = malloc(t * 4);
	if (b->textures) b->litTextures = malloc(t * 2);
	LIGHT_X = lightX;
	LIGHT_Y = lightY;
	LIGHT_Z = lightZ;
	light_model(b, b->info, NULL, NULL, s->types, b->fa, b->fb, b->fc, b->litTextures);
	LIGHT_X = -50;
	LIGHT_Y = -10;
	LIGHT_Z = -50;
	return b;
}

SceneBase* scene_build_npc(Scene* s, const NpcDef* d) {
	const RawModel* parts[16];
	RawModel copies[16];
	const BasDef* basDef = anim_get_bas(d->basId);
	unsigned basUsed = basDef->transformsUsed;
	int count = 0;
	for (int i = 0; i < d->modelCount && count < 16; i++) {
		if (d->models[i] == -1) continue;
		const RawModel* m = get_model(s, d->models[i]);
		if (!m) continue;
		int hasOffset = d->hasOffset && d->hasOffset[i], hasBas = i < 12 && (basUsed >> i & 1);
		if (hasOffset || hasBas) {
			copies[count] = *m;
			copies[count].vx = dup_mem(m->vx, (size_t)m->vertexCount * 4);
			copies[count].vy = dup_mem(m->vy, (size_t)m->vertexCount * 4);
			copies[count].vz = dup_mem(m->vz, (size_t)m->vertexCount * 4);
			if (hasOffset) translate(copies[count].vx, copies[count].vy, copies[count].vz, m->vertexCount, d->offsets[i * 3], d->offsets[i * 3 + 1], d->offsets[i * 3 + 2]);
			if (hasBas) {
				const int16_t* t = basDef->transforms[i];
				rotate_xyz(copies[count].vx, copies[count].vy, copies[count].vz, m->vertexCount, t[3], t[4], t[5]);
				translate(copies[count].vx, copies[count].vy, copies[count].vz, m->vertexCount, t[0], t[1], t[2]);
			}
			parts[count] = &copies[count];
		} else {
			copies[count].vx = NULL;
			parts[count] = m;
		}
		count++;
	}
	if (count == 0) return NULL;
	Work w;
	trackBones = 1;
	if (d->modelCount == 1) work_from_model(&w, parts[0]);
	else work_merge(&w, parts, count);
	trackBones = 0;
	for (int i = 0; i < count; i++) {
		if (copies[i].vx) {
			free(copies[i].vx);
			free(copies[i].vy);
			free(copies[i].vz);
		}
	}
	for (int j = 0; j < d->recolCount; j++) {
		int16_t dest = !d->recolPalette || j >= d->recolPaletteCount ? d->recolD[j] : 0;
		for (int i = 0; i < w.triangleCount; i++) if (w.colors[i] == d->recolS[j]) w.colors[i] = dest;
	}
	if (w.textures) {
		for (int j = 0; j < d->retexCount; j++)
			for (int i = 0; i < w.triangleCount; i++) if (w.textures[i] == d->retexS[j]) w.textures[i] = d->retexD[j];
	}
	return finish_model(s, &w, d->ambient + 64, d->contrast + 850, -30, -50, -30);
}

// ---- players ----

static const IdkDef* get_idk(Scene* s, int id) {
	static Js5Buffer group = {0, 0};
	static IdkDef* table[4096];
	if (id < 0 || id >= 4096) return NULL;
	if (table[id]) return table[id];
	if (!group.data && (js5_load_index(s->cache, 2) != 0 || js5_read_group(s->cache, 2, 3, NULL, &group) != 0)) {
		group.data = NULL;
		return NULL;
	}
	Js5Buffer file = {0, 0};
	if (js5_group_file(&s->cache->index[2], 3, &group, (uint32_t)id, &file) != 0) return NULL;
	IdkDef* d = malloc(sizeof(IdkDef));
	if (d) idk_decode(d, file.data, file.size);
	free(file.data);
	table[id] = d;
	return d;
}

static int read_obj(Scene* s, int id, ObjDef* d) {
	Js5Buffer group = {0, 0}, file = {0, 0};
	uint32_t g = (uint32_t)id >> 8;
	if (js5_load_index(s->cache, 19) != 0 || js5_read_group(s->cache, 19, g, NULL, &group) != 0) return -1;
	int rc = js5_group_file(&s->cache->index[19], g, &group, (uint32_t)(id & 255), &file);
	free(group.data);
	if (rc != 0) return -1;
	obj_decode(d, id, file.data, file.size);
	free(file.data);
	return 0;
}

#define OBJ_BUCKETS 256
typedef struct ObjNode {
	ObjDef def;
	struct ObjNode* next;
} ObjNode;

static const ObjDef* get_obj(Scene* s, int id) {
	static ObjNode* buckets[OBJ_BUCKETS];
	for (ObjNode* n = buckets[id & (OBJ_BUCKETS - 1)]; n; n = n->next) if (n->def.id == id) return &n->def;
	ObjNode* n = malloc(sizeof(ObjNode));
	if (!n) return NULL;
	if (read_obj(s, id, &n->def) != 0) {
		free(n);
		return NULL;
	}
	// A lent item is worn like the item it copies.
	if (n->def.lentTemplate != -1) {
		const ObjDef* link = get_obj(s, n->def.lentLink);
		if (link) {
			ObjDef* d = &n->def;
			d->manwear = link->manwear;
			d->manwear2 = link->manwear2;
			d->manwear3 = link->manwear3;
			d->womanwear = link->womanwear;
			d->womanwear2 = link->womanwear2;
			d->womanwear3 = link->womanwear3;
			memcpy(d->manOffset, link->manOffset, 3);
			memcpy(d->womanOffset, link->womanOffset, 3);
			d->recolCount = link->recolCount;
			d->retexCount = link->retexCount;
			d->recolS = dup_mem(link->recolS, 2 * (size_t)(link->recolCount + 1));
			d->recolD = dup_mem(link->recolD, 2 * (size_t)(link->recolCount + 1));
			d->retexS = dup_mem(link->retexS, 2 * (size_t)(link->retexCount + 1));
			d->retexD = dup_mem(link->retexD, 2 * (size_t)(link->retexCount + 1));
		}
	}
	// A bank note looks like the template paper and is named after the item it stands for.
	if (n->def.certTemplate != -1) {
		const ObjDef* link = get_obj(s, n->def.certLink);
		const ObjDef* paper = get_obj(s, n->def.certTemplate);
		ObjDef* d = &n->def;
		if (link && link->name) {
			free(d->name);
			d->name = dup_mem(link->name, strlen(link->name) + 1);
		}
		if (link) d->cost = link->cost;
		if (paper) {
			d->model = paper->model;
			d->zoom2d = paper->zoom2d;
			d->xan2d = paper->xan2d;
			d->yan2d = paper->yan2d;
			d->zan2d = paper->zan2d;
			d->xof2d = paper->xof2d;
			d->yof2d = paper->yof2d;
			free(d->recolS);
			free(d->recolD);
			free(d->retexS);
			free(d->retexD);
			d->recolCount = paper->recolCount;
			d->retexCount = paper->retexCount;
			d->recolS = dup_mem(paper->recolS, 2 * (size_t)(paper->recolCount + 1));
			d->recolD = dup_mem(paper->recolD, 2 * (size_t)(paper->recolCount + 1));
			d->retexS = dup_mem(paper->retexS, 2 * (size_t)(paper->retexCount + 1));
			d->retexD = dup_mem(paper->retexD, 2 * (size_t)(paper->retexCount + 1));
		}
		d->stackable = 1;
	}
	n->next = buckets[id & (OBJ_BUCKETS - 1)];
	buckets[id & (OBJ_BUCKETS - 1)] = n;
	return &n->def;
}

const ObjDef* scene_get_obj(Scene* s, int id) {
	return get_obj(s, id);
}

SceneBase* scene_build_item(Scene* s, const ObjDef* d) {
	const RawModel* m = get_model(s, d->model);
	if (!m) return NULL;
	Work w;
	work_from_model(&w, m);
	for (int j = 0; j < d->recolCount; j++)
		for (int i = 0; i < w.triangleCount; i++) if (w.colors[i] == d->recolS[j]) w.colors[i] = d->recolD[j];
	if (w.textures) {
		for (int j = 0; j < d->retexCount; j++)
			for (int i = 0; i < w.triangleCount; i++) if (w.textures[i] == d->retexS[j]) w.textures[i] = d->retexD[j];
	}
	SceneBase* b = finish_model(s, &w, d->ambient + 64, d->contrast + 768, -50, -10, -50);
	if (b && (d->resizeX != 128 || d->resizeY != 128 || d->resizeZ != 128)) {
		for (int v = 0; v < b->vertexCount; v++) {
			b->vx[v] = b->vx[v] * d->resizeX / 128;
			b->vy[v] = b->vy[v] * d->resizeY / 128;
			b->vz[v] = b->vz[v] * d->resizeZ / 128;
		}
	}
	return b;
}

int scene_wear_table(Scene* s, int32_t** out) {
	if (js5_load_index(s->cache, 19) != 0) return -1;
	const Js5Index* index = &s->cache->index[19];
	int capacity = (int)index->capacity * 256;
	uint8_t* wears = calloc((size_t)capacity + 1, 1);
	int32_t* lent = malloc(sizeof(int32_t) * ((size_t)capacity + 1));
	int32_t* list = malloc(sizeof(int32_t) * ((size_t)capacity + 1));
	if (!wears || !lent || !list) {
		free(wears);
		free(lent);
		free(list);
		return -1;
	}
	memset(lent, 0xFF, sizeof(int32_t) * ((size_t)capacity + 1));
	for (uint32_t g = 0; g < index->capacity; g++) {
		if (!index->groups[g].present) continue;
		Js5Buffer group = {0, 0};
		if (js5_read_group(s->cache, 19, g, NULL, &group) != 0) continue;
		const Js5Group* info = &index->groups[g];
		for (uint32_t i = 0; i < info->size; i++) {
			uint32_t fileId = info->fileIds ? info->fileIds[i] : i;
			Js5Buffer file = {0, 0};
			if (js5_group_file(index, g, &group, fileId, &file) != 0) continue;
			ObjDef d;
			int id = (int)(g * 256 + fileId);
			obj_decode(&d, id, file.data, file.size);
			free(file.data);
			if (id < capacity) {
				wears[id] = d.manwear >= 0 || d.womanwear >= 0;
				if (d.lentTemplate != -1) lent[id] = d.lentLink;
			}
			obj_free(&d);
		}
		free(group.data);
	}
	int count = 0;
	for (int i = 0; i < capacity; i++) {
		int wearable = wears[i];
		if (lent[i] != -1) wearable = lent[i] < capacity && wears[lent[i]];
		if (wearable) list[count++] = i;
	}
	free(wears);
	free(lent);
	*out = list;
	return count;
}

// One body slot, which may have been merged from several models.
typedef struct {
	RawModel model;
	uint8_t used, merged;
} PlayerSlot;

static void slot_load(Scene* s, PlayerSlot* slot, const int32_t* ids, int count) {
	const RawModel* list[4];
	int found = 0;
	for (int i = 0; i < count && i < 4; i++) {
		const RawModel* m = get_model(s, ids[i]);
		if (m) list[found++] = m;
	}
	if (found == 0) return;
	slot->used = 1;
	if (found == 1 && count == 1) {
		slot->model = *list[0];
		slot->model.vx = dup_mem(list[0]->vx, (size_t)list[0]->vertexCount * 4);
		slot->model.vy = dup_mem(list[0]->vy, (size_t)list[0]->vertexCount * 4);
		slot->model.vz = dup_mem(list[0]->vz, (size_t)list[0]->vertexCount * 4);
		slot->model.colors = dup_mem(list[0]->colors, (size_t)list[0]->triangleCount * 2);
		if (list[0]->textures) slot->model.textures = dup_mem(list[0]->textures, (size_t)list[0]->triangleCount * 2);
		return;
	}
	Work w;
	trackBones = 1;
	work_merge(&w, list, found);
	trackBones = 0;
	RawModel* r = &slot->model;
	memset(r, 0, sizeof(*r));
	slot->merged = 1;
	r->vertexCount = w.vertexCount;
	r->triangleCount = w.triangleCount;
	r->texturedCount = w.texturedCount;
	r->vx = w.vx; r->vy = w.vy; r->vz = w.vz;
	r->ta = w.ta; r->tb = w.tb; r->tc = w.tc;
	r->colors = w.colors;
	r->info = w.info;
	r->alpha = w.alpha;
	r->textures = w.textures;
	r->textureIndex = w.textureIndex;
	r->textureTypes = w.textureTypes;
	r->faceP = w.faceP; r->faceM = w.faceM; r->faceN = w.faceN;
	r->vertexBones = w.vertexBones;
}

static void slot_free(PlayerSlot* slot) {
	if (!slot->used) return;
	RawModel* r = &slot->model;
	free(r->vx); free(r->vy); free(r->vz); free(r->colors);
	if (r->textures) free(r->textures);
	if (!slot->merged) return;
	free(r->ta); free(r->tb); free(r->tc); free(r->info); free(r->alpha); free(r->textureIndex);
	free(r->textureTypes); free(r->faceP); free(r->faceM); free(r->faceN); free(r->vertexBones);
}

static void slot_swap(RawModel* m, int colours, const int16_t* from, const int16_t* to, int count) {
	int16_t* list = colours ? m->colors : m->textures;
	if (!list) return;
	for (int j = 0; j < count; j++)
		for (int i = 0; i < m->triangleCount; i++) if (list[i] == from[j]) list[i] = to[j];
}

SceneBase* scene_build_raw(Scene* s, int model) {
	const RawModel* m = get_model(s, model);
	if (!m) return NULL;
	Work w;
	work_from_model(&w, m);
	return finish_model(s, &w, 64, 768, -50, -10, -50);
}

SceneBase* scene_build_npc_head(Scene* s, const NpcDef* d) {
	const RawModel* parts[16];
	int count = 0;
	for (int i = 0; i < d->headModelCount && count < 16; i++) {
		const RawModel* m = get_model(s, d->headModels[i]);
		if (m) parts[count++] = m;
	}
	if (count == 0) return NULL;
	Work w;
	trackBones = 1;
	if (count == 1) work_from_model(&w, parts[0]);
	else work_merge(&w, parts, count);
	trackBones = 0;
	for (int j = 0; j < d->recolCount; j++) {
		int16_t dest = !d->recolPalette || j >= d->recolPaletteCount ? d->recolD[j] : 0;
		for (int i = 0; i < w.triangleCount; i++) if (w.colors[i] == d->recolS[j]) w.colors[i] = dest;
	}
	if (w.textures) {
		for (int j = 0; j < d->retexCount; j++)
			for (int i = 0; i < w.triangleCount; i++) if (w.textures[i] == d->retexS[j]) w.textures[i] = d->retexD[j];
	}
	return finish_model(s, &w, 64, 768, -50, -10, -50);
}

SceneBase* scene_build_player_head(Scene* s, const PlayerLook* look) {
	PlayerSlot slots[12];
	memset(slots, 0, sizeof(slots));
	for (int i = 0; i < 12; i++) {
		int32_t value = look->identikit[i];
		if (value & 0x40000000) {
			const ObjDef* d = get_obj(s, value & 0x3FFFFFFF);
			if (!d) continue;
			int32_t ids[2] = {look->female ? d->womanhead : d->manhead, look->female ? d->womanhead2 : d->manhead2};
			if (ids[0] == -1) continue;
			slot_load(s, &slots[i], ids, ids[1] == -1 ? 1 : 2);
			if (!slots[i].used) continue;
			slot_swap(&slots[i].model, 1, d->recolS, d->recolD, d->recolCount);
			slot_swap(&slots[i].model, 0, d->retexS, d->retexD, d->retexCount);
		} else if (value & (int32_t)0x80000000) {
			const IdkDef* d = get_idk(s, value & 0x3FFFFFFF);
			if (!d) continue;
			int32_t ids[5];
			int found = 0;
			for (int k = 0; k < 5; k++) if (d->heads[k] != -1) ids[found++] = d->heads[k];
			if (found == 0) continue;
			slot_load(s, &slots[i], ids, found);
			if (!slots[i].used) continue;
			slot_swap(&slots[i].model, 1, d->recolS, d->recolD, d->recolCount);
			slot_swap(&slots[i].model, 0, d->retexS, d->retexD, d->retexCount);
		}
	}
	const RawModel* parts[12];
	int count = 0;
	for (int i = 0; i < 12; i++) if (slots[i].used) parts[count++] = &slots[i].model;
	if (count == 0) return NULL;
	Work w;
	trackBones = 1;
	work_merge(&w, parts, count);
	trackBones = 0;
	for (int i = 0; i < 12; i++) slot_free(&slots[i]);
	for (int i = 0; i < 5; i++) {
		int c = look->colours[i] < PLAYER_DEST_BODY_COUNT[i] ? look->colours[i] : 0;
		if (c < PLAYER_DEST_BODY_COUNT[i]) for (int t = 0; t < w.triangleCount; t++) if (w.colors[t] == PLAYER_SOURCE_BODY[i]) w.colors[t] = PLAYER_DEST_BODY[i][c];
		if (c < PLAYER_DEST_SKIN_COUNT[i]) for (int t = 0; t < w.triangleCount; t++) if (w.colors[t] == PLAYER_SOURCE_SKIN[i]) w.colors[t] = PLAYER_DEST_SKIN[i][c];
	}
	return finish_model(s, &w, 64, 768, -50, -10, -50);
}

SceneBase* scene_build_player(Scene* s, const PlayerLook* look) {
	PlayerSlot slots[12];
	memset(slots, 0, sizeof(slots));
	for (int i = 0; i < 12; i++) {
		int32_t value = look->identikit[i];
		if (value & 0x40000000) {
			const ObjDef* d = get_obj(s, value & 0x3FFFFFFF);
			if (!d) continue;
			int32_t ids[3] = {look->female ? d->womanwear : d->manwear, look->female ? d->womanwear2 : d->manwear2, look->female ? d->womanwear3 : d->manwear3};
			if (ids[0] == -1) continue;
			slot_load(s, &slots[i], ids, ids[1] == -1 ? 1 : ids[2] == -1 ? 2 : 3);
			if (!slots[i].used) continue;
			const int8_t* o = look->female ? d->womanOffset : d->manOffset;
			if (o[0] || o[1] || o[2]) translate(slots[i].model.vx, slots[i].model.vy, slots[i].model.vz, slots[i].model.vertexCount, o[0], o[1], o[2]);
			slot_swap(&slots[i].model, 1, d->recolS, d->recolD, d->recolCount);
			slot_swap(&slots[i].model, 0, d->retexS, d->retexD, d->retexCount);
		} else if (value & (int32_t)0x80000000) {
			const IdkDef* d = get_idk(s, value & 0x3FFFFFFF);
			if (!d || !d->models) continue;
			slot_load(s, &slots[i], d->models, d->modelCount);
			if (!slots[i].used) continue;
			slot_swap(&slots[i].model, 1, d->recolS, d->recolD, d->recolCount);
			slot_swap(&slots[i].model, 0, d->retexS, d->retexD, d->retexCount);
		}
	}
	const BasDef* bas = anim_get_bas(look->basId);
	const RawModel* parts[12];
	int count = 0;
	for (int i = 0; i < 12; i++) {
		if (!slots[i].used) continue;
		RawModel* m = &slots[i].model;
		if (bas->transformsUsed >> i & 1) {
			const int16_t* t = bas->transforms[i];
			rotate_xyz(m->vx, m->vy, m->vz, m->vertexCount, t[3], t[4], t[5]);
			translate(m->vx, m->vy, m->vz, m->vertexCount, t[0], t[1], t[2]);
		}
		parts[count++] = m;
	}
	if (count == 0) return NULL;
	Work w;
	trackBones = 1;
	work_merge(&w, parts, count);
	trackBones = 0;
	for (int i = 0; i < 12; i++) slot_free(&slots[i]);
	for (int i = 0; i < 5; i++) {
		int c = look->colours[i] < PLAYER_DEST_BODY_COUNT[i] ? look->colours[i] : 0;
		if (c < PLAYER_DEST_BODY_COUNT[i]) for (int t = 0; t < w.triangleCount; t++) if (w.colors[t] == PLAYER_SOURCE_BODY[i]) w.colors[t] = PLAYER_DEST_BODY[i][c];
		if (c < PLAYER_DEST_SKIN_COUNT[i]) for (int t = 0; t < w.triangleCount; t++) if (w.colors[t] == PLAYER_SOURCE_SKIN[i]) w.colors[t] = PLAYER_DEST_SKIN[i][c];
	}
	return finish_model(s, &w, 64, 850, -30, -50, -30);
}

void scene_resize_npc(int32_t* vx, int32_t* vy, int32_t* vz, int count, const NpcDef* d) {
	if (d->resizeX == 128 && d->resizeY == 128) return;
	for (int v = 0; v < count; v++) {
		vx[v] = vx[v] * d->resizeX / 128;
		vy[v] = vy[v] * d->resizeY / 128;
		vz[v] = vz[v] * d->resizeX / 128;
	}
}

// ---- dynamic objects ----

static void resolve_entity(Scene* s, SceneEntity* e) {
	if (!e || e->kind != ENTITY_DYNAMIC) return;
	const LocDef* def = get_loc(s, e->locId);
	if (!def) return;
	if (def->multiLocs) {
		// Without server state every variable reads 0.
		int next = def->multiLocs[0] != -1 ? def->multiLocs[0] : def->multiLocs[def->multiLocCount - 1];
		if (next == -1 || !(def = get_loc(s, next))) return;
	}
	LocDef d = *def;
	int quadrant = e->rotation & 3;
	int width = quadrant == 1 || quadrant == 3 ? d.length : d.width;
	int length = quadrant == 1 || quadrant == 3 ? d.width : d.length;
	int x1 = e->tileX + (width >> 1), x2 = e->tileX + ((width + 1) >> 1), y1 = e->tileY + (length >> 1), y2 = e->tileY + ((length + 1) >> 1);
	x1 = x1 > 104 ? 104 : x1;
	x2 = x2 > 104 ? 104 : x2;
	y1 = y1 > 104 ? 104 : y1;
	y2 = y2 > 104 ? 104 : y2;
	HeightMap h = s->terrain.heights[e->level];
	HeightMap above = e->level < 3 ? s->terrain.heights[e->level + 1] : NULL;
	int average = (h[x2][y2] + h[x1][y2] + h[x1][y1] + h[x2][y1]) >> 2;
	int xFine = (width << 6) + (e->tileX << 7), yFine = (length << 6) + (e->tileY << 7);
	d.computeVertexColors = 0;
	SceneEntity* made = make_static(s, &d, e->rotation, e->shape, h, above, average, xFine, yFine);
	if (made) {
		int locId = e->locId, shape = e->shape, rotation = e->rotation, level = e->level, tileX = e->tileX, tileY = e->tileY;
		*e = *made;
		e->locId = locId;
		e->shape = shape;
		e->rotation = rotation;
		e->level = level;
		e->tileX = tileX;
		e->tileY = tileY;
		free(made);
	}
}

void scene_resolve_dynamic(Scene* s) {
	for (int l = 0; l < SCENE_LEVELS; l++)
		for (int x = 0; x < SCENE_SIZE; x++)
			for (int y = 0; y < SCENE_SIZE; y++) {
				SceneTile* t = &s->tiles[l][x][y];
				if (t->wall) {
					resolve_entity(s, t->wall->primary);
					resolve_entity(s, t->wall->secondary);
				}
				if (t->decor) {
					resolve_entity(s, t->decor->primary);
					resolve_entity(s, t->decor->secondary);
				}
				if (t->ground) resolve_entity(s, t->ground->entity);
				for (int i = 0; i < t->sceneryCount; i++) {
					SceneScenery* c = t->scenery[i];
					if (c->xMin == x && c->yMin == y) resolve_entity(s, c->entity);
				}
			}
}
