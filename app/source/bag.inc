// The bag: 28 items in a grid sized for the stylus, with their icons drawn from the cache.
// A press does the top option and a long press shows them all, the same as in the game view.

#define BAG_INVENTORY 93
#define BAG_SLOTS 28
#define BAG_COLUMNS 7
#define BAG_CELL_W 44
#define BAG_CELL_H 41
#define ICON_CACHE_SIZE 160

typedef struct {
	int id, stack; // id is -1 for a free entry
	u32 pixels[ICON_W * ICON_H];
} CachedIcon;

static CachedIcon iconCache[ICON_CACHE_SIZE];
static int iconNext = 0;
static long iconsBuilt = 0;
static bool iconCacheReady = false;
static long bagVersion = -1;

static const NetInv* bagItems(void) {
	return net_inventory(&net, BAG_INVENTORY);
}

static int bagItemAt(int slot) {
	const NetInv* inv = bagItems();
	return inv && slot >= 0 && slot < inv->count ? inv->items[slot] : -1;
}

static int bagAmountAt(int slot) {
	const NetInv* inv = bagItems();
	return inv && slot >= 0 && slot < inv->count ? inv->amounts[slot] : 0;
}

static const ObjDef* itemDef(int id) {
	return scene.cache ? scene_get_obj(&scene, id) : NULL;
}

static const char* itemName(int id) {
	const ObjDef* d = itemDef(id);
	return d && d->name ? d->name : "Item";
}

// Only items with stack pictures look different by amount.
static int iconStack(int id, int amount) {
	const ObjDef* d = itemDef(id);
	return d && d->hasCounts ? amount : 1;
}

static const CachedIcon* findIcon(int id, int stack) {
	if (!iconCacheReady) return NULL;
	for (int i = 0; i < ICON_CACHE_SIZE; i++) if (iconCache[i].id == id && iconCache[i].stack == stack) return &iconCache[i];
	return NULL;
}

// Makes an icon if it is missing, and returns true if it had to, so callers can stop at one a frame.
static bool ensureIcon(int id, int amount) {
	int stack = iconStack(id, amount);
	if (!iconCacheReady || findIcon(id, stack)) return false;
	CachedIcon* icon = &iconCache[iconNext];
	iconNext = (iconNext + 1) % ICON_CACHE_SIZE;
	memset(icon->pixels, 0, sizeof(icon->pixels));
	icon->id = id;
	icon->stack = stack;
	icon_render(&scene, world.palette, id, stack, icon->pixels);
	scene_free(&scene);
	iconsBuilt++;
	panelDirty = true;
	return true;
}

// Runs once per frame while the bag shows. An icon loads models from the SD card, so only one is made per frame.
static void updateBag(void) {
	if (!iconCacheReady) {
		for (int i = 0; i < ICON_CACHE_SIZE; i++) iconCache[i].id = -1;
		iconCacheReady = true;
	}
	if (bagVersion != net.invVersion) {
		bagVersion = net.invVersion;
		panelDirty = true;
		if (usingSlot >= 0 && bagItemAt(usingSlot) != usingItem) usingSlot = usingItem = -1;
	}
	if (panelTab != TAB_BAG && panelTab != TAB_WORN) return;
	const NetInv* shown = panelTab == TAB_BAG ? bagItems() : net_inventory(&net, 94);
	for (int slot = 0; shown && slot < shown->count; slot++) {
		if (shown->items[slot] >= 0 && ensureIcon(shown->items[slot], shown->amounts[slot])) return;
	}
}

static void bagCell(int slot, int width, int* x, int* y) {
	*x = (width - BAG_COLUMNS * BAG_CELL_W) / 2 + (slot % BAG_COLUMNS) * BAG_CELL_W;
	*y = CONTENT_TOP + 1 + (slot / BAG_COLUMNS) * BAG_CELL_H;
}

static int bagSlotAt(int px, int py, int width) {
	if (panelTab != TAB_BAG || !mapVisible() || dialogActive()) return -1;
	for (int slot = 0; slot < BAG_SLOTS; slot++) {
		int x, y;
		bagCell(slot, width, &x, &y);
		if (px >= x && py >= y && px < x + BAG_CELL_W && py < y + BAG_CELL_H) return slot;
	}
	return -1;
}

// The count in the corner, coloured by size as in the original.
static void drawItemAmount(u8* fb, int width, int id, int amount, int x, int y) {
	const ObjDef* d = itemDef(id);
	if (!(d && d->stackable) && amount == 1) return;
	char text[12];
	if (amount < 100000) snprintf(text, sizeof(text), "%d", amount);
	else if (amount < 10000000) snprintf(text, sizeof(text), "%dK", amount / 1000);
	else snprintf(text, sizeof(text), "%dM", amount / 1000000);
	u8 r = amount < 100000 ? 255 : amount < 10000000 ? 255 : 0;
	u8 b = amount < 100000 ? 0 : amount < 10000000 ? 255 : 128;
	drawText(fb, width, x, y, text, r, 255, b);
}

