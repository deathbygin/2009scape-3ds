#include "anim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARCHIVE_FRAMES 0
#define ARCHIVE_BASES 1
#define ARCHIVE_CONFIG 2
#define ARCHIVE_SEQS 20
#define CONFIG_BAS_GROUP 32
#define FRAMESET_BUCKETS 256

typedef struct AnimBase {
	int id, transforms;
	uint8_t* types;
	uint8_t** bones;
	uint8_t* boneCounts;
	struct AnimBase* next;
} AnimBase;

typedef struct {
	const AnimBase* base;
	int length;
	int16_t *indices, *x, *y, *z, *prevOrigin;
} AnimFrame;

typedef struct AnimFrameset {
	int id, capacity;
	AnimFrame** frames;
	struct AnimFrameset* next;
} AnimFrameset;

typedef struct {
	const uint8_t* data;
	size_t size, pos;
} Buf;

static Js5Cache* cache = NULL;
static SeqDef** seqs = NULL;
static Js5Buffer* seqGroups = NULL;
static uint32_t seqGroupCount = 0;
static AnimFrameset* framesets[FRAMESET_BUCKETS];
static AnimBase* bases = NULL;
static int sinTable[2048], cosTable[2048];

static int g1(Buf* b) { return b->pos < b->size ? b->data[b->pos++] : (b->pos++, 0); }
static int g2(Buf* b) { int a = g1(b); return a << 8 | g1(b); }
static int gsmart(Buf* b) {
	int value = b->pos < b->size ? b->data[b->pos] : 0;
	return value < 128 ? g1(b) - 64 : g2(b) - 0xC000;
}

void anim_init(Js5Cache* c) {
	if (cache == c) return;
	cache = c;
	for (int i = 0; i < 2048; i++) {
		sinTable[i] = (int)(sin(i * 0.0030679615) * 65536.0);
		cosTable[i] = (int)(cos(i * 0.0030679615) * 65536.0);
	}
}

// ---- definitions ----

static void seq_decode(SeqDef* s, const uint8_t* src, size_t size) {
	Buf b = {src, src ? size : 0, 0};
	s->exactmove = 2;
	s->movetype = s->replayoff = s->looptype = s->mainhand = s->offhand = -1;
	s->replaycount = 99;
	s->priority = 5;
	while (b.pos < b.size) {
		int op = g1(&b);
		if (op == 0) break;
		if (op == 1) {
			int count = g2(&b);
			s->frameCount = count;
			s->delays = malloc(sizeof(int32_t) * (size_t)(count + 1));
			s->frames = malloc(sizeof(int32_t) * (size_t)(count + 1));
			for (int i = 0; i < count; i++) s->delays[i] = g2(&b);
			for (int i = 0; i < count; i++) s->frames[i] = g2(&b);
			for (int i = 0; i < count; i++) s->frames[i] += g2(&b) << 16;
		} else if (op == 2) {
			s->replayoff = g2(&b);
		} else if (op == 3) {
			int count = g1(&b);
			s->framegroup = calloc(256, 1);
			for (int i = 0; i < count; i++) s->framegroup[g1(&b)] = 1;
		} else if (op == 5) {
			s->priority = g1(&b);
		} else if (op == 6) {
			s->mainhand = g2(&b);
		} else if (op == 7) {
			s->offhand = g2(&b);
		} else if (op == 8) {
			s->replaycount = g1(&b);
		} else if (op == 9) {
			s->looptype = g1(&b);
		} else if (op == 10) {
			s->movetype = g1(&b);
		} else if (op == 11) {
			s->exactmove = g1(&b);
		} else if (op == 12) {
			int count = g1(&b);
			b.pos += (size_t)count * 4;
		} else if (op == 13) {
			int count = g2(&b);
			for (int i = 0; i < count; i++) {
				int sounds = g1(&b);
				if (sounds > 0) b.pos += 3 + (size_t)(sounds - 1) * 2;
			}
		}
	}
	if (s->looptype == -1) s->looptype = s->framegroup ? 2 : 0;
	if (s->movetype == -1) s->movetype = s->framegroup ? 2 : 0;
}

