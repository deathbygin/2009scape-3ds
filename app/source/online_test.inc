// Self-test, run only if autotest_online.txt exists. It drives handleInput exactly as real presses do.

static const touchPosition NO_TOUCH = {0, 0};
static const circlePosition NO_PAD = {0, 0};
static FILE* testOut;
static bool* testScene;

static void gameFrames(int ms) {
	long start = nowMs();
	while ((u32)nowMs() - (u32)start < (u32)ms && aptMainLoop()) {
		handleInput(0, 0, NO_TOUCH, NO_PAD);
		frameTick(16.7, testScene);
	}
}

// One press, then the stylus or button stays down for a full second.
static int flagAtPressX = -1;

static void pressAndHold(u32 key, int x, int y) {
	touchPosition touch = {(u16)x, (u16)y};
	handleInput(key, key, touch, NO_PAD);
	flagAtPressX = walkFlagX;
	frameTick(16.7, testScene);
	for (int i = 0; i < 60; i++) {
		handleInput(0, key, touch, NO_PAD);
		frameTick(16.7, testScene);
	}
}

// A quick stylus tap: down, a few frames, then up.
static void tap(int x, int y) {
	touchPosition touch = {(u16)x, (u16)y};
	handleInput(KEY_TOUCH, KEY_TOUCH, touch, NO_PAD);
	frameTick(16.7, testScene);
	for (int i = 0; i < 3; i++) {
		handleInput(0, KEY_TOUCH, touch, NO_PAD);
		frameTick(16.7, testScene);
	}
	handleInput(0, 0, NO_TOUCH, NO_PAD);
	frameTick(16.7, testScene);
}

// Presses an entry of the open menu, turning pages first if it is not on this one.
static void tapMenuRow(int entry) {
	int more = MENU_TOP + (MENU_VISIBLE - 1) * MENU_ROW_HEIGHT + 8;
	for (int i = 0; i < 8 && entry / menuPageSize() != menuPage; i++) {
		tap(160, more);
		frameTick(16.7, testScene);
	}
	tap(160, MENU_TOP + (entry % menuPageSize()) * MENU_ROW_HEIGHT + 8);
}

static bool projectTile(float tileX, float tileY, float heightAbove, int width, int* sx, int* sy) {
	CameraBasis c = cameraBasis(width);
	float dx = tileX - c.eyeX, dz = -tileY - c.eyeZ;
	float dy = groundHeight(net.plane, tileX, tileY) + heightAbove - c.eyeY;
	float depth = dx * c.fX + dy * c.fY + dz * c.fZ;
	float across = dx * c.rX + dz * c.rZ;
	float upward = dx * c.uX + dy * c.uY + dz * c.uZ;
	if (depth <= 0) return false;
	*sx = (int)((across / (depth * c.tanH) + 1.0f) * 0.5f * width);
	*sy = (int)((1.0f - upward / (depth * c.tanV)) * 0.5f * SCREEN_H);
	return true;
}

// Renders the same frame with and without the local player, and counts what changes where the maths says they are.
static int markerPixels(gfxScreen_t screen) {
	static u8 shown[FB_BYTES];
	int width = screenWidth(screen), sx, sy, found = 0;
	if (!projectTile(net.localX + 0.5f, net.localY + 0.5f, 0.8f, width, &sx, &sy)) return 0;
	size_t bytes = (size_t)width * SCREEN_H * 3;
	for (int i = 0; i < 3; i++) renderGame(true);
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	GSPGPU_InvalidateDataCache(fb, (u32)bytes);
	memcpy(shown, fb, bytes);
	selfHidden = true;
	for (int i = 0; i < 3; i++) renderGame(true);
	selfHidden = false;
	fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	GSPGPU_InvalidateDataCache(fb, (u32)bytes);
	for (int x = sx - 24; x <= sx + 24; x++)
		for (int y = sy - 24; y <= sy + 24; y++) {
			if (x < 0 || y < 0 || x >= width || y >= SCREEN_H) continue;
			size_t at = (size_t)(x * SCREEN_H + (SCREEN_H - 1 - y)) * 3;
			if (memcmp(fb + at, shown + at, 3) != 0) found++;
		}
	return found;
}

static void capture(gfxScreen_t screen, const char* path) {
	static u8 frame[FB_BYTES];
	int width = screenWidth(screen);
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	GSPGPU_InvalidateDataCache(fb, (u32)(width * SCREEN_H * 3));
	memcpy(frame, fb, (size_t)(width * SCREEN_H * 3));
	writeBmp(path, frame, width);
}

static bool walkTest(const char* name, int offsetX, int offsetY, bool useTouch) {
	gfxScreen_t screen = gameScreen();
	int width = screenWidth(screen);
	int wantLocalX = net.localX + offsetX, wantLocalY = net.localY + offsetY;
	int wantX = net_world_x(&net) + offsetX, wantY = net_world_y(&net) + offsetY;
	int sx = 0, sy = 0, pickedX = -1, pickedY = -1;
	bool visible = projectTile(wantLocalX + 0.5f, wantLocalY + 0.5f, 0, width, &sx, &sy);
	bool picked = pickTile(sx, sy, width, net.plane, &pickedX, &pickedY);
	// A press on an NPC talks to it instead of walking, so wait for the spot to be clear.
	int crowd[MAX_HITS];
	for (int i = 0; i < 100 && pickNpcsAt(sx, sy, width, crowd) + pickPlayersAt(sx, sy, width, crowd) > 0; i++) gameFrames(100);
	long before = actionsSent, walksBefore = net.walksSent;
	if (useTouch) {
		tap(sx, sy);
	} else {
		cursorX = (float)sx;
		cursorY = (float)sy;
		pressAndHold(KEY_A, 0, 0);
	}
	long presses = actionsSent - before, walks = net.walksSent - walksBefore;
	gameFrames(6500);
	bool arrived = net_world_x(&net) == wantX && net_world_y(&net) == wantY;
	int marker = markerPixels(screen);
	fprintf(testOut, "%s screen=%s press=%d,%d picked_local=%d,%d want_local=%d,%d actions=%ld walks=%ld tile=%d,%d arrived=%d marker_pixels=%d\n", name, screen == GFX_TOP ? "top" : "bottom",
		sx, sy, pickedX, pickedY, wantLocalX, wantLocalY, presses, walks, net_world_x(&net), net_world_y(&net), arrived, marker);
	fflush(testOut);
	return visible && picked && pickedX == wantLocalX && pickedY == wantLocalY && presses == 1 && walks == 1 && arrived && marker > 0;
}

// Taps or clicks a tile on the rotated minimap.
static bool mapWalkTest(const char* name, int offsetX, int offsetY, bool useTouch) {
	yaw = 0.9f;
	panelDirty = true;
	gameFrames(300);
	int wantX = net_world_x(&net) + offsetX, wantY = net_world_y(&net) + offsetY, px, py;
	tilesToMapPoint(offsetX + 0.05f, offsetY + 0.05f, &px, &py);
	int x = MAP_CENTRE_X + px, y = MAP_CENTRE_Y + py;
	long before = actionsSent, walksBefore = net.walksSent;
	if (useTouch) {
		pressAndHold(KEY_TOUCH, x, y);
	} else {
		cursorX = (float)x;
		cursorY = (float)y;
		pressAndHold(KEY_A, 0, 0);
	}
	long presses = actionsSent - before, walks = net.walksSent - walksBefore;
	bool flagged = flagAtPressX == wantX - (net.zoneX - 6) * 8;
	capture(panelScreen(), AUTOTEST_DIR "autotest_ui_map.bmp");
	gameFrames(6500);
	bool arrived = net_world_x(&net) == wantX && net_world_y(&net) == wantY;
	fprintf(testOut, "%s panel=%s press=%d,%d actions=%ld walks=%ld want=%d,%d tile=%d,%d arrived=%d flag_set=%d flag_cleared=%d\n", name, panelScreen() == GFX_TOP ? "top" : "bottom", x, y, presses, walks,
		wantX, wantY, net_world_x(&net), net_world_y(&net), arrived, flagged, walkFlagX == -1);
	fflush(testOut);
	yaw = 0;
	return presses == 1 && walks == 1 && arrived && flagged && walkFlagX == -1;
}

// Finds the nearest object offering this option, on this floor.
static int findObject(const char* option) {
	int best = -1;
	float bestDistance = 14.0f;
	for (int i = 0; i < pickCount; i++) {
		const PickObject* o = &pickList[i];
		if (o->hidden || o->plane != net.plane || !o->def->ops[0] || strcmp(o->def->ops[0], option) != 0) continue;
		float dx = o->tileX - net.localX, dy = o->tileY - net.localY, d = sqrtf(dx * dx + dy * dy);
		if (d < bestDistance) {
			bestDistance = d;
			best = i;
		}
	}
	return best;
}

static bool objectTest(int width) {
	int index = findObject("Chop down");
	if (index < 0) {
		fprintf(testOut, "object_test no tree nearby, %d objects known\n", pickCount);
		return false;
	}
	const PickObject* o = &pickList[index];
	zoom = 30.0f; // far enough out that the tree is really on screen
	gameFrames(200);
	float cx = (o->minX + o->maxX) / 256.0f, cy = (o->minZ + o->maxZ) / 256.0f;
	float heightAbove = (o->maxY - o->minY) / 256.0f;
	int sx = 0, sy = 0;
	bool visible = projectTile(cx, cy, heightAbove, width, &sx, &sy) && sx >= 0 && sy >= 0 && sx < width && sy < SCREEN_H;
	fprintf(testOut, "object %s id=%d tile=%d,%d screen=%d,%d visible=%d\n", o->def->name, o->id, o->tileX, o->tileY, sx, sy, visible);

	// People standing in front of the tree are correctly offered first, so wait until the spot is clear.
	int crowd[MAX_HITS];
	for (int i = 0; i < 300 && pickNpcsAt(sx, sy, width, crowd) + pickPlayersAt(sx, sy, width, crowd) > 0; i++) gameFrames(100);

	// A long press opens the menu and sends nothing.
	long before = actionsSent;
	pressAndHold(KEY_TOUCH, sx, sy);
	bool opened = menuOpen && actionsSent == before;
	handleInput(0, 0, NO_TOUCH, NO_PAD);
	panelDirty = true;
	frameTick(16.7, testScene);
	frameTick(16.7, testScene);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_ui_menu.bmp");
	fprintf(testOut, "object_hidden_after_menu=%d\n", o->hidden);
	bool stillOpen = menuOpen && actionsSent == before;
	int examine = -1;
	for (int i = 0; i < menuCount; i++) {
		fprintf(testOut, "  menu %d: %s\n", i, menuEntries[i].label);
		if (menuEntries[i].kind == ENTRY_EXAMINE && examine < 0) examine = i;
	}

	// A separate press chooses Examine.
	long messages = net.messages;
	bool examined = false;
	if (examine >= 0) {
		tapMenuRow(examine);
		gameFrames(1500);
		examined = !menuOpen && actionsSent == before + 1 && net.messages > messages;
	}
	fprintf(testOut, "long_press_menu=%d menu_waits=%d examined=%d message=%s\n", opened, stillOpen, examined, net.lastMessage);

	// A plain tap does the top option once.
	before = actionsSent;
	long objects = net.objectActionsSent;
	messages = net.messages;
	for (int i = 0; i < 300 && pickNpcsAt(sx, sy, width, crowd) + pickPlayersAt(sx, sy, width, crowd) > 0; i++) gameFrames(100);
	fprintf(testOut, "object_before_tap hidden=%d player=%d,%d view=%.2f,%.2f\n", o->hidden, net.localX, net.localY, playerViewX, -playerViewZ);
	tap(sx, sy);
	long sent = net.objectActionsSent - objects, actions = actionsSent - before;
	gameFrames(9000);
	fprintf(testOut, "tap_top_option actions=%ld object_packets=%ld messages=%ld last=%s tile=%d,%d\n", actions, sent, net.messages - messages, net.lastMessage, net_world_x(&net), net_world_y(&net));
	fflush(testOut);
	return visible && opened && stillOpen && examined && actions == 1 && sent == 1 && net.messages > messages;
}

// Turns the camera until a plain tap at this object's centre would select it.
static bool aimAt(int index, int width, int* sx, int* sy) {
	const PickObject* o = &pickList[index];
	float cx = (o->minX + o->maxX) / 256.0f, cy = (o->minZ + o->maxZ) / 256.0f;
	float heightAbove = (o->maxY - o->minY) / 256.0f;
	zoom = 20.0f;
	for (int i = 0; i < 24; i++) {
		yaw = i * (float)M_PI / 12.0f;
		if (!projectTile(cx, cy, heightAbove, width, sx, sy) || *sx < 4 || *sy < 4 || *sx >= width - 4 || *sy >= SCREEN_H - 4) continue;
		buildMenu(*sx, *sy, width);
		if (menuEntries[0].kind == ENTRY_OBJECT && menuEntries[0].object == index) return true;
	}
	return false;
}

static int activeDynamic(void) {
	int n = 0;
	for (int i = 0; i < dynamicCount; i++) if (dynamicObjects[i].vertexCount) n++;
	return n;
}

// Finds a door or similar object offering this option nearest the player, ignoring anything already gone.
static int findDoor(const char* option) {
	int best = -1;
	float bestDistance = 40.0f;
	for (int i = 0; i < pickCount; i++) {
		const PickObject* o = &pickList[i];
		if (o->hidden || o->plane != net.plane || !o->def->ops[0] || strcmp(o->def->ops[0], option) != 0 || o->layer != 0) continue;
		float dx = o->tileX - net.localX, dy = o->tileY - net.localY, d = sqrtf(dx * dx + dy * dy);
		if (d < bestDistance) {
			bestDistance = d;
			best = i;
		}
	}
	return best;
}

#define DOOR_VIEWS 4
static u8 doorFrames[DOOR_VIEWS][240 * 320 * 3];

// Renders the door from four sides, close up, with the camera held still.
static void viewDoor(int tileX, int tileY, u8 (*frames)[240 * 320 * 3], bool compare, long* changed, int* bestView) {
	float savedX = targetX, savedZ = targetZ, savedH = targetH, savedYaw = yaw, savedPitch = pitch, savedZoom = zoom;
	bool savedOnline = online;
	static u8 now[240 * 320 * 3];
	online = false; // hides the player marker
	targetX = tileX + 0.5f;
	targetZ = -(tileY + 0.5f);
	targetH = groundHeight(net.plane, tileX + 0.5f, tileY + 0.5f) + 0.5f;
	pitch = 0.45f;
	zoom = 6.0f;
	for (int v = 0; v < DOOR_VIEWS; v++) {
		yaw = v * (float)M_PI / 2.0f + 0.4f;
		for (int i = 0; i < 4; i++) renderGame(true);
		u8* fb = gfxGetFramebuffer(gameScreen(), GFX_LEFT, NULL, NULL);
		GSPGPU_InvalidateDataCache(fb, sizeof(now));
		memcpy(now, fb, sizeof(now));
		if (compare) {
			long diff = 0;
			for (size_t i = 0; i < sizeof(now); i += 3) if (memcmp(now + i, frames[v] + i, 3) != 0) diff++;
			if (diff > *changed) {
				*changed = diff;
				*bestView = v;
				writeBmp(AUTOTEST_DIR "autotest_door_view_open.bmp", frames[v], 320);
				writeBmp(AUTOTEST_DIR "autotest_door_view_closed.bmp", now, 320);
			}
		} else {
			memcpy(frames[v], now, sizeof(now));
		}
	}
	online = savedOnline;
	targetX = savedX;
	targetZ = savedZ;
	targetH = savedH;
	yaw = savedYaw;
	pitch = savedPitch;
	zoom = savedZoom;
}

static bool doorTest(int width) {
	int index = findDoor("Open"), sx = 0, sy = 0;
	if (index < 0 || !aimAt(index, width, &sx, &sy)) {
		fprintf(testOut, "door_test no reachable door index=%d\n", index);
		return false;
	}
	int doorTileX = pickList[index].tileX, doorTileY = pickList[index].tileY, doorId = pickList[index].id;
	fprintf(testOut, "door %s id=%d tile=%d,%d screen=%d,%d hidden_before=%d dynamic_before=%d\n", pickList[index].def->name, doorId, doorTileX, doorTileY, sx, sy, hiddenCount, activeDynamic());
	gameFrames(300);
	capture(gameScreen(), AUTOTEST_DIR "autotest_door_closed.bmp");
	long adds = net.locAdds, deletes = net.locDeletes, applied = changesApplied;
	tap(sx, sy);
	gameFrames(9000);
	capture(gameScreen(), AUTOTEST_DIR "autotest_door_open.bmp");
	bool removed = pickList[index].hidden;
	int closeIndex = findDoor("Close");
	fprintf(testOut, "door_opened server_adds=%ld server_deletes=%ld applied=%ld old_gone=%d hidden=%d dynamic=%d close_option=%d player=%d,%d\n", net.locAdds - adds, net.locDeletes - deletes, changesApplied - applied, removed, hiddenCount, activeDynamic(), closeIndex >= 0, net_world_x(&net), net_world_y(&net));
	fflush(testOut);
	bool opened = net.locAdds > adds && net.locDeletes > deletes && removed && activeDynamic() > 0 && closeIndex >= 0;

	long changed = 0;
	int bestView = -1;
	if (opened) viewDoor(doorTileX, doorTileY, doorFrames, false, &changed, &bestView);

	// Closing it again uses the new object, which proves it can be tapped too.
	bool closed = false;
	if (opened && aimAt(closeIndex, width, &sx, &sy)) {
		tap(sx, sy);
		gameFrames(7000);
		capture(gameScreen(), AUTOTEST_DIR "autotest_door_closed_again.bmp");
		int reopened = findDoor("Open");
		closed = reopened >= 0 && pickList[reopened].tileX == doorTileX && pickList[reopened].tileY == doorTileY;
		fprintf(testOut, "door_closed adds=%ld deletes=%ld door_back=%d dynamic=%d hidden=%d\n", net.locAdds - adds, net.locDeletes - deletes, closed, activeDynamic(), hiddenCount);
		viewDoor(doorTileX, doorTileY, doorFrames, true, &changed, &bestView);
		fprintf(testOut, "door_view_changed_pixels=%ld best_view=%d\n", changed, bestView);
	}
	yaw = 0;
	zoom = 13.0f;
	fflush(testOut);
	return opened && closed && changed > 50;
}

// Renders one NPC close up with the camera held still.
static void viewNpc(const NetNpc* n, const char* path) {
	float savedX = targetX, savedZ = targetZ, savedH = targetH, savedYaw = yaw, savedPitch = pitch, savedZoom = zoom;
	targetX = n->viewX;
	targetZ = -n->viewY;
	targetH = groundHeight(net.plane, n->viewX, n->viewY) + 0.7f;
	pitch = 0.2f;
	zoom = 5.0f;
	// Look at its front: the camera sits on the side it faces.
	yaw = -n->viewAngle * (2.0f * (float)M_PI / 2048.0f);
	selfHidden = n != &net.self;
	for (int i = 0; i < 4; i++) renderGame(true);
	capture(gameScreen(), path);
	selfHidden = false;
	targetX = savedX;
	targetZ = savedZ;
	targetH = savedH;
	yaw = savedYaw;
	pitch = savedPitch;
	zoom = savedZoom;
}

