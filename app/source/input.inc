// All input enters here, from the real main loop and from the self-test alike.
// One press produces at most one action. Holding never repeats anything.

#define LONG_PRESS_MS 450
#define BAG_HOLD_MS 800   // longer than in the game view, so picking up an item never opens a menu by mistake
#define BAG_DRAG_PIXELS 8 // the stylus must move this far to count as a drag, so a shaky hold stays a hold
#define MENU_ROWS 32   // every option is kept, as on PC
#define MENU_VISIBLE 8 // what fits on the screen at once
#define MENU_ROW_HEIGHT 28
#define MAX_HITS 6

static float groundHeight(int plane, float x, float y) {
	int tx = (int)x, ty = (int)y;
	if (tx < 0 || ty < 0 || tx >= 104 || ty >= 104) return 0;
	float fx = x - tx, fy = y - ty;
	const int32_t (*h)[105] = sceneHeights[plane & 3];
	float a = h[tx][ty] * (1 - fx) + h[tx + 1][ty] * fx;
	float b = h[tx][ty + 1] * (1 - fx) + h[tx + 1][ty + 1] * fx;
	return -(a * (1 - fy) + b * fy) / 128.0f;
}

typedef struct {
	float eyeX, eyeY, eyeZ;
	float fX, fY, fZ, rX, rZ, uX, uY, uZ;
	float tanH, tanV;
} CameraBasis;

static CameraBasis cameraBasis(int width) {
	CameraBasis c;
	float cosP = cosf(pitch), sinP = sinf(pitch);
	c.eyeX = targetX + sinf(yaw) * cosP * zoom;
	c.eyeZ = targetZ + cosf(yaw) * cosP * zoom;
	c.eyeY = targetH + sinP * zoom;
	c.fX = -sinf(yaw) * cosP;
	c.fY = -sinP;
	c.fZ = -cosf(yaw) * cosP;
	float length = sqrtf(c.fX * c.fX + c.fZ * c.fZ);
	c.rX = -c.fZ / length;
	c.rZ = c.fX / length;
	c.uX = -c.rZ * c.fY;
	c.uY = c.rZ * c.fX - c.rX * c.fZ;
	c.uZ = c.rX * c.fY;
	c.tanV = tanf(C3D_AngleFromDegrees(25.0f));
	c.tanH = c.tanV * width / (float)SCREEN_H;
	return c;
}

static void viewRay(const CameraBasis* c, int sx, int sy, int width, float* dx, float* dy, float* dz) {
	float ndcX = 2.0f * sx / width - 1.0f, ndcY = 1.0f - 2.0f * sy / SCREEN_H;
	*dx = c->fX + c->rX * ndcX * c->tanH + c->uX * ndcY * c->tanV;
	*dy = c->fY + c->uY * ndcY * c->tanV;
	*dz = c->fZ + c->rZ * ndcX * c->tanH + c->uZ * ndcY * c->tanV;
	float length = sqrtf(*dx * *dx + *dy * *dy + *dz * *dz);
	*dx /= length;
	*dy /= length;
	*dz /= length;
}

// Finds the local tile under a screen position by stepping along the view ray until it meets the ground.
static bool pickTileAt(int sx, int sy, int width, int plane, int* outX, int* outY, float* distance) {
	CameraBasis c = cameraBasis(width);
	float dx, dy, dz;
	viewRay(&c, sx, sy, width, &dx, &dy, &dz);
	for (float t = 0.5f; t < 80.0f; t += 0.05f) {
		float x = c.eyeX + dx * t, y = c.eyeY + dy * t, north = -(c.eyeZ + dz * t);
		if (x < 0 || north < 0 || x >= 103 || north >= 103) continue;
		if (y <= groundHeight(plane, x, north)) {
			*outX = (int)x;
			*outY = (int)north;
			if (distance) *distance = t;
			return true;
		}
	}
	return false;
}

static bool pickTile(int sx, int sy, int width, int plane, int* outX, int* outY) {
	return pickTileAt(sx, sy, width, plane, outX, outY, NULL);
}