const SeqDef* anim_get_seq(int id) {
	if (!cache || id < 0) return NULL;
	if (!seqs) {
		if (js5_load_index(cache, ARCHIVE_SEQS) != 0) return NULL;
		seqGroupCount = cache->index[ARCHIVE_SEQS].capacity;
		seqs = calloc((size_t)seqGroupCount * 128, sizeof(SeqDef*));
		seqGroups = calloc(seqGroupCount, sizeof(Js5Buffer));
		if (!seqs || !seqGroups) return NULL;
	}
	uint32_t group = (uint32_t)id >> 7;
	if (group >= seqGroupCount) return NULL;
	if (seqs[id]) return seqs[id];
	if (!seqGroups[group].data && js5_read_group(cache, ARCHIVE_SEQS, group, NULL, &seqGroups[group]) != 0) {
		seqGroups[group].data = NULL;
		return NULL;
	}
	SeqDef* s = calloc(1, sizeof(SeqDef));
	if (!s) return NULL;
	s->id = id;
	Js5Buffer file = {0, 0};
	if (js5_group_file(&cache->index[ARCHIVE_SEQS], group, &seqGroups[group], (uint32_t)(id & 127), &file) == 0) {
		seq_decode(s, file.data, file.size);
		free(file.data);
	} else {
		seq_decode(s, NULL, 0);
	}
	seqs[id] = s;
	return s;
}

static void bas_decode(BasDef* d, const uint8_t* src, size_t size) {
	Buf b = {src, size, 0};
	while (b.pos < b.size) {
		int op = g1(&b);
		if (op == 0) break;
		if (op == 1) {
			d->idle = g2(&b);
			d->walk = g2(&b);
			if (d->idle == 65535) d->idle = -1;
			if (d->walk == 65535) d->walk = -1;
		} else if (op >= 2 && op <= 5) {
			g2(&b); // slow walking is not used by the server
		} else if (op == 6) d->run = g2(&b);
		else if (op == 7) d->runTurn = g2(&b);
		else if (op == 8) d->runCCW = g2(&b);
		else if (op == 9) d->runCW = g2(&b);
		else if (op == 26) {
			g1(&b);
			g1(&b);
		} else if (op == 27) {
			int slot = g1(&b);
			for (int i = 0; i < 6; i++) {
				int16_t v = (int16_t)g2(&b);
				if (slot < 12) d->transforms[slot][i] = v;
			}
			if (slot < 12) d->transformsUsed |= 1u << slot;
		} else if (op == 29 || op == 31 || op == 34 || op == 37) {
			g1(&b);
		} else if (op == 38) d->standCCW = g2(&b);
		else if (op == 39) d->standCW = g2(&b);
		else if (op == 40) d->walkTurn = g2(&b);
		else if (op == 41) d->walkCCW = g2(&b);
		else if (op == 42) d->walkCW = g2(&b);
		else if (op == 30 || op == 32 || op == 33 || op == 35 || op == 36 || (op >= 43 && op <= 45)) {
			g2(&b);
		}
	}
	// The cache stores "none" as 65535 for these too.
	int* all[] = {&d->run, &d->runTurn, &d->runCW, &d->runCCW, &d->walkTurn, &d->walkCW, &d->walkCCW, &d->standCW, &d->standCCW};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) if (*all[i] == 65535) *all[i] = -1;
}

const BasDef* anim_get_bas(int id) {
	static Js5Buffer group = {0, 0};
	static BasDef none;
	static BasDef** table = NULL;
	static int tableSize = 0;
	static int ready = 0;
	if (!ready) {
		memset(&none, 0xFF, sizeof(none));
		none.transformsUsed = 0;
		ready = 1;
	}
	if (id < 0 || !cache) return &none;
	if (!group.data) {
		if (js5_load_index(cache, ARCHIVE_CONFIG) != 0 || js5_read_group(cache, ARCHIVE_CONFIG, CONFIG_BAS_GROUP, NULL, &group) != 0) {
			group.data = NULL;
			return &none;
		}
		tableSize = 8192;
		table = calloc((size_t)tableSize, sizeof(BasDef*));
		if (!table) return &none;
	}
	if (id >= tableSize) return &none;
	if (table[id]) return table[id];
	BasDef* d = malloc(sizeof(BasDef));
	if (!d) return &none;
	*d = none;
	Js5Buffer file = {0, 0};
	if (js5_group_file(&cache->index[ARCHIVE_CONFIG], CONFIG_BAS_GROUP, &group, (uint32_t)id, &file) == 0) {
		bas_decode(d, file.data, file.size);
		free(file.data);
	}
	table[id] = d;
	return d;
}

// ---- framesets ----

