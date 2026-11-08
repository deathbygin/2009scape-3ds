// Text on the game screen: recent messages in the corner and chat above heads. The game shader has no textures,
// so each letter goes out as small rectangles, one per run of pixels. Nothing is built when there is no text.

#define OVERLAY_VERTS 12000
#define OVERLAY_ROWS 4        // the most message rows shown at once
#define OVERLAY_SHOW_MS 5000
#define OVERHEAD_SHOW_MS 3000 // 150 of the client's 20 ms ticks
#define OVERHEAD_LIMIT 12
#define OVERHEAD_LINES 3
#define OVERLAY_MAX_WIDTH 300
#define HIT_SHOW_MS 1400  // 70 ticks of the client
#define BAR_SHOW_MS 6000  // 300 ticks
#define HIT_KINDS 8
#define SPRITE_RUN_MAX 1024

typedef struct {
	char text[NET_CHAT_TEXT + 32];
	u8 r, g, b;
	u32 until;
} OverlayRow;

static GameFont smallFont, overheadFont;
static bool fontsLoaded;
static u8* overlayBuffer = NULL;
static int overlayFrame, overlayVerts;
static u8* overlayOut;
static OverlayRow overlayRows[OVERLAY_ROWS]; // oldest first
static int overlayRowCount;
static long overlaySeen;

// A sprite as runs of one colour, so it can be drawn without textures.
typedef struct {
	int8_t x, y;
	uint8_t length, r, g, b;
} SpriteRun;

typedef struct {
	SpriteRun* runs;
	int count, width, height;
} RunSprite;

static RunSprite hitMarks[HIT_KINDS], hitBars[2];

static void makeRuns(int group, int frame, RunSprite* out) {
	Sprite s;
	out->count = 0;
	if (group < 0 || sprite_load_frame(&world.cache, group, frame, &s) != 0) return;
	out->runs = malloc(sizeof(SpriteRun) * SPRITE_RUN_MAX);
	out->width = s.width;
	out->height = s.height;
	for (int y = 0; out->runs && y < s.innerHeight; y++) {
		int x = 0;
		while (x < s.innerWidth && out->count < SPRITE_RUN_MAX) {
			u32 p = s.pixels[y * s.innerWidth + x];
			if ((p >> 24) < 128) {
				x++;
				continue;
			}
			int start = x;
			// Close shades are merged, which looks the same at this size and halves the work.
			while (x < s.innerWidth && (s.pixels[y * s.innerWidth + x] >> 24) >= 128 && ((s.pixels[y * s.innerWidth + x] ^ p) & 0xF0F0F0) == 0) x++;
			out->runs[out->count++] = (SpriteRun){(int8_t)(s.xOffset + start), (int8_t)(s.yOffset + y), (uint8_t)(x - start), (u8)(p >> 16), (u8)(p >> 8), (u8)p};
		}
	}
	sprite_free(&s);
}

static void loadHitSprites(void) {
	if (js5_load_index(&world.cache, 8) != 0) return;
	int marks = js5_find_group(&world.cache.index[8], "hitmarks"), bars = js5_find_group(&world.cache.index[8], "hitbar_default");
	for (int i = 0; i < HIT_KINDS; i++) makeRuns(marks, i, &hitMarks[i]);
	for (int i = 0; i < 2; i++) makeRuns(bars, i, &hitBars[i]);
}

static void loadFonts(void) {
	overlayRowCount = 0;
	overlaySeen = 0;
	if (fontsLoaded) return;
	loadHitSprites();
	fontsLoaded = font_load(&world.cache, "p11_full", &smallFont) == 0 && font_load(&world.cache, "b12_full", &overheadFont) == 0;
	char line[64];
	logLine(CPU_MODE_NOTE);
	logLine(DIAG_NOTE);
	snprintf(line, sizeof(line), "fonts loaded=%d small=%d overhead=%d", fontsLoaded, smallFont.lineHeight, overheadFont.lineHeight);
	logLine(line);
}