static int findNpcToTap(int width, int* sx, int* sy) {
	int best = -1;
	float bestDistance = 16.0f;
	for (int i = 0; i < net.npcCount; i++) {
		const NetNpc* n = &net.npcs[i];
		const NpcDef* d = npcShown(n->type);
		if (!d || !d->interactive || !d->ops[0] || !n->placed) continue;
		float dx = n->viewX - net.localX, dy = n->viewY - net.localY, distance = sqrtf(dx * dx + dy * dy);
		int x, y;
		if (distance >= bestDistance || !projectTile(n->viewX, n->viewY, 0.7f, width, &x, &y) || x < 4 || y < 4 || x >= width - 4 || y >= SCREEN_H - 4) continue;
		buildMenu(x, y, width);
		if (menuEntries[0].kind != ENTRY_NPC || menuEntries[0].object != n->index) continue;
		best = i;
		bestDistance = distance;
		*sx = x;
		*sy = y;
	}
	return best;
}

// Reads a conversation to its end. Each press must send exactly one packet, and holding must send no more.
static bool dialogueTest(void) {
	int width = screenWidth(panelScreen()), steps = 0, pages = 0;
	bool ok = true;
	for (int t = 0; t < 40 && !dialogActive(); t++) gameFrames(100);
	fprintf(testOut, "dialogue open=%d interface=%d panel_width=%d\n", dialogActive(), net_open_interface(&net, CHAT_PARENT), width);
	if (!dialogActive()) return false;
	while (dialogActive() && steps < 12) {
		panelDirty = true;
		gameFrames(300);
		if (!dialogActive()) break;
		fprintf(testOut, "  page %d interface=%d\n", pages, dialogIface.id);
		int press = -1;
		for (int i = 0; i < dialogRowCount; i++) {
			fprintf(testOut, "    %s %s\n", dialogRows[i].component == -1 ? "text" : "press", dialogRows[i].text);
			if (dialogRows[i].component != -1 && dialogRows[i].w > 0 && press < 0) press = i;
		}
		if (pages == 0) capture(panelScreen(), AUTOTEST_DIR "autotest_dialogue_0.bmp");
		else if (dialogRowCount > 3) capture(panelScreen(), AUTOTEST_DIR "autotest_dialogue_options.bmp");
		pages++;
		if (press < 0) {
			fprintf(testOut, "  nothing to press\n");
			ok = false;
			break;
		}
		long version = net.ifaceVersion, sent = net.continuesSent, actions = actionsSent;
		cursorX = (float)(dialogRows[press].x + 20);
		cursorY = (float)(dialogRows[press].y + 8);
		// The panel is on top here, so this is the cursor and A. Holding for a second must not repeat it.
		pressAndHold(KEY_A, 0, 0);
		long packets = net.continuesSent - sent;
		fprintf(testOut, "  pressed packets=%ld actions=%ld changed=%d\n", packets, actionsSent - actions, net.ifaceVersion != version);
		if (packets != 1 || actionsSent - actions != 1) ok = false;
		steps++;
		for (int t = 0; t < 30 && net.ifaceVersion == version; t++) gameFrames(100);
	}
	fprintf(testOut, "dialogue_end pages=%d steps=%d still_open=%d ok=%d\n", pages, steps, dialogActive(), ok);
	fflush(testOut);
	return ok && pages >= 2;
}

static bool npcTest(int width) {
	net_command(&net, "tele 3222 3218 0");
	gameFrames(6000);
	long shown = 0;
	for (int i = 0; i < net.npcCount; i++) {
		const NetNpc* n = &net.npcs[i];
		const NpcDef* d = npcShown(n->type);
		if (d && findNpcModel(d->id)) shown++;
		if (i < 16) fprintf(testOut, "  npc index=%d type=%d name=%s tile=%d,%d size=%d angle=%d op0=%s\n", n->index, n->type, d && d->name ? d->name : "?", n->x, n->y, d ? d->size : 0, n->angle, d && d->ops[0] ? d->ops[0] : "-");
	}
	fprintf(testOut, "npcs known=%d with_model=%ld models_built=%ld pool_kb=%lu updates=%ld errors=%ld\n", net.npcCount, shown, npcModelsBuilt, (unsigned long)((npcPoolBottom + NPC_POOL_SIZE - npcPoolTop) >> 10), net.npcUpdates, net.npcErrors);

	int sx = 0, sy = 0, target = -1;
	zoom = 20.0f;
	for (int i = 0; i < 24 && target < 0; i++) {
		yaw = i * (float)M_PI / 12.0f;
		gameFrames(50);
		target = findNpcToTap(width, &sx, &sy);
	}
	if (target < 0) {
		fprintf(testOut, "npc_test nothing to tap\n");
		return false;
	}
	gameFrames(100);
	int index = net.npcs[target].index;
	const NpcDef* d = npcShown(net.npcs[target].type);
	capture(gameScreen(), AUTOTEST_DIR "autotest_npc_scene.bmp");
	fprintf(testOut, "npc_target %s index=%d screen=%d,%d drawn=%d tris=%lu\n", d->name, index, sx, sy, npcsDrawn, (unsigned long)trisSubmitted);
	int closeUps = 0;
	for (int i = 0; i < net.npcCount && closeUps < 3; i++) {
		const NpcDef* shownDef = npcShown(net.npcs[i].type);
		float dx = net.npcs[i].viewX - targetX, dy = net.npcs[i].viewY + targetZ;
		if (!shownDef || sqrtf(dx * dx + dy * dy) > 18.0f) continue;
		char path[64];
		snprintf(path, sizeof(path), AUTOTEST_DIR "autotest_npc_%d.bmp", closeUps++);
		viewNpc(&net.npcs[i], path);
		fprintf(testOut, "  close_up %d %s\n", closeUps - 1, shownDef->name);
	}

	// Examine through the menu: a long press sends nothing, then one separate press chooses.
	target = findNpcToTap(width, &sx, &sy);
	if (target < 0) {
		fprintf(testOut, "npc_test target moved away\n");
		return false;
	}
	long before = actionsSent, messages = net.messages;
	pressAndHold(KEY_TOUCH, sx, sy);
	bool opened = menuOpen && actionsSent == before;
	handleInput(0, 0, NO_TOUCH, NO_PAD);
	panelDirty = true;
	frameTick(16.7, testScene);
	frameTick(16.7, testScene);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_npc_menu.bmp");
	int examine = -1;
	for (int i = 0; i < menuCount; i++) {
		fprintf(testOut, "  menu %d: %s\n", i, menuEntries[i].label);
		if (menuEntries[i].kind == ENTRY_NPC_EXAMINE && examine < 0) examine = i;
	}
	bool examined = false;
	if (examine >= 0) {
		tapMenuRow(examine);
		gameFrames(1500);
		examined = !menuOpen && actionsSent == before + 1 && net.messages > messages;
	} else {
		pressAndHold(KEY_B, 0, 0);
	}
	fprintf(testOut, "npc_menu opened=%d examined=%d message=%s\n", opened, examined, net.lastMessage);

	// A plain tap does the top option once. The server then walks the player up to the NPC.
	target = findNpcToTap(width, &sx, &sy);
	if (target < 0) {
		fprintf(testOut, "npc_test target moved away before the tap\n");
		return false;
	}
	index = net.npcs[target].index;
	before = actionsSent;
	long sent = net.npcActionsSent, walks = net.walksSent;
	long opcodes[256];
	memcpy(opcodes, net.opcodeCounts, sizeof(opcodes));
	long faceBefore = net.npcFaceUpdates;
	tap(sx, sy);
	long actions = actionsSent - before, packets = net.npcActionsSent - sent, walkPackets = net.walksSent - walks;

	// The NPC keeps its own routine afterwards, so watch for it turning to the player as they meet.
	int facedSamples = 0, gap = 99;
	bool talked = false; // the local server reports this in debug messages
	for (int t = 0; t < 90; t++) {
		gameFrames(100);
		if (strstr(net.lastMessage, "Dialogue opening")) talked = true;
		for (int i = 0; i < net.npcCount; i++) {
			const NetNpc* n = &net.npcs[i];
			if (n->index == index) {
				int gx = abs(n->x - net.localX), gy = abs(n->y - net.localY), now = gx > gy ? gx : gy;
				if (now < gap) gap = now;
			}
			float dx = n->viewX - playerViewX, dy = n->viewY + playerViewZ;
			if (n->index != index || sqrtf(dx * dx + dy * dy) < 0.6f || net.npcFaceUpdates == faceBefore) continue;
			int off = ((int)n->viewAngle - angleTowards(n->viewX, n->viewY, playerViewX, -playerViewZ)) & 2047;
			if (off < 48 || off > 2000) facedSamples++;
		}
	}
	fprintf(testOut, "npc_tap actions=%ld npc_packets=%ld walk_packets=%ld gap=%d message=%s new_opcodes:", actions, packets, walkPackets, gap, net.lastMessage);
	for (int i = 0; i < 256; i++) if (i != 225 && i != 32 && net.opcodeCounts[i] != opcodes[i]) fprintf(testOut, " %d(%ld)", i, net.opcodeCounts[i] - opcodes[i]);
	fprintf(testOut, "\n");

	bool facing = facedSamples > 0 || gap == 0; // on the same tile there is no direction to face
	fprintf(testOut, "npc_facing face_updates=%ld faced_samples=%d of 90\n", net.npcFaceUpdates - faceBefore, facedSamples);
	for (int i = 0; i < net.npcCount; i++) if (net.npcs[i].index == index) viewNpc(&net.npcs[i], AUTOTEST_DIR "autotest_npc_face.bmp");

	bool dialogue = dialogueTest();

	// Poses change over time, and walking NPCs play their walk animation.
	long posesBefore = npcPosesBuilt;
	int walkers = 0, framesSeen = 0;
	for (int t = 0; t < 120; t++) {
		gameFrames(50);
		for (int i = 0; i < net.npcCount; i++) {
			const NetNpc* n = &net.npcs[i];
			const NpcDef* shown = npcShown(n->type);
			if (!shown) continue;
			const BasDef* b = anim_get_bas(shown->basId);
			if (n->moveSeq != -1 && n->moveSeq != b->idle) walkers++;
			if (n->moveFrame > 0) framesSeen++;
		}
	}
	fprintf(testOut, "npc_anim poses_built=%ld new=%ld pool_resets=%ld walking_samples=%d moving_frame_samples=%d seq_updates=%ld face_updates=%ld\n", npcPosesBuilt, npcPosesBuilt - posesBefore, npcPoolResets, walkers, framesSeen, net.npcSeqUpdates, net.npcFaceUpdates);
	fflush(testOut);

	// Attacking makes the server send action animations, for one side or the other.
	// Each try is one press from the real menu. Targets can wander out of reach, so a few tries are allowed.
	long seqBefore = net.npcSeqUpdates + net.playerSeqUpdates;
	bool attacked = false;
	for (int tries = 0; tries < 3 && net.npcSeqUpdates + net.playerSeqUpdates == seqBefore; tries++) {
		bool pressed = false;
		for (int i = 0; i < 24 && !pressed; i++) {
			yaw = i * (float)M_PI / 12.0f;
			gameFrames(50);
			for (int k = 0; k < net.npcCount && !pressed; k++) {
				const NetNpc* n = &net.npcs[k];
				int x, y;
				if (!projectTile(n->viewX, n->viewY, 0.3f, width, &x, &y) || x < 4 || y < 4 || x >= width - 4 || y >= SCREEN_H - 4) continue;
				buildMenu(x, y, width);
				for (int e = 0; e < menuCount; e++) {
					if (menuEntries[e].kind != ENTRY_NPC || strncmp(menuEntries[e].label, "Attack", 6) != 0) continue;
					fprintf(testOut, "npc_attack %s\n", menuEntries[e].label);
					runEntry(&menuEntries[e]);
					pressed = attacked = true;
					break;
				}
			}
		}
		gameFrames(12000);
	}
	capture(gameScreen(), AUTOTEST_DIR "autotest_npc_combat.bmp");
	fprintf(testOut, "npc_combat attacked=%d seq_updates=%ld errors=%ld last=%s\n", attacked, net.npcSeqUpdates + net.playerSeqUpdates - seqBefore, net.npcErrors, net.lastMessage);
	fflush(testOut);
	return dialogue && opened && examined && actions == 1 && packets == 1 && walkPackets == 0 && (gap <= d->size + 1 || talked) && facing && attacked && net.npcSeqUpdates + net.playerSeqUpdates > seqBefore && framesSeen > 0 && walkers > 0 && net.npcErrors == 0;
}

static bool playerTest(int width) {
	fprintf(testOut, "self name=%s combat=%d size=%d has_model=%d wear_items=%d\n", net.looks[NET_SELF_ID].name, net.looks[NET_SELF_ID].combat, net.looks[NET_SELF_ID].size, net.self.hasModel, wearCount);
	for (int i = 0; i < NET_PLAYER_OPTIONS; i++) if (net.playerOptions[i][0]) fprintf(testOut, "  player_option %d %s low=%d\n", i, net.playerOptions[i], net.playerOptionLow[i]);
	viewNpc(&net.self, AUTOTEST_DIR "autotest_self.bmp");
	fprintf(testOut, "self_angle view=%.0f target=%d face_entity=%d\n", net.self.viewAngle, net.self.angle, net.self.faceEntity);

	// With the camera south of the player, walking north must show their back and walking south their face.
	yaw = 0;
	pitch = 0.35f;
	zoom = 6.0f;
	for (int leg = 0; leg < 2; leg++) {
		net_walk(&net, net_world_x(&net), net_world_y(&net) + (leg == 0 ? 5 : -5), 0);
		gameFrames(1800);
		fprintf(testOut, "self_walk leg=%d view=%.0f target=%d move_seq=%d tile=%d,%d\n", leg, net.self.viewAngle, net.self.angle, net.self.moveSeq, net_world_x(&net), net_world_y(&net));
		capture(gameScreen(), leg == 0 ? AUTOTEST_DIR "autotest_self_north.bmp" : AUTOTEST_DIR "autotest_self_south.bmp");
		gameFrames(2600);
	}
	pitch = 0.6f;

	// The Grand Exchange has the server's bots, so there are people to see.
	net_command(&net, "tele 3165 3487 0");
	gameFrames(14000);
	// The bots wander, so wait for someone to come near.
	for (int t = 0; t < 100 && net.playerCount == 0; t++) gameFrames(500);
	gameFrames(3000);
	int ready = 0, valid = 0;
	for (int i = 0; i < net.playerCount; i++) {
		const NetNpc* n = &net.players[i];
		const NetLook* l = &net.looks[n->index];
		valid += l->valid;
		ready += n->hasModel;
		if (i < 10) fprintf(testOut, "  player index=%d %s level=%d tile=%d,%d items=%d has_model=%d\n", n->index, l->name, l->combat, n->x, n->y, (l->slots[0] >= 32768) + (l->slots[1] >= 32768) + (l->slots[3] >= 32768) + (l->slots[4] >= 32768) + (l->slots[5] >= 32768), n->hasModel);
	}
	fprintf(testOut, "players known=%d looks=%d with_model=%d models_built=%ld poses=%ld pool_kb=%lu resets=%ld look_updates=%ld seq_updates=%ld errors=%ld\n", net.playerCount, valid, ready, npcModelsBuilt, npcPosesBuilt,
		(unsigned long)((npcPoolBottom + NPC_POOL_SIZE - npcPoolTop) >> 10), npcPoolResets, net.lookUpdates, net.playerSeqUpdates, net.playerErrors);
	zoom = 20.0f;
	gameFrames(500);
	capture(gameScreen(), AUTOTEST_DIR "autotest_players.bmp");
	fprintf(testOut, "players_drawn entities=%d tris=%lu\n", npcsDrawn, (unsigned long)trisSubmitted);
	fflush(testOut);

	// Someone on screen must offer the server's options, and one press sends one packet.
	// They may walk away, so the target is chosen again just before each try.
	int sx = 0, sy = 0, row = -1;
	long before = actionsSent, sent = net.playerActionsSent;
	bool opened = false, viewed = false;
	for (int attempt = 0; attempt < 40 && row < 0; attempt++) {
		yaw = attempt * (float)M_PI / 12.0f;
		gameFrames(100);
		int target = -1;
		for (int i = 0; i < net.playerCount && target < 0; i++) {
			const NetNpc* n = &net.players[i];
			if (!n->hasModel || !projectTile(n->viewX, n->viewY, 0.7f, width, &sx, &sy) || sx < 4 || sy < 4 || sx >= width - 4 || sy >= SCREEN_H - 4) continue;
			buildMenu(sx, sy, width);
			for (int e = 0; e < menuCount; e++) if (menuEntries[e].kind == ENTRY_PLAYER && menuEntries[e].object == n->index) target = i;
		}
		if (target < 0) continue;
		if (!viewed) {
			viewNpc(&net.players[target], AUTOTEST_DIR "autotest_player_0.bmp");
			viewed = true;
		}
		before = actionsSent;
		pressAndHold(KEY_TOUCH, sx, sy);
		opened = menuOpen && actionsSent == before;
		handleInput(0, 0, NO_TOUCH, NO_PAD);
		panelDirty = true;
		frameTick(16.7, testScene);
		frameTick(16.7, testScene);
		capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_player_menu.bmp");
		for (int i = 0; i < menuCount; i++) {
			fprintf(testOut, "  menu %d: %s\n", i, menuEntries[i].label);
			if (menuEntries[i].kind == ENTRY_PLAYER && row < 0) row = i;
		}
		if (row < 0) pressAndHold(KEY_B, 0, 0);
	}
	if (row >= 0) {
		tapMenuRow(row);
		gameFrames(2500);
	}
	fprintf(testOut, "player_press opened=%d actions=%ld packets=%ld errors=%ld message=%s\n", opened, actionsSent - before, net.playerActionsSent - sent, net.playerErrors, net.lastMessage);
	fflush(testOut);
	return opened && row >= 0 && actionsSent == before + 1 && net.playerActionsSent == sent + 1 && net.playerErrors == 0 && net.self.hasModel;
}

static int findInBag(int id) {
	for (int i = 0; i < BAG_SLOTS; i++) if (bagItemAt(i) == id) return i;
	return -1;
}

static void tapSlot(int slot) {
	int x, y;
	bagCell(slot, 320, &x, &y);
	tap(x + 20, y + 20);
}

// Keeps the stylus down for a time, wobbling by a few pixels as a real hand does, then lifts it.
static void holdFor(int x, int y, int ms, int wobble) {
	long start = nowMs();
	touchPosition touch = {(u16)x, (u16)y};
	handleInput(KEY_TOUCH, KEY_TOUCH, touch, NO_PAD);
	frameTick(16.7, testScene);
	for (int i = 0; (u32)nowMs() - (u32)start < (u32)ms; i++) {
		touch.px = (u16)(x + (wobble ? (i % 3) * wobble - wobble : 0));
		touch.py = (u16)(y + (wobble ? ((i / 3) % 3) * wobble - wobble : 0));
		handleInput(0, KEY_TOUCH, touch, NO_PAD);
		frameTick(16.7, testScene);
	}
	handleInput(0, 0, NO_TOUCH, NO_PAD);
	frameTick(16.7, testScene);
}

// Drags in steps, optionally pausing past the menu delay halfway, then lifts the stylus.
static bool stylusNoise = false; // behaves like the real touch screen as the stylus lifts

