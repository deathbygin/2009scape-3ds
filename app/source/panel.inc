// The interface panel, drawn in software on whichever screen is not showing the game.
// Temporary contents until real game interfaces exist.

enum { WIDGET_NONE, WIDGET_PLAY, WIDGET_RUN, WIDGET_DISTANCE, WIDGET_DECOR, WIDGET_SWAP, WIDGET_TAB_MAP, WIDGET_TAB_CHAT, WIDGET_LOGOUT, WIDGET_SOCIAL_ADD, WIDGET_SOCIAL_DEL, WIDGET_SOCIAL_MSG, WIDGET_CHAT_SAY, WIDGET_CHAT_PRIVATE, WIDGET_OPTION0, WIDGET_OPTION_END = WIDGET_OPTION0 + 3, WIDGET_TAB0 };

// The tabs of the original interface: seven along the top and seven along the bottom.
enum { TAB_COMBAT, TAB_STATS, TAB_QUESTS, TAB_BAG, TAB_WORN, TAB_PRAYER, TAB_MAGIC,
	TAB_SUMMONING, TAB_FRIENDS, TAB_IGNORE, TAB_CLAN, TAB_OPTIONS, TAB_EMOTES, TAB_MUSIC, TAB_COUNT, TAB_MAP = TAB_COUNT, TAB_CHAT };
#define WIDGET_TAB_BAG (WIDGET_TAB0 + TAB_BAG)
#define WIDGET_TAB_OPTIONS (WIDGET_TAB0 + TAB_OPTIONS)
static int panelTab = TAB_MAP;

typedef struct {
	int x, y, w, h, id;
	char label[16];
} Widget;

#define MAX_WIDGETS 24
#define SCREEN_H 240
#define MAP_CENTRE_X 88
#define MAP_CENTRE_Y 120
#define MAP_RADIUS 80
#define TAB_BAR_W 249
#define TAB_BAR_H 37
#define CONTENT_TOP TAB_BAR_H
#define CONTENT_BOTTOM (240 - TAB_BAR_H)
#define MAP_PIXELS_PER_TILE 4.0f

static int screenWidth(gfxScreen_t screen) {
	return screen == GFX_TOP ? 400 : 320;
}

static gfxScreen_t gameScreen(void) {
	return gameOnBottom ? GFX_BOTTOM : GFX_TOP;
}

static gfxScreen_t panelScreen(void) {
	return gameOnBottom ? GFX_TOP : GFX_BOTTOM;
}

static bool mapVisible(void) {
	return online && !net.inLobby;
}

// A point on the panel relative to the map centre, turned into tiles east and north of the player.
static void mapPointToTiles(int px, int py, float* east, float* north) {
	float c = cosf(yaw), s = sinf(yaw);
	*east = (px * c + py * s) / MAP_PIXELS_PER_TILE;
	*north = (px * s - py * c) / MAP_PIXELS_PER_TILE;
}

static void tilesToMapPoint(float east, float north, int* px, int* py) {
	float c = cosf(yaw), s = sinf(yaw);
	*px = (int)((east * c + north * s) * MAP_PIXELS_PER_TILE);
	*py = (int)((east * s - north * c) * MAP_PIXELS_PER_TILE);
}

// Where each tab sits on its bar, from interface 548. x is relative to the bar, and the bottom bar starts 3 pixels further right.
typedef struct {
	int x, w, stone, iconX, iconY, icon;
	const char* name;
} TabPlace;

static const TabPlace TAB_PLACES[TAB_COUNT] = {
	{6, 38, 1026, 13, 1, 168, "Combat Styles"}, {44, 33, 1030, 44, 0, 898, "Stats"}, {77, 33, 1030, 79, 1, 899, "Quest List"},
	{110, 33, 1030, 111, -1, 900, "Inventory"}, {143, 33, 1030, 143, 2, 901, "Worn Equipment"}, {176, 33, 1030, 176, 0, 902, "Prayer List"},
	{209, 38, 1027, 210, 1, 903, "Magic Spellbook"},
	{3, 38, 1028, 6, 1, 1242, "Follower Details"}, {41, 33, 1030, 42, 2, 904, "Friends List"}, {74, 33, 1030, 75, 2, 905, "Ignore List"},
	{107, 33, 1030, 107, 2, 1241, "Clan Chat"}, {140, 33, 1030, 142, 1, 907, "Options"}, {173, 33, 1030, 175, 3, 908, "Emotes"},
	{206, 38, 1029, 208, 3, 909, "Music Player"},
};