static bool rayHitsBox(const float* origin, const float* direction, const PickObject* p, float* distance) {
	float lo[3] = {p->minX / 128.0f, -p->maxY / 128.0f, -p->maxZ / 128.0f};
	float hi[3] = {p->maxX / 128.0f, -p->minY / 128.0f, -p->minZ / 128.0f};
	float near = 0.0f, far = 1000.0f;
	for (int i = 0; i < 3; i++) {
		if (fabsf(direction[i]) < 0.00001f) {
			if (origin[i] < lo[i] || origin[i] > hi[i]) return false;
			continue;
		}
		float a = (lo[i] - origin[i]) / direction[i], b = (hi[i] - origin[i]) / direction[i];
		if (a > b) {
			float t = a;
			a = b;
			b = t;
		}
		if (a > near) near = a;
		if (b < far) far = b;
		if (near > far) return false;
	}
	*distance = near;
	return true;
}

// Keeps the nearest MAX_HITS results in order.
static void addHit(int* hits, float* distances, int* count, int index, float d) {
	int at = *count < MAX_HITS ? *count : MAX_HITS - 1;
	if (*count == MAX_HITS && d >= distances[at]) return;
	while (at > 0 && distances[at - 1] > d) {
		distances[at] = distances[at - 1];
		hits[at] = hits[at - 1];
		at--;
	}
	distances[at] = d;
	hits[at] = index;
	if (*count < MAX_HITS) (*count)++;
}

static bool entityHit(const float* origin, const float* direction, float ground, const NetNpc* n, int size, int top, float* hit) {
	int half = size * 64, x = (int)(n->viewX * 128.0f), y = (int)(n->viewY * 128.0f);
	int feet = (int)(-groundHeight(net.plane, n->viewX, n->viewY) * 128.0f);
	PickObject box;
	box.minX = (int16_t)(x - half);
	box.maxX = (int16_t)(x + half);
	box.minZ = (int16_t)(y - half);
	box.maxZ = (int16_t)(y + half);
	box.minY = (int16_t)(feet - top);
	box.maxY = (int16_t)feet;
	return rayHitsBox(origin, direction, &box, hit) && *hit <= ground + 0.5f;
}

// Finds the other players under a screen position, nearest first. Results are positions in net.players.
static int pickPlayersAt(int sx, int sy, int width, int* hits) {
	CameraBasis c = cameraBasis(width);
	float origin[3] = {c.eyeX, c.eyeY, c.eyeZ}, direction[3], distances[MAX_HITS];
	viewRay(&c, sx, sy, width, &direction[0], &direction[1], &direction[2]);
	float ground = 1000.0f, hit;
	int tileX, tileY, count = 0;
	pickTileAt(sx, sy, width, net.plane, &tileX, &tileY, &ground);
	for (int i = 0; i < net.playerCount; i++) {
		const NetNpc* n = &net.players[i];
		const NetLook* l = &net.looks[n->index];
		if (!n->placed || !l->valid) continue;
		if (entityHit(origin, direction, ground, n, l->size, n->hasModel ? npcModelTop(n->modelKey) : 200, &hit)) addHit(hits, distances, &count, i, hit);
	}
	return count;
}

// Finds the NPCs under a screen position, nearest first. Results are positions in net.npcs.
static int pickNpcsAt(int sx, int sy, int width, int* hits) {
	CameraBasis c = cameraBasis(width);
	float origin[3] = {c.eyeX, c.eyeY, c.eyeZ}, direction[3], distances[MAX_HITS];
	viewRay(&c, sx, sy, width, &direction[0], &direction[1], &direction[2]);
	float ground = 1000.0f;
	int tileX, tileY, count = 0;
	pickTileAt(sx, sy, width, net.plane, &tileX, &tileY, &ground);
	for (int i = 0; i < net.npcCount; i++) {
		const NetNpc* n = &net.npcs[i];
		const NpcDef* d = npcShown(n->type);
		if (!d || !d->interactive || !n->placed) continue;
		float hit;
		if (entityHit(origin, direction, ground, n, d->size, npcModelTop((u64)d->id), &hit)) addHit(hits, distances, &count, i, hit);
	}
	return count;
}

// Finds the objects under a screen position, nearest first.
static int pickObjectsAt(int sx, int sy, int width, int* hits) {
	CameraBasis c = cameraBasis(width);
	float origin[3] = {c.eyeX, c.eyeY, c.eyeZ}, direction[3], distances[MAX_HITS];
	viewRay(&c, sx, sy, width, &direction[0], &direction[1], &direction[2]);
	float ground = 1000.0f;
	int tileX, tileY, count = 0;
	pickTileAt(sx, sy, width, net.plane, &tileX, &tileY, &ground);
	for (int i = 0; i < pickCount; i++) {
		float d;
		if (pickList[i].hidden || pickList[i].plane != net.plane || !rayHitsBox(origin, direction, &pickList[i], &d) || d > ground + 0.5f) continue;
		addHit(hits, distances, &count, i, d);
	}
	return count;
}

