// The Combat, Prayer, Emotes and Music tabs. As with quests, the client's scripts decide what each one contains,
// and pressing anything sends one request. Nothing here repeats by itself.

#define TAB_PARENT(tab) (548 << 16 | (83 + (tab)))
#define PRAYER_INTERFACE 271
#define EMOTE_INTERFACE 464
#define MUSIC_INTERFACE 187
#define MUSIC_LIST_COMPONENT 1
#define MUSIC_NOW_COMPONENT 14
#define MUSIC_ROWS 800
#define MUSIC_MADE_LIMIT 1024
#define GRID_CELLS 80
#define MAGIC_CELL 34
#define SPELL_SCRIPT 6
#define GRID_TOP (CONTENT_TOP + 15)
#define PRAYER_COLUMNS 9
#define PRAYER_CELL 34
#define EMOTE_CELL 50
#define SPRITE_CACHE 192
#define COMBAT_BUTTONS 10
#define COMBAT_BUTTON_H 28
#define VARP_STYLE_SPECIAL_ENERGY 300
#define VARP_SPECIAL_ON 301
#define VARP_RETALIATE_OFF 172

typedef struct {
	int component, sprite, mask;
	bool lit, pressable;
	char name[28];
} GridCell;

typedef struct {
	char text[32];
	int child, colour;
} MusicRow;

typedef struct {
	int component;
	bool retaliate, special;
	char label[24];
} CombatButton;

static TabScript prayerScript = {PRAYER_INTERFACE}, emoteScript = {EMOTE_INTERFACE}, musicScript = {MUSIC_INTERFACE}, magicScript = {-1};

static GridCell gridCells[GRID_CELLS];
static int gridCount, gridColumns, gridCellSize, gridScroll, gridTotal;
static int gridOwner = -1; // the tab the grid holds, since Prayer and Emotes share it
static char gridNote[32];

static MusicRow musicRows[MUSIC_ROWS];
static int musicRowCount, musicScroll;
static char musicNow[40];
static long musicVersion = -1;

static Iface combatIface;
static bool combatLoaded;
static CombatButton combatButtons[COMBAT_BUTTONS];
static int combatButtonCount;
static long combatVarps = -1, combatVersion = -1;

static struct {
	int id;
	Sprite sprite;
} spriteCache[SPRITE_CACHE];
static int spriteCacheCount;

static void resetTabs(void) {
	prayerScript.loaded = emoteScript.loaded = musicScript.loaded = magicScript.loaded = false;
	magicScript.iface = -1;
	castComponent = -1;
	gridCount = musicRowCount = 0;
	gridOwner = -1;
	gridScroll = musicScroll = 0;
	gridNote[0] = 0;
	musicVersion = combatVersion = combatVarps = -1;
}

// Sprites change as levels do, so they are read when a tab is rebuilt and never while drawing.
static const Sprite* cachedSprite(int id) {
	if (id < 0) return NULL;
	for (int i = 0; i < spriteCacheCount; i++) if (spriteCache[i].id == id) return &spriteCache[i].sprite;
	if (spriteCacheCount >= SPRITE_CACHE) return NULL;
	if (sprite_load(&world.cache, id, &spriteCache[spriteCacheCount].sprite) != 0) return NULL;
	spriteCache[spriteCacheCount].id = id;
	return &spriteCache[spriteCacheCount++].sprite;
}

static const char* firstOp(const IfaceComponent* c) {
	if (c->opCount < 1 || !c->ops[0]) return NULL;
	const char* op = c->ops[0];
	while (*op == ' ') op++;
	return *op ? c->ops[0] : NULL;
}

static void addGridCell(const IfaceComponent* c, int index, bool lit, const char* name) {
	if (gridCount >= GRID_CELLS) return;
	GridCell cell;
	cell.component = index;
	cell.sprite = c->spriteId;
	cell.lit = lit;
	cell.mask = (int)(c->events >> 11 & 0x7F);
	cell.pressable = (c->events >> 1 & 1) != 0 || cell.mask != 0;
	plainText(name, cell.name, sizeof(cell.name));
	cachedSprite(c->spriteId);
	int at = gridCount++;
	// Reading order, which is not the order they are stored in.
	while (at > 0) {
		const IfaceComponent* before = cs2_component((c->id & (int)0xFFFF0000) | gridCells[at - 1].component);
		if (before->y < c->y || (before->y == c->y && before->x <= c->x)) break;
		gridCells[at] = gridCells[at - 1];
		at--;
	}
	gridCells[at] = cell;
}