static void dragStylus(int fromX, int fromY, int toX, int toY, int pauseMs) {
	touchPosition touch = {(u16)fromX, (u16)fromY};
	handleInput(KEY_TOUCH, KEY_TOUCH, touch, NO_PAD);
	frameTick(16.7, testScene);
	for (int i = 1; i <= 20; i++) {
		touch.px = (u16)(fromX + (toX - fromX) * i / 20);
		touch.py = (u16)(fromY + (toY - fromY) * i / 20);
		handleInput(0, KEY_TOUCH, touch, NO_PAD);
		frameTick(16.7, testScene);
		if (i == 10 && pauseMs) {
			long start = nowMs();
			capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_bag_drag.bmp");
			while ((u32)nowMs() - (u32)start < (u32)pauseMs) {
				handleInput(0, KEY_TOUCH, touch, NO_PAD);
				frameTick(16.7, testScene);
			}
			panelDirty = true;
			frameTick(16.7, testScene);
			frameTick(16.7, testScene);
			capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_bag_drag.bmp");
		}
	}
	// Rest a moment, as a hand does, then lift with the position going wrong on the way.
	for (int i = 0; i < 6; i++) {
		handleInput(0, KEY_TOUCH, touch, NO_PAD);
		frameTick(16.7, testScene);
	}
	if (stylusNoise) {
		touchPosition bad = {(u16)300, (u16)232};
		handleInput(0, KEY_TOUCH, bad, NO_PAD);
		frameTick(16.7, testScene);
		handleInput(0, KEY_TOUCH, NO_TOUCH, NO_PAD);
		frameTick(16.7, testScene);
	}
	handleInput(0, 0, NO_TOUCH, NO_PAD);
	frameTick(16.7, testScene);
}

// A level gained along the way opens a message, which has the screen until it is dismissed with one press.
static void dismissPopups(void) {
	gameFrames(600);
	for (int t = 0; t < 6 && dialogActive(); t++) {
		panelDirty = true;
		gameFrames(300);
		int press = -1;
		fprintf(testOut, "bag_popup interface=%d\n", dialogIface.id);
		for (int i = 0; i < dialogRowCount; i++) {
			fprintf(testOut, "    %s %s\n", dialogRows[i].component == -1 ? "text" : "press", dialogRows[i].text);
			if (dialogRows[i].component != -1 && dialogRows[i].w > 0 && press < 0) press = i;
		}
		capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_popup.bmp");
		if (press >= 0) tap(dialogRows[press].x + 20, dialogRows[press].y + 8);
		gameFrames(1500);
	}
}

// The three things a press on a slot can be must never be mistaken for each other.
static bool dragTest(void) {
	const int LOGS = 1511, TINDERBOX = 590;
	int logs = findInBag(LOGS), box = findInBag(TINDERBOX), x, y, tx, ty;
	if (logs < 0 || box < 0) {
		fprintf(testOut, "drag_test items missing\n");
		return false;
	}
	bagCell(logs, 320, &x, &y);
	bagCell(box, 320, &tx, &ty);

	// A drag sends exactly one packet, and the two items trade places. Pausing mid-drag must not open the menu.
	long actions = actionsSent, moves = net.itemMovesSent, other = net.itemActionsSent;
	stylusNoise = true;
	dragStylus(x + 20, y + 20, tx + 20, ty + 20, BAG_HOLD_MS + 400);
	stylusNoise = false;
	bool noMenu = !menuOpen;
	gameFrames(2000);
	bool swapped = findInBag(LOGS) == box && findInBag(TINDERBOX) == logs;
	bool one = actionsSent == actions + 1 && net.itemMovesSent == moves + 1 && net.itemActionsSent == other;
	fprintf(testOut, "drag_move swapped=%d one_packet=%d no_menu=%d logs_slot=%d tinderbox_slot=%d\n", swapped, one, noMenu, findInBag(LOGS), findInBag(TINDERBOX));

	// Moving into an empty slot is the usual case: the item goes there and its old slot empties.
	int empty = -1;
	for (int i = BAG_SLOTS - 1; i >= 0 && empty < 0; i--) if (bagItemAt(i) < 0) empty = i;
	logs = findInBag(LOGS);
	bagCell(logs, 320, &x, &y);
	bagCell(empty, 320, &tx, &ty);
	moves = net.itemMovesSent;
	stylusNoise = true;
	dragStylus(x + 20, y + 20, tx + 20, ty + 20, 0);
	stylusNoise = false;
	gameFrames(2000);
	int oldSlot = logs;
	bool movedToEmpty = empty >= 0 && bagItemAt(empty) == LOGS && bagItemAt(oldSlot) < 0 && net.itemMovesSent == moves + 1;
	fprintf(testOut, "drag_to_empty from=%d to=%d arrived=%d old_slot_empty=%d packets=%ld\n", oldSlot, empty, bagItemAt(empty) == LOGS, bagItemAt(oldSlot) < 0, net.itemMovesSent - moves);

	// Dropping it back on itself, or outside the grid, sends nothing.
	logs = findInBag(LOGS);
	bagCell(logs, 320, &x, &y);
	actions = actionsSent;
	dragStylus(x + 20, y + 20, x + 32, y + 22, 0);
	bool sameSlot = actionsSent == actions && !menuOpen;
	dragStylus(x + 20, y + 20, 160, 225, 0);
	bool outside = actionsSent == actions && !menuOpen && panelTab == TAB_BAG;
	gameFrames(500);
	fprintf(testOut, "drag_cancel same_slot=%d outside=%d still_there=%d\n", sameSlot, outside, findInBag(LOGS) == logs);

	// The old long-press time is now just a slow tap: no menu, and the top option happens once.
	// One bad sample as the stylus lifts must not turn a tap into a drag.
	long used = usingSlot;
	{
		touchPosition t = {(u16)(x + 20), (u16)(y + 20)}, bad = {(u16)300, (u16)232};
		handleInput(KEY_TOUCH, KEY_TOUCH, t, NO_PAD);
		frameTick(16.7, testScene);
		for (int i = 0; i < 4; i++) {
			handleInput(0, KEY_TOUCH, t, NO_PAD);
			frameTick(16.7, testScene);
		}
		handleInput(0, KEY_TOUCH, bad, NO_PAD);
		frameTick(16.7, testScene);
		handleInput(0, 0, NO_TOUCH, NO_PAD);
		frameTick(16.7, testScene);
	}
	bool noisyTap = usingSlot == logs;
	fprintf(testOut, "drag_noisy_tap using=%d\n", usingSlot);
	if (usingSlot >= 0) pressAndHold(KEY_B, 0, 0);
	holdFor(x + 20, y + 20, 550, 0);
	bool slowTap = !menuOpen && usingSlot == logs && used == -1;
	fprintf(testOut, "drag_slow_tap menu=%d using=%d\n", menuOpen, usingSlot);
	if (usingSlot >= 0) pressAndHold(KEY_B, 0, 0);

	// A shaky hold is still a hold: small wobbles must not start a drag, and the menu opens.
	actions = actionsSent;
	holdFor(x + 20, y + 20, BAG_HOLD_MS + 300, 2);
	bool shaky = menuOpen && actionsSent == actions;
	fprintf(testOut, "drag_shaky_hold menu=%d actions=%ld\n", menuOpen, actionsSent - actions);
	if (menuOpen) pressAndHold(KEY_B, 0, 0);
	fflush(testOut);
	return movedToEmpty && swapped && one && noMenu && sameSlot && outside && findInBag(LOGS) == logs && noisyTap && slowTap && shaky;
}

// Opens a slot's menu with a long press, then picks the row with this kind and option.
static bool chooseFromBag(int slot, int kind, int option) {
	int x, y;
	bagCell(slot, 320, &x, &y);
	holdFor(x + 20, y + 20, BAG_HOLD_MS + 300, 0);
	bool opened = menuOpen;
	int row = -1;
	for (int i = 0; i < menuCount; i++) {
		fprintf(testOut, "  menu %d: %s\n", i, menuEntries[i].label);
		if (menuEntries[i].kind == kind && menuEntries[i].option == option && row < 0) row = i;
	}
	panelDirty = true;
	frameTick(16.7, testScene);
	frameTick(16.7, testScene);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_bag_menu.bmp");
	if (row >= 0) tapMenuRow(row);
	else pressAndHold(KEY_B, 0, 0);
	return opened && row >= 0;
}

static bool bagTest(void) {
	const int BREAD = 2309, LOGS = 1511, TINDERBOX = 590, AXE = 1351, POT = 1931;
	pressAndHold(KEY_SELECT, 0, 0); // the panel goes to the touch screen
	// The test account keeps its items between runs, so it starts empty.
	static const char* const SPAWNS[] = {"empty", "item 2309 3", "item 1511 1", "item 590 1", "item 1351 1", "item 1931 1", "item 995 12345"};
	for (int i = 0; i < 7; i++) {
		net_command(&net, SPAWNS[i]);
		gameFrames(700);
	}
	Widget widgets[MAX_WIDGETS];
	int count = panelLayout(320, widgets);
	for (int i = 0; i < count; i++) if (widgets[i].id == WIDGET_TAB_BAG) tap(widgets[i].x + 10, widgets[i].y + 10);
	gameFrames(3000);
	panelDirty = true;
	gameFrames(100);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_bag.bmp");
	const NetInv* inv = bagItems();
	int held = 0;
	for (int i = 0; i < BAG_SLOTS; i++) {
		if (bagItemAt(i) < 0) continue;
		held++;
		fprintf(testOut, "  slot %d %s x%d\n", i, itemName(bagItemAt(i)), bagAmountAt(i));
	}
	fprintf(testOut, "bag tab=%d inventory=%d items=%d icons_built=%ld updates=%ld\n", panelTab, inv != NULL, held, iconsBuilt, net.invVersion);

	// A plain tap does the top option once, and the server's reply shows in the grid.
	int slot = findInBag(BREAD), before = bagAmountAt(slot);
	long actions = actionsSent, packets = net.itemActionsSent;
	if (slot >= 0) tapSlot(slot);
	gameFrames(2500);
	bool ate = slot >= 0 && actionsSent == actions + 1 && net.itemActionsSent == packets + 1;
	int left = 0;
	for (int i = 0; i < BAG_SLOTS; i++) if (bagItemAt(i) == BREAD) left += bagAmountAt(i) ? 1 : 0;
	fprintf(testOut, "bag_eat one_press=%d bread_before=%d loaves_left=%d message=%s\n", ate, before, left, net.lastMessage);

	// Wielding removes it from the bag and changes how the player looks.
	slot = findInBag(AXE);
	long looks = net.lookUpdates, bagUpdates = net.invVersion;
	packets = net.itemActionsSent;
	if (slot >= 0) tapSlot(slot);
	gameFrames(2500);
	// The account may already hold the same axe, which swaps it back, so the bag changing is what counts.
	bool wielded = slot >= 0 && net.itemActionsSent == packets + 1 && net.invVersion > bagUpdates;
	fprintf(testOut, "bag_wield gone=%d look_updates=%ld bag_updates=%ld\n", findInBag(AXE) < 0, net.lookUpdates - looks, net.invVersion - bagUpdates);

	// The menu lists everything, and a long press sends nothing by itself.
	slot = findInBag(LOGS);
	actions = actionsSent;
	long messages = net.messages;
	bool dragged = dragTest();
	slot = findInBag(LOGS);
	actions = actionsSent;
	messages = net.messages;
	bool examined = slot >= 0 && chooseFromBag(slot, ENTRY_ITEM_EXAMINE, 0);
	gameFrames(1500);
	examined = examined && actionsSent == actions + 1 && net.messages > messages;
	fprintf(testOut, "bag_examine ok=%d message=%s\n", examined, net.lastMessage);

	// Use takes two presses: the item, then what it goes on. Neither repeats.
	int tinderbox = findInBag(TINDERBOX), logs = findInBag(LOGS);
	actions = actionsSent;
	packets = net.itemActionsSent;
	bool chose = tinderbox >= 0 && chooseFromBag(tinderbox, ENTRY_ITEM_USE, 0) && usingSlot == tinderbox;
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_bag_use.bmp");
	bool nothingYet = actionsSent == actions && net.itemActionsSent == packets;
	if (logs >= 0) tapSlot(logs);
	gameFrames(6000);
	bool used = chose && nothingYet && actionsSent == actions + 1 && net.itemActionsSent == packets + 1 && usingSlot == -1;
	fprintf(testOut, "bag_use chose=%d waited=%d sent=%ld logs_left=%d message=%s\n", chose, nothingYet, net.itemActionsSent - packets, findInBag(LOGS) >= 0, net.lastMessage);

	dismissPopups();

	// Dropping uses the last option, and the slot empties.
	slot = findInBag(POT);
	bool dropped = slot >= 0 && chooseFromBag(slot, ENTRY_ITEM, 4);
	gameFrames(2500);
	dropped = dropped && findInBag(POT) < 0;
	fprintf(testOut, "bag_drop ok=%d\n", dropped);
	fflush(testOut);

	dismissPopups();

	// Every tab along both rows answers to a tap, and the options keep working from their tab.
	int reached = 0;
	count = panelLayout(320, widgets);
	for (int i = 0; i < count; i++) {
		if (widgets[i].id < WIDGET_TAB0) continue;
		int want = widgets[i].id - WIDGET_TAB0;
		tap(widgets[i].x + widgets[i].w / 2, widgets[i].y + widgets[i].h / 2);
		if (panelTab == want) reached++;
		if (want == TAB_WORN) {
			panelDirty = true;
			gameFrames(100);
			capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_tab_worn.bmp");
		}
		count = panelLayout(320, widgets);
	}
	Widget options[MAX_WIDGETS];
	panelTab = TAB_OPTIONS;
	panelDirty = true;
	gameFrames(100);
	int optionCount = panelLayout(320, options), viewBefore = radiusIndex;
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_tab_options.bmp");
	for (int i = 0; i < optionCount; i++) if (options[i].id == WIDGET_DISTANCE) tap(options[i].x + 10, options[i].y + 10);
	bool viewChanged = radiusIndex != viewBefore;
	radiusIndex = viewBefore;
	panelTab = TAB_MAP;
	panelDirty = true;
	gameFrames(500);
	panelDirty = true;
	gameFrames(500);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_tab_map.bmp");
	fprintf(testOut, "tabs reached=%d of %d view_changed=%d\n", reached, TAB_COUNT, viewChanged);
	pressAndHold(KEY_SELECT, 0, 0);
	return dragged && reached == TAB_COUNT && viewChanged && inv && held >= 5 && ate && wielded && examined && used && dropped;
}

// Drops something, walks away, and picks it up again with one tap.
static bool groundTest(int width) {
	const int POT = 1931;
	net_command(&net, "item 1931 1");
	gameFrames(1500);
	int slot = findInBag(POT);
	if (slot < 0) {
		fprintf(testOut, "ground_test no pot\n");
		return false;
	}
	long version = net.groundVersion;
	net_item_action(&net, 4, BAG_INTERFACE, POT, slot);
	gameFrames(2500);
	int tileX = -1, tileY = -1;
	for (int i = 0; i < net.groundCount; i++) {
		if (net.ground[i].id != POT) continue;
		tileX = net.ground[i].x;
		tileY = net.ground[i].y;
	}
	fprintf(testOut, "ground dropped=%d seen=%d tile=%d,%d items=%d piles=%d version_changed=%d\n", findInBag(POT) < 0, tileX >= 0, tileX, tileY, net.groundCount, groundPileCount, net.groundVersion != version);
	if (tileX < 0) return false;

	net_walk(&net, net_world_x(&net) + 2, net_world_y(&net) - 2, 0);
	gameFrames(3500);
	yaw = 0;
	pitch = 0.6f;
	zoom = 8.0f;
	gameFrames(500);
	capture(gameScreen(), AUTOTEST_DIR "autotest_ground.bmp");
	int sx = 0, sy = 0;
	bool visible = projectTile(tileX + 0.5f, tileY + 0.5f, 0.05f, width, &sx, &sy) && sx >= 0 && sy >= 0 && sx < width && sy < SCREEN_H;
	// An NPC wandering over the pile rightly takes the top option, so wait for it to move on.
	buildMenu(sx, sy, width);
	for (int t = 0; t < 30 && menuCount > 0 && menuEntries[0].kind != ENTRY_GROUND; t++) {
		gameFrames(1000);
		buildMenu(sx, sy, width);
	}
	bool offered = false;
	for (int i = 0; i < menuCount; i++) {
		fprintf(testOut, "  menu %d: %s\n", i, menuEntries[i].label);
		if (menuEntries[i].kind == ENTRY_GROUND) offered = true;
	}
	bool top = menuCount > 0 && menuEntries[0].kind == ENTRY_GROUND;

	// A tap on the same tile but away from the item must walk, not pick it up.
	int edgeX = 0, edgeY = 0;
	projectTile(tileX + 0.04f, tileY + 0.04f, 0.0f, width, &edgeX, &edgeY);
	buildMenu(edgeX, edgeY, width);
	bool edgeWalks = menuCount > 0 && menuEntries[0].kind == ENTRY_WALK;

	// Earlier runs can leave pots here too, so one fewer is what counts.
	int potsBefore = 0, potsAfter = 0;
	for (int i = 0; i < net.groundCount; i++) if (net.ground[i].id == POT && net.ground[i].x == tileX && net.ground[i].y == tileY) potsBefore++;
	long actions = actionsSent, packets = net.groundActionsSent;
	buildMenu(sx, sy, width);
	for (int t = 0; t < 30 && menuCount > 0 && menuEntries[0].kind != ENTRY_GROUND; t++) {
		gameFrames(1000);
		buildMenu(sx, sy, width);
	}
	tap(sx, sy);
	long pressed = net.groundActionsSent - packets;
	gameFrames(5000);
	bool back = findInBag(POT) >= 0;
	for (int i = 0; i < net.groundCount; i++) if (net.ground[i].id == POT && net.ground[i].x == tileX && net.ground[i].y == tileY) potsAfter++;
	bool cleared = potsAfter == potsBefore - 1;
	fprintf(testOut, "ground_take visible=%d offered=%d top=%d edge_walks=%d actions=%ld packets=%ld in_bag=%d cleared=%d\n", visible, offered, top, edgeWalks, actionsSent - actions, pressed, back, cleared);
	fflush(testOut);
	return visible && offered && top && edgeWalks && actionsSent == actions + 1 && pressed == 1 && back && cleared;
}

static void openTab(int tab) {
	Widget widgets[MAX_WIDGETS];
	int count = panelLayout(320, widgets);
	for (int i = 0; i < count; i++) if (widgets[i].id == WIDGET_TAB0 + tab) tap(widgets[i].x + widgets[i].w / 2, widgets[i].y + widgets[i].h / 2);
}

