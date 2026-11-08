#include "model.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char* copy_text(const char* s) {
	size_t n = strlen(s) + 1;
	char* c = malloc(n);
	if (c) memcpy(c, s, n);
	return c;
}

typedef struct {
	const uint8_t* data;
	size_t size, pos;
} Buf;

static int g1(Buf* b) { return b->pos < b->size ? b->data[b->pos++] : (b->pos++, 0); }
static int g1b(Buf* b) { return (int8_t)g1(b); }
static int g2(Buf* b) { int a = g1(b); return a << 8 | g1(b); }
static int g2b(Buf* b) { return (int16_t)g2(b); }
static int g3(Buf* b) { int a = g2(b); return a << 8 | g1(b); }
static int peek(const Buf* b) { return b->pos < b->size ? b->data[b->pos] : 0; }
static int gsmart(Buf* b) { return peek(b) < 128 ? g1(b) - 64 : g2(b) - 0xC000; }
static int gsmarts(Buf* b) { return peek(b) >= 128 ? g2(b) - 0x8000 : g1(b); }

static int gvarsmart(Buf* b) {
	int value = gsmarts(b), extra = 0;
	while (value == 32767) {
		value = gsmarts(b);
		extra += 32767;
	}
	return extra + value;
}

static int alloc_core(RawModel* m, int vertices, int triangles, int textured) {
	m->vertexCount = vertices;
	m->triangleCount = triangles;
	m->texturedCount = textured;
	m->vx = calloc((size_t)vertices + 1, 4);
	m->vy = calloc((size_t)vertices + 1, 4);
	m->vz = calloc((size_t)vertices + 1, 4);
	m->ta = calloc((size_t)triangles + 1, 4);
	m->tb = calloc((size_t)triangles + 1, 4);
	m->tc = calloc((size_t)triangles + 1, 4);
	m->colors = calloc((size_t)triangles + 1, 2);
	return m->vx && m->vy && m->vz && m->ta && m->tb && m->tc && m->colors ? 0 : -1;
}

static void read_vertices(RawModel* m, Buf* flags, Buf* dx, Buf* dy, Buf* dz, Buf* bones) {
	int px = 0, py = 0, pz = 0;
	for (int v = 0; v < m->vertexCount; v++) {
		int f = g1(flags);
		px += f & 1 ? gsmart(dx) : 0;
		py += f & 2 ? gsmart(dy) : 0;
		pz += f & 4 ? gsmart(dz) : 0;
		m->vx[v] = px;
		m->vy[v] = py;
		m->vz[v] = pz;
		if (m->vertexBones) m->vertexBones[v] = g1(bones);
	}
}

static void read_faces(RawModel* m, Buf* indices, Buf* types) {
	int a = 0, b = 0, c = 0, last = 0;
	for (int t = 0; t < m->triangleCount; t++) {
		int type = g1(types);
		if (type == 1) {
			a = gsmart(indices) + last;
			b = gsmart(indices) + a;
			c = gsmart(indices) + b;
			last = c;
		} else if (type == 2) {
			b = c;
			c = gsmart(indices) + last;
			last = c;
		} else if (type == 3) {
			a = c;
			c = gsmart(indices) + last;
			last = c;
		} else if (type == 4) {
			int swap = a;
			a = b;
			b = swap;
			c = gsmart(indices) + last;
			last = c;
		} else {
			continue;
		}
		m->ta[t] = a;
		m->tb[t] = b;
		m->tc[t] = c;
	}
}

