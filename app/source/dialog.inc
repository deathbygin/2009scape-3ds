// Chatbox interfaces: NPC and player speech, option lists and messages.
// The server opens an interface in the chat area and fills in its text. Rather than shrink a layout made for
// a 765 pixel window, its lines are read in order and redrawn for the panel, with its buttons as large rows.

#define CHAT_PARENT (752 << 16 | 12)
#define DEFAULT_CHATBOX 137
#define DIALOG_MAX_ROWS 16
#define DIALOG_TEXT 160
#define DIALOG_BOTTOM SCREEN_H
#define DIALOG_BUTTON_H 26

typedef struct {
	int component; // the id sent when pressed, or -1 for plain text
	int source; // the text's own component
	uint8_t title;
	char text[DIALOG_TEXT];
	int x, y, w, h; // set when drawn
} DialogRow;

// Screens that are not chatbox dialogue but work the same way: a few pictures and a few presses.
// Most are random events, which cannot be left until they are answered.
static const int DIALOG_SCREENS[] = {103, 178, 184, 186, 188, 189, 297};

#define DIALOG_TILES 16
#define PILLORY_SCREEN 189
#define PILLORY_FIRST_KEY 9749
#define PILLORY_FIRST_LOCK 9753
#define PILLORY_FIRST_PADLOCK 9757
#define MODEL_BACK_VIEW 1024
#define DIALOG_TILE_BUTTONS 6
#define DIALOG_MODELS 8
#define DIALOG_PICTURE 72 // the size of a model picture, which is HEAD_SIZE

// A picture, and whatever is pressed for it. A tile with no buttons only shows, and one with several is make-X.
typedef struct {
	int kind, id, amount, yaw, component;
	int count, buttons[DIALOG_TILE_BUTTONS];
	char labels[DIALOG_TILE_BUTTONS][6];
	char name[28];
	int x[DIALOG_TILE_BUTTONS], y[DIALOG_TILE_BUTTONS], w[DIALOG_TILE_BUTTONS], h[DIALOG_TILE_BUTTONS];
} DialogTile;

typedef struct {
	int id, yaw;
	bool ready;
	u32 pixels[DIALOG_PICTURE * DIALOG_PICTURE];
} DialogModel;

static DialogTile dialogTiles[DIALOG_TILES];
static int dialogTileCount = 0;
static DialogModel dialogModels[DIALOG_MODELS];
static int dialogModelNext = 0;

static Iface dialogIface;
static bool dialogLoaded = false, dialogWaiting = false;
static DialogRow dialogRows[DIALOG_MAX_ROWS];
static int dialogRowCount = 0;
static long dialogVersion = -1;

static bool componentHidden(const Iface* f, const IfaceComponent* c) {
	for (int depth = 0; c && depth < 32; depth++) {
		int forced = net_component_hidden(&net, c->id);
		if (forced == 1 || (forced < 0 && c->hidden)) return true;
		if (c->parent == -1) return false;
		int index = c->parent & 0xFFFF;
		c = index < f->count ? &f->components[index] : NULL;
	}
	return false;
}

// Removes colour tags and turns line breaks into spaces, so the text can be wrapped to the panel.
static void plainText(const char* in, char* out, size_t size) {
	size_t n = 0;
	while (*in && n + 1 < size) {
		if (*in == '<') {
			const char* end = strchr(in, '>');
			if (end) {
				if (strncmp(in, "<br>", 4) == 0 && n > 0 && out[n - 1] != ' ') out[n++] = ' ';
				in = end + 1;
				continue;
			}
		}
		out[n++] = *in++;
	}
	out[n] = 0;
}

static bool dialogPressable(const IfaceComponent* c) {
	return (c->events & 3) != 0 || c->buttonType == BUTTON_OK;
}

static const char* dialogText(const IfaceComponent* c) {
	const char* text = net_component_text(&net, c->id);
	return text ? text : c->text ? c->text : "";
}

static const char* dialogOp(const IfaceComponent* c) {
	for (int i = 0; i < c->opCount; i++) if (c->ops[i] && c->ops[i][0]) return c->ops[i];
	return c->option && c->option[0] ? c->option : "";
}