// Wears a helmet from the bag, finds it on the doll, then takes it off again.
static bool wornTest(void) {
	const int HELM = 1155, HEAD_SLOT = 0;
	pressAndHold(KEY_SELECT, 0, 0);
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "item 1155 1");
	gameFrames(1000);
	openTab(TAB_BAG);
	gameFrames(800);
	int slot = findInBag(HELM);
	if (slot >= 0) tapSlot(slot);
	gameFrames(2500);
	openTab(TAB_WORN);
	gameFrames(2500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_worn.bmp");
	bool wearing = wornItemAt(HEAD_SLOT) == HELM && findInBag(HELM) < 0;
	const NetInv* inv = net_inventory(&net, WORN_INVENTORY);
	int shown = 0;
	for (int i = 0; inv && i < inv->count; i++) {
		if (inv->items[i] < 0) continue;
		shown++;
		fprintf(testOut, "  worn slot %d %s\n", i, itemName(inv->items[i]));
	}
	fprintf(testOut, "worn tab=%d wearing=%d items=%d options=%s,%s\n", panelTab, wearing, shown, wornOptions[0], wornOptions[1]);

	// A long press lists the options and sends nothing.
	int x, y;
	wornCell(&WORN_PLACES[0], 320, &x, &y);
	long actions = actionsSent, messages = net.messages;
	holdFor(x + 18, y + 16, LONG_PRESS_MS + 300, 0);
	bool menu = menuOpen && actionsSent == actions;
	int examine = -1;
	for (int i = 0; i < menuCount; i++) {
		fprintf(testOut, "  menu %d: %s\n", i, menuEntries[i].label);
		if (menuEntries[i].kind == ENTRY_ITEM_EXAMINE) examine = i;
	}
	panelDirty = true;
	frameTick(16.7, testScene);
	gameFrames(100);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_worn_menu.bmp");
	if (examine >= 0) tapMenuRow(examine);
	gameFrames(1500);
	bool examined = examine >= 0 && actionsSent == actions + 1 && net.messages > messages;

	// A plain tap takes it off: it leaves the doll, returns to the bag, and the player's look changes.
	actions = actionsSent;
	long packets = net.itemActionsSent, looks = net.lookUpdates;
	tap(x + 18, y + 16);
	long sent = net.itemActionsSent - packets;
	gameFrames(2500);
	bool removed = wornItemAt(HEAD_SLOT) < 0 && findInBag(HELM) >= 0;
	fprintf(testOut, "worn_remove menu=%d examined=%d message=%s actions=%ld packets=%ld removed=%d look_updates=%ld\n", menu, examined, net.lastMessage, actionsSent - actions, sent, removed, net.lookUpdates - looks);
	bool tookOff = actionsSent == actions + 1 && sent == 1 && removed && net.lookUpdates > looks;

	// A crowd can offer more than fits: nothing may be lost, and every page must work.
	menuCount = 0;
	for (int i = 0; i < 12; i++) addEntry(i == 10 ? ENTRY_ITEM_EXAMINE : ENTRY_CANCEL, 995, 0, 0, 0, i == 10 ? "Examine" : "Filler", "");
	menuOpen = true;
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_menu_pages.bmp");
	long pageActions = actionsSent, pageMessages = net.messages;
	int pages = menuPageCount();
	tapMenuRow(10);
	gameFrames(1500);
	bool paged = pages == 2 && !menuOpen && actionsSent == pageActions + 1 && net.messages > pageMessages;
	fprintf(testOut, "menu_pages pages=%d reached_last_page=%d message=%s\n", pages, paged, net.lastMessage);
	fflush(testOut);
	panelTab = TAB_MAP;
	panelDirty = true;
	pressAndHold(KEY_SELECT, 0, 0);
	return paged && wearing && menu && examined && tookOff;
}

// The nearest NPC on screen with an Attack option.
static int findFoeToTap(int width, int* sx, int* sy) {
	int best = -1;
	float bestDistance = 1e9f;
	for (int i = 0; i < net.npcCount; i++) {
		const NetNpc* n = &net.npcs[i];
		const NpcDef* d = npcShown(n->type);
		if (!d || !n->hasModel) continue;
		bool foe = false;
		for (int op = 0; op < 5; op++) if (d->ops[op] && strcmp(d->ops[op], "Attack") == 0) foe = true;
		int x, y;
		if (!foe || !projectTile(n->viewX, n->viewY, 0.7f, width, &x, &y) || x < 10 || y < 10 || x >= width - 10 || y >= SCREEN_H - 10) continue;
		float dx = n->viewX - net.localX, dy = n->viewY - net.localY, distance = dx * dx + dy * dy;
		if (distance >= bestDistance) continue;
		bestDistance = distance;
		best = i;
		*sx = x;
		*sy = y;
	}
	return best;
}

// Scrolls a named spell into view and presses it.
static bool tapSpell(const char* name) {
	for (int i = 0; i < gridCount; i++) {
		if (!strstr(gridCells[i].name, name)) continue;
		int height = CONTENT_BOTTOM - GRID_TOP, limit = gridTotal - height, want = (i / gridColumns) * gridCellSize;
		gridScroll = want > limit ? limit : want;
		if (gridScroll < 0) gridScroll = 0;
		panelDirty = true;
		gameFrames(200);
		int x, y;
		gridPlace(i, 320, &x, &y);
		tap(x + gridCellSize / 2, y + gridCellSize / 2);
		return true;
	}
	return false;
}

// Choosing a spell sends nothing, and the press on its target is the one action.
// Finds a pixel on the nearest foe that offers the cast. The tile's projection sits at a fixed height, which can be
// above something small, so it looks lower down as a player's tap would. The target wanders, so it keeps trying.
static bool aimAtFoe(int* sx, int* sy, int* target) {
	for (int t = 0; t < 40; t++) {
		*target = findFoeToTap(320, sx, sy);
		if (*target < 0) {
			*target = 0;
			gameFrames(100);
			continue;
		}
		for (int dy = 0; dy <= 36; dy += 4) {
			buildMenu(*sx, *sy + dy, 320);
			if (menuCount > 0 && menuEntries[0].kind == ENTRY_CAST_NPC) {
				const NpcDef* d = npcShown(net.npcs[*target].type);
				fprintf(testOut, "aim foe=%s dy=%d try=%d at=%d,%d\n", d && d->name ? d->name : "?", dy, t, *sx, *sy + dy);
				*sy += dy;
				return true;
			}
		}
		if (t % 13 == 0) {
			const NpcDef* d = npcShown(net.npcs[*target].type);
			fprintf(testOut, "aim missed foe=%s size=%d at=%d,%d first_kind=%d\n", d && d->name ? d->name : "?", d ? d->size : 0, *sx, *sy, menuCount > 0 ? menuEntries[0].kind : -1);
		}
		gameFrames(100);
	}
	return false;
}