static int decode_new(RawModel* m, const uint8_t* src, size_t size) {
	Buf b1 = {src, size, size - 23};
	int vertices = g2(&b1), triangles = g2(&b1), textured = g1(&b1), hasInfoFlags = g1(&b1);
	int hasInfo = (hasInfoFlags & 1) == 1;
	int priority = g1(&b1), hasAlpha = g1(&b1), hasTriangleBones = g1(&b1), hasTextures = g1(&b1), hasVertexBones = g1(&b1);
	int dxLen = g2(&b1), dyLen = g2(&b1), dzLen = g2(&b1), indexLen = g2(&b1), textureIndexLen = g2(&b1);
	int simple = 0;
	if (alloc_core(m, vertices, triangles, textured) != 0) return -1;
	uint8_t* types = NULL;
	if (textured > 0) {
		types = malloc((size_t)textured);
		if (!types) return -1;
		b1.pos = 0;
		for (int i = 0; i < textured; i++) {
			int type = g1b(&b1);
			types[i] = (uint8_t)type;
			if (type == 0) simple++;
		}
	}
	size_t offset = (size_t)textured + vertices;
	size_t infoOff = offset;
	if (hasInfo) offset += triangles;
	size_t typeOff = offset;
	offset += triangles;
	size_t priorityOff = offset;
	if (priority == 255) offset += triangles;
	size_t triBoneOff = offset;
	if (hasTriangleBones == 1) offset += triangles;
	size_t vertBoneOff = offset;
	if (hasVertexBones == 1) offset += vertices;
	size_t alphaOff = offset;
	if (hasAlpha == 1) offset += triangles;
	size_t indexOff = offset;
	offset += indexLen;
	size_t textureOff = offset;
	if (hasTextures == 1) offset += (size_t)triangles * 2;
	size_t textureIndexOff = offset;
	offset += textureIndexLen;
	size_t colorOff = offset;
	offset += (size_t)triangles * 2;
	size_t dxOff = offset;
	offset += dxLen;
	size_t dyOff = offset;
	offset += dyLen;
	size_t dzOff = offset;
	offset += dzLen;
	size_t simplePmnOff = offset;
	offset += (size_t)simple * 6;
	size_t complexPmnOff = offset;

	if (hasVertexBones == 1) m->vertexBones = calloc((size_t)vertices + 1, 4);
	if (hasInfo) m->info = calloc((size_t)triangles + 1, 1);
	if (priority == 255) m->priorities = calloc((size_t)triangles + 1, 1);
	else m->priority = (int8_t)priority;
	if (hasAlpha == 1) m->alpha = calloc((size_t)triangles + 1, 1);
	if (hasTriangleBones == 1) m->triangleBones = calloc((size_t)triangles + 1, 4);
	if (hasTextures == 1) m->textures = calloc((size_t)triangles + 1, 2);
	if (hasTextures == 1 && textured > 0) m->textureIndex = calloc((size_t)triangles + 1, 1);
	if (textured > 0) {
		m->faceP = calloc((size_t)textured, 2);
		m->faceM = calloc((size_t)textured, 2);
		m->faceN = calloc((size_t)textured, 2);
	}

	Buf flags = {src, size, (size_t)textured}, dx = {src, size, dxOff}, dy = {src, size, dyOff}, dz = {src, size, dzOff}, bones = {src, size, vertBoneOff};
	read_vertices(m, &flags, &dx, &dy, &dz, &bones);

	Buf color = {src, size, colorOff}, info = {src, size, infoOff}, prio = {src, size, priorityOff}, alpha = {src, size, alphaOff};
	Buf triBones = {src, size, triBoneOff}, tex = {src, size, textureOff}, texIndex = {src, size, textureIndexOff};
	for (int t = 0; t < triangles; t++) {
		m->colors[t] = (int16_t)g2(&color);
		if (hasInfo) m->info[t] = (int8_t)g1b(&info);
		if (priority == 255) m->priorities[t] = (int8_t)g1b(&prio);
		if (hasAlpha == 1) m->alpha[t] = (int8_t)g1b(&alpha);
		if (hasTriangleBones == 1) m->triangleBones[t] = g1(&triBones);
		if (hasTextures == 1) m->textures[t] = (int16_t)(g2(&tex) - 1);
		if (m->textureIndex) m->textureIndex[t] = m->textures[t] == -1 ? -1 : (int8_t)(g1(&texIndex) - 1);
	}
	Buf indices = {src, size, indexOff}, faceTypes = {src, size, typeOff};
	read_faces(m, &indices, &faceTypes);

	Buf simplePmn = {src, size, simplePmnOff}, complexPmn = {src, size, complexPmnOff};
	for (int t = 0; t < textured; t++) {
		int type = types[t];
		Buf* pmn = type == 0 ? &simplePmn : &complexPmn;
		if (type <= 3) {
			m->faceP[t] = (int16_t)g2(pmn);
			m->faceM[t] = (int16_t)g2(pmn);
			m->faceN[t] = (int16_t)g2(pmn);
		}
	}
	m->textureTypes = types;
	return 0;
}