static void drawItemIcon(u8* fb, int width, int id, int amount, int x, int y) {
	const CachedIcon* icon = findIcon(id, iconStack(id, amount));
	if (!icon) return;
	for (int py = 0; py < ICON_H; py++)
		for (int px = 0; px < ICON_W; px++) {
			u32 c = icon->pixels[py * ICON_W + px];
			if (c) putPixel(fb, width, x + px, y + py, (u8)(c >> 16), (u8)(c >> 8), (u8)c);
		}
}

static void drawItemIconScaled(u8* fb, int width, int id, int amount, int x, int y, int scale) {
	const CachedIcon* icon = findIcon(id, iconStack(id, amount));
	if (!icon) return;
	for (int py = 0; py < ICON_H * scale; py++)
		for (int px = 0; px < ICON_W * scale; px++) {
			u32 c = icon->pixels[(py / scale) * ICON_W + px / scale];
			if (c) putPixel(fb, width, x + px, y + py, (u8)(c >> 16), (u8)(c >> 8), (u8)c);
		}
}

static void drawBag(u8* fb, int width) {
	for (int slot = 0; slot < BAG_SLOTS; slot++) {
		int x, y;
		bagCell(slot, width, &x, &y);
		bool chosen = slot == usingSlot;
		bool target = bagDragging && slot != bagPressSlot && bagSlotAt(bagTouchX, bagTouchY, width) == slot;
		fillRect(fb, width, x + 1, y + 1, BAG_CELL_W - 2, BAG_CELL_H - 2, chosen || target ? 150 : 74, chosen || target ? 130 : 63, chosen || target ? 70 : 48);
		int id = bagItemAt(slot);
		if (id < 0 || (bagDragging && slot == bagPressSlot)) continue; // a dragged item travels with the stylus
		int amount = bagAmountAt(slot);
		drawItemIcon(fb, width, id, amount, x + (BAG_CELL_W - ICON_W) / 2, y + (BAG_CELL_H - ICON_H) / 2 + 2);
		drawItemAmount(fb, width, id, amount, x + 3, y + 3);
	}
	if (bagDragging) drawItemIcon(fb, width, bagPressItem, bagAmountAt(bagPressSlot), bagTouchX - ICON_W / 2, bagTouchY - ICON_H / 2);
	if (usingSlot >= 0 || castComponent >= 0) {
		char line[48];
		if (castComponent >= 0) snprintf(line, sizeof(line), "Cast %s on...", castName);
		else snprintf(line, sizeof(line), "Use %s on...", itemName(usingItem));
		fillRect(fb, width, 0, CONTENT_BOTTOM - 12, width, 12, 30, 25, 18);
		drawText(fb, width, 6, CONTENT_BOTTOM - 10, line, 255, 255, 120);
	}
}

static void cancelUse(void) {
	usingSlot = usingItem = -1;
	panelDirty = true;
}

// The options for one slot, in the order the client lists them.
static void buildBagMenu(int slot) {
	int id = bagItemAt(slot);
	const ObjDef* d = itemDef(id);
	menuCount = 0;
	menuSelected = 0;
	if (!d) return;
	const char* name = d->name ? d->name : "";
	if (castComponent >= 0) {
		char verb[40];
		snprintf(verb, sizeof(verb), "Cast %s on", castName);
		if (castMask & 0x10) addEntry(ENTRY_CAST_ITEM, slot, 0, 0, 0, verb, name);
		addEntry(ENTRY_CANCEL, -1, 0, 0, 0, "Cancel", "");
		return;
	}
	for (int op = 0; op < 3; op++) if (d->iops[op]) addEntry(ENTRY_ITEM, slot, op, 0, 0, d->iops[op], name);
	addEntry(ENTRY_ITEM_USE, slot, 0, 0, 0, "Use", name);
	for (int op = 3; op < 5; op++) if (d->iops[op]) addEntry(ENTRY_ITEM, slot, op, 0, 0, d->iops[op], name);
	addEntry(ENTRY_ITEM_EXAMINE, id, 0, 0, 0, "Examine", name);
	addEntry(ENTRY_CANCEL, -1, 0, 0, 0, "Cancel", "");
}

// Using one item on another happens as soon as the second is pressed.
static bool useOnSlot(int slot) {
	if (usingSlot < 0) return false;
	if (slot != usingSlot && bagItemAt(slot) >= 0) {
		net_use_item_on_item(&net, BAG_INTERFACE, usingSlot, usingItem, slot, bagItemAt(slot));
		actionsSent++;
	}
	cancelUse();
	return true;
}

static void pressBag(int slot) {
	if (useOnSlot(slot) || bagItemAt(slot) < 0) return;
	buildBagMenu(slot);
	if (menuCount > 0 && menuEntries[0].kind != ENTRY_CANCEL) runEntry(&menuEntries[0]);
}