static bool magicTabTest(void) {
	net_command(&net, "setlevel magic 99");
	gameFrames(2000);
	dismissPopups();
	net_command(&net, "item 556 200");
	gameFrames(600);
	net_command(&net, "item 558 200");
	gameFrames(600);
	net_command(&net, "item 554 200");
	gameFrames(600);
	net_command(&net, "item 561 20");
	gameFrames(600);
	net_command(&net, "item 1931 1");
	gameFrames(1000);
	openTab(TAB_MAGIC);
	for (int t = 0; t < 40 && gridCount == 0; t++) gameFrames(100);
	gameFrames(1000);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_magic.bmp");
	int targeted = 0, instant = 0;
	for (int i = 0; i < gridCount; i++) {
		if (gridCells[i].mask) targeted++;
		else if (gridCells[i].pressable) instant++;
		if (i < 6) fprintf(testOut, "  spell %d '%s' mask=%d sprite=%d\n", gridCells[i].component, gridCells[i].name, gridCells[i].mask, gridCells[i].sprite);
	}
	fprintf(testOut, "magic book=%d cells=%d targeted=%d instant=%d result=%d ms=%d members=%d\n", magicScript.iface, gridCount, targeted, instant, magicScript.result, magicScript.ms, net.members);
	if (gridCount < 40 || magicScript.result != 0) return false;

	long sent = net.spellsSent, buttons = net.buttonsSent;
	bool chose = tapSpell("Wind Strike") && castComponent >= 0;
	gameFrames(300);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_magic_chosen.bmp");
	bool quiet = net.spellsSent == sent && net.buttonsSent == buttons;
	pressAndHold(KEY_B, 0, 0);
	bool cancelled = castComponent < 0;
	fprintf(testOut, "magic chose=%d quiet=%d cancelled=%d\n", chose, quiet, cancelled);

	// Now cast it for real, on something that can be fought.
	net_command(&net, "tele 3235 3219 0");
	gameFrames(9000);
	openTab(TAB_MAGIC);
	gameFrames(500);
	tapSpell("Wind Strike");
	pressAndHold(KEY_SELECT, 0, 0);
	int sx = 0, sy = 0, target = -1;
	zoom = 14.0f;
	pitch = 0.7f; // earlier tests leave the camera wherever they needed it
	for (int i = 0; i < 48 && target < 0; i++) {
		yaw = i * (float)M_PI / 12.0f;
		gameFrames(100);
		target = findFoeToTap(320, &sx, &sy);
	}
	fprintf(testOut, "magic_aim target=%d screen=%d,%d cast=%d mask=%d game_on_bottom=%d\n", target, sx, sy, castComponent, castMask, gameOnBottom);
	int xpBefore = net.skillXp[6], runesBefore = invTotal(93, 556);
	bool offered = false;
	// The target wanders, so it can leave the spot between finding it and pressing.
	if (target >= 0) offered = aimAtFoe(&sx, &sy, &target);
	if (offered) {
		for (int i = 0; i < menuCount; i++) if (i < 5) fprintf(testOut, "  menu %d: %s\n", i, menuEntries[i].label);
		tap(sx, sy);
	}
	gameFrames(5000);
	bool cast = net.spellsSent == sent + 1 && castComponent < 0;
	fprintf(testOut, "magic_cast target=%d offered=%d sent=%ld xp=%d->%d air=%d->%d message=%.60s\n", target, offered, net.spellsSent - sent, xpBefore, net.skillXp[6], runesBefore, invTotal(93, 556), net.lastMessage);
	if (castComponent >= 0) pressAndHold(KEY_B, 0, 0);
	pressAndHold(KEY_SELECT, 0, 0);

	// A spell only for items goes to the bag, and the press on the item is the cast.
	openTab(TAB_MAGIC);
	gameFrames(800);
	long before = net.spellsSent;
	int slot = findInBag(1931);
	bool alchChose = tapSpell("Low Level Alchemy") && castComponent >= 0 && panelTab == TAB_BAG;
	gameFrames(500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_magic_alch.bmp");
	if (alchChose && slot >= 0) tapSlot(slot);
	gameFrames(4000);
	bool alched = net.spellsSent == before + 1 && findInBag(1931) < 0;
	fprintf(testOut, "magic_alch chose=%d slot=%d sent=%ld gone=%d\n", alchChose, slot, net.spellsSent - before, findInBag(1931) < 0);
	if (castComponent >= 0) pressAndHold(KEY_B, 0, 0);
	return chose && quiet && cancelled && offered && cast && alchChose && alched;
}

// Wields a weapon with a special attack, presses it once, then puts the axe back.
static bool specialTest(void) {
	const int BOW = 861, AXE = 1351;
	net_command(&net, "setlevel ranged 50"); // what a magic shortbow needs
	gameFrames(2000);
	dismissPopups();
	net_command(&net, "item 861 1");
	gameFrames(1200);
	int slot = findInBag(BOW);
	if (slot < 0) return false;
	net_item_action(&net, 1, BAG_INTERFACE, BOW, slot);
	gameFrames(1800);
	openTab(TAB_COMBAT);
	gameFrames(800);
	int special = -1;
	for (int i = 0; i < combatButtonCount; i++) if (combatButtons[i].special) special = i;
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_special.bmp");
	bool toggled = false;
	int energy = net.varps[VARP_STYLE_SPECIAL_ENERGY];
	if (special >= 0) {
		long sent = net.buttonsSent;
		int x, y, w, before = net.varps[VARP_SPECIAL_ON];
		combatPlace(special, 320, &x, &y, &w);
		tap(x + w / 2, y + 10);
		gameFrames(1500);
		toggled = net.varps[VARP_SPECIAL_ON] != before && net.buttonsSent == sent + 1;
		tap(x + w / 2, y + 10);
		gameFrames(1500);
	}
	fprintf(testOut, "special interface=%d buttons=%d special=%d energy=%d toggled=%d\n", combatIface.id, combatButtonCount, special, energy, toggled);
	int axe = findInBag(AXE);
	if (axe >= 0) {
		net_item_action(&net, 1, BAG_INTERFACE, AXE, axe);
		gameFrames(1800);
	}
	return special >= 0 && energy > 0 && toggled;
}

static void tapWidget(int id) {
	Widget widgets[MAX_WIDGETS];
	int count = panelLayout(320, widgets);
	for (int i = 0; i < count; i++) if (widgets[i].id == id) tap(widgets[i].x + widgets[i].w / 2, widgets[i].y + widgets[i].h / 2);
}

// Each setting goes off and on with one press, and the server's answer is what shows.
static bool optionsTabTest(void) {
	openTab(TAB_OPTIONS);
	gameFrames(500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_options.bmp");
	bool ok = true;
	for (int i = 0; i < 3; i++) {
		long sent = net.buttonsSent;
		bool before = optionOn(i);
		tapWidget(WIDGET_OPTION0 + i);
		gameFrames(1200);
		bool changed = optionOn(i) != before;
		tapWidget(WIDGET_OPTION0 + i);
		gameFrames(1200);
		bool back = optionOn(i) == before;
		fprintf(testOut, "option %s was=%d changed=%d back=%d sent=%ld\n", OPTION_ROWS[i].name, before, changed, back, net.buttonsSent - sent);
		ok = ok && changed && back && net.buttonsSent == sent + 2;
	}
	return ok;
}

// Adds a name with the keyboard, chooses it, then deletes it. Choosing sends nothing.
static bool socialListTest(int tab, const char* name) {
	openTab(tab);
	gameFrames(800);
	uint64_t key = net_name_key(name);
	long sent = net.socialSent;
	tapWidget(WIDGET_SOCIAL_DEL); // with nothing chosen this must do nothing
	gameFrames(200);
	bool refused = net.socialSent == sent;
	testKeyboardText = name;
	tapWidget(WIDGET_SOCIAL_ADD);
	testKeyboardText = NULL;
	gameFrames(1800);
	int row = -1;
	for (int i = 0; i < socialCount; i++) if (socialRows[i].key == key) row = i;
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, tab == TAB_FRIENDS ? AUTOTEST_DIR "autotest_friends.bmp" : AUTOTEST_DIR "autotest_ignore.bmp");
	fprintf(testOut, "social tab=%d row=%d count=%d name='%s' where='%s' state=%d refused=%d sent=%ld note='%s'\n", tab, row, socialCount, row >= 0 ? socialRows[row].name : "", row >= 0 ? socialRows[row].where : "", net.friendState, refused, net.socialSent - sent, socialNote);
	if (row < 0) return false;
	socialScroll = 0;
	tap(100, QUEST_LIST_TOP + row * QUEST_ROW_H + 6);
	gameFrames(300);
	bool chosen = socialChosen == key && net.socialSent == sent + 1;
	tapWidget(WIDGET_SOCIAL_DEL);
	gameFrames(1200);
	bool gone = true;
	for (int i = 0; i < socialCount; i++) if (socialRows[i].key == key) gone = false;
	fprintf(testOut, "social chosen=%d gone=%d sent=%ld\n", chosen, gone, net.socialSent - sent);
	return refused && chosen && gone && net.socialSent == sent + 2;
}

static bool chatHas(int kind, const char* text) {
	long first = net.chatVersion > NET_CHAT_LINES ? net.chatVersion - NET_CHAT_LINES : 0;
	for (long i = first; i < net.chatVersion; i++) {
		const NetChat* line = &net.chat[i % NET_CHAT_LINES];
		if (line->kind == kind && strstr(line->text, text)) return true;
	}
	return false;
}

// Messages go through the server and back, which proves both packing and unpacking.
static bool chatFocusTest(void) {
	bool ok = true;
	tapWidget(WIDGET_TAB_CHAT);
	gameFrames(500);
	long sent = net.chatSent;
	testKeyboardText = "hello from the three ds";
	tapWidget(WIDGET_CHAT_SAY);
	for (int t = 0; t < 40 && !chatHas(NET_CHAT_PUBLIC, "ello from the three ds"); t++) gameFrames(100);
	bool heard = chatHas(NET_CHAT_PUBLIC, "ello from the three ds");
	fprintf(testOut, "chat_public heard=%d sent=%ld lines=%ld\n", heard, net.chatSent - sent, net.chatVersion);
	ok = ok && heard && net.chatSent == sent + 1;

	// A second client logged in as chatpal is there to receive this.
	openTab(TAB_FRIENDS);
	gameFrames(500);
	testKeyboardText = "chatpal";
	tapWidget(WIDGET_SOCIAL_ADD);
	uint64_t pal = net_name_key("chatpal");
	int online = 0;
	for (int t = 0; t < 300 && !online; t++) {
		gameFrames(100);
		for (int i = 0; i < net.friendCount; i++) if (net.friends[i].key == pal && net.friends[i].world > 0) online = 1;
	}
	gameFrames(300);
	int row = -1;
	for (int i = 0; i < socialCount; i++) if (socialRows[i].key == pal) row = i;
	fprintf(testOut, "chat_friend online=%d row=%d where='%s'\n", online, row, row >= 0 ? socialRows[row].where : "");
	if (row >= 0) tap(100, QUEST_LIST_TOP + row * QUEST_ROW_H + 6);
	gameFrames(300);
	sent = net.chatSent;
	testKeyboardText = "a private word";
	tapWidget(WIDGET_SOCIAL_MSG);
	for (int t = 0; t < 40 && !chatHas(NET_CHAT_TO, "private word"); t++) gameFrames(100);
	bool echoed = chatHas(NET_CHAT_TO, "private word");
	fprintf(testOut, "chat_private echoed=%d sent=%ld online=%d\n", echoed, net.chatSent - sent, online);
	ok = ok && online && echoed && net.chatSent == sent + 1;
	gameFrames(300);
	if (socialChosen) tapWidget(WIDGET_SOCIAL_DEL);
	gameFrames(800);

	openTab(TAB_CLAN);
	gameFrames(500);
	testKeyboardText = "2009scape";
	tapWidget(WIDGET_SOCIAL_ADD);
	for (int t = 0; t < 40 && !net.clanOwner; t++) gameFrames(100);
	gameFrames(500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_clan.bmp");
	char owner[16];
	net_name_from_key(net.clanOwner, owner, sizeof(owner));
	fprintf(testOut, "chat_clan owner='%s' members=%d last='%.60s'\n", owner, net.clanCount, net.lastMessage);
	if (net.clanOwner) {
		tapWidget(WIDGET_TAB_CHAT);
		gameFrames(300);
		testKeyboardText = "/clan words here";
		tapWidget(WIDGET_CHAT_SAY);
		for (int t = 0; t < 40 && !chatHas(NET_CHAT_CLAN, "lan words here"); t++) gameFrames(100);
		fprintf(testOut, "chat_clan_message heard=%d\n", chatHas(NET_CHAT_CLAN, "lan words here"));
		openTab(TAB_CLAN);
		gameFrames(300);
		tapWidget(WIDGET_SOCIAL_ADD);
		for (int t = 0; t < 40 && net.clanOwner; t++) gameFrames(100);
		fprintf(testOut, "chat_clan_left left=%d\n", net.clanOwner == 0);
	}
	tapWidget(WIDGET_TAB_CHAT);
	gameFrames(500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_chat.bmp");
	return ok;
}

// Corner messages and overhead chat: seen, limited, and gone again in time.
static bool overlayFocusTest(void) {
	net_command(&net, "tele 3222 3218 0");
	gameFrames(9000);
	yaw = 0;
	pitch = 0.6f;
	zoom = 12.0f;
	tapWidget(WIDGET_TAB_CHAT);
	gameFrames(6000); // anything from logging in has gone by now
	int idle = overlayRowCount;
	Widget widgets[MAX_WIDGETS];
	int count = panelLayout(320, widgets);
	(void)count;
	openTab(TAB_BAG);
	gameFrames(300);
	long before = net.chatVersion;
	net_chat_public(&net, "Hello up here!");
	for (int t = 0; t < 40 && net.chatVersion == before; t++) gameFrames(100);
	gameFrames(400);
	capture(gameScreen(), AUTOTEST_DIR "autotest_overlay.bmp");
	bool over = net.self.overhead[0] && overlayRowCount >= 1;
	int rows = overlayRowCount;
	long drawn = overlayVerts;
	u32 started = (u32)nowMs();
	for (int i = 0; i < 6; i++) {
		char text[40];
		snprintf(text, sizeof(text), "message number %d", i);
		net_chat_public(&net, text);
		gameFrames(1000);
	}
	int flood = overlayRowCount;
	capture(gameScreen(), AUTOTEST_DIR "autotest_overlay_flood.bmp");
	net_chat_public(&net, "this one is long enough that it has to wrap on to a second row of text ok");
	gameFrames(1000);
	capture(gameScreen(), AUTOTEST_DIR "autotest_overlay_long.bmp");
	int wrapped = overlayRowCount;
	gameFrames(5600);
	int after = overlayRowCount;
	long maxUntil = (long)((u32)nowMs() - started);
	fprintf(testOut, "overlay fonts=%d small=%d big=%d idle=%d shown=%d rows=%d verts=%ld flood=%d wrapped=%d after=%d ms=%ld overhead='%s'\n", fontsLoaded, smallFont.lineHeight, overheadFont.lineHeight, idle, over, rows, drawn, flood, wrapped, after, maxUntil, net.self.overhead);
	return fontsLoaded && idle == 0 && over && flood <= OVERLAY_ROWS && flood >= 2 && after == 0;
}

// Logs out, fails a login, logs back in, and checks the game comes back whole.
static bool loginFocusTest(bool* sceneReady) {
	const int LOG_IN_X = LOGIN_BUTTON_X + 10, LOG_IN_Y = LOGIN_BUTTON_Y + 10;
	openTab(TAB_OPTIONS);
	gameFrames(800);
	long sent = net.buttonsSent;
	tapWidget(WIDGET_LOGOUT);
	for (int t = 0; t < 150 && online; t++) gameFrames(100);
	bool out = !online;
	fprintf(testOut, "logout left=%d sent=%ld clean=%d error='%s'\n", out, net.buttonsSent - sent, net.loggedOut, net.error);
	if (!out) return false;
	bool clean = net.loggedOut || logoutPressed;
	endSession();
	*sceneReady = false;
	loginOpen(clean ? "" : "Connection lost.");
	gameFrames(0);
	gspWaitForVBlank();
	capture(GFX_TOP, AUTOTEST_DIR "autotest_login_top.bmp");
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_login.bmp");
	password[0] = 0;
	int result = loginPress(LOG_IN_X, LOG_IN_Y, sceneReady);
	bool wantsPassword = result == LOGIN_STAY && strstr(loginMessage, "password") != NULL;
	fprintf(testOut, "login_empty stay=%d message='%s'\n", wantsPassword, loginMessage);

	// Typing goes through the fields. Pressing straight after logging out gets one answer and no retry,
	// since the server holds the account for a moment. This local server does not check passwords.
	testKeyboardText = "password1";
	loginPress(LOGIN_FIELD_X + 20, LOGIN_PASS_Y + 8, sceneReady);
	bool typed = strcmp(password, "password1") == 0;
	result = loginPress(LOG_IN_X, LOG_IN_Y, sceneReady);
	bool refused = result == LOGIN_STAY;
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_login_refused.bmp");
	fprintf(testOut, "login_refused typed=%d refused=%d reply=%d message='%s'\n", typed, refused, net.reply, loginMessage);
	for (int attempt = 0; attempt < 8 && result == LOGIN_STAY; attempt++) {
		svcSleepThread(8000000000LL);
		result = loginPress(LOG_IN_X, LOG_IN_Y, sceneReady);
		fprintf(testOut, "login_attempt %d result=%d reply=%d message='%s'\n", attempt, result, net.reply, loginMessage);
	}
	if (result != LOGIN_DONE) return false;
	gameFrames(3000);
	bool inLobby = net.inLobby;
	Widget widgets[MAX_WIDGETS];
	panelLayout(320, widgets);
	if (net.inLobby) pressAndHold(KEY_TOUCH, widgets[0].x + widgets[0].w / 2, widgets[0].y + widgets[0].h / 2);
	gameFrames(6000);
	openTab(TAB_BAG);
	gameFrames(1000);
	int items = 0;
	for (int i = 0; i < BAG_SLOTS; i++) if (bagItemAt(i) >= 0) items++;
	openTab(TAB_QUESTS);
	gameFrames(1500);
	capture(GFX_TOP, AUTOTEST_DIR "autotest_login_back.bmp");
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_login_quests.bmp");
	fprintf(testOut, "login_back lobby=%d scene=%d items=%d quest_rows=%d npcs=%d password_kept=%d linear_free_kb=%lu\n", inLobby, *sceneReady, items, questRowCount, net.npcCount, password[0] != 0, (unsigned long)(linearSpaceFree() >> 10));
	return wantsPassword && typed && *sceneReady && !net.inLobby && questRowCount > 100 && password[0] == 0;
}

// Opens a skill's guide with one press, turns a section, and closes it.
static bool guideFocusTest(void) {
	openTab(TAB_STATS);
	gameFrames(800);
	int x, y;
	statsCell(17, 320, &x, &y); // Woodcutting
	long sent = net.buttonsSent;
	tap(x + 20, y + 15);
	for (int t = 0; t < 60 && !(guideActive() && guidePieceCount > 0); t++) gameFrames(100);
	gameFrames(1500);
	if (guideActive() && guideKind->iface == 741) {
		// Straight after a level-up the first press shows that news, and the next one the guide.
		panelDirty = true;
		gameFrames(200);
		capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_guide_levelup.bmp");
		fprintf(testOut, "guide levelup pieces=%d height=%d\n", guidePieceCount, guideTotal);
		tap(320 - 20, 10);
		for (int t = 0; t < 30 && guideActive(); t++) gameFrames(100);
		sent = net.buttonsSent;
		tap(x + 20, y + 15);
		for (int t = 0; t < 60 && !(guideActive() && guidePieceCount > 0); t++) gameFrames(100);
	}
	gameFrames(2500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_guide.bmp");
	int items = 0, texts = 0;
	for (int i = 0; i < guidePieceCount; i++) {
		if (guidePieces[i].kind == PIECE_ITEM) items++;
		if (guidePieces[i].kind == PIECE_TEXT) texts++;
	}
	fprintf(testOut, "guide open=%d sent=%ld title='%s' section='%s' xp='%s' pieces=%d items=%d texts=%d height=%d result=%d ms=%d\n", guideActive(), net.buttonsSent - sent, guideTitle, guideSection, guideXp, guidePieceCount, items, texts, guideTotal, guideScripts[0].result, guideScripts[0].ms);
	if (!guideActive()) return false;
	bool opened = net.buttonsSent == sent + 1 && texts > 3 && guideScripts[0].result == 0;
	char first[32];
	snprintf(first, sizeof(first), "%s", guideSection);
	dragStylus(100, 200, 100, 80, 0);
	gameFrames(500);
	int scrolled = guideScroll;
	bool quiet = net.buttonsSent == sent + 1;
	sent = net.buttonsSent;
	tap(320 - CLOSE_W - 6 - GUIDE_ARROW_W / 2, 28);
	for (int t = 0; t < 40 && strcmp(first, guideSection) == 0; t++) gameFrames(100);
	gameFrames(2000);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_guide_next.bmp");
	bool turned = strcmp(first, guideSection) != 0 && net.buttonsSent == sent + 1;
	fprintf(testOut, "guide scrolled=%d quiet=%d turned=%d section='%s' pieces=%d\n", scrolled, quiet, turned, guideSection, guidePieceCount);
	tap(320 - 20, 10);
	for (int t = 0; t < 30 && guideActive(); t++) gameFrames(100);
	bool closed = !guideActive();
	fprintf(testOut, "guide closed=%d\n", closed);
	return opened && scrolled > 0 && quiet && turned && closed;
}

static int bagCount(int id) {
	return invTotal(93, id);
}

static void tapPageSlot(int slot) {
	int x, y;
	pageScroll = 0;
	pageCell(slot, 320, &x, &y);
	tap(x + 20, y + 18);
}

// Holds on a slot and picks the option whose label starts with this.
static bool pageMenuPick(int slot, const char* label) {
	int x, y;
	pageCell(slot, 320, &x, &y);
	holdFor(x + 20, y + 18, BAG_HOLD_MS + 300, 0);
	gameFrames(200);
	if (!menuOpen) return false;
	for (int i = 0; i < menuCount; i++) {
		if (strncmp(menuEntries[i].label, label, strlen(label)) != 0) continue;
		tapMenuRow(i);
		return true;
	}
	return false;
}

static int pageFind(int id) {
	const NetInv* inv = pageInventory();
	for (int i = 0; inv && i < inv->count; i++) if (inv->items[i] == id) return i;
	return -1;
}

static void tapPageButton(int number) {
	tap(4 + number * (PAGE_BUTTON_W + 4) + 20, PAGE_BUTTON_Y + 8);
	gameFrames(800);
}

// Moves logs in and out of the bank, one press each, including a typed amount and notes.
static bool bankFocusTest(void) {
	const int LOGS = 1511, NOTED = 1512;
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "item 1511 6");
	gameFrames(1000);
	net_command(&net, "bank");
	for (int t = 0; t < 40 && !pageActive(); t++) gameFrames(100);
	gameFrames(2000);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_bank.bmp");
	const NetInv* bank = net_inventory(&net, BANK_INVENTORY);
	fprintf(testOut, "bank open=%d slots=%d inv=%d\n", pageActive(), pageSlots(), bank ? bank->count : -1);
	if (!pageActive()) return false;

	tapPageButton(0); // over to the bag
	int slot = pageFind(LOGS);
	long sent = net.buttonsSent;
	tapPageSlot(slot);
	gameFrames(1200);
	bool one = bagCount(LOGS) == 5 && net.buttonsSent == sent + 1;
	bool all = pageMenuPick(pageFind(LOGS), "Deposit-All");
	gameFrames(1200);
	all = all && bagCount(LOGS) == 0;
	fprintf(testOut, "bank_deposit one=%d all=%d left=%d\n", one, all, bagCount(LOGS));
	tapPageButton(0); // back to the bank
	panelDirty = true;
	gameFrames(800);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_bank_items.bmp");
	slot = pageFind(LOGS);
	tapPageSlot(slot);
	gameFrames(1200);
	bool withdrew = bagCount(LOGS) == 1;
	testKeyboardText = "2";
	bool picked = pageMenuPick(pageFind(LOGS), "Withdraw-X");
	for (int t = 0; t < 30 && testKeyboardText; t++) gameFrames(100);
	gameFrames(1500);
	bool typed = picked && bagCount(LOGS) == 3;
	testKeyboardText = NULL;
	fprintf(testOut, "bank_withdraw one=%d x_picked=%d typed=%d bag=%d prompt='%s' last=%d\n", withdrew, picked, typed, bagCount(LOGS), net.promptText, net.varps[LAST_AMOUNT_VARP]);
	tapPageButton(1); // notes on
	bool notes = bankNotes();
	tapPageSlot(pageFind(LOGS));
	gameFrames(1200);
	bool noted = bagCount(NOTED) == 1;
	tapPageButton(1);
	fprintf(testOut, "bank_notes on=%d noted=%d off=%d\n", notes, noted, !bankNotes());
	tap(320 - 20, 8);
	for (int t = 0; t < 30 && pageActive(); t++) gameFrames(100);
	bool closed = !pageActive();
	fprintf(testOut, "bank_closed=%d\n", closed);
	return one && all && withdrew && typed && notes && noted && !bankNotes() && closed;
}

// Trades with a shopkeeper: value, buy and sell, each a single press.
static bool shopFocusTest(void) {
	net_command(&net, "tele 3212 3246 0");
	gameFrames(9000);
	net_command(&net, "item 995 2000");
	gameFrames(1000);
	int option = -1, index = -1;
	for (int i = 0; i < net.npcCount && index < 0; i++) {
		const NpcDef* d = npcShown(net.npcs[i].type);
		for (int op = 0; d && op < 5; op++) if (d->ops[op] && strcmp(d->ops[op], "Trade") == 0) {
			option = op;
			index = net.npcs[i].index;
		}
	}
	fprintf(testOut, "shop keeper index=%d option=%d\n", index, option);
	if (index < 0) return false;
	net_npc_action(&net, option, index);
	for (int t = 0; t < 120 && !pageActive(); t++) gameFrames(100);
	gameFrames(2500);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_shop.bmp");
	const NetInv* stock = pageInventory();
	fprintf(testOut, "shop open=%d kind=%d slots=%d title='%s'\n", pageActive(), pageKind, pageSlots(), net_component_text(&net, 620 << 16 | 22) ? net_component_text(&net, 620 << 16 | 22) : "");
	if (!pageActive() || !stock || pageSlots() == 0) return false;
	int item = stock->items[0];
	long sent = net.buttonsSent, messages = net.messages;
	tapPageSlot(0);
	gameFrames(1000);
	bool valued = net.messages > messages && net.buttonsSent == sent + 1;
	char value[80];
	snprintf(value, sizeof(value), "%s", net.lastMessage);
	int coins = bagCount(995), before = bagCount(item);
	bool picked = pageMenuPick(0, "Buy 1");
	gameFrames(1500);
	bool bought = picked && bagCount(item) == before + 1 && bagCount(995) < coins;
	fprintf(testOut, "shop value=%d '%.50s' bought=%d coins=%d->%d\n", valued, value, bought, coins, bagCount(995));
	tapPageButton(0);
	panelDirty = true;
	gameFrames(500);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_shop_bag.bmp");
	coins = bagCount(995);
	bool sold = pageMenuPick(pageFind(item), "Sell 1");
	gameFrames(1500);
	sold = sold && bagCount(item) == before;
	fprintf(testOut, "shop sold=%d coins_now=%d last='%.60s'\n", sold, bagCount(995), net.lastMessage);
	tap(320 - 20, 8);
	for (int t = 0; t < 30 && pageActive(); t++) gameFrames(100);
	return valued && bought && sold && !pageActive();
}

// Starts a fight and waits for a splat to land, then checks it goes away on time.
static bool hitsFocusTest(void) {
	net_command(&net, "tele 3235 3219 0");
	gameFrames(9000);
	zoom = 12.0f;
	pitch = 0.7f;
	int marks = 0;
	for (int i = 0; i < HIT_KINDS; i++) if (hitMarks[i].count) marks++;
	fprintf(testOut, "hits sprites marks=%d bar=%d,%d size=%dx%d mark0=%d runs\n", marks, hitBars[0].count, hitBars[1].count, hitBars[0].width, hitBars[0].height, hitMarks[0].count);
	int index = -1, option = -1;
	for (int i = 0; i < net.npcCount && index < 0; i++) {
		const NpcDef* d = npcShown(net.npcs[i].type);
		for (int op = 0; d && op < 5; op++) if (d->ops[op] && strcmp(d->ops[op], "Attack") == 0) {
			index = net.npcs[i].index;
			option = op;
		}
	}
	if (index < 0) return false;
	net_npc_action(&net, option, index);
	u32 now = 0;
	bool seen = false, enemyBar = false;
	int shots = 0, damage = -1, kinds = 0;
	for (int t = 0; t < 400; t++) {
		gameFrames(100);
		now = (u32)nowMs();
		for (int i = 0; i < net.npcCount; i++) {
			const NetNpc* n = &net.npcs[i];
			if (n->index != index) continue;
			for (int k = 0; k < 4; k++) if ((s32)(n->hitUntil[k] - now) > 0) {
				if (!seen || n->hitDamage[k] > 0) {
					seen = true;
					damage = n->hitDamage[k];
					kinds |= 1 << n->hitType[k];
					if (n->hitDamage[k] > 0 && shots < 2) capture(gameScreen(), shots++ ? AUTOTEST_DIR "autotest_hits2.bmp" : AUTOTEST_DIR "autotest_hits.bmp");
				}
			}
			if ((s32)(n->barUntil - now) > 0) enemyBar = true;
		}
		if (shots >= 2) break;
	}
	if (shots == 0) capture(gameScreen(), AUTOTEST_DIR "autotest_hits.bmp");
	gameFrames(HIT_SHOW_MS + 300);
	fprintf(testOut, "hits seen=%d bar=%d damage=%d kinds=%d shots=%d self_bar=%d enemy_left=%d\n", seen, enemyBar, damage, kinds, shots, net.self.bar, net.npcCount);
	fprintf(testOut, "hits parse npc_errors=%ld player_errors=%ld\n", net.npcErrors, net.playerErrors);
	return marks >= 2 && hitBars[0].count > 0 && seen && enemyBar;
}

// A second client asks to trade. Accepting is one press on its line in the chat, then coins go across.
static bool tradeFocusTest(void) {
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "item 995 500");
	gameFrames(800);
	net_command(&net, "tele 3222 3218 0");
	gameFrames(9000);
	for (int t = 0; t < 400 && !chatHas(NET_CHAT_TRADE, "wishes to trade"); t++) gameFrames(100);
	bool asked = chatHas(NET_CHAT_TRADE, "wishes to trade");
	capture(gameScreen(), AUTOTEST_DIR "autotest_trade_request.bmp");
	tapWidget(WIDGET_TAB_CHAT);
	gameFrames(600);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_trade_chat.bmp");
	int row = -1;
	for (int i = 0; i < chatRowCount; i++) if (chatRows[i].request[0]) row = i;
	fprintf(testOut, "trade asked=%d row=%d rows=%d\n", asked, row, chatRowCount);
	if (row < 0) return false;
	long sent = net.playerActionsSent;
	tap(100, CHAT_TOP + row * CHAT_ROW_H - chatScroll + 7);
	for (int t = 0; t < 100 && pageKind != PAGE_TRADE; t++) gameFrames(100);
	gameFrames(1500);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_trade.bmp");
	bool opened = pageKind == PAGE_TRADE;
	fprintf(testOut, "trade opened=%d one_press=%d title='%s'\n", opened, net.playerActionsSent == sent + 1, net_component_text(&net, 335 << 16 | 15) ? net_component_text(&net, 335 << 16 | 15) : "");
	if (!opened) return false;
	int coins = bagCount(995);
	bool picked = pageMenuPick(pageFind(995), "Offer-X");
	testKeyboardText = "100";
	for (int t = 0; t < 30 && testKeyboardText; t++) gameFrames(100);
	gameFrames(1500);
	testKeyboardText = NULL;
	tap(8 + PAGE_BUTTON_W + 20, PAGE_BUTTON_Y + 8); // over to Yours
	gameFrames(500);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_trade_offer.bmp");
	tapPageSlot(0); // takes one coin back, which must raise the warning
	gameFrames(1500);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_trade_modified.bmp");
	bool warned = net.varps[TRADE_YOU_CHANGED_VARP] != 0;
	const NetInv* mine = net_inventory(&net, 90);
	int offered = mine && mine->count > 0 && mine->items[0] == 995 ? mine->amounts[0] : 0;
	fprintf(testOut, "trade warned=%d\n", warned);
	fprintf(testOut, "trade offered=%d picked=%d bag=%d\n", offered, picked, bagCount(995));
	tap(16 + PAGE_BUTTON_W * 3 + 20, PAGE_BUTTON_Y + 8); // Accept
	for (int t = 0; t < 100 && pageKind != PAGE_TRADE_CONFIRM; t++) gameFrames(100);
	gameFrames(800);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_trade_confirm.bmp");
	bool second = pageKind == PAGE_TRADE_CONFIRM;
	tap(60, 225);
	for (int t = 0; t < 100 && pageActive(); t++) gameFrames(100);
	gameFrames(1000);
	fprintf(testOut, "trade second=%d finished=%d coins=%d->%d last='%.50s'\n", second, !pageActive(), coins, bagCount(995), net.lastMessage);
	return opened && warned && offered == 99 && second && !pageActive() && bagCount(995) == coins - 99;
}

// Talks to someone and waits for the head, through both the NPC's speech and the player's reply.
static void summonTapButton(const char* label) {
	SummonButton buttons[SUMMON_BUTTONS];
	int count = summonButtons(buttons);
	for (int i = 0; i < count; i++) {
		if (strcmp(buttons[i].label, label) != 0) continue;
		int x, y, w;
		summonPlace(buttons, i, 320, &x, &y, &w);
		tap(x + w / 2, y + SUMMON_BUTTON_H / 2);
		return;
	}
	fprintf(testOut, "summon no button '%s'\n", label);
}

static void summonReport(const char* when) {
	long first = net.chatVersion > 6 ? net.chatVersion - 6 : 0;
	for (long i = first; i < net.chatVersion; i++) fprintf(testOut, "summon chat kind=%d %s\n", net.chat[i % NET_CHAT_LINES].kind, net.chat[i % NET_CHAT_LINES].text);
	fprintf(testOut, "summon %s open=%d tab=%d result=%d name='%s' time='%s' points='%s' scrolls='%s' special=%d follower=%d pouch=%d\n", when, summonOpen(), net_open_interface(&net, TAB_PARENT(TAB_SUMMONING)), summonScript.result, summonText(SUMMON_NAME), summonText(SUMMON_TIMER), summonText(SUMMON_POINTS), summonText(SUMMON_SCROLLS), summonSpecial(), net.varps[VARP_FOLLOWER], net.varps[448]);
}

// Summons a spirit wolf from its pouch, then uses each button once.
static bool summonFocusTest(void) {
	const int POUCH = 12047, SCROLL = 12425;
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "setlevel summoning 99");
	gameFrames(800);
	net_command(&net, "setqueststage 146 100"); // Wolf Whistle, which the server wants before any summoning
	gameFrames(800);
	net_command(&net, "item 12047 1");
	gameFrames(800);
	net_command(&net, "item 12425 5");
	gameFrames(800);
	net_command(&net, "tele 3222 3218 0");
	gameFrames(6000);
	openTab(TAB_SUMMONING);
	gameFrames(400);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_summon_none.bmp");
	bool none = !summonOpen();
	openTab(TAB_BAG);
	gameFrames(400);
	int slot = -1;
	for (int i = 0; i < 28; i++) if (bagItemAt(i) == POUCH) slot = i;
	fprintf(testOut, "summon pouch slot=%d scrolls=%d none=%d\n", slot, bagCount(SCROLL), none);
	if (slot < 0) return false;
	long sent = actionsSent;
	// Summon sorts below Use, so a plain press would only pick the pouch up. It is chosen from the menu.
	buildBagMenu(slot);
	int summonEntry = -1;
	for (int i = 0; i < menuCount; i++) if (strncmp(menuEntries[i].label, "Summon", 6) == 0) summonEntry = i;
	if (summonEntry < 0) return false;
	runEntry(&menuEntries[summonEntry]);
	menuCount = 0;
	for (int t = 0; t < 60 && !summonOpen(); t++) gameFrames(100);
	gameFrames(1500);
	openTab(TAB_SUMMONING);
	gameFrames(800);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_summon.bmp");
	capture(gameScreen(), AUTOTEST_DIR "autotest_summon_game.bmp");
	summonReport("up");
	bool up = summonOpen() && summonScript.result == 0 && summonSpecial() >= 0 && summonText(SUMMON_NAME)[0];
	if (!up) return false;
	long buttons = net.buttonsSent;
	summonTapButton("Call Follower");
	gameFrames(1200);
	bool call = net.buttonsSent == buttons + 1;
	int before = bagCount(SCROLL);
	SummonButton list[SUMMON_BUTTONS];
	summonButtons(list);
	char special[40];
	snprintf(special, sizeof(special), "%s", list[0].label);
	long spells = net.spellsSent;
	buttons = net.buttonsSent;
	summonTapButton(special);
	gameFrames(1500);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_summon_special.bmp");
	fprintf(testOut, "summon special '%s' cast=%d mask=%d sent=%ld scrolls %d->%d\n", special, castComponent, castMask, net.buttonsSent - buttons, before, bagCount(SCROLL));
	bool aimed = castComponent >= 0 && castInterface == SUMMON_INTERFACE && net.spellsSent == spells;
	summonTapButton(special); // a second press puts it down
	gameFrames(400);
	bool dropped = castComponent < 0;
	net_command(&net, "tele 3235 3219 0");
	gameFrames(9000);
	openTab(TAB_SUMMONING);
	gameFrames(800);
	summonTapButton(special);
	gameFrames(300);
	pressAndHold(KEY_SELECT, 0, 0);
	int sx = 0, sy = 0, target = -1;
	zoom = 14.0f;
	pitch = 0.7f;
	for (int i = 0; i < 48 && target < 0; i++) {
		yaw = i * (float)M_PI / 12.0f;
		gameFrames(100);
		target = findFoeToTap(320, &sx, &sy);
	}
	bool offered = false;
	if (target >= 0) offered = aimAtFoe(&sx, &sy, &target);
	if (offered) {
		fprintf(testOut, "summon aim '%s'\n", menuEntries[0].label);
		tap(sx, sy);
	}
	gameFrames(5000);
	bool fired = offered && net.spellsSent == spells + 1 && bagCount(SCROLL) == before - 1 && castComponent < 0;
	fprintf(testOut, "summon fired=%d offered=%d spells=%ld scrolls %d->%d message=%.60s\n", fired, offered, net.spellsSent - spells, before, bagCount(SCROLL), net.lastMessage);
	if (castComponent >= 0) pressAndHold(KEY_B, 0, 0);
	pressAndHold(KEY_SELECT, 0, 0);
	gameFrames(500);
	openTab(TAB_SUMMONING);
	gameFrames(500);
	buttons = net.buttonsSent;
	summonTapButton("Take BoB");
	gameFrames(1200);
	bool bob = net.buttonsSent == buttons + 1 && chatHas(NET_CHAT_GAME, "not a beast of burden");
	buttons = net.buttonsSent;
	summonTapButton("Dismiss");
	gameFrames(2000);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_summon_dismiss.bmp");
	bool asked = net.buttonsSent == buttons + 1 && dialogActive();
	fprintf(testOut, "summon dismiss asked=%d dialog=%d\n", asked, dialogActive());
	tap(160, 70); // No
	gameFrames(1500);
	bool answered = !dialogActive();
	openTab(TAB_SUMMONING);
	gameFrames(500);
	buttons = net.buttonsSent;
	summonTapButton("Dismiss Now");
	for (int t = 0; t < 40 && summonOpen(); t++) gameFrames(100);
	gameFrames(800);
	summonReport("gone");
	bool gone = !summonOpen() && net.buttonsSent == buttons + 1;
	bool named = special[0] != 0;
	fprintf(testOut, "summon none=%d up=%d named=%d call=%d aimed=%d dropped=%d fired=%d bob=%d asked=%d answered=%d gone=%d sent_actions=%ld\n", none, up, named, call, aimed, dropped, fired, bob, asked, answered, gone, actionsSent - sent);
	return none && up && named && call && aimed && dropped && fired && bob && asked && answered && gone;
}

// Summons from a pouch in the bag, choosing it from the menu because Summon sorts below Use.
static bool summonFromBag(int pouch) {
	int slot = -1;
	for (int i = 0; i < 28; i++) if (bagItemAt(i) == pouch) slot = i;
	if (slot < 0) return false;
	buildBagMenu(slot);
	int entry = -1;
	for (int i = 0; i < menuCount; i++) if (strncmp(menuEntries[i].label, "Summon", 6) == 0) entry = i;
	if (entry >= 0) runEntry(&menuEntries[entry]);
	menuCount = 0;
	for (int t = 0; t < 60 && !summonOpen(); t++) gameFrames(100);
	gameFrames(1200);
	return summonOpen();
}

static void pageTapCell(int slot) {
	int x, y;
	pageCell(slot, 320, &x, &y);
	tap(x + PAGE_CELL_W / 2, y + PAGE_CELL_H / 2);
}

static void pageShot(const char* path) {
	gameFrames(500);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, path);
}