static void addOverlayRow(const char* text, u8 r, u8 g, u8 b, u32 until) {
	if (overlayRowCount == OVERLAY_ROWS) {
		memmove(&overlayRows[0], &overlayRows[1], sizeof(OverlayRow) * (OVERLAY_ROWS - 1));
		overlayRowCount--;
	}
	OverlayRow* row = &overlayRows[overlayRowCount++];
	snprintf(row->text, sizeof(row->text), "%s", text);
	row->r = r;
	row->g = g;
	row->b = b;
	row->until = until;
}

// Long messages take more than one row, breaking at a space.
static void addOverlayText(const char* text, u8 r, u8 g, u8 b, u32 until) {
	char row[NET_CHAT_TEXT + 32];
	int length = 0, lastSpace = -1, width = 0;
	const char* start = text;
	if (!fontsLoaded) return;
	for (const char* at = text; *at; at++) {
		if (*at == ' ') lastSpace = length;
		width += smallFont.advance[(u8)*at];
		length++;
		if (width <= OVERLAY_MAX_WIDTH) continue;
		int take = lastSpace > 0 ? lastSpace : length - 1;
		snprintf(row, sizeof(row), "%.*s", take, start);
		addOverlayRow(row, r, g, b, until);
		start += take;
		while (*start == ' ') start++;
		at = start - 1;
		length = 0;
		lastSpace = -1;
		width = 0;
	}
	if (*start) addOverlayRow(start, r, g, b, until);
}

// Takes in whatever arrived since the last frame. Rows past their time go, oldest first.
static void updateOverlay(void) {
	u32 now = (u32)nowMs();
	long first = overlaySeen;
	if (net.chatVersion - first > OVERLAY_ROWS) first = net.chatVersion - OVERLAY_ROWS;
	for (long i = first; i < net.chatVersion; i++) {
		const NetChat* line = &net.chat[i % NET_CHAT_LINES];
		char plain[NET_CHAT_TEXT], text[NET_CHAT_TEXT + 32];
		u8 r, g, b;
		chatLineText(line, plain, sizeof(plain), text, sizeof(text), &r, &g, &b);
		addOverlayText(text, r, g, b, now + OVERLAY_SHOW_MS);
	}
	overlaySeen = net.chatVersion;
	while (overlayRowCount > 0 && (s32)(overlayRows[0].until - now) <= 0) {
		memmove(&overlayRows[0], &overlayRows[1], sizeof(OverlayRow) * (OVERLAY_ROWS - 1));
		overlayRowCount--;
	}
}

static void putRun(int x, int y, int length, u8 r, u8 g, u8 b, int width) {
	if (overlayVerts + 6 > OVERLAY_VERTS || y < 0 || y >= SCREEN_H) return;
	int right = x + length;
	if (x < 0) x = 0;
	if (right > width) right = width;
	if (right <= x) return;
	const s16 corners[6][2] = {{(s16)x, (s16)y}, {(s16)right, (s16)y}, {(s16)x, (s16)(y + 1)}, {(s16)right, (s16)y}, {(s16)right, (s16)(y + 1)}, {(s16)x, (s16)(y + 1)}};
	for (int i = 0; i < 6; i++) {
		s16 p[4] = {corners[i][0], corners[i][1], 0, 0};
		u8* out = overlayOut + (overlayVerts + i) * VERT_SIZE;
		memcpy(out, p, 8);
		out[8] = r;
		out[9] = g;
		out[10] = b;
		out[11] = 255;
	}
	overlayVerts += 6;
}

static void putGlyphs(const GameFont* f, const char* text, int x, int top, u8 r, u8 g, u8 b, int width) {
	for (; *text; text++) {
		u8 c = (u8)*text;
		for (u32 i = f->first[c]; i < f->first[c + 1]; i++) putRun(x + f->runs[i].x, top + f->runs[i].y, f->runs[i].length, r, g, b, width);
		x += f->advance[c];
	}
}