static void dialogPlace(const IfaceComponent* c, int* x, int* y) {
	*x = *y = 0;
	for (int depth = 0; c && depth < 32; depth++) {
		*x += c->x;
		*y += c->y;
		if (c->parent == -1) return;
		int index = c->parent & 0xFFFF;
		c = index < dialogIface.count ? &dialogIface.components[index] : NULL;
	}
}

// The amount a make-X button stands for, from options such as "Make 10" and "Smelt X <col=ff9040>Bronze".
static void dialogAmountLabel(const char* op, char* out, size_t size) {
	char plain[48];
	plainText(op, plain, sizeof(plain));
	for (char* word = strtok(plain, " "); word; word = strtok(NULL, " ")) {
		if (strcmp(word, "X") == 0 || strcmp(word, "All") == 0 || (word[0] >= '0' && word[0] <= '9')) {
			snprintf(out, size, "%s", word);
			return;
		}
	}
	snprintf(out, size, "Go");
}

static int dialogAmountRank(const char* label) {
	if (label[0] >= '0' && label[0] <= '9') return atoi(label);
	return label[0] == 'X' ? 100000 : 200000;
}

static void dialogTileAdd(DialogTile* t, int component) {
	if (t->count >= DIALOG_TILE_BUTTONS) return;
	char label[6];
	dialogAmountLabel(dialogOp(&dialogIface.components[component]), label, sizeof(label));
	int at = t->count++;
	while (at > 0 && dialogAmountRank(t->labels[at - 1]) > dialogAmountRank(label)) {
		t->buttons[at] = t->buttons[at - 1];
		memcpy(t->labels[at], t->labels[at - 1], sizeof(label));
		at--;
	}
	t->buttons[at] = component;
	memcpy(t->labels[at], label, sizeof(label));
}

static DialogTile* dialogTileNew(int component, int kind, int id, int amount, int yaw) {
	if (dialogTileCount >= DIALOG_TILES) return NULL;
	DialogTile* t = &dialogTiles[dialogTileCount++];
	memset(t, 0, sizeof(*t));
	t->component = component;
	t->kind = kind;
	t->id = id;
	t->amount = amount;
	t->yaw = yaw;
	if (kind == NET_PICTURE_ITEM) snprintf(t->name, sizeof(t->name), "%s", itemName(id));
	return t;
}

static void dialogDropRow(int source) {
	for (int r = 0; r < dialogRowCount; r++) {
		if (dialogRows[r].source != source) continue;
		memmove(&dialogRows[r], &dialogRows[r + 1], sizeof(DialogRow) * (size_t)(dialogRowCount - r - 1));
		dialogRowCount--;
		return;
	}
}

