// The Quest List tab and the quest journal. The client's own scripts work out the list from the game's variables,
// so each quest gets the same name, colour and place as the original, and only the drawing is done here.
// Pressing a quest asks the server for its journal: one press, one request. Scrolling sends nothing.

#define QUEST_INTERFACE 274
#define QUEST_LIST_PARENT (QUEST_INTERFACE << 16 | 1)
#define QUEST_TITLE (QUEST_INTERFACE << 16 | 2)
#define JOURNAL_INTERFACE 275
#define JOURNAL_LIST_PARENT (JOURNAL_INTERFACE << 16 | 6)
#define JOURNAL_TITLE (JOURNAL_INTERFACE << 16 | 2)
#define QUEST_ROWS 200
#define QUEST_ROW_H 15
#define QUEST_LIST_TOP (CONTENT_TOP + 15)
#define QUEST_REFRESH_MS 600
#define JOURNAL_ROWS 400
#define JOURNAL_COLS 48
#define JOURNAL_ROW_H 12
#define JOURNAL_TOP 26
#define JOURNAL_BOTTOM (SCREEN_H - 4)
#define JOURNAL_STRUCK 0x1000000u
#define CLOSE_W 52
#define CLOSE_H 20
#define SCROLL_W 22
#define SCROLL_STEP 120
#define SCROLL_DRAG_PIXELS 8

typedef struct {
	char text[40];
	int component, y, colour;
	bool pressable;
} QuestRow;

typedef struct {
	char text[JOURNAL_COLS + 1];
	u32 marks[JOURNAL_COLS]; // the colour of each letter, and whether it is struck through
} JournalRow;

static QuestRow questRows[QUEST_ROWS];
static int questRowCount, questTotal, questScroll;
static char questTitle[40];
static int questTitleColour;
static bool questAvailable;

static JournalRow journalRows[JOURNAL_ROWS];
static int journalRowCount, journalScroll, journalWidth;
static char journalTitle[48];
static bool journalOpen;
static long journalVersion = -1;

// Keeps an interface's scripts up to date while its tab is showing. Returns true if anything ran.
typedef struct {
	int iface;
	bool loaded;
	long varps, stats, invs;
	u32 refreshed;
	int ms, result;
} TabScript;

// What changed since a version, with -1 if too much did to say.
static int recentChanges(const int* ring, long from, long to, int* out) {
	if (to - from >= NET_CHANGE_RING) {
		out[0] = -1;
		return 1;
	}
	int count = 0;
	for (long i = from; i < to; i++) out[count++] = ring[i % NET_CHANGE_RING];
	return count;
}

static bool refreshTabScript(TabScript* t) {
	if (!net_interface_is_open(&net, t->iface)) return false;
	u32 now = (u32)nowMs();
	bool changed = t->varps != net.varpVersion || t->stats != net.statVersion || t->invs != net.invVersion;
	if (t->loaded && (!changed || now - t->refreshed < QUEST_REFRESH_MS)) return false;
	int varpList[NET_CHANGE_RING], statList[NET_CHANGE_RING], everything = -1;
	int varpCount = recentChanges(net.varpChanges, t->varps, net.varpVersion, varpList);
	int statCount = recentChanges(net.statChanges, t->stats, net.statVersion, statList);
	bool first = !t->loaded, invChanged = t->invs != net.invVersion;
	long before = cs2_instructions();
	t->invs = net.invVersion;
	t->varps = net.varpVersion;
	t->stats = net.statVersion;
	t->refreshed = now;
	int load = first ? cs2_run_load(t->iface) : 0;
	t->loaded = true;
	int varps = first ? cs2_run_varp_changes(t->iface, &everything, 1) : cs2_run_varp_changes(t->iface, varpList, varpCount);
	int stats = first ? cs2_run_stat_changes(t->iface, &everything, 1) : cs2_run_stat_changes(t->iface, statList, statCount);
	int invs = first || invChanged ? cs2_run_inv_changes(t->iface, &everything, 1) : 0;
	t->result = load ? load : varps ? varps : stats ? stats : invs;
	if (cs2_instructions() == before) return false; // nothing here was watching what changed
	t->ms = (int)((u32)nowMs() - now);
	if (first || t->result != 0) {
		char line[96];
		snprintf(line, sizeof(line), "scripts for %d ms=%d result=%d", t->iface, t->ms, t->result);
		logLine(line);
	}
	return true;
}