static void swapScreens(void) {
	gameOnBottom = !gameOnBottom;
	panelDirty = true;
}

static void walkTo(int localX, int localY) {
	net_walk(&net, (net.zoneX - 6) * 8 + localX, (net.zoneY - 6) * 8 + localY, runMode);
	actionsSent++;
	walkFlagX = localX;
	walkFlagY = localY;
	panelDirty = true;
}

// ---- action menu ----

enum { ENTRY_PAGE, ENTRY_CAST_NPC, ENTRY_CAST_PLAYER, ENTRY_CAST_LOC, ENTRY_CAST_GROUND, ENTRY_CAST_ITEM, ENTRY_OBJECT, ENTRY_EXAMINE, ENTRY_NPC, ENTRY_NPC_EXAMINE, ENTRY_PLAYER, ENTRY_ITEM, ENTRY_ITEM_USE, ENTRY_ITEM_EXAMINE, ENTRY_USE_NPC, ENTRY_USE_PLAYER, ENTRY_USE_LOC, ENTRY_GROUND, ENTRY_USE_GROUND, ENTRY_WORN, ENTRY_WALK, ENTRY_CANCEL };

typedef struct {
	int kind, object, option, tileX, tileY;
	char label[44];
} MenuEntry;

static MenuEntry menuEntries[MENU_ROWS];
static int menuCount = 0, menuSelected = 0, menuPage = 0;

// With more options than fit, the last row of each page moves to the next one.
static int menuPageSize(void) {
	return menuCount > MENU_VISIBLE ? MENU_VISIBLE - 1 : MENU_VISIBLE;
}

static int menuPageCount(void) {
	return (menuCount + menuPageSize() - 1) / menuPageSize();
}
static bool menuOpen = false;

static void addEntry(int kind, int object, int option, int tileX, int tileY, const char* verb, const char* name) {
	if (menuCount >= MENU_ROWS) return;
	if (menuCount == 0) menuPage = 0;
	MenuEntry* e = &menuEntries[menuCount++];
	e->kind = kind;
	e->object = object;
	e->option = option;
	e->tileX = tileX;
	e->tileY = tileY;
	snprintf(e->label, sizeof(e->label), "%s%s%s", verb, name[0] ? " " : "", name);
}

static void npcLabel(const NpcDef* d, char* out, size_t size) {
	const char* name = d->name ? d->name : "";
	if (d->combatLevel > 0) snprintf(out, size, "%s (level-%d)", name, d->combatLevel);
	else snprintf(out, size, "%s", name);
}

static void playerLabel(const NetLook* l, char* out, size_t size) {
	if (l->skill) snprintf(out, size, "%s (skill-%d)", l->name, l->skill);
	else snprintf(out, size, "%s (level-%d)", l->name, l->combat);
}

// The client lists Attack below Walk here when the target is stronger, so a plain press cannot start a fight by accident.
static bool npcOptionLow(const NpcDef* d, int op) {
	return strcasecmp(d->ops[op], "Attack") == 0 && d->combatLevel > selfCombat();
}

static bool playerOptionLow(const NetLook* l, int option) {
	if (strcasecmp(net.playerOptions[option], "Attack") == 0) return l->combat > selfCombat();
	return net.playerOptionLow[option];
}

// Adds the options of everyone under the stylus that belong above or below Walk here.
static void addPeopleEntries(const int* npcHits, int npcHitCount, const int* playerHits, int playerHitCount, bool low, int limit) {
	char label[40];
	for (int i = 0; i < npcHitCount; i++) {
		const NetNpc* n = &net.npcs[npcHits[i]];
		const NpcDef* d = npcShown(n->type);
		npcLabel(d, label, sizeof(label));
		for (int op = 0; op < 5; op++)
			if (d->ops[op] && npcOptionLow(d, op) == low && menuCount < limit) addEntry(ENTRY_NPC, n->index, op, 0, 0, d->ops[op], label);
	}
	static const bool SENDABLE[NET_PLAYER_OPTIONS] = {true, false, true, true, false, false, true, true};
	for (int i = 0; i < playerHitCount; i++) {
		const NetNpc* n = &net.players[playerHits[i]];
		const NetLook* l = &net.looks[n->index];
		playerLabel(l, label, sizeof(label));
		for (int op = 0; op < NET_PLAYER_OPTIONS; op++)
			if (net.playerOptions[op][0] && SENDABLE[op] && playerOptionLow(l, op) == low && menuCount < limit) addEntry(ENTRY_PLAYER, n->index, op, 0, 0, net.playerOptions[op], label);
	}
}