// Sorts pictures and presses out. A press belongs to the picture above or below it, or to the one it is paired
// with by order, or to the line of text beside it, and otherwise it is a picture or a plain row of its own.
static void rebuildTiles(void) {
	dialogTileCount = 0;
	const Iface* f = &dialogIface;
	int waiting[64], waitingCount = 0;
	for (int i = 0; i < f->count; i++) {
		const IfaceComponent* c = &f->components[i];
		if (c->type != COMP_MODEL || componentHidden(f, c)) continue;
		const NetPicture* p = net_component_picture(&net, c->id);
		if (!p) continue;
		int id = p->value, yaw = p->yaw;
		if (f->id == PILLORY_SCREEN) {
			// The original spins the locks and swings the keys, which is how their shapes are seen. Still, only the
			// back of a lock shows its keyhole, and a key's tip is too small to read, so a key shows the keyhole it fits.
			if (id >= PILLORY_FIRST_PADLOCK) continue; // the row of padlocks only counts progress
			if (id < PILLORY_FIRST_LOCK) id += PILLORY_FIRST_LOCK - PILLORY_FIRST_KEY;
			yaw = MODEL_BACK_VIEW;
		}
		dialogTileNew(i, p->kind, id, p->amount > 0 ? p->amount : 1, yaw);
	}
	for (int i = 0; i < f->count; i++) {
		const IfaceComponent* c = &f->components[i];
		if (c->type == 255 || !dialogPressable(c) || componentHidden(f, c)) continue;
		bool named = c->type == COMP_TEXT && dialogText(c)[0];
		int x, y, owner = -1;
		dialogPlace(c, &x, &y);
		for (int t = 0; t < dialogTileCount && owner < 0; t++) if (dialogTiles[t].component == i) owner = t; // its own picture comes first
		for (int t = 0; t < dialogTileCount && owner < 0; t++) {
			const IfaceComponent* picture = &f->components[dialogTiles[t].component];
			int px, py;
			dialogPlace(picture, &px, &py);
			if (net_component_picture(&net, f->components[i].id)) continue;
			if (px + picture->width / 2 >= x && px + picture->width / 2 < x + c->width) owner = t;
		}
		if (named) {
			// Make-X puts the item's name on its "Make 1" button, which belongs with the other amounts.
			char label[6];
			dialogAmountLabel(dialogOp(c), label, sizeof(label));
			if (owner < 0 || strcmp(label, "Go") == 0) continue;
			dialogDropRow(i);
			dialogTileAdd(&dialogTiles[owner], i);
		} else if (owner >= 0) {
			dialogTileAdd(&dialogTiles[owner], i);
		} else if (strcmp(dialogOp(c), "Close") == 0) {
			// Never one of the choices, even if it is the only press left over.
			if (dialogRowCount < DIALOG_MAX_ROWS) {
				DialogRow* row = &dialogRows[dialogRowCount++];
				snprintf(row->text, sizeof(row->text), "Close");
				row->component = c->id;
				row->source = i;
				row->title = 0;
				row->w = 0;
			}
		} else if (waitingCount < 64) {
			waiting[waitingCount++] = i;
		}
	}
	int empty = 0;
	for (int t = 0; t < dialogTileCount; t++) if (dialogTiles[t].count == 0) empty++;
	if (empty == waitingCount) {
		int next = 0;
		for (int t = 0; t < dialogTileCount; t++) if (dialogTiles[t].count == 0) dialogTileAdd(&dialogTiles[t], waiting[next++]);
		return;
	}
	for (int w = 0; w < waitingCount; w++) {
		const IfaceComponent* c = &f->components[waiting[w]];
		int x, y;
		dialogPlace(c, &x, &y);
		bool placed = false;
		for (int r = 0; r < dialogRowCount && !placed; r++) {
			DialogRow* row = &dialogRows[r];
			if (row->component != -1) continue;
			const IfaceComponent* line = &f->components[row->source];
			int lx, ly;
			dialogPlace(line, &lx, &ly);
			if (y < ly + line->height && ly < y + c->height && y >= ly) {
				row->component = c->id;
				row->title = false;
				placed = true;
			}
		}
		if (placed) continue;
		if (c->type == COMP_MODEL && c->modelId > 0 && strcmp(dialogOp(c), "Close") != 0) {
			DialogTile* t = dialogTileNew(waiting[w], NET_PICTURE_MODEL, c->modelId, 1, 0);
			if (t) dialogTileAdd(t, waiting[w]);
		} else if (dialogRowCount < DIALOG_MAX_ROWS && dialogOp(c)[0]) {
			DialogRow* row = &dialogRows[dialogRowCount++];
			plainText(dialogOp(c), row->text, sizeof(row->text));
			row->component = c->id;
			row->source = waiting[w];
			row->title = 0;
			row->w = 0;
		}
	}
}

// Loads what the tiles show, one at a time because each is a read from the SD card.
static void dialogLoadPictures(void) {
	for (int t = 0; t < dialogTileCount; t++) {
		const DialogTile* tile = &dialogTiles[t];
		if (tile->kind == NET_PICTURE_ITEM) {
			if (ensureIcon(tile->id, tile->amount)) return;
			continue;
		}
		bool have = false;
		for (int m = 0; m < DIALOG_MODELS; m++) if (dialogModels[m].ready && dialogModels[m].id == tile->id && dialogModels[m].yaw == tile->yaw) have = true;
		if (have) continue;
		DialogModel* model = &dialogModels[dialogModelNext];
		dialogModelNext = (dialogModelNext + 1) % DIALOG_MODELS;
		model->id = tile->id;
		model->yaw = tile->yaw;
		model->ready = renderModelPicture(tile->id, tile->yaw, model->pixels);
		panelDirty = true;
		return;
	}
}

