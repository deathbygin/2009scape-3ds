// The Friends and Ignore tabs. Each is a list of names with two buttons. A row press only chooses a name,
// so Delete is always two deliberate presses, and adding one types a name on the 3DS keyboard.

#define FRIENDS_INTERFACE 550
#define FRIENDS_TITLE (FRIENDS_INTERFACE << 16 | 3)
#define SOCIAL_BAR 24
#define SOCIAL_BOTTOM (CONTENT_BOTTOM - SOCIAL_BAR)

typedef struct {
	uint64_t key;
	char name[16];
	char where[24];
	bool online;
} SocialRow;

static SocialRow socialRows[NET_MAX_FRIENDS];
static int socialCount, socialScroll, socialTab = -1;
static uint64_t socialChosen;
static long socialSeen = -1;
static char socialNote[40];
static const char* testKeyboardText = NULL; // the automatic test cannot type
static const char* testKeyboardNext = NULL;

static bool socialTabShown(void) {
	return panelTab == TAB_FRIENDS || panelTab == TAB_IGNORE || panelTab == TAB_CLAN;
}

static void resetSocial(void) {
	socialCount = socialScroll = 0;
	socialTab = -1;
	socialSeen = -1;
	socialChosen = 0;
	socialNote[0] = 0;
}

static void rebuildSocial(void) {
	socialCount = 0;
	bool chosenStillThere = false;
	if (panelTab == TAB_CLAN) {
		for (int i = 0; i < net.clanCount; i++) {
			const NetFriend* m = &net.clan[i];
			SocialRow* row = &socialRows[socialCount++];
			row->key = m->key;
			net_name_from_key(m->key, row->name, sizeof(row->name));
			row->online = true;
			snprintf(row->where, sizeof(row->where), "%s", m->worldName);
			if (row->key == socialChosen) chosenStillThere = true;
		}
	} else if (panelTab == TAB_IGNORE) {
		for (int i = 0; i < net.ignoreCount; i++) {
			SocialRow* row = &socialRows[socialCount++];
			row->key = net.ignores[i];
			net_name_from_key(row->key, row->name, sizeof(row->name));
			row->where[0] = 0;
			row->online = false;
			if (row->key == socialChosen) chosenStillThere = true;
		}
	} else {
		// Those online come first, as in the original.
		for (int pass = 0; pass < 2; pass++) {
			for (int i = 0; i < net.friendCount; i++) {
				const NetFriend* f = &net.friends[i];
				if ((f->world > 0) != (pass == 0)) continue;
				SocialRow* row = &socialRows[socialCount++];
				row->key = f->key;
				net_name_from_key(f->key, row->name, sizeof(row->name));
				row->online = f->world > 0;
				if (row->online && f->worldName[0]) snprintf(row->where, sizeof(row->where), "%s", f->worldName);
				else snprintf(row->where, sizeof(row->where), "%s", row->online ? "Online" : "Offline");
				if (row->key == socialChosen) chosenStillThere = true;
			}
		}
	}
	if (!chosenStillThere) socialChosen = 0;
}

static void updateSocial(void) {
	if (!socialTabShown() || !mapVisible()) return;
	if (socialTab != panelTab) {
		socialTab = panelTab;
		socialScroll = 0;
		socialChosen = 0;
		socialNote[0] = 0;
		socialSeen = -1;
	}
	if (socialSeen == net.socialVersion) return;
	socialSeen = net.socialVersion;
	rebuildSocial();
	panelDirty = true;
}

