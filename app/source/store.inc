// The bank and shops, which are the same screen here: a grid of items, a button to look at the bag instead, a tap
// for the first option and a hold for them all, as in the bag. Everything sent comes from one press.
// Also answers the server's prompts for an amount or a name, since those are how "X" options work.

#define PAGE_TOP 38
#define PAGE_BOTTOM (SCREEN_H - 4)
#define PAGE_CELL_W 44
#define PAGE_CELL_H 38
#define PAGE_BUTTON_W 70
#define PAGE_BUTTON_Y 20
#define PAGE_BUTTON_H 16
#define BANK_INVENTORY 95
#define BAG_INVENTORY 93
#define SHOP_INVENTORY 92
#define LAST_AMOUNT_VARP 1249
#define BANK_NOTE_BUTTON 16
#define BANK_NOTE_VARP 115
#define SHOP_MAIN_BUTTON 25
#define SHOP_PLAYER_BUTTON 26
#define PROMPT_NUMBER 108
#define TRADE_YOU_CHANGED_VARP 1042
#define TRADE_THEY_CHANGED_VARP 1043
#define PROMPT_NAME 109
#define BURDEN_INTERFACE 671
#define BURDEN_BAG_INTERFACE 665
#define BURDEN_COMPONENT 27
#define BURDEN_INVENTORY 30
#define BURDEN_TAKE_ALL 29
#define POUCH_INTERFACE 669
#define SCROLL_INTERFACE 673
#define INFUSE_COMPONENT 15
#define INFUSE_SWAP_TO_SCROLLS 18
#define INFUSE_SWAP_TO_POUCHES 17
#define INFUSE_ENTRIES 78
#define ENUM_POUCH_ITEM 1182
#define ENUM_POUCH_DARK 1183
#define ENUM_SCROLL_DARK 1184
#define ENUM_INFUSE_LEVEL 1185
#define ENUM_SCROLL_ITEM 1188
#define ENUM_SCROLL_POUCH 1277
#define NO_POUCH_ITEM 12292
#define NO_SCROLL_ITEM 12421
#define SCRIPT_CAN_INFUSE 766
#define SCRIPT_CAN_TRANSFORM 768
#define SKILL_SUMMONING 23

enum { PAGE_NONE, PAGE_BANK, PAGE_SHOP, PAGE_TRADE, PAGE_TRADE_CONFIRM, PAGE_BURDEN, PAGE_INFUSE };
enum { TRADE_BAG, TRADE_YOURS, TRADE_THEIRS };

typedef struct {
	const char* label;
	int opcode;
} PageOption;

#define LAST_AMOUNT_OPCODE 199

static const PageOption WITHDRAW_OPTIONS[] = {{"Withdraw-1", 155}, {"Withdraw-5", 196}, {"Withdraw-10", 124}, {"", LAST_AMOUNT_OPCODE}, {"Withdraw-X", 234}, {"Withdraw-All", 168}, {"Withdraw-All but one", 166}, {"Examine", 9}, {NULL, 0}};
static const PageOption DEPOSIT_OPTIONS[] = {{"Deposit-1", 155}, {"Deposit-5", 196}, {"Deposit-10", 124}, {"", LAST_AMOUNT_OPCODE}, {"Deposit-X", 234}, {"Deposit-All", 168}, {"Examine", 9}, {NULL, 0}};
static const PageOption BUY_OPTIONS[] = {{"Value", 155}, {"Buy 1", 196}, {"Buy 5", 124}, {"Buy 10", 199}, {"Buy X", 234}, {"Examine", 9}, {NULL, 0}};
static const PageOption SELL_OPTIONS[] = {{"Value", 155}, {"Sell 1", 196}, {"Sell 5", 124}, {"Sell 10", 199}, {"Sell X", 234}, {NULL, 0}};