static const AnimBase* get_base(int id) {
	for (AnimBase* b = bases; b; b = b->next) if (b->id == id) return b;
	Js5Buffer group = {0, 0}, file = {0, 0};
	if (js5_load_index(cache, ARCHIVE_BASES) != 0 || js5_read_group(cache, ARCHIVE_BASES, (uint32_t)id, NULL, &group) != 0) return NULL;
	int rc = js5_group_file(&cache->index[ARCHIVE_BASES], (uint32_t)id, &group, 0, &file);
	free(group.data);
	if (rc != 0) return NULL;
	Buf in = {file.data, file.size, 0};
	AnimBase* b = calloc(1, sizeof(AnimBase));
	b->id = id;
	b->transforms = g1(&in);
	int n = b->transforms;
	b->types = malloc((size_t)n + 1);
	b->boneCounts = malloc((size_t)n + 1);
	b->bones = calloc((size_t)n + 1, sizeof(uint8_t*));
	for (int i = 0; i < n; i++) b->types[i] = (uint8_t)g1(&in);
	in.pos += (size_t)n;     // shadow flags
	in.pos += (size_t)n * 2; // part masks
	for (int i = 0; i < n; i++) b->boneCounts[i] = (uint8_t)g1(&in);
	for (int i = 0; i < n; i++) {
		b->bones[i] = malloc((size_t)b->boneCounts[i] + 1);
		for (int j = 0; j < b->boneCounts[i]; j++) b->bones[i][j] = (uint8_t)g1(&in);
	}
	free(file.data);
	b->next = bases;
	bases = b;
	return b;
}

static AnimFrame* frame_decode(const uint8_t* src, size_t size) {
	static int16_t indices[256], tx[256], ty[256], tz[256], prev[256];
	if (size < 3) return NULL;
	const AnimBase* base = get_base(src[0] << 8 | src[1]);
	if (!base) return NULL;
	int headerLength = src[2], length = 0, prevOrigin = -1, prevUsedOrigin = -1;
	Buf header = {src, size, 3}, b = {src, size, 3 + (size_t)headerLength};
	for (int i = 0; i < headerLength && i < base->transforms; i++) {
		int type = base->types[i], attributes = g1(&header);
		if (type == 0) prevOrigin = i;
		if (attributes <= 0) continue;
		if (type == 0) prevUsedOrigin = i;
		indices[length] = (int16_t)i;
		int16_t fallback = type == 3 ? 128 : 0;
		tx[length] = attributes & 1 ? (int16_t)gsmart(&b) : fallback;
		ty[length] = attributes & 2 ? (int16_t)gsmart(&b) : fallback;
		tz[length] = attributes & 4 ? (int16_t)gsmart(&b) : fallback;
		if (type == 2) {
			tx[length] = (int16_t)(((tx[length] & 0xFF) << 3) + (tx[length] >> 8 & 7));
			ty[length] = (int16_t)(((ty[length] & 0xFF) << 3) + (ty[length] >> 8 & 7));
			tz[length] = (int16_t)(((tz[length] & 0xFF) << 3) + (tz[length] >> 8 & 7));
		}
		prev[length] = -1;
		if ((type == 1 || type == 2 || type == 3) && prevOrigin > prevUsedOrigin) {
			prev[length] = (int16_t)prevOrigin;
			prevUsedOrigin = prevOrigin;
		}
		length++;
	}
	AnimFrame* f = malloc(sizeof(AnimFrame));
	if (!f) return NULL;
	f->base = base;
	f->length = length;
	size_t bytes = (size_t)(length + 1) * 2;
	int16_t* block = malloc(bytes * 5);
	if (!block) {
		free(f);
		return NULL;
	}
	f->indices = block;
	f->x = block + length + 1;
	f->y = f->x + length + 1;
	f->z = f->y + length + 1;
	f->prevOrigin = f->z + length + 1;
	memcpy(f->indices, indices, (size_t)length * 2);
	memcpy(f->x, tx, (size_t)length * 2);
	memcpy(f->y, ty, (size_t)length * 2);
	memcpy(f->z, tz, (size_t)length * 2);
	memcpy(f->prevOrigin, prev, (size_t)length * 2);
	return f;
}

static const AnimFrameset* find_frameset(int id) {
	for (AnimFrameset* f = framesets[id & (FRAMESET_BUCKETS - 1)]; f; f = f->next) if (f->id == id) return f;
	return NULL;
}

int anim_frameset_loaded(int id) {
	return find_frameset(id) != NULL;
}