// Builds the same choices a right-click would show. The first entry is what a plain tap does.
static void buildMenu(int sx, int sy, int width) {
	int hits[MAX_HITS], count = pickObjectsAt(sx, sy, width, hits);
	int npcHits[MAX_HITS], npcHitCount = pickNpcsAt(sx, sy, width, npcHits);
	int playerHits[MAX_HITS], playerHitCount = pickPlayersAt(sx, sy, width, playerHits);
	char label[40];
	int tileX = -1, tileY = -1;
	bool ground = pickTile(sx, sy, width, net.plane, &tileX, &tileY);
	menuCount = 0;
	menuSelected = 0;
	if (castComponent >= 0) {
		// With a spell chosen, only what it can be cast on is offered.
		char verb[40];
		snprintf(verb, sizeof(verb), "Cast %s on", castName);
		if (castMask & 2) {
			for (int i = 0; i < npcHitCount; i++) {
				const NetNpc* n = &net.npcs[npcHits[i]];
				const NpcDef* d = npcShown(n->type);
				addEntry(ENTRY_CAST_NPC, n->index, 0, 0, 0, verb, d->name ? d->name : "");
			}
		}
		if (castMask & 8) {
			for (int i = 0; i < playerHitCount; i++) {
				const NetNpc* n = &net.players[playerHits[i]];
				addEntry(ENTRY_CAST_PLAYER, n->index, 0, 0, 0, verb, net.looks[n->index].name);
			}
		}
		if (castMask & 1) addGroundEntries(sx, sy, width, false, MENU_ROWS - 2);
		if (castMask & 4) {
			for (int i = 0; i < count; i++) {
				const LocDef* d = pickList[hits[i]].def;
				if (d->name) addEntry(ENTRY_CAST_LOC, hits[i], 0, 0, 0, verb, d->name);
			}
		}
		if (ground) addEntry(ENTRY_WALK, -1, 0, tileX, tileY, "Walk here", "");
		addEntry(ENTRY_CANCEL, -1, 0, 0, 0, "Cancel", "");
		return;
	}
	if (usingSlot >= 0) {
		// With an item chosen, everything under the stylus is a target for it.
		char verb[32];
		snprintf(verb, sizeof(verb), "Use %s on", itemName(usingItem));
		for (int i = 0; i < npcHitCount; i++) {
			const NetNpc* n = &net.npcs[npcHits[i]];
			const NpcDef* d = npcShown(n->type);
			addEntry(ENTRY_USE_NPC, n->index, 0, 0, 0, verb, d->name ? d->name : "");
		}
		for (int i = 0; i < playerHitCount; i++) {
			const NetNpc* n = &net.players[playerHits[i]];
			addEntry(ENTRY_USE_PLAYER, n->index, 0, 0, 0, verb, net.looks[n->index].name);
		}
		addGroundEntries(sx, sy, width, false, MENU_ROWS - 2);
		for (int i = 0; i < count; i++) {
			const LocDef* d = pickList[hits[i]].def;
			if (d->name) addEntry(ENTRY_USE_LOC, hits[i], 0, 0, 0, verb, d->name);
		}
		if (ground) addEntry(ENTRY_WALK, -1, 0, tileX, tileY, "Walk here", "");
		addEntry(ENTRY_CANCEL, -1, 0, 0, 0, "Cancel", "");
		return;
	}
	// The last rows are reserved so Walk here and Cancel always fit.
	const int reserved = 2;
	addPeopleEntries(npcHits, npcHitCount, playerHits, playerHitCount, false, MENU_ROWS - reserved);
	addGroundEntries(sx, sy, width, false, MENU_ROWS - reserved);
	for (int i = 0; i < count; i++) {
		const LocDef* d = pickList[hits[i]].def;
		const char* name = d->name ? d->name : "";
		for (int op = 0; op < 5; op++)
			if (d->ops[op] && menuCount < MENU_ROWS - reserved) addEntry(ENTRY_OBJECT, hits[i], op, 0, 0, d->ops[op], name);
	}
	if (ground) addEntry(ENTRY_WALK, -1, 0, tileX, tileY, "Walk here", "");
	addPeopleEntries(npcHits, npcHitCount, playerHits, playerHitCount, true, MENU_ROWS - 1);
	addGroundEntries(sx, sy, width, true, MENU_ROWS - 1);
	for (int i = 0; i < npcHitCount && menuCount < MENU_ROWS - 1; i++) {
		const NpcDef* d = npcShown(net.npcs[npcHits[i]].type);
		npcLabel(d, label, sizeof(label));
		addEntry(ENTRY_NPC_EXAMINE, d->id, 0, 0, 0, "Examine", label);
	}
	for (int i = 0; i < count && menuCount < MENU_ROWS - 1; i++) {
		const LocDef* d = pickList[hits[i]].def;
		addEntry(ENTRY_EXAMINE, hits[i], 0, 0, 0, "Examine", d->name ? d->name : "");
	}
	addEntry(ENTRY_CANCEL, -1, 0, 0, 0, "Cancel", "");
}