static const PageOption OFFER_OPTIONS[] = {{"Offer", 155}, {"Offer-5", 196}, {"Offer-10", 124}, {"Offer-All", 199}, {"Offer-X", 234}, {"Examine", 9}, {NULL, 0}};
static const PageOption REMOVE_OPTIONS[] = {{"Remove", 155}, {"Remove-5", 196}, {"Remove-10", 124}, {"Remove-All", 199}, {"Remove-X", 234}, {"Examine", 9}, {NULL, 0}};
static const PageOption THEIR_OPTIONS[] = {{"Examine", 9}, {NULL, 0}};
static const PageOption BEAST_TAKE_OPTIONS[] = {{"Withdraw-1", 155}, {"Withdraw-5", 196}, {"Withdraw-10", 124}, {"Withdraw-All", 199}, {"Withdraw-X", 234}, {"Examine", 168}, {NULL, 0}};
static const PageOption BEAST_GIVE_OPTIONS[] = {{"Store-1", 155}, {"Store-5", 196}, {"Store-10", 124}, {"Store-All", 199}, {"Store-X", 234}, {"Examine", 168}, {NULL, 0}};
static const PageOption INFUSE_OPTIONS[] = {{"Infuse", 155}, {"Infuse-5", 196}, {"Infuse-10", 124}, {"Infuse-All", 199}, {"Infuse-X", 234}, {"List", 166}, {NULL, 0}};
static const PageOption TRANSFORM_OPTIONS[] = {{"Transform", 155}, {"Transform-5", 196}, {"Transform-10", 124}, {"Transform-All", 199}, {"Transform-X", 234}, {NULL, 0}};

static int pageKind = PAGE_NONE, pageScroll, tradeView = TRADE_BAG;
static bool pageOnBag, pageMainStock = true;
static long pageInvSeen = -1, pageVarpsSeen = -1, pageTextSeen = -1, promptSeen;

// The pouch and scroll lists are not containers, but they show and press just like one. A slot here is the number
// the original gives the entry, which is what the server expects back. What cannot be made yet shows its dark item.
static NetInv infuseList;
static int infuseItems[INFUSE_ENTRIES], infuseLevels[INFUSE_ENTRIES], infuseInterface = -1;
static long infuseInvSeen = -1, infuseStatsSeen = -1;

static bool pageActive(void) {
	return pageKind != PAGE_NONE;
}

static bool pageIsConfirm(void) {
	return pageKind == PAGE_TRADE_CONFIRM;
}

static void resetStore(void) {
	pageKind = PAGE_NONE;
	promptSeen = 0;
}

static const NetInv* pageInventory(void) {
	if (pageKind == PAGE_INFUSE) return &infuseList;
	if (pageKind == PAGE_BURDEN) return net_inventory(&net, pageOnBag ? BAG_INVENTORY : BURDEN_INVENTORY);
	if (pageKind == PAGE_TRADE) return net_inventory(&net, tradeView == TRADE_BAG ? BAG_INVENTORY : tradeView == TRADE_YOURS ? 90 : 91 + 32768);
	if (pageKind == PAGE_TRADE_CONFIRM) return NULL;
	return net_inventory(&net, pageOnBag ? BAG_INVENTORY : pageKind == PAGE_BANK ? BANK_INVENTORY : SHOP_INVENTORY);
}

static const PageOption* pageOptions(void) {
	if (pageKind == PAGE_INFUSE) return infuseInterface == POUCH_INTERFACE ? INFUSE_OPTIONS : TRANSFORM_OPTIONS;
	if (pageKind == PAGE_BURDEN) return pageOnBag ? BEAST_GIVE_OPTIONS : BEAST_TAKE_OPTIONS;
	if (pageKind == PAGE_TRADE) return tradeView == TRADE_BAG ? OFFER_OPTIONS : tradeView == TRADE_YOURS ? REMOVE_OPTIONS : THEIR_OPTIONS;
	if (pageKind == PAGE_BANK) return pageOnBag ? DEPOSIT_OPTIONS : WITHDRAW_OPTIONS;
	return pageOnBag ? SELL_OPTIONS : BUY_OPTIONS;
}

static int pageInterface(void) {
	if (pageKind == PAGE_INFUSE) return infuseInterface;
	if (pageKind == PAGE_BURDEN) return pageOnBag ? BURDEN_BAG_INTERFACE : BURDEN_INTERFACE;
	if (pageKind == PAGE_TRADE) return tradeView == TRADE_BAG ? 336 : 335;
	if (pageKind == PAGE_BANK) return pageOnBag ? 763 : 762;
	return pageOnBag ? 621 : 620;
}