static void drawDialogPicture(u8* fb, int width, const DialogTile* t, int x, int y, int size) {
	if (t->kind == NET_PICTURE_ITEM) {
		int scale = size >= ICON_W * 2 ? 2 : 1;
		drawItemIconScaled(fb, width, t->id, t->amount, x + (size - ICON_W * scale) / 2, y + (size - ICON_H * scale) / 2, scale);
		return;
	}
	for (int m = 0; m < DIALOG_MODELS; m++) {
		const DialogModel* model = &dialogModels[m];
		if (!model->ready || model->id != t->id || model->yaw != t->yaw) continue;
		for (int py = 0; py < size; py++) {
			for (int px = 0; px < size; px++) {
				u32 c = model->pixels[(py * DIALOG_PICTURE / size) * DIALOG_PICTURE + px * DIALOG_PICTURE / size];
				if (c) putPixel(fb, width, x + px, y + py, (u8)(c >> 16), (u8)(c >> 8), (u8)c);
			}
		}
		return;
	}
}

static void rebuildDialog(void) {
	dialogRowCount = 0;
	dialogTileCount = 0;
	if (!dialogLoaded) return;
	for (int i = 0; i < dialogIface.count && dialogRowCount < DIALOG_MAX_ROWS; i++) {
		const IfaceComponent* c = &dialogIface.components[i];
		if (c->type != COMP_TEXT || componentHidden(&dialogIface, c)) continue;
		const char* text = dialogText(c);
		if (!text[0]) continue;
		DialogRow* row = &dialogRows[dialogRowCount];
		plainText(text, row->text, sizeof(row->text));
		if (!row->text[0]) continue;
		bool pressable = dialogPressable(c);
		row->component = pressable ? c->id : -1;
		row->source = i;
		row->title = !pressable && dialogRowCount == 0;
		row->w = 0;
		dialogRowCount++;
	}
	if (dialogIface.id == PILLORY_SCREEN && dialogRowCount < DIALOG_MAX_ROWS) {
		// The server says this in the chat area, which this screen covers.
		DialogRow* row = &dialogRows[dialogRowCount++];
		snprintf(row->text, sizeof(row->text), "Pick the key that fits the lock.");
		row->component = -1;
		row->source = 0;
		row->title = 1;
		row->w = 0;
	}
	rebuildTiles();
	// What is being asked goes above the answers, whatever order the interface keeps them in.
	static DialogRow sorted[DIALOG_MAX_ROWS];
	int count = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int r = 0; r < dialogRowCount; r++) {
			if ((dialogRows[r].component == -1) == (pass == 0)) sorted[count++] = dialogRows[r];
		}
	}
	memcpy(dialogRows, sorted, sizeof(DialogRow) * (size_t)count);
}

// Runs once per frame. Reading the interface touches the SD card, so it happens here and not while drawing.
static void updateDialog(void) {
	dialogLoadPictures();
	if (dialogVersion == net.ifaceVersion) return;
	dialogVersion = net.ifaceVersion;
	dialogWaiting = false;
	panelDirty = true;
	int wanted = net_open_interface(&net, CHAT_PARENT);
	if (wanted == DEFAULT_CHATBOX) wanted = -1;
	for (size_t i = 0; i < sizeof(DIALOG_SCREENS) / sizeof(DIALOG_SCREENS[0]); i++) {
		if (net_interface_is_open(&net, DIALOG_SCREENS[i])) wanted = DIALOG_SCREENS[i];
	}
	if (dialogLoaded && dialogIface.id != wanted) {
		iface_free(&dialogIface);
		dialogLoaded = false;
	}
	if (!dialogLoaded && wanted != -1 && iface_load(&world.cache, wanted, &dialogIface) == 0) dialogLoaded = true;
	rebuildDialog();
}

static bool dialogActive(void) {
	return dialogLoaded && (dialogRowCount > 0 || dialogTileCount > 0);
}

// Draws text wrapped to a width, and returns the y below it. With fb NULL it only measures.
static int drawWrapped(u8* fb, int width, int x, int y, int maxWidth, const char* text, u8 r, u8 g, u8 b) {
	int perLine = maxWidth / 8;
	const char* at = text;
	char line[64];
	while (*at) {
		int length = (int)strlen(at);
		if (length > perLine) {
			length = perLine;
			while (length > 0 && at[length] != ' ') length--;
			if (length == 0) length = perLine;
		}
		if (length > (int)sizeof(line) - 1) length = (int)sizeof(line) - 1;
		memcpy(line, at, (size_t)length);
		line[length] = 0;
		if (fb) drawText(fb, width, x, y, line, r, g, b);
		y += 12;
		at += length;
		while (*at == ' ') at++;
	}
	return y;
}