static int decode_old(RawModel* m, const uint8_t* src, size_t size) {
	Buf b1 = {src, size, size - 18};
	int vertices = g2(&b1), triangles = g2(&b1), textured = g1(&b1), hasInfo = g1(&b1);
	int hasPriorities = g1(&b1), hasAlpha = g1(&b1), hasTriangleBones = g1(&b1), hasVertexBones = g1(&b1);
	int dxLen = g2(&b1), dyLen = g2(&b1);
	g2(&b1); // dz length
	int indexLen = g2(&b1);
	size_t offset = (size_t)vertices;
	size_t typeOff = offset;
	offset += triangles;
	size_t priorityOff = offset;
	if (hasPriorities == 255) offset += triangles;
	size_t triBoneOff = offset;
	if (hasTriangleBones == 1) offset += triangles;
	size_t infoOff = offset;
	if (hasInfo == 1) offset += triangles;
	size_t vertBoneOff = offset;
	if (hasVertexBones == 1) offset += vertices;
	size_t alphaOff = offset;
	if (hasAlpha == 1) offset += triangles;
	size_t indexOff = offset;
	offset += indexLen;
	size_t colorOff = offset;
	offset += (size_t)triangles * 2;
	size_t pmnOff = offset;
	offset += (size_t)textured * 6;
	size_t dxOff = offset;
	offset += dxLen;
	size_t dyOff = offset;
	offset += dyLen;
	size_t dzOff = offset;

	if (alloc_core(m, vertices, triangles, textured) != 0) return -1;
	if (textured > 0) {
		m->faceP = calloc((size_t)textured, 2);
		m->faceM = calloc((size_t)textured, 2);
		m->faceN = calloc((size_t)textured, 2);
	}
	if (hasVertexBones == 1) m->vertexBones = calloc((size_t)vertices + 1, 4);
	if (hasInfo == 1) {
		m->info = calloc((size_t)triangles + 1, 1);
		m->textureIndex = calloc((size_t)triangles + 1, 1);
		m->textures = calloc((size_t)triangles + 1, 2);
	}
	if (hasPriorities == 255) m->priorities = calloc((size_t)triangles + 1, 1);
	else m->priority = (int8_t)hasPriorities;
	if (hasAlpha == 1) m->alpha = calloc((size_t)triangles + 1, 1);
	if (hasTriangleBones == 1) m->triangleBones = calloc((size_t)triangles + 1, 4);

	Buf flags = {src, size, 0}, dx = {src, size, dxOff}, dy = {src, size, dyOff}, dz = {src, size, dzOff}, bones = {src, size, vertBoneOff};
	read_vertices(m, &flags, &dx, &dy, &dz, &bones);

	int anyInfo = 0, anyTexture = 0;
	Buf color = {src, size, colorOff}, info = {src, size, infoOff}, prio = {src, size, priorityOff}, alpha = {src, size, alphaOff}, triBones = {src, size, triBoneOff};
	for (int t = 0; t < triangles; t++) {
		m->colors[t] = (int16_t)g2(&color);
		if (hasInfo == 1) {
			int f = g1(&info);
			if ((f & 1) == 1) {
				m->info[t] = 1;
				anyInfo = 1;
			} else {
				m->info[t] = 0;
			}
			if ((f & 2) == 2) {
				m->textureIndex[t] = (int8_t)(f >> 2);
				m->textures[t] = m->colors[t];
				m->colors[t] = 127;
				if (m->textures[t] != -1) anyTexture = 1;
			} else {
				m->textureIndex[t] = -1;
				m->textures[t] = -1;
			}
		}
		if (hasPriorities == 255) m->priorities[t] = (int8_t)g1b(&prio);
		if (hasAlpha == 1) m->alpha[t] = (int8_t)g1b(&alpha);
		if (hasTriangleBones == 1) m->triangleBones[t] = g1(&triBones);
	}
	Buf indices = {src, size, indexOff}, faceTypes = {src, size, typeOff};
	read_faces(m, &indices, &faceTypes);
	if (textured > 0) m->textureTypes = calloc((size_t)textured, 1);
	Buf pmn = {src, size, pmnOff};
	for (int t = 0; t < textured; t++) {
		m->faceP[t] = (int16_t)g2(&pmn);
		m->faceM[t] = (int16_t)g2(&pmn);
		m->faceN[t] = (int16_t)g2(&pmn);
	}
	if (m->textureIndex) {
		int anyFace = 0;
		for (int i = 0; i < triangles; i++) {
			int index = m->textureIndex[i] & 0xFF;
			if (index == 255) continue;
			if (index < textured && (m->faceP[index] & 0xFFFF) == m->ta[i] && (m->faceM[index] & 0xFFFF) == m->tb[i] && (m->faceN[index] & 0xFFFF) == m->tc[i]) m->textureIndex[i] = -1;
			else anyFace = 1;
		}
		if (!anyFace) {
			free(m->textureIndex);
			m->textureIndex = NULL;
		}
	}
	if (!anyTexture) {
		free(m->textures);
		m->textures = NULL;
	}
	if (!anyInfo) {
		free(m->info);
		m->info = NULL;
	}
	return 0;
}