static void rebuildPrayer(void) {
	gridCount = 0;
	gridColumns = PRAYER_COLUMNS;
	gridCellSize = PRAYER_CELL;
	Iface* f = cs2_interface(PRAYER_INTERFACE);
	if (!f) return;
	for (int i = 1; i < f->count; i++) {
		const IfaceComponent* c = &f->components[i];
		const char* op = firstOp(c);
		if (c->type != COMP_SPRITE || !op || !strstr(op, "ctivate")) continue; // it reads Deactivate while on
		// The glow behind each prayer comes just before it, and its scripts show it while the prayer is on.
		addGridCell(c, i, !f->components[i - 1].hidden, c->option ? c->option : "");
	}
	gridTotal = (gridCount + gridColumns - 1) / gridColumns * gridCellSize;
	const IfaceComponent* points = cs2_component(PRAYER_INTERFACE << 16 | 1);
	if (points && points->text) snprintf(gridNote, sizeof(gridNote), "Prayer: %s", points->text);
}

static void rebuildEmotes(int width) {
	gridCount = 0;
	gridCellSize = EMOTE_CELL;
	gridColumns = (width - SCROLL_W - 4) / EMOTE_CELL;
	Iface* f = cs2_interface(EMOTE_INTERFACE);
	if (!f) return;
	for (int i = 0; i < f->count; i++) {
		const IfaceComponent* c = &f->components[i];
		const char* op = firstOp(c);
		if (c->type != COMP_SPRITE || c->hidden || !op) continue;
		addGridCell(c, i, false, op);
	}
	gridTotal = (gridCount + gridColumns - 1) / gridColumns * gridCellSize;
}

static void rebuildMagic(int width) {
	gridCount = 0;
	gridCellSize = MAGIC_CELL;
	gridColumns = (width - SCROLL_W - 4) / MAGIC_CELL;
	Iface* f = cs2_interface(magicScript.iface);
	if (!f) return;
	for (int i = 0; i < f->count; i++) {
		const IfaceComponent* c = &f->components[i];
		if (c->type != COMP_SPRITE || c->hidden || !c->onLoad || c->onLoad[0] != SPELL_SCRIPT || !c->option) continue;
		addGridCell(c, i, castComponent == i, c->option);
	}
	gridTotal = (gridCount + gridColumns - 1) / gridColumns * gridCellSize;
}

static void rebuildMusic(void) {
	static IfaceComponent* made[MUSIC_MADE_LIMIT];
	static int children[MUSIC_MADE_LIMIT];
	musicRowCount = 0;
	int count = cs2_created_list(MUSIC_INTERFACE << 16 | MUSIC_LIST_COMPONENT, made, children, MUSIC_MADE_LIMIT);
	static int ys[MUSIC_ROWS];
	for (int i = 0; i < count && musicRowCount < MUSIC_ROWS; i++) {
		const IfaceComponent* c = made[i];
		if (c->hidden || !c->text || !c->text[0]) continue;
		MusicRow row;
		plainText(c->text, row.text, sizeof(row.text));
		row.child = children[i];
		row.colour = c->colour;
		int at = musicRowCount++;
		while (at > 0 && ys[at - 1] > c->y) {
			musicRows[at] = musicRows[at - 1];
			ys[at] = ys[at - 1];
			at--;
		}
		musicRows[at] = row;
		ys[at] = c->y;
	}
}

// Choosing a spell sends nothing. The press on its target is the action, as in the original.
static void selectSpell(const GridCell* cell) {
	castInterface = magicScript.iface;
	castComponent = cell->component;
	castMask = cell->mask;
	snprintf(castName, sizeof(castName), "%s", cell->name);
	for (int i = 0; i < gridCount; i++) gridCells[i].lit = gridCells[i].component == castComponent;
	snprintf(gridNote, sizeof(gridNote), "Cast on what?");
	// A spell that only works on items is no use without the bag, so it opens, as the original does.
	if ((castMask & 0x10) && !(castMask & 0x0A)) panelTab = TAB_BAG;
	panelDirty = true;
}