#define HEAD_ROOM 80 // the space a talking head takes from the text
#define TILE_ROW_H 36

static void drawDialogButton(u8* fb, int width, int x, int y, int w, int h, const char* label) {
	fillRect(fb, width, x, y, w, h, 110, 90, 60);
	fillRect(fb, width, x + 2, y + 2, w - 4, h - 4, 84, 68, 46);
	if (label) drawText(fb, width, x + (w - (int)strlen(label) * 8) / 2, y + (h - 8) / 2, label, 255, 255, 255);
}

static void drawDialogTiles(u8* fb, int width, int y) {
	int many = 0, single = 0;
	for (int t = 0; t < dialogTileCount; t++) {
		if (dialogTiles[t].count > 1) many++;
		if (dialogTiles[t].count == 1) single++;
	}
	// Make-X: a picture with its amounts beside it, in two columns when there are too many for one.
	int columns = many > 5 ? 2 : 1, column = (width - 8) / columns, at = 0;
	for (int t = 0; t < dialogTileCount; t++) {
		DialogTile* tile = &dialogTiles[t];
		if (tile->count < 2) continue;
		int x = 4 + (at % columns) * column, row = y + (at / columns) * TILE_ROW_H;
		drawDialogPicture(fb, width, tile, x, row, 36);
		int left = x + 38, room = column - 42, each = room / tile->count;
		if (tile->name[0]) {
			drawText(fb, width, left, row + 1, tile->name, 255, 200, 60);
			for (int b = 0; b < tile->count; b++) {
				tile->x[b] = left + b * each;
				tile->y[b] = row + 11;
				tile->w[b] = each - 2;
				tile->h[b] = TILE_ROW_H - 13;
			}
		} else {
			for (int b = 0; b < tile->count; b++) {
				tile->x[b] = left + b * each;
				tile->y[b] = row + 1;
				tile->w[b] = each - 2;
				tile->h[b] = TILE_ROW_H - 3;
			}
		}
		for (int b = 0; b < tile->count; b++) drawDialogButton(fb, width, tile->x[b], tile->y[b], tile->w[b], tile->h[b], tile->labels[b]);
		at++;
	}
	if (many) y += (many + columns - 1) / columns * TILE_ROW_H + 4;
	// Choosing a picture: as large as the room allows.
	if (single == 0) return;
	int across = single < 4 ? single : 4, rows = (single + across - 1) / across;
	int size = DIALOG_PICTURE + 4;
	if (y + rows * size > SCREEN_H) size = (SCREEN_H - y) / rows;
	if (size < 40) size = 40;
	if (size * across > width - 8) size = (width - 8) / across;
	int left = (width - across * size) / 2;
	at = 0;
	for (int t = 0; t < dialogTileCount; t++) {
		DialogTile* tile = &dialogTiles[t];
		if (tile->count != 1) continue;
		tile->x[0] = left + (at % across) * size;
		tile->y[0] = y + (at / across) * size;
		tile->w[0] = size - 3;
		tile->h[0] = size - 3;
		drawDialogButton(fb, width, tile->x[0], tile->y[0], tile->w[0], tile->h[0], NULL);
		drawDialogPicture(fb, width, tile, tile->x[0] + 2, tile->y[0] + 2, size - 7);
		at++;
	}
}