static void drawSocial(u8* fb, int width) {
	const char* title = panelTab == TAB_IGNORE ? "Ignore List" : panelTab == TAB_CLAN ? "Clan Chat" : "Friends List";
	char plain[48];
	if (panelTab == TAB_CLAN && net.clanOwner) {
		char name[16];
		net_name_from_key(net.clanName, name, sizeof(name));
		snprintf(plain, sizeof(plain), "Talking in: %s", name);
		title = plain;
	}
	const char* server = panelTab == TAB_FRIENDS ? net_component_text(&net, FRIENDS_TITLE) : NULL;
	if (server) {
		plainText(server, plain, sizeof(plain));
		title = plain;
	}
	drawText(fb, width, (width - (int)strlen(title) * 8) / 2, CONTENT_TOP + 3, title, 255, 152, 31);
	if (socialCount == 0) {
		const char* text = panelTab == TAB_CLAN ? "Not in a clan chat" : panelTab == TAB_FRIENDS && net.friendState != 2 ? "Loading..." : "Nobody listed";
		drawText(fb, width, (width - (int)strlen(text) * 8) / 2, 100, text, 200, 190, 170);
	}
	for (int i = 0; i < socialCount; i++) {
		const SocialRow* row = &socialRows[i];
		int y = QUEST_LIST_TOP + i * QUEST_ROW_H - socialScroll;
		if (y < QUEST_LIST_TOP || y + QUEST_ROW_H > SOCIAL_BOTTOM) continue;
		if (row->key == socialChosen) fillRect(fb, width, 4, y, width - SCROLL_W - 6, QUEST_ROW_H, 120, 100, 60);
		drawText(fb, width, 8, y + 3, row->name, 255, 255, 255);
		if (row->where[0]) {
			int x = width - SCROLL_W - 6 - (int)strlen(row->where) * 8;
			drawText(fb, width, x, y + 3, row->where, row->online ? 0 : 255, row->online ? 255 : 0, 0);
		}
	}
	if (socialCount * QUEST_ROW_H > SOCIAL_BOTTOM - QUEST_LIST_TOP) drawScrollBar(fb, width, QUEST_LIST_TOP, SOCIAL_BOTTOM, socialScroll, socialCount * QUEST_ROW_H);
	if (socialNote[0]) {
		fillRect(fb, width, 0, SOCIAL_BOTTOM - 12, width, 12, 30, 25, 18);
		drawText(fb, width, 6, SOCIAL_BOTTOM - 10, socialNote, 255, 255, 120);
	}
}

// Asks for text, up to limit letters. Returns false if the keyboard was cancelled.
static bool askForText(const char* hint, char* out, size_t size, int limit) {
	if (testKeyboardText) {
		snprintf(out, size, "%s", testKeyboardText);
		testKeyboardText = testKeyboardNext;
		testKeyboardNext = NULL;
		return true;
	}
	SwkbdState keyboard;
	swkbdInit(&keyboard, SWKBD_TYPE_QWERTY, 2, limit);
	swkbdSetHintText(&keyboard, hint);
	swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	return swkbdInputText(&keyboard, out, size) == SWKBD_BUTTON_CONFIRM;
}

static void pressSocialAdd(void) {
	char name[16];
	bool ignore = panelTab == TAB_IGNORE;
	if (panelTab == TAB_CLAN && net.clanOwner) {
		net_clan_join(&net, 0);
		actionsSent++;
		return;
	}
	bool typed = askForText(panelTab == TAB_CLAN ? "Whose chat to join" : ignore ? "Name to ignore" : "Name of friend", name, sizeof(name), 12);
	panelDirty = true;
	if (!typed) return;
	uint64_t key = net_name_key(name);
	if (panelTab == TAB_CLAN) {
		net_clan_join(&net, key);
		actionsSent++;
		return;
	}
	int result = ignore ? net_add_ignore(&net, key) : net_add_friend(&net, key);
	static const char* const REFUSED[] = {"", "That list is full", "Already on this list", "On your other list", "That is you"};
	snprintf(socialNote, sizeof(socialNote), "%s", REFUSED[result]);
	if (result == 0) actionsSent++;
}

static void pressSocialDelete(void) {
	if (!socialChosen) {
		snprintf(socialNote, sizeof(socialNote), "Choose a name first");
		panelDirty = true;
		return;
	}
	if (panelTab == TAB_IGNORE) net_del_ignore(&net, socialChosen);
	else net_del_friend(&net, socialChosen);
	socialChosen = 0;
	socialNote[0] = 0;
	actionsSent++;
	panelDirty = true;
}