// The settings of the original that mean something here. Its mouse, graphics and audio ones do not, for now.
static const struct {
	int component, varp, onValue;
	const char* name;
} OPTION_ROWS[3] = {{4, 171, 0, "Effects"}, {5, 287, 1, "Split chat"}, {7, 427, 1, "Accept aid"}};

static bool optionOn(int row) {
	return net.varps[OPTION_ROWS[row].varp] == OPTION_ROWS[row].onValue;
}

static int panelLayout(int width, Widget* out) {
	int n = 0;
	if (online && net.inLobby) {
		out[n++] = (Widget){width / 2 - 70, 128, 140, 48, WIDGET_PLAY, "Play"};
		return n;
	}
	if (!online) {
		out[n++] = (Widget){6, 200, width - 12, 34, WIDGET_SWAP, "Swap"};
		return n;
	}
	if (dialogActive() || journalActive() || guideActive() || pageActive()) return 0; // these have the whole screen
	int side = (width - TAB_BAR_W) / 2, right = side + TAB_BAR_W + 1;
	out[n++] = (Widget){right, 1, width - right - 1, TAB_BAR_H - 2, WIDGET_RUN, "Run"};
	out[n++] = (Widget){1, 1, side - 2, TAB_BAR_H - 2, WIDGET_TAB_MAP, "Map"};
	out[n++] = (Widget){1, CONTENT_BOTTOM + 1, side - 2, TAB_BAR_H - 2, WIDGET_SWAP, "Swap"};
	out[n++] = (Widget){right, CONTENT_BOTTOM + 1, width - right - 1, TAB_BAR_H - 2, WIDGET_TAB_CHAT, "Chat"};
	for (int i = 0; i < TAB_COUNT; i++) {
		const TabPlace* t = &TAB_PLACES[i];
		int base = i < 7 ? side : side + 3;
		out[n++] = (Widget){base + t->x, i < 7 ? 0 : CONTENT_BOTTOM, t->w, TAB_BAR_H, WIDGET_TAB0 + i, ""};
	}
	if (panelTab == TAB_OPTIONS) {
		int left = width / 2 - 150, right = width / 2 + 4, top = CONTENT_TOP + 26;
		out[n] = (Widget){left, top, 146, 30, WIDGET_DISTANCE, ""};
		snprintf(out[n++].label, 16, "View: %d", RADII[radiusIndex]);
		out[n] = (Widget){right, top, 146, 30, WIDGET_DECOR, ""};
		snprintf(out[n++].label, 16, "Decor: %s", modeIndex == 0 ? "on" : "off");
		for (int i = 0; i < 3; i++) {
			out[n] = (Widget){(i & 1) ? right : left, top + 36 * (1 + i / 2), 146, 30, WIDGET_OPTION0 + i, ""};
			snprintf(out[n++].label, 16, "%s: %s", OPTION_ROWS[i].name, optionOn(i) ? "On" : "Off");
		}
		out[n++] = (Widget){right, top + 72, 146, 30, WIDGET_LOGOUT, "Log Out"};
	}
	int barY = CONTENT_BOTTOM - 23, third = (width - 16) / 3;
	if (panelTab == TAB_FRIENDS) {
		out[n++] = (Widget){6, barY, third - 2, 21, WIDGET_SOCIAL_ADD, "Add"};
		out[n++] = (Widget){8 + third, barY, third - 2, 21, WIDGET_SOCIAL_MSG, "Message"};
		out[n++] = (Widget){10 + 2 * third, barY, third - 2, 21, WIDGET_SOCIAL_DEL, "Delete"};
	} else if (panelTab == TAB_IGNORE) {
		out[n++] = (Widget){8, barY, width / 2 - 12, 21, WIDGET_SOCIAL_ADD, "Add Name"};
		out[n++] = (Widget){width / 2 + 4, barY, width / 2 - 12, 21, WIDGET_SOCIAL_DEL, "Delete"};
	} else if (panelTab == TAB_CLAN) {
		out[n] = (Widget){width / 2 - 70, barY, 140, 21, WIDGET_SOCIAL_ADD, ""};
		snprintf(out[n++].label, 16, "%s", net.clanOwner ? "Leave Chat" : "Join Chat");
	} else if (panelTab == TAB_CHAT) {
		out[n++] = (Widget){8, barY, width / 2 - 12, 21, WIDGET_CHAT_SAY, "Say"};
		out[n++] = (Widget){width / 2 + 4, barY, width / 2 - 12, 21, WIDGET_CHAT_PRIVATE, "Private"};
	}
	return n;
}

