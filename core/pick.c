#include "pick.h"

const LocDef* scene_get_loc(Scene* s, int id);

typedef struct {
	PickObject* out;
	int count, max;
	Scene* scene;
} Builder;

static const LocDef* shown_def(Scene* s, int id) {
	const LocDef* d = scene_get_loc(s, id);
	if (d && d->multiLocs) {
		// Without server state every variable reads 0.
		int next = d->multiLocs[0] != -1 ? d->multiLocs[0] : d->multiLocs[d->multiLocCount - 1];
		d = next == -1 ? NULL : scene_get_loc(s, next);
	}
	return d;
}

static void grow(PickObject* p, const SceneEntity* e, int wx, int wy, int wz) {
	if (!e || !e->base) return;
	for (int v = 0; v < e->base->vertexCount; v++) {
		int x = e->vx[v] + wx, y = e->vy[v] + wy, z = e->vz[v] + wz;
		if (x < p->minX) p->minX = (int16_t)x;
		if (x > p->maxX) p->maxX = (int16_t)x;
		if (y < p->minY) p->minY = (int16_t)y;
		if (y > p->maxY) p->maxY = (int16_t)y;
		if (z < p->minZ) p->minZ = (int16_t)z;
		if (z > p->maxZ) p->maxZ = (int16_t)z;
	}
}

static PickObject* start(Builder* b, int id, int level, int x, int y, int layer) {
	if (b->count >= b->max) return NULL;
	const LocDef* base = scene_get_loc(b->scene, id);
	if (!base || base->interactable == 0) return NULL;
	const LocDef* def = shown_def(b->scene, id);
	if (!def) return NULL;
	PickObject* p = &b->out[b->count];
	p->def = def;
	p->id = id;
	p->level = (uint8_t)level;
	p->layer = (uint8_t)layer;
	p->hidden = 0;
	p->dynamic = 0;
	p->tileX = (int16_t)x;
	p->tileY = (int16_t)y;
	int bridged = b->scene->terrain.flags[1][x][y] & 2;
	p->plane = (uint8_t)(bridged && level > 0 ? level - 1 : level);
	p->minX = p->minY = p->minZ = 32767;
	p->maxX = p->maxY = p->maxZ = -32768;
	return p;
}

static void finish(Builder* b, const PickObject* p) {
	if (p->minX <= p->maxX) b->count++;
}

int pick_build(Scene* s, PickObject* out, int max) {
	Builder b = {out, 0, max, s};
	for (int l = 0; l < SCENE_LEVELS; l++) {
		for (int x = 0; x < SCENE_SIZE; x++) {
			for (int y = 0; y < SCENE_SIZE; y++) {
				const SceneTile* t = &s->tiles[l][x][y];
				PickObject* p;
				if (t->wall && (p = start(&b, t->wall->locId, l, x, y, 0))) {
					grow(p, t->wall->primary, t->wall->xFine, t->wall->zFine, t->wall->yFine);
					grow(p, t->wall->secondary, t->wall->xFine, t->wall->zFine, t->wall->yFine);
					finish(&b, p);
				}
				if (t->decor && (p = start(&b, t->decor->locId, l, x, y, 1))) {
					const SceneDecor* d = t->decor;
					grow(p, d->primary, d->xFine + d->xOffset, d->height, d->yFine + d->yOffset);
					grow(p, d->secondary, d->xFine, d->height, d->yFine);
					finish(&b, p);
				}
				if (t->ground && (p = start(&b, t->ground->locId, l, x, y, 3))) {
					grow(p, t->ground->entity, t->ground->xFine, t->ground->zFine, t->ground->yFine);
					finish(&b, p);
				}
				for (int i = 0; i < t->sceneryCount; i++) {
					const SceneScenery* c = t->scenery[i];
					if (c->xMin != x || c->yMin != y) continue;
					if ((p = start(&b, c->locId, l, x, y, 2))) {
						grow(p, c->entity, c->xFine, c->zFine, c->yFine);
						finish(&b, p);
					}
				}
			}
		}
	}
	return b.count;
}