static int pageComponent(void) {
	if (pageKind == PAGE_INFUSE) return INFUSE_COMPONENT;
	if (pageKind == PAGE_BURDEN) return pageOnBag ? 0 : BURDEN_COMPONENT;
	if (pageKind == PAGE_TRADE) return tradeView == TRADE_BAG ? 0 : tradeView == TRADE_YOURS ? 30 : 32;
	if (pageOnBag) return 0;
	return pageKind == PAGE_BANK ? 73 : pageMainStock ? 23 : 24;
}

static int pageColumns(int width) {
	return (width - SCROLL_W - 4) / PAGE_CELL_W;
}

// One past the last item, so an empty tail is not scrolled through.
static int pageSlots(void) {
	const NetInv* inv = pageInventory();
	if (!inv) return 0;
	int last = inv->count;
	while (last > 0 && inv->items[last - 1] < 0) last--;
	return last;
}

static int pageTotal(int width) {
	int columns = pageColumns(width);
	return (pageSlots() + columns - 1) / columns * PAGE_CELL_H;
}

static void pageCell(int slot, int width, int* x, int* y) {
	int columns = pageColumns(width);
	*x = 2 + (slot % columns) * PAGE_CELL_W;
	*y = PAGE_TOP + (slot / columns) * PAGE_CELL_H - pageScroll;
}

static int pageSlotAt(int px, int py, int width) {
	if (py < PAGE_TOP || py >= PAGE_BOTTOM) return -1;
	int column = (px - 2) / PAGE_CELL_W, row = (py - PAGE_TOP + pageScroll) / PAGE_CELL_H;
	if (px < 2 || column >= pageColumns(width)) return -1;
	int slot = row * pageColumns(width) + column;
	const NetInv* inv = pageInventory();
	return inv && slot < inv->count && inv->items[slot] >= 0 ? slot : -1;
}

// Whether the original would light this entry: the scripts look at the bag, and the level is checked as they do.
static bool infuseCanMake(bool pouch, int item, int source, int level) {
	int32_t args[3] = {pouch ? SCRIPT_CAN_INFUSE : SCRIPT_CAN_TRANSFORM, item, source};
	int answer = 0;
	if (cs2_call_int(args, pouch ? 2 : 3, &answer) != 0) return true; // the server still has the last word
	return answer == 1 && baseLevel(SKILL_SUMMONING) >= level;
}

static void rebuildInfuse(void) {
	bool pouch = infuseInterface == POUCH_INTERFACE;
	infuseList.count = INFUSE_ENTRIES;
	for (int slot = 0; slot < INFUSE_ENTRIES; slot++) {
		int key = slot + 1, item = cs2_enum_int(pouch ? ENUM_POUCH_ITEM : ENUM_SCROLL_ITEM, key);
		int source = pouch ? item : cs2_enum_int(ENUM_SCROLL_POUCH, key);
		infuseList.items[slot] = -1;
		infuseList.amounts[slot] = 1;
		infuseItems[slot] = item;
		infuseLevels[slot] = 0;
		if (item <= 0 || item == (pouch ? NO_POUCH_ITEM : NO_SCROLL_ITEM)) continue;
		infuseLevels[slot] = cs2_enum_int(ENUM_INFUSE_LEVEL, source);
		int dark = cs2_enum_int(pouch ? ENUM_POUCH_DARK : ENUM_SCROLL_DARK, key);
		infuseList.items[slot] = infuseCanMake(pouch, item, source, infuseLevels[slot]) || dark <= 0 ? item : dark;
	}
}