static void pressSocialRow(int y) {
	int row = (y - QUEST_LIST_TOP + socialScroll) / QUEST_ROW_H;
	if (y < QUEST_LIST_TOP || row < 0 || row >= socialCount) return;
	socialChosen = socialRows[row].key == socialChosen ? 0 : socialRows[row].key;
	socialNote[0] = 0;
	panelDirty = true;
}

// ---- chat ----

#define CHAT_TOP (CONTENT_TOP + 2)
#define CHAT_ROW_H 12
#define CHAT_ROWS 320
#define CHAT_COLUMNS 48
#define CHAT_TEXT_LIMIT 80

typedef struct {
	char text[CHAT_COLUMNS + 1];
	u8 r, g, b;
	char request[16]; // who to answer if this row is pressed, for a trade request
} ChatRow;

static ChatRow chatRows[CHAT_ROWS];
static int chatRowCount, chatScroll, chatWidth;
static long chatRead;

// The table for packing messages, which is the group named huffman in archive 10.
static void loadChatCodec(void) {
	chatRead = 0;
	chatRowCount = chatScroll = 0;
	if (js5_load_index(&world.cache, 10) != 0) return;
	uint32_t hash = 0;
	for (const char* c = "huffman"; *c; c++) hash = hash * 31 + (uint8_t)*c;
	const Js5Index* index = &world.cache.index[10];
	for (uint32_t g = 0; g < index->capacity; g++) {
		if (!index->groups[g].present || index->groups[g].nameHash != hash) continue;
		Js5Buffer group = {0, 0}, file = {0, 0};
		if (js5_read_group(&world.cache, 10, g, NULL, &group) != 0) return;
		int rc = js5_group_file(&world.cache.index[10], g, &group, 0, &file);
		free(group.data);
		if (rc == 0) {
			huffman_init(file.data, (int)file.size);
			free(file.data);
		}
		return;
	}
}

static void addChatRows(const char* text, u8 r, u8 g, u8 b, int columns) {
	const char* at = text;
	while (*at && chatRowCount < CHAT_ROWS) {
		int length = (int)strlen(at);
		if (length > columns) {
			length = columns;
			while (length > 0 && at[length] != ' ') length--;
			if (length == 0) length = columns;
		}
		ChatRow* row = &chatRows[chatRowCount++];
		memcpy(row->text, at, (size_t)length);
		row->text[length] = 0;
		row->r = r;
		row->g = g;
		row->b = b;
		row->request[0] = 0;
		at += length;
		while (*at == ' ') at++;
	}
}

// How a line reads and its colour, the same in the Chat panel and on the game screen.
static void chatLineText(const NetChat* line, char* plain, size_t plainSize, char* text, size_t size, u8* r, u8* g, u8* b) {
	plainText(line->text, plain, plainSize);
	if (line->kind == NET_CHAT_GAME) {
		snprintf(text, size, "%s", plain);
		*r = *g = *b = 255;
	} else if (line->kind == NET_CHAT_PUBLIC) {
		snprintf(text, size, "%s: %s", line->who, plain);
		*r = *g = 255;
		*b = 0;
	} else if (line->kind == NET_CHAT_TRADE) {
		snprintf(text, size, "%s %s", line->who, plain);
		*r = 255;
		*g = 0;
		*b = 255;
	} else if (line->kind == NET_CHAT_FROM) {
		snprintf(text, size, "From %s: %s", line->who, plain);
		*r = 0;
		*g = *b = 255;
	} else if (line->kind == NET_CHAT_TO) {
		snprintf(text, size, "To %s: %s", line->who, plain);
		*r = 0;
		*g = *b = 200;
	} else {
		snprintf(text, size, "[Clan] %s: %s", line->who, plain);
		*r = 255;
		*g = *b = 170;
	}
}