static TabScript questScript = {QUEST_INTERFACE};

static struct {
	bool active, dragged;
	int startX, startY, startScroll, farSamples;
	u32 startMs;
} listTouch;

static int measureLinesHook(const char* text, int width, int font);

static int invTotal(int inv, int item) {
	const NetInv* n = net_inventory(&net, inv);
	int total = 0;
	if (n) for (int i = 0; i < n->count; i++) if (n->items[i] == item) total += n->amounts[i];
	return total;
}

static int objMembers(int item) {
	const ObjDef* d = itemDef(item);
	return d ? d->members : 0;
}

// Called for each new connection, since the variables start again.
static void resetQuests(void) {
	cs2_init(&world.cache, net.varps);
	net.onVarbit = cs2_set_varbit;
	cs2_set_stats(net.skillLevel, net.skillXp, baseLevel);
	cs2_set_game(invTotal, objMembers, &net.members);
	cs2_set_item_names(itemName);
	cs2_set_text_measure(measureLinesHook);
	questScript.loaded = false;
	questRowCount = 0;
	questScroll = 0;
}

static bool journalActive(void) {
	return journalOpen;
}

static int* tabList(int* top, int* bottom, int* total);
static bool pressTabRow(int x, int y, int width);

// The scrolling list showing now, if any, with the part of the screen it has and the height of what is in it.
static int* activeList(int* top, int* bottom, int* total) {
	if (!mapVisible() || dialogActive()) return NULL;
	if (pageActive() && !pageIsConfirm()) return pageList(top, bottom, total);
	if (pageActive()) return NULL;
	if (guideActive()) return guideList(top, bottom, total);
	if (journalOpen) {
		*top = JOURNAL_TOP;
		*bottom = JOURNAL_BOTTOM;
		*total = journalRowCount * JOURNAL_ROW_H;
		return &journalScroll;
	}
	if (panelTab == TAB_QUESTS && questRowCount > 0) {
		*top = QUEST_LIST_TOP;
		*bottom = CONTENT_BOTTOM;
		*total = questTotal;
		return &questScroll;
	}
	return tabList(top, bottom, total);
}

static void scrollList(int* list, int value, int height, int total) {
	int limit = total - height;
	if (value > limit) value = limit;
	if (value < 0) value = 0;
	if (*list != value) panelDirty = true;
	*list = value;
}

static void rebuildQuests(void) {
	questRowCount = 0;
	questTotal = 0;
	Iface* f = cs2_interface(QUEST_INTERFACE);
	if (!f) return;
	for (int i = 0; i < f->count && questRowCount < QUEST_ROWS; i++) {
		const IfaceComponent* c = &f->components[i];
		if (c->type != COMP_TEXT || c->parent != QUEST_LIST_PARENT || c->hidden || !c->text) continue;
		if (net_component_hidden(&net, c->id) == 1) continue;
		QuestRow row;
		plainText(c->text, row.text, sizeof(row.text));
		if (!row.text[0]) continue;
		row.component = i;
		row.y = c->y;
		row.colour = c->colour;
		// The original only offers an option the component has switched on.
		row.pressable = (c->events >> 1 & 1) && c->opCount > 0 && c->ops[0] && c->ops[0][0] != ' ' && c->ops[0][0] != 0;
		int at = questRowCount++;
		while (at > 0 && questRows[at - 1].y > row.y) {
			questRows[at] = questRows[at - 1];
			at--;
		}
		questRows[at] = row;
		if (row.y + QUEST_ROW_H > questTotal) questTotal = row.y + QUEST_ROW_H;
	}
	const IfaceComponent* title = cs2_component(QUEST_TITLE);
	questTitle[0] = 0;
	if (title && title->text) {
		plainText(title->text, questTitle, sizeof(questTitle));
		questTitleColour = title->colour;
	}
	scrollList(&questScroll, questScroll, CONTENT_BOTTOM - QUEST_LIST_TOP, questTotal);
}