static void putPixel(u8* fb, int width, int x, int y, u8 r, u8 g, u8 b) {
	if (x < 0 || y < 0 || x >= width || y >= SCREEN_H) return;
	u8* p = fb + (x * SCREEN_H + (SCREEN_H - 1 - y)) * 3;
	p[0] = b;
	p[1] = g;
	p[2] = r;
}

static void fillRect(u8* fb, int width, int x, int y, int w, int h, u8 r, u8 g, u8 b) {
	for (int i = x; i < x + w; i++)
		for (int j = y; j < y + h; j++) putPixel(fb, width, i, j, r, g, b);
}

static void drawText(u8* fb, int width, int x, int y, const char* text, u8 r, u8 g, u8 b) {
	const ConsoleFont* font = &consoleGetDefault()->font;
	for (; *text; text++, x += 8) {
		int c = (unsigned char)*text - font->asciiOffset;
		if (c < 0 || c >= font->numChars) continue;
		const u8* glyph = font->gfx + c * 8;
		for (int row = 0; row < 8; row++)
			for (int col = 0; col < 8; col++)
				if (glyph[row] & (0x80 >> col)) putPixel(fb, width, x + col, y + row, r, g, b);
	}
}

static void drawLine(u8* fb, int width, int x, int* y, const char* text) {
	drawText(fb, width, x, *y, text, 230, 215, 170);
	*y += 11;
}

static void drawMap(u8* fb, int width) {
	float tileX = playerViewX, tileY = -playerViewZ;
	float c = cosf(yaw), s = sinf(yaw);
	for (int py = -MAP_RADIUS; py <= MAP_RADIUS; py++) {
		for (int px = -MAP_RADIUS; px <= MAP_RADIUS; px++) {
			if (px * px + py * py > MAP_RADIUS * MAP_RADIUS) continue;
			float east = (px * c + py * s) / MAP_PIXELS_PER_TILE, north = (px * s - py * c) / MAP_PIXELS_PER_TILE;
			int ix = (int)((tileX + east) * MAP_PIXELS_PER_TILE), iy = (int)((104.0f - (tileY + north)) * MAP_PIXELS_PER_TILE);
			u8 r = 0, g = 0, b = 0;
			if (ix >= 0 && iy >= 0 && ix < MAP_TILES_PX && iy < MAP_TILES_PX) {
				u16 v = mapImage[iy * MAP_TILES_PX + ix];
				r = (u8)(v >> 8 & 0xF8);
				g = (u8)(v >> 3 & 0xFC);
				b = (u8)(v << 3 & 0xF8);
			}
			putPixel(fb, width, MAP_CENTRE_X + px, MAP_CENTRE_Y + py, r, g, b);
		}
	}
	for (int i = 0; i < net.groundCount; i++) {
		int px, py;
		if (net.ground[i].plane != net.plane) continue;
		tilesToMapPoint(net.ground[i].x + 0.5f - tileX, net.ground[i].y + 0.5f - tileY, &px, &py);
		if (px * px + py * py < MAP_RADIUS * MAP_RADIUS) fillRect(fb, width, MAP_CENTRE_X + px - 1, MAP_CENTRE_Y + py - 1, 3, 3, 230, 30, 30);
	}
	for (int i = 0; i < net.npcCount; i++) {
		int px, py;
		tilesToMapPoint(net.npcs[i].viewX - tileX, net.npcs[i].viewY - tileY, &px, &py);
		if (px * px + py * py < MAP_RADIUS * MAP_RADIUS) fillRect(fb, width, MAP_CENTRE_X + px - 1, MAP_CENTRE_Y + py - 1, 3, 3, 240, 230, 40);
	}
	for (int i = 0; i < net.playerCount; i++) {
		int px, py;
		tilesToMapPoint(net.players[i].viewX - tileX, net.players[i].viewY - tileY, &px, &py);
		if (px * px + py * py < MAP_RADIUS * MAP_RADIUS) fillRect(fb, width, MAP_CENTRE_X + px - 1, MAP_CENTRE_Y + py - 1, 3, 3, 255, 255, 255);
	}
	if (walkFlagX >= 0) {
		int px, py;
		tilesToMapPoint(walkFlagX + 0.5f - tileX, walkFlagY + 0.5f - tileY, &px, &py);
		if (px * px + py * py < MAP_RADIUS * MAP_RADIUS) {
			fillRect(fb, width, MAP_CENTRE_X + px - 1, MAP_CENTRE_Y + py - 7, 2, 8, 90, 50, 20);
			fillRect(fb, width, MAP_CENTRE_X + px + 1, MAP_CENTRE_Y + py - 7, 5, 4, 230, 30, 30);
		}
	}
	fillRect(fb, width, MAP_CENTRE_X - 1, MAP_CENTRE_Y - 1, 3, 3, 255, 255, 255);
}