static void cancelCast(void) {
	castComponent = -1;
	if (gridOwner == TAB_MAGIC) {
		for (int i = 0; i < gridCount; i++) gridCells[i].lit = false;
		gridNote[0] = 0;
	}
	panelDirty = true;
}

// Sends a cast if this entry is one. Any other choice drops the spell, as walking away does.
static bool runCast(const void* entry) {
	const MenuEntry* e = entry;
	if (castComponent < 0) return false;
	int spell = castInterface << 16 | castComponent;
	int worldX = (net.zoneX - 6) * 8 + e->tileX, worldY = (net.zoneY - 6) * 8 + e->tileY;
	if (e->kind == ENTRY_CAST_NPC) net_spell_on_npc(&net, spell, e->object);
	else if (e->kind == ENTRY_CAST_PLAYER) net_spell_on_player(&net, spell, e->object);
	else if (e->kind == ENTRY_CAST_GROUND) net_spell_on_ground(&net, spell, e->object, worldX, worldY);
	else if (e->kind == ENTRY_CAST_ITEM) {
		int item = bagItemAt(e->object);
		if (item >= 0) net_spell_on_item(&net, spell, BAG_INTERFACE, e->object, item);
	} else if (e->kind == ENTRY_CAST_LOC) {
		const PickObject* p = &pickList[e->object];
		net_spell_on_loc(&net, spell, p->id, (net.zoneX - 6) * 8 + p->tileX, (net.zoneY - 6) * 8 + p->tileY);
	} else {
		cancelCast();
		return false;
	}
	actionsSent++;
	cancelCast();
	return true;
}

static void updateMusicTitle(void) {
	const char* now = net_component_text(&net, MUSIC_INTERFACE << 16 | MUSIC_NOW_COMPONENT);
	musicNow[0] = 0;
	if (now) plainText(now, musicNow, sizeof(musicNow));
}

static bool combatLit(const IfaceComponent* c) {
	if (!c->testKind || c->testVarp < 0 || c->testVarp >= NET_MAX_VARPS) return false;
	int value = net.varps[c->testVarp];
	if (c->testKind == 2) return value < c->testValue;
	if (c->testKind == 3) return value > c->testValue;
	if (c->testKind == 4) return value != c->testValue;
	return value == c->testValue;
}

static bool combatHidden(const IfaceComponent* c) {
	while (c) {
		int said = net_component_hidden(&net, c->id);
		if (said == 1 || (said != 0 && c->hidden)) return true;
		int index = c->parent & 0xFFFF;
		c = c->parent != -1 && index < combatIface.count ? &combatIface.components[index] : NULL;
	}
	return false;
}

static void rebuildCombat(void) {
	combatButtonCount = 0;
	int wanted = net_open_interface(&net, TAB_PARENT(TAB_COMBAT));
	if (combatLoaded && combatIface.id != wanted) {
		iface_free(&combatIface);
		combatLoaded = false;
	}
	if (!combatLoaded && wanted != -1 && iface_load(&world.cache, wanted, &combatIface) == 0) combatLoaded = true;
	if (!combatLoaded) return;
	for (int i = 0; i < combatIface.count && combatButtonCount < COMBAT_BUTTONS; i++) {
		const IfaceComponent* c = &combatIface.components[i];
		if (c->type == 255 || !c->option || !c->option[0] || combatHidden(c)) continue;
		if (c->buttonType != BUTTON_OK && c->buttonType != BUTTON_TOGGLE && c->buttonType != BUTTON_SELECT) continue;
		CombatButton* b = &combatButtons[combatButtonCount++];
		b->component = i;
		plainText(c->option, b->label, sizeof(b->label));
		b->retaliate = strstr(b->label, "Retaliate") != NULL;
		b->special = strstr(b->label, "Special") != NULL;
	}
}