static void addJournalRow(const char* chars, const u32* marks, int length) {
	if (journalRowCount >= JOURNAL_ROWS) return;
	JournalRow* row = &journalRows[journalRowCount++];
	memcpy(row->text, chars, (size_t)length);
	row->text[length] = 0;
	memcpy(row->marks, marks, sizeof(u32) * (size_t)length);
}

// Wraps one line at the spaces, keeping each letter's colour.
static void wrapJournalLine(const char* chars, const u32* marks, int length, int columns) {
	if (length == 0) addJournalRow(chars, marks, 0);
	int at = 0;
	while (at < length) {
		int take = length - at;
		if (take > columns) {
			take = columns;
			while (take > 0 && chars[at + take] != ' ') take--;
			if (take == 0) take = columns;
		}
		addJournalRow(chars + at, marks + at, take);
		at += take;
		while (at < length && chars[at] == ' ') at++;
	}
}

static void addJournalText(const char* in, u32 colour, int columns) {
	static char chars[NET_TEXT_SIZE];
	static u32 marks[NET_TEXT_SIZE];
	int n = 0;
	u32 now = colour, struck = 0;
	for (;;) {
		if (*in == 0 || strncmp(in, "<br>", 4) == 0) {
			wrapJournalLine(chars, marks, n, columns);
			n = 0;
			if (*in == 0) break;
			in += 4;
			continue;
		}
		char letter = *in;
		if (letter == '<') {
			const char* end = strchr(in, '>');
			if (end) {
				if (strncmp(in, "<col=", 5) == 0) now = (u32)strtoul(in + 5, NULL, 16) & 0xFFFFFF;
				else if (strncmp(in, "</col>", 6) == 0) now = colour;
				else if (strncmp(in, "<str>", 5) == 0) struck = JOURNAL_STRUCK;
				else if (strncmp(in, "</str>", 6) == 0) struck = 0;
				letter = strncmp(in, "<lt>", 4) == 0 ? '<' : strncmp(in, "<gt>", 4) == 0 ? '>' : 0;
				in = end;
			}
		}
		in++;
		if (letter && n < NET_TEXT_SIZE) {
			chars[n] = letter;
			marks[n++] = now | struck;
		}
	}
}

static void rebuildJournal(int width) {
	journalRowCount = 0;
	journalTitle[0] = 0;
	Iface* f = cs2_interface(JOURNAL_INTERFACE);
	if (!f) return;
	int columns = (width - SCROLL_W - 12) / 8;
	if (columns > JOURNAL_COLS) columns = JOURNAL_COLS;
	for (int i = 0; i < f->count; i++) {
		const IfaceComponent* c = &f->components[i];
		if (c->type != COMP_TEXT || c->parent != JOURNAL_LIST_PARENT) continue;
		// The cache only holds placeholders here, so what counts is what the server sent.
		const char* text = net_component_text(&net, c->id);
		if (!text || net_component_hidden(&net, c->id) == 1) continue;
		addJournalText(text, (u32)c->colour & 0xFFFFFF, columns);
	}
	while (journalRowCount > 0 && !journalRows[journalRowCount - 1].text[0]) journalRowCount--;
	const char* title = net_component_text(&net, JOURNAL_TITLE);
	if (title) plainText(title, journalTitle, sizeof(journalTitle));
	scrollList(&journalScroll, journalScroll, JOURNAL_BOTTOM - JOURNAL_TOP, journalRowCount * JOURNAL_ROW_H);
}

// Runs once per frame. The first run reads scripts from the SD card, so it happens here and not while drawing.
static void updateQuests(void) {
	int width = gameOnBottom ? 400 : 320;
	bool open = net_interface_is_open(&net, JOURNAL_INTERFACE);
	if (open != journalOpen || (open && (journalVersion != net.ifaceVersion || journalWidth != width))) {
		if (open && !journalOpen) journalScroll = 0;
		if (open != journalOpen) listTouch.active = false;
		journalOpen = open;
		journalVersion = net.ifaceVersion;
		journalWidth = width;
		if (open) rebuildJournal(width);
		panelDirty = true;
	}
	if (panelTab != TAB_QUESTS || !mapVisible()) return;
	bool available = net_interface_is_open(&net, QUEST_INTERFACE);
	if (available != questAvailable) panelDirty = true;
	questAvailable = available;
	if (!available) return;
	if (!refreshTabScript(&questScript)) return;
	rebuildQuests();
	panelDirty = true;
}