static void drawDialog(u8* fb, int width) {
	fillRect(fb, width, 0, 0, width, DIALOG_BOTTOM, 44, 37, 28);
	drawHead(fb, width);
	int y = 8, margin = 10, room = width - margin * 2;
	if (headShown()) {
		room -= HEAD_ROOM;
		if (!headOnRight()) margin += HEAD_ROOM;
	}
	// Pictures with nothing to press go first: the thing being asked about.
	int shown = 0, showing = 0;
	for (int t = 0; t < dialogTileCount; t++) if (dialogTiles[t].count == 0) showing++;
	for (int t = 0; t < dialogTileCount; t++) {
		if (dialogTiles[t].count != 0) continue;
		int size = showing > 4 ? 40 : DIALOG_PICTURE;
		drawDialogPicture(fb, width, &dialogTiles[t], (width - showing * (size + 4)) / 2 + shown * (size + 4), y, size);
		shown++;
	}
	if (showing) y += (showing > 4 ? 40 : DIALOG_PICTURE) + 4;
	int choices = 0, choice = 0;
	for (int i = 0; i < dialogRowCount; i++) if (dialogRows[i].component != -1) choices++;
	for (int i = 0; i < dialogRowCount; i++) {
		DialogRow* row = &dialogRows[i];
		if (row->component != -1 && choices > 5) {
			// Too many for one column, such as the mime's eight.
			row->w = (room - 6) / 2;
			row->h = DIALOG_BUTTON_H;
			row->x = margin + (choice % 2) * (row->w + 6);
			row->y = y + 4;
			fillRect(fb, width, row->x, row->y, row->w, row->h, 110, 90, 60);
			fillRect(fb, width, row->x + 2, row->y + 2, row->w - 4, row->h - 4, 84, 68, 46);
			drawText(fb, width, row->x + 6, row->y + 9, row->text, 255, 255, 255);
			if (choice % 2 == 1 || choice == choices - 1) y = row->y + row->h;
			choice++;
			continue;
		}
		if (row->component == -1) {
			if (row->title) y = drawWrapped(fb, width, margin, y, room, row->text, 255, 200, 60) + 4;
			else y = drawWrapped(fb, width, margin, y, room, row->text, 255, 255, 255) + 2;
			continue;
		}
		const char* text = dialogWaiting ? "Please wait..." : row->text;
		int height = drawWrapped(NULL, width, 0, 0, room - 12, text, 0, 0, 0) + 12;
		if (height < DIALOG_BUTTON_H) height = DIALOG_BUTTON_H;
		row->x = margin;
		row->y = y + 4;
		row->w = room;
		row->h = height;
		fillRect(fb, width, row->x, row->y, row->w, row->h, 110, 90, 60);
		fillRect(fb, width, row->x + 2, row->y + 2, row->w - 4, row->h - 4, 84, 68, 46);
		drawWrapped(fb, width, row->x + 6, row->y + (height - 12) / 2 + 2 - (height > DIALOG_BUTTON_H ? 4 : 0), row->w - 12, text, 255, 255, 120);
		y = row->y + height;
	}
	drawDialogTiles(fb, width, y + 6);
}

// One press, one packet. After a dialogue step nothing more is sent until the server answers, as in the original.
// Each kind of press goes the way the original sends it: a continue, an old button, or an option.
static void dialogSend(int index) {
	const IfaceComponent* c = &dialogIface.components[index];
	if (dialogWaiting) return;
	if (c->events & 1) {
		net_continue(&net, c->id);
		dialogWaiting = true;
	} else if (strcmp(dialogOp(c), "Close") == 0) {
		net_close_modal(&net);
	} else if (c->buttonType == BUTTON_OK) {
		net_if_button(&net, dialogIface.id, index);
	} else {
		net_button(&net, dialogIface.id, index);
	}
	actionsSent++;
	panelDirty = true;
}

static bool pressDialog(int x, int y) {
	if (!dialogActive()) return false;
	for (int i = 0; i < dialogRowCount; i++) {
		const DialogRow* row = &dialogRows[i];
		if (row->component == -1 || row->w == 0 || x < row->x || y < row->y || x >= row->x + row->w || y >= row->y + row->h) continue;
		dialogSend(row->component & 0xFFFF);
		return true;
	}
	for (int t = 0; t < dialogTileCount; t++) {
		const DialogTile* tile = &dialogTiles[t];
		for (int b = 0; b < tile->count; b++) {
			if (tile->w[b] == 0 || x < tile->x[b] || y < tile->y[b] || x >= tile->x[b] + tile->w[b] || y >= tile->y[b] + tile->h[b]) continue;
			dialogSend(tile->buttons[b]);
			return true;
		}
	}
	return y < DIALOG_BOTTOM; // the panel behind it cannot be pressed
}