int model_decode(RawModel* m, const uint8_t* src, size_t size) {
	memset(m, 0, sizeof(*m));
	if (size < 18) return -2;
	if (src[size - 1] == 0xFF && src[size - 2] == 0xFF) return size < 23 ? -2 : decode_new(m, src, size);
	return decode_old(m, src, size);
}

void model_free(RawModel* m) {
	free(m->vx); free(m->vy); free(m->vz);
	free(m->ta); free(m->tb); free(m->tc);
	free(m->colors); free(m->info); free(m->alpha); free(m->priorities);
	free(m->textures); free(m->textureIndex);
	free(m->faceP); free(m->faceM); free(m->faceN); free(m->textureTypes);
	free(m->vertexBones); free(m->triangleBones);
	memset(m, 0, sizeof(*m));
}

// ---- loc definitions ----

static void skip_string(Buf* b, char* out, size_t outSize) {
	size_t n = 0;
	for (;;) {
		int c = g1(b);
		if (c == 0 || b->pos > b->size) break;
		if (n + 1 < outSize) out[n++] = (char)c;
	}
	out[n] = 0;
}

static int equals_hidden(const char* s) {
	const char* h = "hidden";
	for (; *s && *h; s++, h++) if (tolower((unsigned char)*s) != *h) return 0;
	return *s == 0 && *h == 0;
}