static void drawScrollBar(u8* fb, int width, int top, int bottom, int scroll, int total) {
	int x = width - SCROLL_W, height = bottom - top;
	fillRect(fb, width, x, top, SCROLL_W - 2, height, 92, 78, 58);
	fillRect(fb, width, x, top, SCROLL_W - 2, 18, 120, 100, 70);
	fillRect(fb, width, x, bottom - 18, SCROLL_W - 2, 18, 120, 100, 70);
	drawText(fb, width, x + 6, top + 5, "^", 255, 255, 255);
	drawText(fb, width, x + 6, bottom - 13, "v", 255, 255, 255);
	int track = height - 36;
	if (total <= height || track <= 0) return;
	int thumb = track * height / total;
	if (thumb < 8) thumb = 8;
	fillRect(fb, width, x + 3, top + 18 + (track - thumb) * scroll / (total - height), SCROLL_W - 8, thumb, 170, 150, 110);
}

static void drawQuests(u8* fb, int width) {
	if (!questAvailable || questRowCount == 0) {
		const char* text = questAvailable ? "Loading..." : "Not available yet";
		drawText(fb, width, (width - (int)strlen(text) * 8) / 2, 116, text, 200, 190, 170);
		return;
	}
	drawText(fb, width, (width - (int)strlen(questTitle) * 8) / 2, CONTENT_TOP + 3, questTitle, (u8)(questTitleColour >> 16), (u8)(questTitleColour >> 8), (u8)questTitleColour);
	int columns = (width - SCROLL_W - 10) / 8;
	for (int i = 0; i < questRowCount; i++) {
		const QuestRow* row = &questRows[i];
		int y = QUEST_LIST_TOP + row->y - questScroll + 3;
		if (y < QUEST_LIST_TOP || y + 8 > CONTENT_BOTTOM) continue;
		char text[40];
		snprintf(text, sizeof(text), "%.*s", columns < 39 ? columns : 39, row->text);
		int x = row->pressable ? 8 : (width - SCROLL_W - (int)strlen(text) * 8) / 2;
		drawText(fb, width, x, y, text, (u8)(row->colour >> 16), (u8)(row->colour >> 8), (u8)row->colour);
	}
	drawScrollBar(fb, width, QUEST_LIST_TOP, CONTENT_BOTTOM, questScroll, questTotal);
}

static void drawJournal(u8* fb, int width) {
	fillRect(fb, width, 0, 0, width, SCREEN_H, 214, 190, 140);
	drawText(fb, width, 8, 8, journalTitle, 138, 8, 8);
	int closeX = width - CLOSE_W - 2;
	fillRect(fb, width, closeX, 2, CLOSE_W, CLOSE_H, 110, 90, 60);
	fillRect(fb, width, closeX + 2, 4, CLOSE_W - 4, CLOSE_H - 4, 84, 68, 46);
	drawText(fb, width, closeX + 6, 8, "Close", 255, 255, 255);
	for (int i = 0; i < journalRowCount; i++) {
		const JournalRow* row = &journalRows[i];
		int y = JOURNAL_TOP + i * JOURNAL_ROW_H - journalScroll + 2;
		if (y < JOURNAL_TOP || y + 8 > JOURNAL_BOTTOM) continue;
		int length = (int)strlen(row->text);
		for (int at = 0; at < length;) {
			u32 mark = row->marks[at];
			int end = at;
			while (end < length && row->marks[end] == mark) end++;
			char run[JOURNAL_COLS + 1];
			memcpy(run, row->text + at, (size_t)(end - at));
			run[end - at] = 0;
			drawText(fb, width, 6 + at * 8, y, run, (u8)(mark >> 16), (u8)(mark >> 8), (u8)mark);
			if (mark & JOURNAL_STRUCK) fillRect(fb, width, 6 + at * 8, y + 4, (end - at) * 8, 1, (u8)(mark >> 16), (u8)(mark >> 8), (u8)mark);
			at = end;
		}
	}
	drawScrollBar(fb, width, JOURNAL_TOP, JOURNAL_BOTTOM, journalScroll, journalRowCount * JOURNAL_ROW_H);
}