static int read_frameset(int id) {
	if (!cache || js5_load_index(cache, ARCHIVE_FRAMES) != 0) return 0;
	const Js5Index* index = &cache->index[ARCHIVE_FRAMES];
	if (id < 0 || (uint32_t)id >= index->capacity || !index->groups[id].present) return 0;
	const Js5Group* g = &index->groups[id];
	Js5Buffer group = {0, 0};
	if (js5_read_group(cache, ARCHIVE_FRAMES, (uint32_t)id, NULL, &group) != 0) return 0;
	int capacity = 0;
	for (uint32_t i = 0; i < g->size; i++) {
		int fileId = g->fileIds ? (int)g->fileIds[i] : (int)i;
		if (fileId + 1 > capacity) capacity = fileId + 1;
	}
	AnimFrameset* set = calloc(1, sizeof(AnimFrameset));
	if (set) set->frames = calloc((size_t)capacity + 1, sizeof(AnimFrame*));
	if (!set || !set->frames) {
		free(set);
		free(group.data);
		return 0;
	}
	set->id = id;
	set->capacity = capacity;
	for (uint32_t i = 0; i < g->size; i++) {
		uint32_t fileId = g->fileIds ? g->fileIds[i] : i;
		Js5Buffer file = {0, 0};
		if (js5_group_file(index, (uint32_t)id, &group, fileId, &file) != 0) continue;
		set->frames[fileId] = frame_decode(file.data, file.size);
		free(file.data);
	}
	free(group.data);
	int bucket = id & (FRAMESET_BUCKETS - 1);
	set->next = framesets[bucket];
	framesets[bucket] = set;
	return 1;
}

int anim_load_frameset(int id) {
	if (find_frameset(id)) return 1;
	if (read_frameset(id)) return 1;
	AnimFrameset* empty = calloc(1, sizeof(AnimFrameset));
	if (!empty) return 0;
	empty->id = id;
	empty->next = framesets[id & (FRAMESET_BUCKETS - 1)];
	framesets[id & (FRAMESET_BUCKETS - 1)] = empty;
	return 0;
}

void anim_clear_framesets(void) {
	for (int i = 0; i < FRAMESET_BUCKETS; i++) {
		AnimFrameset* f = framesets[i];
		while (f) {
			AnimFrameset* next = f->next;
			for (int k = 0; k < f->capacity; k++) {
				if (!f->frames[k]) continue;
				free(f->frames[k]->indices);
				free(f->frames[k]);
			}
			free(f->frames);
			free(f);
			f = next;
		}
		framesets[i] = NULL;
	}
	while (bases) {
		AnimBase* next = bases->next;
		for (int i = 0; i < bases->transforms; i++) free(bases->bones[i]);
		free(bases->bones);
		free(bases->boneCounts);
		free(bases->types);
		free(bases);
		bases = next;
	}
}

int anim_frame_ready(const SeqDef* seq, int frame, int* missing) {
	if (!seq || frame < 0 || frame >= seq->frameCount) return 1;
	int id = (int)((uint32_t)seq->frames[frame] >> 16);
	if (find_frameset(id)) return 1;
	*missing = id;
	return 0;
}

// ---- posing ----

BoneTable* anim_build_bones(const int32_t* vertexBones, int vertexCount) {
	if (!vertexBones) return NULL;
	uint32_t counts[257];
	memset(counts, 0, sizeof(counts));
	int maxBone = 0;
	for (int i = 0; i < vertexCount; i++) {
		int bone = vertexBones[i] & 0xFF;
		counts[bone]++;
		if (bone > maxBone) maxBone = bone;
	}
	BoneTable* t = malloc(sizeof(BoneTable));
	if (!t) return NULL;
	t->boneCount = maxBone + 1;
	t->start = malloc(sizeof(uint32_t) * (size_t)(maxBone + 2));
	t->vertices = malloc(2 * (size_t)(vertexCount + 1));
	if (!t->start || !t->vertices) {
		anim_free_bones(t);
		return NULL;
	}
	uint32_t at = 0;
	for (int i = 0; i <= maxBone; i++) {
		t->start[i] = at;
		at += counts[i];
		counts[i] = t->start[i];
	}
	t->start[maxBone + 1] = at;
	for (int i = 0; i < vertexCount; i++) t->vertices[counts[vertexBones[i] & 0xFF]++] = (uint16_t)i;
	return t;
}

void anim_free_bones(BoneTable* bones) {
	if (!bones) return;
	free(bones->start);
	free(bones->vertices);
	free(bones);
}

typedef struct {
	const BoneTable* bones;
	int32_t *vx, *vy, *vz;
	int originX, originY, originZ;
} Pose;