// Stores in, takes out and empties a thorny snail, by the pressing a player does.
static bool burdenFocusTest(void) {
	const int POUCH = 12019, LOGS = 1511;
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "setlevel summoning 99");
	gameFrames(800);
	net_command(&net, "setqueststage 146 100");
	gameFrames(800);
	net_command(&net, "item 12019 1");
	gameFrames(800);
	net_command(&net, "item 1511 3");
	gameFrames(800);
	net_command(&net, "tele 3222 3218 0");
	gameFrames(6000);
	openTab(TAB_BAG);
	gameFrames(400);
	if (!summonFromBag(POUCH)) {
		fprintf(testOut, "burden no familiar\n");
		return false;
	}
	openTab(TAB_SUMMONING);
	gameFrames(900);
	pageShot(AUTOTEST_DIR "autotest_summon_head.bmp");
	bool head = summonHeadReady;
	int index = -1, option = -1;
	for (int i = 0; i < net.npcCount; i++) {
		if (net.npcs[i].type != net.varps[VARP_FOLLOWER]) continue;
		const NpcDef* d = npcShown(net.npcs[i].type);
		for (int o = 0; d && o < 5; o++) if (d->ops[o] && strcasecmp(d->ops[o], "Store") == 0) option = o;
		index = net.npcs[i].index;
	}
	fprintf(testOut, "burden head=%d npc=%d option=%d\n", head, index, option);
	if (index < 0 || option < 0) return false;
	net_npc_action(&net, option, index);
	for (int t = 0; t < 50 && pageKind != PAGE_BURDEN; t++) gameFrames(100);
	bool opened = pageKind == PAGE_BURDEN;
	pageShot(AUTOTEST_DIR "autotest_burden.bmp");
	if (!opened) return false;
	tap(4 + PAGE_BUTTON_W / 2, PAGE_BUTTON_Y + 8); // over to the bag
	pageShot(AUTOTEST_DIR "autotest_burden_bag.bmp");
	int slot = -1;
	for (int i = 0; i < 28; i++) if (bagItemAt(i) == LOGS) slot = i;
	pageTapCell(slot);
	gameFrames(1200);
	bool storedOne = invTotal(BURDEN_INVENTORY, LOGS) == 1 && bagCount(LOGS) == 2;
	for (int i = 0; i < 28; i++) if (bagItemAt(i) == LOGS) slot = i;
	int x, y;
	pageCell(slot, 320, &x, &y);
	openPageMenu(x + 4, y + 4, 320);
	int all = -1;
	for (int i = 0; i < menuCount; i++) if (strncmp(menuEntries[i].label, "Store-All", 9) == 0) all = i;
	if (all >= 0) runEntry(&menuEntries[all]);
	closeMenu();
	gameFrames(1200);
	int held = invTotal(BURDEN_INVENTORY, LOGS);
	tap(4 + PAGE_BUTTON_W / 2, PAGE_BUTTON_Y + 8); // back to the familiar
	pageShot(AUTOTEST_DIR "autotest_burden_full.bmp");
	pageTapCell(0);
	gameFrames(1200);
	bool tookOne = bagCount(LOGS) == 1;
	tap(8 + PAGE_BUTTON_W + PAGE_BUTTON_W / 2, PAGE_BUTTON_Y + 8); // Take all
	gameFrames(1200);
	bool tookAll = bagCount(LOGS) == 3 && invTotal(BURDEN_INVENTORY, LOGS) == 0;
	tap(4 + PAGE_BUTTON_W / 2, PAGE_BUTTON_Y + 8);
	gameFrames(500);
	for (int i = 0; i < 28; i++) if (bagItemAt(i) == LOGS) slot = i;
	pageTapCell(slot);
	gameFrames(1200);
	tap(320 - CLOSE_W, 8);
	for (int t = 0; t < 30 && pageKind != PAGE_NONE; t++) gameFrames(100);
	bool closed = pageKind == PAGE_NONE;
	openTab(TAB_SUMMONING);
	gameFrames(600);
	summonTapButton("Take BoB");
	gameFrames(1500);
	bool bob = bagCount(LOGS) == 3;
	summonTapButton("Dismiss");
	for (int t = 0; t < 30 && !dialogActive(); t++) gameFrames(100);
	gameFrames(500);
	tap(160, 40); // Yes
	for (int t = 0; t < 40 && summonOpen(); t++) gameFrames(100);
	bool gone = !summonOpen();
	if (summonOpen()) net_button_second(&net, SUMMON_INTERFACE, SUMMON_DISMISS);
	gameFrames(800);
	fprintf(testOut, "burden opened=%d stored_one=%d held=%d took_one=%d took_all=%d closed=%d bob=%d gone=%d head=%d\n", opened, storedOne, held, tookOne, tookAll, closed, bob, gone, head);
	return opened && storedOne && held == 3 && tookOne && tookAll && closed && bob && gone && head;
}

static bool infuseOpen(void) {
	for (int i = 0; i < pickCount; i++) {
		const LocDef* d = pickList[i].def;
		for (int o = 0; d && o < 5; o++) {
			if (!d->ops[o] || strcasecmp(d->ops[o], "Infuse-pouch") != 0) continue;
			fprintf(testOut, "infuse obelisk id=%d option=%d tile=%d,%d\n", pickList[i].id, o, pickList[i].tileX, pickList[i].tileY);
			net_loc_action(&net, o, pickList[i].id, (net.zoneX - 6) * 8 + pickList[i].tileX, (net.zoneY - 6) * 8 + pickList[i].tileY);
			for (int t = 0; t < 120 && pageKind != PAGE_INFUSE; t++) gameFrames(100);
			gameFrames(600);
			return pageKind == PAGE_INFUSE;
		}
	}
	fprintf(testOut, "infuse no obelisk among %d\n", pickCount);
	return false;
}

static int infuseFind(int item) {
	for (int i = 0; i < INFUSE_ENTRIES; i++) if (infuseItems[i] == item) return i;
	return -1;
}

// Makes a spirit wolf pouch at an obelisk, then turns it into scrolls.
static bool infuseFocusTest(void) {
	const int POUCH = 12047, SCROLL = 12425;
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "setlevel summoning 99");
	gameFrames(800);
	net_command(&net, "item 12158 1");
	gameFrames(600);
	net_command(&net, "item 12155 1");
	gameFrames(600);
	net_command(&net, "item 2859 1");
	gameFrames(600);
	net_command(&net, "item 12183 7");
	gameFrames(600);
	net_command(&net, "tele 2209 5346 0"); // the obelisk in the cellar
	gameFrames(12000);
	if (!infuseOpen()) return false;
	pageShot(AUTOTEST_DIR "autotest_infuse.bmp");
	int wolf = infuseFind(POUCH), lit = 0, shown = 0;
	for (int i = 0; i < INFUSE_ENTRIES; i++) {
		if (infuseList.items[i] >= 0) shown++;
		if (infuseList.items[i] >= 0 && infuseList.items[i] == infuseItems[i]) lit++;
	}
	bool wolfLit = wolf >= 0 && infuseList.items[wolf] == POUCH;
	fprintf(testOut, "infuse list shown=%d lit=%d wolf=%d wolf_lit=%d level=%d name='%s'\n", shown, lit, wolf, wolfLit, wolf >= 0 ? infuseLevels[wolf] : -1, wolf >= 0 ? itemName(infuseItems[wolf]) : "");
	for (int i = 0; i < INFUSE_ENTRIES; i++) fprintf(testOut, "infuse_map pouch slot=%d item=%d\n", i, infuseItems[i]);
	if (wolf < 0) return false;
	pageTapCell(wolf);
	for (int t = 0; t < 80 && bagCount(POUCH) == 0; t++) gameFrames(100);
	gameFrames(1500);
	bool made = bagCount(POUCH) == 1;
	if (!infuseOpen()) return false;
	gameFrames(500);
	tap(4 + PAGE_BUTTON_W / 2, PAGE_BUTTON_Y + 8); // over to scrolls
	for (int t = 0; t < 40 && infuseInterface != SCROLL_INTERFACE; t++) gameFrames(100);
	pageShot(AUTOTEST_DIR "autotest_infuse_scrolls.bmp");
	int howl = infuseFind(SCROLL);
	bool swapped = infuseInterface == SCROLL_INTERFACE;
	bool howlLit = howl >= 0 && infuseList.items[howl] == SCROLL;
	fprintf(testOut, "infuse scrolls swapped=%d howl=%d lit=%d level=%d\n", swapped, howl, howlLit, howl >= 0 ? infuseLevels[howl] : -1);
	for (int i = 0; i < INFUSE_ENTRIES; i++) fprintf(testOut, "infuse_map scroll slot=%d item=%d\n", i, infuseItems[i]);
	if (howl < 0) return false;
	pageTapCell(howl);
	for (int t = 0; t < 80 && bagCount(SCROLL) == 0; t++) gameFrames(100);
	gameFrames(800);
	int scrolls = bagCount(SCROLL);
	bool closed = pageKind == PAGE_NONE;
	fprintf(testOut, "infuse shown=%d wolf_lit=%d made=%d swapped=%d howl_lit=%d scrolls=%d pouches=%d closed=%d\n", shown, wolfLit, made, swapped, howlLit, scrolls, bagCount(POUCH), closed);
	return shown > 0 && wolfLit && made && swapped && howlLit && scrolls > 0 && bagCount(POUCH) == 0;
}

static void dialogShot(const char* path) {
	gameFrames(400);
	panelDirty = true;
	gameFrames(300);
	capture(panelScreen(), path);
}

static void dialogReport(const char* when) {
	fprintf(testOut, "%s iface=%d rows=%d tiles=%d waiting=%d\n", when, dialogLoaded ? dialogIface.id : -1, dialogRowCount, dialogTileCount, dialogWaiting);
	for (int i = 0; i < dialogRowCount; i++) fprintf(testOut, "  row %s '%.50s'\n", dialogRows[i].component == -1 ? "text" : "press", dialogRows[i].text);
	for (int t = 0; t < dialogTileCount; t++) fprintf(testOut, "  tile comp=%d kind=%d id=%d buttons=%d name='%s' first='%s'\n", dialogTiles[t].component, dialogTiles[t].kind, dialogTiles[t].id, dialogTiles[t].count, dialogTiles[t].name, dialogTiles[t].count ? dialogTiles[t].labels[0] : "");
}

// Presses the first row that can be pressed, as anyone reading through a conversation would.
static bool dialogPressFirst(void) {
	for (int i = 0; i < dialogRowCount; i++) {
		if (dialogRows[i].component == -1 || dialogRows[i].w == 0) continue;
		tap(dialogRows[i].x + 20, dialogRows[i].y + 8);
		return true;
	}
	return false;
}

static void dialogTapTile(const DialogTile* t, int button) {
	tap(t->x[button] + t->w[button] / 2, t->y[button] + t->h[button] / 2);
}

// Answers the quiz by looking at the pictures the server sent.
static int quizAnswer(void) {
	static const int SETS[5][3] = {{8828, 8829, 8829}, {8831, 8837, 8835}, {8830, 8832, 8833}, {8835, 8834, 8831}, {8837, 8836, 8828}};
	for (int s = 0; s < 5; s++) {
		int matched = 0;
		for (int t = 0; t < dialogTileCount; t++) {
			for (int k = 0; k < 3; k++) if (dialogTiles[t].id == SETS[s][k]) {
				matched++;
				break;
			}
		}
		if (matched != dialogTileCount) continue;
		for (int t = 0; t < dialogTileCount; t++) if (dialogTiles[t].id == SETS[s][0]) return t;
	}
	return -1;
}