static void runEntry(const MenuEntry* e) {
	if (runPage(e)) return;
	if (runCast(e)) return;
	if (usingSlot >= 0 && e->kind != ENTRY_USE_NPC && e->kind != ENTRY_USE_PLAYER && e->kind != ENTRY_USE_LOC && e->kind != ENTRY_USE_GROUND) cancelUse();
	if (e->kind == ENTRY_WORN) {
		int item = wornItemAt(e->object);
		if (item >= 0) {
			net_worn_action(&net, e->option, item, e->object);
			actionsSent++;
		}
	} else if (e->kind == ENTRY_ITEM) {
		int item = bagItemAt(e->object);
		if (item >= 0) {
			net_item_action(&net, e->option, BAG_INTERFACE, item, e->object);
			actionsSent++;
		}
	} else if (e->kind == ENTRY_ITEM_USE) {
		usingSlot = e->object;
		usingItem = bagItemAt(e->object);
		panelDirty = true;
	} else if (e->kind == ENTRY_ITEM_EXAMINE) {
		net_item_examine(&net, e->object);
		actionsSent++;
	} else if (e->kind == ENTRY_USE_NPC) {
		net_use_item_on_npc(&net, BAG_INTERFACE, usingSlot, usingItem, e->object);
		actionsSent++;
		cancelUse();
	} else if (e->kind == ENTRY_USE_PLAYER) {
		net_use_item_on_player(&net, BAG_INTERFACE, usingSlot, usingItem, e->object);
		actionsSent++;
		cancelUse();
	} else if (e->kind == ENTRY_USE_LOC) {
		const PickObject* p = &pickList[e->object];
		net_use_item_on_loc(&net, BAG_INTERFACE, usingSlot, usingItem, p->id, (net.zoneX - 6) * 8 + p->tileX, (net.zoneY - 6) * 8 + p->tileY);
		actionsSent++;
		cancelUse();
	} else if (e->kind == ENTRY_GROUND) {
		net_ground_action(&net, e->option, e->object, (net.zoneX - 6) * 8 + e->tileX, (net.zoneY - 6) * 8 + e->tileY);
		actionsSent++;
	} else if (e->kind == ENTRY_USE_GROUND) {
		net_use_item_on_ground(&net, BAG_INTERFACE, usingSlot, usingItem, e->object, (net.zoneX - 6) * 8 + e->tileX, (net.zoneY - 6) * 8 + e->tileY);
		actionsSent++;
		cancelUse();
	} else if (e->kind == ENTRY_WALK) {
		walkTo(e->tileX, e->tileY);
	} else if (e->kind == ENTRY_OBJECT) {
		const PickObject* p = &pickList[e->object];
		net_loc_action(&net, e->option, p->id, (net.zoneX - 6) * 8 + p->tileX, (net.zoneY - 6) * 8 + p->tileY);
		actionsSent++;
	} else if (e->kind == ENTRY_EXAMINE) {
		net_loc_examine(&net, pickList[e->object].def->id);
		actionsSent++;
	} else if (e->kind == ENTRY_NPC) {
		// It may have left while the menu was open.
		for (int i = 0; i < net.npcCount; i++) {
			if (net.npcs[i].index != e->object) continue;
			net_npc_action(&net, e->option, e->object);
			actionsSent++;
			break;
		}
	} else if (e->kind == ENTRY_PLAYER) {
		for (int i = 0; i < net.playerCount; i++) {
			if (net.players[i].index != e->object) continue;
			if (net_player_action(&net, e->option, e->object)) actionsSent++;
			break;
		}
	} else if (e->kind == ENTRY_NPC_EXAMINE) {
		net_npc_examine(&net, e->object);
		actionsSent++;
	}
}

