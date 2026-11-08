// Full-screen pages built by scripts: the Skill Guide and the page a level-up opens. The scripts lay the page out
// for a width that happens to fit this screen, so their positions are used as they are, on the journal's parchment.
// Moving between sections asks the server, one press at a time, and scrolling sends nothing.

#define GUIDE_PIECES 640
#define GUIDE_TOP 40
#define GUIDE_BOTTOM (SCREEN_H - 4)
#define GUIDE_LEFT 2
#define GUIDE_ARROW_W 40
#define GUIDE_MADE_LIMIT 1024
#define GUIDE_SECTION_VARBIT 3289
#define FONT_SLOTS 8
#define GUIDE_RIGHT (320 - SCROLL_W - 2) // where a page must stop on the narrower screen

enum { PIECE_TEXT, PIECE_ITEM, PIECE_SPRITE, PIECE_RECT };

typedef struct {
	int iface, title, subtitle, list, firstSection, lastSection;
} GuideKind;

static const GuideKind GUIDE_KINDS[2] = {{499, 6, 9, 7, 10, 25}, {741, 5, 4, 2, -1, -1}};

typedef struct {
	uint8_t kind;
	int16_t x, y, w, h;
	u8 r, g, b;
	int id, amount;
	const GameFont* font;
	char text[72];
} GuidePiece;

static TabScript guideScripts[2] = {{499}, {741}};
static const GuideKind* guideKind = NULL; // the page showing, if any
static GuidePiece guidePieces[GUIDE_PIECES];
static int guidePieceCount, guideScroll, guideTotal;
static int guideShift; // how far rows have moved down because text took more lines than the script planned
static char guideTitle[48], guideSubtitle[64], guideSection[32], guideXp[48];
static long guideVersion = -1;
static struct {
	int id;
	GameFont font;
} fontSlots[FONT_SLOTS];
static int fontSlotCount;

static void resetGuide(void) {
	guideKind = NULL;
	guideScripts[0].loaded = guideScripts[1].loaded = false;
	guideSkill = -1;
}

static bool guideActive(void) {
	return guideKind != NULL;
}

static const GameFont* fontById(int id) {
	for (int i = 0; i < fontSlotCount; i++) if (fontSlots[i].id == id) return &fontSlots[i].font;
	if (id >= 0 && fontSlotCount < FONT_SLOTS && font_load_id(&world.cache, id, &fontSlots[fontSlotCount].font) == 0) {
		fontSlots[fontSlotCount].id = id;
		return &fontSlots[fontSlotCount++].font;
	}
	return &smallFont;
}

static int measureLinesHook(const char* text, int width, int font) {
	return font_lines(fontById(font), text, width);
}

static GuidePiece* newPiece(int kind, int x, int y, int w, int h, int colour) {
	if (guidePieceCount >= GUIDE_PIECES) return NULL;
	GuidePiece* p = &guidePieces[guidePieceCount++];
	memset(p, 0, sizeof(*p));
	p->kind = (uint8_t)kind;
	y += guideShift;
	p->x = (int16_t)x;
	p->y = (int16_t)y;
	p->w = (int16_t)w;
	p->h = (int16_t)h;
	p->r = (u8)(colour >> 16);
	p->g = (u8)(colour >> 8);
	p->b = (u8)colour;
	if (y + h > guideTotal) guideTotal = y + h;
	return p;
}

// Breaks text the way the font does, with the tags taken out, and adds one piece per line.
static void addTextPieces(const IfaceComponent* c, int planned, int height) {
	const GameFont* f = fontById(c->font);
	int width = planned;
	if (c->x + width > GUIDE_RIGHT - GUIDE_LEFT) width = GUIDE_RIGHT - GUIDE_LEFT - c->x;
	char lines[12][72];
	int count = 0, length = 0, lineWidth = 0, breakAt = -1, breakWidth = 0;
	const char* at = c->text;
	char line[256];
	while (count < 12) {
		bool newline = *at == 0;
		if (*at == '<') {
			const char* end = strchr(at, '>');
			if (end) {
				newline = strncmp(at, "<br>", 4) == 0;
				at = end + 1;
				if (!newline) continue;
			}
		} else if (*at) {
			if (length < (int)sizeof(line) - 1) line[length++] = *at;
			lineWidth += f->advance[(u8)*at];
			if (*at == ' ' || *at == '-') {
				breakAt = length;
				breakWidth = lineWidth;
			}
			at++;
			if (lineWidth > width && breakAt > 0) {
				snprintf(lines[count++], sizeof(lines[0]), "%.*s", breakAt, line);
				memmove(line, line + breakAt, (size_t)(length - breakAt));
				length -= breakAt;
				lineWidth -= breakWidth;
				breakAt = -1;
			}
			continue;
		}
		if (length > 0 || *at) {
			snprintf(lines[count++], sizeof(lines[0]), "%.*s", length, line);
			length = lineWidth = 0;
			breakAt = -1;
		}
		if (*at == 0) break;
	}
	int step = c->vpadding ? c->vpadding : f->lineHeight + 2;
	int top = c->y + (c->valign == 1 ? (height - count * step) / 2 : c->valign == 2 ? height - count * step : 0);
	for (int i = 0; i < count; i++) {
		int textWidth = font_width(f, lines[i]);
		int x = c->x + (c->halign == 1 ? (width - textWidth) / 2 : c->halign == 2 ? width - textWidth : 0);
		GuidePiece* p = newPiece(PIECE_TEXT, x, top + i * step, textWidth, step, c->colour);
		if (!p) return;
		p->font = f;
		snprintf(p->text, sizeof(p->text), "%s", lines[i]);
	}
	int extra = count - font_lines(f, c->text, planned);
	if (extra > 0) guideShift += extra * step;
}