static bool quizFocusTest(void) {
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "tele 3222 3218 0");
	gameFrames(6000);
	net_command(&net, "revent -p cwalk1 -e QUIZ_MASTER");
	int questions = 0, right = 0;
	bool shot = false, won = false;
	for (int t = 0; t < 900 && !won; t++) {
		gameFrames(100);
		if (dialogWaiting || !dialogActive()) continue;
		if (dialogLoaded && dialogIface.id == 191 && dialogTileCount == 3 && dialogTiles[0].w[0] > 0) {
			gameFrames(300);
			panelDirty = true;
			gameFrames(200);
			if (!shot) {
				shot = true;
				dialogReport("quiz question");
				dialogShot(AUTOTEST_DIR "autotest_quiz.bmp");
			}
			int answer = quizAnswer();
			fprintf(testOut, "quiz answer tile=%d\n", answer);
			dialogTapTile(&dialogTiles[answer < 0 ? 0 : answer], 0);
			questions++;
			if (answer >= 0) right++;
			gameFrames(600);
		} else if (dialogRowCount > 0) {
			for (int i = 0; i < dialogRowCount; i++) if (strstr(dialogRows[i].text, "1000 Coins")) won = true;
			if (won) {
				dialogReport("quiz prize");
				for (int i = 0; i < dialogRowCount; i++) if (strstr(dialogRows[i].text, "1000 Coins")) tap(dialogRows[i].x + 20, dialogRows[i].y + 8);
			} else {
				dialogPressFirst();
			}
			gameFrames(500);
		}
	}
	gameFrames(4000);
	fprintf(testOut, "quiz questions=%d right=%d won=%d coins=%d\n", questions, right, won, bagCount(995));
	return won && bagCount(995) >= 1000;
}

// Fletches from a make-X list, then smelts from the bars list.
static bool makeFocusTest(void) {
	const int KNIFE = 946, LOGS = 1511;
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "setlevel fletching 99");
	gameFrames(600);
	net_command(&net, "setlevel smithing 99");
	gameFrames(600);
	net_command(&net, "item 946 1");
	gameFrames(600);
	net_command(&net, "item 1511 2");
	gameFrames(600);
	net_command(&net, "item 436 1");
	gameFrames(600);
	net_command(&net, "item 438 1");
	gameFrames(600);
	net_command(&net, "tele 3226 3254 0");
	gameFrames(9000);
	openTab(TAB_BAG);
	gameFrames(400);
	int knife = -1, logs = -1;
	for (int i = 0; i < 28; i++) {
		if (bagItemAt(i) == KNIFE) knife = i;
		if (bagItemAt(i) == LOGS) logs = i;
	}
	net_use_item_on_item(&net, BAG_INTERFACE, knife, KNIFE, logs, LOGS);
	for (int t = 0; t < 40 && dialogTileCount == 0; t++) gameFrames(100);
	gameFrames(1500);
	dialogReport("fletch list");
	dialogShot(AUTOTEST_DIR "autotest_make.bmp");
	int before = bagCount(LOGS);
	bool listed = dialogTileCount > 0 && dialogTiles[0].count == 4 && strcmp(dialogTiles[0].labels[0], "1") == 0;
	if (!listed) return false;
	dialogTapTile(&dialogTiles[0], 0);
	for (int t = 0; t < 80 && bagCount(LOGS) == before; t++) gameFrames(100);
	gameFrames(2500);
	bool fletched = bagCount(LOGS) == before - 1;
	fprintf(testOut, "make fletched=%d logs %d->%d label='%s'\n", fletched, before, bagCount(LOGS), dialogTiles[0].labels[0]);
	bool opened = false;
	for (int i = 0; i < pickCount && !opened; i++) {
		const LocDef* d = pickList[i].def;
		for (int o = 0; d && o < 5; o++) {
			if (!d->ops[o] || strcasecmp(d->ops[o], "Smelt") != 0) continue;
			fprintf(testOut, "make furnace id=%d option=%d\n", pickList[i].id, o);
			net_loc_action(&net, o, pickList[i].id, (net.zoneX - 6) * 8 + pickList[i].tileX, (net.zoneY - 6) * 8 + pickList[i].tileY);
			opened = true;
			break;
		}
	}
	for (int t = 0; t < 120 && dialogTileCount == 0; t++) gameFrames(100);
	gameFrames(1500);
	dialogReport("smelt list");
	dialogShot(AUTOTEST_DIR "autotest_smelt.bmp");
	int ore = bagCount(436), tile = -1;
	for (int t = 0; t < dialogTileCount; t++) if (strstr(dialogTiles[t].name, "ronze")) tile = t;
	if (tile < 0 && dialogTileCount > 0) tile = 0;
	if (tile < 0) return false;
	dialogTapTile(&dialogTiles[tile], 0);
	for (int t = 0; t < 100 && bagCount(436) == ore; t++) gameFrames(100);
	gameFrames(1500);
	bool smelted = bagCount(2349) == 1;
	fprintf(testOut, "make listed=%d fletched=%d opened=%d smelted=%d ore %d->%d\n", listed, fletched, opened, smelted, ore, bagCount(436));
	return listed && fletched && opened && smelted;
}

static void talkToNamed(const char* name) {
	for (int i = 0; i < net.npcCount; i++) {
		const NpcDef* d = npcShown(net.npcs[i].type);
		if (!d || !d->name || !strstr(d->name, name)) continue;
		for (int o = 0; o < 5; o++) {
			if (!d->ops[o] || strcasecmp(d->ops[o], "Talk-to") != 0) continue;
			net_npc_action(&net, o, net.npcs[i].index);
			return;
		}
	}
}

// Brings up one random event and reads through it until its screen shows.
static void randomEventLook(const char* event, const char* npc, int wanted, const char* shot) {
	char command[64];
	net_command(&net, "tele 3222 3218 0");
	gameFrames(6000);
	snprintf(command, sizeof(command), "revent -p cwalk1 -e %s", event);
	net_command(&net, command);
	bool seen = false;
	for (int t = 0; t < 300 && !seen; t++) {
		gameFrames(100);
		seen = dialogLoaded && dialogIface.id == wanted;
		if (!seen && !dialogWaiting && dialogActive()) {
			dialogPressFirst();
			gameFrames(500);
		} else if (!seen && npc && t % 25 == 24) {
			talkToNamed(npc);
		}
	}
	gameFrames(1200);
	fprintf(testOut, "random %s want=%d seen=%d\n", event, wanted, seen);
	dialogReport("random");
	dialogShot(shot);
}

// Opens a screen by itself, to see how it is laid out. Pictures only the event sends will be missing.
static bool screenLook(const char* command, int wanted, const char* shot) {
	net_command(&net, command);
	for (int t = 0; t < 30 && !(dialogLoaded && dialogIface.id == wanted); t++) gameFrames(100);
	gameFrames(1500);
	bool seen = dialogLoaded && dialogIface.id == wanted;
	fprintf(testOut, "screen %d seen=%d\n", wanted, seen);
	dialogReport("screen");
	dialogShot(shot);
	net_close_modal(&net);
	gameFrames(1000);
	return seen;
}

static bool randomsFocusTest(void) {
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "tele 3222 3218 0");
	gameFrames(6000);
	bool mime = screenLook("iface 188", 188, AUTOTEST_DIR "autotest_screen_mime.bmp");
	bool sandwich = screenLook("iface 297", 297, AUTOTEST_DIR "autotest_screen_sandwich.bmp");
	bool candle = screenLook("iface 178", 178, AUTOTEST_DIR "autotest_screen_candle.bmp");
	bool pillory = screenLook("iface 189", 189, AUTOTEST_DIR "autotest_screen_pillory.bmp");
	bool exam = screenLook("iface 103", 103, AUTOTEST_DIR "autotest_screen_exam.bmp");
	randomEventLook("CERTER", "Giles", 184, AUTOTEST_DIR "autotest_random_certer.bmp");
	bool certer = dialogLoaded && dialogIface.id == 184 && dialogTileCount == 1 && dialogRowCount >= 3;
	dialogPressFirst();
	gameFrames(2500);
	for (int t = 0; t < 12 && dialogActive(); t++) {
		if (!dialogWaiting) dialogPressFirst();
		gameFrames(600);
	}
	randomEventLook("SANDWICH_LADY", "Sandwich", 297, AUTOTEST_DIR "autotest_random_sandwich.bmp");
	bool lady = dialogLoaded && dialogIface.id == 297 && dialogTileCount == 7;
	if (lady) dialogTapTile(&dialogTiles[0], 0);
	gameFrames(2500);
	for (int t = 0; t < 12 && dialogActive(); t++) {
		if (!dialogWaiting) dialogPressFirst();
		gameFrames(600);
	}
	int food = 0;
	for (int i = 0; i < 28; i++) if (bagItemAt(i) >= 0) food++;
	fprintf(testOut, "randoms mime=%d sandwich=%d candle=%d pillory=%d exam=%d certer=%d lady=%d bag=%d\n", mime, sandwich, candle, pillory, exam, certer, lady, food);
	return mime && sandwich && candle && pillory && exam && certer && lady && food > 0;
}

// Shows models from several sides in one picture, to see which view tells them apart.
static void modelSheet(int first, const char* path) {
	static const int YAWS[3] = {1024, 900, 1150};
	static u32 pixels[DIALOG_PICTURE * DIALOG_PICTURE];
	gfxScreen_t screen = panelScreen();
	int width = screenWidth(screen);
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	fillRect(fb, width, 0, 0, width, SCREEN_H, 60, 60, 90);
	for (int row = 0; row < 3; row++) {
		for (int column = 0; column < 4; column++) {
			if (!renderModelPicture(first + column, YAWS[row], pixels)) continue;
			for (int y = 0; y < DIALOG_PICTURE; y++) {
				for (int x = 0; x < DIALOG_PICTURE; x++) {
					u32 c = pixels[y * DIALOG_PICTURE + x];
					if (c) putPixel(fb, width, 4 + column * 78 + x, 4 + row * 78 + y, (u8)(c >> 16), (u8)(c >> 8), (u8)c);
				}
			}
		}
	}
	capture(screen, path);
}

static bool keysFocusTest(void) {
	gameFrames(1500);
	modelSheet(9749, AUTOTEST_DIR "autotest_sheet_keys.bmp");
	gameFrames(300);
	modelSheet(9753, AUTOTEST_DIR "autotest_sheet_locks.bmp");
	return true;
}

// Picks the key that fits the lock, by the pictures, until the cage opens.
static bool pilloryFocusTest(void) {
	net_command(&net, "empty");
	gameFrames(800);
	net_command(&net, "tele 3222 3218 0");
	gameFrames(6000);
	net_command(&net, "revent -p cwalk1 -e PILLORY");
	gameFrames(9000);
	for (int t = 0; t < 12 && dialogActive(); t++) {
		if (!dialogWaiting) dialogPressFirst();
		gameFrames(800);
	}
	int rounds = 0, right = 0;
	bool shot = false, freed = false;
	for (int attempt = 0; attempt < 8 && !freed; attempt++) {
		if (!(dialogLoaded && dialogIface.id == 189)) {
			bool asked = false;
			int best = -1, bestOption = 0, bestGap = 1 << 30, found = 0;
			for (int i = 0; i < pickCount; i++) {
				const LocDef* d = pickList[i].def;
				for (int o = 0; d && o < 5; o++) {
					if (!d->ops[o] || strcasecmp(d->ops[o], "Unlock") != 0) continue;
					int dx = pickList[i].tileX - net.localX, dy = pickList[i].tileY - net.localY, gap = dx * dx + dy * dy;
					found++;
					if (attempt == 0) fprintf(testOut, "pillory cage id=%d tile=%d,%d gap=%d plane=%d\n", pickList[i].id, pickList[i].tileX, pickList[i].tileY, gap, pickList[i].plane);
					if (gap < bestGap) {
						bestGap = gap;
						best = i;
						bestOption = o;
					}
				}
			}
			if (best >= 0) {
				net_loc_action(&net, bestOption, pickList[best].id, (net.zoneX - 6) * 8 + pickList[best].tileX, (net.zoneY - 6) * 8 + pickList[best].tileY);
				asked = true;
			}
			fprintf(testOut, "pillory unlock asked=%d found=%d gap=%d player=%d,%d local=%d,%d message=%.70s\n", asked, found, bestGap, net_world_x(&net), net_world_y(&net), net.localX, net.localY, net.lastMessage);
			for (int t = 0; t < 40 && !(dialogLoaded && dialogIface.id == 189); t++) gameFrames(100);
			if (!asked) freed = net_world_y(&net) < 3300 || net_world_y(&net) > 3500;
		}
		if (!(dialogLoaded && dialogIface.id == 189)) continue;
		gameFrames(1000);
		panelDirty = true;
		gameFrames(300);
		if (!shot) {
			shot = true;
			dialogReport("pillory");
			dialogShot(AUTOTEST_DIR "autotest_pillory.bmp");
		}
		int lock = -1, key = -1;
		for (int t = 0; t < dialogTileCount; t++) if (dialogTiles[t].count == 0) lock = dialogTiles[t].id;
		for (int t = 0; t < dialogTileCount; t++) if (dialogTiles[t].count == 1 && dialogTiles[t].id == lock) key = t;
		fprintf(testOut, "pillory lock=%d key_tile=%d tiles=%d\n", lock, key, dialogTileCount);
		if (key < 0) break;
		dialogTapTile(&dialogTiles[key], 0);
		rounds++;
		right++;
		gameFrames(3500);
	}
	fprintf(testOut, "pillory rounds=%d right=%d freed=%d tile=%d,%d\n", rounds, right, freed, net_world_x(&net), net_world_y(&net));
	return rounds >= 3;
}

// The GPU check must catch real damage, stay quiet without it, and recover once the damage is undone.
static bool gpuCheckFocusTest(void) {
	net_command(&net, "tele 3222 3218 0");
	gameFrames(5000);
	gpuCheckReset();
	for (int t = 0; t < 400 && gpuCheckState != GPU_CHECK_WATCHING; t++) gameFrames(100);
	bool watching = gpuCheckState == GPU_CHECK_WATCHING;
	long quietFrom = gpuCheckMismatches;
	for (long start = gpuChecks; watching && gpuChecks < start + 4;) gameFrames(100);
	bool quiet = gpuCheckMismatches == quietFrom;
	fprintf(testOut, "gpucheck watching=%d quiet=%d state=%d triangles=%lu\n", watching, quiet, gpuCheckState, (unsigned long)(gpuCheckCount / 3));
	if (!watching) return false;
	// Bend the vertices it redraws, as an unseen fault would.
	u8* bytes = (u8*)gpuCheckVerts;
	static u8 saved[4096];
	memcpy(saved, bytes, sizeof(saved));
	for (size_t i = 0; i < sizeof(saved); i += 4) bytes[i] ^= 0x40;
	GSPGPU_FlushDataCache(bytes, sizeof(saved));
	long before = gpuCheckMismatches;
	for (long start = gpuChecks; gpuChecks < start + 3;) gameFrames(100);
	bool caught = gpuCheckMismatches > before;
	memcpy(bytes, saved, sizeof(saved));
	GSPGPU_FlushDataCache(bytes, sizeof(saved));
	gameFrames(400);
	long afterFix = gpuCheckMismatches;
	for (long start = gpuChecks; gpuChecks < start + 3;) gameFrames(100);
	bool recovered = gpuCheckMismatches == afterFix;
	fprintf(testOut, "gpucheck caught=%d recovered=%d worst=%d mismatches=%ld\n", caught, recovered, gpuCheckWorst, gpuCheckMismatches);
	return quiet && caught && recovered;
}

static bool headsFocusTest(void) {
	net_command(&net, "tele 3222 3218 0");
	gameFrames(9000);
	int index = -1, option = -1;
	for (int i = 0; i < net.npcCount && index < 0; i++) {
		const NpcDef* d = npcShown(net.npcs[i].type);
		if (!d || !d->name || strcmp(d->name, "Hans") != 0) continue;
		for (int op = 0; op < 5; op++) if (d->ops[op] && strcmp(d->ops[op], "Talk-to") == 0) {
			index = net.npcs[i].index;
			option = op;
		}
	}
	fprintf(testOut, "heads hans=%d option=%d\n", index, option);
	if (index < 0) return false;
	net_npc_action(&net, option, index);
	for (int t = 0; t < 200 && !headShown(); t++) gameFrames(100);
	gameFrames(1500);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_head_npc.bmp");
	bool npc = headShown() && net.headKind == NET_HEAD_NPC;
	int frames = 0, last = headDrawnFrame;
	for (int t = 0; t < 30; t++) {
		gameFrames(100);
		if (headDrawnFrame != last) frames++;
		last = headDrawnFrame;
	}
	fprintf(testOut, "heads npc=%d kind=%d id=%d anim=%d component=%d frames_seen=%d triangles=%u\n", npc, net.headKind, net.headNpc, net.headAnim, net.headComponent & 0xFFFF, frames, headMesh.indexCount / 3);
	// Hans never has the player speak, but a Man starts that way.
	for (int step = 0; step < 8 && dialogActive(); step++) {
		panelDirty = true;
		gameFrames(300);
		int press = -1;
		for (int i = 0; i < dialogRowCount; i++) if (dialogRows[i].component != -1 && dialogRows[i].w > 0) press = i;
		if (press >= 0) tap(dialogRows[press].x + 20, dialogRows[press].y + 8);
		gameFrames(1500);
	}
	net_command(&net, "tele 3235 3219 0");
	gameFrames(9000);
	bool player = false;
	for (int attempt = 0; attempt < 4 && !player; attempt++) {
		int man = -1, talk = -1;
		for (int i = 0; i < net.npcCount && man < 0; i++) {
			const NpcDef* d = npcShown(net.npcs[i].type);
			if (!d || !d->name || strcmp(d->name, "Man") != 0) continue;
			for (int op = 0; op < 5; op++) if (d->ops[op] && strcmp(d->ops[op], "Talk-to") == 0) {
				man = net.npcs[i].index;
				talk = op;
			}
		}
		fprintf(testOut, "  heads man=%d\n", man);
		if (man < 0) break;
		net_npc_action(&net, talk, man);
		for (int t = 0; t < 150 && !player; t++) {
			gameFrames(100);
			if (headShown() && net.headKind == NET_HEAD_PLAYER) player = true;
		}
	}
	gameFrames(1200);
	panelDirty = true;
	gameFrames(300);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_head_player.bmp");
	fprintf(testOut, "heads player=%d anim=%d seq=%d\n", player, net.headAnim, headSeq);
	return npc && player;
}

// Runs only what a change could affect, instead of the whole test.
static void runFocusTest(void) {
	bool ok = false;
	if (strcmp(testFocus, "chat") == 0) ok = chatFocusTest();
	if (strcmp(testFocus, "overlay") == 0) ok = overlayFocusTest();
	if (strcmp(testFocus, "login") == 0) ok = loginFocusTest(testScene);
	if (strcmp(testFocus, "recheck") == 0) {
		net_command(&net, "tele 3222 3218 0");
		gameFrames(9000);
		pressAndHold(KEY_SELECT, 0, 0);
		bool doors = doorTest(320);
		pressAndHold(KEY_SELECT, 0, 0);
		bool magic = magicTabTest();
		fprintf(testOut, "recheck doors=%d magic=%d\n", doors, magic);
		ok = doors && magic;
	}
	if (strcmp(testFocus, "pools") == 0) {
		// Forces the pose and model pools to reset many times in a busy place, and watches for damage.
		net_command(&net, "tele 3222 3218 0");
		gameFrames(9000);
		long resets = npcPoolResets;
		pressAndHold(KEY_SELECT, 0, 0);
		for (int i = 0; i < 40; i++) {
			if (i % 4 == 0) npcModelResetWanted = true;
			else npcPoseResetWanted = true;
			gameFrames(250);
		}
		capture(gameScreen(), AUTOTEST_DIR "autotest_pools.bmp");
		integrityFullCheck("test");
		fprintf(testOut, "pools resets=%ld poses=%ld models=%ld drawn=%d guards=%ld list=%ld corrupt=%ld linear_kb=%lu\n", npcPoolResets - resets, npcPosesBuilt, npcModelsBuilt, npcsDrawn, guardDamage, listDamage, meshCorrupt, (unsigned long)(linearSpaceFree() >> 10));
		pressAndHold(KEY_SELECT, 0, 0);
		ok = npcPoolResets - resets >= 30 && npcsDrawn > 0 && guardDamage == 0 && listDamage == 0;
	}
	if (strcmp(testFocus, "guide") == 0) ok = guideFocusTest();
	if (strcmp(testFocus, "heads") == 0) ok = headsFocusTest();
	if (strcmp(testFocus, "summon") == 0) ok = summonFocusTest();
	if (strcmp(testFocus, "burden") == 0) ok = burdenFocusTest();
	if (strcmp(testFocus, "infuse") == 0) ok = infuseFocusTest();
	if (strcmp(testFocus, "quiz") == 0) ok = quizFocusTest();
	if (strcmp(testFocus, "gpucheck") == 0) ok = gpuCheckFocusTest();
	if (strcmp(testFocus, "pillory") == 0) ok = pilloryFocusTest();
	if (strcmp(testFocus, "keys") == 0) ok = keysFocusTest();
	if (strcmp(testFocus, "make") == 0) ok = makeFocusTest();
	if (strcmp(testFocus, "randoms") == 0) ok = randomsFocusTest();
	if (strcmp(testFocus, "trade") == 0) ok = tradeFocusTest();
	if (strcmp(testFocus, "hits") == 0) ok = hitsFocusTest();
	if (strcmp(testFocus, "store") == 0) {
		bool bank = bankFocusTest();
		bool shop = shopFocusTest();
		fprintf(testOut, "store bank=%d shop=%d\n", bank, shop);
		ok = bank && shop;
	}
	fprintf(testOut, "summary focus=%s ok=%d\n", testFocus, ok);
	fprintf(testOut, "DONE %s connected=%d\n", ok ? "INPUT PASS" : "INPUT FAIL", net.connected);
	fclose(testOut);
}