static void updateStore(void) {
	int width = gameOnBottom ? 400 : 320;
	int kind = net_interface_is_open(&net, 762) ? PAGE_BANK : net_interface_is_open(&net, 620) ? PAGE_SHOP : net_interface_is_open(&net, 335) ? PAGE_TRADE : net_interface_is_open(&net, 334) ? PAGE_TRADE_CONFIRM : net_interface_is_open(&net, BURDEN_INTERFACE) ? PAGE_BURDEN : net_interface_is_open(&net, POUCH_INTERFACE) || net_interface_is_open(&net, SCROLL_INTERFACE) ? PAGE_INFUSE : PAGE_NONE;
	if (kind != pageKind) {
		pageKind = kind;
		pageOnBag = false;
		pageMainStock = true;
		pageScroll = 0;
		tradeView = TRADE_BAG;
		listTouchCancel();
		if (menuOpen) closeMenu();
		panelDirty = true;
	}
	if (!pageActive()) return;
	if (pageKind == PAGE_INFUSE) {
		int open = net_interface_is_open(&net, POUCH_INTERFACE) ? POUCH_INTERFACE : SCROLL_INTERFACE;
		if (open != infuseInterface || infuseInvSeen != net.invVersion || infuseStatsSeen != net.statVersion) {
			if (open != infuseInterface) pageScroll = 0;
			infuseInterface = open;
			infuseInvSeen = net.invVersion;
			infuseStatsSeen = net.statVersion;
			rebuildInfuse();
			panelDirty = true;
		}
	}
	if (pageInvSeen != net.invVersion || pageVarpsSeen != net.varpVersion || pageTextSeen != net.ifaceVersion) {
		pageTextSeen = net.ifaceVersion;
		pageInvSeen = net.invVersion;
		pageVarpsSeen = net.varpVersion;
		scrollList(&pageScroll, pageScroll, PAGE_BOTTOM - PAGE_TOP, pageTotal(width));
		panelDirty = true;
	}
	const NetInv* inv = pageInventory();
	int slots = pageSlots();
	for (int slot = 0; inv && slot < slots; slot++) {
		int x, y;
		pageCell(slot, width, &x, &y);
		if (inv->items[slot] < 0 || y + PAGE_CELL_H < PAGE_TOP || y >= PAGE_BOTTOM) continue;
		if (ensureIcon(inv->items[slot], inv->amounts[slot])) break;
	}
}

static bool bankNotes(void) {
	return net.varps[BANK_NOTE_VARP] != 0;
}

static void pageButton(u8* fb, int width, int x, const char* label, bool lit) {
	fillRect(fb, width, x, PAGE_BUTTON_Y, PAGE_BUTTON_W, PAGE_BUTTON_H, 110, 90, 60);
	fillRect(fb, width, x + 1, PAGE_BUTTON_Y + 1, PAGE_BUTTON_W - 2, PAGE_BUTTON_H - 2, lit ? 120 : 84, lit ? 60 : 68, lit ? 40 : 46);
	drawText(fb, width, x + (PAGE_BUTTON_W - (int)strlen(label) * 8) / 2, PAGE_BUTTON_Y + 4, label, 255, 255, 255);
}

static void pageText(int component, char* out, size_t size) {
	const char* text = net_component_text(&net, component);
	out[0] = 0;
	if (text) plainText(text, out, size);
}

// The last screen lists both offers as the server wrote them, one line per item.
static void drawConfirmList(u8* fb, int width, int component, int x, const char* heading) {
	const char* text = net_component_text(&net, 334 << 16 | component);
	int y = 62;
	drawFontText(fb, width, &smallFont, heading, x, 46, 255, 255, 0);
	char line[64];
	while (text && *text && y < 200) {
		const char* end = strstr(text, "<br>");
		size_t length = end ? (size_t)(end - text) : strlen(text);
		char raw[64];
		snprintf(raw, sizeof(raw), "%.*s", length < 63 ? (int)length : 63, text);
		plainText(raw, line, sizeof(line));
		if (line[0]) {
			drawFontText(fb, width, &smallFont, line, x, y, 255, 255, 255);
			y += 12;
		}
		text = end ? end + 4 : NULL;
	}
}