// The black copy behind keeps text readable on any scenery, as in the original.
static void putText(const GameFont* f, const char* text, int x, int top, u8 r, u8 g, u8 b, int width) {
	putGlyphs(f, text, x + 1, top + 1, 0, 0, 0, width);
	putGlyphs(f, text, x, top, r, g, b, width);
}

// Where a point above an entity's feet lands on the screen.
static bool projectAbove(const NetNpc* n, float top, int width, int* sx, int* sy) {
	CameraBasis c = cameraBasis(width);
	float dx = n->viewX - c.eyeX, dz = -n->viewY - c.eyeZ;
	float dy = groundHeight(net.plane, n->viewX, n->viewY) + top - c.eyeY;
	float depth = dx * c.fX + dy * c.fY + dz * c.fZ;
	if (depth <= 0.5f) return false;
	float across = dx * c.rX + dz * c.rZ, upward = dx * c.uX + dy * c.uY + dz * c.uZ;
	*sx = (int)((across / (depth * c.tanH) + 1.0f) * 0.5f * width);
	*sy = (int)((1.0f - upward / (depth * c.tanV)) * 0.5f * SCREEN_H);
	return *sx > -60 && *sx < width + 60 && *sy > -20 && *sy < SCREEN_H;
}

static bool projectHead(const NetNpc* n, int width, int* sx, int* sy) {
	return projectAbove(n, (n->hasModel ? npcModelTop(n->modelKey) : 200) / 128.0f + 0.1f, width, sx, sy);
}

static void putSpriteRuns(const RunSprite* s, int x, int y, int clip, int width) {
	for (int i = 0; i < s->count; i++) {
		const SpriteRun* r = &s->runs[i];
		int length = r->length;
		if (r->x >= clip) continue;
		if (r->x + length > clip) length = clip - r->x;
		putRun(x + r->x, y + r->y, length, r->r, r->g, r->b, width);
	}
}

// Takes in new hits, as PathingEntity.addHit does: each needs a free slot of the four, or it goes unseen.
static void acceptHits(NetNpc* n, u32 now) {
	for (int i = 0; i < n->newHitCount; i++) {
		for (int slot = 0; slot < 4; slot++) {
			if ((s32)(n->hitUntil[slot] - now) > 0) continue;
			n->hitDamage[slot] = n->newHitDamage[i];
			n->hitType[slot] = n->newHitType[i];
			n->hitUntil[slot] = now + HIT_SHOW_MS;
			break;
		}
	}
	n->newHitCount = 0;
	if (n->barNew) {
		n->barNew = 0;
		n->barUntil = now + BAR_SHOW_MS;
	}
}

// The health bar over the head, and the splats on the body in the original's four places.
static void putHits(NetNpc* n, u32 now, int width) {
	acceptHits(n, now);
	if (!n->placed) return;
	int height = n->hasModel ? npcModelTop(n->modelKey) : 200, x, y;
	if ((s32)(n->barUntil - now) > 0 && hitBars[0].count && projectAbove(n, (height + 10 + hitBars[0].height) / 128.0f, width, &x, &y)) {
		int left = x - hitBars[0].width / 2, top = y - 3;
		putSpriteRuns(&hitBars[0], left, top, hitBars[0].width, width);
		putSpriteRuns(&hitBars[1], left, top, hitBars[0].width * n->bar / 255, width);
	}
	if (!projectAbove(n, height / 2 / 128.0f, width, &x, &y)) return;
	for (int slot = 0; slot < 4; slot++) {
		if ((s32)(n->hitUntil[slot] - now) <= 0) continue;
		int hx = x + (slot == 2 ? -15 : slot == 3 ? 15 : 0), hy = y - (slot == 1 ? 20 : slot >= 2 ? 10 : 0);
		int kind = n->hitType[slot] < HIT_KINDS ? n->hitType[slot] : 0;
		putSpriteRuns(&hitMarks[kind], hx - 12, hy - 12, 255, width);
		char text[8];
		snprintf(text, sizeof(text), "%d", n->hitDamage[slot]);
		putText(&smallFont, text, hx - 1 - font_width(&smallFont, text) / 2, hy + 3 - smallFont.lineHeight, 255, 255, 255, width);
	}
}