void loc_decode(LocDef* d, int id, const uint8_t* src, size_t size) {
	memset(d, 0, sizeof(*d));
	d->id = id;
	d->width = d->length = 1;
	d->blockwalk = 2;
	d->blockrange = 1;
	d->interactable = -1;
	d->walloff = 16;
	d->anim = -1;
	d->hillskewAmount = -1;
	d->resizeX = d->resizeY = d->resizeZ = 128;
	d->multiLocVarbit = d->multiLocVarp = -1;
	d->supportitems = -1;
	d->mapscene = -1;
	d->active = 1;
	d->castshadow = 1;
	int hasOps = 0;
	Buf b = {src, src ? size : 0, 0};
	char text[256];
	while (src) {
		int op = g1(&b);
		if (op == 0 || b.pos > b.size) break;
		if (op == 1) {
			int count = g1(&b);
			if (count > 0) {
				if (!d->models) {
					d->modelCount = count;
					d->models = malloc(sizeof(int32_t) * (size_t)count);
					d->shapes = malloc(sizeof(int32_t) * (size_t)count);
					for (int i = 0; i < count; i++) {
						d->models[i] = g2(&b);
						d->shapes[i] = g1(&b);
					}
				} else {
					b.pos += (size_t)count * 3;
				}
			}
		} else if (op == 2) {
			skip_string(&b, text, sizeof(text));
			free(d->name);
			d->name = copy_text(text);
		} else if (op == 5) {
			int count = g1(&b);
			if (count > 0) {
				if (!d->models) {
					d->modelCount = count;
					d->models = malloc(sizeof(int32_t) * (size_t)count);
					free(d->shapes);
					d->shapes = NULL;
					for (int i = 0; i < count; i++) d->models[i] = g2(&b);
				} else {
					b.pos += (size_t)count * 2;
				}
			}
		} else if (op == 14) d->width = g1(&b);
		else if (op == 15) d->length = g1(&b);
		else if (op == 17) {
			d->blockwalk = 0;
			d->blockrange = 0;
		} else if (op == 18) d->blockrange = 0;
		else if (op == 19) d->interactable = g1(&b);
		else if (op == 21) d->hillskewType = 1;
		else if (op == 22) d->computeVertexColors = 1;
		else if (op == 23) d->occlude = 1;
		else if (op == 24) {
			d->anim = g2(&b);
			if (d->anim == 65535) d->anim = -1;
		} else if (op == 27) d->blockwalk = 1;
		else if (op == 28) d->walloff = g1(&b);
		else if (op == 29) d->ambient = g1b(&b);
		else if (op == 39) d->contrast = g1b(&b) * 5;
		else if (op >= 30 && op < 35) {
			skip_string(&b, text, sizeof(text));
			free(d->ops[op - 30]);
			d->ops[op - 30] = NULL;
			if (!equals_hidden(text)) {
				hasOps |= 1 << (op - 30);
				d->ops[op - 30] = copy_text(text);
			} else {
				hasOps &= ~(1 << (op - 30));
			}
		} else if (op == 40) {
			int count = g1(&b);
			free(d->recolS);
			free(d->recolD);
			d->recolCount = count;
			d->recolS = malloc(2 * (size_t)(count + 1));
			d->recolD = malloc(2 * (size_t)(count + 1));
			for (int i = 0; i < count; i++) {
				d->recolS[i] = (int16_t)g2(&b);
				d->recolD[i] = (int16_t)g2(&b);
			}
		} else if (op == 41) {
			int count = g1(&b);
			free(d->retexS);
			free(d->retexD);
			d->retexCount = count;
			d->retexS = malloc(2 * (size_t)(count + 1));
			d->retexD = malloc(2 * (size_t)(count + 1));
			for (int i = 0; i < count; i++) {
				d->retexS[i] = (int16_t)g2(&b);
				d->retexD[i] = (int16_t)g2(&b);
			}
		} else if (op == 42) {
			int count = g1(&b);
			free(d->recolPalette);
			d->recolPaletteCount = count;
			d->recolPalette = malloc((size_t)count + 1);
			for (int i = 0; i < count; i++) d->recolPalette[i] = (int8_t)g1b(&b);
		} else if (op == 60) g2(&b);
		else if (op == 62) d->mirror = 1;
		else if (op == 64) d->active = 0;
		else if (op == 65) d->resizeX = g2(&b);
		else if (op == 66) d->resizeY = g2(&b);
		else if (op == 67) d->resizeZ = g2(&b);
		else if (op == 69) g1(&b);
		else if (op == 70) d->xoff = g2b(&b);
		else if (op == 71) d->yoff = g2b(&b);
		else if (op == 72) d->zoff = g2b(&b);
		else if (op == 73) d->forcedecor = 1;
		else if (op == 74) d->breakroutefinding = 1;
		else if (op == 75) d->supportitems = g1(&b);
		else if (op == 77 || op == 92) {
			int fallback = -1;
			d->multiLocVarbit = g2(&b);
			if (d->multiLocVarbit == 65535) d->multiLocVarbit = -1;
			d->multiLocVarp = g2(&b);
			if (d->multiLocVarp == 65535) d->multiLocVarp = -1;
			if (op == 92) {
				fallback = g2(&b);
				if (fallback == 65535) fallback = -1;
			}
			int len = g1(&b);
			free(d->multiLocs);
			d->multiLocCount = len + 2;
			d->multiLocs = malloc(sizeof(int32_t) * (size_t)(len + 2));
			for (int i = 0; i <= len; i++) {
				d->multiLocs[i] = g2(&b);
				if (d->multiLocs[i] == 65535) d->multiLocs[i] = -1;
			}
			d->multiLocs[len + 1] = fallback;
		} else if (op == 78) {
			g2(&b);
			g1(&b);
		} else if (op == 79) {
			g2(&b);
			g2(&b);
			g1(&b);
			int count = g1(&b);
			for (int i = 0; i < count; i++) g2(&b);
		} else if (op == 81) {
			d->hillskewType = 2;
			d->hillskewAmount = (int16_t)(g1(&b) * 256);
		} else if (op == 82) d->render = 1;
		else if (op == 88) d->castshadow = 0;
		else if (op == 90) d->clipped = 1;
		else if (op == 93) {
			d->hillskewType = 3;
			d->hillskewAmount = (int16_t)g2(&b);
		} else if (op == 94) d->hillskewType = 4;
		else if (op == 95) d->hillskewType = 5;
		else if (op == 98) d->dynamic = 1;
		else if (op == 99 || op == 100) {
			g1(&b);
			g2(&b);
		} else if (op == 101) g1(&b);
		else if (op == 102) d->mapscene = g2(&b);
		else if (op == 249) {
			int count = g1(&b);
			for (int i = 0; i < count; i++) {
				int isString = g1(&b) == 1;
				g3(&b);
				if (isString) skip_string(&b, text, sizeof(text));
				else b.pos += 4;
			}
		}
		// 89, 91, 96 and 97 have no payload and no effect on the scene
	}
	if (d->interactable == -1) {
		d->interactable = 0;
		if (d->models && (!d->shapes || d->shapes[0] == 10)) d->interactable = 1;
		if (hasOps) d->interactable = 1;
	}
	if (d->supportitems == -1) d->supportitems = d->blockwalk == 0 ? 0 : 1;
	if (d->breakroutefinding) {
		d->blockwalk = 0;
		d->blockrange = 0;
	}
}