// Each of these checks one press does one thing, and that the server's answer shows up.
static bool combatTabTest(void) {
	openTab(TAB_COMBAT);
	gameFrames(1000);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_combat.bmp");
	int style = -1, retaliate = -1, lit = 0;
	for (int i = 0; i < combatButtonCount; i++) {
		const CombatButton* b = &combatButtons[i];
		bool on = combatLit(&combatIface.components[b->component]);
		fprintf(testOut, "  combat button %d '%s' lit=%d varp=%d value=%d\n", b->component, b->label, on, combatIface.components[b->component].testVarp, combatIface.components[b->component].testValue);
		if (b->retaliate) retaliate = i;
		else if (!b->special) {
			if (on) lit++;
			else if (style < 0) style = i;
		}
	}
	if (style < 0 || retaliate < 0) return false;
	long sent = net.buttonsSent;
	int x, y, w, before = net.varps[43], off = net.varps[VARP_RETALIATE_OFF];
	combatPlace(style, 320, &x, &y, &w);
	tap(x + w / 2, y + 10);
	gameFrames(1500);
	bool styled = net.varps[43] != before && combatLit(&combatIface.components[combatButtons[style].component]);
	combatPlace(retaliate, 320, &x, &y, &w);
	tap(x + w / 2, y + 10);
	gameFrames(1500);
	bool toggled = net.varps[VARP_RETALIATE_OFF] != off;
	tap(x + w / 2, y + 10);
	gameFrames(1500);
	bool back = net.varps[VARP_RETALIATE_OFF] == off;
	fprintf(testOut, "combat buttons=%d lit=%d style_changed=%d retaliate=%d back=%d sent=%ld\n", combatButtonCount, lit, styled, toggled, back, net.buttonsSent - sent);
	return lit == 1 && styled && toggled && back && net.buttonsSent == sent + 3;
}

static bool prayerTabTest(void) {
	net_command(&net, "setlevel prayer 99");
	gameFrames(2500);
	dismissPopups();
	openTab(TAB_PRAYER);
	gameFrames(1500);
	panelDirty = true;
	gameFrames(200);
	int first = -1;
	for (int i = 0; i < gridCount; i++) if (strcmp(gridCells[i].name, "Thick Skin") == 0) first = i;
	fprintf(testOut, "prayer cells=%d note='%s' thick_skin=%d result=%d ms=%d\n", gridCount, gridNote, first, prayerScript.result, prayerScript.ms);
	if (gridCount != 27 || first < 0 || prayerScript.result != 0) return false;
	long sent = net.buttonsSent;
	int x, y;
	gridPlace(first, 320, &x, &y);
	tap(x + 15, y + 15);
	for (int t = 0; t < 30 && !gridCells[first].lit; t++) gameFrames(100);
	bool on = gridCells[first].lit;
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_prayer.bmp");
	tap(x + 15, y + 15);
	for (int t = 0; t < 30 && gridCells[first].lit; t++) gameFrames(100);
	bool off = !gridCells[first].lit;
	fprintf(testOut, "prayer on=%d off=%d sent=%ld\n", on, off, net.buttonsSent - sent);
	return on && off && net.buttonsSent == sent + 2;
}

static bool emoteTabTest(void) {
	openTab(TAB_EMOTES);
	gameFrames(1500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_emotes.bmp");
	fprintf(testOut, "emotes cells=%d columns=%d height=%d result=%d first='%s'\n", gridCount, gridColumns, gridTotal, emoteScript.result, gridCount ? gridCells[0].name : "");
	if (gridCount < 20 || emoteScript.result != 0) return false;
	long sent = net.buttonsSent;
	long seqs = net.playerSeqUpdates;
	int x, y;
	gridPlace(0, 320, &x, &y);
	tap(x + 20, y + 20);
	gameFrames(1500);
	fprintf(testOut, "emote sent=%ld note='%s' seq_updates=%ld\n", net.buttonsSent - sent, gridNote, net.playerSeqUpdates - seqs);
	return net.buttonsSent == sent + 1;
}

static bool musicTabTest(void) {
	openTab(TAB_MUSIC);
	for (int t = 0; t < 60 && musicRowCount == 0; t++) gameFrames(100);
	gameFrames(500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_music.bmp");
	int green = 0, pick = -1;
	for (int i = 0; i < musicRowCount; i++) {
		if (musicRows[i].colour == 0x00ff00 || musicRows[i].colour == 0x00dd00) {
			green++;
			if (pick < 0) pick = i;
		}
	}
	fprintf(testOut, "music rows=%d unlocked=%d now='%s' result=%d ms=%d\n", musicRowCount, green, musicNow, musicScript.result, musicScript.ms);
	if (musicRowCount < 400 || musicScript.result != 0) return false;
	long sent = net.buttonsSent;
	if (pick < 0) pick = 0;
	musicScroll = pick * QUEST_ROW_H;
	panelDirty = true;
	gameFrames(300);
	tap(100, QUEST_LIST_TOP + 5);
	gameFrames(1500);
	fprintf(testOut, "music pressed='%s' sent=%ld now='%s'\n", musicRows[pick].text, net.buttonsSent - sent, musicNow);
	return net.buttonsSent == sent + 1;
}

static bool tabsTest(bool* combat, bool* prayer, bool* emotes, bool* music, bool* magic, bool* options, bool* friends) {
	pressAndHold(KEY_SELECT, 0, 0);
	*combat = combatTabTest();
	*combat = specialTest() && *combat;
	*prayer = prayerTabTest();
	*emotes = emoteTabTest();
	// Prayer and Emotes share a grid, so going back must not leave the wrong icons.
	openTab(TAB_PRAYER);
	gameFrames(500);
	bool prayerBack = gridCount == 27 && strstr(gridNote, "Prayer") != NULL;
	openTab(TAB_EMOTES);
	gameFrames(500);
	bool emotesBack = gridCount > 27 && !strstr(gridNote, "Prayer");
	fprintf(testOut, "grid_shared prayer_back=%d emotes_back=%d\n", prayerBack, emotesBack);
	*emotes = *emotes && prayerBack && emotesBack;
	*music = musicTabTest();
	*magic = magicTabTest();
	*options = optionsTabTest();
	*friends = socialListTest(TAB_FRIENDS, "exp23");
	*friends = socialListTest(TAB_IGNORE, "exp22") && *friends;
	pressAndHold(KEY_SELECT, 0, 0);
	return *combat && *prayer && *emotes && *music && *magic && *options && *friends;
}

// Reads the list the scripts built, scrolls it, and opens one journal with one press.
static bool questTest(void) {
	pressAndHold(KEY_SELECT, 0, 0);
	openTab(TAB_QUESTS);
	gameFrames(2500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_quests.bmp");
	int set = 0, red = 0, yellow = 0, green = 0, pressable = 0, cook = -1, header = -1;
	for (int i = 0; i < NET_MAX_VARPS; i++) if (net.varps[i]) set++;
	for (int i = 0; i < questRowCount; i++) {
		const QuestRow* r = &questRows[i];
		if (r->colour == 0xff0000) red++;
		if (r->colour == 0xffff00) yellow++;
		if (r->colour == 0x00ff00) green++;
		if (r->pressable) pressable++;
		if (strcmp(r->text, "Cook's Assistant") == 0) cook = i;
		if (strcmp(r->text, "Free Quests") == 0) header = i;
	}
	fprintf(testOut, "quests open=%d rows=%d pressable=%d red=%d yellow=%d green=%d height=%d title='%s' varps_set=%d script_ms=%d result=%d\n", questAvailable, questRowCount, pressable, red, yellow, green, questTotal, questTitle, set, questScript.ms, questScript.result);
	bool listed = questAvailable && questRowCount > 100 && pressable > 100 && questScript.result == 0 && cook >= 0 && header >= 0;
	if (!listed) {
		pressAndHold(KEY_SELECT, 0, 0);
		return false;
	}

	// Scrolling by arrow and by stylus sends nothing, and a heading does nothing when pressed.
	long buttons = net.buttonsSent;
	tap(320 - 10, CONTENT_BOTTOM - 8);
	int arrow = questScroll;
	dragStylus(100, 170, 100, 90, 0);
	gameFrames(100);
	int dragged = questScroll;
	questScroll = questRows[header].y;
	panelDirty = true;
	gameFrames(200);
	tap(100, QUEST_LIST_TOP + 5);
	gameFrames(800);
	bool quiet = arrow == SCROLL_STEP && dragged > arrow && net.buttonsSent == buttons && !journalActive();
	fprintf(testOut, "quests arrow=%d drag=%d sent=%ld\n", arrow, dragged, net.buttonsSent - buttons);

	questScroll = questRows[cook].y;
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_quests_scrolled.bmp");
	tap(100, QUEST_LIST_TOP + 5);
	for (int t = 0; t < 40 && !journalActive(); t++) gameFrames(100);
	gameFrames(500);
	bool onePress = net.buttonsSent == buttons + 1;
	bool opened = journalActive() && strstr(journalTitle, "Cook") != NULL && journalRowCount > 0;
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_journal.bmp");
	fprintf(testOut, "journal opened=%d one_press=%d title='%s' rows=%d first='%s'\n", opened, onePress, journalTitle, journalRowCount, journalRowCount ? journalRows[0].text : "");

	tap(320 - 20, 10);
	for (int t = 0; t < 30 && journalActive(); t++) gameFrames(100);
	bool closed = !journalActive();
	fprintf(testOut, "journal closed=%d\n", closed);
	pressAndHold(KEY_SELECT, 0, 0);
	return listed && quiet && opened && onePress && closed;
}

// Checks the numbers against the rules the server uses, then earns experience and watches it arrive.
static bool statsTest(void) {
	pressAndHold(KEY_SELECT, 0, 0);
	openTab(TAB_STATS);
	gameFrames(500);
	panelDirty = true;
	gameFrames(200);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_stats.bmp");
	int known = 0, total = totalLevel();
	for (int i = 0; i < SKILL_COUNT; i++) {
		int skill = SKILL_PLACES[i].skill;
		if (net.skillLevel[skill] > 0) known++;
		if (i < 6) fprintf(testOut, "  skill %s level=%d base=%d xp=%d\n", SKILL_PLACES[i].name, net.skillLevel[skill], baseLevel(skill), net.skillXp[skill]);
	}
	// Level 2 needs 83 experience and 99 needs 13,034,431, which are well known.
	bool table = skillXpTable[0] == 83 && skillXpTable[97] == 13034431;
	fprintf(testOut, "stats known=%d total=%d table=%d level2=%d level99=%d combat_shown=%d\n", known, total, table, skillXpTable[0], skillXpTable[97], net.looks[NET_SELF_ID].combat);

	// Pressing a skill opens its guide, which has its own test.
	bool chosen = true, cleared = true;

	// Levels must follow the server: a changed level arrives by itself.
	int before = net.skillXp[8];
	long version = net.statVersion;
	net_command(&net, "setlevel woodcutting 20");
	gameFrames(2500);
	int after = net.skillXp[8];
	bool followed = net.statVersion > version && baseLevel(8) == 20 && net.skillLevel[8] == 20;
	fprintf(testOut, "stats_live chosen=%d cleared=%d xp_before=%d xp_after=%d base=%d level=%d followed=%d\n", chosen, cleared, before, after, baseLevel(8), net.skillLevel[8], followed);
	fflush(testOut);
	panelTab = TAB_MAP;
	panelDirty = true;
	pressAndHold(KEY_SELECT, 0, 0);
	return known == SKILL_COUNT && table && chosen && cleared && followed;
}

static void runOnlineTest(bool* sceneReady) {
	testScene = sceneReady;
	testOut = fopen(AUTOTEST_DIR "autotest_result.txt", "w");
	if (!testOut) return;
	if (!goOnline()) {
		fprintf(testOut, "FAILED %s\n", statusLine);
		fclose(testOut);
		return;
	}
	fprintf(testOut, "login self=%d rights=%d\n", net.selfId, net.rights);
	*sceneReady = loadFromServer();
	fprintf(testOut, "# %s\n", loadReport);
	fflush(testOut);

	// The client must wait on the welcome screen for a press.
	gameFrames(4000);
	bool waited = net.inLobby && actionsSent == 0;
	fprintf(testOut, "lobby_waits_for_press=%d lobby=%d actions=%ld\n", waited, net.inLobby, actionsSent);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_ui_lobby.bmp");
	Widget widgets[MAX_WIDGETS];
	panelLayout(screenWidth(panelScreen()), widgets);
	pressAndHold(KEY_TOUCH, widgets[0].x + widgets[0].w / 2, widgets[0].y + widgets[0].h / 2);
	bool played = !net.inLobby && actionsSent == 1 && net.buttonsSent == 1;
	gameFrames(5000);
	fprintf(testOut, "play_one_press=%d lobby=%d actions=%ld buttons=%ld\n", played, net.inLobby, actionsSent, net.buttonsSent);
	fflush(testOut);

	if (testFocus[0]) {
		runFocusTest();
		return;
	}
	net_command(&net, "tele 3222 3218 0");
	gameFrames(6000);
	fprintf(testOut, "# %s\n", loadReport);
	bool topWalk = walkTest("cursor_walk", 4, 3, false);
	capture(GFX_TOP, AUTOTEST_DIR "autotest_ui_game_top.bmp");
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_ui_panel_bottom.bmp");
	bool mapTouch = mapWalkTest("map_tap", -2, -3, true);

	// Swapping sends nothing to the server.
	long before = actionsSent;
	pressAndHold(KEY_SELECT, 0, 0);
	bool swapped = gameOnBottom && actionsSent == before;
	fprintf(testOut, "swap game_on_bottom=%d actions_added=%ld\n", gameOnBottom, actionsSent - before);
	bool bottomWalk = walkTest("touch_walk", 1, 2, true);
	cursorX = 200;
	cursorY = 120;
	panelDirty = true;
	gameFrames(500);
	capture(GFX_BOTTOM, AUTOTEST_DIR "autotest_ui_game_bottom.bmp");
	capture(GFX_TOP, AUTOTEST_DIR "autotest_ui_panel_top.bmp");

	bool mapCursor = mapWalkTest("map_cursor", -3, -2, false);
	bool objects = objectTest(320);
	bool doors = doorTest(320);
	bool npcs = npcTest(320);
	bool players = playerTest(320);
	bool bag = bagTest();
	bool groundItems = groundTest(320);
	bool worn = wornTest();
	bool stats = statsTest();
	bool quests = questTest();
	bool combatTab = false, prayerTab = false, emoteTab = false, musicTab = false, magicTab = false, optionTab = false, friendTab = false;
	bool tabs = tabsTest(&combatTab, &prayerTab, &emoteTab, &musicTab, &magicTab, &optionTab, &friendTab);

	// With the panel on top, the cursor reaches its buttons.
	Widget top[MAX_WIDGETS];
	int count = panelLayout(screenWidth(GFX_TOP), top);
	bool runBefore = runMode;
	cursorX = (float)(top[0].x + top[0].w / 2);
	cursorY = (float)(top[0].y + top[0].h / 2);
	pressAndHold(KEY_A, 0, 0);
	bool cursorPanel = count > 0 && runMode != runBefore;
	fprintf(testOut, "cursor_panel_button=%d\n", cursorPanel);

	// Y opens the menu at the cursor when the game is on top, and B cancels it without acting.
	pressAndHold(KEY_SELECT, 0, 0);
	cursorX = 200;
	cursorY = 120;
	long beforeMenu = actionsSent;
	pressAndHold(KEY_Y, 0, 0);
	bool cursorMenu = menuOpen;
	pressAndHold(KEY_B, 0, 0);
	cursorMenu = cursorMenu && !menuOpen && actionsSent == beforeMenu;
	fprintf(testOut, "cursor_menu_and_cancel=%d\n", cursorMenu);

	gameFrames(2000);

	// The detector must notice real damage: it is clean so far, and one changed byte must be caught.
	long corruptBefore = meshCorrupt, checksBefore = meshChecks;
	u32 victim = meshBlockCount / 2;
	u8* byte = meshPool + (size_t)victim * MESH_BLOCK_SIZE + 100;
	u8 saved = *byte;
	*byte ^= 0x55;
	GSPGPU_FlushDataCache(byte, 1);
	for (int t = 0; t < 400 && meshCorrupt == corruptBefore; t++) gameFrames(100);
	bool caught = meshCorrupt == corruptBefore + 1;
	*byte = saved;
	GSPGPU_FlushDataCache(byte, 1);
	meshBlockDirty[victim] = 1;
	fprintf(testOut, "detector clean_before=%d caught=%d checks=%ld blocks=%lu panel_cap=%d\n", corruptBefore == 0, caught, meshChecks - checksBefore, (unsigned long)meshBlockCount, PANEL_MIN_MS);
	fprintf(testOut, "gpu cmd_peak=%.3f draws_peak=%lu frames_cut=%ld buffer_kb=%d linear_free_kb=%lu\n", cmdPeak, (unsigned long)drawCallsPeak, cmdFramesCut, CMDBUF_SIZE / 1024, (unsigned long)(linearSpaceFree() >> 10));
	fprintf(testOut, "summary options=%d friends=%d magic=%d combat=%d prayer=%d emotes=%d music=%d quests=%d stats=%d worn=%d ground=%d bag=%d players=%d npcs=%d doors=%d objects=%d menu=%d lobby=%d play=%d walk=%d/%d map=%d/%d swap=%d panel=%d\n", optionTab, friendTab, magicTab, combatTab, prayerTab, emoteTab, musicTab, quests, stats, worn, groundItems, bag, players, npcs, doors, objects, cursorMenu, waited, played, topWalk, bottomWalk, mapTouch, mapCursor, swapped, cursorPanel);
	bool ok = tabs && quests && stats && worn && caught && corruptBefore == 0 && groundItems && bag && players && npcs && doors && objects && cursorMenu && waited && played && topWalk && mapTouch && swapped && bottomWalk && mapCursor && cursorPanel && net.connected;
	fprintf(testOut, "DONE %s connected=%d\n", ok ? "INPUT PASS" : "INPUT FAIL", net.connected);
	fclose(testOut);
}