// Each button's place: the styles two to a row, then whole rows for the rest.
static void combatPlace(int index, int width, int* x, int* y, int* w) {
	int styles = 0, place = 0;
	for (int i = 0; i < combatButtonCount; i++) {
		bool style = !combatButtons[i].retaliate && !combatButtons[i].special;
		if (i < index && style) styles++;
		if (style) place++;
	}
	int left = 8, full = width - 16, half = (full - 6) / 2, rows = (place + 1) / 2;
	if (!combatButtons[index].retaliate && !combatButtons[index].special) {
		*x = left + (styles & 1) * (half + 6);
		*y = CONTENT_TOP + 28 + (styles / 2) * (COMBAT_BUTTON_H + 3);
		*w = half;
		return;
	}
	int extra = 0;
	for (int i = 0; i < index; i++) if (combatButtons[i].retaliate || combatButtons[i].special) extra++;
	*x = left;
	*y = CONTENT_TOP + 28 + (rows + extra) * (COMBAT_BUTTON_H + 3);
	*w = full;
}

static int combatButtonAt(int px, int py, int width) {
	if (panelTab != TAB_COMBAT || !mapVisible() || dialogActive() || journalActive()) return -1;
	for (int i = 0; i < combatButtonCount; i++) {
		int x, y, w;
		combatPlace(i, width, &x, &y, &w);
		if (px >= x && py >= y && px < x + w && py < y + COMBAT_BUTTON_H) return i;
	}
	return -1;
}

static void pressCombat(int index) {
	const IfaceComponent* c = &combatIface.components[combatButtons[index].component];
	net_if_button(&net, combatIface.id, combatButtons[index].component);
	actionsSent++;
	// The original shows the change straight away, and the server's answer settles it.
	if (c->testKind && c->testVarp >= 0 && c->testVarp < NET_MAX_VARPS) {
		if (c->buttonType == BUTTON_SELECT) net.varps[c->testVarp] = c->testValue;
		else if (c->buttonType == BUTTON_TOGGLE) net.varps[c->testVarp] = 1 - net.varps[c->testVarp];
	}
	panelDirty = true;
}

static void drawCombat(u8* fb, int width) {
	if (!combatLoaded) {
		drawText(fb, width, width / 2 - 68, 116, "Not available yet", 200, 190, 170);
		return;
	}
	char line[64];
	const char* weapon = net_component_text(&net, combatIface.id << 16);
	snprintf(line, sizeof(line), "%s", weapon ? weapon : "");
	drawText(fb, width, (width - (int)strlen(line) * 8) / 2, CONTENT_TOP + 3, line, 255, 152, 31);
	snprintf(line, sizeof(line), "Combat Lvl: %d", net.looks[NET_SELF_ID].combat);
	drawText(fb, width, (width - (int)strlen(line) * 8) / 2, CONTENT_TOP + 15, line, 255, 152, 31);
	for (int i = 0; i < combatButtonCount; i++) {
		const CombatButton* b = &combatButtons[i];
		const IfaceComponent* c = &combatIface.components[b->component];
		int x, y, w;
		combatPlace(i, width, &x, &y, &w);
		bool lit = b->retaliate ? net.varps[VARP_RETALIATE_OFF] == 0 : b->special ? net.varps[VARP_SPECIAL_ON] == 1 : combatLit(c);
		fillRect(fb, width, x, y, w, COMBAT_BUTTON_H, 110, 90, 60);
		fillRect(fb, width, x + 2, y + 2, w - 4, COMBAT_BUTTON_H - 4, lit ? 120 : 84, lit ? 60 : 68, lit ? 40 : 46);
		char label[40];
		if (b->special) {
			int energy = net.varps[VARP_STYLE_SPECIAL_ENERGY] / 10;
			fillRect(fb, width, x + 4, y + COMBAT_BUTTON_H - 8, (w - 8) * energy / 100, 4, 230, 200, 40);
			snprintf(label, sizeof(label), "Special Attack: %d%%", energy);
		} else if (b->retaliate) {
			snprintf(label, sizeof(label), "Auto Retaliate (%s)", lit ? "On" : "Off");
		} else {
			snprintf(label, sizeof(label), "%s", b->label);
		}
		drawText(fb, width, x + (w - (int)strlen(label) * 8) / 2, y + (b->special ? 6 : 10), label, 255, 255, 255);
	}
}