// Blends over what is already on the panel, since the tab stones have soft edges.
static void drawSprite(u8* fb, int width, const Sprite* s, int x, int y) {
	if (!s->pixels) return;
	x += s->xOffset;
	y += s->yOffset;
	for (int py = 0; py < s->innerHeight; py++) {
		for (int px = 0; px < s->innerWidth; px++) {
			u32 c = s->pixels[py * s->innerWidth + px];
			int alpha = (int)(c >> 24), dx = x + px, dy = y + py;
			if (alpha == 0 || dx < 0 || dy < 0 || dx >= width || dy >= SCREEN_H) continue;
			u8* p = fb + (dx * SCREEN_H + (SCREEN_H - 1 - dy)) * 3;
			p[2] = (u8)((((c >> 16) & 255) * (u32)alpha + p[2] * (u32)(255 - alpha)) / 255);
			p[1] = (u8)((((c >> 8) & 255) * (u32)alpha + p[1] * (u32)(255 - alpha)) / 255);
			p[0] = (u8)(((c & 255) * (u32)alpha + p[0] * (u32)(255 - alpha)) / 255);
		}
	}
}

static Sprite tabBarTop, tabBarBottom, tabStones[TAB_COUNT], tabIcons[TAB_COUNT];

// Read once when going online. Stones repeat, but they are tiny.
static void loadTabSprites(void) {
	sprite_load(&world.cache, 1036, &tabBarTop);
	sprite_load(&world.cache, 1032, &tabBarBottom);
	for (int i = 0; i < TAB_COUNT; i++) {
		sprite_load(&world.cache, TAB_PLACES[i].stone, &tabStones[i]);
		sprite_load(&world.cache, TAB_PLACES[i].icon, &tabIcons[i]);
	}
}

static void drawTabBars(u8* fb, int width) {
	int side = (width - TAB_BAR_W) / 2;
	drawSprite(fb, width, &tabBarTop, side, 0);
	drawSprite(fb, width, &tabBarBottom, side + 3, CONTENT_BOTTOM);
	for (int i = 0; i < TAB_COUNT; i++) {
		const TabPlace* t = &TAB_PLACES[i];
		int base = i < 7 ? side : side + 3, y = i < 7 ? 0 : CONTENT_BOTTOM;
		if (panelTab == i) drawSprite(fb, width, &tabStones[i], base + t->x, y);
		drawSprite(fb, width, &tabIcons[i], base + t->iconX, y + t->iconY);
	}
}

