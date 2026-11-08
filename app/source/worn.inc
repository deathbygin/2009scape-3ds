// The worn equipment tab: the original paper doll, with the rows closed up to fit between the tab bars.

#define WORN_INVENTORY 94
#define WORN_FRAME_SPRITE 170
#define WORN_CELL 36
#define WORN_ROW_STEP 32
#define WORN_COLUMN_STEP 56

typedef struct {
	int slot, column, row;
} WornPlace;

// Head, then cape, amulet and ammunition, then weapon, body and shield, then legs, then gloves, boots and ring.
static const WornPlace WORN_PLACES[] = {
	{0, 1, 0}, {1, 0, 1}, {2, 1, 1}, {13, 2, 1}, {3, 0, 2}, {4, 1, 2}, {5, 2, 2}, {7, 1, 3}, {9, 0, 4}, {10, 1, 4}, {12, 2, 4},
};
#define WORN_PLACE_COUNT 11

static Sprite wornFrame;
static char wornOptions[2][16] = {"Remove", "Operate"};

// The option names come from the interface in the cache, as the client reads them.
static void loadWornTab(void) {
	sprite_load(&world.cache, WORN_FRAME_SPRITE, &wornFrame);
	Iface f;
	if (iface_load(&world.cache, 387, &f) != 0) return;
	if (f.count > 28)
		for (int i = 0; i < 2 && i < f.components[28].opCount; i++)
			if (f.components[28].ops[i] && f.components[28].ops[i][0]) snprintf(wornOptions[i], sizeof(wornOptions[i]), "%s", f.components[28].ops[i]);
	iface_free(&f);
}

static int wornItemAt(int slot) {
	const NetInv* inv = net_inventory(&net, WORN_INVENTORY);
	return inv && slot >= 0 && slot < inv->count ? inv->items[slot] : -1;
}

static int wornAmountAt(int slot) {
	const NetInv* inv = net_inventory(&net, WORN_INVENTORY);
	return inv && slot >= 0 && slot < inv->count ? inv->amounts[slot] : 0;
}

static void wornCell(const WornPlace* p, int width, int* x, int* y) {
	*x = width / 2 - WORN_CELL / 2 + (p->column - 1) * WORN_COLUMN_STEP;
	*y = CONTENT_TOP + p->row * WORN_ROW_STEP;
}

static int wornSlotAt(int px, int py, int width) {
	if (panelTab != TAB_WORN || !mapVisible() || dialogActive()) return -1;
	for (int i = 0; i < WORN_PLACE_COUNT; i++) {
		int x, y;
		wornCell(&WORN_PLACES[i], width, &x, &y);
		if (px >= x && py >= y && px < x + WORN_CELL && py < y + WORN_ROW_STEP) return WORN_PLACES[i].slot;
	}
	return -1;
}

static void drawWorn(u8* fb, int width) {
	for (int i = 0; i < WORN_PLACE_COUNT; i++) {
		int x, y, slot = WORN_PLACES[i].slot;
		wornCell(&WORN_PLACES[i], width, &x, &y);
		drawSprite(fb, width, &wornFrame, x, y);
		int id = wornItemAt(slot);
		if (id < 0) continue;
		int amount = wornAmountAt(slot);
		drawItemIcon(fb, width, id, amount, x, y + 2);
		const ObjDef* d = itemDef(id);
		if (d && d->stackable) {
			char text[12];
			snprintf(text, sizeof(text), "%d", amount);
			drawText(fb, width, x + 2, y + 2, text, 255, 255, 0);
		}
	}
}

static void buildWornMenu(int slot) {
	int id = wornItemAt(slot);
	const ObjDef* d = itemDef(id);
	menuCount = 0;
	menuSelected = 0;
	if (!d) return;
	const char* name = d->name ? d->name : "";
	addEntry(ENTRY_WORN, slot, 0, 0, 0, wornOptions[0], name);
	addEntry(ENTRY_WORN, slot, 1, 0, 0, wornOptions[1], name);
	addEntry(ENTRY_ITEM_EXAMINE, id, 0, 0, 0, "Examine", name);
	addEntry(ENTRY_CANCEL, -1, 0, 0, 0, "Cancel", "");
}

static void pressWorn(int slot) {
	if (wornItemAt(slot) < 0) return;
	buildWornMenu(slot);
	if (menuCount > 0) runEntry(&menuEntries[0]);
}