void loc_free(LocDef* d) {
	free(d->name);
	for (int i = 0; i < 5; i++) free(d->ops[i]);
	free(d->models); free(d->shapes);
	free(d->recolS); free(d->recolD); free(d->retexS); free(d->retexD);
	free(d->recolPalette); free(d->multiLocs);
	memset(d, 0, sizeof(*d));
}

static void read_swaps(Buf* b, int* count, int16_t** from, int16_t** to) {
	int n = g1(b);
	free(*from);
	free(*to);
	*count = n;
	*from = malloc(2 * (size_t)(n + 1));
	*to = malloc(2 * (size_t)(n + 1));
	for (int i = 0; i < n; i++) {
		(*from)[i] = (int16_t)g2(b);
		(*to)[i] = (int16_t)g2(b);
	}
}

void idk_decode(IdkDef* d, const uint8_t* src, size_t size) {
	memset(d, 0, sizeof(*d));
	for (int i = 0; i < 5; i++) d->heads[i] = -1;
	Buf b = {src, src ? size : 0, 0};
	while (b.pos < b.size) {
		int op = g1(&b);
		if (op == 0) break;
		if (op == 1) g1(&b);
		else if (op == 2) {
			int count = g1(&b);
			d->modelCount = count;
			d->models = malloc(sizeof(int32_t) * (size_t)(count + 1));
			for (int i = 0; i < count; i++) d->models[i] = g2(&b);
		} else if (op == 40) read_swaps(&b, &d->recolCount, &d->recolS, &d->recolD);
		else if (op == 41) read_swaps(&b, &d->retexCount, &d->retexS, &d->retexD);
		else if (op >= 60 && op < 70) {
			int head = g2(&b);
			if (op < 65) d->heads[op - 60] = head;
		}
	}
}

void obj_free(ObjDef* d) {
	free(d->recolS);
	free(d->recolD);
	free(d->retexS);
	free(d->retexD);
	free(d->name);
	for (int i = 0; i < 5; i++) {
		free(d->iops[i]);
		free(d->ops[i]);
	}
}