static void drawPanel(u8* fb, int width) {
	fillRect(fb, width, 0, 0, width, SCREEN_H, 62, 53, 41);
	char line[96];
	if (mapVisible() && dialogActive()) {
		drawDialog(fb, width);
	} else if (mapVisible() && pageActive()) {
		drawPage(fb, width);
	} else if (mapVisible() && guideActive()) {
		drawGuide(fb, width);
	} else if (mapVisible() && journalActive()) {
		drawJournal(fb, width);
	} else if (mapVisible() && panelTab == TAB_QUESTS) {
		drawQuests(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && (panelTab == TAB_FRIENDS || panelTab == TAB_IGNORE || panelTab == TAB_CLAN)) {
		drawSocial(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && panelTab == TAB_CHAT) {
		drawChat(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && panelTab == TAB_COMBAT) {
		drawCombat(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && (panelTab == TAB_PRAYER || panelTab == TAB_EMOTES || panelTab == TAB_MAGIC)) {
		drawGrid(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && panelTab == TAB_SUMMONING) {
		drawSummon(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && panelTab == TAB_MUSIC) {
		drawMusic(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && panelTab == TAB_BAG) {
		drawBag(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && panelTab == TAB_WORN) {
		drawWorn(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && panelTab == TAB_STATS) {
		drawStats(fb, width);
		drawTabBars(fb, width);
	} else if (mapVisible() && panelTab < TAB_COUNT) {
		drawText(fb, width, (width - (int)strlen(TAB_PLACES[panelTab].name) * 8) / 2, CONTENT_TOP + 10, TAB_PLACES[panelTab].name, 255, 200, 60);
		if (panelTab != TAB_OPTIONS) drawText(fb, width, width / 2 - 44, 116, "Not built yet", 200, 190, 170);
		drawTabBars(fb, width);
	} else if (mapVisible()) {
		drawTabBars(fb, width);
		drawMap(fb, width);
		int x = 176, y = CONTENT_TOP + 4;
		drawText(fb, width, x, y, "2009scape 3DS", 255, 200, 60);
		y += 16;
		snprintf(line, sizeof(line), "Tile %d,%d", net_world_x(&net), net_world_y(&net));
		drawLine(fb, width, x, &y, line);
		snprintf(line, sizeof(line), "Plane %d", net.plane);
		drawLine(fb, width, x, &y, line);
		y += 4;
		snprintf(line, sizeof(line), "Frame %.1f ms", hudFrameMs);
		drawLine(fb, width, x, &y, line);
		snprintf(line, sizeof(line), "GPU %.1f ms", C3D_GetDrawingTime());
		drawLine(fb, width, x, &y, line);
		y += 4;
		drawLine(fb, width, x, &y, "Tap: walk");
		drawLine(fb, width, x, &y, "Select: swap");
		drawLine(fb, width, x, &y, "L/R: rotate");
		drawLine(fb, width, x, &y, "Up/Down: zoom");
		if (net.lastMessage[0]) {
			// Game messages carry colour tags that are not drawn yet.
			const char* text = net.lastMessage;
			while (*text == '<') {
				const char* end = strchr(text, '>');
				if (!end) break;
				text = end + 1;
			}
			snprintf(line, width / 8, "%s", text);
			drawText(fb, width, 4, 192, line, 255, 255, 120);
		}
	} else {
		int y = 8;
		drawText(fb, width, 8, y, "2009scape 3DS", 255, 200, 60);
		y += 16;
		if (online) {
			drawLine(fb, width, 8, &y, "Welcome. Press Play to enter the game.");
		} else {
			snprintf(line, sizeof(line), "Offline: %s", SCENE_NAMES[sceneIndex]);
			drawLine(fb, width, 8, &y, line);
			snprintf(line, sizeof(line), "Frame %.1f ms   GPU %.1f ms", hudFrameMs, C3D_GetDrawingTime());
			drawLine(fb, width, 8, &y, line);
			snprintf(line, sizeof(line), "Triangles %lu", (unsigned long)trisSubmitted);
			drawLine(fb, width, 8, &y, line);
			if (bench.running) {
				snprintf(line, sizeof(line), "Benchmark %d of %d", (bench.scene * RADIUS_COUNT + bench.radius) * MODE_COUNT + bench.mode + 1, SCENE_COUNT * RADIUS_COUNT * MODE_COUNT);
				drawLine(fb, width, 8, &y, line);
			}
			y += 4;
			drawLine(fb, width, 8, &y, "Select: swap  A: next  Y: benchmark");
			drawLine(fb, width, 8, &y, "Pad: pan  L/R: rotate  X/B: zoom");
			drawLine(fb, width, 8, &y, "START: exit");
		}
		if (statusLine[0]) drawText(fb, width, 8, 186, statusLine, 255, 170, 120);
	}

	Widget widgets[MAX_WIDGETS];
	int count = panelLayout(width, widgets);
	for (int i = 0; i < count; i++) {
		const Widget* w = &widgets[i];
		if (w->id >= WIDGET_TAB0) continue; // drawn with the tab bars
		bool lit = (w->id == WIDGET_RUN && runMode) || (w->id == WIDGET_TAB_MAP && panelTab == TAB_MAP) || (w->id == WIDGET_TAB_CHAT && panelTab == TAB_CHAT) || (w->id >= WIDGET_OPTION0 && w->id < WIDGET_OPTION_END && optionOn(w->id - WIDGET_OPTION0));
		fillRect(fb, width, w->x, w->y, w->w, w->h, 110, 90, 60);
		fillRect(fb, width, w->x + 2, w->y + 2, w->w - 4, w->h - 4, lit ? 120 : 84, lit ? 60 : 68, lit ? 40 : 46);
		drawText(fb, width, w->x + (w->w - (int)strlen(w->label) * 8) / 2, w->y + w->h / 2 - 4, w->label, 255, 255, 255);
	}
	if (gameOnBottom) {
		// The cursor on top: a small arrow.
		for (int i = 0; i < 11; i++) {
			fillRect(fb, width, (int)cursorX, (int)cursorY + i, i < 8 ? i + 1 : 3, 1, 255, 255, 255);
		}
	}
}

static void panelPresent(void) {
	gfxScreen_t screen = panelScreen();
	int width = screenWidth(screen);
	u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
	drawPanel(fb, width);
	GSPGPU_FlushDataCache(fb, (u32)(width * SCREEN_H * 3));
	gfxScreenSwapBuffers(screen, false);
}