static void closeMenu(void) {
	menuOpen = false;
	panelDirty = true;
}

static void openMenu(int sx, int sy, int width) {
	if (!online || net.inLobby) return;
	buildMenu(sx, sy, width);
	menuOpen = true;
	panelDirty = true;
}

static void handleMenu(u32 down, touchPosition touch) {
	if (down & KEY_DUP) menuSelected = (menuSelected + menuCount - 1) % menuCount;
	if (down & KEY_DDOWN) menuSelected = (menuSelected + 1) % menuCount;
	if (down & (KEY_DUP | KEY_DDOWN)) {
		menuPage = menuSelected / menuPageSize();
		panelDirty = true;
	}
	if (down & KEY_B) {
		closeMenu();
	} else if (down & KEY_A) {
		runEntry(&menuEntries[menuSelected]);
		closeMenu();
	} else if (down & KEY_TOUCH) {
		int row = (touch.py - MENU_TOP) / MENU_ROW_HEIGHT, entry = menuPage * menuPageSize() + row;
		if (touch.py >= MENU_TOP && menuCount > MENU_VISIBLE && row == MENU_VISIBLE - 1) {
			menuPage = (menuPage + 1) % menuPageCount();
			menuSelected = menuPage * menuPageSize();
			panelDirty = true;
			return;
		}
		if (touch.py >= MENU_TOP && row < menuPageSize() && entry < menuCount) runEntry(&menuEntries[entry]);
		closeMenu();
	}
}

// ---- presses ----

static void pressPanel(int x, int y, int width) {
	if (pressDialog(x, y)) return;
	if (pressPage(x, y, width)) return;
	if (pressGuide(x, y, width)) return;
	if (pressList(x, y, width)) return;
	int combatButton = combatButtonAt(x, y, width);
	if (combatButton >= 0) {
		pressCombat(combatButton);
		return;
	}
	int summonButton = summonButtonAt(x, y, width);
	if (summonButton >= 0) {
		pressSummon(summonButton);
		return;
	}
	int bagSlot = bagSlotAt(x, y, width);
	if (bagSlot >= 0) {
		pressBag(bagSlot);
		return;
	}
	int statsPlace = statsPlaceAt(x, y, width);
	if (statsPlace >= 0) {
		pressStats(statsPlace);
		return;
	}
	int wornSlot = wornSlotAt(x, y, width);
	if (wornSlot >= 0) {
		pressWorn(wornSlot);
		return;
	}
	if (mapVisible() && panelTab == TAB_MAP) {
		int px = x - MAP_CENTRE_X, py = y - MAP_CENTRE_Y;
		if (px * px + py * py <= MAP_RADIUS * MAP_RADIUS) {
			float east, north;
			mapPointToTiles(px, py, &east, &north);
			walkTo((int)floorf(playerViewX + east), (int)floorf(-playerViewZ + north));
			return;
		}
	}
	Widget widgets[MAX_WIDGETS];
	int count = panelLayout(width, widgets);
	for (int i = 0; i < count; i++) {
		const Widget* w = &widgets[i];
		if (x < w->x || y < w->y || x >= w->x + w->w || y >= w->y + w->h) continue;
		if (w->id == WIDGET_PLAY) {
			net_play(&net);
			actionsSent++;
		} else if (w->id == WIDGET_RUN) {
			runMode = !runMode;
		} else if (w->id == WIDGET_DISTANCE) {
			radiusIndex = (radiusIndex + 1) % RADIUS_COUNT;
		} else if (w->id == WIDGET_DECOR) {
			modeIndex = (modeIndex + 1) % MODE_COUNT;
		} else if (w->id == WIDGET_SWAP) {
			swapScreens();
		} else if (w->id == WIDGET_TAB_MAP) {
			panelTab = TAB_MAP;
		} else if (w->id == WIDGET_TAB_CHAT) {
			panelTab = TAB_CHAT;
		} else if (w->id == WIDGET_LOGOUT) {
			pressLogout();
		} else if (w->id == WIDGET_SOCIAL_MSG) {
			pressSocialMessage();
		} else if (w->id == WIDGET_CHAT_SAY) {
			pressChatSay();
		} else if (w->id == WIDGET_CHAT_PRIVATE) {
			pressChatPrivate();
		} else if (w->id == WIDGET_SOCIAL_ADD) {
			pressSocialAdd();
		} else if (w->id == WIDGET_SOCIAL_DEL) {
			pressSocialDelete();
		} else if (w->id >= WIDGET_OPTION0 && w->id < WIDGET_OPTION_END) {
			net_button(&net, 261, OPTION_ROWS[w->id - WIDGET_OPTION0].component);
			actionsSent++;
		} else if (w->id >= WIDGET_TAB0) {
			panelTab = w->id - WIDGET_TAB0;
		}
		panelDirty = true;
		return;
	}
}