void obj_decode(ObjDef* d, int id, const uint8_t* src, size_t size) {
	memset(d, 0, sizeof(*d));
	d->manhead = d->womanhead = d->manhead2 = d->womanhead2 = -1;
	d->id = id;
	d->manwear = d->manwear2 = d->manwear3 = d->womanwear = d->womanwear2 = d->womanwear3 = -1;
	d->certTemplate = d->certLink = d->lentLink = d->lentTemplate = -1;
	d->zoom2d = 2000;
	d->resizeX = d->resizeY = d->resizeZ = 128;
	d->iops[4] = copy_text("Drop");
	d->ops[2] = copy_text("Take");
	d->cost = 1;
	Buf b = {src, src ? size : 0, 0};
	char text[256];
	while (b.pos < b.size) {
		int op = g1(&b);
		if (op == 0) break;
		if (op == 1) d->model = g2(&b);
		else if (op == 4) d->zoom2d = g2(&b);
		else if (op == 5) d->xan2d = g2(&b);
		else if (op == 6) d->yan2d = g2(&b);
		else if (op == 7) d->xof2d = g2b(&b);
		else if (op == 8) d->yof2d = g2b(&b);
		else if (op == 11) d->stackable = 1;
		else if (op == 16) d->members = 1;
		else if (op == 95) d->zan2d = g2(&b);
		else if (op == 110) d->resizeX = g2(&b);
		else if (op == 111) d->resizeY = g2(&b);
		else if (op == 112) d->resizeZ = g2(&b);
		else if (op == 113) d->ambient = g1b(&b);
		else if (op == 114) d->contrast = g1b(&b) * 5;
		else if (op >= 90 && op <= 93) {
			int head = g2(&b);
			if (op == 90) d->manhead = head;
			else if (op == 91) d->womanhead = head;
			else if (op == 92) d->manhead2 = head;
			else d->womanhead2 = head;
		}
		else if (op == 2) {
			skip_string(&b, text, sizeof(text));
			free(d->name);
			d->name = copy_text(text);
		} else if (op == 12) {
			d->cost = g2(&b) << 16;
			d->cost |= g2(&b);
		}
		else if (op == 23) d->manwear = g2(&b);
		else if (op == 24) d->manwear2 = g2(&b);
		else if (op == 25) d->womanwear = g2(&b);
		else if (op == 26) d->womanwear2 = g2(&b);
		else if (op >= 30 && op <= 34) {
			skip_string(&b, text, sizeof(text));
			free(d->ops[op - 30]);
			d->ops[op - 30] = equals_hidden(text) ? NULL : copy_text(text);
		}
		else if (op >= 35 && op <= 39) {
			skip_string(&b, text, sizeof(text));
			free(d->iops[op - 35]);
			d->iops[op - 35] = copy_text(text);
		} else if (op == 40) read_swaps(&b, &d->recolCount, &d->recolS, &d->recolD);
		else if (op == 41) read_swaps(&b, &d->retexCount, &d->retexS, &d->retexD);
		else if (op == 42) {
			int count = g1(&b);
			b.pos += (size_t)count;
		} else if (op == 78) d->manwear3 = g2(&b);
		else if (op == 79) d->womanwear3 = g2(&b);
		else if (op == 96 || op == 115) g1(&b);
		else if (op == 97) d->certLink = g2(&b);
		else if (op == 98) d->certTemplate = g2(&b);
		else if (op >= 100 && op <= 109) {
			d->hasCounts = 1;
			d->countObj[op - 100] = g2(&b);
			d->countCo[op - 100] = g2(&b);
		} else if (op == 121) d->lentLink = g2(&b);
		else if (op == 122) d->lentTemplate = g2(&b);
		else if (op == 125) for (int i = 0; i < 3; i++) d->manOffset[i] = (int8_t)g1b(&b);
		else if (op == 126) for (int i = 0; i < 3; i++) d->womanOffset[i] = (int8_t)g1b(&b);
		else if (op >= 127 && op <= 130) {
			g1(&b);
			g2(&b);
		} else if (op == 249) {
			int count = g1(&b);
			for (int i = 0; i < count; i++) {
				int isString = g1(&b) == 1;
				g3(&b);
				if (isString) skip_string(&b, text, sizeof(text));
				else b.pos += 4;
			}
		}
	}
}