static void drawTradeConfirm(u8* fb, int width) {
	fillRect(fb, width, 0, 0, width, SCREEN_H, 62, 53, 41);
	char line[64];
	pageText(334 << 16 | 2, line, sizeof(line));
	drawFontText(fb, width, &overheadFont, line, 6, 3, 0, 255, 255);
	drawFontText(fb, width, &smallFont, "Are you sure you want to make this trade?", 6, 22, 255, 255, 255);
	drawConfirmList(fb, width, 37, 6, "You are about to give:");
	drawConfirmList(fb, width, 41, width / 2 + 4, "In return you will receive:");
	if (net_component_hidden(&net, 334 << 16 | 46) == 0) {
		fillRect(fb, width, 0, 188, width, 14, 110, 20, 20);
		drawCentred(fb, width, &smallFont, "Trade modified by the other player!", width / 2, 189, 255, 255, 255);
	} else if (net_component_hidden(&net, 334 << 16 | 45) == 0) {
		drawCentred(fb, width, &smallFont, "You modified the trade", width / 2, 189, 255, 200, 120);
	}
	pageText(334 << 16 | 33, line, sizeof(line));
	drawFontText(fb, width, &smallFont, line, 6, 204, 255, 255, 255);
	fillRect(fb, width, 8, 216, 146, 21, 60, 110, 60);
	fillRect(fb, width, width - 154, 216, 146, 21, 120, 50, 40);
	drawText(fb, width, 8 + 73 - 24, 223, "Accept", 255, 255, 255);
	drawText(fb, width, width - 154 + 73 - 28, 223, "Decline", 255, 255, 255);
}

static void drawPage(u8* fb, int width) {
	if (pageKind == PAGE_TRADE_CONFIRM) {
		drawTradeConfirm(fb, width);
		return;
	}
	const NetInv* inv = pageInventory();
	int slots = pageSlots();
	for (int slot = 0; inv && slot < slots; slot++) {
		int x, y;
		pageCell(slot, width, &x, &y);
		if (y < PAGE_TOP || y + PAGE_CELL_H > PAGE_BOTTOM) continue;
		fillRect(fb, width, x + 1, y + 1, PAGE_CELL_W - 2, PAGE_CELL_H - 2, 74, 63, 48);
		if (inv->items[slot] < 0) continue;
		drawItemIcon(fb, width, inv->items[slot], inv->amounts[slot], x + 4, y + 3);
		if (pageKind == PAGE_INFUSE) {
			char level[8];
			snprintf(level, sizeof(level), "%d", infuseLevels[slot]);
			drawCentred(fb, width, &smallFont, level, x + PAGE_CELL_W / 2, y + 25, 255, 200, 60);
		} else {
			drawItemAmount(fb, width, inv->items[slot], inv->amounts[slot], x + 2, y + 2);
		}
	}
	fillRect(fb, width, 0, 0, width, PAGE_TOP - 1, 62, 53, 41);
	char title[48];
	const char* shop = pageKind == PAGE_SHOP ? net_component_text(&net, 620 << 16 | 22) : NULL;
	if (pageKind == PAGE_BANK) snprintf(title, sizeof(title), "Bank of 2009scape");
	else if (pageKind == PAGE_TRADE) pageText(335 << 16 | 15, title, sizeof(title));
	else if (pageKind == PAGE_BURDEN) snprintf(title, sizeof(title), "Familiar Inventory");
	else if (pageKind == PAGE_INFUSE) snprintf(title, sizeof(title), infuseInterface == POUCH_INTERFACE ? "Pouch Creation" : "Scroll Creation");
	else plainText(shop ? shop : "Shop", title, sizeof(title));
	drawFontText(fb, width, &overheadFont, title, 6, 3, 255, 152, 31);
	int closeX = width - CLOSE_W - 2;
	fillRect(fb, width, closeX, 2, CLOSE_W, 16, 110, 90, 60);
	fillRect(fb, width, closeX + 1, 3, CLOSE_W - 2, 14, 84, 68, 46);
	if (pageKind == PAGE_TRADE) {
		drawCentred(fb, width, &smallFont, "Decline", closeX + CLOSE_W / 2, 4, 255, 200, 200);
		pageButton(fb, width, 4, "Bag", tradeView == TRADE_BAG);
		pageButton(fb, width, 8 + PAGE_BUTTON_W, "Yours", tradeView == TRADE_YOURS);
		pageButton(fb, width, 12 + PAGE_BUTTON_W * 2, "Theirs", tradeView == TRADE_THEIRS);
		pageButton(fb, width, 16 + PAGE_BUTTON_W * 3, "Accept", false);
		// The original's warning that an offer changed, which is how a late swap gets noticed.
		if (net.varps[TRADE_THEY_CHANGED_VARP]) {
			fillRect(fb, width, 0, PAGE_BOTTOM - 26, width - SCROLL_W, 14, 110, 20, 20);
			drawCentred(fb, width, &smallFont, "Trade modified by the other player!", (width - SCROLL_W) / 2, PAGE_BOTTOM - 25, 255, 255, 255);
		} else if (net.varps[TRADE_YOU_CHANGED_VARP]) {
			fillRect(fb, width, 0, PAGE_BOTTOM - 26, width - SCROLL_W, 14, 110, 70, 20);
			drawCentred(fb, width, &smallFont, "You modified the trade", (width - SCROLL_W) / 2, PAGE_BOTTOM - 25, 255, 255, 255);
		}
		char status[64];
		pageText(335 << 16 | 36, status, sizeof(status));
		if (status[0]) {
			fillRect(fb, width, 0, PAGE_BOTTOM - 12, width - SCROLL_W, 14, 30, 25, 18);
			drawCentred(fb, width, &smallFont, status, (width - SCROLL_W) / 2, PAGE_BOTTOM - 12, 255, 255, 255);
		}
		drawScrollBar(fb, width, PAGE_TOP, PAGE_BOTTOM, pageScroll, pageTotal(width));
		return;
	}
	drawText(fb, width, closeX + 6, 6, "Close", 255, 255, 255);
	if (pageKind == PAGE_INFUSE) pageButton(fb, width, 4, infuseInterface == POUCH_INTERFACE ? "Scrolls" : "Pouches", false);
	else pageButton(fb, width, 4, pageOnBag ? (pageKind == PAGE_BANK ? "Bank" : pageKind == PAGE_BURDEN ? "Familiar" : "Shop") : "Bag", false);
	if (pageKind == PAGE_BANK) pageButton(fb, width, 8 + PAGE_BUTTON_W, "Notes", bankNotes());
	else if (pageKind == PAGE_BURDEN) pageButton(fb, width, 8 + PAGE_BUTTON_W, "Take all", false);
	else if (pageKind == PAGE_SHOP && !pageOnBag) pageButton(fb, width, 8 + PAGE_BUTTON_W, pageMainStock ? "Main" : "Sold", false);
	drawScrollBar(fb, width, PAGE_TOP, PAGE_BOTTOM, pageScroll, pageTotal(width));
}