// Returns true if the grid held another tab, leaving it empty for this one.
static bool takeGrid(void) {
	if (gridOwner == panelTab) return false;
	gridOwner = panelTab;
	gridScroll = 0;
	gridNote[0] = 0;
	return true;
}

// The server may refuse, such as just after a fight, and says why in a message.
static void pressLogout(void) {
	net_if_button(&net, 182, 6);
	logoutPressed = true;
	actionsSent++;
}

static void updateSummon(void);

// Runs once per frame while the tab is showing.
static void updateTabs(void) {
	updateSummon();
	if (!mapVisible()) return;
	int width = gameOnBottom ? 400 : 320;
	if (panelTab == TAB_COMBAT) {
		if (combatVersion != net.ifaceVersion) {
			combatVersion = net.ifaceVersion;
			rebuildCombat();
			panelDirty = true;
		}
		if (combatVarps != net.varpVersion) {
			combatVarps = net.varpVersion;
			panelDirty = true;
		}
	} else if (panelTab == TAB_OPTIONS) {
		if (combatVarps != net.varpVersion) {
			combatVarps = net.varpVersion;
			panelDirty = true;
		}
	} else if (panelTab == TAB_PRAYER) {
		if (refreshTabScript(&prayerScript) || takeGrid()) {
			rebuildPrayer();
			panelDirty = true;
		}
	} else if (panelTab == TAB_EMOTES) {
		if (refreshTabScript(&emoteScript) || takeGrid() || gridColumns != (width - SCROLL_W - 4) / EMOTE_CELL) {
			rebuildEmotes(width);
			panelDirty = true;
		}
	} else if (panelTab == TAB_MAGIC) {
		int book = net_open_interface(&net, TAB_PARENT(TAB_MAGIC));
		bool changedBook = book != magicScript.iface;
		if (changedBook) {
			magicScript.iface = book;
			magicScript.loaded = false;
			if (castComponent >= 0) cancelCast();
		}
		if (book != -1 && (refreshTabScript(&magicScript) || takeGrid() || changedBook || gridColumns != (width - SCROLL_W - 4) / MAGIC_CELL)) {
			rebuildMagic(width);
			panelDirty = true;
		}
	} else if (panelTab == TAB_MUSIC) {
		if (refreshTabScript(&musicScript)) {
			rebuildMusic();
			panelDirty = true;
		}
		if (musicVersion != net.ifaceVersion) {
			musicVersion = net.ifaceVersion;
			updateMusicTitle();
			panelDirty = true;
		}
	}
}

static void gridPlace(int index, int width, int* x, int* y) {
	int across = gridColumns * gridCellSize;
	int room = panelTab == TAB_PRAYER ? width : width - SCROLL_W;
	*x = (room - across) / 2 + (index % gridColumns) * gridCellSize;
	*y = GRID_TOP + (index / gridColumns) * gridCellSize - gridScroll;
}

static void drawGrid(u8* fb, int width) {
	const char* title = gridNote[0] ? gridNote : TAB_PLACES[panelTab].name;
	drawText(fb, width, (width - (int)strlen(title) * 8) / 2, CONTENT_TOP + 3, title, 255, 152, 31);
	const Sprite* glow = panelTab == TAB_EMOTES ? NULL : cachedSprite(155);
	for (int i = 0; i < gridCount; i++) {
		int x, y;
		gridPlace(i, width, &x, &y);
		if (y < GRID_TOP || y + gridCellSize > CONTENT_BOTTOM) continue;
		const GridCell* cell = &gridCells[i];
		if (cell->lit && glow) drawSprite(fb, width, glow, x, y);
		const Sprite* s = cachedSprite(cell->sprite);
		if (s) drawSprite(fb, width, s, x + (gridCellSize - s->width) / 2, y + (gridCellSize - s->height) / 2);
	}
	if (panelTab != TAB_PRAYER) drawScrollBar(fb, width, GRID_TOP, CONTENT_BOTTOM, gridScroll, gridTotal);
}