void npc_decode(NpcDef* d, int id, const uint8_t* src, size_t size) {
	memset(d, 0, sizeof(*d));
	d->id = id;
	d->size = 1;
	d->combatLevel = -1;
	d->rotationSpeed = 32;
	d->basId = -1;
	d->resizeX = d->resizeY = 128;
	d->multiVarbit = d->multiVarp = -1;
	d->interactive = 1;
	Buf b = {src, src ? size : 0, 0};
	char text[256];
	while (src) {
		int op = g1(&b);
		if (op == 0 || b.pos > b.size) break;
		if (op == 1) {
			int count = g1(&b);
			d->modelCount = count;
			d->models = malloc(sizeof(int32_t) * (size_t)(count + 1));
			for (int i = 0; i < count; i++) {
				d->models[i] = g2(&b);
				if (d->models[i] == 65535) d->models[i] = -1;
			}
		} else if (op == 2) {
			skip_string(&b, text, sizeof(text));
			free(d->name);
			d->name = copy_text(text);
		} else if (op == 12) d->size = g1(&b);
		else if (op >= 30 && op < 35) {
			skip_string(&b, text, sizeof(text));
			free(d->ops[op - 30]);
			d->ops[op - 30] = equals_hidden(text) ? NULL : copy_text(text);
		} else if (op == 40) {
			int count = g1(&b);
			d->recolCount = count;
			d->recolS = malloc(2 * (size_t)(count + 1));
			d->recolD = malloc(2 * (size_t)(count + 1));
			for (int i = 0; i < count; i++) {
				d->recolS[i] = (int16_t)g2(&b);
				d->recolD[i] = (int16_t)g2(&b);
			}
		} else if (op == 41) {
			int count = g1(&b);
			d->retexCount = count;
			d->retexS = malloc(2 * (size_t)(count + 1));
			d->retexD = malloc(2 * (size_t)(count + 1));
			for (int i = 0; i < count; i++) {
				d->retexS[i] = (int16_t)g2(&b);
				d->retexD[i] = (int16_t)g2(&b);
			}
		} else if (op == 42) {
			int count = g1(&b);
			d->recolPaletteCount = count;
			d->recolPalette = malloc((size_t)count + 1);
			for (int i = 0; i < count; i++) d->recolPalette[i] = (int8_t)g1b(&b);
		} else if (op == 60) {
			int count = g1(&b);
			free(d->headModels);
			d->headModels = malloc(sizeof(int32_t) * (size_t)(count + 1));
			d->headModelCount = count;
			for (int i = 0; i < count; i++) d->headModels[i] = g2(&b);
		} else if (op == 93 || op == 99 || op == 109 || op == 111) {
			// flags that do not affect the model
		} else if (op == 95) d->combatLevel = g2(&b);
		else if (op == 97) d->resizeX = g2(&b);
		else if (op == 98) d->resizeY = g2(&b);
		else if (op == 100) d->ambient = g1b(&b);
		else if (op == 101) d->contrast = g1b(&b) * 5;
		else if (op == 102) g2(&b);
		else if (op == 103) d->rotationSpeed = g2(&b);
		else if (op == 106 || op == 118) {
			int fallback = -1;
			d->multiVarbit = g2(&b);
			if (d->multiVarbit == 65535) d->multiVarbit = -1;
			d->multiVarp = g2(&b);
			if (d->multiVarp == 65535) d->multiVarp = -1;
			if (op == 118) {
				fallback = g2(&b);
				if (fallback == 65535) fallback = -1;
			}
			int count = g1(&b);
			d->multiCount = count + 2;
			d->multiNpcs = malloc(sizeof(int32_t) * (size_t)(count + 2));
			for (int i = 0; i <= count; i++) {
				d->multiNpcs[i] = g2(&b);
				if (d->multiNpcs[i] == 65535) d->multiNpcs[i] = -1;
			}
			d->multiNpcs[count + 1] = fallback;
		} else if (op == 107) d->interactive = 0;
		else if (op == 113) {
			g2(&b);
			g2(&b);
		} else if (op == 114 || op == 115) {
			g1(&b);
			g1(&b);
		} else if (op == 119 || op == 125 || op == 128) g1(&b);
		else if (op == 121) {
			int count = g1(&b);
			d->offsets = calloc((size_t)(d->modelCount + 1) * 3, sizeof(int32_t));
			d->hasOffset = calloc((size_t)d->modelCount + 1, 1);
			for (int i = 0; i < count; i++) {
				int j = g1(&b);
				int x = g1b(&b), y = g1b(&b), z = g1b(&b);
				if (j < d->modelCount) {
					d->hasOffset[j] = 1;
					d->offsets[j * 3] = x;
					d->offsets[j * 3 + 1] = y;
					d->offsets[j * 3 + 2] = z;
				}
			}
		} else if (op == 122 || op == 123 || op == 126 || op == 137) g2(&b);
		else if (op == 127) d->basId = g2(&b);
		else if (op == 134) {
			g2(&b);
			g2(&b);
			g2(&b);
			g2(&b);
			g1(&b);
		} else if (op == 135 || op == 136) {
			g1(&b);
			g2(&b);
		} else if (op == 249) {
			int count = g1(&b);
			for (int i = 0; i < count; i++) {
				int isString = g1(&b) == 1;
				g3(&b);
				if (isString) skip_string(&b, text, sizeof(text));
				else b.pos += 4;
			}
		}
	}
}

void loc_read_placements(const uint8_t* src, size_t size, int baseX, int baseY, LocEmit emit, void* context) {
	Buf b = {src, size, 0};
	int id = -1;
	for (;;) {
		int step = gvarsmart(&b);
		if (step == 0 || b.pos > b.size) return;
		id += step;
		int position = 0;
		for (;;) {
			int offset = gsmarts(&b);
			if (offset == 0 || b.pos > b.size) break;
			position += offset - 1;
			int flags = g1(&b);
			LocPlacement p = {id, flags >> 2, flags & 3, position >> 12, (position >> 6 & 0x3F) + baseX, (position & 0x3F) + baseY};
			if (p.x > 0 && p.y > 0 && p.x < 103 && p.y < 103) emit(context, &p);
		}
	}
}