static int* pageList(int* top, int* bottom, int* total) {
	*top = PAGE_TOP;
	*bottom = PAGE_BOTTOM;
	*total = pageTotal(gameOnBottom ? 400 : 320);
	return &pageScroll;
}

static void pageSend(int opcode, int slot) {
	net_if_option(&net, opcode, pageInterface(), pageComponent(), slot);
	actionsSent++;
}

static void pressPageCell(int x, int y, int width) {
	int slot = pageSlotAt(x, y, width);
	if (slot >= 0) pageSend(pageOptions()[0].opcode, slot);
}

// Every option for what is under the stylus. Returns false if there is nothing there.
static bool openPageMenu(int x, int y, int width) {
	int slot = pageSlotAt(x, y, width);
	if (slot < 0) return false;
	const NetInv* inv = pageInventory();
	const char* name = itemName(pageKind == PAGE_INFUSE ? infuseItems[slot] : inv->items[slot]);
	menuCount = 0;
	menuSelected = 0;
	for (const PageOption* o = pageOptions(); o->label; o++) {
		char label[32];
		snprintf(label, sizeof(label), "%s", o->label);
		if (!label[0]) {
			int last = net.varps[LAST_AMOUNT_VARP];
			if (last <= 0) continue;
			snprintf(label, sizeof(label), "%s-%d", pageOnBag ? "Deposit" : "Withdraw", last);
		}
		addEntry(ENTRY_PAGE, slot, o->opcode, 0, 0, label, name);
	}
	addEntry(ENTRY_CANCEL, -1, 0, 0, 0, "Cancel", "");
	menuOpen = true;
	panelDirty = true;
	return true;
}

static bool runPage(const void* entry) {
	const MenuEntry* e = entry;
	if (e->kind != ENTRY_PAGE) return false;
	if (pageActive()) pageSend(e->option, e->object);
	return true;
}