// A plain press does whatever the top of the menu would do.
static void pressGame(int x, int y, int width) {
	if (!online || net.inLobby) return;
	buildMenu(x, y, width);
	if (menuEntries[0].kind != ENTRY_CANCEL) runEntry(&menuEntries[0]);
}

static bool touchPending = false;
static long touchStart;

// A press on a bag slot is one of three things, decided by what the stylus does next:
// moving it far enough is a drag, holding it still long enough is the menu, and lifting it early is a tap.
// Once it becomes a drag it can no longer open the menu, and once the menu opens the drag is over.
static void handleBagPress(u32 held, touchPosition touch) {
	if (held & KEY_TOUCH) {
		// The touch screen reports nonsense as pressure fades, so empty samples are ignored
		// and it takes two in a row to start a drag: one bad sample must not turn a tap into a drag.
		if (touch.px == 0 && touch.py == 0) return;
		bagTouchX = touch.px;
		bagTouchY = touch.py;
		for (int i = 0; i < 2; i++) {
			bagHistoryX[i] = bagHistoryX[i + 1];
			bagHistoryY[i] = bagHistoryY[i + 1];
		}
		bagHistoryX[2] = touch.px;
		bagHistoryY[2] = touch.py;
		int dx = bagTouchX - bagStartX, dy = bagTouchY - bagStartY;
		bagFarSamples = dx * dx + dy * dy >= BAG_DRAG_PIXELS * BAG_DRAG_PIXELS ? bagFarSamples + 1 : 0;
		if (bagFarSamples >= 2) bagDragging = true;
		if (bagDragging) {
			panelDirty = true;
		} else if ((u32)nowMs() - (u32)touchStart >= BAG_HOLD_MS) {
			bagPressSlot = -1;
			menuOpen = true;
			panelDirty = true;
		}
		return;
	}
	int from = bagPressSlot;
	bagPressSlot = -1;
	if (bagDragging) {
		bagDragging = false;
		panelDirty = true;
		// The oldest kept position is from before the stylus began to lift, so it is where it was really resting.
		int to = bagSlotAt(bagHistoryX[0], bagHistoryY[0], 320);
		char line[96];
		snprintf(line, sizeof(line), "drag from=%d to=%d rest=%d,%d last=%d,%d item=%d now=%d", from, to, bagHistoryX[0], bagHistoryY[0], bagTouchX, bagTouchY, bagPressItem, bagItemAt(from));
		logLine(line);
		// Dropping it outside the grid or back on itself does nothing, which is how to change your mind.
		if (to >= 0 && to != from && bagItemAt(from) == bagPressItem) {
			net_move_item(&net, BAG_INTERFACE, from, to);
			actionsSent++;
		}
	} else if (menuEntries[0].kind != ENTRY_CANCEL) {
		runEntry(&menuEntries[0]);
	}
}