static void rebuildChat(int width) {
	int columns = (width - SCROLL_W - 12) / 8;
	if (columns > CHAT_COLUMNS) columns = CHAT_COLUMNS;
	int height = SOCIAL_BOTTOM - CHAT_TOP, oldTotal = chatRowCount * CHAT_ROW_H;
	bool atEnd = chatScroll >= oldTotal - height;
	chatRowCount = 0;
	long first = net.chatVersion > NET_CHAT_LINES ? net.chatVersion - NET_CHAT_LINES : 0;
	for (long i = first; i < net.chatVersion; i++) {
		char plain[NET_CHAT_TEXT], text[NET_CHAT_TEXT + 32];
		u8 r, g, b;
		const NetChat* line = &net.chat[i % NET_CHAT_LINES];
		int before = chatRowCount;
		chatLineText(line, plain, sizeof(plain), text, sizeof(text), &r, &g, &b);
		addChatRows(text, r, g, b, columns);
		if (line->kind == NET_CHAT_TRADE) for (int k = before; k < chatRowCount; k++) snprintf(chatRows[k].request, sizeof(chatRows[k].request), "%s", line->who);
	}
	int total = chatRowCount * CHAT_ROW_H;
	if (atEnd) chatScroll = total > height ? total - height : 0;
}

static void updateChat(void) {
	int width = gameOnBottom ? 400 : 320;
	if (panelTab != TAB_CHAT) return;
	if (chatRead == net.chatVersion && chatWidth == width) return;
	chatRead = net.chatVersion;
	chatWidth = width;
	rebuildChat(width);
	panelDirty = true;
}

static void drawChat(u8* fb, int width) {
	for (int i = 0; i < chatRowCount; i++) {
		const ChatRow* row = &chatRows[i];
		int y = CHAT_TOP + i * CHAT_ROW_H - chatScroll + 2;
		if (y < CHAT_TOP || y + 8 > SOCIAL_BOTTOM) continue;
		drawText(fb, width, 6, y, row->text, row->r, row->g, row->b);
	}
	drawScrollBar(fb, width, CHAT_TOP, SOCIAL_BOTTOM, chatScroll, chatRowCount * CHAT_ROW_H);
}

// A trade request is answered by asking back, which is what pressing it does in the original.
static void pressChatRow(int y) {
	int row = (y - CHAT_TOP - 2 + chatScroll) / CHAT_ROW_H;
	if (y < CHAT_TOP || row < 0 || row >= chatRowCount || !chatRows[row].request[0]) return;
	int option = -1;
	for (int i = 0; i < NET_PLAYER_OPTIONS; i++) if (strcasecmp(net.playerOptions[i], "Trade with") == 0) option = i;
	for (int i = 0; i < net.playerCount; i++) {
		const NetNpc* n = &net.players[i];
		if (strcasecmp(net.looks[n->index].name, chatRows[row].request) != 0) continue;
		if (option >= 0 && net_player_action(&net, option, n->index)) actionsSent++;
		return;
	}
}

static void pressChatSay(void) {
	char text[CHAT_TEXT_LIMIT + 4];
	panelDirty = true;
	if (!askForText("Say", text, sizeof(text), CHAT_TEXT_LIMIT)) return;
	if (strncmp(text, "::", 2) == 0) net_command(&net, text + 2);
	else net_chat_public(&net, text);
	actionsSent++;
}

static void sendPrivate(uint64_t key) {
	char text[CHAT_TEXT_LIMIT + 4], name[16], hint[32];
	net_name_from_key(key, name, sizeof(name));
	snprintf(hint, sizeof(hint), "Message to %s", name);
	panelDirty = true;
	if (!askForText(hint, text, sizeof(text), CHAT_TEXT_LIMIT)) return;
	net_chat_private(&net, key, text);
	actionsSent++;
}

static void pressChatPrivate(void) {
	char name[16];
	panelDirty = true;
	if (askForText("Message who?", name, sizeof(name), 12)) sendPrivate(net_name_key(name));
}

static void pressSocialMessage(void) {
	if (socialChosen) {
		sendPrivate(socialChosen);
		return;
	}
	snprintf(socialNote, sizeof(socialNote), "Choose a name first");
	panelDirty = true;
}
