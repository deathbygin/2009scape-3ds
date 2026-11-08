// Items on the ground. Each tile shows its most valuable item, as the client does, and the menu lists them all.

#define GROUND_SHOWN_MAX 256
#define GROUND_KEY (1ULL << 62)

typedef struct {
	u8 x, y;
	int id; // the item whose model is drawn
	u64 key;
} GroundPile;

static GroundPile groundPiles[GROUND_SHOWN_MAX];
static int groundPileCount = 0;
static long groundSeen = -1;

// A big stack can have its own model.
static const ObjDef* groundVariant(const ObjDef* d, int amount) {
	if (amount <= 1 || !d->hasCounts) return d;
	int id = -1;
	for (int i = 0; i < 10; i++) if (amount >= d->countCo[i] && d->countCo[i] != 0) id = d->countObj[i];
	const ObjDef* other = id == -1 ? NULL : itemDef(id);
	return other ? other : d;
}

static void rebuildGroundPiles(void) {
	groundPileCount = 0;
	for (int i = 0; i < net.groundCount; i++) {
		const NetGround* g = &net.ground[i];
		if (g->plane != net.plane) continue;
		bool seen = false;
		for (int k = 0; k < groundPileCount; k++) if (groundPiles[k].x == g->x && groundPiles[k].y == g->y) seen = true;
		if (seen || groundPileCount == GROUND_SHOWN_MAX) continue;
		// Protocol.spawnGroundObject: the first item with the highest value wins.
		long best = -99999999;
		const NetGround* top = NULL;
		for (int k = 0; k < net.groundCount; k++) {
			const NetGround* o = &net.ground[k];
			if (o->x != g->x || o->y != g->y || o->plane != g->plane) continue;
			const ObjDef* d = itemDef(o->id);
			if (!d) continue;
			long value = d->cost;
			if (d->stackable) value *= (long)o->amount + 1;
			if (best < value) {
				best = value;
				top = o;
			}
		}
		if (!top) continue;
		const ObjDef* shown = groundVariant(itemDef(top->id), top->amount);
		GroundPile* p = &groundPiles[groundPileCount++];
		p->x = g->x;
		p->y = g->y;
		p->id = shown->id;
		p->key = GROUND_KEY | (u64)shown->id;
	}
}

// Runs once per frame. One model at most, since it reads from the SD card.
static void updateGround(bool* built) {
	if (groundSeen != net.groundVersion) {
		groundSeen = net.groundVersion;
		rebuildGroundPiles();
		panelDirty = true;
	}
	for (int i = 0; i < groundPileCount && !*built; i++) {
		if (findNpcModel(groundPiles[i].key)) continue;
		buildBody(groundPiles[i].key, NULL, NULL, itemDef(groundPiles[i].id));
		*built = true;
	}
}

static void drawGroundItems(const C3D_Mtx* mvp, float limit) {
	for (int i = 0; i < groundPileCount; i++) {
		const NpcModel* m = findNpcModel(groundPiles[i].key);
		if (m) drawModelAt(m, m->restPose, groundPiles[i].x + 0.5f, groundPiles[i].y + 0.5f, 0, mvp, limit);
	}
}

// True if a screen position is on the item itself, not merely on its tile,
// so that tapping to walk across a tile does not pick things up by accident.
static bool groundPileHit(const GroundPile* p, const float* origin, const float* direction, float ground) {
	const NpcModel* m = findNpcModel(p->key);
	int half = m ? m->radius : 40, top = m && m->top > 24 ? m->top : 24;
	if (half < 32) half = 32;
	if (half > 64) half = 64;
	int x = p->x * 128 + 64, y = p->y * 128 + 64, feet = (int)(-groundHeight(net.plane, p->x + 0.5f, p->y + 0.5f) * 128.0f);
	PickObject box;
	box.minX = (int16_t)(x - half);
	box.maxX = (int16_t)(x + half);
	box.minZ = (int16_t)(y - half);
	box.maxZ = (int16_t)(y + half);
	box.minY = (int16_t)(feet - top);
	box.maxY = (int16_t)feet;
	float hit;
	return rayHitsBox(origin, direction, &box, &hit) && hit <= ground + 0.5f;
}

// The server only understands the two ground options that real items use.
static bool groundOptionSendable(int op) {
	return op == 2 || op == 3;
}

static void addGroundEntries(int sx, int sy, int width, bool examine, int limit) {
	CameraBasis c = cameraBasis(width);
	float origin[3] = {c.eyeX, c.eyeY, c.eyeZ}, direction[3], ground = 1000.0f;
	viewRay(&c, sx, sy, width, &direction[0], &direction[1], &direction[2]);
	int tileX, tileY;
	pickTileAt(sx, sy, width, net.plane, &tileX, &tileY, &ground);
	for (int i = 0; i < groundPileCount; i++) {
		const GroundPile* p = &groundPiles[i];
		if (!groundPileHit(p, origin, direction, ground)) continue;
		for (int k = net.groundCount - 1; k >= 0; k--) {
			const NetGround* g = &net.ground[k];
			if (g->x != p->x || g->y != p->y || g->plane != net.plane) continue;
			const ObjDef* d = itemDef(g->id);
			if (!d) continue;
			const char* name = d->name ? d->name : "";
			if (examine) {
				if (menuCount < limit) addEntry(ENTRY_ITEM_EXAMINE, g->id, 0, 0, 0, "Examine", name);
			} else if (castComponent >= 0) {
				char verb[40];
				snprintf(verb, sizeof(verb), "Cast %s on", castName);
				if (menuCount < limit) addEntry(ENTRY_CAST_GROUND, g->id, 0, g->x, g->y, verb, name);
			} else if (usingSlot >= 0) {
				char verb[32];
				snprintf(verb, sizeof(verb), "Use %s on", itemName(usingItem));
				if (menuCount < limit) addEntry(ENTRY_USE_GROUND, g->id, 0, g->x, g->y, verb, name);
			} else {
				for (int op = 0; op < 5; op++)
					if (d->ops[op] && groundOptionSendable(op) && menuCount < limit) addEntry(ENTRY_GROUND, g->id, op, g->x, g->y, d->ops[op], name);
			}
		}
	}
}