static int putOverhead(NetNpc* n, u32 now, int width, int shown) {
	if (n->overheadNew) {
		n->overheadNew = 0;
		n->overheadUntil = now + OVERHEAD_SHOW_MS;
	}
	if (shown >= OVERHEAD_LIMIT || !n->overhead[0] || !n->placed || (s32)(n->overheadUntil - now) <= 0) return shown;
	int x, y;
	if (!projectHead(n, width, &x, &y)) return shown;
	// The original's window is wider than these screens, so long lines stack upwards from the head.
	char lines[OVERHEAD_LINES][sizeof(n->overhead)];
	int count = 0, length = 0, lastSpace = -1, lineWidth = 0;
	const char* start = n->overhead;
	for (const char* at = start; *at && count < OVERHEAD_LINES - 1; at++) {
		if (*at == ' ') lastSpace = length;
		lineWidth += overheadFont.advance[(u8)*at];
		length++;
		if (lineWidth <= width - 8) continue;
		int take = lastSpace > 0 ? lastSpace : length - 1;
		snprintf(lines[count++], sizeof(lines[0]), "%.*s", take, start);
		start += take;
		while (*start == ' ') start++;
		at = start - 1;
		length = 0;
		lastSpace = -1;
		lineWidth = 0;
	}
	snprintf(lines[count++], sizeof(lines[0]), "%s", start);
	for (int i = 0; i < count; i++) {
		int textWidth = font_width(&overheadFont, lines[i]), left = x - textWidth / 2;
		if (left + textWidth > width - 2) left = width - 2 - textWidth;
		if (left < 2) left = 2;
		putText(&overheadFont, lines[i], left, y - (count - i) * (overheadFont.lineHeight + 1), 255, 255, 0, width);
	}
	return shown + 1;
}

static void drawTextOverlay(void) {
	if (!online || !fontsLoaded) return;
	if (!overlayBuffer) overlayBuffer = linearAlloc(OVERLAY_VERTS * VERT_SIZE * 2);
	if (!overlayBuffer) return;
	int width = gameOnBottom ? 320 : 400, shown = 0;
	u32 now = (u32)nowMs();
	overlayOut = overlayBuffer + overlayFrame * OVERLAY_VERTS * VERT_SIZE;
	overlayVerts = 0;
	for (int i = 0; i < net.npcCount; i++) putHits(&net.npcs[i], now, width);
	for (int i = 0; i < net.playerCount; i++) putHits(&net.players[i], now, width);
	putHits(&net.self, now, width);
	for (int i = 0; i < net.npcCount; i++) shown = putOverhead(&net.npcs[i], now, width, shown);
	for (int i = 0; i < net.playerCount; i++) shown = putOverhead(&net.players[i], now, width, shown);
	shown = putOverhead(&net.self, now, width, shown);
	int rowHeight = smallFont.lineHeight + 1;
	for (int i = 0; i < overlayRowCount; i++) {
		const OverlayRow* row = &overlayRows[i];
		putText(&smallFont, row->text, 4, SCREEN_H - 4 - (overlayRowCount - i) * rowHeight, row->r, row->g, row->b, width);
	}
	if (overlayVerts == 0) return;
	overlayFrame ^= 1;
	GSPGPU_FlushDataCache(overlayOut, (u32)overlayVerts * VERT_SIZE);
	C3D_Mtx ortho;
	Mtx_OrthoTilt(&ortho, 0.0f, (float)width, 240.0f, 0.0f, -1.0f, 1.0f, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_mvp, &ortho);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, overlayOut, VERT_SIZE, 2, 0x10);
	C3D_DrawArrays(GPU_TRIANGLES, 0, overlayVerts);
	C3D_CullFace(GPU_CULL_BACK_CCW);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}