static void handleInput(u32 down, u32 held, touchPosition touch, circlePosition pad) {
	if (menuOpen) {
		handleMenu(down, touch);
		return;
	}
	if (bagPressSlot >= 0 && (down & KEY_SELECT)) {
		bagPressSlot = -1;
		bagDragging = false;
		panelDirty = true;
	}
	if (down & KEY_SELECT) {
		listTouchCancel();
		swapScreens();
	}
	if ((down & KEY_B) && castComponent >= 0) cancelCast();
	if (held & (KEY_L | KEY_CSTICK_LEFT)) {
		yaw -= 0.03f;
		panelDirty = true;
	}
	if (held & (KEY_R | KEY_CSTICK_RIGHT)) {
		yaw += 0.03f;
		panelDirty = true;
	}

	if (online) {
		if (held & (KEY_DUP | KEY_CSTICK_UP) && zoom > 5.0f) zoom -= 0.2f;
		if (held & (KEY_DDOWN | KEY_CSTICK_DOWN) && zoom < 30.0f) zoom += 0.2f;
		float mx = pad.dx / 156.0f, my = pad.dy / 156.0f;
		if (fabsf(mx) > 0.12f || fabsf(my) > 0.12f) {
			cursorX += mx * 5.0f;
			cursorY -= my * 5.0f;
			cursorX = cursorX < 0 ? 0 : cursorX > 399 ? 399 : cursorX;
			cursorY = cursorY < 0 ? 0 : cursorY > 239 ? 239 : cursorY;
			if (gameOnBottom) panelDirty = true;
		}
		if (down & KEY_A) {
			if (gameOnBottom) pressPanel((int)cursorX, (int)cursorY, 400);
			else pressGame((int)cursorX, (int)cursorY, 400);
		}
		if ((down & KEY_Y) && !gameOnBottom) {
			openMenu((int)cursorX, (int)cursorY, 400);
		} else if (down & KEY_Y) {
			int slot = bagSlotAt((int)cursorX, (int)cursorY, 400);
			int worn = wornSlotAt((int)cursorX, (int)cursorY, 400);
			if (slot >= 0 && bagItemAt(slot) >= 0) {
				buildBagMenu(slot);
				menuOpen = true;
				panelDirty = true;
			} else if (worn >= 0 && wornItemAt(worn) >= 0) {
				buildWornMenu(worn);
				menuOpen = true;
				panelDirty = true;
			}
		}
		if ((down & KEY_B) && usingSlot >= 0) cancelUse();
	}

	if (down & KEY_TOUCH) {
		if (gameOnBottom) {
			// Wait to see whether this is a tap or a long press. The choices are fixed now,
			// so they describe what was under the stylus even if an NPC then walks away.
			touchPending = online && !net.inLobby;
			if (touchPending) buildMenu(touch.px, touch.py, 320);
			touchStart = nowMs();
		} else {
			// A bag slot works like the game view: a tap does the top option and a long press shows them all.
			int slot = bagSlotAt(touch.px, touch.py, 320);
			if (slot >= 0 && usingSlot < 0 && bagItemAt(slot) >= 0) {
				buildBagMenu(slot);
				bagPressSlot = slot;
				bagPressItem = bagItemAt(slot);
				bagDragging = false;
				bagFarSamples = 0;
				bagStartX = bagTouchX = touch.px;
				bagStartY = bagTouchY = touch.py;
				for (int i = 0; i < 3; i++) {
					bagHistoryX[i] = touch.px;
					bagHistoryY[i] = touch.py;
				}
				touchStart = nowMs();
			} else if (wornSlotAt(touch.px, touch.py, 320) >= 0 && wornItemAt(wornSlotAt(touch.px, touch.py, 320)) >= 0) {
				// No dragging here, so this works exactly like the game view.
				buildWornMenu(wornSlotAt(touch.px, touch.py, 320));
				touchPending = true;
				touchStart = nowMs();
			} else if (!listTouchBegin(touch.px, touch.py, 320)) {
				pressPanel(touch.px, touch.py, 320);
			}
		}
	} else if (listTouching()) {
		handleListTouch(held, touch);
	} else if (bagPressSlot >= 0) {
		handleBagPress(held, touch);
	} else if (touchPending) {
		if (!(held & KEY_TOUCH)) {
			touchPending = false;
			if (menuEntries[0].kind != ENTRY_CANCEL) runEntry(&menuEntries[0]);
		} else if ((u32)nowMs() - (u32)touchStart >= LONG_PRESS_MS) {
			touchPending = false;
			menuOpen = true;
			panelDirty = true;
		}
	}
}