static void addPiece(const IfaceComponent* c) {
	if (c->hidden) return;
	int width = cs2_width(c), height = cs2_height(c);
	if (c->type == COMP_TEXT && c->text && c->text[0]) {
		addTextPieces(c, width, height);
	} else if (c->itemPlusOne > 0) {
		GuidePiece* p = newPiece(PIECE_ITEM, c->x, c->y, ICON_W, ICON_H, 0);
		if (p) {
			p->id = c->itemPlusOne - 1;
			p->amount = c->itemAmount;
		}
	} else if (c->type == COMP_SPRITE && c->spriteId >= 0) {
		GuidePiece* p = newPiece(PIECE_SPRITE, c->x, c->y, width, height, 0);
		if (p) p->id = c->spriteId;
		cachedSprite(c->spriteId);
	} else if (c->type == COMP_RECT && height <= 3) {
		newPiece(PIECE_RECT, c->x, c->y, width, height, c->colour);
	}
}

static void guideText(int component, char* out, size_t size) {
	const IfaceComponent* c = cs2_component(guideKind->iface << 16 | component);
	const char* server = net_component_text(&net, guideKind->iface << 16 | component);
	out[0] = 0;
	if (server) plainText(server, out, size);
	else if (c && c->text) plainText(c->text, out, size);
}

static int guideSectionNow(void) {
	return guideKind->firstSection + cs2_get_varbit(GUIDE_SECTION_VARBIT);
}

static void rebuildGuide(void) {
	static IfaceComponent* made[GUIDE_MADE_LIMIT];
	static int children[GUIDE_MADE_LIMIT];
	guidePieceCount = guideTotal = guideShift = 0;
	int parent = guideKind->iface << 16 | guideKind->list;
	Iface* f = cs2_interface(guideKind->iface);
	if (!f) return;
	for (int i = 0; i < f->count; i++) if (f->components[i].type != 255 && f->components[i].parent == parent) addPiece(&f->components[i]);
	int count = cs2_created_list(parent, made, children, GUIDE_MADE_LIMIT);
	for (int i = 0; i < count; i++) addPiece(made[i]);
	guideText(guideKind->title, guideTitle, sizeof(guideTitle));
	guideText(guideKind->subtitle, guideSubtitle, sizeof(guideSubtitle));
	guideSection[0] = 0;
	if (guideKind->firstSection >= 0) guideText(guideSectionNow(), guideSection, sizeof(guideSection));
	guideXp[0] = 0;
	if (guideSkill >= 0) {
		int level = baseLevel(guideSkill), xp = net.skillXp[guideSkill];
		if (level >= 99) snprintf(guideXp, sizeof(guideXp), "Level %d, %d XP", level, xp);
		else snprintf(guideXp, sizeof(guideXp), "Level %d, %d XP, %d to go", level, xp, skillXpTable[level - 1] - xp);
	}
	scrollList(&guideScroll, guideScroll, GUIDE_BOTTOM - GUIDE_TOP, guideTotal);
}

// Runs once per frame. Icons load models from the SD card, so only those in view are made, one a frame.
static void updateGuide(void) {
	const GuideKind* open = NULL;
	int index = 0;
	for (int i = 0; i < 2; i++) {
		if (!net_interface_is_open(&net, GUIDE_KINDS[i].iface)) continue;
		open = &GUIDE_KINDS[i];
		index = i;
	}
	if (open != guideKind) {
		// The client loads a page afresh each time it opens.
		for (int i = 0; i < 2; i++) if (!open || i != index) guideScripts[i].loaded = false;
		guideKind = open;
		guideScroll = 0;
		guidePieceCount = 0;
		guideVersion = -1;
		listTouchCancel();
		panelDirty = true;
	}
	if (!guideKind) return;
	if (refreshTabScript(&guideScripts[index]) || guideVersion != net.ifaceVersion) {
		guideVersion = net.ifaceVersion;
		rebuildGuide();
		panelDirty = true;
	}
	for (int i = 0; i < guidePieceCount; i++) {
		const GuidePiece* p = &guidePieces[i];
		if (p->kind != PIECE_ITEM || p->y + p->h < guideScroll || p->y > guideScroll + GUIDE_BOTTOM - GUIDE_TOP) continue;
		if (ensureIcon(p->id, p->amount)) break;
	}
}