static bool pressPage(int x, int y, int width) {
	if (!pageActive() || !mapVisible() || dialogActive()) return false;
	if (pageKind == PAGE_TRADE_CONFIRM) {
		if (y >= 216 && y < 237 && x >= 8 && x < 154) net_button(&net, 334, 20);
		else if (y >= 216 && y < 237 && x >= width - 154 && x < width - 8) net_button(&net, 334, 21);
		else return true;
		actionsSent++;
		return true;
	}
	if (y < PAGE_BUTTON_Y && x >= width - CLOSE_W - 2) {
		if (pageKind == PAGE_TRADE) net_button(&net, 335, 18);
		else net_close_modal(&net);
		actionsSent++;
		return true;
	}
	if (pageKind == PAGE_TRADE && y >= PAGE_BUTTON_Y && y < PAGE_BUTTON_Y + PAGE_BUTTON_H) {
		int button = (x - 4) / (PAGE_BUTTON_W + 4);
		if (button == 3) {
			net_button(&net, 335, 16);
			actionsSent++;
		} else if (button >= 0 && button < 3) {
			tradeView = button;
			pageScroll = 0;
			panelDirty = true;
		}
		return true;
	}
	if (pageKind == PAGE_INFUSE && y >= PAGE_BUTTON_Y && y < PAGE_BUTTON_Y + PAGE_BUTTON_H) {
		if (x >= 4 && x < 4 + PAGE_BUTTON_W) {
			net_button(&net, infuseInterface, infuseInterface == POUCH_INTERFACE ? INFUSE_SWAP_TO_SCROLLS : INFUSE_SWAP_TO_POUCHES);
			actionsSent++;
		}
		return true;
	}
	if (y >= PAGE_BUTTON_Y && y < PAGE_BUTTON_Y + PAGE_BUTTON_H) {
		if (x >= 4 && x < 4 + PAGE_BUTTON_W) {
			pageOnBag = !pageOnBag;
			pageScroll = 0;
			panelDirty = true;
		} else if (x >= 8 + PAGE_BUTTON_W && x < 8 + PAGE_BUTTON_W * 2) {
			if (pageKind == PAGE_BANK) {
				net_button(&net, 762, BANK_NOTE_BUTTON);
				actionsSent++;
			} else if (pageKind == PAGE_BURDEN) {
				net_button(&net, BURDEN_INTERFACE, BURDEN_TAKE_ALL);
				actionsSent++;
			} else if (!pageOnBag) {
				pageMainStock = !pageMainStock;
				pageScroll = 0;
				net_button(&net, 620, pageMainStock ? SHOP_MAIN_BUTTON : SHOP_PLAYER_BUTTON);
				actionsSent++;
				panelDirty = true;
			}
		}
		return true;
	}
	if (y >= PAGE_TOP && x >= width - SCROLL_W) {
		int total = pageTotal(width);
		if (y < PAGE_TOP + 18) scrollList(&pageScroll, pageScroll - SCROLL_STEP, PAGE_BOTTOM - PAGE_TOP, total);
		else if (y >= PAGE_BOTTOM - 18) scrollList(&pageScroll, pageScroll + SCROLL_STEP, PAGE_BOTTOM - PAGE_TOP, total);
		return true;
	}
	if (y >= PAGE_TOP) pressPageCell(x, y, width); // the cursor comes here, and the stylus goes through the list
	return true;
}

// The server asks for typed input with a script. Cancelling just leaves it unanswered, as the original can.
static void updatePrompt(void) {
	if (promptSeen == net.promptVersion) return;
	promptSeen = net.promptVersion;
	char text[84], hint[64];
	plainText(net.promptText, hint, sizeof(hint));
	panelDirty = true;
	if (!askForText(hint, text, sizeof(text), net.promptKind == PROMPT_NUMBER ? 10 : net.promptKind == PROMPT_NAME ? 12 : 80)) return;
	if (net.promptKind == PROMPT_NUMBER) net_answer_number(&net, atoi(text));
	else if (net.promptKind == PROMPT_NAME) net_answer_name(&net, net_name_key(text));
	else net_answer_text(&net, text);
	actionsSent++;
}