static void drawMusic(u8* fb, int width) {
	if (musicRowCount == 0) {
		drawText(fb, width, width / 2 - 40, 116, "Loading...", 200, 190, 170);
		return;
	}
	drawText(fb, width, (width - (int)strlen(musicNow) * 8) / 2, CONTENT_TOP + 3, musicNow, 233, 153, 0);
	for (int i = 0; i < musicRowCount; i++) {
		int y = QUEST_LIST_TOP + i * QUEST_ROW_H - musicScroll + 3;
		if (y < QUEST_LIST_TOP || y + 8 > CONTENT_BOTTOM) continue;
		const MusicRow* row = &musicRows[i];
		drawText(fb, width, 8, y, row->text, (u8)(row->colour >> 16), (u8)(row->colour >> 8), (u8)row->colour);
	}
	drawScrollBar(fb, width, QUEST_LIST_TOP, CONTENT_BOTTOM, musicScroll, musicRowCount * QUEST_ROW_H);
}

static int gridCellAt(int px, int py, int width) {
	for (int i = 0; i < gridCount; i++) {
		int x, y;
		gridPlace(i, width, &x, &y);
		if (y < GRID_TOP || y + gridCellSize > CONTENT_BOTTOM) continue;
		if (px >= x && py >= y && px < x + gridCellSize && py < y + gridCellSize) return i;
	}
	return -1;
}

// The list on these tabs, for the scrolling shared with quests.
static int* tabList(int* top, int* bottom, int* total) {
	if (panelTab == TAB_PRAYER || panelTab == TAB_EMOTES || panelTab == TAB_MAGIC) {
		if (gridCount == 0) return NULL;
		*top = GRID_TOP;
		*bottom = CONTENT_BOTTOM;
		*total = gridTotal;
		return &gridScroll;
	}
	if (panelTab == TAB_CHAT && chatRowCount > 0) {
		*top = CHAT_TOP;
		*bottom = SOCIAL_BOTTOM;
		*total = chatRowCount * CHAT_ROW_H;
		return &chatScroll;
	}
	if (socialTabShown() && socialCount > 0) {
		*top = QUEST_LIST_TOP;
		*bottom = SOCIAL_BOTTOM;
		*total = socialCount * QUEST_ROW_H;
		return &socialScroll;
	}
	if (panelTab == TAB_MUSIC && musicRowCount > 0) {
		*top = QUEST_LIST_TOP;
		*bottom = CONTENT_BOTTOM;
		*total = musicRowCount * QUEST_ROW_H;
		return &musicScroll;
	}
	return NULL;
}

// One press on a list row or icon. Returns true if it belonged to one of these tabs.
static bool pressTabRow(int x, int y, int width) {
	if (socialTabShown()) {
		pressSocialRow(y);
		return true;
	}
	if (panelTab == TAB_CHAT) {
		pressChatRow(y);
		return true;
	}
	if (panelTab == TAB_MAGIC) {
		int cell = gridCellAt(x, y, width);
		if (cell < 0 || !gridCells[cell].pressable) return true;
		if (gridCells[cell].mask == 0) {
			if (castComponent >= 0) cancelCast();
			net_button(&net, magicScript.iface, gridCells[cell].component);
			actionsSent++;
		} else if (castComponent == gridCells[cell].component) {
			cancelCast();
		} else {
			selectSpell(&gridCells[cell]);
		}
		return true;
	}
	if (panelTab == TAB_PRAYER || panelTab == TAB_EMOTES) {
		int cell = gridCellAt(x, y, width);
		if (cell >= 0 && gridCells[cell].pressable) {
			net_button(&net, panelTab == TAB_PRAYER ? PRAYER_INTERFACE : EMOTE_INTERFACE, gridCells[cell].component);
			actionsSent++;
			if (panelTab == TAB_EMOTES) snprintf(gridNote, sizeof(gridNote), "%s", gridCells[cell].name);
			panelDirty = true;
		}
		return true;
	}
	if (panelTab == TAB_MUSIC) {
		int row = (y - QUEST_LIST_TOP + musicScroll) / QUEST_ROW_H;
		if (y >= QUEST_LIST_TOP && row >= 0 && row < musicRowCount) {
			net_button_slot(&net, MUSIC_INTERFACE, MUSIC_LIST_COMPONENT, musicRows[row].child);
			actionsSent++;
		}
		return true;
	}
	return false;
}