static void drawGuide(u8* fb, int width) {
	fillRect(fb, width, 0, 0, width, SCREEN_H, 214, 190, 140);
	for (int i = 0; i < guidePieceCount; i++) {
		const GuidePiece* p = &guidePieces[i];
		int x = GUIDE_LEFT + p->x, y = GUIDE_TOP + p->y - guideScroll;
		if (y + p->h <= GUIDE_TOP || y >= GUIDE_BOTTOM) continue;
		if (p->kind == PIECE_TEXT) {
			if (y >= GUIDE_TOP - 2 && y + p->font->lineHeight <= GUIDE_BOTTOM) drawPlainFont(fb, width, p->font, p->text, x, y, p->r, p->g, p->b);
		} else if (p->kind == PIECE_ITEM) {
			if (y >= GUIDE_TOP && y + ICON_H <= GUIDE_BOTTOM) drawItemIcon(fb, width, p->id, p->amount, x, y);
		} else if (p->kind == PIECE_SPRITE) {
			const Sprite* s = cachedSprite(p->id);
			if (s && y >= GUIDE_TOP && y + p->h <= GUIDE_BOTTOM) drawSprite(fb, width, s, x, y);
		} else if (y >= GUIDE_TOP && y < GUIDE_BOTTOM) {
			fillRect(fb, width, x, y, p->w, p->h, p->r, p->g, p->b);
		}
	}
	fillRect(fb, width, 0, 0, width, GUIDE_TOP - 1, 200, 174, 120);
	drawPlainFont(fb, width, &overheadFont, guideTitle, 6, 3, 70, 50, 10);
	drawPlainFont(fb, width, &smallFont, guideXp[0] ? guideXp : guideSubtitle, 8 + font_width(&overheadFont, guideTitle), 5, 70, 50, 10);
	if (guideKind->firstSection >= 0) {
		fillRect(fb, width, 2, 20, GUIDE_ARROW_W, 17, 110, 90, 60);
		fillRect(fb, width, width - CLOSE_W - 6 - GUIDE_ARROW_W, 20, GUIDE_ARROW_W, 17, 110, 90, 60);
		drawText(fb, width, 2 + GUIDE_ARROW_W / 2 - 4, 25, "<", 255, 255, 255);
		drawText(fb, width, width - CLOSE_W - 6 - GUIDE_ARROW_W / 2 - 4, 25, ">", 255, 255, 255);
		int room = width - CLOSE_W - 6 - GUIDE_ARROW_W, textWidth = font_width(&smallFont, guideSection);
		drawPlainFont(fb, width, &smallFont, guideSection, (GUIDE_ARROW_W + room - textWidth) / 2, 23, 70, 50, 10);
	}
	int closeX = width - CLOSE_W - 2;
	fillRect(fb, width, closeX, 2, CLOSE_W, CLOSE_H + 14, 110, 90, 60);
	fillRect(fb, width, closeX + 2, 4, CLOSE_W - 4, CLOSE_H + 10, 84, 68, 46);
	drawText(fb, width, closeX + 6, 15, "Close", 255, 255, 255);
	drawScrollBar(fb, width, GUIDE_TOP, GUIDE_BOTTOM, guideScroll, guideTotal);
}

// The section beside the one showing, skipping those a skill does not use.
static int guideNextSection(int step) {
	int span = guideKind->lastSection - guideKind->firstSection + 1, now = guideSectionNow();
	for (int i = 1; i < span; i++) {
		int at = guideKind->firstSection + (now - guideKind->firstSection + step * i + span * 2) % span;
		const IfaceComponent* c = cs2_component(guideKind->iface << 16 | at);
		if (c && !c->hidden && c->text && c->text[0]) return at;
	}
	return -1;
}

static int* guideList(int* top, int* bottom, int* total) {
	*top = GUIDE_TOP;
	*bottom = GUIDE_BOTTOM;
	*total = guideTotal;
	return &guideScroll;
}

static bool pressGuide(int x, int y, int width) {
	if (!guideKind || !mapVisible() || dialogActive()) return false;
	if (x >= width - CLOSE_W - 2 && y < GUIDE_TOP - 2) {
		net_close_modal(&net);
		actionsSent++;
		return true;
	}
	if (guideKind->firstSection >= 0 && y >= 20 && y < 37) {
		int step = x < 2 + GUIDE_ARROW_W ? -1 : x >= width - CLOSE_W - 6 - GUIDE_ARROW_W ? 1 : 0;
		int section = step ? guideNextSection(step) : -1;
		if (section >= 0) {
			net_button(&net, guideKind->iface, section);
			actionsSent++;
			guideScroll = 0;
		}
		return true;
	}
	if (y >= GUIDE_TOP && x >= width - SCROLL_W) {
		if (y < GUIDE_TOP + 18) scrollList(&guideScroll, guideScroll - SCROLL_STEP, GUIDE_BOTTOM - GUIDE_TOP, guideTotal);
		else if (y >= GUIDE_BOTTOM - 18) scrollList(&guideScroll, guideScroll + SCROLL_STEP, GUIDE_BOTTOM - GUIDE_TOP, guideTotal);
	}
	return true;
}