// SoftwareModel.transformBone, for the types that move vertices.
static void transform_bone(Pose* p, int type, const uint8_t* labels, int labelCount, int tx, int ty, int tz) {
	const BoneTable* t = p->bones;
	int32_t *vx = p->vx, *vy = p->vy, *vz = p->vz;
	if (type == 0) {
		long sumX = 0, sumY = 0, sumZ = 0;
		int count = 0;
		for (int i = 0; i < labelCount; i++) {
			int label = labels[i];
			if (label >= t->boneCount) continue;
			for (uint32_t k = t->start[label]; k < t->start[label + 1]; k++) {
				int v = t->vertices[k];
				sumX += vx[v];
				sumY += vy[v];
				sumZ += vz[v];
				count++;
			}
		}
		p->originX = (count > 0 ? (int)(sumX / count) : 0) + tx;
		p->originY = (count > 0 ? (int)(sumY / count) : 0) + ty;
		p->originZ = (count > 0 ? (int)(sumZ / count) : 0) + tz;
		return;
	}
	if (type != 1 && type != 2 && type != 3) return;
	int sinX = sinTable[tx & 2047], cosX = cosTable[tx & 2047];
	int sinY = sinTable[ty & 2047], cosY = cosTable[ty & 2047];
	int sinZ = sinTable[tz & 2047], cosZ = cosTable[tz & 2047];
	int ox = p->originX, oy = p->originY, oz = p->originZ;
	for (int i = 0; i < labelCount; i++) {
		int label = labels[i];
		if (label >= t->boneCount) continue;
		for (uint32_t k = t->start[label]; k < t->start[label + 1]; k++) {
			int v = t->vertices[k];
			if (type == 1) {
				vx[v] += tx;
				vy[v] += ty;
				vz[v] += tz;
			} else if (type == 2) {
				int x = vx[v] - ox, y = vy[v] - oy, z = vz[v] - oz, temp;
				if (tz != 0) {
					temp = (y * sinZ + x * cosZ + 32767) >> 16;
					y = (y * cosZ + 32767 - x * sinZ) >> 16;
					x = temp;
				}
				if (tx != 0) {
					temp = (y * cosX + 32767 - z * sinX) >> 16;
					z = (y * sinX + z * cosX + 32767) >> 16;
					y = temp;
				}
				if (ty != 0) {
					temp = (z * sinY + x * cosY + 32767) >> 16;
					z = (z * cosY + 32767 - x * sinY) >> 16;
					x = temp;
				}
				vx[v] = x + ox;
				vy[v] = y + oy;
				vz[v] = z + oz;
			} else {
				vx[v] = (vx[v] - ox) * tx / 128 + ox;
				vy[v] = (vy[v] - oy) * ty / 128 + oy;
				vz[v] = (vz[v] - oz) * tz / 128 + oz;
			}
		}
	}
}

static void apply_frame(Pose* p, const AnimFrame* f, const uint8_t* mask, int maskValue) {
	const AnimBase* base = f->base;
	for (int i = 0; i < f->length; i++) {
		int index = f->indices[i];
		if (mask && mask[index] != maskValue && base->types[index] != 0) continue;
		int previous = f->prevOrigin[i];
		if (previous != -1) transform_bone(p, 0, base->bones[previous], base->boneCounts[previous], 0, 0, 0);
		transform_bone(p, base->types[index], base->bones[index], base->boneCounts[index], f->x[i], f->y[i], f->z[i]);
	}
}

static const AnimFrame* frame_of(const SeqDef* seq, int frame) {
	if (!seq || frame < 0 || frame >= seq->frameCount) return NULL;
	uint32_t packed = (uint32_t)seq->frames[frame];
	const AnimFrameset* set = find_frameset((int)(packed >> 16));
	int index = (int)(packed & 0xFFFF);
	return set && index < set->capacity ? set->frames[index] : NULL;
}

void anim_apply_pose(const BoneTable* bones, int32_t* vx, int32_t* vy, int32_t* vz,
	const SeqDef* primary, int primaryFrame, const SeqDef* secondary, int secondaryFrame) {
	if (!bones) return;
	const AnimFrame* first = frame_of(primary, primaryFrame);
	const AnimFrame* second = frame_of(secondary, secondaryFrame);
	Pose p = {bones, vx, vy, vz, 0, 0, 0};
	if (second && first && secondary->framegroup) {
		apply_frame(&p, second, secondary->framegroup, 0);
		p.originX = p.originY = p.originZ = 0;
		apply_frame(&p, first, secondary->framegroup, 1);
	} else if (second) {
		apply_frame(&p, second, NULL, 0);
	} else if (first) {
		apply_frame(&p, first, NULL, 0);
	}
}
