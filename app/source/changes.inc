// Applies the server's object changes, such as a door opening, to the world already on screen.

#define DYNAMIC_POOL_SIZE (2u * 1024 * 1024)
#define MAX_DYNAMIC 256
#define MAX_HIDDEN 1024

static const u8 SHAPE_LAYERS[23] = {0,0,0,0,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,3};

typedef struct {
	const MeshObject* object;
	u16* saved[2];
} HiddenObject;

static HiddenObject hiddenObjects[MAX_HIDDEN];
static int hiddenCount = 0;
static u8* dynamicPool = NULL;
static size_t dynamicUsed = 0;

static void resetChanges(void) {
	for (int i = 0; i < hiddenCount; i++) {
		free(hiddenObjects[i].saved[0]);
		free(hiddenObjects[i].saved[1]);
	}
	hiddenCount = 0;
	dynamicCount = 0;
	dynamicUsed = 0;
}

// A bridge moves a tile down one level, so the server's plane and the stored level can differ.
static int actualLevel(int plane, int x, int y) {
	return scene.terrain.flags[1][x][y] & 2 ? plane + 1 : plane;
}

static void hideObject(const MeshObject* o) {
	if (hiddenCount >= MAX_HIDDEN) return;
	const MeshChunk* chunk = &mesh.chunks[o->chunk];
	HiddenObject* h = &hiddenObjects[hiddenCount++];
	h->object = o;
	for (int s = 0; s < 2; s++) {
		size_t bytes = (size_t)o->count[s] * 2;
		h->saved[s] = NULL;
		if (bytes == 0) continue;
		u16* range = chunk->indices[s] + o->start[s];
		h->saved[s] = malloc(bytes);
		if (h->saved[s]) memcpy(h->saved[s], range, bytes);
		memset(range, 0, bytes); // every triangle collapses to a point and draws nothing
		meshMarkChanged(range, bytes);
		GSPGPU_FlushDataCache(range, (u32)bytes);
	}
}

static void restoreHidden(int index) {
	HiddenObject* h = &hiddenObjects[index];
	const MeshObject* o = h->object;
	const MeshChunk* chunk = &mesh.chunks[o->chunk];
	for (int s = 0; s < 2; s++) {
		if (!h->saved[s]) continue;
		u16* range = chunk->indices[s] + o->start[s];
		memcpy(range, h->saved[s], (size_t)o->count[s] * 2);
		meshMarkChanged(range, (size_t)o->count[s] * 2);
		GSPGPU_FlushDataCache(range, (u32)o->count[s] * 2);
		free(h->saved[s]);
	}
	*h = hiddenObjects[--hiddenCount];
}

static bool alreadyHidden(const MeshObject* o) {
	for (int i = 0; i < hiddenCount; i++) if (hiddenObjects[i].object == o) return true;
	return false;
}

static void removeAt(int level, int x, int y, int layer) {
	for (u32 i = 0; i < mesh.objectCount; i++) {
		const MeshObject* o = &mesh.objects[i];
		if (o->level == level && o->x == x && o->y == y && o->layer == layer && !alreadyHidden(o)) hideObject(o);
	}
	for (int i = 0; i < dynamicCount; i++) {
		DynamicObject* d = &dynamicObjects[i];
		if (d->level == level && d->x == x && d->y == y && d->layer == layer) d->vertexCount = 0;
	}
	for (int i = 0; i < pickCount; i++) {
		PickObject* p = &pickList[i];
		if (p->level == level && p->tileX == x && p->tileY == y && p->layer == layer) p->hidden = 1;
	}
}

static void addAt(int id, int level, int plane, int x, int y, int shape, int rotation) {
	if (dynamicCount >= MAX_DYNAMIC) return;
	LocPlacement placement = {id, shape, rotation, level, x, y};
	scene_add_loc(&scene, &placement);
	scene_merge(&scene);
	scene_resolve_dynamic(&scene);
	size_t room = (DYNAMIC_POOL_SIZE - dynamicUsed) / MESH_VERTEX_SIZE;
	u8* vertices = dynamicPool + dynamicUsed;
	u32 count = (u32)mesh_emit_tile(&scene, world.palette, level, x, y, vertices, room);
	if (count) {
		GSPGPU_FlushDataCache(vertices, count * MESH_VERTEX_SIZE);
		dynamicUsed += ((size_t)count * MESH_VERTEX_SIZE + 15) & ~(size_t)15;
		dynamicObjects[dynamicCount++] = (DynamicObject){vertices, count, level, x, y, SHAPE_LAYERS[shape]};
	}
	if (pickCount < MAX_PICK_OBJECTS) {
		int added = pick_build(&scene, pickList + pickCount, MAX_PICK_OBJECTS - pickCount);
		for (int i = 0; i < added; i++) pickList[pickCount + i].dynamic = 1;
		pickCount += added;
	}
	(void)plane;
	scene_free(&scene);
}

// The server is about to resend this chunk, so it returns to how the map files describe it.
static void resetChunk(int chunkX, int chunkY) {
	for (int i = hiddenCount - 1; i >= 0; i--) {
		const MeshObject* o = hiddenObjects[i].object;
		if (o->x >= chunkX && o->x < chunkX + 8 && o->y >= chunkY && o->y < chunkY + 8) restoreHidden(i);
	}
	for (int i = 0; i < dynamicCount; i++) {
		DynamicObject* d = &dynamicObjects[i];
		if (d->x >= chunkX && d->x < chunkX + 8 && d->y >= chunkY && d->y < chunkY + 8) d->vertexCount = 0;
	}
	for (int i = 0; i < pickCount; i++) {
		PickObject* p = &pickList[i];
		if (p->tileX >= chunkX && p->tileX < chunkX + 8 && p->tileY >= chunkY && p->tileY < chunkY + 8) p->hidden = p->dynamic;
	}
}

static void applyChanges(void) {
	NetLocChange c;
	while (net_next_change(&net, &c)) {
		if (c.kind == NET_CHANGE_RESET) {
			resetChunk(c.x, c.y);
			continue;
		}
		if (c.x >= SCENE_SIZE || c.y >= SCENE_SIZE || c.shape > 22) continue;
		int level = actualLevel(c.plane, c.x, c.y);
		if (level >= SCENE_LEVELS) continue;
		removeAt(level, c.x, c.y, SHAPE_LAYERS[c.shape]);
		if (c.kind == NET_CHANGE_ADD) addAt(c.id, level, c.plane, c.x, c.y, c.shape, c.rotation);
		changesApplied++;
		panelDirty = true;
	}
}