static int questRowAt(int y) {
	int at = y - QUEST_LIST_TOP + questScroll;
	if (y < QUEST_LIST_TOP || y >= CONTENT_BOTTOM) return -1;
	for (int i = 0; i < questRowCount; i++) if (at >= questRows[i].y && at < questRows[i].y + QUEST_ROW_H) return i;
	return -1;
}

static void pressQuestRow(int y) {
	int row = questRowAt(y);
	if (journalOpen || panelTab != TAB_QUESTS || row < 0 || !questRows[row].pressable) return;
	net_button(&net, QUEST_INTERFACE, questRows[row].component);
	actionsSent++;
}

static void pressListRow(int x, int y, int width) {
	if (pageActive()) {
		pressPageCell(x, y, width);
		return;
	}
	if (guideActive()) return;
	if (!pressTabRow(x, y, width)) pressQuestRow(y);
}

// A press that does not depend on dragging, which also makes it the way in for the cursor on the top screen.
static bool pressList(int x, int y, int width) {
	int top, bottom, total;
	int* list = activeList(&top, &bottom, &total);
	if (!list && !(journalOpen && mapVisible() && !dialogActive())) return false;
	if (journalOpen && x >= width - CLOSE_W - 2 && y < 2 + CLOSE_H) {
		net_close_modal(&net);
		actionsSent++;
		return true;
	}
	if (y >= top && y < bottom && x >= width - SCROLL_W) {
		if (y < top + 18) scrollList(list, *list - SCROLL_STEP, bottom - top, total);
		else if (y >= bottom - 18) scrollList(list, *list + SCROLL_STEP, bottom - top, total);
		return true;
	}
	if (journalOpen) return true;
	if (y < top || y >= bottom) return false;
	pressListRow(x, y, width);
	return true;
}

// The stylus on a list: nothing happens until it lifts, since it may be about to scroll.
static bool listTouchBegin(int x, int y, int width) {
	int top, bottom, total;
	int* list = activeList(&top, &bottom, &total);
	if (!list || y < top || y >= bottom || x >= width - SCROLL_W) return false;
	listTouch.active = true;
	listTouch.dragged = false;
	listTouch.startX = x;
	listTouch.startY = y;
	listTouch.startScroll = *list;
	listTouch.farSamples = 0;
	listTouch.startMs = (u32)nowMs();
	return true;
}

static bool listTouching(void) {
	return listTouch.active;
}

static void listTouchCancel(void) {
	listTouch.active = false;
}

static void handleListTouch(u32 held, touchPosition touch) {
	int top, bottom, total;
	int* list = activeList(&top, &bottom, &total);
	if (!list) {
		listTouch.active = false;
		return;
	}
	if (held & KEY_TOUCH) {
		// As with the bag, an empty sample is noise and one far sample is not yet a drag.
		if (touch.px == 0 && touch.py == 0) return;
		int dy = touch.py - listTouch.startY;
		listTouch.farSamples = abs(dy) >= SCROLL_DRAG_PIXELS ? listTouch.farSamples + 1 : 0;
		if (listTouch.farSamples >= 2) listTouch.dragged = true;
		if (listTouch.dragged) scrollList(list, listTouch.startScroll - dy, bottom - top, total);
		else if (pageActive() && (u32)nowMs() - listTouch.startMs >= BAG_HOLD_MS) {
			listTouch.active = false; // a hold lists the options, and the lift then does nothing
			openPageMenu(listTouch.startX, listTouch.startY, 320);
		}
		return;
	}
	listTouch.active = false;
	if (!listTouch.dragged) pressListRow(listTouch.startX, listTouch.startY, 320);
}
